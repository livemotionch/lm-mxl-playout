#include "media.hpp"

#include "conform.hpp"
#include "motion.hpp"
#include "decode.hpp"
#include "direct.hpp"
#include "process.hpp"
#include "util.hpp"
#include "uuid_util.hpp"
#include "v210.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace mtp {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ext_of(const std::string& path) {
    auto e = fs::path(path).extension().string();
    if (e.empty()) return ".bin";
    return e;
}

json probe_json(const std::string& path) {
    auto r = run_process({"ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path});
    if (r.code != 0) return json::object();
    try {
        return json::parse(r.output);
    } catch (...) {
        return json::object();
    }
}

bool is_animated_codec(const std::string& codec) {
    return codec == "gif" || codec == "apng" || codec == "webp";
}

// One entry of index.json, or an item.json. Throws on a field of the wrong type.
LibraryItem item_from_json(const json& el) {
    LibraryItem it;
    it.id = el.value("id", "");
    it.name = el.value("name", it.id);
    it.type = parse_item_type(el.value("type", "video"));
    if (el.contains("tags")) {
        for (const auto& t : el.at("tags")) it.tags.push_back(t.get<std::string>());
    }
    it.original = el.value("original", json::object());
    it.fit = el.value("fit", "fit");
    it.fps_mode = el.value("fps_mode", "drop");
    it.alpha_mode = el.value("alpha_mode", "straight");
    it.loudness = el.value("loudness", false);
    it.crossfade_ms = el.value("crossfade_ms", 0);
    it.map_channels = el.value("map_channels", 0);
    it.conversions = el.value("conversions", json::object());
    it.direct = el.value("direct", false);
    return it;
}

// Written to a temporary file next to `path` and renamed over it: a reader sees the old file or
// the new one, never a part of one, and a file that cannot be written leaves the old one as it
// was. A failure is logged (it was silent before 1.3.1).
bool write_json_atomic(const fs::path& path, const json& j) {
    // Unique per write: two processes on one library (an old and a new pod) never share it.
    const auto tmp = fs::path(path.string() + ".tmp-" + std::to_string(std::random_device{}()));
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2, ' ', false, json::error_handler_t::replace);  // invalid UTF-8 does not throw
        out.close();
        if (!out) {
            log_error("library: cannot write " + tmp.string() + "; " + path.filename().string() + " is not updated");
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        log_error("library: cannot replace " + path.string() + " (" + ec.message() + ")");
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::uint64_t dir_bytes(const fs::path& p) {
    std::uint64_t n = 0;
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) n += static_cast<std::uint64_t>(it->file_size());
    }
    return n;
}

}  // namespace

std::shared_ptr<void> Library::reserve_ram(std::uint64_t bytes) {
    auto budget = ram_;
    std::uint64_t used = budget->used.load();
    do {
        if (used + bytes > budget->limit) return nullptr;
    } while (!budget->used.compare_exchange_weak(used, used + bytes));
    return std::shared_ptr<void>(new char, [budget, bytes](void* p) {
        delete static_cast<char*>(p);
        budget->used -= bytes;
    });
}

std::uint64_t Library::ram_used() const { return ram_->used.load(); }
std::uint64_t Library::ram_budget() const { return ram_->limit; }

std::shared_ptr<const Library::SharedBlob> Library::shared_blob(const std::string& key, std::uint64_t bytes,
                                                                const std::function<bool(std::vector<std::uint8_t>&)>& load, std::string& error) {
    std::lock_guard lock(blob_mu_);
    for (auto it = blobs_.begin(); it != blobs_.end();) it = it->second.expired() ? blobs_.erase(it) : std::next(it);
    if (auto found = blobs_[key].lock()) return found;
    auto hold = reserve_ram(bytes);
    if (!hold) {
        error = "PLAYER_RAM_BUDGET_MB (one budget for all outputs) is used up: " + std::to_string(ram_used() >> 20) + " of " +
                std::to_string(ram_budget() >> 20) + " MB in use, this clip needs " + std::to_string(bytes >> 20) + " MB more";
        return nullptr;
    }
    auto blob = std::make_shared<SharedBlob>();
    blob->reservation = std::move(hold);
    blob->data.resize(bytes);
    if (!load(blob->data)) {
        if (error.empty()) error = "clip files missing or short; re-convert the item";
        return nullptr;
    }
    std::shared_ptr<const SharedBlob> out = std::move(blob);
    blobs_[key] = out;
    return out;
}

const char* item_type_name(ItemType t) {
    switch (t) {
        case ItemType::Still: return "still";
        case ItemType::Sprite: return "sprite";
        default: return "video";
    }
}

ItemType parse_item_type(std::string_view s) {
    if (s == "still") return ItemType::Still;
    if (s == "sprite") return ItemType::Sprite;
    return ItemType::Video;
}

bool needs_conversion(const LibraryItem& it) {
    return it.loudness || it.fps_mode == "motion" || it.crossfade_ms > 0 || it.map_channels > 0;
}

json direct_entry(const LibraryItem& it, const VideoFormat& format, int ram_budget_mb) {
    const auto& o = it.original;
    if (it.type == ItemType::Still) return json{{"status", "ready"}, {"direct", true}, {"frames", 1}, {"has_alpha", true}, {"audio_samples", 0}};
    if (it.type == ItemType::Sprite) {
        const auto durations = o.value("anim_durations", std::vector<double>{});
        if (durations.empty()) return json{{"status", "failed"}, {"direct", true}, {"error", o.value("anim_error", "the animation could not be decoded")}};
        double total = 0;
        for (double d : durations) total += d;
        json e{{"status", "ready"}, {"direct", true}, {"frames", durations.size()}, {"sprite", true}, {"clip", true},
               {"has_alpha", true}, {"seconds", total}, {"clip_frames", animation_loop_frames(total, format.frame_rate)}};
        // At play the clip takes the fill and the alpha plane (v210a) or the fill and the key
        // (fill_key, the larger): that must fit the budget, else it is a burn-in sprite only.
        const std::uint64_t fill = v210_size(format.width, format.height), a10 = alpha10_size(format.width, format.height);
        const std::uint64_t clip_bytes = durations.size() * std::max(fill + a10, 2 * fill);
        if (clip_bytes > (static_cast<std::uint64_t>(ram_budget_mb) << 20)) {
            e["clip"] = false;
            e["clip_error"] = "clip needs " + std::to_string(clip_bytes >> 20) + " MB at " + format.name + " (" + std::to_string(durations.size()) +
                              " frames), more than PLAYER_RAM_BUDGET_MB (" + std::to_string(ram_budget_mb) + ")";
            e.erase("clip_frames");
        }
        return e;
    }
    if (o.value("codec", "").empty() && o.value("audio_codec", "").empty())
        return json{{"status", "failed"}, {"direct", true}, {"error", "no video or audio the player can read"}};
    const double duration = o.value("video_duration", 0.0) > 0 ? o.value("video_duration", 0.0) : o.value("duration", 0.0);
    const auto frames = direct_frames(duration, format);
    return json{{"status", "ready"},
                {"direct", true},
                {"frames", frames},
                {"audio_channels", o.value("audio_channels", 0)},
                {"audio_samples", samples_until_grain(static_cast<std::uint64_t>(frames), format.frame_rate)},
                {"has_alpha", false}};
}

namespace {

// The moving-box sprite of an animation (frames of at most `max_px`, sprite/ and sprite.json) and
// its thumbnail, decoded once at the upload. Returns the frame durations; empty with `error`.
std::vector<double> write_sprite(const fs::path& original, const fs::path& dir, int max_px, std::string& error) {
    std::error_code ec;
    const auto part = dir / "sprite.part";
    fs::remove_all(part, ec);
    fs::create_directories(part);
    json frames = json::array();
    std::vector<double> durations;
    RgbaFitter fitter;
    RgbaBuffer sprite;
    int sw = 0, sh = 0;
    bool write_failed = false;
    VisualInfo info;
    if (!probe_visual(original.string(), info, error)) return {};
    if (info.frames > kMaxAnimationFrames) {
        error = "animation has " + std::to_string(info.frames) + " frames, more than " + std::to_string(kMaxAnimationFrames);
        return {};
    }
    const bool decoded = decode_visual_frames(original.string(), [&](const RgbaBuffer& f, double d) {
        fitter.fit(f, ScaleMode::SpriteMax, max_px, max_px, sprite);
        char name[32];
        std::snprintf(name, sizeof(name), "frame-%04d.rgba", static_cast<int>(durations.size()));
        std::ofstream out(part / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(sprite.px.data()), static_cast<std::streamsize>(sprite.px.size()));
        if (!out) write_failed = true;
        frames.push_back({{"file", name}, {"duration", d}, {"w", sprite.w}, {"h", sprite.h}});
        if (durations.empty()) {
            sw = sprite.w;
            sh = sprite.h;
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(sw) * sh * 3);
            for (int p = 0; p < sw * sh; ++p)
                for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>(p) * 3 + c] = sprite.px[static_cast<std::size_t>(p) * 4 + c];
            write_jpeg((dir / "thumb.jpg").string(), rgb.data(), sw, sh, 80);
        }
        durations.push_back(d);
        return true;
    }, error);
    double total = 0;
    for (double d : durations) total += d;
    if (decoded && !write_failed) {
        std::ofstream meta(part / "sprite.json");
        meta << json{{"frames", frames}, {"total", total}, {"w", sw}, {"h", sh}}.dump(2);
        meta.close();
        if (!meta) write_failed = true;
    }
    if (!decoded || write_failed || durations.empty()) {
        fs::remove_all(part, ec);
        if (error.empty()) error = write_failed ? "writing the sprite failed (disk full?)" : "the animation has no frame";
        return {};
    }
    // sprite.json last: the writer reads a sprite only once sprite.json is there.
    fs::remove_all(dir / "sprite", ec);
    fs::rename(part / "sprite.json", dir / "sprite.json.part", ec);
    fs::rename(part, dir / "sprite", ec);
    fs::rename(dir / "sprite.json.part", dir / "sprite.json", ec);
    return durations;
}

// Removes the files of every conversion (an item that plays direct again); the original, the
// sprite and the thumbnail stay.
void remove_conversions(const fs::path& dir) {
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        const auto name = ent.path().filename().string();
        for (const char* prefix : {"mezz-", "frame-", "key-", "alpha-", "anim-"}) {
            if (name.rfind(prefix, 0) == 0) {
                fs::remove(ent.path(), ec);
                break;
            }
        }
    }
}

}  // namespace

Library::Library(Config cfg) : cfg_(std::move(cfg)) {
    ram_->limit = static_cast<std::uint64_t>(std::max(0, cfg_.ram_budget_mb)) << 20;
    std::error_code ec;
    fs::create_directories(cfg_.library_dir, ec);
    load_index();
}

Library::~Library() { stop(); }

void Library::start() {
    stop_ = false;
    const int n = std::max(1, cfg_.convert_concurrency);
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { worker(); });
    if (!cfg_.import_dir.empty()) watcher_ = std::thread([this] { watch_import(); });
}

void Library::stop() {
    stop_ = true;
    {
        std::lock_guard lock(mu_);
        // wake workers by pushing nothing; they poll stop_
    }
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    if (watcher_.joinable()) watcher_.join();
}

void Library::load_index() {
    // A bad entry or a bad index.json is logged and skipped, never the rest with it; an item
    // that is not in index.json (an entry skipped here, or one a failed write of index.json left
    // out) is restored from its own item.json.
    const auto dir = fs::path(cfg_.library_dir);
    const auto path = dir / "index.json";
    auto add = [&](LibraryItem it) {
        // Before 1.0.3 a re-import appended the same id again; keep the first entry.
        const bool known = std::any_of(items_.begin(), items_.end(), [&](const LibraryItem& o) { return o.id == it.id; });
        if (it.id.empty() || known) return false;
        items_.push_back(std::move(it));
        return true;
    };
    json j = json::array();
    if (std::ifstream in(path); in) {
        try {
            in >> j;
        } catch (const std::exception& ex) {
            log_error("library: " + path.string() + " is not valid JSON (" + ex.what() + "); items are restored from their item.json");
            j = json::array();
        }
    }
    if (!j.is_array()) {
        log_error("library: " + path.string() + " is not a JSON array; items are restored from their item.json");
        j = json::array();
    }
    std::size_t n = 0;
    for (const auto& el : j) {
        try {
            add(item_from_json(el));
        } catch (const std::exception& ex) {
            log_warn("library: entry " + std::to_string(n) + " of index.json skipped (" + ex.what() + ")");
        }
        ++n;
    }
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (ent.is_directory(ec) && fs::exists(ent.path() / "item.json", ec)) dirs.push_back(ent.path());
        // A temporary index left by a write that did not finish.
        else if (ent.path().filename().string().rfind("index.json.tmp", 0) == 0) fs::remove(ent.path(), ec);
    }
    std::sort(dirs.begin(), dirs.end());
    std::size_t restored = 0;
    for (const auto& d : dirs) {
        try {
            std::ifstream in(d / "item.json");
            json el;
            in >> el;
            auto it = item_from_json(el);
            if (it.id != d.filename().string()) continue;  // not an item folder of its own
            const auto name = it.name;
            if (add(std::move(it))) {
                ++restored;
                log_warn("library: restored " + d.filename().string() + " (" + name + ") from its item.json; it was missing from index.json");
            }
        } catch (const std::exception& ex) {
            log_warn("library: " + (d / "item.json").string() + " not read (" + ex.what() + ")");
        }
    }
    if (restored > 0) save_index();
    log_info("library: " + std::to_string(items_.size()) + " items (" + std::to_string(restored) + " restored from item.json)");
    bytes_ = dir_bytes(cfg_.library_dir);
}

// Callers hold mu_ (or are the constructor): one writer of index.json at a time in a process.
void Library::save_index() const {
    json arr = json::array();
    for (const auto& it : items_) arr.push_back(to_json(it));
    write_json_atomic(fs::path(cfg_.library_dir) / "index.json", arr);
}

void Library::save_item(const LibraryItem& it) const { write_json_atomic(fs::path(item_dir(it.id)) / "item.json", to_json(it)); }

nlohmann::json Library::to_json(const LibraryItem& it) const {
    return json{{"id", it.id},
                {"name", it.name},
                {"type", item_type_name(it.type)},
                {"tags", it.tags},
                {"original", it.original},
                {"fit", it.fit},
                {"fps_mode", it.fps_mode},
                {"alpha_mode", it.alpha_mode},
                {"loudness", it.loudness},
                {"crossfade_ms", it.crossfade_ms},
                {"map_channels", it.map_channels},
                {"conversions", it.conversions},
                {"direct", it.direct},
                {"in_use", it.in_use}};
}

std::string Library::item_dir(const std::string& id) const { return (fs::path(cfg_.library_dir) / id).string(); }

std::vector<LibraryItem> Library::list() const {
    std::lock_guard lock(mu_);
    return items_;
}

LibraryItem Library::get(const std::string& id) const {
    std::lock_guard lock(mu_);
    for (const auto& it : items_)
        if (it.id == id) return it;
    throw Error("item not found");
}

bool Library::exists(const std::string& id) const {
    std::lock_guard lock(mu_);
    for (const auto& it : items_)
        if (it.id == id) return true;
    return false;
}

void Library::enqueue(const std::string& id, const VideoFormat& format) {
    for (auto& j : jobs_) {
        if (j.item_id == id && j.format == format.name && (j.state == "queued" || j.state == "running")) return;
    }
    // Finished jobs are kept for the job list, the oldest dropped beyond 100.
    std::size_t finished = 0;
    for (const auto& j : jobs_) finished += j.state == "done" || j.state == "failed";
    for (auto it = jobs_.begin(); finished > 100 && it != jobs_.end();) {
        if (it->state == "done" || it->state == "failed") {
            it = jobs_.erase(it);
            --finished;
        } else {
            ++it;
        }
    }
    ConvertJob job;
    job.id = uuid_v5(uuid_namespace_dns(), id + format.name + std::to_string(++job_serial_)).str();
    job.item_id = id;
    job.format = format.name;
    job.state = "queued";
    jobs_.push_back(job);
    for (auto& it : items_) {
        if (it.id == id) {
            it.conversions[format.name] = json{{"status", "queued"}, {"log", ""}};
            save_item(it);
        }
    }
    save_index();
}

LibraryItem Library::ingest_file(const std::string& path, const std::string& name, const nlohmann::json& options, const VideoFormat& format) {
    const auto probed = probe_json(path);
    std::string vcodec, acodec, rate = "0/1", scan = "progressive", color_space;
    int w = 0, h = 0, ach = 0;
    double duration = 0, video_duration = 0;
    int vframes = 0;
    if (probed.contains("format")) duration = std::atof(probed["format"].value("duration", "0").c_str());
    for (const auto& st : probed.value("streams", json::array())) {
        // The cover art of an audio file is not its video.
        const bool cover = st.contains("disposition") && st["disposition"].value("attached_pic", 0) == 1;
        if (st.value("codec_type", "") == "video" && w == 0 && !cover) {
            color_space = st.value("color_space", "");
            video_duration = std::atof(st.value("duration", "0").c_str());
            vcodec = st.value("codec_name", "");
            w = st.value("width", 0);
            h = st.value("height", 0);
            rate = st.value("avg_frame_rate", st.value("r_frame_rate", "0/1"));
            const auto fo = st.value("field_order", "progressive");
            scan = (fo == "tt" || fo == "tb") ? "interlaced_tff" : (fo == "bb" || fo == "bt") ? "interlaced_bff" : "progressive";
            vframes = std::atoi(st.value("nb_frames", "0").c_str());
        } else if (st.value("codec_type", "") == "audio" && ach == 0) {
            acodec = st.value("codec_name", "");
            ach = st.value("channels", st.value("nb_channels", 0));
        }
    }
    if (vcodec.empty() && !probed.contains("streams")) vcodec = probe_video_codec(path);  // without ffprobe
    // A file of audio only (or audio with cover art) plays as a video: black, with its audio.
    const bool image_codec = vcodec == "png" || vcodec == "mjpeg" || vcodec == "tiff" || vcodec == "webp" || vcodec == "gif" ||
                             vcodec == "apng" || vcodec == "bmp";
    ItemType type = ItemType::Video;
    if (image_codec && (is_animated_codec(vcodec) || vframes > 1)) type = ItemType::Sprite;
    else if (image_codec) type = ItemType::Still;

    LibraryItem it;
    it.id = uuid_v5(uuid_namespace_dns(), path + "|" + name + "|" + std::to_string(fs::file_size(path))).str();
    {
        // The same file again (the import directory is scanned at every start): keep the item
        // and its conversions. Adding it again listed it twice and converted it again, and an
        // output playing it read the mezzanine while ffmpeg rewrote it.
        std::lock_guard lock(mu_);
        for (const auto& existing : items_) {
            if (existing.id != it.id) continue;
            const auto st = existing.conversions.value(format.name, json::object()).value("status", "");
            if (st != "ready" && !existing.direct) enqueue(existing.id, format);
            return existing;
        }
    }
    it.name = name.empty() ? fs::path(path).stem().string() : name;
    it.type = type;
    it.fit = options.value("fit", "fit");
    it.fps_mode = options.value("fps_mode", "drop");
    it.alpha_mode = options.value("alpha_mode", "straight");
    it.loudness = options.value("loudness", false);
    it.crossfade_ms = options.value("crossfade_ms", 0);
    it.map_channels = options.value("map_channels", 0);
    // Played as uploaded, unless the upload asks for a conversion (or for what only one applies).
    it.direct = !options.value("convert", false) && !needs_conversion(it);
    if (options.contains("tags")) {
        for (const auto& t : options.at("tags")) it.tags.push_back(t.get<std::string>());
    }
    it.original = json{{"filename", "original" + ext_of(path)},
                       {"codec", vcodec},
                       {"audio_codec", acodec},
                       {"width", w},
                       {"height", h},
                       {"rate", rate},
                       {"scan", scan},
                       {"duration", duration},
                       {"audio_channels", ach},
                       {"frames", vframes},
                       {"color_space", color_space},
                       {"video_duration", video_duration}};
    const auto dir = fs::path(item_dir(it.id));
    fs::create_directories(dir);
    const auto original = dir / it.original["filename"].get<std::string>();
    fs::copy_file(path, original, fs::copy_options::overwrite_existing);
    if (it.direct) {
        // No conversion: what the outputs need to play it is read now, and the thumbnail made.
        std::string err;
        if (it.type == ItemType::Sprite) {
            const auto durations = write_sprite(original, dir, cfg_.sprite_max_px, err);
            if (durations.empty()) it.original["anim_error"] = err;
            else it.original["anim_durations"] = durations;
        } else if (it.type == ItemType::Still) {
            std::vector<std::uint8_t> fill, key, a10;
            if (render_still(original.string(), it.fit, it.alpha_mode == "premultiplied", format, fill, key, a10, err)) {
                v210_to_jpeg((dir / "thumb.jpg").string(), fill.data(), format.width, format.height, 320);
            }
        } else if (!vcodec.empty()) {
            run_process({"ffmpeg", "-hide_banner", "-y", "-ss", std::to_string(std::min(1.0, duration / 2)), "-i", original.string(), "-vf",
                         "scale=320:-2", "-frames:v", "1", (dir / "thumb.jpg").string()});
        }
        it.conversions[format.name] = direct_entry(it, format, cfg_.ram_budget_mb);
        log_info("library: " + it.name + " plays direct (" + (vcodec.empty() ? acodec : vcodec) + "): " + it.conversions[format.name].value("status", "") + " " + it.conversions[format.name].value("error", ""));
    }
    {
        std::lock_guard lock(mu_);
        items_.push_back(it);
        if (it.direct) {
            save_item(it);
            save_index();
        } else {
            enqueue(it.id, format);
        }
        bytes_ = dir_bytes(cfg_.library_dir);
    }
    if (on_change) on_change();
    return it;
}

bool Library::reconvert(const std::string& id, const nlohmann::json& options, const VideoFormat& format, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto& it : items_) {
        if (it.id != id) continue;
        if (options.contains("fit")) it.fit = options.at("fit").get<std::string>();
        if (options.contains("fps_mode")) it.fps_mode = options.at("fps_mode").get<std::string>();
        if (options.contains("alpha_mode")) it.alpha_mode = options.at("alpha_mode").get<std::string>();
        if (options.contains("loudness")) it.loudness = options.at("loudness").get<bool>();
        if (options.contains("crossfade_ms")) it.crossfade_ms = options.at("crossfade_ms").get<int>();
        if (options.contains("map_channels")) it.map_channels = options.at("map_channels").get<int>();
        // {"direct": true}: play the original again (when no option needs a conversion).
        if (options.value("direct", false)) {
            if (needs_conversion(it)) {
                error = "loudness, motion interpolation, a crossfade or a channel mapping needs a conversion";
                return false;
            }
            for (const auto& j : jobs_)
                if (j.item_id == id && (j.state == "queued" || j.state == "running")) {
                    error = "a conversion of this item is running";
                    return false;
                }
            const auto dir = fs::path(item_dir(id));
            if (it.type == ItemType::Sprite && !it.original.contains("anim_durations")) {
                std::string err;
                const auto durations = write_sprite(dir / it.original.value("filename", "original.bin"), dir, cfg_.sprite_max_px, err);
                if (durations.empty()) it.original["anim_error"] = err;
                else it.original["anim_durations"] = durations;
            }
            it.direct = true;
            remove_conversions(dir);
            it.conversions = json::object();
            it.conversions[format.name] = direct_entry(it, format, cfg_.ram_budget_mb);
            save_item(it);
            save_index();
            bytes_ = dir_bytes(cfg_.library_dir);
            return true;
        }
        it.direct = false;
        enqueue(id, format);
        return true;
    }
    error = "not found";
    return false;
}

bool Library::remove(const std::string& id, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id != id) continue;
        if (it->in_use) {
            error = "item is in use";
            return false;
        }
        std::error_code ec;
        // item.json first: a folder that is not removed completely is not restored at the next start.
        fs::remove(fs::path(item_dir(id)) / "item.json", ec);
        fs::remove_all(item_dir(id), ec);
        items_.erase(it);
        save_index();
        bytes_ = dir_bytes(cfg_.library_dir);
        return true;
    }
    error = "not found";
    return false;
}

bool Library::update_meta(const std::string& id, const std::string& name, const std::vector<std::string>& tags, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto& it : items_) {
        if (it.id != id) continue;
        if (!name.empty()) it.name = name;
        it.tags = tags;
        save_item(it);
        save_index();
        return true;
    }
    error = "not found";
    return false;
}

void Library::purge_uploads() {
    std::error_code ec;
    fs::remove_all(fs::path(cfg_.library_dir) / "_uploads", ec);
}

void Library::mark_used(const std::string& id, bool used) {
    std::lock_guard lock(mu_);
    for (auto& it : items_)
        if (it.id == id) it.in_use = used;
}

void Library::ensure_format(const std::string& id, const VideoFormat& format) {
    std::lock_guard lock(mu_);
    for (auto& it : items_) {
        if (it.id != id) continue;
        if (it.direct) {
            // Another output format: only its frame count is new.
            if (!it.conversions.contains(format.name)) {
                it.conversions[format.name] = direct_entry(it, format, cfg_.ram_budget_mb);
                save_item(it);
                save_index();
            }
            return;
        }
        const auto conv = it.conversions.value(format.name, json::object());
        const auto st = conv.value("status", "");
        // A sprite converted before 1.3.0 has no clip yet: it is converted once more.
        const bool old_sprite = st == "ready" && it.type == ItemType::Sprite && !conv.contains("clip");
        // A failed conversion is not queued again here (each play would queue it again, without
        // end); only an explicit re-convert does.
        if ((st == "ready" && !old_sprite) || st == "queued" || st == "running" || st == "failed") return;
        enqueue(id, format);
        return;
    }
}

std::vector<ConvertJob> Library::jobs() const {
    std::lock_guard lock(mu_);
    return jobs_;
}

std::uint64_t Library::total_bytes() const {
    std::lock_guard lock(mu_);
    return bytes_;
}

int Library::item_count() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(items_.size());
}

bool Library::wait_ready(const std::string& id, const std::string& format, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mu_);
            for (const auto& it : items_) {
                if (it.id != id) continue;
                const auto st = it.conversions.value(format, json::object()).value("status", "");
                if (st == "ready") return true;
                if (st == "failed") return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void Library::worker() {
    while (!stop_) {
        ConvertJob job;
        VideoFormat fmt;
        LibraryItem snapshot;
        bool have = false;
        {
            std::lock_guard lock(mu_);
            for (auto& j : jobs_) {
                if (j.state == "queued") {
                    j.state = "running";
                    job = j;
                    have = true;
                    break;
                }
            }
            if (have) {
                auto parsed = parse_format(job.format);
                if (!parsed) {
                    for (auto& j : jobs_)
                        if (j.id == job.id) {
                            j.state = "failed";
                            j.error = "bad format";
                        }
                    have = false;
                } else {
                    fmt = *parsed;
                    for (auto& it : items_)
                        if (it.id == job.item_id) snapshot = it;
                }
            }
        }
        if (!have) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = convert_one(snapshot, fmt, job);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard lock(mu_);
            for (auto& it : items_) {
                if (it.id == snapshot.id) {
                    it.conversions[fmt.name] = snapshot.conversions[fmt.name];
                    it.conversions[fmt.name]["duration_s"] = secs;
                    save_item(it);
                }
            }
            for (auto& j : jobs_) {
                if (j.id == job.id) {
                    j.state = ok ? "done" : "failed";
                    j.progress = ok ? 1 : j.progress;
                    j.error = job.error;
                    j.duration_s = secs;
                }
            }
            save_index();
            bytes_ = dir_bytes(cfg_.library_dir);
        }
        if (on_change) on_change();
    }
}

bool Library::convert_one(LibraryItem& item, const VideoFormat& format, ConvertJob& job) {
    try {
        const auto dir = fs::path(item_dir(item.id));
        const auto original = dir / item.original.value("filename", "original.bin");
        std::ofstream logf(dir / "convert.log", std::ios::app);
        auto log = [&](const std::string& s) {
            logf << s << "\n";
            job.error.clear();
        };
        if (item.type == ItemType::Still || item.type == ItemType::Sprite) {
            auto fail = [&](const std::string& err) {
                log("failed: " + err);
                job.error = err;
                item.conversions[format.name] = json{{"status", "failed"}, {"error", err}};
                return false;
            };
            // Counted from the packets first: an animation that cannot fit is refused before
            // anything is decoded.
            VisualInfo info;
            std::string err;
            if (!probe_visual(original.string(), info, err)) return fail(err);
            if (info.frames > kMaxAnimationFrames)
                return fail("animation has " + std::to_string(info.frames) + " frames, more than " + std::to_string(kMaxAnimationFrames));
            // A multi-frame image uploaded as a still is a sprite.
            if (item.type == ItemType::Still && info.frames > 1) item.type = ItemType::Sprite;
            const ScaleMode fit = item.fit == "fill" ? ScaleMode::Fill : item.fit == "center" || item.fit == "1:1" ? ScaleMode::Center : ScaleMode::Fit;
            const bool premul = item.alpha_mode == "premultiplied";
            const std::uint64_t budget = static_cast<std::uint64_t>(cfg_.ram_budget_mb) << 20;
            const std::size_t fill_bytes = v210_size(format.width, format.height);
            const std::size_t a10_bytes = alpha10_size(format.width, format.height);
            RgbaFitter fitter;
            RgbaBuffer canvas;
            if (item.type == ItemType::Still) {
                std::vector<std::uint8_t> fill(fill_bytes), key(fill_bytes), a10(a10_bytes);
                const bool decoded = decode_visual_frames(original.string(), [&](const RgbaBuffer& f, double) {
                    fitter.fit(f, fit, format.width, format.height, canvas);
                    rgba_to_fill_key(canvas.px.data(), canvas.w, canvas.h, premul, fill.data(), key.data(), a10.data(), 64, 940);
                    return false;  // a still needs the first frame only
                }, err);
                if (!decoded) return fail(err);
                std::ofstream(dir / ("frame-" + format.name + ".v210"), std::ios::binary).write(reinterpret_cast<const char*>(fill.data()), static_cast<std::streamsize>(fill.size()));
                std::ofstream(dir / ("key-" + format.name + ".v210"), std::ios::binary).write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key.size()));
                std::ofstream(dir / ("alpha-" + format.name + ".a10"), std::ios::binary).write(reinterpret_cast<const char*>(a10.data()), static_cast<std::streamsize>(a10.size()));
                v210_to_jpeg((dir / "thumb.jpg").string(), fill.data(), format.width, format.height, 320);
                item.conversions[format.name] = json{{"status", "ready"}, {"frames", 1}, {"has_alpha", true}, {"audio_samples", 0}};
                log("still converted");
                return true;
            }
            // An animation (GIF, APNG): decoded once, frame by frame, into the moving-box sprite
            // (frames of at most PLAYER_SPRITE_MAX_PX) and into a clip at this format (fill and
            // alpha per frame, item.fit), each frame written out as it comes. Both must fit
            // PLAYER_RAM_BUDGET_MB, as they are played from RAM; a clip that does not is skipped
            // with clip_error and the sprite stays usable.
            const int sp = cfg_.sprite_max_px;
            const auto frames_n = static_cast<std::uint64_t>(info.frames);
            // Every frame is decoded at the file's own size: at most as many pixels in all as the
            // budget holds in RGBA, so a small file of many large frames is refused before decoding.
            const std::uint64_t decode_px = frames_n * static_cast<std::uint64_t>(info.w) * static_cast<std::uint64_t>(info.h);
            if (decode_px * 4 > budget)
                return fail("animation of " + std::to_string(info.frames) + " frames of " + std::to_string(info.w) + "x" + std::to_string(info.h) +
                            " has " + std::to_string(decode_px / 1000000) + " Mpx to decode, more than PLAYER_RAM_BUDGET_MB allows (" +
                            std::to_string(budget / 4 / 1000000) + " Mpx)");
            if (frames_n * static_cast<std::uint64_t>(sp) * sp * 4 > budget)
                return fail("animation of " + std::to_string(info.frames) + " frames does not fit PLAYER_RAM_BUDGET_MB as a sprite");
            // At play a clip takes the fill and the alpha plane (v210a) or the fill and the key
            // (fill_key, the larger): that must fit the budget.
            const std::uint64_t clip_bytes = frames_n * std::max(fill_bytes + a10_bytes, 2 * fill_bytes);
            std::string clip_error;
            if (clip_bytes > budget) {
                clip_error = "clip needs " + std::to_string(clip_bytes >> 20) + " MB at " + format.name + " (" + std::to_string(info.frames) +
                             " frames), more than PLAYER_RAM_BUDGET_MB (" + std::to_string(cfg_.ram_budget_mb) + ")";
                log(clip_error);
            }
            const bool clip = clip_error.empty();
            std::error_code ec;
            const auto fill_path = dir / ("anim-" + format.name + ".v210");
            const auto a10_path = dir / ("anim-" + format.name + ".a10");
            const auto fill_part = dir / ("anim-" + format.name + ".v210.part");
            const auto a10_part = dir / ("anim-" + format.name + ".a10.part");
            std::ofstream fill_out, a10_out;
            if (clip) {
                fill_out.open(fill_part, std::ios::binary);
                a10_out.open(a10_part, std::ios::binary);
            }
            // The sprite does not depend on the format: written once (also kept from before 1.3.0),
            // into sprite.part and renamed, so the writer never reads a file being written.
            const bool write_sprite = !fs::exists(dir / "sprite.json");
            const auto sprite_part = dir / "sprite.part";
            if (write_sprite) {
                fs::remove_all(sprite_part, ec);
                fs::create_directories(sprite_part);
            }
            std::vector<std::uint8_t> fill(clip ? fill_bytes : 0), a10(clip ? a10_bytes : 0);
            json frames = json::array();
            std::vector<double> durations;
            double total = 0;
            RgbaFitter sprite_fitter;  // one per size: a scaler is set up for one size
            RgbaBuffer sprite;
            int sw = 0, sh = 0;
            bool write_failed = false;
            const bool decoded = decode_visual_frames(original.string(), [&](const RgbaBuffer& f, double d) {
                if (write_sprite) {
                    sprite_fitter.fit(f, ScaleMode::SpriteMax, sp, sp, sprite);
                    char name[32];
                    std::snprintf(name, sizeof(name), "frame-%04d.rgba", static_cast<int>(durations.size()));
                    std::ofstream out(sprite_part / name, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(sprite.px.data()), static_cast<std::streamsize>(sprite.px.size()));
                    if (!out) write_failed = true;
                    frames.push_back({{"file", name}, {"duration", d}, {"w", sprite.w}, {"h", sprite.h}});
                    if (durations.empty()) {
                        sw = sprite.w;
                        sh = sprite.h;
                        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(sw) * sh * 3);
                        for (int p = 0; p < sw * sh; ++p)
                            for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>(p) * 3 + c] = sprite.px[static_cast<std::size_t>(p) * 4 + c];
                        write_jpeg((dir / "thumb.jpg").string(), rgb.data(), sw, sh, 80);
                    }
                }
                durations.push_back(d);
                total += d;
                if (clip) {
                    fitter.fit(f, fit, format.width, format.height, canvas);
                    rgba_to_fill_key(canvas.px.data(), canvas.w, canvas.h, premul, fill.data(), nullptr, a10.data(), 64, 940);
                    fill_out.write(reinterpret_cast<const char*>(fill.data()), static_cast<std::streamsize>(fill.size()));
                    a10_out.write(reinterpret_cast<const char*>(a10.data()), static_cast<std::streamsize>(a10.size()));
                    if (!fill_out || !a10_out) write_failed = true;
                }
                return !write_failed;
            }, err);
            if (clip) {
                fill_out.close();
                a10_out.close();
            }
            if (write_sprite && !write_failed && decoded) {
                std::ofstream meta(sprite_part / "sprite.json");
                meta << json{{"frames", frames}, {"total", total}, {"w", sw}, {"h", sh}}.dump(2);
                meta.close();
                if (!meta) write_failed = true;
            }
            if (!decoded || write_failed || (clip && (!fill_out || !a10_out))) {
                fs::remove(fill_part, ec);
                fs::remove(a10_part, ec);
                if (write_sprite) fs::remove_all(sprite_part, ec);
                return fail(decoded ? "writing the conversion failed (disk full?)" : err);
            }
            if (write_sprite) {
                // sprite.json last: the writer reads a sprite only once sprite.json is there.
                fs::remove_all(dir / "sprite", ec);
                fs::rename(sprite_part / "sprite.json", dir / "sprite.json.part", ec);
                fs::rename(sprite_part, dir / "sprite", ec);
                fs::rename(dir / "sprite.json.part", dir / "sprite.json", ec);
            }
            if (clip) {
                // Renamed over the clip of an earlier conversion: an output loading it sees the old or the new one.
                fs::rename(fill_part, fill_path, ec);
                fs::rename(a10_part, a10_path, ec);
            } else {
                fs::remove(fill_path, ec);
                fs::remove(a10_path, ec);
            }
            const int n = static_cast<int>(durations.size());
            json conv{{"status", "ready"}, {"frames", n}, {"sprite", true}, {"clip", clip}, {"has_alpha", true}, {"seconds", total}};
            if (clip) conv["clip_frames"] = animation_loop_frames(total, format.frame_rate);
            else conv["clip_error"] = clip_error;
            item.conversions[format.name] = conv;
            log("animation frames " + std::to_string(n) + (clip ? ", clip " + std::to_string(clip_bytes >> 20) + " MB" : ""));
            return true;
        }

        // Video mezzanine: the filters a direct play uses (direct.cpp), and motion interpolation.
        const std::string vf = conform_filter(item.fit, item.fps_mode, item.original.value("scan", "progressive"),
                                              item.original.value("color_space", ""), format);
        const auto video_only = dir / "video-tmp.mov";
        const auto mezz = dir / ("mezz-" + format.name + ".mov");
        // PLAYER_CONVERT_THREADS for the decoder, the filters and the encoder: an ffmpeg with a
        // thread per host core used the pod's whole CPU limit, and the outputs underran.
        const std::string threads = std::to_string(cfg_.convert_threads);
        auto pr = run_process({"ffmpeg", "-hide_banner", "-y", "-threads", threads, "-i", original.string(), "-an", "-vf", vf, "-filter_threads",
                               threads, "-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le", "-threads", threads, video_only.string()},
                              [&](std::string_view line) {
                                  auto s = std::string(line);
                                  if (s.rfind("out_time_ms=", 0) == 0) {
                                      // progress is informational
                                  }
                              });
        // ffmpeg writes progress to stdout only with -progress. Parse stderr for errors.
        if (pr.code != 0) {
            job.error = "video transcode failed";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}, {"log", pr.output}};
            log(pr.output);
            return false;
        }
        const int dst_ch = item.map_channels > 0 ? item.map_channels : std::max(2, item.original.value("audio_channels", 0));
        const int use_ch = std::clamp(dst_ch, 2, 64);
        std::string af = "aresample=48000";
        if (item.loudness) af = "loudnorm=I=-23:TP=-1.5:LRA=11," + af;
        const auto raw = dir / "audio.f32le";
        auto ar = run_process({"ffmpeg", "-hide_banner", "-y", "-i", original.string(), "-vn", "-af", af, "-f", "f32le", "-ac",
                               std::to_string(std::max(1, item.original.value("audio_channels", 0) == 0 ? 1 : item.original.value("audio_channels", 1))),
                               raw.string()});
        std::vector<float> src_audio;
        int src_ch = std::max(1, item.original.value("audio_channels", 0));
        if (item.original.value("audio_channels", 0) == 0) src_ch = 0;
        if (ar.code == 0 && src_ch > 0 && fs::exists(raw)) {
            const auto bytes = fs::file_size(raw);
            src_audio.resize(bytes / sizeof(float));
            std::ifstream in(raw, std::ios::binary);
            in.read(reinterpret_cast<char*>(src_audio.data()), static_cast<std::streamsize>(bytes));
        }
        const auto probed = probe_json(video_only.string());
        std::int64_t frames = 0;
        for (const auto& st : probed.value("streams", json::array())) {
            if (st.value("codec_type", "") == "video") frames = std::atoll(st.value("nb_frames", "0").c_str());
        }
        if (frames <= 0) {
            job.error = "mezzanine has no frames";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}};
            return false;
        }
        const std::uint64_t need = samples_until_grain(static_cast<std::uint64_t>(frames), format.frame_rate);
        std::vector<float> dst(need * static_cast<std::uint64_t>(use_ch), 0.f);
        const std::uint64_t src_samples = src_ch > 0 ? src_audio.size() / static_cast<std::uint64_t>(src_ch) : 0;
        const int xfade = static_cast<int>(item.crossfade_ms * 48);
        conform_interleaved(src_audio.empty() ? nullptr : src_audio.data(), src_ch, src_samples, dst.data(), use_ch, need, xfade);
        const auto conf = dir / "audio-conformed.f32le";
        {
            std::ofstream out(conf, std::ios::binary);
            out.write(reinterpret_cast<const char*>(dst.data()), static_cast<std::streamsize>(dst.size() * sizeof(float)));
        }
        auto mx = run_process({"ffmpeg", "-hide_banner", "-y", "-i", video_only.string(), "-f", "f32le", "-ar", "48000", "-ac",
                               std::to_string(use_ch), "-i", conf.string(), "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a",
                               "pcm_f32le", "-shortest", mezz.string()});
        if (mx.code != 0) {
            job.error = "mux failed";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}, {"log", mx.output}};
            log(mx.output);
            return false;
        }
        run_process({"ffmpeg", "-hide_banner", "-y", "-i", mezz.string(), "-vf", "scale=320:-1", "-frames:v", "1", (dir / "thumb.jpg").string()});
        std::error_code ec;
        fs::remove(video_only, ec);
        fs::remove(raw, ec);
        fs::remove(conf, ec);
        item.conversions[format.name] = json{{"status", "ready"},
                                             {"frames", frames},
                                             {"audio_samples", need},
                                             {"audio_channels", use_ch},
                                             {"mezz", mezz.filename().string()},
                                             {"has_alpha", false}};
        log("mezzanine frames " + std::to_string(frames) + " samples " + std::to_string(need));
        return true;
    } catch (const std::exception& ex) {
        job.error = ex.what();
        item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}};
        return false;
    }
}

void Library::watch_import() {
    std::map<std::string, std::uint64_t> seen;
    while (!stop_) {
        std::error_code ec;
        if (fs::exists(cfg_.import_dir, ec)) {
            for (const auto& ent : fs::directory_iterator(cfg_.import_dir, ec)) {
                if (!ent.is_regular_file()) continue;
                const auto p = ent.path().string();
                const auto sz = ent.file_size();
                auto it = seen.find(p);
                if (it != seen.end() && it->second == sz) continue;
                // Wait until the size is stable.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::error_code ec2;
                const auto sz2 = fs::file_size(p, ec2);
                if (ec2 || sz2 != sz) continue;
                seen[p] = sz;
                try {
                    ingest_file(p, ent.path().filename().string(), json::object(), cfg_.format);
                    log_info("imported " + p);
                } catch (const std::exception& ex) {
                    log_warn(std::string("import failed: ") + ex.what());
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

}  // namespace mtp
