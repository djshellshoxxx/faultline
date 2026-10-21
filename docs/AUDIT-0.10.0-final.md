# Vivisect 0.10.0 beta: final audit

Branch `fix/audit`. Harness: 6598 checks, 0 failed. Regression suite: `Tests/TestsAudit.inc`.

| Item | Verdict | Evidence | Where |
|------|---------|----------|-------|
| A. New parameters inserted mid-layout shifted later parameter indices (LV2 ports, CC maps, host automation) | fixed | Scale/Dose/routeAmt now appended after `mm4_depth`; test "parameter-index stability" | Source/Parameters.h (routeAmt loop ~312) |
| A. Old sessions, presets, A/B slots and history frames did not default `routeAmt` | fixed | `defaultAbsentNewParams` covers effects, scale/dose, routeAmt; test "old session/preset" | Source/PluginProcessor.cpp:27, 993, 1100 |
| A. Right-click Reset and Set value bypassed UNDO | fixed | `setParameterAsGesture`; one undo step each | Source/Ui.cpp:135, 145, 182 |
| A. MUTATE amount 0 pushed an undo step | fixed | UndoScope created after early return | Source/PluginProcessor.cpp ~1724 |
| A. Closing the editor mid-drag left the undo gesture open | fixed | `commitOpenUndoGesture` in editor destructor | Source/PluginProcessor.cpp:2034, Source/PluginEditor.cpp:429 |
| A. RESET, RANDOM, MUTATE, locks, A/B for new params; RANDOM/MUTATE leave root/type alone | verified | TestsMusical/TestsAudit | Source/PluginProcessor.cpp |
| A. Mod destinations 13-16, DOSE identity at 0, bypass bit-exact, undo cap 100 | verified | TestsAudit "each new mod destination changes DSP" | Source/Dsp.cpp |
| B. Unwired callbacks/methods | fixed | Removed dead `crashLogPath`, `setRouteVisible`, effects `preferredWidth/Height`; UNDO/REDO/CAPTURE/CANCEL wired, shelf refresh via `CapturePanel::poll()` | Source/Ui.h, Source/PanelsEffects.h, Source/PanelsCapture.cpp |
| C. Real-time safety (capture ring preallocated, no locks/IO/alloc on audio thread) | verified | Code read of Capture.h, Effects.h, Musical.h | Source/Capture.h |
| C. Editor closed or processor destroyed mid-capture | verified | TestsAudit | Tests/TestsAudit.inc |
| D. Rates 22.05k-192k, blocks 1-8192, mono/stereo, BPM 20/300, NaN/Inf/denormals | verified | TestsAudit finite and bounded cases | Tests/TestsAudit.inc |
| E. Troubleshooting file lacked new feature state | fixed | "FEATURES (0.10.0)" section | Source/PluginProcessor.cpp:1282 |
| E. After Hard Reset hint strip and Simple view did not refresh | fixed | re-applied in `onHardReset` | Source/PluginEditor.cpp:376 |
| E. Hard Reset clears undo and settings but keeps captures; crash log off on load | verified | TestsAudit | Source/PluginProcessor.cpp |
| E. Help text, tooltips (gear button tooltip added), hard reset wording | fixed | Ui.cpp / PluginEditor.cpp | Source/Ui.cpp |
| F. README / Help claims: RANDOM/MUTATE and key, CLOT 4 s limit, 64 s capture, Scale Lock inert DRIFT/SPRAY | fixed | corrected | README.md, Source/Ui.cpp |
| Deferred: first STUTTER repeat is unison (not in key) with Scale Lock on and non-C root | deferred | behaviour unchanged from integration | Source/Dsp.cpp |
| Deferred: pluginval, clap-validator, Windows harness | deferred | not available in this environment | n/a |
| Note: test `pump()` uses `stopDispatchLoop()`, which latches quit; audit tests call `onClick`/`poll()` directly | noted | harness limitation | Tests/TestsUndoCapture.inc |
