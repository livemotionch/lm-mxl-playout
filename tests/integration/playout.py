#!/usr/bin/env python3
"""CPU integration of the playout: park and take, a playlist with a cut and with a mix, from RAM and
streamed. Reads the written grains back (centre pixel luma) and checks that the clips follow each
other frame-accurately: no black and no repeated frame at a cut, a monotonic mix.

By default the uploads play as uploaded (direct: decoded while they play), and a clip of another
size and frame rate with long GOPs and B-frames (640x360, 30 fps) is cut frame-accurately too.
PLAYOUT_CONVERT=1 converts the uploads to ProRes mezzanines (played from RAM), and with
PLAYOUT_STREAM=1 every mezzanine streams (PLAYER_RAM_CLIP_MAX_S=0)."""

import ctypes
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "lm-mxl-playout"
BASE = os.environ.get("PLAYER_BASE", "http://127.0.0.1:18140")
PORT = int(BASE.rsplit(":", 1)[-1])
STREAM = os.environ.get("PLAYOUT_STREAM") == "1"
CONVERT = os.environ.get("PLAYOUT_CONVERT") == "1" or STREAM
FPS = 25


def http(method, path, body=None, raw=None, timeout=60):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    headers = {"Content-Type": "application/json"} if body is not None else {}
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as res:
        raw_body = res.read()
        if res.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw_body) if raw_body else None
        return raw_body


libc = ctypes.CDLL(None)


class Ts(ctypes.Structure):
    _fields_ = [("sec", ctypes.c_long), ("nsec", ctypes.c_long)]


def tai_index():
    t = Ts()
    libc.clock_gettime(11, ctypes.byref(t))  # CLOCK_TAI
    return (t.sec * 10**9 + t.nsec) * FPS // 10**9


def make_clip(path, colour, seconds, size="320x180", rate=FPS, gop=["-g", "1"]):
    subprocess.check_call(
        ["ffmpeg", "-hide_banner", "-y",
         "-f", "lavfi", "-i", f"color=c={colour}:s={size}:r={rate}:d={seconds}",
         "-f", "lavfi", "-i", f"sine=frequency=1000:duration={seconds}:sample_rate=44100",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", *gop, "-c:a", "aac", "-ac", "2", str(path)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def upload(path):
    meta = http("POST", "/api/v1/uploads", {"name": path.name, "size": path.stat().st_size, "options": {"fit": "fill", "convert": CONVERT}})
    http("PUT", f"/api/v1/uploads/{meta['id']}/chunks/0", raw=path.read_bytes())
    item = http("POST", f"/api/v1/uploads/{meta['id']}/complete", {})
    deadline = time.time() + 120
    while time.time() < deadline:
        item = http("GET", f"/api/v1/library/{item['id']}")
        conv = item["conversions"].get("720p25", {})
        if conv.get("status") == "ready":
            assert bool(conv.get("direct")) == (not CONVERT), conv
            return item
        if conv.get("status") == "failed":
            raise SystemExit(f"conversion failed: {conv}")
        time.sleep(0.3)
    raise SystemExit("conversion timeout")


def playout(body, expect=200):
    try:
        return http("POST", "/api/v1/outputs/0/playout", body)
    except urllib.error.HTTPError as e:
        if e.code == expect:
            return json.loads(e.read())
        raise SystemExit(f"{body}: HTTP {e.code} {e.read()!r}")


def lumas(first, last):
    """Centre luma of grains first..last (None when a grain could not be read)."""
    out = []
    for i in range(first, last + 1):
        p = http("GET", f"/api/v1/outputs/0/probe?index={i}")
        out.append(p["y"] if p["ok"] else None)
    return out


def classify(y, levels):
    name, level = min(levels.items(), key=lambda kv: abs(kv[1] - y))
    return name if abs(level - y) <= 12 else "mix"


def from_first(seq, value):
    """seq from the first `value` on, without black at its end."""
    seq = seq[seq.index(value):]
    while seq and seq[-1] == "black":
        seq = seq[:-1]
    return seq


def runs(seq):
    """[(value, count)] of consecutive equal values."""
    out = []
    for v in seq:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return [tuple(r) for r in out]


def wait_state(want, timeout=10):
    deadline = time.time() + timeout
    st = None
    while time.time() < deadline:
        st = http("GET", "/api/v1/outputs/0")
        if st["playout"]["state"] == want:
            return st
        time.sleep(0.05)
    raise SystemExit(f"state {want} not reached: {st['playout'] if st else None}")


def main():
    work = Path("/tmp/lm-playout-int" + ("-stream" if STREAM else "-convert" if CONVERT else ""))
    subprocess.call(["rm", "-rf", str(work)])
    work.mkdir(parents=True)
    a, b = work / "light.mp4", work / "dark.mp4"
    make_clip(a, "0xB4B4B4", 1)
    make_clip(b, "0x404040", 1)
    m = work / "mid30.mp4"  # another size and rate, long GOPs with B-frames
    make_clip(m, "0x808080", 1, size="640x360", rate=30, gop=["-g", "15", "-bf", "2"])

    env = os.environ.copy()
    env.update({
        "PLAYER_FORMAT": "720p25",
        "PLAYER_OUTPUTS": "1",
        "PLAYER_AUDIO_CHANNELS": "2",
        "PLAYER_LIBRARY_DIR": str(work / "lib"),
        "PLAYER_STATE": str(work / "state.json"),
        "PLAYER_FONT_DIR": str(ROOT / "assets" / "fonts"),
        "PLAYER_WEB_ROOT": str(ROOT / "web" / "dist"),
        "PLAYER_RAM_CLIP_MAX_S": "0" if STREAM else "20",
        "MXL_OUTPUT_DOMAIN_DIR": str(work / "domain"),
        "MXL_HISTORY_DURATION_NS": "6000000000",  # grains stay readable for 6 s
        "NMOS_SEED": "int-playout",
        "WEB_PORT": str(PORT),
        "NMOS_PORT": str(PORT + 1),
    })
    log = open(work / "player.log", "w")
    proc = subprocess.Popen([str(BIN), "--config", str(work / "missing.json")], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 10
        while True:
            try:
                if http("GET", "/livez") == b"ok\n":
                    break
            except Exception:
                pass
            if time.time() > deadline:
                raise SystemExit("player did not start")
            time.sleep(0.1)

        st = http("GET", "/api/v1/outputs/0")
        assert st["source"]["type"] == "playout", st["source"]
        assert st["playout"]["state"] == "idle", st["playout"]
        assert not st["burnin"]["label"] and not st["burnin"]["timecode"], st["burnin"]
        time.sleep(0.3)
        black = http("GET", "/api/v1/outputs/0/probe")
        assert black["ok"] and abs(black["y"] - 64) <= 2, black  # nothing on air: black, not bars

        light = upload(a)
        dark = upload(b)
        assert light["conversions"]["720p25"]["frames"] == FPS

        # Nothing to take yet.
        playout({"action": "take"}, expect=409)

        # Park and take: the cued clip stands at its first frame, the take rolls it.
        st = playout({"action": "cue", "item_id": light["id"], "end": "hold"})
        assert st["playout"]["state"] == "parked", st["playout"]
        time.sleep(0.6)
        st = http("GET", "/api/v1/outputs/0")
        assert st["playout"]["program"]["ready"] and st["playout"]["program"]["position"] == 0, st["playout"]
        levels = {"black": 64, "light": http("GET", "/api/v1/outputs/0/probe")["y"]}
        assert levels["light"] > 500, levels
        playout({"action": "take"})
        st = wait_state("ended", timeout=4)
        assert st["playout"]["program"]["position"] == FPS - 1, st["playout"]
        playout({"action": "stop"})
        wait_state("idle")

        # Learn the dark level.
        playout({"action": "cue", "item_id": dark["id"], "end": "hold"})
        time.sleep(0.6)
        levels["dark"] = http("GET", "/api/v1/outputs/0/probe")["y"]
        playout({"action": "stop"})
        assert 150 < levels["dark"] < 400, levels
        print("levels", levels)

        # From here on a cue waits in the background (preloaded) until the take.
        st = http("PATCH", "/api/v1/outputs/0", {"cue_mode": "background"})
        assert st["playout"]["cue_mode"] == "background", st["playout"]

        # A playlist with a cut: 25 light frames, then 25 dark ones, then black. Nothing missing.
        cut = http("POST", "/api/v1/playlists", {"name": "cut", "entries": [
            {"item_id": light["id"], "end": "next"},
            {"item_id": dark["id"], "end": "stop"}]})
        assert all(e.get("id") for e in cut["entries"]), cut
        playout({"action": "cue", "playlist_id": cut["id"]})  # background: preloaded, air stays black
        time.sleep(0.6)
        assert http("GET", "/api/v1/outputs/0")["playout"]["next"]["ready"]
        start = tai_index()
        playout({"action": "take"})
        time.sleep(2.6)
        seq = [classify(y, levels) if y is not None else "none" for y in lumas(start - 2, start + 70)]
        r = runs(seq)
        print("cut", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
        assert runs(from_first(seq, "light")) == [("light", FPS), ("dark", FPS)], r
        assert r[-1][0] == "black", r

        # A mix of 10 frames into the second clip: it starts 10 frames before the first ends.
        mix = http("POST", "/api/v1/playlists", {"name": "mix", "entries": [
            {"item_id": light["id"], "end": "next"},
            {"item_id": dark["id"], "end": "stop", "transition": "mix", "transition_frames": 10}]})
        playout({"action": "cue", "playlist_id": mix["id"]})  # background: preloaded, air stays black
        time.sleep(0.6)
        assert http("GET", "/api/v1/outputs/0")["playout"]["next"]["ready"]
        start = tai_index()
        playout({"action": "take"})
        time.sleep(2.4)
        ys = lumas(start - 2, start + 60)
        seq = [classify(y, levels) if y is not None else "none" for y in ys]
        r = runs(seq)
        print("mix", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
        assert runs(from_first(seq, "light")) == [("light", FPS - 10), ("mix", 10), ("dark", FPS - 10)], r
        mixed = [y for y, c in zip(ys, seq) if c == "mix"]
        assert all(x > y for x, y in zip(mixed, mixed[1:])), mixed  # light to dark: falling

        # A dip of 10 frames: down to black, then up; the dark clip starts at the middle.
        dip = http("POST", "/api/v1/playlists", {"name": "dip", "entries": [
            {"item_id": light["id"], "end": "next"},
            {"item_id": dark["id"], "end": "stop", "transition": "dip", "transition_frames": 10}]})
        playout({"action": "cue", "playlist_id": dip["id"]})  # background: preloaded, air stays black
        time.sleep(0.6)
        assert http("GET", "/api/v1/outputs/0")["playout"]["next"]["ready"]
        start = tai_index()
        playout({"action": "take"})
        time.sleep(2.4)
        ys = lumas(start - 2, start + 60)
        seq = [classify(y, levels) if y is not None else "none" for y in ys]
        r = runs(seq)
        print("dip", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
        first = seq.index("light")
        last = max(i for i, c in enumerate(seq) if c == "dark")
        core = seq[first:last + 1]
        assert runs(core)[0] == ("light", FPS - 10), r
        between = [i for i in range(first, last + 1) if seq[i] not in ("light", "dark")]
        assert len(between) == 10 and between == list(range(between[0], between[0] + 10)), r
        assert min(ys[i] for i in between) <= levels["black"] + 40, [ys[i] for i in between]
        assert runs(core)[-1] == ("dark", FPS - 5), r  # dark rolls from the dip's middle

        # Another size and frame rate, long GOPs with B-frames: 30 fps for 1 s is 25 frames here,
        # cut frame-accurately after the dark clip (direct: decoded and conformed while it plays).
        if not CONVERT:
            mid = upload(m)
            assert mid["conversions"]["720p25"]["frames"] == FPS, mid["conversions"]
            playout({"action": "cue", "item_id": mid["id"], "end": "hold", "take": True})
            wait_state("ended", timeout=4)
            levels["mid"] = http("GET", "/api/v1/outputs/0/probe")["y"]
            playout({"action": "stop"})
            wait_state("idle")
            assert levels["dark"] + 100 < levels["mid"] < levels["light"] - 100, levels
            other = http("POST", "/api/v1/playlists", {"name": "other", "entries": [
                {"item_id": dark["id"], "end": "next"},
                {"item_id": mid["id"], "end": "next"},
                {"item_id": light["id"], "end": "stop"}]})
            playout({"action": "cue", "playlist_id": other["id"]})
            time.sleep(0.6)
            assert http("GET", "/api/v1/outputs/0")["playout"]["next"]["ready"]
            start = tai_index()
            playout({"action": "take"})
            time.sleep(3.6)
            seq = [classify(y, levels) if y is not None else "none" for y in lumas(start - 2, start + 90)]
            r = runs(seq)
            print("other format", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
            assert runs(from_first(seq, "dark")) == [("dark", FPS), ("mid", FPS), ("light", FPS)], r

        # Auto take: a single clip that holds, with another cued while it plays, goes straight on
        # into it at its end (25 light, then 25 dark, then black); off again, a held clip stays.
        playout({"action": "cue", "item_id": light["id"], "end": "hold"})
        time.sleep(0.6)
        start = tai_index()
        playout({"action": "take"})
        st = http("PATCH", "/api/v1/outputs/0", {"auto_take": True})
        assert st["playout"]["auto_take"], st["playout"]
        playout({"action": "cue", "item_id": dark["id"], "end": "stop"})
        time.sleep(2.6)
        seq = [classify(y, levels) if y is not None else "none" for y in lumas(start - 2, start + 70)]
        r = runs(seq)
        print("auto take", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
        assert runs(from_first(seq, "light")) == [("light", FPS), ("dark", FPS)], r
        # Nothing on air: a cue goes on air by itself once it is loaded, no take.
        wait_state("idle", timeout=3)
        playout({"action": "cue", "item_id": light["id"], "end": "stop"})
        wait_state("playing", timeout=3)
        wait_state("idle", timeout=3)
        st = http("PATCH", "/api/v1/outputs/0", {"auto_take": False})
        assert not st["playout"]["auto_take"], st["playout"]
        try:
            http("PATCH", "/api/v1/outputs/0", {"auto_take": "yes"})
            raise SystemExit("auto_take 'yes' accepted")
        except urllib.error.HTTPError as e:
            assert e.code == 400, e.code
        wait_state("idle", timeout=3)

        # Loop the clip on air: a clip that stops loops on (no black between the passes) until
        # looping is off again, then it ends at the end of that pass.
        playout({"action": "cue", "item_id": light["id"], "end": "stop"})
        time.sleep(0.6)
        start = tai_index()
        playout({"action": "take"})
        st = playout({"action": "loop", "on": True})
        assert st["playout"]["program"]["end"] == "loop", st["playout"]["program"]
        assert st["playout"]["program"]["planned_end"] == "stop", st["playout"]["program"]
        time.sleep(3.2)
        seq = [classify(y, levels) if y is not None else "none" for y in lumas(start - 2, start + 2 * FPS + 25)]
        r = runs(seq)
        print("loop on air", r, "missed", http("GET", "/api/v1/outputs/0")["stream_missed"])
        assert runs(from_first(seq, "light"))[0][1] >= 2 * FPS + 20, r
        playout({"action": "loop"}, expect=400)
        st = playout({"action": "loop", "on": False})
        assert st["playout"]["program"]["end"] == "stop", st["playout"]["program"]
        wait_state("idle", timeout=2)
        playout({"action": "loop", "on": True}, expect=409)

        # The clip level: measured after the upload (EBU R128), a gain applied while it plays (no
        # conversion), and the automatic level to -23 LUFS.
        deadline = time.time() + 30
        while (lv := http("GET", f"/api/v1/library/{light['id']}")["level"])["status"] in ("queued", "measuring"):
            assert time.time() < deadline, lv
            time.sleep(0.2)
        assert lv["status"] == "ready" and -24 < lv["integrated_lufs"] < -12, lv
        playout({"action": "cue", "item_id": light["id"], "end": "loop", "take": True})
        time.sleep(0.6)

        def peak():
            """The median grain peak over 0.6 s (the loop point of the AAC clip has a transient)."""
            vals = []
            for _ in range(15):
                vals.append(max(http("GET", "/api/v1/outputs/0/meters")["peaks"]))
                time.sleep(0.04)
            return sorted(vals)[len(vals) // 2]

        p0 = peak()
        http("PATCH", f"/api/v1/library/{light['id']}", {"gain_db": -12})
        time.sleep(0.3)
        p1 = peak()
        print("level", lv, "peak", round(p0, 4), "at -12 dB", round(p1, 4))
        assert p0 > 0.05 and abs(p1 / p0 - 10 ** (-12 / 20)) < 0.03, (p0, p1)
        item = http("PATCH", f"/api/v1/library/{light['id']}", {"loudness": True})
        assert item["loudness"] and abs(lv["integrated_lufs"] + item["gain_db"] - (-23)) < 0.05, item
        time.sleep(0.3)
        p2 = peak()
        print("automatic gain", item["gain_db"], "peak", round(p2, 4))
        assert abs(p2 / p0 - 10 ** (item["gain_db"] / 20)) < 0.03, (p0, p2, item["gain_db"])
        try:
            http("PATCH", f"/api/v1/library/{light['id']}", {"gain_db": 99})
            raise SystemExit("gain 99 dB accepted")
        except urllib.error.HTTPError as e:
            assert e.code == 400, e.code
        http("PATCH", f"/api/v1/library/{light['id']}", {"gain_db": 0})
        playout({"action": "stop"})
        wait_state("idle")

        # Stop with a fade, then the status says idle; the API refuses an unknown action.
        playout({"action": "cue", "item_id": light["id"], "end": "loop", "take": True})
        time.sleep(0.5)
        playout({"action": "stop", "transition": "mix", "transition_frames": 12})
        wait_state("idle", timeout=3)
        playout({"action": "bogus"}, expect=400)

        st = http("GET", "/api/v1/outputs/0")
        print("underruns", st["underruns"], "stream_missed", st["stream_missed"])
        assert st["stream_missed"] == 0, st["stream_missed"]
        print("playout integration OK" + (" (converted, streamed)" if STREAM else " (converted)" if CONVERT else " (direct)"))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        if proc.returncode not in (0, 143, -15):
            print((work / "player.log").read_text()[-4000:], file=sys.stderr)


if __name__ == "__main__":
    main()
