# VIVISECT

> Repository name: **faultline**. The product and build target are named **Vivisect**; references to these names in paths and artifacts refer to this project.

**Sample butchery VST.** The incoming signal is treated as a *specimen*, not a
signal. A rolling live buffer (4 / 8 / 16 bars) is continuously analysed and a
rack of six parallel "surgeons" cut, granulate, rearrange, corrupt and reinject
slices in real time. A single **Chaos** axis sweeps from grid-locked repeatable
destruction (trip hop / stutter) to no-two-bars-alike chaos (glitch /
industrial). **Rhythmic Gravity** independently controls grid snap + swing.

Medical / anatomical UI: dark, high-contrast, sharp edges, waveform rendered as
a heart monitor, chaos shown as a pulse. It should look like it might hurt you.

---

## Build

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE is fetched automatically.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Pre-built packages are produced by the **Release builds** GitHub Actions
workflow: Windows x64 (VST3, CLAP, Standalone), Linux x64 (VST3, CLAP, LV2,
Standalone) and macOS arm64 plus macOS x86_64 (VST3, CLAP, AU, Standalone).
Tests run once, the per-platform build jobs do not repeat them, and publishing
needs both. Beta builds are tagged `v<version>-beta.<N>` (N comes from
`release-beta.txt`) and published as GitHub pre-releases. The macOS builds are
ad-hoc signed, not notarized; [INSTALL.md](INSTALL.md) explains where each file
goes and the Gatekeeper step.

Outputs (under `build/Vivisect_artefacts/Release/`):

* `VST3/Vivisect.vst3`
* `CLAP/Vivisect.clap`
* `Standalone/Vivisect.exe`
* on macOS also `AU/Vivisect.component` and `Standalone/Vivisect.app`

Override the JUCE version with `-DJUCE_TAG=8.0.x`, or point at a local copy with
`-DVIVISECT_SYSTEM_JUCE=ON` + `-DJUCE_DIR=...`. AAX can be added to
`FORMATS` in `CMakeLists.txt`. AU is already enabled on macOS, with an 11.0
deployment target; pass `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64` to build
one architecture.

### CLAP

JUCE has no CLAP wrapper of its own, so the CLAP build goes through
[clap-juce-extensions][cje], fetched automatically on first configure. It wraps
the *same* shared-code target as the VST3, so the CLAP is the same plugin —
same DSP, same editor, same state — not a parallel implementation that can
drift out of step.

```
cmake --build build --config Release --target Vivisect_CLAP
```

Disable it (and skip the extra network fetch) with `-DVIVISECT_BUILD_CLAP=OFF`.

Event resolution is left at 0 samples, i.e. no sub-block automation splitting.
That is deliberate: Vivisect reads its parameters once per block, so splitting
blocks would cost CPU without changing a single sample of output.

[cje]: https://github.com/free-audio/clap-juce-extensions

### Linux

Linux builds VST3, **LV2** and Standalone (LV2 is what Ardour, Qtractor and
Zrythm reach for first), plus CLAP as above. Install the JUCE dependencies
first — on Debian/Ubuntu:

```
sudo apt install build-essential cmake ninja-build pkg-config \
     libasound2-dev libfreetype-dev libfontconfig1-dev \
     libx11-dev libxext-dev libxrandr-dev libxinerama-dev \
     libxcursor-dev libxcomposite-dev libgl1-mesa-dev
```

`libcurl` and `webkit2gtk` are *not* needed: the build sets `JUCE_USE_CURL=0`
and `JUCE_WEB_BROWSER=0`.

```
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

Outputs land under `build-linux/Vivisect_artefacts/Release/`. Install per-user
by copying `VST3/Vivisect.vst3` to `~/.vst3/`, `CLAP/Vivisect.clap` to
`~/.clap/`, and `LV2/Vivisect.lv2` to `~/.lv2/`.

---

## Signal flow

```
input ──▶ input trim ──▶ SPECIMEN BUFFER (rolling, bar-synced, freezable)
                              │
             ┌────────────────┼─────────────────┐
             ▼                ▼                 ▼
     transient onsets   spectral frames    A / B sample slots
     (energy flux)      (centroid, flatness, rms)
             └────────────────┬─────────────────┘
                              ▼
                     SLICE SCHEDULER  ◀── Chaos field + Rhythmic Gravity
                     picks WHEN and WHICH slice per surgeon
                     (grid / nearest-transient / brightest / loudest / most-tonal)
                              ▼
        ┌───────┬───────┬─────┴────┬────────┬────────┐
     STUTTER GRANULAR REVERSE   CORRUPT  REORDER  FREEZE     ◀── 6 parallel surgeons
        └───────┴───────┴────┬─────┴────────┴────────┘
                             ▼
                   sum (per-surgeon mix) + feedback routing
                             ▼
                   reinject into specimen  ◀── Reinject / Chaos feedback
                             ▼
              dry/wet (dry ducks under surgeon activity)
                             ▼
                SCAR ──▶ TOURNIQUET ──▶ CLOT ──▶ FLATLINE (hidden)
                             ▼
                        output trim ──▶ out
```

Final order after the rack: `rack -> dry/wet -> SCAR -> TOURNIQUET -> CLOT ->
FLATLINE -> output trim`. Every post-rack stage is skipped, and its state reset,
when it is off, so bypass is bit-exact.

## The surgeons

| # | Surgeon  | Icon         | P1     | P2      | P3     | Notes |
|---|----------|--------------|--------|---------|--------|-------|
| 1 | STUTTER  | scalpel      | LEN    | RATCHET | DRIFT  | beat-repeat, ratchet subdivision, micro pitch-drift per repeat |
| 2 | GRANULAR | syringe      | SIZE   | DENSITY | SPRAY  | grain cloud pulled straight from the buffer, position spread scales with chaos |
| 3 | REVERSE  | forceps      | LEN    | BLOOM   | TAIL   | reversed slice, amplitude bloom (sucks inward), reverb tail |
| 4 | CORRUPT  | bone saw     | BITS   | RATE    | MODE   | bitcrush + SR reduction always; MODE morphs dropouts → digital clicks → tape wow + azimuth error |
| 5 | REORDER  | bandage      | SLICES | PERM    | REPEAT | slice a region, permute (retrograde / palindrome / Fibonacci / Euclidean), replay |
| 6 | FREEZE   | defibrillator| OFFSET | LENGTH  | BLUR   | phase-vocoder spectral freeze; BLUR randomises bin phase |

Each surgeon has **On**, **Mix**, **Prob** (activity probability), **Route In**
(feed another surgeon's output back through the specimen for feedback stacks) and
**Route Amt** (0 to 1, default 0.5: how much of the chosen source surgeon is fed
through this one). Routing loops are limited, so any cycle stays bounded.

## Master controls

* **Chaos** — one automatable axis. ORDER: grid-locked, repeatable. MIDDLE:
  jittered lengths, skipped triggers, occasional wrong slices. CHAOS: randomised
  positions, open feedback, grid ignored.
* **Rhythmic Gravity** — `Gravity Pull` drags slice positions toward the grid;
  `Snap` = 1/16 · 1/32 · 1/8T · 1/8. · Free; `Swing` pushes odd steps.
* **Trigger Rate** — global surgeon activity.
* **Reinject** — how much of the butchered output is written back into the specimen.
* **Analysis Inform** — probability that a slice is chosen by analysis
  ("brightest 50 ms", "nearest transient") instead of by the grid.
* **Source** — Live / Sample A / Sample B / Morph A/B.
* **Panic Freeze** — freezes the buffer and holds it on the Freeze surgeon.
* **Decay Over Time** — progressively drives chaos + corruption to maximum across
  `Decay T` seconds.
* **SCAR** — optional post-rack texture stage. Drive adds bounded soft saturation
  plus a torn transient edge; Mix blends it against the untreated master signal.
* **DOSE**: one knob that pushes the whole rack harder (see below).

## TOURNIQUET

Post-rack resonant filter (a stereo TPT state-variable filter) that clamps the
sound off. Modes: Low-pass, Band-pass, High-pass, Notch. **Cutoff** 20 Hz to
20 kHz, **Resonance** (Q 0.5 to 18, it rings hard but stays stable), **Env**
(-1 to 1: the input level moves the cutoff by up to four octaves) and **Mix**.
Mix 0 or Off is an exact bypass. Mod destinations: TQ Cutoff, TQ Resonance.

## CLOT

Post-rack tempo-synced stereo delay. **Time** is 1/16, 1/8T, 1/8, 1/8., 1/4,
1/4., 1/2, 1/1 or Free (**Free** sets 20 to 2000 ms). It follows the host tempo
and uses 120 BPM when there is none. **Feedback** goes to 0.95 and is soft
clipped inside the loop, so it stays bounded. **Tone** damps the loop (200 Hz to
16 kHz), **Spread** goes from mono to full ping-pong, **Mix** blends. Time
changes glide over about 60 ms. Mod destinations: Clot Feedback, Clot Mix.
Turning Clot off clears the delay line.

## Undo / Redo

The header UNDO and REDO buttons (Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl+Y) step
through your last 100 changes to the sound controls. One knob drag is one step,
and so are RANDOM, MUTATE, RESET, a preset load, an A/B swap and a history
rewind. Host automation is not recorded. Settings, locks and MIDI maps are never
undone, and the stack is not saved with the session.

## Scale Lock

Keeps pitch scatter in tune. With it on, STUTTER drift, GRANULAR grain pitch and
REVERSE tail pitch pick a degree of the chosen **Root** and **Scale** (Chromatic,
Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Harmonic Minor, Major and
Minor Pentatonic, Blues, Whole Tone) within **Range** (0 to plus or minus 12
semitones). Chaos widens the pick but never leaves the scale. Off behaves
exactly like 0.9.3.

## Capture and drag-out

CAPTURE records the final output (after every effect) for 1, 2, 4 or 8 bars.
While the host plays it starts on the next bar line; standalone or stopped it
starts at once. Takes are written by a background thread as 24-bit WAV files
(`Vivisect_<date-time>_<bpm>bpm_<bars>bar.wav`) into a Captures folder, and the
last eight show in a shelf. Drag a take into the DAW or file manager, or onto
Sample A or B. Right-click a take to open its folder, rename or delete it.
CANCEL discards a take in progress.

## DOSE and Simple view

**Dose** (0 to 1, default 0) pushes the rack harder with one control: Chaos,
Trigger Rate, Reinject, Corrupt Bits (crunchier) and SCAR Drive go up, and
Gravity Pull loosens. Dose 0 changes nothing at all. **Simple view** (header
button) hides the mod matrix, routing, sidechain and effect detail panels and
keeps Dose, Chaos, Gravity, Dry/Wet, surgeon On and Mix, source, presets and
RANDOM / MUTATE / UNDO / REDO / CAPTURE. Hidden controls keep working and
nothing is reset when you switch. It is a user setting: off for existing users,
on for a fresh install. A dismissable first-run hint explains Dose, RANDOM,
MUTATE and Simple view.

## RANDOM, MUTATE and locks

**RANDOM** creates a new sound. The first press starts from the current settings;
later presses reset unlocked sound controls to their defaults before randomizing.
Input/output trim, buffer size, source selection, MIDI mode and the hidden
FLATLINE controls are left alone. If every surgeon switch is locked off, RANDOM
keeps them off and Diagnostics explains why the effect is silent.

**MUTATE** makes a nearby variation instead of replacing the sound. Set
**MUTATE AMT** low for subtle changes or high for larger changes. It preserves
infrastructure settings, loaded samples and user settings.

Right-click a control and choose **Lock for Randomize / Mutate** to protect its
current value. The same menu can clear every lock. RESET still restores all
parameter defaults. A/B snapshots, presets and history rewind switch sound
settings while preserving the current tooltips, MIDI mappings and locks.

## Mod matrix

4 slots. Sources: LFO 1, LFO 2 (rate + shape), Env Follow, Macro 1, Macro 2,
Random Walk. Destinations: Chaos, Gravity Pull, Swing, Trigger Rate, Reinject,
Morph, Stutter Drift, Granular Density, Granular Spray, Corrupt Bits, Corrupt
Rate, Freeze Blur, TQ Cutoff, TQ Resonance, Clot Feedback, Clot Mix.

## Live / MIDI

* Notes **C3–F3** (MIDI note numbers 48–53) trigger surgeons 1–6.
* **CC20** Macro 1 · **CC21** Macro 2 · **CC22** Chaos · **CC23** Trigger Rate.
* **MIDI Mode** suppresses the auto-scheduler so the plugin is a pure
  performance instrument.

## Sidechain

Enable the sidechain input. `SC to Main`: transients on the sidechain trigger
glitches on the main. `Main to B`: transients on the main trigger surgeons that
read from Sample B.

## History rewind

The bottom bar records sound settings at 2 Hz for 60 s. Drag it to rewind the
sound while global user settings remain unchanged. **Freeze / Save** writes the current state as a user preset.

## Factory vibes

Shipped as characters, not slots: `Clean Specimen`, `Late 90s Bristol`,
`Berlin Basement`, `Warp 1995`, `Modern Hyperpop`, `SOPHIE-adjacent`.

Eight more, added in 0.10.0, show off the newer stages and are level-matched to
peak under about -1 dBFS on the test signal: `Tourniquet Sweep`, `Clot Dub
Smear`, `Minor Key Shrapnel`, `Dose Ramp`, `Feedback Ward`, `Notch Ghost`,
`Telephone Gate`, `Whole Tone Haze`. Presets older than 0.10.0 load the new
parameters at their defaults.

---

## Source map

| File | Contents |
|------|----------|
| `Source/Parameters.h`     | all APVTS parameter ids + layout + labels |
| `Source/Dsp.h/.cpp`       | SpecimenBuffer, TransientDetector, SpectralAnalyzer, Chaos field, gravity, the 6 surgeons, SurgeonRack, SliceScheduler, ModMatrix |
| `Source/Effects.h`        | TOURNIQUET filter, CLOT delay, FLATLINE |
| `Source/Musical.h`        | ScaleQuantizer, DOSE mapping helpers |
| `Source/Capture.h`        | capture ring, bar alignment, background WAV writer |
| `Source/PluginProcessor.*`| wiring, musical clock, MIDI, sidechain, presets, undo stack, capture control, sample loading, history, monitor snapshot |
| `Source/Ui.h/.cpp`        | LookAndFeel, heart-monitor display, chaos pulse, surgeon strips, icons, history bar, Help, Options, Debug |
| `Source/PanelsEffects.*`  | TOURNIQUET and CLOT panels |
| `Source/PanelsMusical.*`  | Scale Lock, DOSE, Simple view button, first-run hint |
| `Source/PanelsCapture.*`  | capture button, shelf and drag-out |
| `Source/PluginEditor.*`   | layout |
| `Tests/`                  | headless harness: `TestMain.cpp` plus the `Tests*.inc` suites |

## Status

Build order from the brief. Everything on it is done for the 0.10.0 beta:

1. specimen buffer + transient/spectral analysis: **done**
2. STUTTER end-to-end: **done**
3. grid / gravity / chaos on that surgeon: **done**
4. GRANULAR + REVERSE: **done**
5. surgeon routing / feedback: **done**. Route In picks the source and Route Amt
   sets how much of it is fed through. A full N x N patch matrix was rejected on
   purpose: it would add 30 more automatable parameters and unbounded feedback
   paths for no audible gain over Route In + Reinject, so it is not planned.
6. CORRUPT / REORDER / FREEZE: **done**. FREEZE output level is held within
   +-1.5 dB of the frozen specimen for noise, sine and drum material at every
   BLUR setting at 44.1, 48 and 96 kHz (worst measured case about 0.5 dB), with
   no edge spikes. A regression test guards it.
7. MIDI trigger mode: **done**
8. sidechain + B buffer: **done** (basic onset detection)
9. character presets: **done** (6 vibes + 8 for the new stages)
10. UI polish + marketing: **done** for the beta
11. 0.10.0: TOURNIQUET, CLOT, Undo / Redo, Scale Lock, Capture, DOSE + Simple
    view, Route Amt, macOS builds incl. AU: **done**

`include.md` asks for MIDI import/export. Vivisect is an effect, not a
sequencer or instrument, so that item is intentionally N/A. The "special
function" export is the specimen WAV export and Capture instead.

Current audit notes: the UI monitor reads DSP state without a lock (display only,
benign). FREEZE now uses the correct constant gain compensation for its 75%-overlapped
Hann synthesis windows, avoiding both the old attenuation and edge spikes. Random Walk advances on a fixed internal clock so its
behaviour is independent of host buffer size. Reorder's Euclidean mode uses a
maximally-even Bjorklund-style pattern instead of front-loading pulse slices.
The specimen exporter supports 16/24/32-bit PCM WAV and reports filename, folder,
duration and selected quality.

0.9.3 release audit: BUFFER (4/8/16 bars) now limits how far back the surgeons
reach (previously it only changed the display, and slices could come from up to
45 s back); CORRUPT clicks scale with the slice so silence stays silent; MIDI CC
moves are applied on the audio thread but reported to the host from the message
thread (required by CLAP); loaded samples are re-loaded with the host session;
the crash handler is removed cleanly when the plug-in closes; the editor no
longer crashes if it opens before audio starts; and the window is resizable
(50-150 %, opens shrunk to fit small screens). Verified with pluginval
(strictness 10, VST3), clap-validator (CLAP) and an ASan/UBSan debug run of the
test harness.


## Plain-language overview

New to the project? Start with the [ELI5 guide](ELI5.md).
