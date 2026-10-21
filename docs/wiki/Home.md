# Vivisect Wiki

Vivisect (repo: **faultline**) is a sample butchery VST. It treats the incoming audio as a specimen. A rolling live buffer (4, 8 or 16 bars) is analysed all the time, and a rack of six parallel "surgeons" cuts, granulates, reverses, corrupts, reorders and freezes slices, then reinjects the result. A single **Chaos** control moves the result from grid-locked, repeatable stutter (trip hop) to no-two-bars-alike glitch (industrial).

**Who it is for:** producers who want stutter, granular and glitch processing that still locks to the bar, and developers who want to know how the rack is built.

## Quick start

1. Download the zip for your system from the Releases page, or build it (see [[Developer-Build-and-Test]]).
2. Insert Vivisect on a track. Play some material through it.
3. Turn **Chaos** to the ORDER end for a locked stutter, or move it up for more movement.
4. Turn on one or two surgeons and raise their **Mix**. Use **Rhythmic Gravity** to snap slices to the grid.
5. Read [[User-Guide]] for every control.

## Pages

**For users**
- [[User-Guide]]: surgeons, master controls, RANDOM and MUTATE, mod matrix, MIDI, history
- [[Install]]: where each file goes on Windows and Linux

**For developers**
- [[Developer-Overview]]: architecture, design decisions, signal flow
- [[Developer-Code-Map]]: where everything lives and where to edit what
- [[Developer-Build-and-Test]]: toolchain, build, tests, CI, release
- [[Developer-Conventions]]: style, real-time rules, gotchas, known limitations

**Repo docs**
- `README.md`: the full design brief and status
- `ELI5.md`: plain-language overview
- `SPECS-0.9.3.md`, `SPECS-0.10.0.md`: version specs
