# Vivisect 0.9.3 Feature Specifications

This document defines the two user-facing features added after the 0.9.2 audit. These requirements are normative for implementation and tests.

## Feature 1: Randomize / Mutate Parameter Locks

### Goal
Let a user protect important controls while exploring variations. A locked parameter must not be changed by RANDOM or MUTATE.

### User interaction
- Every parameter control that already exposes the shared right-click menu gains a checked menu item:
  - "Lock for Randomize / Mutate" when unlocked.
  - "Unlock from Randomize / Mutate" when locked.
- The menu item must visibly indicate the current lock state with the popup-menu tick state.
- Locking must not change the parameter's current value.
- RESET remains an explicit full reset and ignores locks.
- Hard Reset clears every lock.
- A menu command must be available to clear all Randomize / Mutate locks at once.

### State and persistence
- Locks are user/session settings, not audio parameters.
- Lock state is stored in the VSX_SETTINGS subtree so project/session state restores it.
- Lock state must not be changed by factory presets, user presets, A/B comparison, or history rewind.
- Unknown/stale parameter IDs in saved lock data must be ignored safely.

### RANDOM behavior
- On the first RANDOM press, unlocked parameters randomize from the current state.
- On subsequent RANDOM presses, unlocked parameters reset to defaults before randomization as before.
- Locked parameters retain their exact normalized values across both the reset stage and randomization stage.
- The hidden FLATLINE controls remain excluded from RANDOM whether locked or not.
- RANDOM must still guarantee at least one surgeon is enabled among the surgeons that are not locked off. If every surgeon On control is locked off, RANDOM must not violate locks; the result may remain dry and diagnostics should be able to explain it.

### API/test requirements
- Processor methods: setParameterLocked(id, bool), isParameterLocked(id), clearParameterLocks(), lockedParameterCount().
- Invalid IDs are ignored.
- State round trip preserves locks.
- RANDOM leaves every locked parameter unchanged through repeated presses.

---

## Feature 2: Controlled MUTATE Variations

### Goal
Provide a musically useful alternative to full randomization: create a nearby variation of the current sound instead of replacing it.

### Controls
- Add an automatable parameter "Mutation Amount" with normalized range 0..1 and default 0.20.
- Add a momentary MUTATE button next to RANDOM.
- Add a small MUTATE AMT knob with a tooltip explaining that low values make subtle variations and high values make larger changes.
- MUTATE is an action, not a toggle and is not itself an automatable parameter.

### Mutation rules
- mutationAmount = 0 makes no parameter changes.
- Mutation operates in normalized parameter space and clamps every result to 0..1.
- Continuous creative parameters move by a random signed delta bounded by mutationAmount.
- Choice parameters may change only when mutationAmount >= 0.35; the probability of changing grows with amount.
- On/off surgeon controls may flip only when mutationAmount >= 0.55 and with a low probability.
- Parameter locks are always respected.
- Mutation must not change:
  - Input Trim
  - Output Trim
  - Buffer length
  - Source selection
  - MIDI Mode
  - Panic Freeze
  - Decay Arm
  - FLATLINE hidden controls
  - Mutation Amount itself
  - user settings, MIDI mappings, loaded samples or preset files
- SCAR Drive/Mix and creative surgeon/modulation controls may mutate.
- SCAR On may flip only at high mutation amount (>= 0.70).
- After mutation, if at least one surgeon On parameter is not locked, at least one surgeon must remain enabled. If all surgeon On controls are locked, their states are preserved exactly.
- Every changed APVTS parameter uses setValueNotifyingHost.

### Test requirements
- Amount 0 is bit-for-bit/no-normalized-value change.
- Repeated full-strength mutations keep all parameters within legal range and DSP output finite/bounded.
- Protected infrastructure controls listed above never change.
- Locked creative parameters never change.
- At least one eligible parameter changes over repeated nonzero mutations.
- Hidden FLATLINE cannot be revealed/enabled by mutation.

---

## Audit invariants introduced in 0.9.3

The full audit after these features must also enforce:
- A/B comparison changes sound state only; it must not change tooltips, MIDI mappings, parameter locks or other VSX_SETTINGS values.
- History rewind changes sound parameters only; it must not rewind VSX_SETTINGS.
- Loading a factory or user preset changes sound state only; it must not overwrite VSX_SETTINGS.
- Hard Reset restores documented default MIDI mappings after clearing custom mappings.
- The final build must pass the existing sample-rate/block-size matrix, parameter sweep, randomized fuzz, feedback stress, diagnostics, preset, MIDI, export, SCAR, FLATLINE, FREEZE, Euclidean and Random Walk regression tests.
