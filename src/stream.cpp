#include "stream.hpp"

#include "direct.hpp"
#include "util.hpp"
#include "v210.hpp"

#include <algorithm>
#include <chrono>

namespace mtp {

StreamDecoder::StreamDecoder(int output, int preroll) : output_(output), preroll_(preroll) {}

StreamDecoder::~StreamDecoder() { stop(); }

void StreamDecoder::start() {
    stop_ = false;
    thread_ = std::thread([this] { main(); });
}

void StreamDecoder::stop() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void StreamDecoder::set_format(const VideoFormat& fmt) {
    std::lock_guard lock(mu_);
    if (fmt.name == fmt_.name && fmt.width == fmt_.width) return;
    fmt_ = fmt;
    drop(window_.size());
    ++gen_;
}

// stream mu_ held.
void StreamDecoder::bind(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t lo, std::int64_t hi, bool wrap) {
    if (owner_ != owner) {
        owner_ = owner;
        mezz_ = owner ? mezz : std::string();
        last_ = -1;
        exact_ = false;
        drop(window_.size());
        shown_ = Frame{};
        ++gen_;
    }
    lo_ = std::max<std::int64_t>(0, lo);
    hi_ = std::max(lo_ + 1, hi);
    wrap_ = wrap;
}

const StreamDecoder::Frame* StreamDecoder::take(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t frame, std::int64_t lo,
                                                std::int64_t hi, bool wrap) {
    std::lock_guard lock(mu_);
    bind(owner, mezz, lo, hi, wrap);
    if (!owner) return nullptr;
    want_ = frame;
    const auto it = std::find_if(window_.begin(), window_.end(), [&](const Frame& f) { return f.frame == frame; });
    const Frame* found = nullptr;
    if (it != window_.end()) {
        drop(static_cast<std::size_t>(it - window_.begin()));
        found = &window_.front();
        last_ = frame;
        exact_ = false;
    } else if (!window_.empty()) {
        drop(window_.size());
        exact_ = false;
        ++gen_;
    }
    cv_.notify_one();
    return found;
}

void StreamDecoder::prime(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t frame, std::int64_t lo, std::int64_t hi,
                          bool wrap) {
    std::lock_guard lock(mu_);
    bind(owner, mezz, lo, hi, wrap);
    if (!owner) return;
    if (!window_.empty() && window_.front().frame == frame) return;  // primed already
    if (!window_.empty() || want_ != frame || !exact_) {
        drop(window_.size());
        ++gen_;
    }
    want_ = frame;
    exact_ = true;
    cv_.notify_one();
}

bool StreamDecoder::has(const std::shared_ptr<const void>& owner, std::int64_t frame) {
    std::lock_guard lock(mu_);
    if (owner_ != owner) return false;
    return std::any_of(window_.begin(), window_.end(), [&](const Frame& f) { return f.frame == frame; });
}

void StreamDecoder::release() {
    std::lock_guard lock(mu_);
    bind(nullptr, std::string(), 0, 1, false);
}

// Drops the first `count` frames of the window (mu_ held). The frame shown last moves to `shown_`;
// the other buffers go to the pool, so neither is copied nor allocated again.
void StreamDecoder::drop(std::size_t count) {
    auto recycle = [&](std::vector<std::uint8_t>&& buf) {
        if (!buf.empty() && pool_.size() < 4) pool_.push_back(std::move(buf));
    };
    for (std::size_t i = 0; i < count && !window_.empty(); ++i) {
        Frame& f = window_.front();
        if (f.frame == last_) {
            std::swap(shown_, f);
            last_ = -1;
        }
        recycle(std::move(f.v210));
        window_.pop_front();
    }
}

void StreamDecoder::main() {
    MezzanineReader reader;
    DirectReader direct;  // an original that plays direct (a "direct:" key)
    bool is_direct = false;
    std::string open_path;
    std::string open_format;  // a direct reader conforms to one format: another one reopens it
    std::string last_error;
    Frame next;
    while (!stop_) {
        std::string mezz;
        VideoFormat fmt;
        std::int64_t k = 0, lo = 0, hi = 1;
        bool wrap = true;
        std::uint64_t gen = 0;
        bool work = false;
        {
            std::unique_lock lock(mu_);
            ahead_ = static_cast<int>(window_.size());
            const auto cap = static_cast<std::size_t>(std::max(2, preroll_));
            if (owner_ && !mezz_.empty() && window_.size() < cap) {
                work = true;
                mezz = mezz_;
                fmt = fmt_;
                gen = gen_;
                lo = lo_;
                hi = hi_;
                wrap = wrap_;
                if (next.v210.empty() && !pool_.empty()) {
                    next.v210 = std::move(pool_.back());
                    pool_.pop_back();
                }
                // An empty window starts one frame after the writer's (it shows that one by the
                // time the frame is decoded); a paused writer keeps asking for its frame. A primed
                // window starts at the frame itself.
                k = window_.empty() ? want_ : window_.back().frame + 1;
                if (window_.empty() && !exact_ && k > lo) ++k;
            } else {
                cv_.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
        if (!work) {
            std::lock_guard lock(mu_);
            if (!open_path.empty() && !owner_) {
                reader.close();
                direct.close();
                open_path.clear();
            }
            continue;
        }
        if (k < lo) k = lo;
        if (k >= hi) {
            if (!wrap) {
                std::unique_lock lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            k = lo + (k - lo) % std::max<std::int64_t>(1, hi - lo);
        }
        std::string err;
        if (open_path != mezz || open_format != fmt.name) {
            open_path.clear();
            reader.close();
            direct.close();
            DirectSpec spec;
            is_direct = DirectSpec::parse(mezz, spec);
            const bool opened = is_direct ? direct.open(spec, fmt, err)
                                          : reader.open(mezz, err) && reader.width() == fmt.width && reader.height() == fmt.height;
            if (!opened) {
                if (err.empty()) err = "mezzanine size differs from the output format";
                if (err != last_error) log_error("output " + std::to_string(output_) + ": stream: " + err);
                last_error = err;
                reader.close();
                direct.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            open_path = mezz;
            open_format = fmt.name;
        }
        const std::uint64_t a0 = samples_until_grain(static_cast<std::uint64_t>(k), fmt.frame_rate);
        const int count = static_cast<int>(samples_in_grain(static_cast<std::uint64_t>(k), fmt.frame_rate));
        next.frame = k;
        next.channels = is_direct ? direct.channels() : reader.channels();
        next.v210.resize(v210_size(fmt.width, fmt.height));
        next.audio.assign(static_cast<std::size_t>(count) * static_cast<std::size_t>(std::max(0, next.channels)), 0.f);
        float* audio = next.channels > 0 ? next.audio.data() : nullptr;
        const bool read = is_direct ? direct.read(k, next.v210.data(), audio, a0, count, err) : reader.read(k, next.v210.data(), audio, a0, count, err);
        if (!read) {
            if (err != last_error) log_error("output " + std::to_string(output_) + ": stream frame " + std::to_string(k) + ": " + err);
            last_error = err;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        last_error.clear();
        std::lock_guard lock(mu_);
        if (gen == gen_ && mezz == mezz_) {
            window_.push_back(std::move(next));
            next = Frame{};
        }
    }
}

}  // namespace mtp
