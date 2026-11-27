#pragma once

#include "config.hpp"
#include "format.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

enum class ItemType { Video, Still, Sprite };

struct LibraryItem {
    std::string id;
    std::string name;
    ItemType type = ItemType::Video;
    std::vector<std::string> tags;
    nlohmann::json original = nlohmann::json::object();
    std::string fit = "fit";
    std::string fps_mode = "drop";
    std::string alpha_mode = "straight";
    // The clip's level: `gain_db` is applied at playout (no conversion), to every output. With
    // `loudness` it is set from the measured loudness to the target (PLAYER_LOUDNESS_TARGET,
    // -23 LUFS), lowered so the true peak stays at or below -1 dBTP.
    bool loudness = false;
    double gain_db = 0;
    // The loudness of the original (EBU R128): status (queued, measuring, ready, failed, none for
    // no audio), integrated_lufs, true_peak_dbtp, lra_lu, error.
    nlohmann::json level = nlohmann::json::object();
    int crossfade_ms = 0;
    int map_channels = 0;  // 0 = keep source, pad
    // Plays the original as it is (no conversion): its `conversions` entries say `direct`.
    bool direct = false;
    nlohmann::json conversions = nlohmann::json::object();
    bool in_use = false;
};

struct ConvertJob {
    std::string id;
    std::string item_id;
    std::string format;
    std::string state;  // queued, running, done, failed
    double progress = 0;
    std::string error;
    double duration_s = 0;
};

class Library {
public:
    explicit Library(Config cfg);
    ~Library();

    void start();
    void stop();

    std::vector<LibraryItem> list() const;
    LibraryItem get(const std::string& id) const;
    bool exists(const std::string& id) const;
    nlohmann::json to_json(const LibraryItem& it) const;

    // Assemble a finished upload into the library and queue conversion for `format`.
    LibraryItem ingest_file(const std::string& path, const std::string& name, const nlohmann::json& options, const VideoFormat& format);

    bool reconvert(const std::string& id, const nlohmann::json& options, const VideoFormat& format, std::string& error);
    bool remove(const std::string& id, std::string& error);
    bool update_meta(const std::string& id, const std::string& name, const std::vector<std::string>& tags, std::string& error);
    void mark_used(const std::string& id, bool used);
    // {"gain_db": dB} sets the gain by hand (and ends `loudness`); {"loudness": true} sets it to
    // the target from the measurement, now or when the measurement is done. {"measure": true}
    // measures the original again.
    bool set_level(const std::string& id, const nlohmann::json& body, std::string& error);
    double loudness_target() const { return cfg_.loudness_target_lufs; }
    void purge_uploads();

    void ensure_format(const std::string& id, const VideoFormat& format);
    // Threads a direct video decodes with (PLAYER_DECODE_THREADS).
    int decode_threads() const { return cfg_.decode_threads; }

    std::vector<ConvertJob> jobs() const;
    std::string item_dir(const std::string& id) const;
    std::uint64_t total_bytes() const;
    int item_count() const;

    // Blocks until queued work for this item/format is finished or failed. Used by tests.
    bool wait_ready(const std::string& id, const std::string& format, int timeout_ms);

    std::function<void()> on_change;

    // PLAYER_RAM_BUDGET_MB is one budget for the whole process: every output's clips in RAM.
    // A reservation holds `bytes` of it until the last copy of the returned pointer is gone; null
    // when they do not fit.
    std::shared_ptr<void> reserve_ram(std::uint64_t bytes);
    std::uint64_t ram_used() const;
    std::uint64_t ram_budget() const;

    // Decoded clip data, loaded once and shared by every output that plays it (the fill and the
    // alpha of an animation are the same on every output). `key` names the content; `load` fills
    // `bytes` of it. Counted once against the budget while any output holds it.
    struct SharedBlob {
        std::vector<std::uint8_t> data;
        std::shared_ptr<void> reservation;
    };
    std::shared_ptr<const SharedBlob> shared_blob(const std::string& key, std::uint64_t bytes,
                                                  const std::function<bool(std::vector<std::uint8_t>&)>& load, std::string& error);

private:
    void worker();
    void measurer();
    // The gain that brings `it` to the target (its measurement ready), else its gain as it is.
    double auto_gain(const LibraryItem& it) const;
    void watch_import();
    void load_index();
    void save_index() const;
    void save_item(const LibraryItem& it) const;
    bool convert_one(LibraryItem& item, const VideoFormat& format, ConvertJob& job);
    void enqueue(const std::string& id, const VideoFormat& format);

    Config cfg_;
    mutable std::mutex mu_;
    std::vector<LibraryItem> items_;
    std::vector<ConvertJob> jobs_;
    std::vector<std::thread> workers_;
    std::thread watcher_;
    std::thread measurer_;
    std::atomic<bool> stop_{false};
    std::uint64_t bytes_ = 0;
    std::uint64_t job_serial_ = 0;
    struct RamBudget {
        std::atomic<std::uint64_t> used{0};
        std::uint64_t limit = 0;
    };
    std::shared_ptr<RamBudget> ram_ = std::make_shared<RamBudget>();  // outlives the reservations
    std::mutex blob_mu_;  // held while a blob loads: two outputs asking for the same clip load it once
    std::map<std::string, std::weak_ptr<const SharedBlob>> blobs_;
};

const char* item_type_name(ItemType t);

// The gain range of a clip, in dB.
constexpr double kGainMinDb = -40, kGainMaxDb = 20;
// The summary of ffmpeg's ebur128 filter (peak=true) in `log`: integrated_lufs, true_peak_dbtp,
// lra_lu. False when there is none (no audio, or silence: integrated -70 LUFS or below).
bool parse_ebur128(const std::string& log, nlohmann::json& out);
// The gain to `target` LUFS from a measurement, lowered so the true peak stays at -1 dBTP or
// below, within kGainMinDb..kGainMaxDb. `limited` tells whether the peak lowered it.
double gain_to_target(double integrated_lufs, double true_peak_dbtp, double target, bool* limited = nullptr);

// Options that only a conversion can apply (motion interpolation, a crossfade at the
// loop, a channel mapping): with any of them an item is converted, else it plays direct.
bool needs_conversion(const LibraryItem& it);
// The `conversions` entry of a direct item at `format` (frames, audio channels, alpha). An
// animation whose clip needs more than `ram_budget_mb` is a sprite only (clip false, clip_error).
nlohmann::json direct_entry(const LibraryItem& it, const VideoFormat& format, int ram_budget_mb);
ItemType parse_item_type(std::string_view s);

}  // namespace mtp
