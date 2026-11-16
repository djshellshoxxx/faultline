# Architecture

- **Input and buffer.** Input trim feeds a rolling, bar-synced, freezable specimen buffer.
- **Analysis.** Transient onsets, spectral frames (centroid, flatness, RMS) and A/B sample slots.
- **Scheduler.** A slice scheduler, driven by the Chaos field and Rhythmic Gravity, chooses when and which slice each surgeon plays.
- **Surgeons.** Six parallel processes whose output is summed per surgeon, with feedback routing that reinjects into the specimen.
- **Output.** Dry/wet mix, where dry ducks under surgeon activity, then SCAR, TOURNIQUET, CLOT and a hidden FLATLINE stage.
- **Source files.** `Source/Dsp.*` for DSP, `Source/Capture.h` for capture, `Source/Ui.*` and `Source/Panels*.{h,cpp}` for the interface, and `Source/PluginProcessor.*` for the plugin shell.
