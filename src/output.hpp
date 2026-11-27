#pragma once

#include "config.hpp"
#include "decode.hpp"
#include "ids.hpp"
#include "media.hpp"
#include "mxl_io.hpp"
#include "playlist.hpp"
#include "playout.hpp"
#include "stream.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mtp {

struct SourceDesc {
    // pattern, video, still, playlist (the test player's sources) or playout (cue and take,
    // Output::playout): the default.
    std::string type = "playout";
    std::string pattern = "smpte_rp219";
    bool pluge = false;
    std::string audio = "sine";
    double frequency = 1000;
    double level_dbfs = -18;
    bool sync_beep = false;
    std::string item_id;
    std::string playlist_id;
    std::vector<PlaylistEntry> entries;
    bool alpha_item = false;  // the item has alpha (a still, an animation); set by set_source, not saved
};

class Output {
public:
    Output(int index, OutputConfig cfg, VideoFormat platform, MxlSession& mxl, Library& library, Uuid node, std::string font_dir,
           double ram_max_s, int preroll_frames = 25);
    ~Output();
    void start();
    void join();

    int index() const { return index_; }
    nlohmann::json status() const;
    nlohmann::json ids_json() const;
    void command(const std::string& action);
    void set_source(const SourceDesc& src);
    void set_burnin(const BurnInConfig& b);
    void patch(const nlohmann::json& body);
    void seek(std::int64_t frame);
    void set_master(const std::string& sender_id, bool enabled);
    // Runs a playout command (cue, take, ...) at the next output frame, under the output's lock,
    // and switches the output to the playout source first. `fn` gets the frame the command takes
    // effect at. Returns what `fn` returns.
    bool playout(const std::function<bool(Playout&, std::int64_t now)>& fn);
    void set_cue_mode(CueMode m);
    // The library changed (a conversion finished): media that was not ready is loaded again.
    void library_changed();
    std::vector<std::uint8_t> thumbnail() const;
    std::vector<float> meters() const;
    OutputIds ids() const;
    // flow_def.json of every flow this output writes (also its IS-04 flows).
    std::vector<std::string> flow_definitions() const;
    VideoFormat format() const;
    std::uint64_t grains() const { return grains_.load(); }
    std::uint64_t underruns() const { return underruns_.load(); }
    std::uint64_t loops_done() const { return loops_.load(); }
    int ahead() const { return ahead_.load(); }
    std::uint64_t stream_missed() const { return stream_missed_.load(); }
    std::uint64_t ram_bytes() const { return ram_bytes_.load(); }

    std::function<void()> on_change;

private:
    struct Media {
        bool ready = false;
        bool ram = false;
        bool still = false;
        std::string item_id;
        std::string format;
        RamClip clip;
        std::string mezz;  // a video that streams from its mezzanine (not in RAM): its path
        std::shared_ptr<void> ram_hold;  // the clip's share of the process-wide RAM budget
        std::vector<std::uint8_t> still_fill;
        std::vector<std::uint8_t> still_key;
        std::vector<std::uint8_t> still_a10;
        bool premultiplied = false;  // the still's fill is premultiplied
        // An animation (GIF, APNG) played as a clip, from RAM: `frames` output frames per loop.
        bool anim = false;
        std::shared_ptr<const Library::SharedBlob> anim_fill;   // one v210 frame per animation frame (shared by outputs)
        std::shared_ptr<const Library::SharedBlob> anim_alpha;  // per frame: the v210a alpha plane, or the fill_key key; null without a key
        std::vector<double> anim_durations;
        std::string error;  // why the item cannot play (status `media_error`)
        // A pattern that does not move, rendered by the loader (never by the real-time writer):
        // [0] the frame, [1] its A/V-sync flash frame (av_sync and the sync beep only).
        std::string pattern;
        bool pattern_pluge = false;
        std::vector<std::uint8_t> pattern_v210[2];
        std::int64_t frames = 1;
        int channels = 0;
        std::string name;
        std::string tc_start = "00:00:00:00";
    };

    void writer_main();
    void loader_main();
    bool load_layers(const VideoFormat& fmt, std::vector<std::shared_ptr<const Media>>& graveyard);
    // The playout's layers (program, incoming, next) and their media; mu_ held.
    void sync_layers();
    std::int64_t frame_now() const;  // the output frame on the TAI timeline now
    // The frame a command takes effect at: the next one the writer writes (mu_ held).
    std::int64_t command_frame() const;
    struct FlowDefs {
        std::string video, audio, data, key;  // empty when the flow is off
    };
    static FlowDefs flow_defs(const OutputIds& ids, const OutputConfig& cfg, const VideoFormat& fmt);
    void reopen(const OutputConfig& cfg, const VideoFormat& fmt);
    AudioProgram program_for(const SourceDesc& src, int channels) const;
    Media load_media(const SourceDesc& src, const VideoFormat& fmt) const;
    Media load_animation(const LibraryItem& item, const std::filesystem::path& dir, const VideoFormat& fmt) const;

    int index_;
    MxlSession& mxl_;
    Library& library_;
    Uuid node_;
    std::string font_dir_;
    VideoFormat platform_;
    double ram_max_s_ = 20;
    int preroll_frames_ = 25;

    mutable std::mutex mu_;
    OutputConfig cfg_;
    SourceDesc source_;
    VideoFormat fmt_;
    OutputIds ids_;
    std::string transport_ = "play";
    bool loop_ = true;
    std::int64_t origin_frame_index_ = 0;
    std::int64_t origin_media_ = 0;
    std::int64_t hold_frame_ = 0;
    std::uint64_t play_origin_grain_ = 0;
    bool origin_valid_ = false;
    int config_gen_ = 0;
    int applied_gen_ = -1;
    int media_gen_ = 0;
    SourceDesc load_src_;
    bool load_override_ = false;
    std::atomic<std::shared_ptr<const Media>> published_;
    // Each library item's gain (linear, only those not 0 dB), applied to its sound at playout:
    // set from the library (refresh_gains), so a level change is heard at once.
    using Gains = std::unordered_map<std::string, float>;
    std::atomic<std::shared_ptr<const Gains>> gains_{std::make_shared<const Gains>()};
    void refresh_gains();

    std::atomic<bool> stop_{false};
    std::atomic<bool> master_v_{true};
    std::atomic<bool> master_a_{true};
    std::atomic<bool> master_d_{true};
    std::atomic<bool> master_k_{true};
    std::atomic<std::uint64_t> grains_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> loops_{0};
    std::atomic<int> ahead_{0};
    std::atomic<std::uint64_t> stream_missed_{0};  // grains of a streamed video whose frame was not decoded in time
    std::atomic<std::uint64_t> ram_bytes_{0};
    // Written by the writer each grain: the playhead (what pause holds), and the frame within the
    // current item and the item's length (for the status).
    std::atomic<std::int64_t> playhead_{0};
    std::atomic<std::int64_t> position_{0};
    std::atomic<std::int64_t> length_{0};
    std::thread thread_;
    std::thread loader_;
    // Two decode-ahead streams: the clip on air and the next (or the two clips of a transition).
    // The test player's sources use the first.
    std::unique_ptr<StreamDecoder> decoders_[2];

    // Playout (mu_): the state machine and the media of its layers by layer id.
    Playout playout_;
    struct LayerMedia {
        ClipSpec spec;
        std::int64_t preroll = 0;
        std::shared_ptr<const Media> media;
        bool load = true;    // (re)load it: new, or its item was not ready
        int decoder = -1;    // the stream it decodes on (a streamed video)
        int order = 0;       // 0 program, then incoming, then next: the loader loads in this order
        // The playout was told it is ready: when loaded, or for a streamed video once its stream
        // holds its first frame (so a take never cuts to a frame that is not decoded).
        bool signalled = false;
    };
    std::map<int, LayerMedia> layers_;
    std::vector<std::shared_ptr<const Media>> layer_graveyard_;  // freed by the loader

    MxlFlow video_, audio_, data_, key_;
    bool flows_open_ = false;

    struct Sprite {
        std::string id;
        int w = 0, h = 0;
        std::vector<std::vector<std::uint8_t>> frames;
        std::vector<double> durations;
    };
    Sprite sprites_[2];

    // The writer publishes a rendered frame every few grains; thumbnail() encodes
    // the JPEG from it when asked, so the writer thread never encodes.
    struct Frame {
        std::vector<std::uint8_t> v210;
        int width = 0;
        int height = 0;
        std::uint64_t index = 0;
    };
    std::atomic<std::shared_ptr<const Frame>> thumb_frame_;
    mutable std::atomic<bool> thumb_wanted_{false};  // grains rendered in place are copied out only on request
    mutable std::mutex thumb_mu_;
    mutable std::vector<std::uint8_t> thumb_;
    mutable std::uint64_t thumb_index_ = 0;
    mutable std::mutex meter_mu_;
    std::vector<float> meters_;
};

SourceDesc source_from_json(const nlohmann::json& j);
nlohmann::json source_to_json(const SourceDesc& s);

}  // namespace mtp
