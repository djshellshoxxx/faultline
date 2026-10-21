# User Guide

## What it does

Vivisect records a rolling window of what you feed it (4, 8 or 16 bars, set by **BUFFER**), keeps analysing it, and lets six surgeons pull slices out of that window. Each surgeon has its own way of cutting. The output is mixed with the input through a dry/wet balance that ducks under surgeon activity.

## Install

See [[Install]]. Windows: the VST3 folder goes in `C:\Program Files\Common Files\VST3\`, the CLAP in `...\CLAP\`, and the standalone `.exe` can go anywhere. Linux: VST3 to `~/.vst3/`, CLAP to `~/.clap/`, LV2 to `~/.lv2/`. Rescan in your DAW.

## The surgeons

| # | Surgeon | Does | P1 | P2 | P3 |
|---|---|---|---|---|---|
| 1 | STUTTER | beat-repeat, ratchet subdivision, small pitch drift per repeat | LEN | RATCHET | DRIFT |
| 2 | GRANULAR | grain cloud pulled from the buffer, spread grows with chaos | SIZE | DENSITY | SPRAY |
| 3 | REVERSE | reversed slice with amplitude bloom and reverb tail | LEN | BLOOM | TAIL |
| 4 | CORRUPT | bitcrush and sample-rate reduction. MODE morphs from dropouts to digital clicks to tape wow and azimuth error | BITS | RATE | MODE |
| 5 | REORDER | slices a region and permutes it (retrograde, palindrome, Fibonacci, Euclidean), then replays | SLICES | PERM | REPEAT |
| 6 | FREEZE | phase-vocoder spectral freeze. BLUR randomises bin phase | OFFSET | LENGTH | BLUR |

Every surgeon has **On**, **Mix**, **Prob** (how often it fires) and **Route In** (feeds another surgeon's output back through the buffer for feedback stacks).

## Master controls

- **Chaos**: one axis. ORDER is grid-locked and repeatable. MIDDLE adds jittered lengths, skipped triggers and occasional wrong slices. CHAOS randomises positions, opens feedback and ignores the grid.
- **Rhythmic Gravity**: **Gravity Pull** drags slice positions toward the grid. **Snap** is 1/16, 1/32, 1/8T, 1/8 or Free. **Swing** pushes odd steps.
- **Trigger Rate**: global surgeon activity.
- **Reinject**: how much of the output is written back into the buffer.
- **Analysis Inform**: how often a slice is chosen by analysis (brightest 50 ms, nearest transient, loudest, most tonal) instead of by the grid.
- **Source**: Live, Sample A, Sample B or Morph A/B.
- **Panic Freeze**: freezes the buffer and holds it on the FREEZE surgeon.
- **Decay Over Time**: drives chaos and corruption to maximum across **Decay T** seconds.
- **SCAR**: optional texture stage after the rack. Drive adds soft saturation and a torn transient edge. Mix blends it against the untreated signal.
- **BUFFER**: 4, 8 or 16 bars. This also limits how far back the surgeons can reach.

## RANDOM, MUTATE and locks

**RANDOM** makes a new sound. The first press starts from current settings, and later presses reset unlocked sound controls to default before randomising. Input and output trim, buffer size, source selection, MIDI mode and the hidden controls are left alone. If every surgeon switch is locked off, RANDOM keeps them off and Diagnostics says why the output is silent.

**MUTATE** makes a nearby variation. **MUTATE AMT** sets how far it goes. It keeps infrastructure settings, loaded samples and user settings.

Right-click a control and choose **Lock for Randomize / Mutate** to protect its value. The same menu clears every lock. **RESET** still restores all defaults.

## Mod matrix

Four slots. Sources: LFO 1, LFO 2 (rate and shape), Env Follow, Macro 1, Macro 2, Random Walk. Destinations: Chaos, Gravity Pull, Swing, Trigger Rate, Reinject, Morph, Stutter Drift, Granular Density, Granular Spray, Corrupt Bits, Corrupt Rate, Freeze Blur.

## Live and MIDI

- Notes **C3 to F3** (MIDI 48 to 53) trigger surgeons 1 to 6.
- **CC20** Macro 1, **CC21** Macro 2, **CC22** Chaos, **CC23** Trigger Rate.
- **MIDI Mode** turns off the automatic scheduler, so the plug-in is a pure performance instrument.

## Sidechain

Enable the sidechain input. **SC to Main**: transients on the sidechain trigger glitches on the main signal. **Main to B**: transients on the main signal trigger surgeons that read from Sample B.

## History rewind

The bottom bar records sound settings at 2 Hz for 60 seconds. Drag it to rewind the sound. Global user settings do not change. **Freeze / Save** writes the current state as a user preset.

## Presets and samples

Factory characters: Clean Specimen, Late 90s Bristol, Berlin Basement, Warp 1995, Modern Hyperpop, SOPHIE-adjacent. User presets are `.vsxpreset` files in **MENU > Open Preset Folder**. Samples can be loaded into A and B.

Specimen exports write 16, 24 or 32-bit PCM WAV and report the filename, folder, duration and quality.

## Troubleshooting

- Silent output: check the surgeon **On** switches and Mix, and whether every switch is locked off. Diagnostics explains the cause.
- Crash: turn on the crash log from Help. Logs and the troubleshooting file are in the user data folder shown on the Help page.
