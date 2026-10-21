# QA pass, 0.10.0 beta

## What was run
- Harness under ASan + UBSan (RelWithDebInfo, `-fsanitize=address,undefined`, no-recover on UBSan, leak check off): 6611 checks, 0 failed, no sanitizer reports in project or JUCE code.
- New optional `qa` suite (`Tests/TestsQa.inc`): randomized soak over all parameters (including tq*, clot*, scale*, dose, routeAmt) mixed with RANDOM, MUTATE, undo, redo, preset load, A/B, capture start/cancel, state round trips, MIDI, 5 sample rates (22.05k to 192k), block sizes 1/7/64/512/8192 and BPM 20..300. Output stayed finite and below 0.19 of the ceiling. Adds well under a minute to the Release harness.
- pluginval 1.x, strictness 10, in-process, under xvfb (editor tests included) on the VST3: SUCCESS.
- clap-validator 0.3.2 on the CLAP: 21 run, 18 passed, 0 failed, 3 skipped (preset-discovery factory not implemented).
- Normal harness (Release): 6611 checks, 0 failed.

## Fixed
1. STUTTER's first repeat was unison with Scale Lock on and a non-C root; it now picks a scale degree (regression test measures the pitch of the first repeat).
2. Ctrl+Z / Redo pressed during an open drag, and A/B swap, preset load or rewind during a drag, could fold the drag into the wrong undo step. An open gesture is now committed first.
3. Toggling Simple view mid-drag could leave an undo gesture open (hidden control never gets its mouse-up); it is now committed.
4. "Minor Key Shrapnel" factory preset peaked at 1.04 once the first repeat was in key; output trim -2.5 dB.

## Not covered
No GUI interaction testing beyond the existing headless editor tests.
