#include "playout.hpp"

#include <algorithm>
#include <cmath>

namespace mtp {

const char* end_action_name(EndAction a) {
    switch (a) {
        case EndAction::Next: return "next";
        case EndAction::Hold: return "hold";
        case EndAction::Loop: return "loop";
        case EndAction::Stop: return "stop";
    }
    return "stop";
}

EndAction parse_end_action(std::string_view s, EndAction fallback) {
    if (s == "next") return EndAction::Next;
    if (s == "hold" || s == "freeze") return EndAction::Hold;
    if (s == "loop") return EndAction::Loop;
    if (s == "stop" || s == "black") return EndAction::Stop;
    return fallback;
}

const char* transition_name(TransitionKind t) {
    switch (t) {
        case TransitionKind::Cut: return "cut";
        case TransitionKind::Mix: return "mix";
        case TransitionKind::Dip: return "dip";
    }
    return "cut";
}

TransitionKind parse_transition(std::string_view s, TransitionKind fallback) {
    if (s == "cut") return TransitionKind::Cut;
    if (s == "mix" || s == "dissolve") return TransitionKind::Mix;
    if (s == "dip" || s == "dip_black") return TransitionKind::Dip;
    return fallback;
}

const char* cue_mode_name(CueMode m) { return m == CueMode::Park ? "park" : "background"; }

CueMode parse_cue_mode(std::string_view s, CueMode fallback) {
    if (s == "park") return CueMode::Park;
    if (s == "background") return CueMode::Background;
    return fallback;
}

std::unique_ptr<Playout::Layer> Playout::make_layer(ClipSpec spec, std::shared_ptr<const PlaylistContext> ctx) {
    auto l = std::make_unique<Layer>();
    l->id = ++serial_;
    if (spec.in < 0) spec.in = 0;
    if (spec.out < 0) spec.out = 0;
    if (spec.transition_frames < 0) spec.transition_frames = 0;
    l->spec = std::move(spec);
    l->ctx = std::move(ctx);
    return l;
}

std::int64_t Playout::length(const Layer& l) {
    if (l.spec.type == "still") return l.spec.hold_frames > 0 ? l.spec.hold_frames : -1;
    if (!l.ready || l.media_frames <= 0) return -1;
    const std::int64_t end = l.spec.out > 0 ? std::min(l.spec.out, l.media_frames) : l.media_frames;
    return std::max<std::int64_t>(1, end - std::min(l.spec.in, l.media_frames - 1));
}

std::int64_t Playout::pos_at(const Layer& l, std::int64_t abs) {
    if (l.playing && l.started) return l.origin_pos + std::max<std::int64_t>(0, abs - l.origin_frame);
    return l.hold_pos;
}

std::int64_t Playout::media_frame(const Layer& l, std::int64_t pos) {
    if (l.spec.type == "still") return 0;
    const std::int64_t in = l.ready && l.media_frames > 0 ? std::min(l.spec.in, l.media_frames - 1) : l.spec.in;
    const std::int64_t len = length(l);
    pos = std::max<std::int64_t>(0, pos);
    if (len <= 0) return in + pos;
    if (l.spec.end == EndAction::Loop) return in + pos % len;
    return in + std::min(pos, len - 1);
}

void Playout::roll(Layer& l, std::int64_t at) {
    l.playing = true;
    l.started = false;
    l.rolled = true;
    l.ended = false;
    l.waiting = false;
    l.start_at = at;
    l.origin_pos = l.hold_pos;
}

int Playout::cue(ClipSpec spec, std::shared_ptr<const PlaylistContext> ctx, std::int64_t now) {
    (void)now;
    auto l = make_layer(std::move(spec), std::move(ctx));
    const int id = l->id;
    if (!program_ && !incoming_ && cue_mode_ == CueMode::Park && !auto_take_) {
        program_ = std::move(l);
        next_.reset();
        auto_cue();
    } else if (program_ && !program_->rolled && !tr_.active) {
        // A parked clip is replaced by the one cued after it (a shotbox click on another clip).
        program_ = std::move(l);
        next_.reset();
        auto_cue();
    } else {
        next_ = std::move(l);
    }
    return id;
}

void Playout::start_transition(std::int64_t now, TransitionKind kind, int frames) {
    if (!next_) return;
    if (tr_.active) finish_transition();
    if (kind == TransitionKind::Cut || frames <= 0) {
        program_ = std::move(next_);
        roll(*program_, now);
        auto_cue();
        return;
    }
    incoming_ = std::move(next_);
    // A dip shows black at its middle: the incoming clip starts there, not under the fade-out.
    roll(*incoming_, now + (kind == TransitionKind::Dip ? frames / 2 : 0));
    tr_ = Transition{true, kind, now, frames, false};
}

void Playout::finish_transition() {
    if (!tr_.active) return;
    if (tr_.to_black) {
        program_.reset();
    } else if (incoming_) {
        program_ = std::move(incoming_);
        // A dip finished early (a take during it) starts the incoming clip now.
        if (program_->playing && !program_->started && program_->start_at > last_tick_) program_->start_at = last_tick_;
    }
    incoming_.reset();
    tr_ = Transition{};
    auto_cue();
}

void Playout::auto_cue() {
    if (next_ || !program_ || !program_->ctx || program_->spec.entry < 0) return;
    const auto& entries = program_->ctx->entries;
    if (entries.empty()) return;
    std::size_t i = static_cast<std::size_t>(program_->spec.entry) + 1;
    if (i >= entries.size()) {
        if (!program_->ctx->loop) return;
        i = 0;
    }
    next_ = make_layer(entries[i], program_->ctx);
}

bool Playout::take(std::int64_t now, std::optional<TransitionKind> kind, std::optional<int> frames) {
    if (tr_.active) finish_transition();
    if (program_ && !program_->rolled) {
        roll(*program_, now);
        return true;
    }
    if (next_) {
        const TransitionKind k = kind.value_or(next_->spec.transition);
        const int f = frames.value_or(next_->spec.transition_frames);
        start_transition(now, k, f);
        return true;
    }
    if (program_ && !program_->playing && !program_->ended) {
        roll(*program_, now);
        return true;
    }
    return false;
}

bool Playout::pause(std::int64_t now) {
    if (tr_.active) finish_transition();
    if (!program_ || !program_->playing) return false;
    const std::int64_t len = length(*program_);
    std::int64_t pos = pos_at(*program_, now);
    if (len > 0) pos = program_->spec.end == EndAction::Loop ? pos % len : std::min(pos, len - 1);
    program_->hold_pos = pos;
    program_->playing = false;
    program_->started = false;
    program_->waiting = false;
    return true;
}

bool Playout::resume(std::int64_t now) {
    if (tr_.active) finish_transition();
    if (!program_ || program_->playing || program_->ended) return false;
    roll(*program_, now);
    return true;
}

void Playout::stop(std::int64_t now, TransitionKind kind, int frames) {
    if (tr_.active) finish_transition();
    if (!program_) return;
    if (kind == TransitionKind::Cut || frames <= 0) {
        program_.reset();
        return;
    }
    tr_ = Transition{true, kind, now, frames, true};
}

void Playout::clear_next() { next_.reset(); }

bool Playout::seek(std::int64_t now, std::int64_t pos) {
    if (tr_.active) finish_transition();
    if (!program_) return false;
    const std::int64_t len = length(*program_);
    pos = std::max<std::int64_t>(0, pos);
    if (len > 0) pos = std::min(pos, len - 1);
    program_->ended = false;
    program_->waiting = false;
    program_->hold_pos = pos;
    if (program_->playing) {
        program_->origin_pos = pos;
        if (program_->started) program_->origin_frame = now;
    }
    return true;
}

bool Playout::jump(int index, std::shared_ptr<const PlaylistContext> ctx, std::int64_t now) {
    if (!ctx && program_) ctx = program_->ctx;
    if (!ctx && next_) ctx = next_->ctx;
    if (!ctx || index < 0 || index >= static_cast<int>(ctx->entries.size())) return false;
    cue(ctx->entries[static_cast<std::size_t>(index)], ctx, now);
    return true;
}

void Playout::set_ready(int layer, std::int64_t media_frames) {
    for (auto* l : {program_.get(), incoming_.get(), next_.get()}) {
        if (l && l->id == layer) {
            l->ready = true;
            l->media_frames = std::max<std::int64_t>(1, media_frames);
        }
    }
}

PlayoutFrame Playout::tick(std::int64_t abs) {
    last_tick_ = abs;
    if (tr_.active && abs >= tr_.start + tr_.frames) finish_transition();
    // The program's end: an auto-advance (a transition starts that many frames before the end, so
    // the outgoing clip ends with it), or the end action once it is past its last frame.
    if (program_ && program_->playing && program_->started && !tr_.active) {
        const std::int64_t len = length(*program_);
        if (len > 0) {
            const std::int64_t pos = pos_at(*program_, abs);
            const EndAction e = program_->spec.end;
            // With auto take a clip that holds or stops follows on like one that plays the next.
            const bool follows = e == EndAction::Next || (auto_take_ && next_ && e != EndAction::Loop);
            bool advanced = false;
            if (follows && next_ && next_->ready) {
                const TransitionKind k = next_->spec.transition;
                const int f = k == TransitionKind::Cut ? 0 : next_->spec.transition_frames;
                if (pos >= len - f) {
                    start_transition(abs, k, f);
                    advanced = true;
                }
            }
            if (!advanced && pos >= len && follows && next_) {
                program_->waiting = true;  // its last frame stays until the next clip is loaded
            } else if (!advanced && pos >= len) {
                switch (e) {
                    case EndAction::Loop: break;
                    case EndAction::Hold:
                        program_->hold_pos = len - 1;
                        program_->playing = false;
                        program_->started = false;
                        program_->ended = true;
                        break;
                    case EndAction::Stop: program_.reset(); break;
                    case EndAction::Next:
                        if (next_) program_->waiting = true;  // its last frame stays until the next clip is loaded
                        else program_.reset();
                        break;
                }
            }
        }
    }
    // Auto take after the program is done: held at its end, or black (stopped, or nothing cued
    // before); a parked or paused clip is not done.
    if (auto_take_ && !tr_.active && next_ && next_->ready && (!program_ || program_->ended)) {
        const TransitionKind k = next_->spec.transition;
        start_transition(abs, k, k == TransitionKind::Cut ? 0 : next_->spec.transition_frames);
    }
    for (auto* l : {program_.get(), incoming_.get(), next_.get()})
        if (l && l->ready && l->ready_tick < 0) l->ready_tick = abs;
    // A clip loaded by its start frame starts exactly there (also when this tick comes late);
    // one that loads later starts when it is loaded.
    for (auto* l : {program_.get(), incoming_.get()}) {
        if (l && l->playing && !l->started && l->ready && abs >= l->start_at) {
            l->started = true;
            l->origin_frame = l->ready_tick <= l->start_at ? l->start_at : abs;
        }
    }
    PlayoutFrame f;
    f.a = view(program_.get(), abs);
    if (tr_.active) {
        f.transition = true;
        const double n = std::max(1, tr_.frames);
        const double k = static_cast<double>(abs - tr_.start);
        auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
        const double t = clamp01((k + 0.5) / n), t0 = clamp01(k / n), t1 = clamp01((k + 1) / n);
        auto out_gain = [&](double x) { return tr_.kind == TransitionKind::Dip && !tr_.to_black ? std::max(0.0, 1 - 2 * x) : 1 - x; };
        auto in_gain = [&](double x) { return tr_.kind == TransitionKind::Dip ? std::max(0.0, 2 * x - 1) : x; };
        f.a_gain = out_gain(t);
        f.a_gain0 = out_gain(t0);
        f.a_gain1 = out_gain(t1);
        if (!tr_.to_black) {
            f.b = view(incoming_.get(), abs);
            f.b_gain = in_gain(t);
            f.b_gain0 = in_gain(t0);
            f.b_gain1 = in_gain(t1);
        }
    }
    if (!f.a.layer) f.a_gain = f.a_gain0 = f.a_gain1 = 0;
    if (!f.b.layer) f.b_gain = f.b_gain0 = f.b_gain1 = 0;
    return f;
}

LayerView Playout::view(const Layer* l, std::int64_t abs) const {
    LayerView v;
    if (!l) return v;
    v.layer = l->id;
    const std::int64_t pos = pos_at(*l, abs);
    v.frame = media_frame(*l, pos);
    const std::int64_t len = length(*l);
    const bool past_end = len > 0 && pos >= len && l->spec.end != EndAction::Loop;
    v.rolling = l->ready && l->playing && l->started && !l->waiting && !past_end;
    return v;
}

std::vector<LayerRef> Playout::layers() const {
    std::vector<LayerRef> out;
    for (const auto* l : {program_.get(), incoming_.get(), next_.get()}) {
        if (!l) continue;
        out.push_back(LayerRef{l->id, l->spec, media_frame(*l, pos_at(*l, last_tick_))});
    }
    return out;
}

nlohmann::json Playout::layer_json(const Layer& l, std::int64_t abs, double fps) const {
    const std::int64_t len = length(l);
    const std::int64_t known = len > 0 ? len : (l.spec.type == "still" ? -1 : std::max<std::int64_t>(0, (l.spec.out > 0 ? l.spec.out : l.spec.frames_hint) - l.spec.in));
    std::int64_t pos = pos_at(l, abs);
    if (len > 0) pos = l.spec.end == EndAction::Loop ? pos % len : std::min(pos, len);
    nlohmann::json j{{"layer", l.id},
                     {"item_id", l.spec.item_id},
                     {"name", l.spec.name},
                     {"type", l.spec.type},
                     {"entry", l.spec.entry},
                     {"entry_id", l.spec.entry_id},
                     {"in", l.spec.in},
                     {"out", l.spec.out},
                     {"end", end_action_name(l.spec.end)},
                     {"transition", transition_name(l.spec.transition)},
                     {"transition_frames", l.spec.transition_frames},
                     {"ready", l.ready},
                     {"playing", l.playing && l.started},
                     {"ended", l.ended},
                     {"waiting", l.waiting},
                     {"position", pos},
                     {"frames", known > 0 ? known : 0},
                     {"endless", known < 0}};
    // An ended (held) clip has no time left; else the frames after the one on air.
    j["remaining_s"] = known > 0 && fps > 0 && !l.ended ? static_cast<double>(std::max<std::int64_t>(0, known - pos)) / fps : 0.0;
    if (l.ctx) {
        j["playlist_id"] = l.ctx->id;
        j["playlist_name"] = l.ctx->name;
        j["playlist_count"] = l.ctx->entries.size();
    } else {
        j["playlist_id"] = "";
    }
    return j;
}

nlohmann::json Playout::status(std::int64_t abs, double fps) const {
    std::string state = "idle";
    if (tr_.active) state = tr_.to_black ? "fading" : "transition";
    else if (program_) {
        if (!program_->rolled) state = "parked";
        else if (program_->ended) state = "ended";
        else if (program_->waiting) state = "waiting";
        else if (program_->playing) state = "playing";
        else state = "paused";
    }
    nlohmann::json j{{"state", state}, {"cue_mode", cue_mode_name(cue_mode_)}, {"auto_take", auto_take_}};
    j["program"] = program_ ? layer_json(*program_, abs, fps) : nlohmann::json();
    j["incoming"] = incoming_ ? layer_json(*incoming_, abs, fps) : nlohmann::json();
    j["next"] = next_ ? layer_json(*next_, abs, fps) : nlohmann::json();
    if (tr_.active) {
        const double n = std::max(1, tr_.frames);
        j["transition"] = nlohmann::json{{"kind", transition_name(tr_.kind)},
                                         {"frames", tr_.frames},
                                         {"to_black", tr_.to_black},
                                         {"progress", std::clamp(static_cast<double>(abs - tr_.start) / n, 0.0, 1.0)}};
    } else {
        j["transition"] = nlohmann::json();
    }
    // The playlist on air: its entry and the time left to its end (endless when it loops or an
    // entry holds or loops).
    const Layer* cur = incoming_ ? incoming_.get() : program_.get();
    if (cur && cur->ctx && cur->spec.entry >= 0) {
        const auto& ctx = *cur->ctx;
        double remain = 0;
        bool endless = ctx.loop;
        const auto cj = layer_json(*cur, abs, fps);
        remain += cj.value("remaining_s", 0.0);
        // A clip that loops (or a still without a hold time) has no end; one that holds ends the run.
        if (cj.value("endless", false) || cur->spec.end == EndAction::Loop) endless = true;
        // Only entries that follow by themselves count: an entry that stops, holds or loops ends the run.
        for (std::size_t i = static_cast<std::size_t>(cur->spec.entry) + 1; cur->spec.end == EndAction::Next && i < ctx.entries.size() && !endless; ++i) {
            const auto& e = ctx.entries[i];
            const std::int64_t frames = e.type == "still" ? e.hold_frames : std::max<std::int64_t>(0, (e.out > 0 ? e.out : e.frames_hint) - e.in);
            if (frames <= 0 || e.end == EndAction::Loop) endless = true;
            if (fps > 0) remain += static_cast<double>(std::max<std::int64_t>(0, frames)) / fps;
            if (e.end != EndAction::Next) break;
        }
        j["playlist"] = nlohmann::json{{"id", ctx.id},
                                       {"name", ctx.name},
                                       {"entry", cur->spec.entry},
                                       {"count", ctx.entries.size()},
                                       {"loop", ctx.loop},
                                       {"remaining_s", remain},
                                       {"endless", endless}};
    } else {
        j["playlist"] = nlohmann::json();
    }
    return j;
}

}  // namespace mtp
