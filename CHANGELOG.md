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
- FREEZE level tuning.
- macOS builds for Apple Silicon (arm64) and Intel (x86_64), including the
  Audio Unit format. Ad-hoc signed, not notarized; see INSTALL.md.

### Build and release
- CI is split into `tests` (Linux and Windows, run once), independent
  per-platform `build` jobs that do not re-run tests, and a `publish` job
  that needs both.
- Compile caching (ccache) and a cached JUCE download for faster rebuilds.
- Releases are tagged `v<version>-beta.<N>`, with N read from `release-beta.txt`.

## 0.9.3

See git history.
