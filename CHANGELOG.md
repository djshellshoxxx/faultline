# Changelog

## 0.10.0-beta.1

Pre-release. Published on GitHub as a pre-release, not the latest stable build.

### Added
- TOURNIQUET filter stage.
- CLOT delay stage.
- Undo / Redo for parameter edits.
- Scale Lock.
- Capture and drag-out.
- DOSE control and a Simple view.
- Surgeon route amount.
- FREEZE level tuning: output level within +-1.5 dB of the frozen specimen at
  all BLUR settings.
- Eight new factory presets for the new stages: Tourniquet Sweep, Clot Dub
  Smear, Minor Key Shrapnel, Dose Ramp, Feedback Ward, Notch Ghost, Telephone
  Gate, Whole Tone Haze. The original six are unchanged.
- Help page and README cover every new feature, macOS (AU, Gatekeeper) and LV2
  install paths; tooltips for every new control.
- macOS builds for Apple Silicon (arm64) and Intel (x86_64), including the
  Audio Unit format. Ad-hoc signed, not notarized; see INSTALL.md.

### Notes
- Older sessions and presets load the new parameters at their defaults and
  sound the same as before.
- MIDI import/export from the shared plug-in checklist is intentionally not
  provided: Vivisect is an effect.

### Build and release
- CI is split into `tests` (Linux and Windows, run once), independent
  per-platform `build` jobs that do not re-run tests, and a `publish` job
  that needs both.
- Compile caching (ccache) and a cached JUCE download for faster rebuilds.
- Releases are tagged `v<version>-beta.<N>`, with N read from `release-beta.txt`.

## 0.9.3

See git history.
