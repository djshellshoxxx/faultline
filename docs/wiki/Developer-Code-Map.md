# Developer Code Map

Paths are relative to the repo root. Line numbers are approximate. Search for the symbol if one looks off.

## Directory tree

```
faultline/
├── CMakeLists.txt              project(Vivisect 0.9.3), formats, FetchContent for JUCE and CLAP, test target
├── README.md                   design brief, surgeon table, status, audit notes
├── INSTALL.md                  where each file goes (Windows and Linux)
├── ELI5.md                     plain-language overview
├── SPECS-0.9.3.md, SPECS-0.10.0.md   version specs
├── Source/
│   ├── Parameters.h            namespace vsx: surgeon indexes, surgeon parameter slots, namespace vsx::id (all APVTS IDs)
│   ├── Dsp.h / Dsp.cpp         all DSP: Scar, Flatline, TelemetryRing, SpecimenBuffer, TransientDetector,
│   │                           SpectralAnalyzer, ChaosField, Surgeon base + six surgeons, SurgeonRack,
│   │                           SliceScheduler, ModMatrix
│   ├── PluginProcessor.h/.cpp  VivisectProcessor: processBlock, processChunk, presets, samples, state, MIDI, history, troubleshooting
│   ├── PluginEditor.h/.cpp     editor layout
│   ├── Ui.h / Ui.cpp           look and feel and custom components (heart monitor, meters, surgeon strips, history bar, overlays)
│   └── Theme.h / Theme.cpp     colours, fonts, product strings, user data folders (userPresetDir and friends)
├── Assets/
│   └── gen_icon.py             pure-stdlib generator for the ECG-style icon (writes Assets/icon.png)
├── Tests/
│   └── TestMain.cpp            headless harness (VivisectTests). Includes optional .inc suites if present
├── shared/scripts/             shared Circuit Drift Labs tooling (not used by CI here)
├── skills/                     bundled agent skills and licence notices (not plugin code)
├── docs/wiki/                  these wiki pages
├── .github/workflows/
│   ├── ci.yml                  Linux build and test on push to main and PRs
│   └── release.yml             Windows and Linux packages, test run, publish a release
├── .github/agents/feature-auditor.agent.md
└── include.md, COPYRIGHT-TRADEMARK.md, LICENSE, ELI5.md
```

## Key symbols

| Symbol (file:approx line) | Purpose |
|---|---|
| `enum SurgeonIndex` (`Source/Parameters.h:15`) | S_STUTTER, S_GRANULAR, S_REVERSE, S_CORRUPT, S_REORDER, S_FREEZE |
| `enum SurgeonParameter` (`Parameters.h:16`) | SP_ON, SP_MIX, SP_PROB, SP_P1, SP_P2, SP_P3, SP_ROUTE |
| `namespace vsx::id` (`Parameters.h:18`) | parameter ID strings, starting with `bufferBars` |
| `sidRaw()`, `surgeonName()`, `paramLabel()`, `modSourceNames()`, `modDestNames()` (`Parameters.h:72` to `:110`) | ID and label helpers for surgeons and the mod matrix |
| `createParameterLayout()` (`Parameters.h:117`) | builds the APVTS layout |
| `class SpecimenBuffer` (`Dsp.h:285`) | rolling bar-synced buffer |
| `class TransientDetector` (`Dsp.h:329`) | onset detection (energy flux) |
| `class SpectralAnalyzer` (`Dsp.h:348`) | centroid, flatness and RMS frames |
| `struct SpecimenAnalysis` (`Dsp.h:380`) | analysis result |
| `struct ChaosField` (`Dsp.h:391`) | chaos behaviour |
| `struct TriggerContext` (`Dsp.h:412`) | everything one trigger needs |
| `class Surgeon` (`Dsp.h:431`) | base class for the six surgeons |
| `class SurgeonRack` (`Dsp.h:464`, `Dsp.cpp:860`) | owns the six surgeons, `process()` at `Dsp.cpp:878` |
| `class SliceScheduler` (`Dsp.h:501`) | when and which slice. `makeManualContext()` at `Dsp.cpp:923` |
| `class ModMatrix` (`Dsp.h:613`) | four slots. `process()` at `Dsp.cpp:970`, `sourceValue()` at `:1013`, `destOffset()` at `:1026` |
| `class Scar`, `class Flatline` (`Dsp.h:45`, `:92`) | post-rack texture stage and the hidden effect |
| `class Stutter`, `Granular`, `Reverse`, `Corrupt`, `Reorder`, `Freeze` (`Dsp.cpp:393`, `446`, `522`, `592`, `665`, `743`) | the six surgeons |
| `VivisectProcessor::processBlock()` (`PluginProcessor.cpp:126`) | splits oversized blocks |
| `VivisectProcessor::processChunk()` (`:148`) | the audio work, sectioned by `// ----` comments |
| `triggerSurgeonManual()` (`:109`) | manual trigger from MIDI or the buttons |
| `buildMonitorSnapshot()` (`:423`) | UI snapshot each frame |
| `timerCallback()` (`:509`) | queued CC flush, MIDI map write, history |
| `rewindToNormalized()` (`:525`) | history rewind |
| `factoryPreset()` (`:635`) | the six factory characters |
| `loadSampleInto()`, `clearSampleSlot()` (`:707`, `:722`) | sample deck A and B |
| `getStateInformation()`, `setStateInformation()` (`:732`, `:747`) | host state |
| `soundStateSnapshot()`, `restoreSoundState()` (`:811`, `:820`) | sound-only state for history and A/B |
| `detectMisconfiguration()`, `buildTroubleshootingReport()` (`:892`, `:953`) | diagnostics |
| `setCrashLogEnabled()`, `hardResetAllSettings()` (`:1062`, `:1114`) | crash log and hard reset |
| `seedDefaultMidiMap()` (`:1158`) | default CC mapping |
| `VsxLookAndFeel` (`Source/Ui.h:42`) | look and feel |
| `HeartMonitorDisplay` (`Ui.h:139`), `PulseMeter` (`:152`), `LevelMeter` (`:171`), `OutputLed` (`:189`), `SurgeonStrip` (`:202`), `HistoryBar` (`:227`), `DataStream` (`:264`), `OverlayPanel` (`:300`), `SecretPanel` (`:393`) | UI components |
| `userPresetDir()`, `userLogDir()`, `userCacheDir()` (`Source/Theme.cpp:104` to `:106`) | per-user data folders |

## Where do I find X / to change X, edit Y

| I want to... | Edit |
|---|---|
| add a parameter | `Source/Parameters.h`: add the ID in `namespace vsx::id` and the entry in `createParameterLayout()`, label in `paramLabel()` if it is a surgeon parameter. Read it through the APVTS in the DSP. The control goes in `Ui.cpp` / `PluginEditor.cpp` |
| change what a surgeon does | its class in `Source/Dsp.cpp` (lines 393 to 743) |
| add a surgeon | new `Surgeon` subclass in `Dsp.cpp`, add it to `SurgeonRack`, add an index to `SurgeonIndex`, a button or strip in the UI, and the MIDI note map |
| change how slices are chosen | `SliceScheduler` in `Dsp.h` and `Dsp.cpp` |
| change Chaos behaviour | `ChaosField` in `Dsp.h` |
| change Rhythmic Gravity and Swing | `SliceScheduler` and `MusicalContext` in `Dsp.h` |
| change analysis (transients, centroid) | `TransientDetector`, `SpectralAnalyzer` in `Dsp.h` |
| change the mod matrix sources or destinations | `ModMatrix` in `Dsp.h` / `Dsp.cpp`, its destination enum, and `ModMatrix::destOffset()` |
| change MIDI notes or CCs | `seedDefaultMidiMap()` and the MIDI section of `processChunk()` in `PluginProcessor.cpp`, plus `triggerSurgeonManual()` |
| change sidechain behaviour | sidechain section of `processChunk()` |
| change the master texture (SCAR) | `class Scar` in `Dsp.h` / `Dsp.cpp` |
| change the factory characters | `factoryPreset()` in `PluginProcessor.cpp` |
| change preset or sample handling | `PluginProcessor.cpp` (preset and sample functions), folders in `Theme.cpp` |
| change history rewind | `timerCallback()`, `rewindToNormalized()`, `soundStateSnapshot()` |
| change the look | `Source/Theme.cpp` and `VsxLookAndFeel` in `Source/Ui.cpp` |
| change product name, IDs, LV2 URI | `CMakeLists.txt` (`PLUGIN_CODE`, `PLUGIN_MANUFACTURER_CODE`, LV2 URI) and the `product::` constants in `Source/Theme.h` (name, vendor, home, github)
| change the icon | `Assets/gen_icon.py` |
| add a test | a new `.inc` suite (see [[Developer-Build-and-Test]]) or `Tests/TestMain.cpp` |

## Gotchas for navigation

- `Ui.cpp` and `PluginProcessor.cpp` are large. Use `grep -n` on symbols.
- The LV2 build only happens on Linux (`VIVISECT_FORMATS` changes by platform in `CMakeLists.txt`).
- `Source/Parameters.h` is the only place the parameter IDs are defined. Renaming an ID breaks saved presets.
