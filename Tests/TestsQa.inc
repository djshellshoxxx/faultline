// ============================================================================
//  QA pass (0.10.0 beta): randomized soak / fuzz plus regression tests for bugs
//  the pass found. Optional suite; needs TestsUndoCapture.inc (FakePlayHead).
//  Included by TestMain.cpp after check()/beginCase()/inspect()/fillTestSignal().
// ============================================================================
#include "Musical.h"

namespace qaTests
{
// Counts positive-going zero crossings in x[from, to) and returns the frequency
// estimated from the first to the last crossing (Hz), or 0 if too few crossings.
inline double estimateFreq (const std::vector<float>& x, int from, int to, double rate)
{
    int first = -1, last = -1, n = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[(size_t) i - 1] < 0.f && x[(size_t) i] >= 0.f)
        {
            if (first < 0) first = i;
            last = i;
            ++n;
        }
    if (n < 4) return 0.0;
    return (double) (n - 1) * rate / (double) (last - first);
}

struct RenderedFirstRepeat { double ratio = 0.0; bool ok = false; };

// Triggers a lone STUTTER surgeon directly (one repeat length == slice, so the
// first repeat is the whole audible event) and measures its pitch ratio.
inline RenderedFirstRepeat stutterFirstRepeatRatio (const vsx::ScaleLock& lock, unsigned rngSeed)
{
    constexpr double rate = 48000.0;
    constexpr double srcHz = 1000.0;
    vsx::SpecimenBuffer spec;
    spec.prepare (rate, 4.0);
    AudioBuffer<float> in (2, 512);
    for (int b = 0; b < (int) (2.0 * rate / 512); ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            const float s = 0.5f * (float) std::sin (MathConstants<double>::twoPi * srcHz
                                                       * (double) (b * 512 + i) / rate);
            in.setSample (0, i, s);
            in.setSample (1, i, s);
        }
        spec.push (in);
    }

    auto stutter = vsx::makeSurgeon (vsx::S_STUTTER);
    stutter->prepare (rate, 512);
    stutter->setParams (0.35f, 0.f, 0.f);          // pp2 0 -> ratchet 1, DRIFT 0
    stutter->setScale (lock);

    juce::Random rng ((int) rngSeed);
    vsx::TriggerContext ctx;
    ctx.nowPos = spec.writePos();
    ctx.sourcePos = (double) spec.writePos() - 0.5 * rate;
    ctx.lengthSamples = 12000.0;
    ctx.chaos = 0.f;
    ctx.pitchChaos = 0.f;
    ctx.rng = &rng;
    ctx.specimen = &spec;
    stutter->trigger (ctx);

    std::vector<float> out;
    AudioBuffer<float> add (2, 512);
    for (int b = 0; b < 24; ++b)
    {
        add.clear();
        stutter->process (add, 512);
        out.insert (out.end(), add.getReadPointer (0), add.getReadPointer (0) + 512);
    }
    RenderedFirstRepeat r;
    // first repeat is ~12000 samples at ratio >= 0.5; skip the fade-in, stay inside it
    const double f = estimateFreq (out, 200, 6000, rate);
    if (f > 0.0) { r.ok = true; r.ratio = f / srcHz; }
    return r;
}

inline float qaGainOf (VivisectProcessor& p, const char* pid)
{
    if (auto* par = p.apvts.getParameter (pid))
        return Decibels::decibelsToGain (par->getNormalisableRange().convertFrom0to1 (par->getValue()));
    return 1.f;
}
} // namespace qaTests

static void run_qa_tests (double /*kRate*/, int /*kBlock*/)
{
    using namespace qaTests;
    using vsx::ScaleQuantizer;

    // -----------------------------------------------------------------------
    beginCase ("QA: STUTTER's FIRST repeat already sits on a scale degree (non-C root)");
    {
        // D Major (root 2, type 1) does not contain C (offset 0), so a unison first repeat is out of key.
        bool allInKey = true, anyMeasured = false, anyUnison = false;
        String detail;
        for (int root : { 2, 4, 6, 9 })
            for (int type : { 1, 2, 8 })
            {
                vsx::ScaleLock lock;
                lock.on = true; lock.root = root; lock.type = type; lock.range = 0.25f;   // +-3 semitones
                if (ScaleQuantizer::isInScale (0, root, type)) continue;                    // unison would be legal here
                for (unsigned seed = 1; seed <= 6; ++seed)
                {
                    const auto r = stutterFirstRepeatRatio (lock, seed);
                    if (! r.ok) continue;
                    anyMeasured = true;
                    const double semis = 12.0 * std::log2 (r.ratio);
                    const int nearest = (int) std::lround (semis);
                    if (std::abs (semis - nearest) > 0.2 || ! ScaleQuantizer::isInScale (nearest, root, type))
                    {
                        allInKey = false;
                        detail = "root " + String (root) + " type " + String (type) + " got " + String (semis, 2) + " st";
                    }
                    if (nearest == 0) anyUnison = true;
                }
            }
        check (anyMeasured, "the first repeat's pitch could be measured");
        check (allInKey, "every measured first repeat is in key (" + detail + ")");
        check (! anyUnison, "the first repeat is never unison when unison is out of key");

        // Off path: the first repeat stays exactly unison (legacy behaviour).
        vsx::ScaleLock off;
        const auto r = stutterFirstRepeatRatio (off, 3);
        check (r.ok && std::abs (r.ratio - 1.0) < 0.01, "with Scale Lock off the first repeat is still unison");
    }

    // -----------------------------------------------------------------------
    beginCase ("QA: undo / A-B / preset while a drag gesture is still open");
    {
        auto setNorm = [] (VivisectProcessor& p, const char* pid, float v) { p.apvts.getParameter (pid)->setValueNotifyingHost (v); };
        auto getNorm = [] (VivisectProcessor& p, const char* pid) { return p.apvts.getParameter (pid)->getValue(); };
        VivisectProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.clearUndoHistory();
        setNorm (p, vsx::id::chaos, 0.2f);
        p.clearUndoHistory();

        auto* chaos = p.apvts.getParameter (vsx::id::chaos);
        chaos->beginChangeGesture();                 // drag starts
        setNorm (p, vsx::id::chaos, 0.8f);
        check (p.undo(), "Ctrl+Z during a drag is accepted");
        check (std::abs (getNorm (p, vsx::id::chaos) - 0.2f) < 1.0e-5f, "Ctrl+Z during a drag undoes that drag");
        chaos->endChangeGesture();
        check (p.redo() && std::abs (getNorm (p, vsx::id::chaos) - 0.8f) < 1.0e-5f, "...and redo brings the dragged value back");
        check (! p.canRedo(), "closing the stale gesture afterwards adds nothing");

        // A/B swap mid drag: the drag is one step, the swap another, undo walks back in order
        p.clearUndoHistory();
        setNorm (p, vsx::id::chaos, 0.3f);
        p.clearUndoHistory();
        chaos->beginChangeGesture();
        setNorm (p, vsx::id::chaos, 0.6f);
        p.selectABSlot (1);
        chaos->endChangeGesture();
        check (p.undoDepth() == 2, "drag and A/B swap are two separate steps (" + String (p.undoDepth()) + ")");
        p.undo();                                    // undo the swap
        check (std::abs (getNorm (p, vsx::id::chaos) - 0.6f) < 1.0e-5f, "undoing the swap returns to the dragged value");
        p.undo();                                    // undo the drag
        check (std::abs (getNorm (p, vsx::id::chaos) - 0.3f) < 1.0e-5f, "undoing the drag returns to the value before it");
    }

    // -----------------------------------------------------------------------
    beginCase ("QA: randomized soak (all parameters x RANDOM/MUTATE/undo/redo/preset/A-B/capture)");
    {
        const double rates[]  { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 };
        const int    blocks[] { 1, 7, 64, 512, 8192 };
        juce::Random rng (0x0A11CE);

        auto captureDir = File::getSpecialLocation (File::tempDirectory)
                              .getChildFile ("vsx_qa_capture_" + String (rng.nextInt (1000000)));
        captureDir.deleteRecursively();

        int scenarios = 0, ops = 0, nonFinite = 0, overCeiling = 0;
        float worst = 0.f;
        String firstBad;

        for (double rate : rates)
            for (int block : blocks)
            {
                ++scenarios;
                VivisectProcessor p;
                p.setCaptureDirectory (captureDir);
                p.prepareToPlay (rate, block);
                ucTests::FakePlayHead ph;
                p.setPlayHead (&ph);
                ph.bpm = 20.0 + rng.nextDouble() * 280.0;
                ph.playing = rng.nextBool();

                AudioBuffer<float> buf (2, block);
                MidiBuffer midi;
                int64 pos = 0;
                // roughly 0.35 s of audio per chunk; block 1 gets a short chunk so it stays quick
                const int chunkSamples = block == 1 ? 1500 : (int) (0.35 * rate);
                const int chunkBlocks  = jmax (1, chunkSamples / block);

                auto params = p.getParameters();

                for (int step = 0; step < 14; ++step)
                {
                    ++ops;
                    switch (rng.nextInt (18))
                    {
                        case 0:  p.randomizeAll(); break;
                        case 1:  p.mutateCurrent(); break;
                        case 2:  p.undo(); break;
                        case 3:  p.redo(); break;
                        case 4:  p.loadPreset (rng.nextInt (VivisectProcessor::numFactoryPresets)); break;
                        case 5:  p.selectABSlot (rng.nextInt (2)); break;
                        case 6:  p.copyABSlot(); break;
                        case 7:  p.startCapture (1 + rng.nextInt (2)); break;
                        case 8:  p.cancelCapture(); break;
                        case 9:  p.resetAllToDefaults(); break;
                        case 10: p.rewindToNormalized (rng.nextFloat()); break;
                        case 11: p.triggerSurgeonManual (rng.nextInt (vsx::kNumSurgeons)); break;
                        case 12: ph.bpm = 20.0 + rng.nextDouble() * 280.0; ph.playing = rng.nextBool(); break;
                        case 13: p.setSimpleViewEnabled (rng.nextBool()); break;
                        case 14:
                        {
                            MemoryBlock blob;
                            p.getStateInformation (blob);
                            p.setStateInformation (blob.getData(), (int) blob.getSize());
                            break;
                        }
                        default:   // slam a batch of parameters (new ones included) to extremes / random values
                        {
                            const int n = 1 + rng.nextInt (40);
                            for (int k = 0; k < n; ++k)
                            {
                                auto* par = params[rng.nextInt (params.size())];
                                const int mode = rng.nextInt (4);
                                const float v = mode == 0 ? 0.f : mode == 1 ? 1.f : rng.nextFloat();
                                par->beginChangeGesture();
                                par->setValueNotifyingHost (v);
                                par->endChangeGesture();
                            }
                            break;
                        }
                    }

                    for (int b = 0; b < chunkBlocks; ++b)
                    {
                        if (rng.nextInt (9) == 0) buf.clear(); else fillTestSignal (buf, pos, rate);
                        midi.clear();
                        if (rng.nextInt (6) == 0)
                            midi.addEvent (MidiMessage::controllerEvent (1, rng.nextInt (128), rng.nextInt (128)),
                                           rng.nextInt (block));
                        if (rng.nextInt (6) == 0)
                            midi.addEvent (MidiMessage::noteOn (1, rng.nextInt (128), (uint8) 100), rng.nextInt (block));
                        p.processBlock (buf, midi);
                        pos += block;
                        if (ph.playing) ph.ppq += (double) block / (60.0 / ph.bpm * rate);

                        const auto v = inspect (buf);
                        const float bar = kCeiling * jmax (1.f, qaGainOf (p, vsx::id::outputTrim))
                                                   * jmax (1.f, qaGainOf (p, vsx::id::inputTrim));
                        if (! v.finite || v.peak >= bar)
                        {
                            if (! v.finite) ++nonFinite; else ++overCeiling;
                            if (firstBad.isEmpty())
                                firstBad = String (rate, 0) + "Hz/" + String (block) + " bpm " + String (ph.bpm, 1)
                                         + " step " + String (step) + " peak " + String (v.peak, 2);
                            buf.clear();
                        }
                        else worst = jmax (worst, v.peak / bar);
                    }
                }
                p.cancelCapture();
                ucTests::waitIdle (p, 3000);
                p.setPlayHead (nullptr);
            }

        check (nonFinite == 0, "soak: no NaN/Inf (" + String (nonFinite) + " bad) " + firstBad);
        check (overCeiling == 0, "soak: output stayed bounded (" + String (overCeiling) + " over) " + firstBad);
        std::printf ("   soak: %d scenarios, %d operations, worst peak %.3f of ceiling\n", scenarios, ops, worst);
        captureDir.deleteRecursively();
    }
}
