# Developer Conventions

## Style

- C++17, JUCE idioms, `juce::` names. Match the file you are editing.
- Namespaces: `vsx` for everything in `Dsp`, `Parameters`, `Ui`. Product strings are in `namespace product` in `Theme.h`.
- Section markers inside `processChunk()` use `// ---- name ----` banners. Keep them and their order.
- Comments explain why. Many record a past bug or an audit finding. Keep those.
- Parameter IDs are strings in `namespace vsx::id`. Never rename one that has shipped, because saved presets store it.

## Real-time rules

- No allocation on the audio thread. Buffers are sized in `prepareToPlay` for the prepared block. Oversized host blocks are split in `processBlock()`, not allocated for.
- `processChunk()` guards against NaN and Inf from upstream at the start, before they reach the specimen ring, the reverb tails and the envelope follower. Keep that guard first.
- Cross-thread state uses `std::atomic` (surgeon activity, pulse, output peaks, learn state, pending CC values). Do not add a lock on the audio path.
- The UI reads DSP state for display without a lock. This is a known and accepted tradeoff, noted in `README.md`.
- MIDI CC values are queued and reported to the host on the message thread (`flushPendingMidiParameterChanges`). CLAP requires this. Do not notify the host from the audio thread.
- `SurgeonRack`, `ModMatrix` and the scheduler are called from `processChunk()` only. Keep them out of the UI.

## Determinism and timing

- Random Walk runs on a fixed internal clock, so its result does not depend on the host buffer size. Keep it that way.
- Parameters are read once per block. Sample-accurate automation splitting is off on purpose (`README.md`).
- Musical timing comes from the host through `MusicalContext` and `TriggerContext`. Manual triggers use the same context.

## Gotchas

- **Hidden controls.** `FLATLINE` and the `SecretPanel` are hidden. Do not add them to user docs or the manual unless the maintainer changes that.
- **Presets.** `.vsxpreset` files are read by name from the user preset folder. Renaming a factory character changes what users see in the preset list.
- **History** stores sound state every 15 timer ticks as XML (`soundStateSnapshot`). A new sound parameter is stored automatically if it is in the APVTS. Global settings (tooltips, UI scale) are not in the history.
- **Sample loading** happens on the message thread. Slots are cleared with `clearSampleSlot()`. Samples are re-loaded with the host session.
- **Crash handler** is removed cleanly when the plug-in closes. Keep that when you change the hook.
- **Test suites** are optional `.inc` files. Do not make the harness depend on them.

## How to add a feature

1. Add the parameter in `Source/Parameters.h` (ID, layout, label).
2. Put the DSP in `Source/Dsp.h` or `Dsp.cpp`. Keep it free of JUCE GUI code.
3. Read the parameter in `processChunk()` in the matching section.
4. Add the UI control in `Source/Ui.cpp` and wire it in `PluginEditor.cpp`.
5. Add a test in `Tests/` (a new `.inc` suite or `TestMain.cpp`), and run it.
6. Update `README.md` (surgeon or control tables) and this wiki.
7. Open a PR. CI must pass.

## Known limitations

From `README.md`:

- The full N by N surgeon patch matrix is future work. Feedback currently goes through Reinject and Route In.
- FREEZE's overlap-add gain is approximate and needs tuning.
- The UI monitor reads DSP state without a lock. It is display only.
- The marketing and UI polish step is only a first pass.
- LV2 is Linux only. AU and AAX are not built.
