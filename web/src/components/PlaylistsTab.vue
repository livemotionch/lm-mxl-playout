<script setup>
// Playlists: ordered library clips, each with its in and out point, what it does at its end
// (next, hold, loop, black), the transition into it (cut, mix, dip) and, for a still, how long it
// stays. The playlist can loop. Cue (or play) it on the selected output. A playlist on air plays
// as it was cued: cue it again to take changes on air.
import { computed, ref, watch } from "vue";
import OutputPicker from "./OutputPicker.vue";
import { api, canon, clone, formatFps } from "../api.js";
import { act, cuePlaylist, drafts, fmtClock, itemFrames, itemsById, live, refreshLists, selectedOutput } from "../store.js";

const ENDS = [
  { value: "next", label: "Next", title: "the next entry follows" },
  { value: "hold", label: "Hold", title: "the last frame stays until the next take" },
  { value: "loop", label: "Loop", title: "loops until the next take" },
  { value: "stop", label: "Black", title: "black and silence; the next entry waits for a take" },
];
const TRANSITIONS = [
  { value: "cut", label: "Cut" },
  { value: "mix", label: "Mix" },
  { value: "dip", label: "Dip" },
];

const d = computed(() => drafts.playlist);
const dirty = computed(() => !!d.value && canon(d.value.value) !== d.value.saved);
const newName = ref("");
const addItem = ref("");

function entryOf(e) {
  return {
    id: e.id || "",
    item_id: e.item_id,
    in_s: Number(e.in_s ?? 0),
    out_s: Number(e.out_s ?? 0),
    hold_s: Number(e.hold_s ?? 5),
    end: e.end || (Number(e.loops) === 0 ? "loop" : "next"),
    transition: e.transition || "cut",
    transition_frames: Number(e.transition_frames ?? 12),
  };
}
function load(id) {
  if (d.value && id !== d.value.id && dirty.value && !confirm(`Discard the unsaved changes to "${d.value.value.name}"?`)) return;
  const pl = live.playlists.find((p) => p.id === id);
  if (!pl) return;
  const value = { name: pl.name || "", loop: !!pl.loop, entries: (pl.entries || []).map(entryOf) };
  drafts.playlist = { id, value, saved: canon(value) };
}
watch(
  () => live.playlists.length,
  () => {
    if ((!d.value || !live.playlists.some((p) => p.id === d.value.id)) && live.playlists.length) load(live.playlists[0].id);
  },
  { immediate: true },
);

const format = computed(() => selectedOutput.value?.format);
const fps = computed(() => formatFps(format.value));
const itemSeconds = (item) => (item ? itemFrames(item, format.value) / fps.value : 0);
function seconds(e) {
  const item = itemsById.value[e.item_id];
  if (!item) return 0;
  if (item.type === "still") return Number(e.hold_s) || 0;
  const len = itemSeconds(item);
  const out = Number(e.out_s) > 0 ? Math.min(Number(e.out_s), len) : len;
  return Math.max(0, out - (Number(e.in_s) || 0));
}
const entries = computed(() => d.value?.value.entries || []);
const total = computed(() => entries.value.reduce((sum, e) => sum + seconds(e), 0));
const endless = computed(() => d.value?.value.loop || entries.value.some((e) => e.end === "loop" || e.end === "hold" || (itemsById.value[e.item_id]?.type === "still" && !Number(e.hold_s))));
const playable = computed(() => live.library.filter((i) => i.type === "video" || i.type === "still" || i.type === "sprite"));
const onAirOn = (id) => live.outputs.filter((o) => ["program", "incoming", "next"].some((k) => o.playout?.[k]?.playlist_id === id));
const invalid = (e) => Number(e.out_s) > 0 && Number(e.out_s) <= Number(e.in_s);

function move(i, by) {
  const list = d.value.value.entries;
  const [e] = list.splice(i, 1);
  list.splice(i + by, 0, e);
}
function add() {
  if (!addItem.value) return;
  d.value.value.entries.push(entryOf({ item_id: addItem.value }));
  addItem.value = "";
}
function duplicate(i) {
  const e = clone(d.value.value.entries[i]);
  e.id = "";
  d.value.value.entries.splice(i + 1, 0, e);
}
async function create() {
  const created = await act(() => api.post("/api/v1/playlists", { name: newName.value.trim(), loop: false, entries: [] }));
  if (!created) return;
  newName.value = "";
  await refreshLists();
  load(created.id);
}
async function save() {
  const body = clone(d.value.value);
  body.name = body.name.trim();
  for (const e of body.entries) if (!e.id) delete e.id;
  const saved = await act(() => api.put(`/api/v1/playlists/${encodeURIComponent(d.value.id)}`, body), `Saved playlist ${body.name}.`);
  if (saved) {
    // The player gives new entries their ids.
    const value = { name: saved.name || "", loop: !!saved.loop, entries: (saved.entries || []).map(entryOf) };
    drafts.playlist = { id: d.value.id, value, saved: canon(value) };
    refreshLists();
  }
}
async function remove() {
  if (!confirm(`Delete playlist "${d.value.value.name}"? The clips stay in the library.`)) return;
  if (await act(() => api.del(`/api/v1/playlists/${encodeURIComponent(d.value.id)}`))) {
    drafts.playlist = null;
    await refreshLists();
    if (live.playlists.length) load(live.playlists[0].id);
  }
}
const revert = () => load(d.value.id);
function cue(takeNow) {
  const pl = live.playlists.find((p) => p.id === d.value.id);
  if (pl && selectedOutput.value) cuePlaylist(selectedOutput.value.index, pl, 0, takeNow ? { take: true } : {});
}
</script>

<template>
  <div class="toolbar">
    <OutputPicker />
    <span class="muted small">Lengths at {{ format }}. Cue sends the playlist to this output.</span>
  </div>
  <div class="split">
    <div class="panel">
      <h3>Playlists</h3>
      <button v-for="p in live.playlists" :key="p.id" class="listbtn" :class="{ active: p.id === d?.id }" :aria-pressed="p.id === d?.id" @click="load(p.id)">
        <strong>{{ p.name || "(no name)" }}</strong>
        <span v-for="o in onAirOn(p.id)" :key="o.index" class="pill ok" style="margin-left: 0.4rem">{{ o.label }}</span>
        <div class="meta">{{ (p.entries || []).length }} entries{{ p.loop ? " · loops" : "" }}</div>
      </button>
      <div v-if="!live.playlists.length" class="empty">No playlists yet.</div>
      <form class="row tight" style="margin-top: 0.8rem" @submit.prevent="create">
        <input v-model="newName" class="grow" placeholder="New playlist name" aria-label="New playlist name" />
        <button class="btn" type="submit" :disabled="!newName.trim()">Create</button>
      </form>
    </div>

    <div v-if="d" class="panel">
      <h3>
        Edit playlist
        <span class="spacer"></span>
        <span v-if="dirty" class="pill warn">not saved</span>
        <span v-for="o in onAirOn(d.id)" :key="o.index" class="pill ok">on {{ o.label }}</span>
      </h3>
      <div class="row">
        <div>
          <label for="pl-name">Name</label>
          <input id="pl-name" v-model="d.value.name" maxlength="120" />
        </div>
        <label class="check" style="flex: 0 0 auto; margin-bottom: 0.5rem" title="after the last entry the first one follows">
          <input v-model="d.value.loop" type="checkbox" /> Loop the playlist
        </label>
      </div>
      <div class="table-scroll">
        <table class="rundown-table" style="margin-top: 0.8rem">
          <thead>
            <tr>
              <th class="num">#</th>
              <th>Clip</th>
              <th title="seconds from the clip's start">In (s)</th>
              <th title="seconds from the clip's start; 0: its end">Out (s)</th>
              <th class="num">Length</th>
              <th title="what the entry does at its end">At end</th>
              <th title="the transition into this entry">Into it</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="(e, i) in d.value.entries" :key="e.id || `new-${i}`" :class="{ dim: !itemsById[e.item_id] }">
              <td class="num">{{ i + 1 }}</td>
              <td>
                <template v-if="itemsById[e.item_id]">
                  {{ itemsById[e.item_id].name }} <span class="badge">{{ itemsById[e.item_id].type }}</span>
                  <div class="desc" v-if="itemsById[e.item_id].type !== 'still'">clip {{ fmtClock(itemSeconds(itemsById[e.item_id]), fps) }}</div>
                </template>
                <span v-else class="muted">{{ e.item_id }} (deleted, skipped)</span>
              </td>
              <template v-if="itemsById[e.item_id]?.type === 'still'">
                <td colspan="2">
                  <label class="inline" style="margin: 0" title="how long the still stays; 0: until the next take">
                    hold <input v-model.number="e.hold_s" type="number" min="0" step="0.5" style="width: 5rem" :aria-label="`Hold time of entry ${i + 1}`" /> s
                  </label>
                </td>
              </template>
              <template v-else>
                <td><input v-model.number="e.in_s" type="number" min="0" :step="1 / fps" style="width: 5.5rem" :aria-label="`In point of entry ${i + 1}`" /></td>
                <td><input v-model.number="e.out_s" type="number" min="0" :step="1 / fps" style="width: 5.5rem" :class="{ invalid: invalid(e) }" :aria-label="`Out point of entry ${i + 1}`" /></td>
              </template>
              <td class="num">{{ itemsById[e.item_id] ? (itemsById[e.item_id].type === "still" && !Number(e.hold_s) ? "∞" : fmtClock(seconds(e), fps)) : "–" }}</td>
              <td>
                <select v-model="e.end" style="width: 6.5rem" :aria-label="`End of entry ${i + 1}`">
                  <option v-for="x in ENDS" :key="x.value" :value="x.value" :title="x.title">{{ x.label }}</option>
                </select>
              </td>
              <td class="nowrap">
                <select v-model="e.transition" style="width: 5.2rem" :aria-label="`Transition into entry ${i + 1}`">
                  <option v-for="x in TRANSITIONS" :key="x.value" :value="x.value">{{ x.label }}</option>
                </select>
                <input
                  v-if="e.transition !== 'cut'"
                  v-model.number="e.transition_frames"
                  type="number"
                  min="1"
                  max="250"
                  style="width: 4.2rem; margin-left: 0.3rem"
                  title="frames"
                  :aria-label="`Transition frames of entry ${i + 1}`"
                />
              </td>
              <td class="actions-cell">
                <button class="btn small secondary" :disabled="i === 0" title="Move up" @click="move(i, -1)">↑</button>
                <button class="btn small secondary" :disabled="i === d.value.entries.length - 1" title="Move down" @click="move(i, 1)">↓</button>
                <button class="btn small secondary" title="Duplicate" @click="duplicate(i)">⧉</button>
                <button class="btn small danger" title="Remove" @click="d.value.entries.splice(i, 1)">✕</button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-if="!d.value.entries.length" class="empty">No clips yet: add some below.</div>
      <div class="row tight" style="margin-top: 0.7rem">
        <select v-model="addItem" class="grow" aria-label="Clip to add">
          <option value="">Add a clip…</option>
          <option v-for="i in playable" :key="i.id" :value="i.id">{{ i.name }} ({{ i.type }})</option>
        </select>
        <button class="btn secondary" :disabled="!addItem" @click="add">Add</button>
      </div>
      <p class="note">
        Total {{ fmtClock(total, fps) }}{{ endless ? " (then it holds, loops or repeats)" : "" }}. A mix or dip into an entry starts that many frames before the end of
        the one before it. The next entry is always loaded ahead, so cuts are frame-accurate.
      </p>
      <div class="actions">
        <button class="btn danger" @click="remove">Delete</button>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="revert">Revert</button>
        <button class="btn" :disabled="!dirty || !d.value.name.trim() || d.value.entries.some(invalid)" @click="save">Save</button>
        <button class="btn secondary" :disabled="dirty || !d.value.entries.length || !selectedOutput" :title="dirty ? 'save first' : 'cue the first entry'" @click="cue(false)">
          Cue on {{ selectedOutput?.label }}
        </button>
        <button class="btn" :disabled="dirty || !d.value.entries.length || !selectedOutput" :title="dirty ? 'save first' : 'cue and take'" @click="cue(true)">
          Play on {{ selectedOutput?.label }}
        </button>
      </div>
    </div>
    <div v-else class="panel empty">Create a playlist on the left.</div>
  </div>
</template>
