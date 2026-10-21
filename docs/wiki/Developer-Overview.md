# Developer Overview

## What the code is

A JUCE 8 plug-in (C++17), product name **Vivisect**, repo **faultline**. One processor, one editor, one DSP file pair and one UI file pair. Formats: VST3 and Standalone everywhere, LV2 on Linux, and CLAP through `clap-juce-extensions`, which wraps the same JUCE target. The CLAP is the same plug-in, not a second implementation.

Identity in `CMakeLists.txt`: `project(Vivisect VERSION 0.9.3)`, manufacturer code `Spcm`, plugin code `Vvsc`, LV2 URI `https://specimenaudio.com/plugins/vivisect`. JUCE tag is `8.0.4` (`JUCE_TAG`), or use a system copy with `-DVIVISECT_SYSTEM_JUCE=ON`.

## Tech stack

- C++17, JUCE 8.0.4 (`juce_audio_utils`, `juce_dsp`, GUI modules)
- APVTS for parameters (`Source/Parameters.h`, IDs in `namespace vsx::id`)
- Phase-vocoder FFT for FREEZE, a rolling `SpecimenBuffer` for everything else
- Test harness: one executable, `VivisectTests`, built from `Tests/TestMain.cpp`
- CMake and GitHub Actions. Packaging is a zip per platform.

## Design decisions (and why)

**Specimen, not signal.** The input is kept as a bar-synced rolling buffer and analysed (transient onsets, spectral frames for centroid, flatness and RMS). Surgeons take slices from that analysed buffer. This is what lets a surgeon choose a slice by musical rule (brightest, nearest transient, loudest) rather than by time alone.

**Six parallel surgeons, one scheduler.** `SliceScheduler` picks when and which slice, driven by the Chaos field and Rhythmic Gravity. The six `Surgeon` subclasses (`Stutter`, `Granular`, `Reverse`, `Corrupt`, `Reorder`, `Freeze`) only render. Keeping choice and rendering apart means a new surgeon does not change the scheduling rules.

**Chaos is one knob on purpose.** The chaos field sets how much the schedule drifts from the grid, how often triggers are skipped and how much positions randomise. The UI shows it as one pulse. Users should be able to go from locked to wild in one movement.

**Reinject for feedback.** Output is written back into the specimen through Reinject, and Route In feeds one surgeon into another. The notes in `README.md` say a full N by N patch matrix is future work. Feedback today goes through the buffer.

**Latency and blocks.** Parameters are read once per block. Event splitting is off (CLAP resolution 0 samples). Hosts may send bigger blocks than `prepareToPlay` promised, so `processBlock()` splits them into prepared-size chunks before `processChunk()`.

**Musical timing comes from the host.** `MusicalContext` and `TriggerContext` carry tempo and position. The manual trigger path (`triggerSurgeonManual`) uses the same context so MIDI and automation behave like the scheduler.

**Random Walk on a fixed internal clock.** Its behaviour does not depend on host buffer size. Reorder's Euclidean mode uses a maximally even pattern.

**MIDI CC learn across threads.** The audio thread can learn a CC (`midiMapDirty`). `timerCallback()` writes the map to state on the message thread. Parameter changes from CC are also reported on the message thread, because CLAP needs that. Pending CC values sit in `pendingCCValue` and `pendingCCParam`, so the audio thread only writes atomics.

**Known tradeoff.** The UI monitor reads DSP state without a lock. It is display only. The README calls this benign.

## Signal flow

```
host in (stereo, optional sidechain)
    │
    ▼
input trim ──► SpecimenBuffer (rolling, bar-synced, freezable)  ◄── Reinject
    │                 │
    │     ┌───────────┼──────────────┐
    │     ▼           ▼              ▼
    │  TransientDetector   SpectralAnalyzer   A / B sample slots
    │     └───────────┬──────────────┘
    │                 ▼
    │    SliceScheduler  ◄── ChaosField + Rhythmic Gravity + ModMatrix
    │                 ▼
    │  SurgeonRack: STUTTER GRANULAR REVERSE CORRUPT REORDER FREEZE
    │                 ▼
    │       sum (per-surgeon Mix) + Route In feedback
    │                 ▼
    │         wetOut ──► SCAR (optional) ──► ...
    ▼                 ▼
dry (ducks under surgeon activity) ──► dry/wet mix ──► output trim ──► out
```

`processBlock()` (`Source/PluginProcessor.cpp:126`) splits oversized blocks, then `processChunk()` (`:148`) does the work, in this order: NaN guard, MIDI, musical clock, mod matrix, decay-over-time, resolved globals, write to specimen and analyse, schedule, sidechain, run surgeons and reinject, mix, SCAR, FLATLINE (hidden), meters.

## Threads

- **Audio thread**: `processChunk()`, `SurgeonRack::process()`, `ModMatrix::process()`. No allocation intended.
- **Message thread**: editor, presets, samples, history. `timerCallback()` (`PluginProcessor.cpp:509`) flushes queued CC changes, writes the MIDI map when dirty, and records a history snapshot every 15 ticks.
- **Atomics** carry state across: surgeon activity (`surgAct`), pulse, output peaks, learn state, pending CC values.

## Data

- `vsx::MonitorSnapshot` (`Dsp.h:23`): what the UI reads each frame, built by `buildMonitorSnapshot()` (`PluginProcessor.cpp:423`)
- `vsx::SpecimenAnalysis` (`Dsp.h:380`): what analysis produces
- `vsx::TriggerContext` (`Dsp.h:412`): what one trigger needs
- `vsx::ModMatrix` (`Dsp.h:613`): four slots, sources to destinations
- State: `getStateInformation()` and `setStateInformation()` save the APVTS, plus settings in `settingsTree()`. Sound state is separate (`soundStateSnapshot`, `restoreSoundState`), so history and A/B can restore sound without touching global settings.

## UI

`Source/Ui.h/.cpp` has the look and feel (`VsxLookAndFeel`), the heart monitor (`HeartMonitorDisplay`), pulse meter, level meters, surgeon strips, history bar, data stream, overlay panels and the hidden secret panel. `PluginEditor` lays them out. The look is medical and anatomical on purpose. Defined by `Theme.h` and `Theme.cpp`.

See [[Developer-Code-Map]] for locations and [[Developer-Conventions]] for the rules.
