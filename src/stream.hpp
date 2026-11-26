#pragma once

#include "decode.hpp"
#include "format.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

// Decode-ahead for a video that streams from its mezzanine (§7): a thread decodes the frames from
// the writer's playhead on, up to `preroll` ahead, in play order: frames lo..hi-1, then lo again
// when `wrap`. The writer takes the frame it shows. An output has two: the clip on air and the next
// one (preloaded at its in point, so a cut to it is clean), or the two clips of a transition.
class StreamDecoder {
public:
    struct Frame {
        std::int64_t frame = 0;
        std::vector<std::uint8_t> v210;
        std::vector<float> audio;  // the frame's samples, interleaved, `channels` per sample
        int channels = 0;
    };

    StreamDecoder(int output, int preroll);
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    void start();
    void stop();
    void set_format(const VideoFormat& fmt);

    // Frame `frame` of the mezzanine `mezz` (`owner` names the loaded media: a new owner starts a
    // new window), or null while it is not decoded yet; then shown() holds the frame shown last.
    // Frames before it are dropped; a frame the window does not hold restarts it there (a seek, a
    // new item). The pointer stays valid until the next call (the thread only appends).
    const Frame* take(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t frame, std::int64_t lo, std::int64_t hi,
                      bool wrap);
    // Decodes from `frame` on (exactly, not one ahead) without taking it: a cued clip at its in point.
    void prime(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t frame, std::int64_t lo, std::int64_t hi,
               bool wrap);
    // Lets go of the media.
    void release();
    const std::shared_ptr<const void>& owner() const { return owner_; }  // writer thread only
    const Frame& shown() const { return shown_; }  // writer thread only
    int ahead() const { return ahead_.load(); }

private:
    void bind(const std::shared_ptr<const void>& owner, const std::string& mezz, std::int64_t lo, std::int64_t hi, bool wrap);
    void drop(std::size_t count);
    void main();

    int output_;
    int preroll_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> ahead_{0};

    std::mutex mu_;
    std::condition_variable cv_;
    VideoFormat fmt_;
    std::shared_ptr<const void> owner_;
    std::string mezz_;
    std::int64_t lo_ = 0, hi_ = 0;
    bool wrap_ = true;
    std::int64_t want_ = 0;       // the frame the writer showed last (or primed)
    bool exact_ = false;          // an empty window starts at want_, not one after it
    std::uint64_t gen_ = 0;       // counts restarts of the window (a seek, new media)
    std::deque<Frame> window_;
    std::int64_t last_ = -1;      // the frame take returned last (still in the window)
    Frame shown_;                 // that frame once it left the window
    std::vector<std::vector<std::uint8_t>> pool_;  // frame buffers to reuse
};

}  // namespace mtp
