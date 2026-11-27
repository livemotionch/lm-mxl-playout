<script setup>
// Shotbox: one button per library clip and per playlist. A click cues it on the output (parked
// on air while nothing plays, else next), a double click cues and takes it. The clip on air is
// red, the next one yellow. "At end" is what a single clip does at its end.
import { computed, ref } from "vue";
import Segmented from "./Segmented.vue";
import { formatFps } from "../api.js";
import { cueItem, cuePlaylist, fmtClock, itemFrames, live, operator, saveOperator, take } from "../store.js";

const props = defineProps({
  output: { type: Object, required: true },
  compact: { type: Boolean, default: false },
});

const query = ref("");
const kind = ref("all");
const KINDS = [
  { value: "all", label: "All" },
  { value: "clips", label: "Clips" },
  { value: "playlists", label: "Playlists" },
];
const ENDS = [
  { value: "stop", label: "Black", title: "a single clip goes to black at its end" },
  { value: "hold", label: "Hold", title: "the last frame stays until the next take" },
  { value: "loop", label: "Loop", title: "it loops until the next take" },
];

const fps = computed(() => formatFps(props.output.format));
const matches = (text, tags = []) => {
  const q = query.value.trim().toLowerCase();
  return !q || text.toLowerCase().includes(q) || tags.some((t) => t.toLowerCase().includes(q));
};
const clips = computed(() =>
  kind.value === "playlists" ? [] : live.library.filter((i) => ["video", "still", "sprite"].includes(i.type) && matches(i.name || "", i.tags || [])),
);
const lists = computed(() => (kind.value === "clips" ? [] : live.playlists.filter((p) => matches(p.name || ""))));

const po = computed(() => props.output.playout || {});
function stateOfItem(id) {
  if (po.value.program?.item_id === id && !po.value.program.playlist_id) return po.value.state === "parked" ? "cued" : "air";
  if (po.value.incoming?.item_id === id && !po.value.incoming.playlist_id) return "air";
  if (po.value.next?.item_id === id && !po.value.next.playlist_id) return "next";
  return "";
}
function stateOfList(id) {
  if (po.value.program?.playlist_id === id || po.value.incoming?.playlist_id === id) return po.value.state === "parked" ? "cued" : "air";
  if (po.value.next?.playlist_id === id) return "next";
  return "";
}
function length(item) {
  if (item.type === "still") return "still";
  const f = itemFrames(item, props.output.format);
  return f ? fmtClock(f / fps.value, fps.value) : item.conversions?.[props.output.format]?.status || "converting";
}
const ready = (item) => item.conversions?.[props.output.format]?.status === "ready";
// A click cues at once; a double click then takes it (the second click cued it again: no change).
const cueClip = (item) => cueItem(props.output.index, item);
const cueList = (pl) => cuePlaylist(props.output.index, pl, 0);
const takeNow = () => take(props.output.index);
function setEnd(v) {
  operator.clipEnd = v;
  saveOperator();
}
</script>

<template>
  <div class="shotbox" :class="{ compact }">
    <div class="row tight shotbox-filter">
      <input v-model="query" class="grow" type="search" placeholder="Search name or tag" aria-label="Search the shotbox" />
      <Segmented :options="KINDS" :model-value="kind" label="Show" @update:model-value="(v) => (kind = v)" />
      <span v-if="!compact" class="caption">At end</span>
      <Segmented v-if="!compact" :options="ENDS" :model-value="operator.clipEnd" label="A single clip at its end" @update:model-value="setEnd" />
    </div>
    <div class="shots">
      <button
        v-for="p in lists"
        :key="p.id"
        class="shot list"
        :class="stateOfList(p.id)"
        :disabled="!(p.entries || []).length"
        :title="`Playlist ${p.name}: click cues it, double click plays it`"
        @click="cueList(p)"
        @dblclick="takeNow"
      >
        <span class="shot-pic list-pic">≡ {{ (p.entries || []).length }}</span>
        <span class="shot-name">{{ p.name || "(no name)" }}</span>
        <span class="shot-meta">playlist{{ p.loop ? " · loops" : "" }}</span>
      </button>
      <button
        v-for="i in clips"
        :key="i.id"
        class="shot"
        :class="[stateOfItem(i.id), { unready: !ready(i) }]"
        :disabled="!ready(i)"
        :title="`${i.name}: click cues it, double click plays it`"
        @click="cueClip(i)"
        @dblclick="takeNow"
      >
        <span class="shot-pic"><img :src="`/api/v1/library/${i.id}/thumbnail`" alt="" loading="lazy" @error="(e) => (e.target.style.visibility = 'hidden')" /></span>
        <span class="shot-name">{{ i.name }}</span>
        <span class="shot-meta">{{ length(i) }}</span>
      </button>
      <div v-if="!lists.length && !clips.length" class="empty">{{ live.library.length ? "Nothing matches." : "No clips yet: upload some in the Library." }}</div>
    </div>
  </div>
</template>
