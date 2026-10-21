# Vivisect 0.10.0 (beta) Feature Specifications

Normative for implementation and tests. Builds on SPECS-0.9.3.md; every 0.9.3 invariant still holds.
New sound parameters follow the same rules as existing ones: APVTS, `ParameterID{id,1}`, automatable,
reset by RESET, covered by RANDOM / MUTATE / locks / A-B / presets / history, and exercised by the
parameter-sweep and fuzz tests. User settings (anything in `VSX_SETTINGS`) are never touched by presets,
A/B, history, RANDOM or MUTATE.

## Signal chain (post-rack), final order

`rack -> dry/wet -> SCAR -> TOURNIQUET -> CLOT -> FLATLINE -> output trim -> out`

Every new stage: bypass is bit-exact (stage skipped, state reset when it turns off, like SCAR); output is
finite and bounded for any parameter combination and any input including NaN/denormal; no allocation,
locks or file I/O on the audio thread; parameters are smoothed so automation never clicks.

---

## E1. TOURNIQUET (effect: resonant filter)

Stereo TPT state-variable filter that "clamps off" the signal. Source: `Source/Effects.h` (`vsx::Tourniquet`).

| id | label | type | range / choices | default |
|----|-------|------|-----------------|---------|
| `tqOn` | Tourniquet | bool | | false |
| `tqMode` | TQ Mode | choice | Low-pass, Band-pass, High-pass, Notch | Low-pass |
| `tqCutoff` | TQ Cutoff | float | 20..20000 Hz, skew 0.3 | 1200 |
| `tqRes` | TQ Resonance | float | 0..1 (maps Q 0.5..18, self-oscillation prevented) | 0.25 |
| `tqEnv` | TQ Env | float | -1..1 envelope-follower depth on cutoff (+-4 octaves) | 0 |
| `tqMix` | TQ Mix | float | 0..1 | 1 |

Rules: cutoff clamped to 0.45*fs; envelope follower reads the stage input (attack 2 ms, release 80 ms);
resonance 1.0 stays stable (output soft-limited with a tanh in the integrator feedback). Mix 0 or On off = exact bypass.
Mod destinations added: `TQ Cutoff`, `TQ Resonance`. UI: panel with mode selector, three knobs, env knob, mix, on button.
Tests: each mode attenuates as expected (LP at 10x cutoff < -30 dB; HP mirror; BP peak near cutoff; notch dip),
stability sweep of every (cutoff,res,mode,env) grid with white noise + full-scale square, bypass bit-exactness,
sample-rate matrix 44.1/48/96/192 kHz.

## E2. CLOT (effect: tempo-synced stereo delay)

Source: `Source/Effects.h` (`vsx::Clot`). Delay lines preallocated for 4 s at the prepared rate.

| id | label | type | range / choices | default |
|----|-------|------|-----------------|---------|
| `clotOn` | Clot | bool | | false |
| `clotTime` | Clot Time | choice | 1/16, 1/8T, 1/8, 1/8., 1/4, 1/4., 1/2, 1/1, Free | 1/8 |
| `clotFree` | Clot Free | float | 20..2000 ms (used only when Time = Free) | 375 |
| `clotFeedback` | Clot Feedback | float | 0..0.95 | 0.45 |
| `clotTone` | Clot Tone | float | 200..16000 Hz damping LP in the loop | 5000 |
| `clotSpread` | Clot Spread | float | 0..1 (0 = mono delay, 1 = full ping-pong) | 0.6 |
| `clotMix` | Clot Mix | float | 0..1 | 0.3 |

Rules: synced time derives from host BPM via the existing musical clock (fallback 120 BPM); time changes are
slewed (no zipper/pitch glitch beyond a gentle tape-style glide, 60 ms); feedback loop contains a soft clipper so
it is bounded at fb 0.95 forever; denormals flushed; turning Clot off resets the line (like SCAR). `getTailLengthSeconds()` stays >= 4.
Mod destinations added: `Clot Feedback`, `Clot Mix`. Tests: echo spacing sample-accurate at 120 BPM for each
division at 44.1/48 kHz; feedback decay monotonic; stability at fb 0.95 with full-scale input for 60 s of audio;
bypass bit-exact; Free vs synced switch finite.

---

## F1. Undo / Redo (more usable)

Sound-parameter history, same scope as A/B (never VSX_SETTINGS, never locks, never samples/presets on disk).
- Header buttons UNDO / REDO and keyboard Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z and Ctrl+Y (editor focused).
- One step per completed gesture (a knob drag, however long, is one step; `beginChangeGesture`/`endChangeGesture`
  coalesce). RANDOM, MUTATE, RESET, preset load, A/B swap and history-bar rewind each add exactly one step.
- Depth 100; oldest dropped. Not saved in plug-in state; cleared on Hard Reset. Redo cleared by any new edit.
- Host automation moves are not recorded (would flood the stack); only UI/gesture edits and the actions above.
- Buttons grey out when empty; tooltips state the next action ("Undo: Chaos 0.12 -> 0.40" style, plain label OK).
- Processor API: `pushUndoSnapshot()`, `undo()`, `redo()`, `canUndo()`, `canRedo()`, `clearUndoHistory()`.
Tests: undo restores exact normalized values for all sound params; locks, tooltips, MIDI maps unchanged; redo
round trip; depth cap; gesture coalescing; RANDOM undoes in one step; no-op undo on empty stack.

## F2. Scale Lock (more musical)

Pitch-bearing events are quantized to a key so butchery stays in tune. Source: `Source/Musical.h`
(`vsx::ScaleQuantizer`, pure and unit-tested).

| id | label | type | range / choices | default |
|----|-------|------|-----------------|---------|
| `scaleOn` | Scale Lock | bool | | false |
| `scaleRoot` | Scale Root | choice | C..B | C |
| `scaleType` | Scale Type | choice | Chromatic, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Harm. Minor, Maj Pentatonic, Min Pentatonic, Blues, Whole Tone | Minor |
| `scaleRange` | Scale Range | float | 0..1 -> +-0..+-12 semitones of per-event pitch scatter | 0.35 |

Behaviour: when On, STUTTER per-repeat drift, GRANULAR per-grain pitch and REVERSE tail pitch pick a random
scale degree within `scaleRange` (chaos widens the pick, never leaves the scale) instead of free micro-drift.
Off = existing behaviour bit-exact. Quantizer: `float quantize(float semis, root, type)` returns nearest in-scale
offset; ties resolve upward; chromatic returns rounded semis. Tests: every output is in-scale for all 12x12
(root,type) pairs and a dense input sweep; Off path unchanged vs 0.9.3 reference render; ratio finite for +-12 st.

## F3. Capture and drag-out (more valuable)

Resample the butchered result straight into the DAW or back into a sample slot.
- CAPTURE button (+ length selector 1 / 2 / 4 / 8 bars, a user setting) records the final post-FX output.
  When the host transport is playing, recording starts on the next bar line; standalone/stopped starts at once.
- Audio thread only copies into a preallocated stereo ring (max 8 bars at 40 BPM, 64 s); a background thread
  writes a 24-bit WAV (`Vivisect_<yyyyMMdd-HHmmss>_<bpm>bpm_<bars>bar.wav`) into `<user data>/Captures/`.
- A shelf of the last 8 captures is shown (name, length, tiny waveform). Drag a take out to the DAW/Explorer
  (`DragAndDropContainer::performExternalDragDropOfFiles`) or onto Sample A / Sample B to load it. Right-click: open
  location, delete, rename. A completed capture shows the standard "saved" popup (location, length, name, quality).
- Never records while the processor is suspended; cancel button; recording state visible and in Debug panel.
- Processor API: `startCapture(bars)`, `cancelCapture()`, `captureState()`, `listCaptures()`.
Tests: captured length exact for bars at several BPMs; bar alignment; output file readable and finite; cancel
leaves no partial file; no allocation after prepare (asserted by capacity checks).

## F4. DOSE and Simple view (easier to use)

- `dose` (float 0..1, default 0, automatable, label "Dose"): one knob that pushes the whole rack harder.
  Applied after modulation as `x_eff = x + dose * k * (1 - x)` with k: Chaos 0.60, Trigger Rate 0.50,
  Reinject 0.25, Corrupt Bits (towards crunchier) 0.50, SCAR Drive 0.40, Gravity Pull -(0.30) towards freer
  (`x_eff = x - dose*0.30*x`). Dose 0 = identity (bit-exact vs without). RANDOM/MUTATE treat it like any param.
- SIMPLE view toggle (user setting in VSX_SETTINGS, default OFF for existing users, ON for a fresh install):
  hides the mod matrix, routing, sidechain and effect detail panels and shows Dose, Chaos, Gravity, Dry/Wet,
  surgeon On/Mix, source, presets, RANDOM/MUTATE/UNDO/REDO/CAPTURE. Every hidden control still works; nothing is
  reset when switching. Window opens at the same size; tooltips unchanged.
- A first-run hint strip (dismissable, remembered) explains Dose, RANDOM, MUTATE and Simple view.
Tests: dose mapping table, identity at 0, monotonic in dose, clamped 0..1; toggling Simple changes no parameter;
fresh-install setting default.

---

## G1. Surgeon feedback depth (completes "surgeon routing")

The single-choice `Route In` stays (back compat). Add `<surgeon>_routeAmt` (0..1, default 0.5) per surgeon:
how much of the chosen source surgeon's output is fed through this surgeon. A full N x N matrix is rejected
by design: 30 extra automatable params and unbounded feedback paths for no audible gain over Route In + Reinject;
README "future work" note replaced with this rationale. Feedback is limited so any routing cycle stays bounded.
Tests: cycle of all six routed around at amt 1 stays bounded for 60 s; amt 0 equals no routing.

## G2. FREEZE level tuning

Spectral freeze output RMS within +-1.5 dB of the frozen specimen's RMS for noise, sine and drum material at
all BLUR settings and 44.1/48/96 kHz; no edge spikes. Add regression test with those tolerances; adjust
compensation constants if they fail. Remove "approximate" wording from README when it passes.

## G3. Release, platforms, docs

- Version 0.10.0, GitHub **pre-release** ("beta") tagged `v0.10.0-beta.1`.
- Artifacts: Windows x64 (VST3, CLAP, Standalone), Linux x64 (VST3, CLAP, LV2, Standalone),
  macOS arm64 and macOS x86_64 (VST3, CLAP, AU, Standalone; ad-hoc signed, not notarized, INSTALL.md explains
  the Gatekeeper step).
- CI split into parts: `tests` (Linux + Windows, once), per-platform `build` jobs without re-running tests,
  compile cache, `publish` needs both. Tests are not repeated in build jobs.
- Docs: README (effects, features, signal flow, status), INSTALL (macOS), ELI5, CHANGELOG.md, in-plugin Help
  page text, tooltips for every new control, and factory presets updated/added to showcase the new stages.
- Compliance with `theme.md` and `include.md` re-audited (Help items, LED, data stream, header, footer version).
  `include.md` MIDI import/export is intentionally N/A for an effect; documented in README.

## Final audit checklist (must all be true before release)

1. Every id above exists in `Parameters.h`, is in the layout, has a tooltip, a label, a UI control, a mod/RANDOM/
   MUTATE/lock/preset/A-B/history path, a test, and README + Help text.
2. No orphan parameters or UI controls; no stub methods; no TODO/FIXME in Source/.
3. State round trip including all new params; old 0.9.3 sessions load with new params at defaults and sound identical.
4. Full harness green on Linux and Windows; pluginval (VST3, strictness 10) and clap-validator on the Linux build.
