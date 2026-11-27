// Shared UI state: info, outputs and meters from the WebSocket (/api/v1/events, ten times a
// second), the lists (library, jobs, presets, playlists), the selected output, the drafts that
// survive tab switches and reconnects, and the actions every page uses.
import { computed, reactive } from "vue";
import { api, canon, clone, formatFps, patternLabel, signalLabel } from "./api.js";

function storedOutput() {
  try {
    return Number(localStorage.getItem("lm-mxl-playout.output")) || 0;
  } catch {
    return 0;
  }
}

export const live = reactive({
  info: null, // GET /api/v1/info
  nmos: null, // GET /api/v1/nmos
  outputs: [], // output status, newest from the WebSocket
  peaks: {}, // output index -> peak per channel (linear)
  jobs: [],
  library: [],
  presets: [],
  playlists: [],
  connected: false,
  everConnected: false,
  error: "", // API unreachable
  actionError: "", // the last failed action (banner)
  notice: "", // the last action's result worth showing (banner)
  selected: storedOutput(),
  underruns: {}, // output index -> new underruns in the last minute
});

export const selectedOutput = computed(() => live.outputs.find((o) => o.index === live.selected) || live.outputs[0] || null);
export const outputLabel = (index) => live.outputs.find((o) => o.index === index)?.label || `Out ${index + 1}`;
export const itemsById = computed(() => Object.fromEntries(live.library.map((i) => [i.id, i])));
export const activeJobs = computed(() => live.jobs.filter((j) => j.state === "queued" || j.state === "running"));

/** What an output plays, in one line: the clip on air (playout), the pattern and test audio, the playlist and item, or the item. */
export function sourceText(o) {
  const s = o.source || {};
  if (s.type === "playout") {
    const p = o.playout || {};
    const prog = p.program;
    if (!prog) return p.next ? `Black · next ${p.next.name}` : "Black";
    return `${prog.name}${prog.playlist_name ? ` · ${prog.playlist_name} ${prog.entry + 1}/${prog.playlist_count}` : ""}`;
  }
  if (s.type === "pattern") return `${patternLabel(s.pattern)}${s.pluge ? " + PLUGE" : ""} · ${signalLabel(s.audio)}`;
  if (s.type === "playlist") return `Playlist ${live.playlists.find((p) => p.id === s.playlist_id)?.name || ""}${o.item ? ` · ${o.item}` : ""}`;
  return `${s.type === "still" ? "Still" : "Video"} ${o.item || live.library.find((i) => i.id === s.item_id)?.name || ""}`;
}

export function selectOutput(index) {
  live.selected = index;
  try {
    localStorage.setItem("lm-mxl-playout.output", String(index));
  } catch {
    /* kept for this tab only */
  }
}

/** Runs an action; a failure goes to the error banner. Returns the result (true for an empty answer), or undefined. */
export async function act(fn, notice = "") {
  try {
    const result = await fn();
    live.actionError = "";
    if (notice) live.notice = typeof notice === "function" ? notice(result) : notice;
    return result ?? true;
  } catch (e) {
    live.actionError = e.message;
    return undefined;
  }
}

function applyOutput(status) {
  if (!status || typeof status.index !== "number") return;
  const i = live.outputs.findIndex((o) => o.index === status.index);
  if (i >= 0) live.outputs[i] = status;
}

/** POST/PUT/PATCH on /api/v1/outputs/{n}[/leaf]; the answer is the output's new status. */
function outputCall(method, index, leaf, body) {
  return act(async () => {
    const status = await api[method](`/api/v1/outputs/${index}${leaf ? `/${leaf}` : ""}`, body);
    applyOutput(status);
    return status;
  });
}
export const transport = (index, action) => outputCall("post", index, "transport", { action });
/** POST /api/v1/outputs/{n}/playout: cue, take, pause, resume, stop, clear, seek, jump. */
export const playout = (index, body) => outputCall("post", index, "playout", body);
export const seek = (index, frame) => outputCall("post", index, "seek", { frame: Math.max(0, Math.round(frame)) });
export const setSource = (index, body) => outputCall("put", index, "source", body);
export const patchOutput = (index, body) => outputCall("patch", index, "", body);

/** Options only a conversion applies: with any of them an item is converted, else it plays as uploaded. */
export function needsConversion(o) {
  return o.fps_mode === "motion" || Number(o.crossfade_ms) > 0 || Number(o.map_channels) > 0;
}

/** Item length in frames at an output's format (0 while it is not converted for it). */
export function itemFrames(item, format) {
  const conv = item?.conversions?.[format];
  if (conv?.status !== "ready") return 0;
  return item.type === "sprite" ? conv.clip_frames || 1 : conv.frames || 1;  // an animation: output frames per loop
}

// ---- playout (operator) -------------------------------------------------------

function storedOperator() {
  try {
    return JSON.parse(localStorage.getItem("lm-mxl-playout.operator") || "{}");
  } catch {
    return {};
  }
}
/** The operator's choices on this browser: the transition of the take, the end of a single clip. */
export const operator = reactive({ transition: "auto", frames: 12, clipEnd: "stop", ...storedOperator() });
export function saveOperator() {
  try {
    localStorage.setItem("lm-mxl-playout.operator", JSON.stringify(operator));
  } catch {
    /* kept for this tab only */
  }
}
/** The transition override of a take ({} for the entry's own). */
export function takeOverride() {
  if (operator.transition === "auto") return {};
  return { transition: operator.transition, transition_frames: operator.transition === "cut" ? 0 : Math.max(1, Number(operator.frames) || 12) };
}
export const take = (index) => playout(index, { action: "take", ...takeOverride() });
export const cueItem = (index, item, extra = {}) => playout(index, { action: "cue", item_id: item.id, end: operator.clipEnd, ...extra });
export const cuePlaylist = (index, playlist, entry = 0, extra = {}) => playout(index, { action: "cue", playlist_id: playlist.id, entry, ...extra });

/** The playlist an output plays (on air, incoming or next), or null. */
export function playlistOnAir(o) {
  const p = o?.playout || {};
  for (const k of ["incoming", "program", "next"]) if (p[k]?.playlist_id) return live.playlists.find((x) => x.id === p[k].playlist_id) || null;
  return null;
}

/** A length in seconds as M:SS:FF at `fps` (operator countdowns). */
export function fmtClock(seconds, fps = 25) {
  if (seconds == null || !Number.isFinite(seconds)) return "--:--:--";
  const total = Math.max(0, Math.round(seconds * fps));
  const f = Math.round(fps);
  const ff = total % f;
  const s = Math.floor(total / f);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const pad = (n) => String(n).padStart(2, "0");
  return `${h ? `${h}:${pad(m)}` : m}:${pad(s % 60)}:${pad(ff)}`;
}

/** Plays a library item on an output now (cue and take, on the playout). */
export const playItem = (index, item) => cueItem(index, item, { take: true });
/** Plays a playlist on an output now (cue its first entry and take). */
export const playPlaylist = (index, playlist) => cuePlaylist(index, playlist, 0, { take: true });

/**
 * The test player's playlist source (Source tab): the API takes the entries with their frame
 * counts: a video's converted length, a still's hold time (`hold_s`) at the output's rate.
 */
export async function playPlaylistSource(index, playlist) {
  const output = live.outputs.find((o) => o.index === index);
  const fps = formatFps(output?.format);
  const entries = (playlist.entries || [])
    .filter((e) => itemsById.value[e.item_id])
    .map((e) => {
      const item = itemsById.value[e.item_id];
      const frames = item.type === "still" ? Math.max(1, Math.round((e.hold_s || 5) * fps)) : itemFrames(item, output?.format) || 1;
      return { item_id: e.item_id, loops: e.end === "loop" ? 0 : Number(e.loops ?? 1), frames };
    });
  if (!entries.length) {
    live.actionError = `Playlist "${playlist.name}" has no items in the library.`;
    return undefined;
  }
  const result = await setSource(index, { type: "playlist", playlist_id: playlist.id, item_id: entries[0].item_id, entries });
  if (result) await transport(index, "play");
  return result;
}

/** Plays a library item as the output's test source (a still keeps the output's test audio). */
export async function playItemSource(index, item) {
  const output = live.outputs.find((o) => o.index === index);
  const src = output?.source || {};
  const body =
    item.type === "still"
      ? { type: "still", item_id: item.id, audio: src.audio || "sine", frequency: src.frequency ?? 1000, level_dbfs: src.level_dbfs ?? -18 }
      : { type: "video", item_id: item.id };  // an animated sprite plays as a clip
  const result = await setSource(index, body);
  if (result) await transport(index, "play");
  return result;
}

// ---- lists -----------------------------------------------------------------

export async function refreshLists() {
  const [library, presets, playlists, jobs] = await Promise.allSettled([
    api.get("/api/v1/library"),
    api.get("/api/v1/presets"),
    api.get("/api/v1/playlists"),
    api.get("/api/v1/jobs"),
  ]);
  if (library.status === "fulfilled") live.library = library.value;
  if (presets.status === "fulfilled") live.presets = presets.value;
  if (playlists.status === "fulfilled") live.playlists = playlists.value;
  if (jobs.status === "fulfilled") live.jobs = jobs.value;
}

export async function refreshNmos() {
  try {
    live.nmos = await api.get("/api/v1/nmos");
  } catch {
    /* keep the last answer; the connection banner tells */
  }
}

// ---- uploads (chunked, §6.1); they go on while other tabs are open --------------

export const uploads = reactive({
  options: { fit: "fit", fps_mode: "drop", alpha_mode: "straight", loudness: true, crossfade_ms: 0, map_channels: 0, convert: false, tags: "" },
  files: [], // { name, size, sent, state: sending | ingesting | done | failed, error, item }
});

export async function uploadFiles(fileList) {
  const o = uploads.options;
  const options = {
    fit: o.fit,
    fps_mode: o.fps_mode,
    alpha_mode: o.alpha_mode,
    loudness: o.loudness,
    crossfade_ms: Number(o.crossfade_ms) || 0,
    map_channels: Number(o.map_channels) || 0,
    convert: !!o.convert,
    tags: o.tags
      .split(",")
      .map((t) => t.trim())
      .filter(Boolean),
  };
  for (const file of fileList) {
    uploads.files.unshift({ name: file.name, size: file.size, sent: 0, state: "sending", error: "", item: null });
    const entry = uploads.files[0];
    try {
      const meta = await api.post("/api/v1/uploads", { name: file.name, size: file.size, options });
      const chunk = meta.chunk_size || 8 * 1024 * 1024;
      for (let off = 0, n = 0; off < file.size; off += chunk, n++) {
        await api.putRaw(`/api/v1/uploads/${meta.id}/chunks/${n}`, await file.slice(off, Math.min(file.size, off + chunk)).arrayBuffer());
        entry.sent = Math.min(file.size, off + chunk);
      }
      entry.state = "ingesting";
      entry.item = await api.post(`/api/v1/uploads/${meta.id}/complete`, {});
      entry.state = "done";
    } catch (e) {
      entry.state = "failed";
      entry.error = e.message;
    }
    refreshLists();
  }
}

// ---- drafts ----------------------------------------------------------------
// A draft is taken from the status while it is unchanged and kept while it is edited, so tab
// switches, WebSocket reconnects and status pushes never overwrite an edit.

export const drafts = reactive({ source: {}, setup: {}, burnin: {}, playlist: null, preset: null });

export function sourceOf(status) {
  const s = status?.source || {};
  return {
    type: s.type || "pattern",
    pattern: s.pattern || "smpte_rp219",
    pluge: !!s.pluge,
    audio: s.audio || "sine",
    frequency: s.frequency ?? 1000,
    level_dbfs: s.level_dbfs ?? -18,
    sync_beep: !!s.sync_beep,
    item_id: s.item_id || "",
    playlist_id: s.playlist_id || "",
  };
}
export function setupOf(status) {
  const keys = ["label", "format", "audio_channels", "key_mode", "idle_key", "anc", "tc_source", "drop_frame", "atc", "pause_audio", "loop"];
  return Object.fromEntries(keys.map((k) => [k, status?.[k]]));
}

/** Takes `fresh` into drafts[kind][index] unless the draft holds an edit. */
export function syncDraft(kind, index, fresh) {
  const d = drafts[kind][index];
  const c = canon(fresh);
  if (!d || canon(d.value) === d.base) {
    if (!d || d.base !== c) drafts[kind][index] = { value: clone(fresh), base: c };
  } else {
    d.base = c; // edited: the draft stays, compared with the newest status
  }
}
export const isDirty = (kind, index) => {
  const d = drafts[kind][index];
  return !!d && canon(d.value) !== d.base;
};
export function revertDraft(kind, index, fresh) {
  drafts[kind][index] = { value: clone(fresh), base: canon(fresh) };
}

// Burn-ins are live (§5.4): each edit goes out 80 ms after the last change, one request at a time.
// A status from elsewhere is taken while nothing is pending.
const burninTimers = {};
export function syncBurnin(index, status) {
  const d = drafts.burnin[index];
  const c = canon(status);
  if (!d) {
    drafts.burnin[index] = { value: clone(status), pushed: c, sending: false, error: "", quietUntil: 0 };
    return;
  }
  if (burninTimers[index] || d.sending || Date.now() < d.quietUntil || c === d.pushed) return;
  d.value = clone(status);
  d.pushed = c;
  d.error = "";
}
export function editBurnin(index) {
  clearTimeout(burninTimers[index]);
  burninTimers[index] = setTimeout(() => pushBurnin(index), 80);
}
async function pushBurnin(index) {
  delete burninTimers[index];
  const d = drafts.burnin[index];
  if (!d) return;
  if (d.sending) {
    editBurnin(index);
    return;
  }
  d.sending = true;
  try {
    const status = await api.put(`/api/v1/outputs/${index}/burnin`, clone(d.value));
    d.pushed = canon(status.burnin);
    d.error = "";
    d.quietUntil = Date.now() + 600; // a status pushed before the change may still arrive
    applyOutput(status);
  } catch (e) {
    d.error = e.message;
  } finally {
    d.sending = false;
  }
}
export const burninPending = (index) => {
  const d = drafts.burnin[index];
  return !!d && (!!burninTimers[index] || d.sending || canon(d.value) !== d.pushed);
};

// ---- live connection ---------------------------------------------------------

let socket = null;
let retry = 0;
let timers = [];
const history = {}; // output index -> [{ t, n }] underrun counts, one per second, the last minute

function trackUnderruns() {
  const now = Date.now();
  for (const o of live.outputs) {
    const h = (history[o.index] ||= []);
    if (!h.length || now - h[h.length - 1].t >= 1000) h.push({ t: now, n: o.underruns });
    while (h.length > 1 && now - h[0].t > 60000) h.shift();
    live.underruns[o.index] = Math.max(0, h[h.length - 1].n - h[0].n);
  }
}

function applyJobs(jobs) {
  const before = activeJobs.value.length;
  const known = Object.fromEntries(live.jobs.map((j) => [j.id, j]));
  live.jobs = jobs.map((j) => ({ ...(known[j.id] || {}), ...j }));
  // A conversion finished: the library has new conversions (and errors in /api/v1/jobs).
  if (activeJobs.value.length !== before || jobs.length !== Object.keys(known).length) refreshLists();
}

async function pollOutputs() {
  try {
    live.outputs = await api.get("/api/v1/outputs");
    if (!live.info) live.info = await api.get("/api/v1/info");
    live.error = "";
    trackUnderruns();
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
  };
  socket.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (!msg.outputs) return;
      live.outputs = msg.outputs.map((o) => o.status);
      for (const o of msg.outputs) live.peaks[o.index] = o.peaks || [];
      if (msg.jobs) applyJobs(msg.jobs);
      trackUnderruns();
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 2000);
  };
}

/** Loads /api/v1/info and the lists, then follows the WebSocket; polls while it is down. */
export async function startLive() {
  try {
    live.info = await api.get("/api/v1/info");
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
  await pollOutputs();
  refreshLists();
  refreshNmos();
  connect();
  timers = [
    setInterval(() => {
      if (!live.connected) pollOutputs();
    }, 2000),
    // Other UIs and API clients change the lists too.
    setInterval(refreshLists, 5000),
    setInterval(refreshNmos, 5000),
  ];
}

export function stopLive() {
  timers.forEach(clearInterval);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}
