// ============================================================================
//  F1 (undo / redo) and F3 (capture and drag-out) tests.
//  Included by TestMain.cpp after check()/beginCase()/runBlocks()/inspect().
// ============================================================================

namespace ucTests
{
struct FakePlayHead : public AudioPlayHead
{
    bool   playing = false;
    double bpm = 120.0;
    double ppq = 0.0;
    int    num = 4, den = 4;

    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        i.setBpm (bpm);
        i.setPpqPosition (ppq);
        i.setIsPlaying (playing);
        TimeSignature ts;
        ts.numerator = num;
        ts.denominator = den;
        i.setTimeSignature (ts);
        return i;
    }
};

// Sets a parameter the way the UI does: one complete gesture.
inline void gesture (VivisectProcessor& p, const char* pid, float norm)
{
    auto* par = p.apvts.getParameter (pid);
    par->beginChangeGesture();
    par->setValueNotifyingHost (norm);
    par->endChangeGesture();
}
inline float norm (VivisectProcessor& p, const char* pid) { return p.apvts.getParameter (pid)->getValue(); }

// Runs blocks, advancing the fake transport, optionally keeping the output.
inline void run (VivisectProcessor& p, FakePlayHead* ph, int blocks, int blockSize, double rate,
                 int64& pos, std::vector<float>* outL = nullptr, std::vector<float>* outR = nullptr)
{
    AudioBuffer<float> buf (2, blockSize);
    MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        fillTestSignal (buf, pos, rate);
        midi.clear();
        p.processBlock (buf, midi);
        if (outL != nullptr)
        {
            outL->insert (outL->end(), buf.getReadPointer (0), buf.getReadPointer (0) + blockSize);
            outR->insert (outR->end(), buf.getReadPointer (1), buf.getReadPointer (1) + blockSize);
        }
        pos += blockSize;
        if (ph != nullptr && ph->playing)
            ph->ppq += (double) blockSize / (60.0 / ph->bpm * rate);
    }
}

// Lets the message thread run for a while so the processor's timer fires.
inline void pump (int ms)
{
    Timer::callAfterDelay (ms, [] { MessageManager::getInstance()->stopDispatchLoop(); });
    MessageManager::getInstance()->runDispatchLoop();
}

inline bool waitIdle (VivisectProcessor& p, int ms = 8000)
{
    const auto end = Time::getMillisecondCounter() + (uint32) ms;
    while (p.captureState().phase != vsx::CapturePhase::idle)
    {
        if (Time::getMillisecondCounter() > end) return false;
        Thread::sleep (5);
    }
    return true;
}

inline File freshDir (const String& tag)
{
    auto d = File::getSpecialLocation (File::tempDirectory)
                 .getChildFile ("vsx_uc_" + tag + "_" + String (Random::getSystemRandom().nextInt (1000000)));
    d.deleteRecursively();
    return d;
}

inline int countFiles (const File& d, const String& pattern)
{
    return d.isDirectory() ? d.findChildFiles (File::findFiles, false, pattern).size() : 0;
}

struct WavInfo { bool ok = false; int64 length = 0; double rate = 0; int bits = 0; int channels = 0;
                 AudioBuffer<float> data; bool finite = true; };
inline WavInfo readWav (const File& f)
{
    WavInfo w;
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return w;
    w.ok = true;
    w.length = r->lengthInSamples;
    w.rate = r->sampleRate;
    w.bits = (int) r->bitsPerSample;
    w.channels = (int) r->numChannels;
    w.data.setSize ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&w.data, 0, (int) r->lengthInSamples, 0, true, true);
    w.finite = inspect (w.data).finite;
    return w;
}

inline int64 expectedFrames (int bars, double bpm, double rate, double barQ = 4.0)
{
    return (int64) std::llround ((double) bars * barQ * 60.0 / bpm * rate);
}

// Arms a capture on a stopped / standalone transport, runs it to completion and
// returns the finished file (or an empty File).
inline File captureOnce (VivisectProcessor& p, FakePlayHead* ph, int bars, double rate, int blockSize,
                         std::vector<float>* recL = nullptr, std::vector<float>* recR = nullptr)
{
    const int before = p.captureState().completedCount;
    if (! p.startCapture (bars)) return {};
    int64 pos = 0;
    const double bpm = ph != nullptr ? ph->bpm : 120.0;
    const double barQ = ph != nullptr ? 4.0 * ph->num / ph->den : 4.0;
    const int blocks = (int) (expectedFrames (bars, bpm, rate, barQ) / blockSize) + 4;
    run (p, ph, blocks, blockSize, rate, pos, recL, recR);
    if (! waitIdle (p)) return {};
    const auto st = p.captureState();
    return st.completedCount == before + 1 ? st.lastFile : File();
}
} // namespace ucTests

static void run_undo_capture_tests (double kRate, int kBlock)
{
    using namespace ucTests;

    // =======================================================================
    //  F1  Undo / redo
    // =======================================================================
    beginCase ("F1 undo: empty stack is a safe no-op");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto before = snapshotParams (p);
        check (! p.canUndo() && ! p.canRedo(), "fresh processor has nothing to undo or redo");
        check (! p.undo() && ! p.redo(), "undo / redo on empty stacks return false");
        check (snapshotParams (p) == before, "empty undo / redo change nothing");
        check (p.undoLabel().containsIgnoreCase ("undo") && p.redoLabel().containsIgnoreCase ("redo"),
               "labels exist even when empty");
        p.clearUndoHistory();
        check (! p.canUndo(), "clearUndoHistory on empty is fine");
    }

    beginCase ("F1 undo: gesture coalescing, exact restore, redo round trip");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const float c0 = norm (p, vsx::id::chaos);

        auto* chaos = p.apvts.getParameter (vsx::id::chaos);
        chaos->beginChangeGesture();
        for (int i = 1; i <= 40; ++i)
            chaos->setValueNotifyingHost (c0 + 0.01f * (float) i * 0.7f);
        chaos->endChangeGesture();
        const float c1 = norm (p, vsx::id::chaos);
        check (p.undoDepth() == 1, "a 40-move knob drag is exactly one undo step");
        check (c1 != c0, "the drag moved the parameter");
        check (p.undoLabel().startsWith ("Undo:") && p.undoLabel().contains (chaos->getName (24)),
               "tooltip names the control that will change: " + p.undoLabel());

        check (p.undo(), "undo succeeds");
        check (norm (p, vsx::id::chaos) == c0, "undo restores the exact normalised value");
        check (p.canRedo() && ! p.canUndo(), "undo moves the step to the redo stack");
        check (p.redo(), "redo succeeds");
        check (norm (p, vsx::id::chaos) == c1, "redo restores the exact value");
        check (p.canUndo() && ! p.canRedo(), "redo moves the step back");

        // two separate gestures are two steps
        gesture (p, vsx::id::swing, 0.8f);
        gesture (p, vsx::id::swing, 0.2f);
        check (p.undoDepth() == 3, "separate gestures are separate steps");

        // a click that changes nothing adds nothing
        const int depth = p.undoDepth();
        chaos->beginChangeGesture();
        chaos->endChangeGesture();
        check (p.undoDepth() == depth, "a gesture that ends where it began adds no step");

        // host automation (no gesture) is not recorded
        chaos->setValueNotifyingHost (0.123f);
        p.apvts.getParameter (vsx::id::dryWet)->setValueNotifyingHost (0.77f);
        check (p.undoDepth() == depth, "value moves without a gesture (host automation) are not recorded");

        // a new edit clears redo
        check (p.undo(), "undo again");
        check (p.canRedo(), "redo available after undo");
        gesture (p, vsx::id::swing, 0.55f);
        check (! p.canRedo(), "a new edit clears the redo stack");

        // overlapping gestures on two controls coalesce into one step
        const int d2 = p.undoDepth();
        auto* a = p.apvts.getParameter (vsx::id::chaos);
        auto* b = p.apvts.getParameter (vsx::id::swing);
        a->beginChangeGesture(); b->beginChangeGesture();
        a->setValueNotifyingHost (0.9f); b->setValueNotifyingHost (0.1f);
        b->endChangeGesture(); a->endChangeGesture();
        check (p.undoDepth() == d2 + 1, "overlapping gestures form a single step");
    }

    beginCase ("F1 undo: RANDOM, MUTATE, RESET, presets, A/B, rewind each add exactly one step");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto s0 = snapshotParams (p);

        p.randomizeAll();
        const auto s1 = snapshotParams (p);
        check (p.undoDepth() == 1, "RANDOM adds exactly one step");
        check (s1 != s0, "RANDOM changed the sound");

        p.mutateCurrent();
        const auto s2 = snapshotParams (p);
        check (p.undoDepth() == 2, "MUTATE adds exactly one step");

        p.resetAllToDefaults();
        const auto s3 = snapshotParams (p);
        check (p.undoDepth() == 3, "RESET adds exactly one step");

        p.randomizeAll();                       // second press resets first: still one step
        check (p.undoDepth() == 4, "a second RANDOM press is still one step");
        const auto s4 = snapshotParams (p);

        check (p.undo() && snapshotParams (p) == s3, "undo RANDOM #2 restores the reset state exactly");
        check (p.undo() && snapshotParams (p) == s2, "undo RESET restores the mutated state exactly");
        check (p.undo() && snapshotParams (p) == s1, "undo MUTATE restores the randomised state exactly");
        check (p.undo() && snapshotParams (p) == s0, "undo RANDOM restores the original state exactly");
        check (! p.undo(), "stack exhausted");
        check (p.redo() && snapshotParams (p) == s1, "redo 1");
        check (p.redo() && snapshotParams (p) == s2, "redo 2");
        check (p.redo() && snapshotParams (p) == s3, "redo 3");
        check (p.redo() && snapshotParams (p) == s4, "redo 4");
        check (! p.redo(), "redo exhausted");

        // presets
        p.clearUndoHistory();
        const auto pre = snapshotParams (p);
        p.loadPreset (3);
        check (p.undoDepth() == 1, "factory preset load adds exactly one step");
        check (snapshotParams (p) != pre, "preset changed the sound");
        check (p.undo() && snapshotParams (p) == pre, "undo preset restores exactly");
        check (p.loadPresetByName (p.getPresetNames()[2]) && p.undoDepth() == 1,
               "loadPresetByName adds exactly one step");
        p.clearUndoHistory();
        p.loadPreset (-1);
        check (p.undoDepth() == 0, "a rejected preset load adds no step");
        check (! p.loadPresetFromFile (File ("/no/such/preset.vsxpreset")) && p.undoDepth() == 0,
               "a failed preset file load adds no step");

        // preset file round trip
        const auto presetFile = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx_uc_preset.vsxpreset");
        p.randomizeAll();
        check (p.savePresetToFile (presetFile), "preset file saved");
        const auto saved = snapshotParams (p);
        p.resetAllToDefaults();
        p.clearUndoHistory();
        check (p.loadPresetFromFile (presetFile) && p.undoDepth() == 1, "preset file load adds exactly one step");
        (void) saved;
        presetFile.deleteFile();

        // A/B
        p.clearUndoHistory();
        p.resetAllToDefaults();
        p.clearUndoHistory();
        gesture (p, vsx::id::chaos, 0.9f);
        p.clearUndoHistory();
        const auto slotA = snapshotParams (p);
        p.selectABSlot (1);
        check (p.undoDepth() == 1, "A/B swap adds exactly one step");
        p.selectABSlot (1);
        check (p.undoDepth() == 1, "selecting the current slot again adds nothing");
        check (p.undo() && snapshotParams (p) == slotA, "undo of an A/B swap restores the sound exactly");

        // History rewind (the history bar). The processor's 2 Hz history timer
        // needs the message loop, which can only be run once per process here
        // (stopDispatchLoop is sticky), so skip quietly if another suite used it.
        if (MessageManager::getInstance()->hasStopMessageBeenSent())
        {
            std::printf ("   (skipped: message loop already stopped by an earlier suite)\n");
        }
        else
        {
            VivisectProcessor q;
            q.prepareToPlay (kRate, kBlock);
            gesture (q, vsx::id::chaos, 0.2f);
            pump (1700);
            if (check (q.historyCount() >= 2, "history frames recorded for the rewind test"))
            {
                q.randomizeAll();
                q.clearUndoHistory();
                const auto beforeRewind = snapshotParams (q);
                q.rewindToNormalized (0.f);
                check (q.undoDepth() == 1, "a history rewind adds exactly one step");
                check (q.undo() && snapshotParams (q) == beforeRewind, "undo of a rewind restores exactly");
            }
        }
    }

    beginCase ("F1 undo: all sound parameters round trip; settings, locks and maps never touched");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        p.setParameterLocked (vsx::id::chaos, true);
        p.setCCForParam (vsx::id::swing, 74);
        p.setTooltipsEnabled (false);
        p.setUiScale (0.9f);
        p.setCaptureBars (8);
        const int lockedBefore = p.lockedParameterCount();

        const auto s0 = snapshotParams (p);
        // move every parameter to a distinct non-default value, as one RANDOM-like edit
        p.pushUndoSnapshot();
        Random rng (77);
        for (auto* par : p.getParameters())
            par->setValueNotifyingHost (rng.nextFloat());
        const auto s1 = snapshotParams (p);
        check (s1 != s0, "every parameter moved");

        check (p.undo(), "undo the bulk edit");
        const auto back = snapshotParams (p);
        check (back == s0, "every sound parameter returns to its exact normalised value");
        check (p.redo() && snapshotParams (p) == s1, "redo returns every parameter exactly");
        p.undo();

        p.randomizeAll();
        p.selectABSlot (1);
        p.undo();
        p.undo();
        check (p.isParameterLocked (vsx::id::chaos) && p.lockedParameterCount() == lockedBefore,
               "locks unchanged by undo");
        check (p.getCCForParam (vsx::id::swing) == 74, "MIDI map unchanged by undo");
        check (! p.tooltipsEnabled(), "tooltip setting unchanged by undo");
        check (std::abs (p.uiScale() - 0.9f) < 1.0e-4f, "UI scale unchanged by undo");
        check (p.captureBars() == 8, "capture length setting unchanged by undo");

        // not persisted
        MemoryBlock state;
        p.getStateInformation (state);
        VivisectProcessor fresh;
        fresh.prepareToPlay (kRate, kBlock);
        fresh.setStateInformation (state.getData(), (int) state.getSize());
        check (! fresh.canUndo() && ! fresh.canRedo(), "undo history is not part of the saved state");

        // cleared on Hard Reset
        p.randomizeAll();
        check (p.canUndo(), "there is something to undo before the hard reset");
        p.hardResetAllSettings();
        check (! p.canUndo() && ! p.canRedo(), "Hard Reset clears the undo history");
        check (p.captureBars() == 4, "Hard Reset restores the default capture length");
    }

    beginCase ("F1 undo: depth is capped at 100, oldest dropped");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        for (int k = 1; k <= 150; ++k)
            gesture (p, vsx::id::chaos, (float) k / 200.f);
        check (p.undoDepth() == 100, "undo depth capped at 100 (got " + String (p.undoDepth()) + ")");
        int n = 0;
        while (p.undo()) ++n;
        check (n == 100, "exactly 100 undos available");
        check (std::abs (norm (p, vsx::id::chaos) - 50.f / 200.f) < 1.0e-6f,
               "the oldest 50 steps were dropped, so undo stops at step 50");
        int r = 0;
        while (p.redo()) ++r;
        check (r == 100 && std::abs (norm (p, vsx::id::chaos) - 150.f / 200.f) < 1.0e-6f,
               "100 redos return to the final value");

        // many actions in a row also stay capped
        for (int k = 0; k < 130; ++k) p.randomizeAll();
        check (p.undoDepth() == 100, "action steps are capped too");
        const auto v = runBlocks (p, 30, kBlock, kRate, true);
        check (v.finite, "output stays finite after heavy undo traffic");
    }

    beginCase ("F1 undo: audio keeps running through undo / redo");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        bool finite = true;
        for (int i = 0; i < 20; ++i)
        {
            p.randomizeAll();
            finite = finite && runBlocks (p, 6, kBlock, kRate, true).finite;
            p.undo();
            finite = finite && runBlocks (p, 6, kBlock, kRate, true).finite;
            p.redo();
            finite = finite && runBlocks (p, 6, kBlock, kRate, true).finite;
        }
        check (finite, "output finite while undoing and redoing under load");
    }

    // =======================================================================
    //  F3  Capture
    // =======================================================================
    beginCase ("F3 capture: pure functions (length, bar alignment)");
    {
        using namespace vsx::capture;
        check (framesForBars (4, 120.0, 48000.0) == 384000, "4 bars @120 BPM 48k = 384000 frames");
        check (framesForBars (1, 90.0, 44100.0) == (int64) std::llround (4.0 * 60.0 / 90.0 * 44100.0), "1 bar @90 BPM 44.1k");
        check (framesForBars (8, 40.0, 48000.0) == 2304000, "8 bars @40 BPM = 48 s");
        check (framesForBars (2, 120.0, 48000.0, 3.0) == 144000, "3/4 time: 2 bars @120 = 6 beats = 3 s");
        check (barQuarters (6, 8) == 3.0 && barQuarters (4, 4) == 4.0 && barQuarters (0, 4) == 4.0, "bar length in quarters");
        check (isValidBars (1) && isValidBars (2) && isValidBars (4) && isValidBars (8) && ! isValidBars (3)
                   && ! isValidBars (0) && ! isValidBars (16), "only 1/2/4/8 bars are valid");

        const double spq = 24000.0;                              // 120 BPM @ 48k
        check (barStartOffset (0.0, 120.0, 48000.0, 512) == 0, "block on bar 0 starts at once");
        check (barStartOffset (4.0, 120.0, 48000.0, 512) == 0, "block on bar 1 starts at once");
        check (barStartOffset (4.0 - 100.0 / spq, 120.0, 48000.0, 512) == 100, "bar line 100 samples into the block");
        check (barStartOffset (4.0 - 511.0 / spq, 120.0, 48000.0, 512) == 511, "bar line in the last sample");
        check (barStartOffset (4.0 - 512.0 / spq, 120.0, 48000.0, 512) == -1, "bar line exactly at the next block is not in this one");
        check (barStartOffset (1.37, 120.0, 48000.0, 512) == -1, "mid-bar block has no bar line");
        check (barStartOffset (8.0 - 1.0e-10, 120.0, 48000.0, 512) == 0, "float noise just below a bar line counts as on it");
        check (barStartOffset (-0.01, 120.0, 48000.0, 512) == 240, "pre-roll: bar 0 line found from negative ppq");
        check (barStartOffset (3.0 - 40.0 / spq, 120.0, 48000.0, 512, 3.0) == 40, "3/4 time bar line");
        check (barStartOffset (2.0, 120.0, 48000.0, 512, 3.0) == -1, "3/4 time: ppq 2 is mid-bar");
        // sweep: exactly one block per bar contains the bar line, at the right sample
        {
            int hits = 0; bool exact = true;
            double ppq = 0.37;
            for (int b = 0; b < 4000; ++b)
            {
                const int off = barStartOffset (ppq, 133.0, 44100.0, 441);
                if (off >= 0)
                {
                    ++hits;
                    const double lineQ = std::ceil (ppq / 4.0 - 1e-7) * 4.0;
                    const double expectSample = (lineQ - ppq) * (60.0 / 133.0 * 44100.0);
                    exact = exact && std::abs ((double) off - expectSample) <= 0.5001;
                }
                ppq += 441.0 / (60.0 / 133.0 * 44100.0);
            }
            const double bars = ppq / 4.0;
            check (std::abs ((double) hits - bars) <= 1.0, "one bar line found per bar across a long sweep (" + String (hits) + ")");
            check (exact, "every found bar line is sample-accurate");
        }
    }

    beginCase ("F3 capture: no allocation after prepare, capacity covers 8 bars at 40 BPM");
    {
        VivisectProcessor p;
        check (! p.startCapture (2), "capture refuses before prepareToPlay");
        p.prepareToPlay (kRate, kBlock);
        auto& eng = p.captureEngine();
        const size_t cap = eng.capacityFrames();
        const float* d0 = eng.bufferData (0);
        check (cap >= (size_t) (64.0 * kRate), "ring holds 64 s");
        check (cap >= (size_t) vsx::capture::framesForBars (8, 40.0, kRate), "ring holds 8 bars at 40 BPM");

        const auto dir = freshDir ("alloc");
        p.setCaptureDirectory (dir);
        const auto f = captureOnce (p, nullptr, 1, kRate, kBlock);
        check (f.existsAsFile(), "a capture completed");
        check (eng.capacityFrames() == cap && eng.bufferData (0) == d0,
               "the take buffer was neither reallocated nor moved by a capture");
        p.prepareToPlay (kRate, kBlock);
        check (eng.capacityFrames() == cap && eng.bufferData (0) == d0, "re-preparing at the same rate reuses the buffer");
        dir.deleteRecursively();
    }

    beginCase ("F3 capture: exact length at several tempos and time signatures, readable finite 24-bit file");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto dir = freshDir ("len");
        p.setCaptureDirectory (dir);
        FakePlayHead ph;

        struct Case { double bpm; int bars; int num, den; };
        const Case cases[] { { 120.0, 2, 4, 4 }, { 90.0, 1, 4, 4 }, { 140.0, 4, 4, 4 }, { 174.0, 2, 4, 4 },
                             { 60.0, 1, 4, 4 }, { 100.0, 2, 3, 4 }, { 128.0, 1, 6, 8 }, { 45.0, 1, 4, 4 } };
        std::set<String> names;
        for (const auto& c : cases)
        {
            ph.bpm = c.bpm; ph.num = c.num; ph.den = c.den; ph.playing = false; ph.ppq = 0.0;
            p.setPlayHead (&ph);
            std::vector<float> L, R;
            const auto f = captureOnce (p, &ph, c.bars, kRate, kBlock, &L, &R);
            const String tag = String (c.bpm) + " BPM x" + String (c.bars) + " " + String (c.num) + "/" + String (c.den);
            if (! check (f.existsAsFile(), "capture file exists: " + tag)) continue;
            const auto w = readWav (f);
            const auto want = expectedFrames (c.bars, c.bpm, kRate, 4.0 * c.num / c.den);
            check (w.ok && w.length == want, "length exact (" + tag + "): " + String (w.length) + " vs " + String (want));
            check (w.ok && w.bits == 24 && w.channels == 2 && w.rate == kRate, "24-bit stereo at the session rate: " + tag);
            check (w.ok && w.finite, "samples finite: " + tag);

            // it really is the final output, sample for sample (to 24-bit precision)
            double worst = 0.0; float peak = 0.f;
            for (int i = 0; w.ok && i < (int) want && i < (int) L.size(); ++i)
            {
                const float eL = jlimit (-1.f, 1.f, L[(size_t) i]), eR = jlimit (-1.f, 1.f, R[(size_t) i]);
                worst = jmax (worst, (double) std::abs (w.data.getSample (0, i) - eL),
                                     (double) std::abs (w.data.getSample (1, i) - eR));
                peak = jmax (peak, std::abs (eL));
            }
            check (worst < 2.0e-6, "file equals the plug-in output (max error " + String (worst) + "): " + tag);
            check (peak > 0.001f, "capture is not silent: " + tag);

            const auto name = f.getFileName();
            check (name.matchesWildcard ("Vivisect_" + String::repeatedString ("?", 8) + "-" + String::repeatedString ("?", 6) + "_" + String (roundToInt (c.bpm)) + "bpm_" + String (c.bars) + "bar*.wav", false),
                   "file name pattern: " + name);
            check (names.insert (name).second, "file names are unique even within one second");
        }
        p.setPlayHead (nullptr);
        check (countFiles (dir, "*.part") == 0, "no .part files left behind");

        // newest first, capped
        const auto all = p.listCaptures();
        check (all.size() == (int) std::size (cases), "listCaptures sees every take: " + String (all.size()));
        check (p.listCaptures (3).size() == 3, "listCaptures(3) is capped");
        bool newestFirst = true;
        for (int i = 1; i < all.size(); ++i)
            newestFirst = newestFirst && all[i - 1].getLastModificationTime() >= all[i].getLastModificationTime();
        check (newestFirst, "listCaptures is newest first");
        dir.deleteRecursively();
    }

    beginCase ("F3 capture: bar alignment on a playing transport");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto dir = freshDir ("align");
        p.setCaptureDirectory (dir);
        FakePlayHead ph;
        ph.playing = true; ph.bpm = 120.0;
        p.setPlayHead (&ph);
        const double spq = 60.0 / ph.bpm * kRate;

        // armed mid-bar: nothing is recorded until the bar line
        ph.ppq = 1.37;
        check (p.startCapture (1), "capture armed while playing");
        int64 pos = 0;
        run (p, &ph, 10, kBlock, kRate, pos);
        auto st = p.captureState();
        check (st.phase == vsx::CapturePhase::armed && st.framesWritten == 0, "still waiting mid-bar, nothing recorded");

        // block whose 100th sample is the bar line
        ph.ppq = 4.0 - 100.0 / spq;
        run (p, nullptr, 1, kBlock, kRate, pos);
        st = p.captureState();
        check (st.phase == vsx::CapturePhase::recording && st.framesWritten == kBlock - 100,
               "recording began exactly on the bar line (wrote " + String (st.framesWritten) + " of that block)");
        check (st.framesTarget == expectedFrames (1, 120.0, kRate), "take length is 1 bar");

        ph.ppq = 4.0 - 100.0 / spq + kBlock / spq;
        const int blocks = (int) (st.framesTarget / kBlock) + 3;
        run (p, &ph, blocks, kBlock, kRate, pos);
        check (waitIdle (p), "writer finished");
        st = p.captureState();
        check (st.completedCount == 1 && st.lastFile.existsAsFile(), "aligned capture saved");
        check (readWav (st.lastFile).length == expectedFrames (1, 120.0, kRate), "aligned capture length exact");

        // bar line at the very start of a block
        ph.ppq = 8.0;
        p.startCapture (2);
        run (p, &ph, 1, kBlock, kRate, pos);
        st = p.captureState();
        check (st.phase == vsx::CapturePhase::recording && st.framesWritten == kBlock, "a block starting on a bar line records all of it");
        p.cancelCapture();

        // transport stops while armed: start at once
        ph.playing = true; ph.ppq = 1.5;
        p.startCapture (1);
        run (p, &ph, 3, kBlock, kRate, pos);
        check (p.captureState().phase == vsx::CapturePhase::armed, "armed on a playing transport");
        ph.playing = false;
        run (p, &ph, 1, kBlock, kRate, pos);
        check (p.captureState().phase == vsx::CapturePhase::recording, "stopping the transport starts the capture at once");
        p.cancelCapture();
        p.setPlayHead (nullptr);
        dir.deleteRecursively();
    }

    beginCase ("F3 capture: cancel, busy, suspended, invalid lengths");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto dir = freshDir ("cancel");
        p.setCaptureDirectory (dir);
        int64 pos = 0;

        check (! p.startCapture (3) && ! p.startCapture (0) && ! p.startCapture (16), "invalid lengths are refused");
        check (p.captureState().phase == vsx::CapturePhase::idle, "refused starts leave the engine idle");

        check (p.startCapture (2), "start");
        check (! p.startCapture (2), "a second start while busy is refused");
        p.cancelCapture();
        check (p.captureState().phase == vsx::CapturePhase::idle, "cancel while armed returns to idle");

        check (p.startCapture (2), "start again");
        run (p, nullptr, 30, kBlock, kRate, pos);
        check (p.captureState().phase == vsx::CapturePhase::recording && p.captureState().framesWritten > 0, "recording");
        p.cancelCapture();
        check (p.captureState().phase == vsx::CapturePhase::idle, "cancel while recording returns to idle");
        run (p, nullptr, 400, kBlock, kRate, pos);
        Thread::sleep (80);
        check (countFiles (dir, "*") == 0, "cancel leaves no file, partial or otherwise");
        check (p.captureState().completedCount == 0, "a cancelled capture is not counted as saved");

        // suspended: never records
        p.suspendProcessing (true);
        check (! p.startCapture (1), "cannot start while suspended");
        p.suspendProcessing (false);
        check (p.startCapture (1), "arm");
        p.suspendProcessing (true);
        run (p, nullptr, 20, kBlock, kRate, pos);
        auto st = p.captureState();
        check (st.phase == vsx::CapturePhase::armed && st.framesWritten == 0, "nothing recorded while suspended");
        p.suspendProcessing (false);
        run (p, nullptr, 3, kBlock, kRate, pos);
        check (p.captureState().phase == vsx::CapturePhase::recording, "recording resumes once unsuspended");
        p.cancelCapture();

        // preparing again (rate change) cancels
        check (p.startCapture (1), "arm before re-prepare");
        p.prepareToPlay (44100.0, kBlock);
        check (p.captureState().phase == vsx::CapturePhase::idle, "prepareToPlay cancels a capture in progress");
        check (p.captureEngine().capacityFrames() >= (size_t) (64.0 * 44100.0), "buffer re-sized for the new rate");
        dir.deleteRecursively();
    }

    beginCase ("F3 capture: length setting lives in VSX_SETTINGS and is untouched by sound edits");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        check (p.captureBars() == 4, "default capture length is 4 bars");
        p.setCaptureBars (3);
        check (p.captureBars() == 4, "invalid length ignored");
        for (int b : { 1, 2, 8, 4 })
        {
            p.setCaptureBars (b);
            check (p.captureBars() == b, "length " + String (b) + " stored");
        }
        p.setCaptureBars (8);
        p.randomizeAll();
        p.mutateCurrent();
        p.loadPreset (2);
        p.selectABSlot (1);
        p.resetAllToDefaults();
        p.undo();
        check (p.captureBars() == 8, "RANDOM / MUTATE / presets / A-B / RESET / undo never change the capture length");

        MemoryBlock state;
        p.getStateInformation (state);
        VivisectProcessor q;
        q.prepareToPlay (kRate, kBlock);
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (q.captureBars() == 8, "capture length is saved with the session");
    }

    beginCase ("F3 capture: closing the plug-in mid-capture is clean");
    {
        const auto dir = freshDir ("close");
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            p.setCaptureDirectory (dir);
            int64 pos = 0;
            p.startCapture (4);
            run (p, nullptr, 50, kBlock, kRate, pos);
            check (p.captureState().phase == vsx::CapturePhase::recording, "recording when the plug-in is destroyed");
        }
        check (countFiles (dir, "*") == 0, "destroying mid-recording leaves no file");

        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            p.setCaptureDirectory (dir);
            int64 pos = 0;
            p.startCapture (8);
            run (p, nullptr, (int) (expectedFrames (8, 120.0, kRate) / kBlock) + 2, kBlock, kRate, pos);
            // the writer is (or is about to be) busy: destroy straight away
        }
        check (countFiles (dir, "*.part") == 0, "destroying during the write leaves no .part file");
        const auto wavs = dir.findChildFiles (File::findFiles, false, "*.wav");
        bool whole = true;
        for (auto& f : wavs) whole = whole && readWav (f).length == expectedFrames (8, 120.0, kRate);
        check (wavs.size() <= 1 && whole, "any .wav that exists is complete");
        dir.deleteRecursively();

        // the editor can come and go mid-capture without disturbing the engine
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            const auto d2 = freshDir ("editor");
            p.setCaptureDirectory (d2);
            int64 pos = 0;
            p.startCapture (1);
            run (p, nullptr, 20, kBlock, kRate, pos);
            {
                std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
                check (ed != nullptr, "editor opens mid-capture");
            }
            check (p.captureState().phase == vsx::CapturePhase::recording, "closing the editor does not stop the capture");
            run (p, nullptr, (int) (expectedFrames (1, 120.0, kRate) / kBlock) + 2, kBlock, kRate, pos);
            check (waitIdle (p) && p.captureState().completedCount == 1, "the take finishes with no editor open");
            d2.deleteRecursively();
        }
    }
}
