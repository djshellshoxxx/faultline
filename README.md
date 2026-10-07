# VIVISECT

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

Pre-built Windows and Linux packages (VST3, CLAP, Standalone; LV2 on Linux)
are produced by the **Release builds** GitHub Actions workflow on every pull
request and attached to a GitHub Release whenever a `v*` tag is pushed. See
[INSTALL.md](INSTALL.md) for where each file goes.

Outputs (under `build/Vivisect_artefacts/Release/`):

* `VST3/Vivisect.vst3`
* `CLAP/Vivisect.clap`
* `Standalone/Vivisect.exe`

Override the JUCE version with `-DJUCE_TAG=8.0.x`, or point at a local copy with
`-DVIVISECT_SYSTEM_JUCE=ON` + `-DJUCE_DIR=...`. AU/AAX targets can be added to
`FORMATS` in `CMakeLists.txt` on the relevant platform.

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
              dry/wet (dry ducks under surgeon activity) ──▶ output trim ──▶ out
```

## The surgeons

| # | Surgeon  | Icon         | P1     | P2      | P3     | Notes |
|---|----------|--------------|--------|---------|--------|-------|
| 1 | STUTTER  | scalpel      | LEN    | RATCHET | DRIFT  | beat-repeat, ratchet subdivision, micro pitch-drift per repeat |
| 2 | GRANULAR | syringe      | SIZE   | DENSITY | SPRAY  | grain cloud pulled straight from the buffer, position spread scales with chaos |
| 3 | REVERSE  | forceps      | LEN    | BLOOM   | TAIL   | reversed slice, amplitude bloom (sucks inward), reverb tail |
| 4 | CORRUPT  | bone saw     | BITS   | RATE    | MODE   | bitcrush + SR reduction always; MODE morphs dropouts → digital clicks → tape wow + azimuth error |
| 5 | REORDER  | bandage      | SLICES | PERM    | REPEAT | slice a region, permute (retrograde / palindrome / Fibonacci / Euclidean), replay |
| 6 | FREEZE   | defibrillator| OFFSET | LENGTH  | BLUR   | phase-vocoder spectral freeze; BLUR randomises bin phase |

Each surgeon has **On**, **Mix**, **Prob** (activity probability) and **Route In**
(feed another surgeon's output back through the specimen for feedback stacks).

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
Rate, Freeze Blur.

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

---

## Source map

| File | Contents |
|------|----------|
| `Source/Parameters.h`     | all APVTS parameter ids + layout + labels |
| `Source/Dsp.h/.cpp`       | SpecimenBuffer, TransientDetector, SpectralAnalyzer, Chaos field, gravity, the 6 surgeons, SurgeonRack, SliceScheduler, ModMatrix |
| `Source/PluginProcessor.*`| wiring, musical clock, MIDI, sidechain, presets, sample loading, history, monitor snapshot |
| `Source/Ui.h/.cpp`        | LookAndFeel, heart-monitor display, chaos pulse, surgeon strips, icons, history bar |
| `Source/PluginEditor.*`   | layout |

## Status

Build order from the brief, and where this tree currently sits:

1. specimen buffer + transient/spectral analysis — **done**
2. STUTTER end-to-end — **done**
3. grid / gravity / chaos on that surgeon — **done**
4. GRANULAR + REVERSE — **done**
5. surgeon routing / feedback — **done** (feedback via reinject; a full N×N
   patch matrix is future work)
6. CORRUPT / REORDER / FREEZE — **done** (FREEZE is a straightforward
   phase-vocoder freeze; overlap-add gain is approximate and wants tuning)
7. MIDI trigger mode — **done**
8. sidechain + B buffer — **done** (basic onset detection)
9. character presets — **done** (6 vibes)
10. UI polish + marketing — **first pass**

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
