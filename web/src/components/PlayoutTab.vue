<script setup>
// Playout (operator screen): the selected output's picture and on-air panel (Take, Pause, Black,
// Fade), and the shotbox or the rundown to cue from. Keys (outside text fields): Space take,
// P pause/resume, L loop the clip on air, B black, F fade to black.
import { computed, onMounted, onUnmounted, ref } from "vue";
import LivePicture from "./LivePicture.vue";
import Meters from "./Meters.vue";
import OutputPicker from "./OutputPicker.vue";
import PlayoutControls from "./PlayoutControls.vue";
import RundownPanel from "./RundownPanel.vue";
import Segmented from "./Segmented.vue";
import ShotboxPanel from "./ShotboxPanel.vue";
import { live, operator, patchOutput, playout, selectedOutput, setSource, take } from "../store.js";

const o = computed(() => selectedOutput.value);
const isPlayout = computed(() => o.value?.source?.type === "playout");

function stored() {
  try {
    return localStorage.getItem("lm-mxl-playout.pane") || "shotbox";
  } catch {
    return "shotbox";
  }
}
const pane = ref(stored());
const PANES = [
  { value: "shotbox", label: "Shotbox" },
  { value: "rundown", label: "Rundown" },
];
function setPane(v) {
  pane.value = v;
  try {
    localStorage.setItem("lm-mxl-playout.pane", v);
  } catch {
    /* this tab only */
  }
}
const CUE_MODES = [
  { value: "park", label: "Park on air", title: "a clip cued while nothing plays stands on air at its first frame (clip player for a vision mixer)" },
  { value: "background", label: "Background", title: "a cue never changes what is on air; Take puts it on air (channel to air)" },
];

function onKey(e) {
  if (!o.value || !isPlayout.value || e.ctrlKey || e.metaKey || e.altKey) return;
  const t = e.target;
  if (t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
  const st = o.value.playout?.state;
  if (e.code === "Space") {
    e.preventDefault();
    take(o.value.index);
  } else if (e.key === "p" || e.key === "P") {
    playout(o.value.index, { action: ["playing", "transition", "waiting"].includes(st) ? "pause" : "resume" });
  } else if ((e.key === "l" || e.key === "L") && o.value.playout?.program) {
    playout(o.value.index, { action: "loop", on: o.value.playout.program.end !== "loop" });
  } else if (e.key === "b" || e.key === "B") {
    playout(o.value.index, { action: "stop" });
  } else if (e.key === "f" || e.key === "F") {
    playout(o.value.index, { action: "stop", transition: "mix", transition_frames: Math.max(1, Number(operator.frames) || 12) });
  }
}
onMounted(() => window.addEventListener("keydown", onKey));
onUnmounted(() => window.removeEventListener("keydown", onKey));
</script>

<template>
  <div class="toolbar">
    <OutputPicker />
    <div v-if="o && isPlayout" class="group">
      <span class="caption">Cue</span>
      <Segmented :options="CUE_MODES" :model-value="o.playout?.cue_mode" label="Cue mode" @update:model-value="(m) => patchOutput(o.index, { cue_mode: m })" />
    </div>
    <label v-if="o && isPlayout" class="check" title="When the clip on air is done (its end, also one that holds or stops), the cued clip goes on air by itself with its transition. A looping clip loops until Take.">
      <input type="checkbox" :checked="!!o.playout?.auto_take" @change="patchOutput(o.index, { auto_take: $event.target.checked })" /> Auto take
    </label>
    <span class="spacer"></span>
    <span class="muted small keys"><kbd>Space</kbd> take · <kbd>P</kbd> pause · <kbd>L</kbd> loop · <kbd>B</kbd> black · <kbd>F</kbd> fade</span>
  </div>
  <div v-if="!o" class="empty">{{ live.error || "Waiting for the player…" }}</div>
  <div v-else-if="!isPlayout" class="panel">
    <h3>{{ o.label }} plays a test source</h3>
    <p class="note">This output plays a test source ({{ o.source?.type }}) from the Source tab. Switch it to playout to cue and take clips and playlists.</p>
    <button class="btn" @click="setSource(o.index, { type: 'playout' })">Switch {{ o.label }} to playout</button>
  </div>
  <div v-else class="playout-grid">
    <div class="playout-left">
      <div class="panel onair-panel" :class="o.playout?.state">
        <LivePicture :src="`/api/v1/outputs/${o.index}/thumbnail`" :interval="400" :alt="`${o.label} on air`">
          <span class="ov tl">{{ o.label }} · {{ o.format }}</span>
          <span v-if="o.playout?.state === 'playing' || o.playout?.state === 'transition'" class="ov tr live-dot">● ON AIR</span>
        </LivePicture>
        <div class="statusline">
          <span>{{ o.audio_channels }} ch</span>
          <Meters :peaks="live.peaks[o.index] || []" :label="`${o.label} audio levels`" style="flex: 1; height: 22px" />
        </div>
        <PlayoutControls :output="o" />
        <div class="statusline">
          <span :class="{ warn: live.underruns[o.index] }" :title="`${live.underruns[o.index] || 0} in the last minute`">underruns {{ o.underruns }}</span>
          <span :class="{ warn: o.stream_missed }" title="frames of a streamed clip that were not decoded in time">late frames {{ o.stream_missed }}</span>
          <span>decode ahead {{ o.decode_ahead }}</span>
          <span>key {{ o.key_mode }}</span>
        </div>
      </div>
    </div>
    <div class="panel playout-right">
      <h3>
        <Segmented :options="PANES" :model-value="pane" label="Cue from" @update:model-value="setPane" />
        <span class="spacer"></span>
        <span v-if="pane === 'shotbox'" class="muted small" style="text-transform: none; letter-spacing: 0">click cues · double click plays</span>
      </h3>
      <ShotboxPanel v-if="pane === 'shotbox'" :output="o" />
      <RundownPanel v-else :output="o" />
    </div>
  </div>
</template>
