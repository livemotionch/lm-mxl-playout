#pragma once

// The playout state machine of one output: what is on air (the program), what comes next (cued
// and preloaded), and the transition between them. Pure logic on output frame numbers (TAI frame
// indices), so it is tested without MXL. The output asks it each grain which layers to show, at
// which media frame, and with which gains (Output::writer_main does the pixels and the audio).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mtp {

// What a clip does when it reaches its out point.
enum class EndAction {
    Next,  // the next clip follows (the playlist's next entry, or a cued clip); without one: black
    Hold,  // the last frame stays on air until the next take
    Loop,  // plays in..out again until the next take
    Stop,  // black and silence
};

enum class TransitionKind {
    Cut,
    Mix,  // cross-fade picture and sound
    Dip,  // fade to black, then up from black
};

// Cue while nothing is on air: park the clip on air at its first frame (a clip player feeding a
// vision mixer), or keep it in the background until the take (a channel going straight to air).
enum class CueMode { Park, Background };

const char* end_action_name(EndAction a);
EndAction parse_end_action(std::string_view s, EndAction fallback);
const char* transition_name(TransitionKind t);
TransitionKind parse_transition(std::string_view s, TransitionKind fallback);
const char* cue_mode_name(CueMode m);
CueMode parse_cue_mode(std::string_view s, CueMode fallback);

// One clip as it will play: a library item with its in and out points, end action and the
// transition into it.
struct ClipSpec {
    std::string item_id;
    std::string name;
    std::string type = "video";  // video, still or sprite (an animation)
    std::int64_t in = 0;         // first frame
    std::int64_t out = 0;        // one past the last frame; 0: the item's end
    std::int64_t hold_frames = 0;  // a still: how long it stays (0: until the next take)
    std::int64_t frames_hint = 0;  // the expected length (for countdowns before the item is loaded)
    EndAction end = EndAction::Stop;
    TransitionKind transition = TransitionKind::Cut;
    int transition_frames = 0;
    int entry = -1;          // index in its playlist; -1: a single clip
    std::string entry_id;    // the playlist entry's id (stable while the list is edited)
};

// The playlist a clip was cued from: after it, the next entry is cued automatically.
struct PlaylistContext {
    std::string id;
    std::string name;
    std::vector<ClipSpec> entries;
    bool loop = false;  // after the last entry the first one follows
};

// One layer the output shows at a frame: which media frame of which loaded layer.
struct LayerView {
    int layer = 0;            // 0: none (black)
    std::int64_t frame = 0;   // media frame (in the item, counted from its first frame)
    bool rolling = false;     // its sound plays (paused, parked, held and ended layers are silent)
};

// What one output frame shows: layer a times a_gain plus layer b times b_gain, over black for the
// rest (a dip or a fade). The audio gains ramp from *_gain0 at the frame's first sample to
// *_gain1 after its last, so a fade has no steps.
struct PlayoutFrame {
    LayerView a;
    LayerView b;
    double a_gain = 1, b_gain = 0;
    double a_gain0 = 1, a_gain1 = 1;
    double b_gain0 = 0, b_gain1 = 0;
    bool transition = false;
};

// A layer the output keeps media for: the program, the incoming clip of a transition, and the next.
struct LayerRef {
    int id = 0;
    ClipSpec spec;
    std::int64_t preroll = 0;  // the media frame it starts at (for the decode-ahead)
};

class Playout {
public:
    void set_cue_mode(CueMode m) { cue_mode_ = m; }
    CueMode cue_mode() const { return cue_mode_; }

    // Cues a clip, effective at frame `now`. Nothing on air (and cue mode Park): it is parked on air
    // at its in point; else it becomes the next clip (replacing one cued before). Returns the layer id.
    int cue(ClipSpec spec, std::shared_ptr<const PlaylistContext> ctx, std::int64_t now);
    // Rolls a parked or paused program, else transitions to the next clip (its own transition, or
    // the override). False when there is nothing to take.
    bool take(std::int64_t now, std::optional<TransitionKind> kind = std::nullopt, std::optional<int> frames = std::nullopt);
    bool pause(std::int64_t now);
    bool resume(std::int64_t now);
    // Black and silence: at once (cut) or as a fade over `frames`. The next clip stays cued.
    void stop(std::int64_t now, TransitionKind kind = TransitionKind::Cut, int frames = 0);
    void clear_next();
    // Moves the program to `pos` frames after its in point (it keeps playing or stays paused).
    bool seek(std::int64_t now, std::int64_t pos);
    // Cues entry `index` of the program's playlist (or of `ctx`) as the next clip.
    bool jump(int index, std::shared_ptr<const PlaylistContext> ctx, std::int64_t now);

    // The media of a layer is loaded: `media_frames` long (1 for a still). Before that a layer
    // shows black and does not start to roll.
    void set_ready(int layer, std::int64_t media_frames);

    // The frame `abs` (output frame index on the TAI timeline): runs the end actions and
    // transitions due by then, and says what to show. Calls for the same frame (the two fields of
    // an interlaced frame) answer the same.
    PlayoutFrame tick(std::int64_t abs);

    std::vector<LayerRef> layers() const;
    std::int64_t last_tick() const { return last_tick_; }
    bool idle() const { return !program_ && !incoming_ && !next_; }
    // The state, positions and countdowns at frame `abs` (`fps` output frames per second).
    nlohmann::json status(std::int64_t abs, double fps) const;

private:
    struct Layer {
        int id = 0;
        ClipSpec spec;
        std::shared_ptr<const PlaylistContext> ctx;
        bool ready = false;
        std::int64_t media_frames = 0;
        bool playing = false;   // rolling, or about to (start_at)
        bool started = false;   // the origin is set
        bool rolled = false;    // played at least once (a parked clip has not)
        bool ended = false;     // held at its last frame (end action hold)
        bool waiting = false;   // at its end, waiting for the next clip to load
        std::int64_t start_at = 0;
        std::int64_t ready_tick = -1;  // the first frame it was seen loaded
        std::int64_t origin_frame = 0;
        std::int64_t origin_pos = 0;
        std::int64_t hold_pos = 0;
    };
    struct Transition {
        bool active = false;
        TransitionKind kind = TransitionKind::Cut;
        std::int64_t start = 0;
        int frames = 0;
        bool to_black = false;
    };

    std::unique_ptr<Layer> make_layer(ClipSpec spec, std::shared_ptr<const PlaylistContext> ctx);
    static std::int64_t length(const Layer& l);  // -1: unknown or endless
    static std::int64_t pos_at(const Layer& l, std::int64_t abs);
    static std::int64_t media_frame(const Layer& l, std::int64_t pos);
    static void roll(Layer& l, std::int64_t at);
    void start_transition(std::int64_t now, TransitionKind kind, int frames);
    void finish_transition();
    void auto_cue();
    LayerView view(const Layer* l, std::int64_t abs) const;
    nlohmann::json layer_json(const Layer& l, std::int64_t abs, double fps) const;

    CueMode cue_mode_ = CueMode::Park;
    std::unique_ptr<Layer> program_;
    std::unique_ptr<Layer> incoming_;
    std::unique_ptr<Layer> next_;
    Transition tr_;
    int serial_ = 0;
    std::int64_t last_tick_ = 0;
};

}  // namespace mtp
