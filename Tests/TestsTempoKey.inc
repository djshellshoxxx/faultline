// ============================================================================
//  Tap tempo (H1) and key detection (H2) tests.
//  Included by TestMain.cpp after the shared helpers.
// ============================================================================
#include "Tempo.h"
#include "KeyDetect.h"

namespace tktest
{
// Sum of sine partials: notes are MIDI numbers, each with 3 decaying harmonics.
inline void addNote (std::vector<float>& x, double sr, int midi, float amp)
{
    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    for (int h = 1; h <= 3; ++h)
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += amp / (float) h * (float) std::sin (MathConstants<double>::twoPi * f * h * (double) i / sr);
}

inline std::vector<float> chord (double sr, int seconds100, std::initializer_list<std::pair<int, float>> notes)
{
    std::vector<float> x ((size_t) (sr * seconds100 / 100));
    for (auto& n : notes) addNote (x, sr, n.first, n.second);
    return x;
}

// Plays `x` through the processor (stereo, 512 blocks, looping), requests detection
// part-way and returns whether a result arrived.
inline bool detectThroughProcessor (VivisectProcessor& p, const std::vector<float>& x, int block)
{
    AudioBuffer<float> buf (2, block);
    MidiBuffer midi;
    size_t pos = 0;
    auto feed = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i, ++pos)
            {
                const float v = 0.25f * x[pos % x.size()];
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
            p.processBlock (buf, midi);
        }
    };
    feed (260);                              // > 1.4 s at 48 kHz / 512
    p.requestKeyDetect();
    feed (2);
    return p.pollKeyDetect();
}

inline float rawVal (VivisectProcessor& p, const char* pid) { return p.apvts.getRawParameterValue (pid)->load(); }
} // namespace tktest

static void run_tempokey_tests (double kRate, int kBlock)
{
    // -----------------------------------------------------------------------
    beginCase ("H1 TapTempo: steady taps, median, reset, clamp");
    {
        vsx::TapTempo t;
        check (t.tap (10.0) == 0.0, "first tap gives no tempo yet");
        check (std::abs (t.tap (10.5) - 120.0) < 1.0e-9, "two taps 0.5 s apart = 120 BPM");
        t.tap (11.0); t.tap (11.5);
        check (std::abs (t.tap (12.0) - 120.0) < 1.0e-9, "steady taps stay at 120 BPM");
        check (std::abs (t.tap (12.9) - 120.0) < 1.0e-9, "one sloppy tap does not move the median");
        t.reset();
        t.tap (0.0);
        check (std::abs (t.tap (1.0) - 60.0) < 1.0e-9, "60 BPM at one second");
        check (t.tap (5.0) == 0.0, "a gap over 2 s starts a new count");
        check (std::abs (t.tap (5.25) - 240.0) < 1.0e-9, "...and counts again from there");
        t.reset();
        t.tap (0.0);
        check (t.tap (0.001) == vsx::kMaxBpm, "absurdly fast taps clamp to the maximum");
        t.reset();
        t.tap (0.0);
        check (t.tap (1.9) == 60.0 / 1.9 && t.tap (3.8) > 0.0, "slow taps inside the window still work");
        t.reset();
        t.tap (1.0);
        check (t.tap (0.5) == 0.0, "time going backwards restarts the count");
        check (vsx::TapTempo::clampBpm (std::nan ("")) == 120.0, "non-finite tempo falls back to 120");
        // long run keeps working past the tap history size
        t.reset();
        double r = 0.0;
        for (int i = 0; i < 40; ++i) r = t.tap (i * 0.4);
        check (std::abs (r - 150.0) < 1.0e-9, "40 taps at 0.4 s = 150 BPM");
    }

    // -----------------------------------------------------------------------
    beginCase ("H2 KeyDetect: all 24 keys from ideal profiles");
    {
        static constexpr float maj[12] { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static constexpr float mnr[12] { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
        bool ok = true;
        for (int minor = 0; minor < 2; ++minor)
            for (int root = 0; root < 12; ++root)
            {
                float c[12];
                for (int i = 0; i < 12; ++i) c[(i + root) % 12] = (minor ? mnr : maj)[i];
                const auto k = vsx::keydetect::detectKey (c);
                ok = ok && k.valid && k.root == root && k.minor == (minor != 0) && k.confidence > 0.99f;
            }
        check (ok, "every ideal profile maps back to its own root and mode");
        float flat[12] {};
        check (! vsx::keydetect::detectKey (flat).valid, "a flat chromagram is not a key");
        float bad[12] {}; bad[3] = std::nanf ("");
        check (! vsx::keydetect::detectKey (bad).valid, "NaN chromagram is rejected");
    }

    // -----------------------------------------------------------------------
    beginCase ("H2 KeyDetect: audio");
    {
        using namespace tktest;
        const auto cMaj = chord (kRate, 150, { { 48, 1.f }, { 55, 0.7f }, { 60, 0.8f }, { 64, 0.6f }, { 67, 0.6f } });
        auto k = vsx::keydetect::analyse (cMaj.data(), (int) cMaj.size(), kRate);
        check (k.valid && k.root == 0 && ! k.minor, "C major chord detected as C major");

        const auto dMin = chord (kRate, 150, { { 38, 1.f }, { 45, 0.7f }, { 50, 0.8f }, { 53, 0.6f }, { 57, 0.6f } });
        k = vsx::keydetect::analyse (dMin.data(), (int) dMin.size(), kRate);
        check (k.valid && k.root == 2 && k.minor, "D minor chord detected as D minor");

        const auto fsMaj = chord (kRate, 150, { { 42, 1.f }, { 49, 0.7f }, { 54, 0.8f }, { 58, 0.6f }, { 61, 0.6f } });
        k = vsx::keydetect::analyse (fsMaj.data(), (int) fsMaj.size(), kRate);
        check (k.valid && k.root == 6 && ! k.minor, "F# major chord detected as F# major");

        std::vector<float> silent (65536, 0.f);
        check (! vsx::keydetect::analyse (silent.data(), (int) silent.size(), kRate).valid, "silence is not a key");
        check (! vsx::keydetect::analyse (cMaj.data(), 100, kRate).valid, "a too-short block is not a key");
        // noise must never crash and must stay finite
        Random rng (7);
        std::vector<float> noise (65536);
        for (auto& v : noise) v = rng.nextFloat() * 2.f - 1.f;
        const auto kn = vsx::keydetect::analyse (noise.data(), (int) noise.size(), kRate);
        check (kn.root >= 0 && kn.root < 12 && std::isfinite (kn.confidence), "noise gives a finite, in-range answer");
        for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
        {
            const auto c = chord (sr, 150, { { 48, 1.f }, { 55, 0.7f }, { 60, 0.8f }, { 64, 0.6f }, { 67, 0.6f } });
            const auto r = vsx::keydetect::analyse (c.data(), (int) c.size(), sr);
            check (r.valid && r.root == 0 && ! r.minor, "C major holds at " + String (sr, 0) + " Hz");
        }
    }

    // -----------------------------------------------------------------------
    beginCase ("H2 DETECT through the processor: sets key, one undo step, respects locks");
    {
        using namespace tktest;
        const auto dMin = chord (kRate, 150, { { 38, 1.f }, { 45, 0.7f }, { 50, 0.8f }, { 53, 0.6f }, { 57, 0.6f } });
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();
        p.clearUndoHistory();
        check (! p.pollKeyDetect() && ! p.keyDetectPending(), "nothing pending at start");
        check (detectThroughProcessor (p, dMin, kBlock), "a result arrives after the audio thread copied the window");
        check (rawVal (p, vsx::id::scaleRoot) == 2.f && rawVal (p, vsx::id::scaleType) == 2.f,
               "Scale Root = D, Scale Type = Minor");
        check (rawVal (p, vsx::id::scaleOn) > 0.5f, "Scale Lock turned on");
        check (! p.keyDetectPending(), "request cleared");
        check (p.canUndo(), "detection is undoable");
        p.undo();
        check (rawVal (p, vsx::id::scaleRoot) == 0.f && rawVal (p, vsx::id::scaleOn) < 0.5f && ! p.canUndo(),
               "one undo restores root, type and Scale Lock together");

        // a locked root is never touched
        p.setParameterLocked (vsx::id::scaleRoot, true);
        check (detectThroughProcessor (p, dMin, kBlock), "detects again with a lock set");
        check (rawVal (p, vsx::id::scaleRoot) == 0.f && rawVal (p, vsx::id::scaleType) == 2.f,
               "locked Scale Root stays, unlocked Scale Type still follows");
        p.setParameterLocked (vsx::id::scaleRoot, false);

        // silence: nothing changes
        VivisectProcessor q;
        q.prepareToPlay (kRate, kBlock);
        q.resetAllToDefaults();
        std::vector<float> silent (65536, 0.f);
        check (detectThroughProcessor (q, silent, kBlock), "silence still answers");
        check (! q.lastKeyResult().valid && rawVal (q, vsx::id::scaleOn) < 0.5f, "...with no key and no parameter change");
    }

    // -----------------------------------------------------------------------
    beginCase ("H1 tempo fallback: standalone uses tapped tempo, never touches sound state");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();
        AudioBuffer<float> buf (2, kBlock);
        MidiBuffer midi;
        buf.clear();
        p.processBlock (buf, midi);
        check (! p.hostHasTempo() && std::abs (p.currentBpm() - 120.0) < 1.0e-3, "no host, nothing tapped = 120");
        p.setTempoFallback (93.5);
        buf.clear();
        p.processBlock (buf, midi);
        check (std::abs (p.currentBpm() - 93.5) < 1.0e-3, "tapped tempo is used without a host tempo");
        p.setTempoFallback (1000.0);
        check (p.tempoFallback() == vsx::kMaxBpm, "stored tempo is clamped to 300");
        p.setTempoFallback (0.0);
        buf.clear();
        p.processBlock (buf, midi);
        check (p.tempoFallback() == 0.0 && std::abs (p.currentBpm() - 120.0) < 1.0e-3, "clearing returns to 120");

        // persists with the session, as a user setting
        p.setTempoFallback (88.0);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        VivisectProcessor q;
        q.prepareToPlay (kRate, kBlock);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::abs (q.tempoFallback() - 88.0) < 1.0e-9, "tapped tempo survives a state round trip");

        // RESET / RANDOM / MUTATE leave it alone
        q.resetAllToDefaults();
        q.randomizeAll();
        check (std::abs (q.tempoFallback() - 88.0) < 1.0e-9, "RESET and RANDOM do not touch the tapped tempo");
    }
}
