<script setup>
// The operator's on-air panel of one output: what is on air (state, countdown to the clip's end
// and the playlist's end, position with seek), what comes next (and whether it is loaded), and
// Take, Pause/Resume, Stop and Fade with the take's transition. `compact` is the widget's layout.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { formatFps } from "../api.js";
import { fmtClock, operator, playout, saveOperator, take } from "../store.js";

const props = defineProps({
  output: { type: Object, required: true },
  compact: { type: Boolean, default: false },
});

const po = computed(() => props.output.playout || { state: "idle" });
const prog = computed(() => po.value.program);
const next = computed(() => po.value.next);
const fps = computed(() => formatFps(props.output.format));
const state = computed(() => po.value.state);

const STATE = {
  idle: { text: "BLACK", kind: "stop" },
  parked: { text: "CUED", kind: "cued" },
  playing: { text: "ON AIR", kind: "air" },
  transition: { text: "MIX", kind: "air" },
  fading: { text: "FADE", kind: "air" },
  paused: { text: "PAUSED", kind: "pause" },
  ended: { text: "HOLD", kind: "pause" },
  waiting: { text: "LOADING NEXT", kind: "pause" },
};
const tag = computed(() => STATE[state.value] || STATE.idle);

const remaining = computed(() => prog.value?.remaining_s ?? 0);
const urgency = computed(() => {
  if (!prog.value || prog.value.endless || !["playing", "transition"].includes(state.value)) return "";
  if (remaining.value <= 5) return "bad";
  if (remaining.value <= 15) return "warn";
  return "";
});
const progress = computed(() => (prog.value?.frames > 0 ? Math.min(1, prog.value.position / prog.value.frames) : 0));
const playlist = computed(() => po.value.playlist);

const END_TEXT = { next: "then next", hold: "then hold", loop: "loops", stop: "then black" };
const TRANSITIONS = [
  { value: "auto", label: "Entry", title: "the transition the cued entry has" },
  { value: "cut", label: "Cut" },
  { value: "mix", label: "Mix" },
  { value: "dip", label: "Dip", title: "through black" },
];

const canTake = computed(() => !!next.value || state.value === "parked" || state.value === "paused");
const takeLabel = computed(() => (state.value === "parked" || (state.value === "paused" && !next.value) ? "Play" : "Take"));
const running = computed(() => ["playing", "transition", "fading", "waiting"].includes(state.value));

function onBar(event) {
  if (!prog.value?.frames) return;
  const rect = event.currentTarget.getBoundingClientRect();
  const frac = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
  playout(props.output.index, { action: "seek", frame: Math.min(prog.value.frames - 1, Math.round(frac * prog.value.frames)) });
}
const pauseOrResume = () => playout(props.output.index, { action: running.value ? "pause" : "resume" });
const stop = () => playout(props.output.index, { action: "stop" });
const fade = () => playout(props.output.index, { action: "stop", transition: "mix", transition_frames: Math.max(1, Number(operator.frames) || 12) });
const clearNext = () => playout(props.output.index, { action: "clear" });
function setTransition(v) {
  operator.transition = v;
  saveOperator();
}
function setFrames(e) {
  operator.frames = Math.max(1, Math.min(250, Number(e.target.value) || 12));
  saveOperator();
}
function transitionText(l) {
  if (!l || l.transition === "cut" || !l.transition_frames) return "cut";
  return `${l.transition} ${l.transition_frames} f`;
}
</script>

<template>
  <div class="onair" :class="{ compact }">
    <div class="onair-head">
      <span class="air-tag" :class="tag.kind">{{ tag.text }}</span>
      <span class="name" :title="prog?.name">{{ prog?.name || "Nothing on air" }}</span>
      <span class="spacer"></span>
      <span class="countdown" :class="urgency" :title="prog?.endless ? 'holds or loops until the next take' : 'time left in the clip'">
        {{ prog ? (prog.endless || prog.end === "loop" ? "∞" : `−${fmtClock(remaining, fps)}`) : "" }}
      </span>
    </div>
    <div v-if="prog" class="onair-meta">
      <span v-if="prog.playlist_name">{{ prog.playlist_name }} · {{ prog.entry + 1 }}/{{ prog.playlist_count }}</span>
      <span>{{ END_TEXT[prog.end] || prog.end }}</span>
      <span v-if="!prog.ready" class="warn">loading…</span>
      <span v-if="output.media_error" class="bad">{{ output.media_error }}</span>
      <span v-if="playlist && !compact" class="spacer"></span>
      <span v-if="playlist" :title="'time left in the playlist'">playlist {{ playlist.endless ? "∞" : `−${fmtClock(playlist.remaining_s, fps)}` }}</span>
    </div>
    <div v-if="prog && prog.frames > 1" class="position">
      <span>{{ fmtClock(prog.position / fps, fps) }}</span>
      <div class="bar seekable" title="Click to seek" role="progressbar" :aria-valuenow="prog.position" aria-valuemin="0" :aria-valuemax="prog.frames" @click="onBar">
        <div :style="{ width: `${progress * 100}%` }" :class="urgency"></div>
      </div>
      <span>{{ fmtClock(prog.frames / fps, fps) }}</span>
    </div>
    <div v-if="po.transition" class="position">
      <span>{{ po.transition.to_black ? "fade" : po.transition.kind }}</span>
      <div class="bar"><div :style="{ width: `${po.transition.progress * 100}%` }"></div></div>
      <span>{{ po.transition.frames }} f</span>
    </div>

    <div class="next-line">
      <span class="caption">Next</span>
      <template v-if="next">
        <span class="name" :title="next.name">{{ next.name }}</span>
        <span class="pill" :class="next.ready ? 'ok' : 'warn'">{{ next.ready ? "ready" : "loading" }}</span>
        <span v-if="po.auto_take && prog?.end !== 'loop'" class="pill neutral" title="Auto take: it goes on air when the clip on air is done">auto</span>
        <span class="muted small">{{ transitionText(next) }}<template v-if="next.frames"> · {{ fmtClock(next.frames / fps, fps) }}</template></span>
        <span class="spacer"></span>
        <button v-if="!compact" class="btn small secondary" title="Remove the next clip" @click="clearNext">Clear</button>
      </template>
      <span v-else class="muted small">{{ state === "parked" ? "the cued clip plays on Play" : "nothing cued" }}</span>
    </div>

    <div class="take-row">
      <button class="btn take" :disabled="!canTake" :title="`${takeLabel} (Space)`" @click="take(output.index)">{{ takeLabel }}</button>
      <button class="btn secondary big" :disabled="!prog || state === 'parked' || state === 'ended'" :title="running ? 'Pause (P)' : 'Resume (P)'" @click="pauseOrResume">
        {{ running ? "Pause" : "Resume" }}
      </button>
      <button class="btn secondary big" :disabled="!prog" title="Cut to black (B)" @click="stop">Black</button>
      <button class="btn secondary big" :disabled="!prog" :title="`Fade to black over ${operator.frames} frames (F)`" @click="fade">Fade</button>
    </div>
    <div class="row tight transition-row">
      <span class="caption">Take with</span>
      <Segmented :options="TRANSITIONS" :model-value="operator.transition" label="Transition of the take" @update:model-value="setTransition" />
      <label class="inline frames" title="frames of a mix, dip or fade">
        <input type="number" min="1" max="250" :value="operator.frames" aria-label="Transition frames" @change="setFrames" />
        <span class="muted small">frames</span>
      </label>
    </div>
  </div>
</template>
