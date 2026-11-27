// Playout state machine (src/playout.cpp) and the transition blends (mix_v210, mix_a10).
#include "playout.hpp"
#include "v210.hpp"

#include <doctest.h>

#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace mtp;

namespace {

ClipSpec clip(const std::string& id, EndAction end = EndAction::Stop, TransitionKind tr = TransitionKind::Cut, int tr_frames = 0) {
    ClipSpec c;
    c.item_id = id;
    c.name = id;
    c.end = end;
    c.transition = tr;
    c.transition_frames = tr_frames;
    return c;
}

// Marks every layer ready with the length its item has in `lengths` (10 frames by default).
void ready_all(Playout& p, const std::map<std::string, std::int64_t>& lengths = {}) {
    for (const auto& l : p.layers()) {
        const auto it = lengths.find(l.spec.item_id);
        p.set_ready(l.id, it == lengths.end() ? 10 : it->second);
    }
}

std::shared_ptr<PlaylistContext> playlist(std::vector<ClipSpec> entries, bool loop = false) {
    auto ctx = std::make_shared<PlaylistContext>();
    ctx->id = "pl";
    ctx->name = "Playlist";
    ctx->loop = loop;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].entry = static_cast<int>(i);
        ctx->entries.push_back(entries[i]);
    }
    return ctx;
}

std::string state(const Playout& p, std::int64_t abs) { return p.status(abs, 50).value("state", ""); }

// The item on air: the name of the layer a frame shows (empty: black).
std::string on_air(const Playout& p, const PlayoutFrame& f) {
    const auto st = p.status(0, 50);
    (void)f;
    return st["program"].is_object() ? st["program"].value("item_id", "") : "";
}

}  // namespace

TEST_CASE("playout: a clip cued while nothing is on air is parked at its in point, the take rolls it") {
    Playout p;
    auto c = clip("a");
    c.in = 5;
    p.cue(c, nullptr, 100);
    ready_all(p);
    auto f = p.tick(100);
    CHECK(f.a.layer != 0);
    CHECK(f.a.frame == 5);
    CHECK_FALSE(f.a.rolling);
    CHECK(state(p, 100) == "parked");
    f = p.tick(101);
    CHECK(f.a.frame == 5);  // parked: it does not move
    REQUIRE(p.take(102));
    CHECK(p.tick(102).a.frame == 5);
    CHECK(p.tick(102).a.rolling);
    CHECK(p.tick(103).a.frame == 6);
    CHECK(p.tick(104).a.frame == 7);
    CHECK(state(p, 104) == "playing");
}

TEST_CASE("playout: background cue mode keeps air black until the take") {
    Playout p;
    p.set_cue_mode(CueMode::Background);
    p.cue(clip("a"), nullptr, 0);
    ready_all(p);
    auto f = p.tick(0);
    CHECK(f.a.layer == 0);
    CHECK(f.a_gain == 0);
    CHECK(state(p, 0) == "idle");
    CHECK(p.status(0, 50)["next"].value("item_id", "") == "a");
    REQUIRE(p.take(1));
    f = p.tick(1);
    CHECK(f.a.layer != 0);
    CHECK(f.a.frame == 0);
    CHECK(p.tick(2).a.frame == 1);
}

TEST_CASE("playout: a clip taken before it is loaded starts when its media is ready") {
    Playout p;
    p.set_cue_mode(CueMode::Background);
    const int id = p.cue(clip("a"), nullptr, 0);
    p.take(1);
    for (int t = 1; t < 5; ++t) {
        const auto f = p.tick(t);
        CHECK(f.a.frame == 0);
        CHECK_FALSE(f.a.rolling);
    }
    p.set_ready(id, 10);
    CHECK(p.tick(5).a.frame == 0);
    CHECK(p.tick(5).a.rolling);
    CHECK(p.tick(6).a.frame == 1);
}

TEST_CASE("playout: end actions stop, hold and loop") {
    SUBCASE("stop goes to black after the last frame") {
        Playout p;
        p.cue(clip("a", EndAction::Stop), nullptr, 0);
        ready_all(p, {{"a", 4}});
        p.take(10);
        std::vector<std::int64_t> frames;
        for (int t = 10; t < 14; ++t) frames.push_back(p.tick(t).a.frame);
        CHECK(frames == std::vector<std::int64_t>{0, 1, 2, 3});
        CHECK(p.tick(14).a.layer == 0);
        CHECK(state(p, 14) == "idle");
    }
    SUBCASE("hold keeps the last frame, silent, until the next take") {
        Playout p;
        p.cue(clip("a", EndAction::Hold), nullptr, 0);
        ready_all(p, {{"a", 4}});
        p.take(10);
        for (int t = 10; t < 14; ++t) p.tick(t);
        auto f = p.tick(14);
        CHECK(f.a.layer != 0);
        CHECK(f.a.frame == 3);
        CHECK_FALSE(f.a.rolling);
        CHECK(p.tick(30).a.frame == 3);
        CHECK(state(p, 30) == "ended");
    }
    SUBCASE("loop plays in..out again") {
        Playout p;
        auto c = clip("a", EndAction::Loop);
        c.in = 2;
        c.out = 5;
        p.cue(c, nullptr, 0);
        ready_all(p, {{"a", 10}});
        p.take(0);
        std::vector<std::int64_t> frames;
        for (int t = 0; t < 7; ++t) frames.push_back(p.tick(t).a.frame);
        CHECK(frames == std::vector<std::int64_t>{2, 3, 4, 2, 3, 4, 2});
    }
    SUBCASE("next without a next clip goes to black") {
        Playout p;
        p.cue(clip("a", EndAction::Next), nullptr, 0);
        ready_all(p, {{"a", 3}});
        p.take(0);
        for (int t = 0; t < 3; ++t) CHECK(p.tick(t).a.layer != 0);
        CHECK(p.tick(3).a.layer == 0);
    }
}

TEST_CASE("playout: a playlist with cuts plays frame-accurately, the next entry cued by itself") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Next), clip("c", EndAction::Next)});
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p, {{"a", 5}, {"b", 3}, {"c", 2}});
    CHECK(p.status(0, 50)["next"].value("item_id", "") == "b");  // preloaded while a is parked
    p.take(100);
    std::vector<std::pair<std::string, std::int64_t>> seen;
    for (int t = 100; t < 112; ++t) {
        const auto f = p.tick(t);
        ready_all(p, {{"a", 5}, {"b", 3}, {"c", 2}});
        seen.emplace_back(on_air(p, f), f.a.layer ? f.a.frame : -1);
    }
    const std::vector<std::pair<std::string, std::int64_t>> want = {
        {"a", 0}, {"a", 1}, {"a", 2}, {"a", 3}, {"a", 4}, {"b", 0}, {"b", 1}, {"b", 2}, {"c", 0}, {"c", 1}, {"", -1}, {"", -1}};
    CHECK(seen == want);
}

TEST_CASE("playout: a mix into the next entry starts before the end, so the outgoing clip plays to its last frame") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Stop, TransitionKind::Mix, 4)});
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p, {{"a", 10}, {"b", 10}});
    p.take(0);
    for (int t = 0; t < 6; ++t) CHECK_FALSE(p.tick(t).transition);
    double last_b = 0;
    for (int t = 6; t < 10; ++t) {
        const auto f = p.tick(t);
        REQUIRE(f.transition);
        CHECK(f.a.frame == t);       // a plays on: 6, 7, 8, 9
        CHECK(f.b.frame == t - 6);   // b from its first frame
        CHECK(f.b.rolling);
        CHECK(f.a_gain + f.b_gain == doctest::Approx(1.0));
        CHECK(f.b_gain > last_b);
        CHECK(f.a_gain1 == doctest::Approx(f.a_gain0 - 0.25));
        last_b = f.b_gain;
    }
    CHECK(state(p, 9) == "transition");
    const auto f = p.tick(10);
    CHECK_FALSE(f.transition);
    CHECK(f.a.frame == 4);  // b continues where the mix left it
    CHECK(on_air(p, f) == "b");
}

TEST_CASE("playout: a dip fades the program to black and the incoming clip up from black, starting at the middle") {
    Playout p;
    p.cue(clip("a", EndAction::Loop), nullptr, 0);
    ready_all(p);
    p.take(0);
    p.tick(0);
    p.cue(clip("b"), nullptr, 1);
    ready_all(p);
    REQUIRE(p.take(10, TransitionKind::Dip, 6));
    std::vector<double> ga, gb;
    std::vector<std::int64_t> bf;
    for (int t = 10; t < 16; ++t) {
        const auto f = p.tick(t);
        ga.push_back(f.a_gain);
        gb.push_back(f.b_gain);
        bf.push_back(f.b.frame);
    }
    CHECK(ga[0] > ga[1]);
    CHECK(ga[2] > 0);
    CHECK(ga[3] == 0);
    CHECK(gb[2] == 0);
    CHECK(gb[3] > 0);
    CHECK(gb[5] > gb[4]);
    CHECK(bf == std::vector<std::int64_t>{0, 0, 0, 0, 1, 2});  // b rolls from the dip's middle
    CHECK(p.tick(16).a.frame == 3);
}

TEST_CASE("playout: stop cuts or fades to black and keeps the next clip cued") {
    Playout p;
    p.cue(clip("a", EndAction::Loop), nullptr, 0);
    ready_all(p);
    p.take(0);
    p.tick(0);
    p.cue(clip("b"), nullptr, 0);
    p.stop(5, TransitionKind::Mix, 4);
    double prev = 1;
    for (int t = 5; t < 9; ++t) {
        const auto f = p.tick(t);
        CHECK(f.transition);
        CHECK(f.b.layer == 0);
        CHECK(f.a_gain < prev);
        prev = f.a_gain;
    }
    CHECK(state(p, 8) == "fading");
    CHECK(p.tick(9).a.layer == 0);
    CHECK(state(p, 9) == "idle");
    CHECK(p.status(9, 50)["next"].value("item_id", "") == "b");
    p.take(10);
    ready_all(p);
    CHECK(p.tick(10).a.layer != 0);
}

TEST_CASE("playout: pause holds the frame, resume continues from it, seek moves the program") {
    Playout p;
    p.cue(clip("a", EndAction::Hold), nullptr, 0);
    ready_all(p, {{"a", 100}});
    p.take(0);
    for (int t = 0; t < 5; ++t) p.tick(t);  // frames 0..4
    REQUIRE(p.pause(5));
    CHECK(p.tick(5).a.frame == 5);
    CHECK(p.tick(9).a.frame == 5);
    CHECK_FALSE(p.tick(9).a.rolling);
    CHECK(state(p, 9) == "paused");
    REQUIRE(p.resume(10));
    CHECK(p.tick(10).a.frame == 5);
    CHECK(p.tick(11).a.frame == 6);
    REQUIRE(p.seek(12, 50));
    CHECK(p.tick(12).a.frame == 50);
    CHECK(p.tick(13).a.frame == 51);
    // take on a paused clip with nothing cued rolls it again
    p.pause(14);
    CHECK(p.take(15));
    CHECK(p.tick(16).a.frame == 53);
}

TEST_CASE("playout: a still holds for its time, then the next entry follows; a looping playlist starts over") {
    Playout p;
    auto s = clip("s", EndAction::Next);
    s.type = "still";
    s.hold_frames = 3;
    auto ctx = playlist({s, clip("v", EndAction::Next)}, true);
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p, {{"s", 1}, {"v", 2}});
    p.take(0);
    std::vector<std::string> seen;
    for (int t = 0; t < 10; ++t) {
        const auto f = p.tick(t);
        ready_all(p, {{"s", 1}, {"v", 2}});
        seen.push_back(on_air(p, f));
    }
    CHECK(seen == std::vector<std::string>{"s", "s", "s", "v", "v", "s", "s", "s", "v", "v"});
    const auto st = p.status(9, 50);
    CHECK(st["playlist"].value("endless", false));
}

TEST_CASE("playout: the next clip waits at the end of the program until it is loaded") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Stop)});
    p.cue(ctx->entries[0], ctx, 0);
    // only a is loaded
    for (const auto& l : p.layers())
        if (l.spec.item_id == "a") p.set_ready(l.id, 3);
    p.take(0);
    for (int t = 0; t < 3; ++t) p.tick(t);
    auto f = p.tick(3);
    CHECK(f.a.frame == 2);  // the last frame stays
    CHECK_FALSE(f.a.rolling);
    CHECK(state(p, 3) == "waiting");
    ready_all(p, {{"a", 3}, {"b", 5}});
    f = p.tick(4);
    CHECK(on_air(p, f) == "b");
    CHECK(f.a.frame == 0);
}

TEST_CASE("playout: jump cues another entry of the playlist on air; cue replaces a parked clip") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Next), clip("c", EndAction::Next)});
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p);
    p.take(0);
    p.tick(0);
    CHECK(p.status(0, 50)["next"].value("item_id", "") == "b");
    REQUIRE(p.jump(2, nullptr, 1));
    CHECK(p.status(1, 50)["next"].value("item_id", "") == "c");
    CHECK_FALSE(p.jump(7, nullptr, 1));

    Playout q;
    q.cue(clip("x"), nullptr, 0);
    q.cue(clip("y"), nullptr, 0);
    CHECK(q.status(0, 50)["program"].value("item_id", "") == "y");
    CHECK(q.status(0, 50)["next"].is_null());
}

TEST_CASE("playout: status gives countdowns for the clip and the playlist") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Stop)});
    ctx->entries[1].frames_hint = 100;
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p, {{"a", 50}, {"b", 100}});
    p.take(0);
    p.tick(0);
    const auto st = p.status(25, 50);
    CHECK(st["program"].value("remaining_s", 0.0) == doctest::Approx(0.5));
    CHECK(st["playlist"].value("remaining_s", 0.0) == doctest::Approx(2.5));
    CHECK_FALSE(st["playlist"].value("endless", true));
    CHECK(st["playlist"].value("entry", -1) == 0);
    CHECK(st["playlist"].value("count", 0) == 2);
}

TEST_CASE("mix_v210 and mix_a10 blend 10-bit samples towards each other and towards black") {
    const int w = 48, h = 1;
    const auto bytes = v210_size(w, h);
    std::vector<std::uint8_t> a(bytes), b(bytes), out(bytes);
    fill_v210(a.data(), w, h, 100, 300, 700);
    fill_v210(b.data(), w, h, 500, 700, 300);
    std::vector<std::uint16_t> y(w), cb(w / 2), cr(w / 2);

    mix_v210(out.data(), a.data(), b.data(), bytes, 0.5, 0.5);
    unpack_v210_line(out.data(), w, y.data(), cb.data(), cr.data());
    CHECK(y[0] == 300);
    CHECK(y[47] == 300);
    CHECK(cb[0] == 500);
    CHECK(cr[23] == 500);

    // a fade to black: chroma goes to 512, luma to 64
    mix_v210(out.data(), a.data(), nullptr, bytes, 0.0, 0.0);
    unpack_v210_line(out.data(), w, y.data(), cb.data(), cr.data());
    CHECK(y[3] == 64);
    CHECK(cb[3] == 512);
    CHECK(cr[3] == 512);

    mix_v210(out.data(), a.data(), nullptr, bytes, 0.5, 0.0);
    unpack_v210_line(out.data(), w, y.data(), cb.data(), cr.data());
    CHECK(y[5] == 82);   // 64 + (100 - 64) / 2
    CHECK(cb[2] == 406); // 512 + (300 - 512) / 2

    // in place (dst = a)
    std::vector<std::uint8_t> c = a;
    mix_v210(c.data(), c.data(), b.data(), bytes, 0.0, 1.0);
    CHECK(std::memcmp(c.data(), b.data(), bytes) == 0);

    const auto abytes = alpha10_size(w, h);
    std::vector<std::uint8_t> aa(abytes), ab(abytes), ao(abytes);
    fill_alpha10(aa.data(), w, h, 1023);
    fill_alpha10(ab.data(), w, h, 0);
    std::vector<std::uint16_t> al(w);
    mix_a10(ao.data(), aa.data(), ab.data(), abytes, 0.25, 0.75, 0);
    unpack_alpha10_line(ao.data(), w, al.data());
    CHECK(al[0] == 256);
    CHECK(al[47] == 256);
    mix_a10(ao.data(), nullptr, nullptr, abytes, 0, 0, 1023);
    unpack_alpha10_line(ao.data(), w, al.data());
    CHECK(al[10] == 1023);
}

TEST_CASE("playout: a playlist that ends with a held clip counts down to it; a held clip has no time left") {
    Playout p;
    auto ctx = playlist({clip("a", EndAction::Next), clip("b", EndAction::Hold)});
    ctx->entries[1].frames_hint = 50;
    p.cue(ctx->entries[0], ctx, 0);
    ready_all(p, {{"a", 50}, {"b", 50}});
    p.take(0);
    p.tick(0);
    auto st = p.status(0, 50);
    CHECK_FALSE(st["playlist"].value("endless", true));
    CHECK(st["playlist"].value("remaining_s", 0.0) == doctest::Approx(2.0));
    for (int t = 1; t <= 101; ++t) {
        p.tick(t);
        ready_all(p, {{"a", 50}, {"b", 50}});
    }
    st = p.status(101, 50);
    CHECK(st.value("state", "") == "ended");
    CHECK(st["program"].value("remaining_s", 1.0) == 0.0);
}

TEST_CASE("playout: auto take puts the cued clip on air when the program is done") {
    SUBCASE("a clip that stops: the cued clip follows on the frame after its last") {
        Playout p;
        p.cue(clip("a", EndAction::Stop), nullptr, 0);
        ready_all(p, {{"a", 4}});
        p.take(0);
        p.set_auto_take(true);
        p.cue(clip("b", EndAction::Stop), nullptr, 0);
        ready_all(p, {{"a", 4}, {"b", 6}});
        for (int t = 0; t < 4; ++t) CHECK(p.tick(t).a.frame == t);
        const auto f = p.tick(4);
        CHECK(on_air(p, f) == "b");
        CHECK(f.a.frame == 0);
        CHECK(f.a.rolling);
        CHECK(state(p, 4) == "playing");
    }
    SUBCASE("a clip that holds: a mix starts before its end, with the cued clip's transition") {
        Playout p;
        p.cue(clip("a", EndAction::Hold), nullptr, 0);
        p.take(0);
        p.set_auto_take(true);
        p.cue(clip("b", EndAction::Stop, TransitionKind::Mix, 2), nullptr, 0);
        ready_all(p, {{"a", 6}, {"b", 6}});
        for (int t = 0; t < 4; ++t) CHECK_FALSE(p.tick(t).transition);
        const auto f = p.tick(4);
        CHECK(f.transition);
        CHECK(f.a.frame == 4);
        CHECK(f.b.frame == 0);
        CHECK(p.tick(6).a.frame == 2);  // b on air, two frames in
        CHECK(on_air(p, p.tick(6)) == "b");
    }
    SUBCASE("a clip that loops loops on until a take") {
        Playout p;
        p.cue(clip("a", EndAction::Loop), nullptr, 0);
        p.take(0);
        p.set_auto_take(true);
        p.cue(clip("b"), nullptr, 0);
        ready_all(p, {{"a", 3}, {"b", 3}});
        for (int t = 0; t < 10; ++t) CHECK(p.tick(t).a.frame == t % 3);
        CHECK(on_air(p, p.tick(10)) == "a");
        p.take(11);
        CHECK(on_air(p, p.tick(11)) == "b");
    }
    SUBCASE("a held clip takes a clip cued after its end once that is loaded; the last frame stays meanwhile") {
        Playout p;
        p.cue(clip("a", EndAction::Hold), nullptr, 0);
        ready_all(p, {{"a", 3}});
        p.take(0);
        p.set_auto_take(true);
        for (int t = 0; t < 6; ++t) p.tick(t);
        CHECK(state(p, 6) == "ended");
        p.cue(clip("b"), nullptr, 6);
        CHECK(p.tick(6).a.frame == 2);  // b is not loaded yet
        CHECK(on_air(p, p.tick(7)) == "a");
        ready_all(p, {{"a", 3}, {"b", 5}});
        const auto f = p.tick(8);
        CHECK(on_air(p, f) == "b");
        CHECK(f.a.frame == 0);
    }
    SUBCASE("a clip that stops waits on its last frame for a cued clip that is still loading") {
        Playout p;
        p.cue(clip("a", EndAction::Stop), nullptr, 0);
        ready_all(p, {{"a", 3}});
        p.take(0);
        p.set_auto_take(true);
        p.cue(clip("b"), nullptr, 0);
        for (int t = 0; t < 3; ++t) p.tick(t);
        auto f = p.tick(3);
        CHECK(on_air(p, f) == "a");
        CHECK(f.a.frame == 2);
        CHECK_FALSE(f.a.rolling);
        CHECK(state(p, 3) == "waiting");
        ready_all(p, {{"a", 3}, {"b", 5}});
        f = p.tick(4);
        CHECK(on_air(p, f) == "b");
        CHECK(f.a.frame == 0);
        // Cleared while it waits: the stop goes ahead.
        Playout q;
        q.cue(clip("a", EndAction::Stop), nullptr, 0);
        ready_all(q, {{"a", 2}});
        q.take(0);
        q.set_auto_take(true);
        q.cue(clip("b"), nullptr, 0);
        for (int t = 0; t < 3; ++t) q.tick(t);
        q.clear_next();
        CHECK(q.tick(3).a.layer == 0);
    }
    SUBCASE("with nothing on air a cue goes on air once loaded, not parked (also in park mode)") {
        Playout p;
        p.set_auto_take(true);
        p.cue(clip("a", EndAction::Stop), nullptr, 0);
        CHECK(state(p, 0) == "idle");
        CHECK(p.tick(0).a.layer == 0);
        ready_all(p, {{"a", 4}});
        auto f = p.tick(1);
        CHECK(on_air(p, f) == "a");
        CHECK(f.a.frame == 0);
        CHECK(f.a.rolling);
        CHECK(p.tick(2).a.frame == 1);
    }
    SUBCASE("off: a held clip stays held with a clip cued") {
        Playout p;
        p.cue(clip("a", EndAction::Hold), nullptr, 0);
        p.take(0);
        p.cue(clip("b"), nullptr, 0);
        ready_all(p, {{"a", 3}, {"b", 3}});
        for (int t = 0; t < 8; ++t) p.tick(t);
        CHECK(state(p, 8) == "ended");
        CHECK(on_air(p, p.tick(8)) == "a");
    }
}
