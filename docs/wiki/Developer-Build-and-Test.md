# Developer Build and Test

## Toolchain

- CMake 3.22 or newer, C++17 compiler
- Windows: Visual Studio 2022
- Linux: `build-essential cmake ninja-build pkg-config` plus the JUCE dependencies (ALSA, FreeType, fontconfig, X11 extensions, Mesa GL). Full list in `README.md`
- JUCE 8.0.4 is fetched by `FetchContent` unless you set `-DJUCE_TAG=` or `-DVIVISECT_SYSTEM_JUCE=ON` with `-DJUCE_DIR=`
- CLAP through `clap-juce-extensions`, fetched on first configure. Turn off with `-DVIVISECT_BUILD_CLAP=OFF`

## Build

Windows:

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Linux:

```
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

Outputs go under `build/Vivisect_artefacts/Release/` (or `build-linux/...`): `VST3/Vivisect.vst3`, `CLAP/Vivisect.clap`, `Standalone/Vivisect.exe`, plus `LV2/Vivisect.lv2` on Linux.

Build only one target:

```
cmake --build build --config Release --target Vivisect_CLAP
```

### CMake options

| Option | Default | Effect |
|---|---|---|
| `JUCE_TAG` | `8.0.4` | JUCE git tag to fetch |
| `VIVISECT_SYSTEM_JUCE` | OFF | use a local JUCE with `JUCE_DIR` |
| `VIVISECT_BUILD_CLAP` | ON | build the CLAP format |
| `VIVISECT_BUILD_TESTS` | ON | build `VivisectTests` |
| `VIVISECT_FORMATS` | `VST3 Standalone` (Linux adds LV2) | formats to build |

Add AU or AAX to `FORMATS` in `CMakeLists.txt` on the right platform if needed.

## Tests

```
cmake --build build --target VivisectTests --parallel 2
./build/VivisectTests
```

`Tests/TestMain.cpp` is the headless harness. It drives the processor block by block, with helpers `check()`, `beginCase()` and `runBlocks()`. It includes four optional suites, each guarded by `__has_include`:

- `TestsEffects.inc`
- `TestsMusical.inc`
- `TestsUndoCapture.inc`
- `TestsRouting.inc`

The suites are not in this repo right now. Add one as a file next to `TestMain.cpp` and the harness picks it up. The `VSX_HAVE_*_TESTS` macros show which ones were found.

The README records verification with pluginval (strictness 10, VST3), clap-validator (CLAP) and an ASan and UBSan debug run of the harness.

## CI (`.github/workflows/ci.yml`)

- Triggers: push to `main`, pull requests, manual dispatch
- Ubuntu job, 30 minute timeout. Installs the JUCE dependencies, configures with Ninja and `-DVIVISECT_BUILD_CLAP=OFF`, builds `VivisectTests`, runs it.

## Release (`.github/workflows/release.yml`)

- Matrix: `windows-x64` and `linux-x64`. Each one builds, runs the test harness (`Run test harness (Windows)` and `(Linux)`), packages a zip, and uploads it as an artifact.
- The `publish` job runs only on pushes. Tag pushes of `v*` publish that tag. Pushes to `main` publish `v<version>` from `CMakeLists.txt` if no release has that tag yet. Bump the version to publish again.
- Release notes are generated (`generate_release_notes: true`).

## Release steps

1. Bump `project(Vivisect VERSION x.y.z)` in `CMakeLists.txt`.
2. Make sure the tests pass locally and CI is green.
3. Push to `main`, or tag `vx.y.z`. The publish job creates the release.
4. Check the zips and install them in a host (see [[Install]]).

## Wiki sync

`.github/workflows/wiki-sync.yml` mirrors `docs/wiki/` into the GitHub wiki on pushes to `main` that touch that folder, and on manual dispatch. If the wiki repo does not exist yet, the job posts a notice and exits without failing. Create it once from the Wiki tab.

## Troubleshooting builds

- JUCE download fails: set `-DVIVISECT_SYSTEM_JUCE=ON -DJUCE_DIR=<path>`.
- No CLAP needed: `-DVIVISECT_BUILD_CLAP=OFF`, which also skips the extra network fetch.
- Missing test suites: expected. The `.inc` files are optional.
