# Developer Guide

## Build

Requires CMake 3.22 or later and a C++17 compiler. JUCE is fetched on first configure. Windows:

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Linux (VST3, CLAP, LV2 and Standalone) needs the packages listed in the README, then:

```
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

CLAP is fetched from clap-juce-extensions and can be disabled with `-DVIVISECT_BUILD_CLAP=OFF`.

## Tests

The `VivisectTests` target is in `CMakeLists.txt`. Its test sources are in `Tests/`, with the `.inc` suites included by `TestMain.cpp`. Run `build/Release/VivisectTests` on Windows, or the matching path on Linux. The QA record in `docs/QA-0.10.0.md` describes the sanitizer, pluginval and clap-validator runs the project has performed.

## Release

Release builds run from the `Release builds` workflow. Beta versions come from `release-beta.txt`.
