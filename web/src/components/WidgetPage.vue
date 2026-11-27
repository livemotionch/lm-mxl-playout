<script setup>
// Operator-screen widgets (SPECIFICATION.md §9.1), each one output's, without the app around them,
// on this player's own API (same origin). `output` counts from 1 (Out 1 is API index 0).
//   /widget/transport?output=<n>  on air, next, countdowns, Take / Pause / Black / Fade (a test
//                                 source: its play / pause / stop transport)
//   /widget/shotbox?output=<n>    clips and playlists to cue (click) or play (double click)
//   /widget/rundown?output=<n>[&playlist=<id>]  the playlist on air (or that one) to cue from
// [&theme=dark|light|transparent]. It posts {type: "widget-ready"} and {type: "widget-size", w, h}
// to the page that frames it.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import PlayoutControls from "./PlayoutControls.vue";
import RundownPanel from "./RundownPanel.vue";
import ShotboxPanel from "./ShotboxPanel.vue";
import Transport from "./Transport.vue";
import { live, sourceText, startLive, stopLive } from "../store.js";

const params = new URLSearchParams(location.search);
const widget = location.pathname.replace(/^\/widget\//, "").replace(/\/$/, "");
const index = Number(params.get("output")) - 1;
const playlistId = params.get("playlist") || "";
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its color scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const o = computed(() => live.outputs.find((x) => x.index === index));
const isPlayout = computed(() => o.value?.source?.type === "playout");
const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  o,
  (x) => {
    if (!x || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <template v-if="o">
      <div class="widget-head">
        <span class="name">{{ o.label }}</span>
        <span class="fmt">{{ o.format }}</span>
        <span class="spacer"></span>
        <span class="state-tag" :class="o.transport">{{ (isPlayout ? o.playout?.state : o.transport)?.toUpperCase() }}</span>
      </div>
      <template v-if="widget === 'transport'">
        <PlayoutControls v-if="isPlayout" :output="o" compact />
        <template v-else>
          <div class="srcline" :title="sourceText(o)">{{ sourceText(o) }}</div>
          <Transport :output="o" />
        </template>
      </template>
      <ShotboxPanel v-else-if="widget === 'shotbox'" :output="o" compact />
      <RundownPanel v-else-if="widget === 'rundown'" :output="o" :playlist-id="playlistId" compact />
    </template>
    <div v-else class="empty">{{ live.error || `Waiting for output ${index + 1}…` }}</div>
    <div v-if="live.actionError" class="widget-error" title="Dismiss" @click="live.actionError = ''">{{ live.actionError }}</div>
  </div>
</template>
