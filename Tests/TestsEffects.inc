// ============================================================================
//  Effects tests (0.10.0): E1 TOURNIQUET and E2 CLOT.
//  Included by TestMain.cpp after check()/beginCase()/runBlocks()/inspect().
// ============================================================================
#include <set>
#include "Effects.h"

namespace fxtest
{
inline double rmsOf (const AudioBuffer<float>& b, int from, int to, int ch = 0)
{
    double s = 0.0;
    const float* d = b.getReadPointer (ch);
    for (int i = from; i < to; ++i) s += (double) d[i] * d[i];
    return std::sqrt (s / jmax (1, to - from));
}

// Gain in dB of a TOURNIQUET at one sine frequency (steady state).
inline double tqGainDb (int mode, double sr, float cutoff, float res, double freq,
                        float envDepth = 0.f, float amp = 0.1f)
{
    vsx::Tourniquet t;
    t.prepare (sr);
    const int total = (int) sr;
    AudioBuffer<float> b (2, total);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < total; ++i)
            b.setSample (ch, i, amp * (float) std::sin (MathConstants<double>::twoPi * freq * i / sr));
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = jmin (512, total - pos);
        AudioBuffer<float> blk (b.getArrayOfWritePointers(), 2, pos, n);
        t.process (blk, n, mode, cutoff, res, envDepth, 1.f);
    }
    const double out = rmsOf (b, total / 2, total);
    const double in = amp / std::sqrt (2.0);
    return 20.0 * std::log10 (jmax (1.0e-12, out / in));
}

struct EchoResult { std::vector<float> l, r; };

// Render an impulse (0.1 in both channels) through a CLOT with mix = 1.
inline EchoResult clotImpulse (double sr, int timeChoice, double bpm, float fb, float spread,
                               int totalSamples, float freeMs = 375.f, float tone = 16000.f)
{
    vsx::Clot c;
    c.prepare (sr);
    const double spb = 60.0 / bpm * sr;
    EchoResult res;
    res.l.assign ((size_t) totalSamples, 0.f);
    res.r.assign ((size_t) totalSamples, 0.f);
    AudioBuffer<float> blk (2, 512);
    for (int pos = 0; pos < totalSamples; pos += 512)
    {
        const int n = jmin (512, totalSamples - pos);
        blk.clear();
        if (pos == 0) { blk.setSample (0, 0, 0.1f); blk.setSample (1, 0, 0.1f); }
        c.process (blk, n, spb, timeChoice, freeMs, fb, tone, spread, 1.f);
        for (int i = 0; i < n; ++i)
        {
            res.l[(size_t) (pos + i)] = blk.getSample (0, i);
            res.r[(size_t) (pos + i)] = blk.getSample (1, i);
        }
    }
    return res;
}

inline int argMaxAbs (const std::vector<float>& v, int lo, int hi)
{
    int best = lo;
    float bv = -1.f;
    for (int i = jmax (0, lo); i < jmin ((int) v.size(), hi); ++i)
        if (std::abs (v[(size_t) i]) > bv) { bv = std::abs (v[(size_t) i]); best = i; }
    return best;
}

inline void setChoiceIdx (VivisectProcessor& p, const char* pid, int idx)
{
    if (auto* par = p.apvts.getParameter (pid))
        par->setValueNotifyingHost (par->getNormalisableRange().convertTo0to1 ((float) idx));
}
inline void setReal (VivisectProcessor& p, const char* pid, float real)
{
    if (auto* par = p.apvts.getParameter (pid))
        par->setValueNotifyingHost (par->convertTo0to1 (real));
}

inline AudioBuffer<float> renderProcessor (VivisectProcessor& p, int blocks, int blockSize, double sr)
{
    AudioBuffer<float> out (2, blocks * blockSize);
    AudioBuffer<float> buf (2, blockSize);
    MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        fillTestSignal (buf, (int64) b * blockSize, sr);
        midi.clear();
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, b * blockSize, buf, ch, 0, blockSize);
    }
    return out;
}

inline bool buffersIdentical (const AudioBuffer<float>& a, const AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples()) return false;
    for (int ch = 0; ch < 2; ++ch)
        if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch),
                         sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;
    return true;
}
} // namespace fxtest

static void run_effects_tests (double kRate, int kBlock)
{
    using namespace fxtest;

    // -----------------------------------------------------------------------
    beginCase ("Effects: parameters, defaults and mod destinations exist");
    {
        VivisectProcessor p;
        for (const char* pid : { "tqOn", "tqMode", "tqCutoff", "tqRes", "tqEnv", "tqMix",
                                 "clotOn", "clotTime", "clotFree", "clotFeedback", "clotTone",
                                 "clotSpread", "clotMix" })
            check (p.apvts.getParameter (pid) != nullptr && p.apvts.getRawParameterValue (pid) != nullptr,
                   String ("effect parameter exists: ") + pid);

        auto raw = [&p] (const char* pid) { return p.apvts.getRawParameterValue (pid)->load(); };
        check (raw ("tqOn") < 0.5f && raw ("clotOn") < 0.5f, "both effects are off by default");
        check (raw ("tqMode") == 0.f, "TQ mode defaults to low-pass");
        check (std::abs (raw ("tqCutoff") - 1200.f) < 0.5f, "TQ cutoff defaults to 1200 Hz");
        check (std::abs (raw ("tqRes") - 0.25f) < 1.0e-4f, "TQ resonance defaults to 0.25");
        check (raw ("tqEnv") == 0.f && raw ("tqMix") == 1.f, "TQ env 0, mix 1 by default");
        check (raw ("clotTime") == 2.f, "Clot time defaults to 1/8");
        check (std::abs (raw ("clotFree") - 375.f) < 0.5f, "Clot free defaults to 375 ms");
        check (std::abs (raw ("clotFeedback") - 0.45f) < 1.0e-4f, "Clot feedback defaults to 0.45");
        check (std::abs (raw ("clotTone") - 5000.f) < 1.f, "Clot tone defaults to 5 kHz");
        check (std::abs (raw ("clotSpread") - 0.6f) < 1.0e-4f, "Clot spread defaults to 0.6");
        check (std::abs (raw ("clotMix") - 0.3f) < 1.0e-4f, "Clot mix defaults to 0.3");

        auto range = [&p] (const char* pid) { return p.apvts.getParameter (pid)->getNormalisableRange(); };
        check (range ("tqCutoff").start == 20.f && range ("tqCutoff").end == 20000.f
               && std::abs (range ("tqCutoff").skew - 0.3f) < 1.0e-4f, "TQ cutoff range 20..20000, skew 0.3");
        check (range ("clotFeedback").end == 0.95f, "Clot feedback tops out at 0.95");
        check (range ("tqEnv").start == -1.f && range ("tqEnv").end == 1.f, "TQ env is bipolar");
        check (range ("clotFree").start == 20.f && range ("clotFree").end == 2000.f, "Clot free 20..2000 ms");
        check (range ("clotTone").start == 200.f && range ("clotTone").end == 16000.f, "Clot tone 200..16000 Hz");

        const auto dst = vsx::modDestNames();
        check (dst.size() == 17, "mod destination list grew by exactly four");
        check (dst[13] == "TQ Cutoff" && dst[14] == "TQ Resonance"
               && dst[15] == "Clot Feedback" && dst[16] == "Clot Mix", "new destinations appended at the end");
        check (dst[12] == "Fz Blur" && dst[1] == "Chaos", "existing destination indices did not move");
        check (vsx::MD_TQ_CUTOFF == 13 && vsx::MD_CLOT_MIX == 16, "destination enums match the name list");

        check (p.getTailLengthSeconds() >= 4.0, "tail length stays at least 4 seconds");
    }

    // -----------------------------------------------------------------------
    beginCase ("TOURNIQUET: each mode attenuates and passes as expected");
    {
        const double sr = 48000.0;
        const float fc = 1000.f;
        const double lpStop = tqGainDb (vsx::Tourniquet::LowPass, sr, fc, 0.25f, 10.0 * fc);
        const double lpPass = tqGainDb (vsx::Tourniquet::LowPass, sr, fc, 0.25f, fc / 10.0);
        check (lpStop < -30.0, "LP at 10x cutoff below -30 dB (" + String (lpStop, 1) + ")");
        check (std::abs (lpPass) < 2.0, "LP passes a tenth of the cutoff (" + String (lpPass, 2) + ")");

        const double hpStop = tqGainDb (vsx::Tourniquet::HighPass, sr, fc, 0.25f, fc / 10.0);
        const double hpPass = tqGainDb (vsx::Tourniquet::HighPass, sr, fc, 0.25f, 10.0 * fc);
        check (hpStop < -30.0, "HP at cutoff/10 below -30 dB (" + String (hpStop, 1) + ")");
        check (std::abs (hpPass) < 2.0, "HP passes 10x the cutoff (" + String (hpPass, 2) + ")");

        const double bpLo = tqGainDb (vsx::Tourniquet::BandPass, sr, fc, 0.5f, fc / 5.0);
        const double bpMid = tqGainDb (vsx::Tourniquet::BandPass, sr, fc, 0.5f, fc);
        const double bpHi = tqGainDb (vsx::Tourniquet::BandPass, sr, fc, 0.5f, fc * 5.0);
        check (bpMid > bpLo + 6.0 && bpMid > bpHi + 6.0, "BP peaks at the cutoff");
        check (std::abs (bpMid) < 1.0, "BP has unity gain at the cutoff (" + String (bpMid, 2) + ")");

        const double ntMid = tqGainDb (vsx::Tourniquet::Notch, sr, fc, 0.5f, fc);
        const double ntLo = tqGainDb (vsx::Tourniquet::Notch, sr, fc, 0.5f, fc / 10.0);
        const double ntHi = tqGainDb (vsx::Tourniquet::Notch, sr, fc, 0.5f, fc * 10.0);
        check (ntMid < -20.0, "notch dips at the cutoff (" + String (ntMid, 1) + ")");
        check (ntLo > -1.0 && ntHi > -1.0, "notch passes both sides");

        // Resonance raises the peak at the cutoff.
        const double lpLow = tqGainDb (vsx::Tourniquet::LowPass, sr, fc, 0.f, fc);
        const double lpRes = tqGainDb (vsx::Tourniquet::LowPass, sr, fc, 1.f, fc);
        check (lpRes > lpLow + 10.0, "resonance lifts the cutoff peak (" + String (lpLow, 1)
                                     + " -> " + String (lpRes, 1) + " dB)");

        // Envelope follower: louder input opens the cutoff when depth is positive.
        const double envOff = tqGainDb (vsx::Tourniquet::LowPass, sr, 200.f, 0.25f, 2000.0, 0.f, 0.5f);
        const double envOn  = tqGainDb (vsx::Tourniquet::LowPass, sr, 200.f, 0.25f, 2000.0, 1.f, 0.5f);
        const double envNeg = tqGainDb (vsx::Tourniquet::LowPass, sr, 2000.f, 0.25f, 2000.0, -1.f, 0.5f);
        const double envRef = tqGainDb (vsx::Tourniquet::LowPass, sr, 2000.f, 0.25f, 2000.0, 0.f, 0.5f);
        check (envOn > envOff + 6.0, "positive ENV opens the cutoff (" + String (envOff, 1)
                                     + " -> " + String (envOn, 1) + " dB)");
        check (envNeg < envRef - 6.0, "negative ENV closes the cutoff");

        check (std::abs (vsx::Tourniquet::resToQ (0.f) - 0.5f) < 1.0e-4f
               && std::abs (vsx::Tourniquet::resToQ (1.f) - 18.f) < 1.0e-3f, "resonance maps to Q 0.5..18");
        check (vsx::Tourniquet::effectiveCutoff (30000.f, 0.f, 0.f, 48000.0) <= 0.45f * 48000.f,
               "cutoff is clamped to 0.45 * fs");
        check (std::abs (vsx::Tourniquet::effectiveCutoff (1000.f, 1.f, 1.f, 48000.0) - 16000.f) < 1.f,
               "full ENV depth is plus four octaves");
    }

    // -----------------------------------------------------------------------
    beginCase ("TOURNIQUET: sample-rate matrix 44.1 / 48 / 96 / 192 kHz");
    {
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const String tag = " @" + String (sr / 1000.0, 1) + "k";
            const double lp = tqGainDb (vsx::Tourniquet::LowPass, sr, 1000.f, 0.25f, 10000.0);
            const double hp = tqGainDb (vsx::Tourniquet::HighPass, sr, 1000.f, 0.25f, 100.0);
            const double bp = tqGainDb (vsx::Tourniquet::BandPass, sr, 1000.f, 0.5f, 1000.0);
            const double nt = tqGainDb (vsx::Tourniquet::Notch, sr, 1000.f, 0.5f, 1000.0);
            check (lp < -30.0, "LP stopband" + tag + " (" + String (lp, 1) + ")");
            check (hp < -30.0, "HP stopband" + tag + " (" + String (hp, 1) + ")");
            check (std::abs (bp) < 1.0, "BP centre" + tag + " (" + String (bp, 2) + ")");
            check (nt < -20.0, "notch dip" + tag + " (" + String (nt, 1) + ")");
        }
    }

    // -----------------------------------------------------------------------
    beginCase ("TOURNIQUET: stable across the whole cutoff/res/mode/env grid");
    {
        const double sr = 48000.0;
        int bad = 0, loud = 0;
        float worst = 0.f;
        Random rng (1234);
        for (float cutoff : { 20.f, 200.f, 2000.f, 12000.f, 20000.f })
            for (float res : { 0.f, 0.5f, 1.f })
                for (int mode = 0; mode < 4; ++mode)
                    for (float env : { -1.f, 0.f, 1.f })
                    {
                        vsx::Tourniquet t;
                        t.prepare (sr);
                        AudioBuffer<float> b (2, 512);
                        for (int blk = 0; blk < 60; ++blk)
                        {
                            const bool square = blk >= 30;
                            for (int ch = 0; ch < 2; ++ch)
                                for (int i = 0; i < 512; ++i)
                                {
                                    const int pos = blk * 512 + i;
                                    b.setSample (ch, i, square ? (((pos / 24) & 1) ? 1.f : -1.f)
                                                               : rng.nextFloat() * 2.f - 1.f);
                                }
                            t.process (b, 512, mode, cutoff, res, env, 1.f);
                            const auto v = inspect (b);
                            if (! v.finite) { ++bad; break; }
                            worst = jmax (worst, v.peak);
                            if (v.peak > 6.01f) ++loud;
                        }
                    }
        check (bad == 0, "no NaN/Inf anywhere on the grid (" + String (bad) + " bad)");
        check (loud == 0, "output stays bounded on the grid (worst peak " + String (worst, 2) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("TOURNIQUET: hostile input and fast automation stay finite");
    {
        vsx::Tourniquet t;
        t.prepare (48000.0);
        AudioBuffer<float> b (2, 256);
        Random rng (77);
        bool ok = true;
        for (int blk = 0; blk < 400 && ok; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    float v = rng.nextFloat() * 2.f - 1.f;
                    if (i == 7) v = std::numeric_limits<float>::quiet_NaN();
                    if (i == 9) v = std::numeric_limits<float>::infinity();
                    if (i == 11) v = 1.0e-38f;
                    if (i == 13) v = 1.0e6f;
                    b.setSample (ch, i, v);
                }
            t.process (b, 256, blk & 3, (blk & 1) ? 20.f : 20000.f, (blk & 2) ? 1.f : 0.f,
                       (blk & 4) ? 1.f : -1.f, 1.f);
            ok = inspect (b).finite;
        }
        check (ok, "NaN/Inf/denormal/huge input with slammed parameters never escapes");
    }

    // -----------------------------------------------------------------------
    beginCase ("TOURNIQUET: bypass is exact and turning off resets the state");
    {
        const double sr = 48000.0;
        vsx::Tourniquet t;
        t.prepare (sr);
        AudioBuffer<float> a (2, 512), ref (2, 512);
        fillTestSignal (a, 0, sr);
        ref.makeCopyOf (a);
        t.process (a, 512, 0, 800.f, 0.7f, 0.5f, 0.f);       // mix 0
        check (buffersIdentical (a, ref), "mix 0 leaves the buffer bit-identical");

        // Warm the filter up, reset, then compare against a brand new instance.
        for (int i = 0; i < 20; ++i)
        {
            fillTestSignal (a, i * 512, sr);
            t.process (a, 512, 1, 500.f, 1.f, 0.f, 1.f);
        }
        t.reset();
        vsx::Tourniquet fresh;
        fresh.prepare (sr);
        AudioBuffer<float> x (2, 512), y (2, 512);
        fillTestSignal (x, 99999, sr);
        y.makeCopyOf (x);
        t.process (x, 512, 0, 900.f, 0.4f, 0.f, 1.f);
        fresh.process (y, 512, 0, 900.f, 0.4f, 0.f, 1.f);
        check (buffersIdentical (x, y), "reset() returns the filter to its initial state");

        // Mix returning from 0 after a long bypass starts from a clean state.
        vsx::Tourniquet u;
        u.prepare (sr);
        AudioBuffer<float> q (2, 512), q2 (2, 512);
        for (int i = 0; i < 10; ++i) { fillTestSignal (q, i * 512, sr); u.process (q, 512, 0, 500.f, 0.9f, 0.f, 1.f); }
        fillTestSignal (q, 0, sr);
        for (int i = 0; i < 400; ++i) u.process (q, 512, 0, 500.f, 0.9f, 0.f, 0.f);
        check (inspect (q).finite, "long zero-mix bypass stays finite");
    }

    // -----------------------------------------------------------------------
    beginCase ("CLOT: echo spacing is sample-accurate at 120 BPM for every division");
    {
        static const double beats[8] { 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 };
        static const char* names[8] { "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1/1" };
        for (double sr : { 44100.0, 48000.0 })
            for (int div = 0; div < 8; ++div)
            {
                const double D = beats[div] * 0.5 * sr;     // 120 BPM: one beat is sr/2 samples
                const int total = (int) std::ceil (D * 3.0) + 2048;
                const auto r = clotImpulse (sr, div, 120.0, 0.6f, 0.f, total);
                bool ok = true;
                String detail;
                for (int e = 1; e <= 3; ++e)
                {
                    const double expect = D * e;
                    const int at = argMaxAbs (r.l, (int) expect - 64, (int) expect + 64);
                    if (std::abs ((double) at - expect) > 1.0)
                    {
                        ok = false;
                        detail << " echo" << e << "@" << at << " vs " << String (expect, 1);
                    }
                }
                check (ok, String (names[div]) + " @" + String (sr / 1000.0, 1) + "k echo spacing exact" + detail);
                check (std::abs (r.l[0]) < 1.0e-9f, String (names[div]) + " produces no output before the first echo");
            }

        // Free time in milliseconds, and the tempo fallback for a nonsense BPM.
        const auto fr = clotImpulse (48000.0, vsx::Clot::kTimeFree, 120.0, 0.5f, 0.f, 48000, 250.f);
        check (std::abs (argMaxAbs (fr.l, 0, 48000) - 12000) <= 1, "Free time 250 ms = 12000 samples at 48k");
        vsx::Clot c;
        c.prepare (48000.0);
        AudioBuffer<float> blk (2, 512);
        blk.clear();
        blk.setSample (0, 0, 0.1f);
        blk.setSample (1, 0, 0.1f);
        c.process (blk, 512, 0.0, 2, 375.f, 0.5f, 5000.f, 0.f, 1.f);     // spb = 0 -> 120 BPM fallback
        check (inspect (blk).finite, "invalid tempo falls back safely");
        check (c.capacitySamples() >= 4 * 48000, "delay line holds at least 4 seconds at the prepared rate");
    }

    // -----------------------------------------------------------------------
    beginCase ("CLOT: feedback decays monotonically; ping-pong alternates");
    {
        const double sr = 48000.0;
        const int D = 12000;                                   // 1/8 at 120 BPM
        const auto r = clotImpulse (sr, 2, 120.0, 0.6f, 0.f, D * 12 + 512);
        float prev = 1.0e9f;
        bool mono = true, decreasing = true;
        for (int e = 1; e <= 10; ++e)
        {
            const float v = std::abs (r.l[(size_t) (D * e)]);
            if (! (v > 0.f && v < prev)) decreasing = false;
            prev = v;
            if (std::abs (r.l[(size_t) (D * e)] - r.r[(size_t) (D * e)]) > 1.0e-6f) mono = false;
        }
        check (decreasing, "each repeat is quieter than the last (fb 0.6)");
        check (mono, "spread 0 gives identical left and right");
        check (std::abs (r.l[(size_t) D]) > 0.05f && std::abs (r.l[(size_t) D]) < 0.101f, "first echo carries the input level");

        const auto pp = clotImpulse (sr, 2, 120.0, 0.6f, 1.f, D * 6 + 512);
        check (std::abs (pp.l[(size_t) D]) > 0.05f && std::abs (pp.r[(size_t) D]) < 1.0e-6f, "ping-pong: echo 1 only on the left");
        check (std::abs (pp.r[(size_t) (2 * D)]) > 0.01f && std::abs (pp.l[(size_t) (2 * D)]) < 1.0e-6f, "ping-pong: echo 2 only on the right");
        check (std::abs (pp.l[(size_t) (3 * D)]) > 0.005f && std::abs (pp.r[(size_t) (3 * D)]) < 1.0e-6f, "ping-pong: echo 3 back on the left");

        // Tone lowers the brightness of repeats: compare energy after echo 3 of a bright click.
        const auto bright = clotImpulse (sr, 2, 120.0, 0.7f, 0.f, D * 4, 375.f, 16000.f);
        const auto dark   = clotImpulse (sr, 2, 120.0, 0.7f, 0.f, D * 4, 375.f, 400.f);
        check (std::abs (dark.l[(size_t) (2 * D)]) < std::abs (bright.l[(size_t) (2 * D)]) * 0.6f,
               "low TONE damps the second repeat");
    }

    // -----------------------------------------------------------------------
    beginCase ("CLOT: bounded forever at feedback 0.95 with full-scale input (60 s)");
    {
        const double sr = 48000.0;
        vsx::Clot c;
        c.prepare (sr);
        AudioBuffer<float> b (2, 512);
        Random rng (4242);
        const int blocks = (int) (60.0 * sr / 512.0);
        float worst = 0.f;
        bool finite = true;
        for (int blk = 0; blk < blocks && finite; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    b.setSample (ch, i, (blk % 2 == 0) ? ((i / 8) & 1 ? 1.f : -1.f) : (rng.nextFloat() * 2.f - 1.f));
            c.process (b, 512, 0.5 * sr, (blk / 500) % 9, 123.f, 0.95f, 16000.f, (blk % 3) / 2.f, 1.f);
            const auto v = inspect (b);
            finite = v.finite;
            worst = jmax (worst, v.peak);
        }
        check (finite, "60 s at fb 0.95 never produces NaN/Inf");
        check (worst < 4.0f, "60 s at fb 0.95 stays bounded (worst peak " + String (worst, 3) + ")");

        // and the tail dies away once the input stops (feedback < 1 forever)
        AudioBuffer<float> silent (2, 512);
        float tailPeak = 0.f;
        for (int blk = 0; blk < (int) (120.0 * sr / 512.0); ++blk)
        {
            silent.clear();
            c.process (silent, 512, 0.5 * sr, 2, 375.f, 0.95f, 16000.f, 0.6f, 1.f);
            if (blk > (int) (110.0 * sr / 512.0)) tailPeak = jmax (tailPeak, inspect (silent).peak);
        }
        check (tailPeak < 0.5f, "the loop does not sustain itself indefinitely (peak " + String (tailPeak, 3) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("CLOT: bypass is exact, off empties the line, Free vs synced is safe");
    {
        const double sr = 48000.0;
        vsx::Clot c;
        c.prepare (sr);
        AudioBuffer<float> a (2, 512), ref (2, 512);
        fillTestSignal (a, 0, sr);
        ref.makeCopyOf (a);
        c.process (a, 512, 0.5 * sr, 2, 375.f, 0.5f, 5000.f, 0.6f, 0.f);      // mix 0
        check (buffersIdentical (a, ref), "mix 0 leaves the buffer bit-identical");

        // Fill the line, reset, then silence must stay silent.
        for (int i = 0; i < 40; ++i)
        {
            fillTestSignal (a, i * 512, sr);
            c.process (a, 512, 0.5 * sr, 2, 375.f, 0.9f, 5000.f, 0.6f, 0.5f);
        }
        c.reset();
        a.clear();
        c.process (a, 512, 0.5 * sr, 2, 375.f, 0.9f, 5000.f, 0.6f, 1.f);
        check (inspect (a).peak == 0.f, "reset() empties the delay line");

        // Zero mix after running: the line is cleared by the bypass path.
        for (int i = 0; i < 40; ++i)
        {
            fillTestSignal (a, i * 512, sr);
            c.process (a, 512, 0.5 * sr, 2, 375.f, 0.9f, 5000.f, 0.6f, 0.5f);
        }
        for (int i = 0; i < 200; ++i) { fillTestSignal (a, 0, sr); c.process (a, 512, 0.5 * sr, 2, 375.f, 0.9f, 5000.f, 0.6f, 0.f); }
        a.clear();
        c.process (a, 512, 0.5 * sr, 2, 375.f, 0.9f, 5000.f, 0.6f, 1.f);
        check (inspect (a).peak == 0.f, "a long zero-mix bypass forgets old echoes");

        // Switching between Free and every synced division every block.
        vsx::Clot d;
        d.prepare (sr);
        bool finite = true;
        float worst = 0.f;
        for (int blk = 0; blk < 600 && finite; ++blk)
        {
            fillTestSignal (a, blk * 512, sr);
            d.process (a, 512, 0.5 * sr, blk % 9, 20.f + (float) (blk % 7) * 300.f, 0.9f, 3000.f, 0.5f, 0.7f);
            const auto v = inspect (a);
            finite = v.finite;
            worst = jmax (worst, v.peak);
        }
        check (finite, "switching Free and synced every block stays finite");
        check (worst < 8.f, "switching time keeps the output bounded (" + String (worst, 2) + ")");

        // Absurd tempi clamp to the 4 s line rather than reading out of range.
        AudioBuffer<float> e (2, 512);
        bool ok = true;
        for (double spb : { 1.0, 100.0, 1.0e6, 1.0e12 })
        {
            fillTestSignal (e, 0, sr);
            d.process (e, 512, spb, 7, 375.f, 0.9f, 3000.f, 0.5f, 0.7f);
            ok = ok && inspect (e).finite;
        }
        check (ok, "extreme tempi stay inside the delay line");
    }

    // -----------------------------------------------------------------------
    beginCase ("Effects in the plug-in: bypass bit-exact, audible when on, finite everywhere");
    {
        auto make = [kRate, kBlock] (std::unique_ptr<VivisectProcessor>& p)
        {
            p = std::make_unique<VivisectProcessor>();
            p->prepareToPlay (kRate, kBlock);
        };
        std::unique_ptr<VivisectProcessor> ref, bypass, active;
        make (ref); make (bypass); make (active);

        // "On" with zero mix is an exact bypass of the stage.
        setReal (*bypass, "tqOn", 1.f);   setReal (*bypass, "tqMix", 0.f);
        setReal (*bypass, "clotOn", 1.f); setReal (*bypass, "clotMix", 0.f);
        const auto a = renderProcessor (*ref, 40, kBlock, kRate);
        const auto b = renderProcessor (*bypass, 40, kBlock, kRate);
        check (buffersIdentical (a, b), "effects switched on at mix 0 render bit-identical to effects off");

        // Switching on then off again returns to the exact dry path.
        VivisectProcessor toggled;
        toggled.prepareToPlay (kRate, kBlock);
        setReal (toggled, "tqOn", 1.f); setReal (toggled, "clotOn", 1.f);
        setReal (toggled, "tqCutoff", 300.f);
        (void) renderProcessor (toggled, 10, kBlock, kRate);
        setReal (toggled, "tqOn", 0.f); setReal (toggled, "clotOn", 0.f);
        (void) renderProcessor (toggled, 2, kBlock, kRate);       // the stages reset on the way out
        check (toggled.tourniquetWasOn == false && toggled.clotWasOn == false, "both stages report off after switching off");

        // Audible when on: a low-pass at 200 Hz changes the output a lot.
        setReal (*active, "tqOn", 1.f); setReal (*active, "tqCutoff", 200.f); setReal (*active, "tqRes", 0.f);
        setReal (*active, "clotOn", 1.f); setReal (*active, "clotMix", 0.6f);
        const auto c = renderProcessor (*active, 40, kBlock, kRate);
        check (! buffersIdentical (a, c), "TOURNIQUET + CLOT change the output when on");
        check (inspect (c).finite, "active effects render finite");

        // Every mode and time choice through the full plug-in at several block sizes.
        for (int blockSize : { 32, 64, 512, 2048 })
        {
            VivisectProcessor q;
            q.prepareToPlay (kRate, blockSize);
            setReal (q, "tqOn", 1.f); setReal (q, "clotOn", 1.f);
            setReal (q, "clotFeedback", 0.95f); setReal (q, "tqRes", 1.f);
            bool ok = true;
            float worst = 0.f;
            for (int i = 0; i < 18 && ok; ++i)
            {
                setChoiceIdx (q, "tqMode", i % 4);
                setChoiceIdx (q, "clotTime", i % 9);
                const auto r = runBlocks (q, 6, blockSize, kRate, true);
                ok = r.finite;
                worst = jmax (worst, r.peak);
            }
            check (ok, "effects at extremes are finite at block size " + String (blockSize));
            check (worst < ceilingFor (q), "effects at extremes stay bounded at block size " + String (blockSize)
                                           + " (" + String (worst, 2) + ")");
        }

        // Sample-rate matrix through the full plug-in.
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            VivisectProcessor q;
            q.prepareToPlay (sr, 512);
            setReal (q, "tqOn", 1.f); setReal (q, "clotOn", 1.f);
            const auto r = runBlocks (q, 24, 512, sr, true);
            check (r.finite && r.peak < ceilingFor (q), "effects finite and bounded at " + String (sr / 1000.0, 1) + " kHz");
        }
    }

    // -----------------------------------------------------------------------
    beginCase ("Effects: mod matrix reaches TQ Cutoff and Clot Mix");
    {
        auto energy = [kRate, kBlock] (bool withMod, bool clot)
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            for (const char* s : { "st_on" }) setReal (p, s, 0.f);
            setReal (p, "dryWet", 0.f);
            if (! clot)
            {
                setReal (p, "tqOn", 1.f); setChoiceIdx (p, "tqMode", 0);
                setReal (p, "tqCutoff", 120.f); setReal (p, "tqRes", 0.f);
            }
            else
            {
                setReal (p, "clotOn", 1.f); setReal (p, "clotMix", 0.f);
                setReal (p, "clotFeedback", 0.5f);
            }
            if (withMod)
            {
                setReal (p, "macro1", 1.f);
                setChoiceIdx (p, "mm1_src", 4);                       // Macro 1
                setChoiceIdx (p, "mm1_dst", clot ? 16 : 13);          // Clot Mix / TQ Cutoff
                setReal (p, "mm1_depth", 1.f);
            }
            const auto out = renderProcessor (p, 60, kBlock, kRate);
            return (double) rmsOf (out, out.getNumSamples() / 2, out.getNumSamples());
        };
        const double tqBase = energy (false, false), tqMod = energy (true, false);
        check (tqMod > tqBase * 1.5, "TQ Cutoff mod opens the filter (" + String (tqBase, 4) + " -> " + String (tqMod, 4) + ")");

        const double clBase = energy (false, true), clMod = energy (true, true);
        check (std::abs (clMod - clBase) > 1.0e-4, "Clot Mix mod changes the output (" + String (clBase, 4)
                                                    + " -> " + String (clMod, 4) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("Effects: RANDOM, MUTATE, locks, RESET, A/B, history and state cover the new params");
    {
        const auto& fxIds = vsx::effectsParamIds();
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        // RANDOM touches the effect parameters.
        std::map<String, std::set<int>> seen;
        for (int i = 0; i < 60; ++i)
        {
            p.randomizeAll();
            for (const char* pid : fxIds)
                seen[pid].insert ((int) std::lround (p.apvts.getParameter (pid)->getValue() * 1000.f));
        }
        for (const char* pid : fxIds)
            check (seen[pid].size() > 1, String ("RANDOM varies ") + pid);

        // Locks protect effect params through RANDOM and MUTATE.
        p.resetAllToDefaults();
        std::map<String, float> lockedVals;
        for (const char* pid : fxIds)
        {
            auto* par = p.apvts.getParameter (pid);
            par->setValueNotifyingHost (0.37f);
            p.setParameterLocked (pid, true);
            lockedVals[pid] = par->getValue();
        }
        p.apvts.getParameter (vsx::id::mutationAmount)->setValueNotifyingHost (1.f);
        for (int i = 0; i < 12; ++i) { p.randomizeAll(); p.mutateCurrent(); }
        for (const char* pid : fxIds)
            check (std::abs (p.apvts.getParameter (pid)->getValue() - lockedVals[pid]) < 1.0e-6f,
                   String ("locked effect parameter unchanged by RANDOM/MUTATE: ") + pid);
        p.clearParameterLocks();

        // MUTATE moves them (continuous and choice), and respects the on/off thresholds.
        p.resetAllToDefaults();
        p.apvts.getParameter (vsx::id::mutationAmount)->setValueNotifyingHost (1.f);
        std::map<String, float> base;
        for (const char* pid : fxIds) base[pid] = p.apvts.getParameter (pid)->getValue();
        std::set<String> moved;
        for (int i = 0; i < 400; ++i)
        {
            p.mutateCurrent();
            for (const char* pid : fxIds)
                if (std::abs (p.apvts.getParameter (pid)->getValue() - base[pid]) > 1.0e-5f)
                    moved.insert (pid);
        }
        for (const char* pid : { "tqCutoff", "tqRes", "tqEnv", "tqMix", "clotFree", "clotFeedback",
                                 "clotTone", "clotSpread", "clotMix", "tqMode", "clotTime", "tqOn", "clotOn" })
            check (moved.count (pid) > 0, String ("MUTATE can move ") + pid);

        p.resetAllToDefaults();
        p.apvts.getParameter (vsx::id::mutationAmount)->setValueNotifyingHost (0.69f);
        bool flipped = false;
        for (int i = 0; i < 400; ++i)
        {
            p.mutateCurrent();
            flipped = flipped || p.apvts.getParameter (vsx::id::tqOn)->getValue() > 0.5f
                              || p.apvts.getParameter (vsx::id::clotOn)->getValue() > 0.5f;
        }
        check (! flipped, "effect On switches never flip below mutation amount 0.70");

        // RESET and hard reset restore defaults.
        for (const char* pid : fxIds) p.apvts.getParameter (pid)->setValueNotifyingHost (0.81f);
        p.resetAllToDefaults();
        VivisectProcessor fresh;
        bool allDefault = true;
        for (const char* pid : fxIds)
            allDefault = allDefault && std::abs (p.apvts.getParameter (pid)->getValue()
                                                 - fresh.apvts.getParameter (pid)->getValue()) < 1.0e-6f;
        check (allDefault, "RESET restores every effect parameter");

        // A/B compare covers them.
        p.resetAllToDefaults();
        p.selectABSlot (0);
        p.apvts.getParameter ("tqRes")->setValueNotifyingHost (0.9f);
        p.apvts.getParameter ("clotMix")->setValueNotifyingHost (0.77f);
        p.selectABSlot (1);
        p.apvts.getParameter ("tqRes")->setValueNotifyingHost (0.1f);
        p.selectABSlot (0);
        check (std::abs (p.apvts.getParameter ("tqRes")->getValue() - 0.9f) < 1.0e-4f
               && std::abs (p.apvts.getParameter ("clotMix")->getValue() - 0.77f) < 1.0e-4f,
               "A/B swap restores effect parameters");
        p.selectABSlot (0);

        // State round trip with non-default effect values.
        for (const char* pid : fxIds)
            p.apvts.getParameter (pid)->setValueNotifyingHost (String (pid).endsWith ("On") ? 1.f : 0.63f);
        MemoryBlock blob;
        p.getStateInformation (blob);
        VivisectProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        bool same = true;
        String firstBad;
        for (const char* pid : fxIds)
            if (std::abs (q.apvts.getParameter (pid)->getValue() - p.apvts.getParameter (pid)->getValue()) >= 1.0e-4f)
            {
                same = false;
                if (firstBad.isEmpty()) firstBad = pid;
            }
        check (same, "session state round trip keeps the effect parameters (first mismatch: " + firstBad + ")");

        // A preset saved before the effects existed loads them at defaults.
        VivisectProcessor older;
        older.prepareToPlay (kRate, kBlock);
        const auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx_fx_old_preset.vsxpreset");
        check (older.savePresetToFile (tmp), "wrote a preset to strip");
        if (auto xml = XmlDocument::parse (tmp))
        {
            for (int i = xml->getNumChildElements() - 1; i >= 0; --i)
                if (auto* ch = xml->getChildElement (i))
                    for (const char* pid : fxIds)
                        if (ch->getStringAttribute ("id") == pid) { xml->removeChildElement (ch, true); break; }
            xml->writeTo (tmp);
        }
        for (const char* pid : fxIds) q.apvts.getParameter (pid)->setValueNotifyingHost (0.9f);
        check (q.loadPresetFromFile (tmp), "loaded the stripped preset");
        bool defaulted = true;
        for (const char* pid : fxIds)
            defaulted = defaulted && std::abs (q.apvts.getParameter (pid)->getValue()
                                               - fresh.apvts.getParameter (pid)->getValue()) < 1.0e-4f;
        check (defaulted, "an older preset loads the new effect parameters at defaults");
        tmp.deleteFile();

        // Fresh instance + old session blob: sounds identical to before (effects off).
        check (fresh.apvts.getRawParameterValue ("tqOn")->load() < 0.5f
               && fresh.apvts.getRawParameterValue ("clotOn")->load() < 0.5f,
               "new effects default to off so older sessions sound unchanged");
    }
}
