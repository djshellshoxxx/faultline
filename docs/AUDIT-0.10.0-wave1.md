# Audit: include.md / theme.md compliance (0.10.0 wave 1)

Static read of the code on `feat/routing-freeze-audit`, plus the headless harness. Status: OK, FIXED (changed in
this branch), GAP (needs someone else or a decision), UNVERIFIED (needs a GUI or host run).

| Item | Status | Where | Action |
|------|--------|-------|--------|
| Help: feature explanations, workflow, GUI tour | OK | Source/Ui.cpp:1050-1200 | Routing AMT text added (FIXED, Ui.cpp:1136). Later waves must add their own controls (TQ, Clot, Scale, Dose, Capture, Undo). |
| Help: version | OK | Ui.cpp:1051, 1251, 1280 (`JucePlugin_VersionString`) | CMake still says 0.9.3 (CMakeLists.txt:2); version bump is the release owner's job. |
| Help: license | OK | Ui.cpp:1250-1268 | none |
| Help: github link | OK | Ui.cpp:1035, 1272; Theme.h:64 | none |
| Help: homepage | UNVERIFIED | Ui.cpp:1034, 1271; Theme.h:63 (`specimenaudio.com/vivisect`) | Owner to confirm the domain is live. |
| Help: support email | UNVERIFIED | Ui.cpp:1036, 1273; Theme.h:65 | Owner to confirm the mailbox exists. |
| Help: install / uninstall troubleshooting | OK | Ui.cpp:1201-1228 | LV2 (Linux) and macOS AU/Gatekeeper not mentioned; add with G3 docs. |
| Help: preset location | OK | Ui.cpp:1230-1248, Open Preset Folder button Ui.cpp:1042 | none |
| Help: debug button | OK | Ui.cpp:1044; PluginEditor.cpp:247 | none |
| Custom icon | OK | CMakeLists.txt:47-48, Assets/icon.png, icon_small.png, Assets/gen_icon.py | none |
| Preset bank, descriptive names | OK (thin) | PluginProcessor.cpp:582 (6 factory) | Six presets; G3 asks for new ones showcasing the new stages. |
| RESET | OK | PluginEditor.cpp:226-228; PluginProcessor.cpp:1285 | Covers Route Amt (test added). |
| Menu: Save / Save As / Open / Options / open location | OK | PluginEditor.cpp:560-610 | none |
| Options page: tooltips toggle, audio/MIDI setup | OK | Ui.cpp:1303-1345 | Toggle and device button had no tooltip (FIXED, Ui.cpp:1309-1310). Device setup is standalone-only by design. |
| Export audio popup (name, location, length, quality) | OK | PluginEditor.cpp:441-470 | none |
| MIDI import/export | N/A | n/a | Effect plug-in; README note belongs to the docs wave. |
| Right-click: MIDI map, reset, set value, lock | OK | Ui.cpp:142 `showParamContextMenu`; every control takes a `ParamHost` | none |
| Double-click reset, shift coarse, ctrl/cmd fine, vertical cursor | OK | Ui.cpp:492-498, 543-544 | Feel not checked by eye. |
| Drag and drop of samples | OK | PluginEditor.cpp:500-558 (left half A, right half B) | none |
| Tooltips on every control | FIXED | Ui.cpp (overlay CLOSE buttons had none, FIXED at :986) | New controls from other waves need tips. AMT tip added. |
| Tooltip delay 400 ms, dark pill | OK | Theme.h:97; Ui.h:445-453 | none |
| Debug panel: raw data live | OK | Ui.cpp:1620-1760; PluginEditor.cpp:249-269 | none |
| Crash-log checkbox, default off every load | OK | PluginProcessor.cpp:1064-1107; test "crash log lifecycle" | none |
| Troubleshooting file (diagnostic, settings, audio/MIDI, licence, version, DAW) | OK | PluginProcessor.cpp:955-1058 | Licence line is a summary, full text is on the Help page. |
| Hard reset + cache removal | OK | PluginProcessor.cpp:1116-1148; confirm dialog Ui.cpp:1645 | none |
| Easter egg: corner notch reveal, close, secret tooltips | OK | PluginEditor.cpp:485-494; PluginEditor.h:125 (12x12 hotspot); Ui.cpp:1823-1930 | none |
| Theme: palette | OK | Theme.h:21-34 | none |
| Theme: header 32 px, name 14 px semibold, gear / preset / A-B | OK | Theme.h:89; PluginEditor.cpp:613-623, 705-723 | none |
| Theme: 2 px / 12 px notch, top-left | OK | Theme.cpp:59-65 | none |
| Theme: footer version, 9 px mono, bottom right | OK | PluginEditor.cpp:672-677 | none |
| Theme: output LED grey to white, red above 0 dB | OK (small deviation) | Ui.cpp:1360-1405 | Red releases with an ease-out (about 80 ms), not instantly on dropping below 0 dB. Acceptable, flag only. |
| Theme: data stream (rolling, faded rows, light green) | OK | Ui.cpp:1410-1590; PluginEditor.cpp:827-838 | Hidden when the gap is too small, as spec allows. |
| Theme: Inter / JetBrains Mono fallback chain | OK | Theme.h:105-126 | Fonts are not bundled; falls back to system fonts. |
| RANDOM / MUTATE rules in include.md | OK | PluginProcessor.cpp:1297-1480 | Sidechain mode is not in the preserved list, so later RANDOM presses reset it to default. include.md does not list it; leave unless the owner wants it kept. |
| Orphan parameters (no UI control) | OK | Parameters.h vs PluginEditor.cpp:100-161, Ui.cpp:855-895, Ui.cpp:1823 | All ids have a control. FLATLINE is behind the notch by design. |
| Orphan DSP fields | FIXED | Dsp.h:397, Dsp.cpp:287 `ChaosField::routeSwap` was computed and never read | Removed (no sound change). |
| TODO / FIXME / stub in Source/ | OK | grep found none | none |
| Route Amt (G1) in layout, RANDOM, MUTATE, locks, A/B, presets, RESET, tooltip, label, UI | OK | Parameters.h ROUTING (G1) blocks; PluginProcessor.cpp; Ui.cpp ROUTING (G1) blocks | Tests in Tests/TestsRouting.inc. |
| FREEZE level (G2) | FIXED | Dsp.cpp Freeze::trigger `compGain` | Within 0.5 dB on 90 combinations. README "approximate" wording (README.md:216) must be removed by the docs wave. |
| README "future work" for routing matrix | GAP | README.md:214 | Docs wave: replace with the G1 rationale. Not touched here by instruction. |
| History bar includes Route Amt | OK by construction | rewind uses APVTS state snapshots (PluginProcessor.cpp:soundStateSnapshot) | none |

## Bigger items left for others
- README.md lines 125, 214, 216 (routing note, matrix rationale, freeze wording).
- Help text and tooltips for every control added by the effects, scale, dose, capture and undo waves.
- CMake / Help version number still 0.9.3.
- Factory preset bank: G3 asks for presets showing the new stages.
- Confirm homepage domain and support mailbox are real.
- Visual checks (knob feel, LED red timing, stream fade, strip layout at 50 percent and 150 percent) need a GUI run.
