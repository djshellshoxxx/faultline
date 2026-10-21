# Vivisect 0.11.0 (beta) Feature Specifications

Normative for implementation and tests. Builds on SPECS-0.10.0.md; every earlier invariant still holds.
Neither feature adds a sound parameter, so presets, A/B, RANDOM, MUTATE and the parameter-sweep tests
are unaffected. Both persist (if at all) only in `VSX_SETTINGS`.

## H1. Tap tempo (essential in standalone)

Standalone has no transport, so synced features (STUTTER/CLOT divisions, gravity grid, CAPTURE bar length) ran at a
fixed 120 BPM with no way to change it.

- `vsx::TapTempo` (`Source/Tempo.h`, pure, allocation-free): `double tap (double seconds)` returns BPM from the
  **median** of the last <= 7 tap intervals (8 taps kept), `0` until two taps. A gap > 2 s, or time going
  backwards, starts a new count. Result clamped to 20..300 BPM; non-finite -> 120.
- Processor: `tapTempo()`, `setTempoFallback (bpm)` (0 clears, else clamped), `tempoFallback()`,
  `currentBpm()`, `hostHasTempo()`. Stored as `tempoFallback` in `VSX_SETTINGS`; mirrored into an atomic read by
  the audio thread (restored by `setStateInformation`).
- Precedence in `processBlock`: host tempo > tapped tempo > 120 BPM. The tapped tempo never overrides a host tempo, so
  bar alignment of CAPTURE and the musical clock stay consistent with the host. Hosts that stop sending a tempo keep
  their last one unless a tapped tempo is set.
- UI: header button `HOST <bpm>` / `TAP <bpm>`. Left click taps; right-click menu: Double (x2), Half (/2), Clear.
  Tooltip explains precedence. Help page section "TAP TEMPO AND DETECT KEY".
- Tests: median/outlier, reset gap, clamp, backwards time, long run; fallback used without host, clamped, cleared to
  120, survives a state round trip, untouched by RESET/RANDOM.

## H2. Detect key (more musical)

- `vsx::keydetect` (`Source/KeyDetect.h`, pure): `chromagram()` = Hann-windowed Goertzel magnitude on every semitone
  C2..B6 (below 0.45 fs), summed per pitch class; `detectKey()` = Pearson correlation against the Krumhansl-Schmuckler
  major/minor profiles rotated through 12 roots; `analyse()` runs both. Silence, NaN, blocks < 2048 samples -> invalid.
  `root` uses Scale Root numbering (C = 0). Relative major/minor ambiguity is inherent to the method.
- Threading: `requestKeyDetect()` sets a flag; the audio thread copies the last min(65536, ring, written) samples
  (mono mix of the specimen) into a buffer preallocated in `prepareToPlay` (no allocation, no locks); the message
  thread's `pollKeyDetect()` runs the analysis (~0.1 s) so the audio thread never does it.
- Result application (valid key only): one `UndoScope` step setting Scale Root, Scale Type (Major=1 / Minor=2) and
  Scale Lock On, **skipping any locked parameter**. Invalid result changes nothing and the button shows `NO KEY`.
- UI: `DETECT` button under SCALE LOCK; shows `LISTENING...` then e.g. `D MINOR`.
- Tests: all 24 ideal profiles round-trip; flat/NaN rejected; synthesized C major / D minor / F# major chords detected
  at 22.05-192 kHz; silence/short/noise safe; through the processor: sets root+type+on, one undo restores all, locked
  root untouched, silence changes nothing.
