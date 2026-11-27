<script setup>
// Rundown: the entries of a playlist (the one on air, or any), the entry on air in red, the next
// in yellow, the ones before dimmed. "Next" cues an entry as the next clip (the rundown goes on
// from it), "Play" cues and takes it. The times are each entry's length (in to out).
import { computed, ref, watch } from "vue";
import { formatFps } from "../api.js";
import { cuePlaylist, fmtClock, itemFrames, itemsById, live, playlistOnAir, playout, take, takeOverride } from "../store.js";

const props = defineProps({
  output: { type: Object, required: true },
  compact: { type: Boolean, default: false },
  playlistId: { type: String, default: "" },
});

const onAir = computed(() => playlistOnAir(props.output));
const chosen = ref(props.playlistId);
watch(
  () => onAir.value?.id,
  (id) => {
    if (id && !props.playlistId) chosen.value = id;
  },
  { immediate: true },
);
watch(
  () => live.playlists.length,
  () => {
    if (!chosen.value && live.playlists.length) chosen.value = live.playlists[0].id;
  },
  { immediate: true },
);
const pl = computed(() => live.playlists.find((p) => p.id === chosen.value) || null);
const fps = computed(() => formatFps(props.output.format));
const po = computed(() => props.output.playout || {});
const airEntry = computed(() => {
  const l = po.value.incoming || po.value.program;
  return l && l.playlist_id === chosen.value ? l.entry_id : "";
});
const nextEntry = computed(() => (po.value.next?.playlist_id === chosen.value ? po.value.next.entry_id : ""));
const airIndex = computed(() => (pl.value?.entries || []).findIndex((e) => e.id === airEntry.value));

function seconds(e) {
  const item = itemsById.value[e.item_id];
  if (!item) return 0;
  if (item.type === "still") return Number(e.hold_s ?? 5) || 0;
  const len = itemFrames(item, props.output.format) / fps.value;
  const out = Number(e.out_s) > 0 ? Math.min(Number(e.out_s), len) : len;
  return Math.max(0, out - (Number(e.in_s) || 0));
}
const rows = computed(() =>
  (pl.value?.entries || []).map((e, i) => ({
    e,
    i,
    item: itemsById.value[e.item_id],
    secs: seconds(e),
    state: e.id === airEntry.value ? (po.value.state === "parked" ? "cued" : "air") : e.id === nextEntry.value ? "next" : airIndex.value >= 0 && i < airIndex.value ? "done" : "",
  })),
);
const total = computed(() => rows.value.reduce((s, r) => s + r.secs, 0));
const END = { next: "→", hold: "⏸", loop: "⟲", stop: "■" };
const END_TITLE = { next: "then the next entry", hold: "holds its last frame", loop: "loops until the next take", stop: "then black" };

const isOnAir = computed(() => !!airEntry.value || !!nextEntry.value);
// "Next": on air, the entry becomes the next clip; otherwise it is cued (parked or next).
function cueEntry(r) {
  if (isOnAir.value) return playout(props.output.index, { action: "jump", playlist_id: chosen.value, entry_id: r.e.id });
  return cuePlaylist(props.output.index, pl.value, 0, { entry_id: r.e.id });
}
async function playEntry(r) {
  if (isOnAir.value) {
    if (await playout(props.output.index, { action: "jump", playlist_id: chosen.value, entry_id: r.e.id })) take(props.output.index);
    return;
  }
  cuePlaylist(props.output.index, pl.value, 0, { entry_id: r.e.id, take: true, ...takeOverride() });
}
</script>

<template>
  <div class="rundown" :class="{ compact }">
    <div class="row tight">
      <select v-model="chosen" class="grow" aria-label="Playlist">
        <option v-if="!live.playlists.length" value="">No playlists</option>
        <option v-for="p in live.playlists" :key="p.id" :value="p.id">{{ p.name || "(no name)" }}{{ p.id === onAir?.id ? " · on air" : "" }}</option>
      </select>
      <span v-if="pl" class="muted small nowrap">{{ rows.length }} entries · {{ fmtClock(total, fps) }}{{ pl.loop ? " · loops" : "" }}</span>
      <button v-if="pl && !compact" class="btn small" :disabled="!rows.length" title="Cue the first entry" @click="cuePlaylist(output.index, pl, 0)">Cue from top</button>
    </div>
    <ol class="entries">
      <li v-for="r in rows" :key="r.e.id" class="entry" :class="[r.state, { missing: !r.item }]">
        <span class="entry-no">{{ r.i + 1 }}</span>
        <span class="entry-pic"><img v-if="r.item" :src="`/api/v1/library/${r.item.id}/thumbnail`" alt="" loading="lazy" @error="(e) => (e.target.style.visibility = 'hidden')" /></span>
        <span class="entry-main">
          <span class="entry-name">{{ r.item ? r.item.name : "(deleted item, skipped)" }}</span>
          <span class="entry-meta">
            <span v-if="r.e.transition && r.e.transition !== 'cut'">{{ r.e.transition }} {{ r.e.transition_frames || 12 }} f ·</span>
            <span v-if="Number(r.e.in_s) > 0 || Number(r.e.out_s) > 0">trimmed ·</span>
            <span :title="END_TITLE[r.e.end || 'next']">{{ END[r.e.end || "next"] }} {{ r.e.end || "next" }}</span>
          </span>
        </span>
        <span class="entry-len num">{{ r.item?.type === "still" && !Number(r.e.hold_s ?? 5) ? "∞" : fmtClock(r.secs, fps) }}</span>
        <span class="entry-actions">
          <button class="btn small secondary" :disabled="!r.item" title="Cue as the next clip" @click="cueEntry(r)">Next</button>
          <button class="btn small" :disabled="!r.item" title="Cue and take now" @click="playEntry(r)">Play</button>
        </span>
      </li>
    </ol>
    <div v-if="pl && !rows.length" class="empty">This playlist is empty: add clips in Playlists.</div>
  </div>
</template>
