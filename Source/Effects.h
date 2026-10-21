#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ============================================================================
//  VIVISECT — post-rack effects (0.10.0)
//    TOURNIQUET : stereo TPT state-variable filter that clamps the signal off
//    CLOT       : tempo-synced stereo delay with a damped, soft-clipped loop
//  Header-only. Nothing here allocates outside prepare(), takes a lock or
//  touches a file. Both stages are an exact no-op when their mix is zero.
// ============================================================================
namespace vsx
{
namespace fxdetail
{
    inline float clean (float v) noexcept
    {
        // NaN/Inf -> 0, denormals -> 0
        if (! std::isfinite (v)) return 0.f;
        return std::abs (v) < 1.0e-20f ? 0.f : v;
    }
    inline float onePoleCoef (double sr, double seconds) noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / juce::jmax (1.0, seconds * sr)));
    }
}

// ---------------------------------------------------------------------------
//  TOURNIQUET
// ---------------------------------------------------------------------------
class Tourniquet
{
public:
    enum Mode { LowPass = 0, BandPass, HighPass, Notch };

    void prepare (double sr)
    {
        sampleRate = juce::jmax (1000.0, sr);
        smoothCoef = fxdetail::onePoleCoef (sampleRate, 0.010);
        envAttack  = fxdetail::onePoleCoef (sampleRate, 0.002);
        envRelease = fxdetail::onePoleCoef (sampleRate, 0.080);
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch) c = {};
        env = 0.f;
        snap = true;
        coefCounter = 0;
    }

    // Resonance 0..1 -> Q 0.5..18 (exponential).
    static float resToQ (float res) noexcept
    {
        return 0.5f * std::pow (36.f, juce::jlimit (0.f, 1.f, res));
    }

    // Cutoff after the envelope follower: +-4 octaves at full depth, clamped.
    static float effectiveCutoff (float baseHz, float env01, float depth, double sr) noexcept
    {
        const float oct = juce::jlimit (-1.f, 1.f, depth) * 4.f * juce::jlimit (0.f, 1.f, env01);
        const float hz = baseHz * std::exp2 (oct);
        return juce::jlimit (20.f, (float) (0.45 * sr), hz);
    }

    void process (juce::AudioBuffer<float>& buf, int n, int mode, float cutoffHz, float res,
                  float envDepth, float mix) noexcept
    {
        if (! prepared || n <= 0 || buf.getNumChannels() < 1)
            return;

        mix = std::isfinite (mix) ? juce::jlimit (0.f, 1.f, mix) : 0.f;
        res = std::isfinite (res) ? juce::jlimit (0.f, 1.f, res) : 0.f;
        envDepth = std::isfinite (envDepth) ? juce::jlimit (-1.f, 1.f, envDepth) : 0.f;
        cutoffHz = std::isfinite (cutoffHz) ? juce::jlimit (20.f, 20000.f, cutoffHz) : 1200.f;
        mode = juce::jlimit (0, 3, mode);

        if (snap) { cutS = cutoffHz; resS = res; mixS = mix; snap = false; }

        if (mix <= 1.0e-4f && mixS <= 1.0e-4f)
        {
            // exact bypass: leave the buffer alone and forget the filter
            mixS = mix;
            cutS = cutoffHz;
            resS = res;
            for (auto& c : ch) c = {};
            env = 0.f;
            return;
        }

        const int nch = juce::jmin (2, buf.getNumChannels());
        float* io[2] { buf.getWritePointer (0), buf.getWritePointer (nch > 1 ? 1 : 0) };

        for (int i = 0; i < n; ++i)
        {
            cutS += (cutoffHz - cutS) * smoothCoef;
            resS += (res - resS) * smoothCoef;
            mixS += (mix - mixS) * smoothCoef;

            const float l = fxdetail::clean (io[0][i]);
            const float r = nch > 1 ? fxdetail::clean (io[1][i]) : l;

            const float level = 0.5f * (std::abs (l) + std::abs (r));
            env += (level - env) * (level > env ? envAttack : envRelease);
            env = fxdetail::clean (env);

            if ((coefCounter++ & 7) == 0)
            {
                const float fc = effectiveCutoff (cutS, juce::jmin (1.f, env * 2.f), envDepth, sampleRate);
                g = std::tan (juce::MathConstants<float>::pi * fc / (float) sampleRate);
                k = 1.f / resToQ (resS);
                a1 = 1.f / (1.f + k * g + g * g);
            }

            const float m = mixS;
            const float yl = tick (ch[0], l, mode);
            io[0][i] = l * (1.f - m) + yl * m;
            if (nch > 1)
            {
                const float yr = tick (ch[1], r, mode);
                io[1][i] = r * (1.f - m) + yr * m;
            }
            else
            {
                ch[1] = {};
            }
        }
    }

private:
    struct State { float s1 = 0.f, s2 = 0.f; };

    float tick (State& st, float x, int mode) noexcept
    {
        // The tanh on the bandpass integrator in the feedback path is what
        // keeps a Q of 18 bounded for any input; at normal levels it is linear.
        const float hp = (x - (k + g) * std::tanh (st.s1) - st.s2) * a1;
        const float v1 = g * hp;
        const float bp = v1 + st.s1;
        st.s1 = bp + v1;
        const float v2 = g * bp;
        const float lp = v2 + st.s2;
        st.s2 = lp + v2;

        st.s1 = fxdetail::clean (juce::jlimit (-64.f, 64.f, st.s1));
        st.s2 = fxdetail::clean (juce::jlimit (-64.f, 64.f, st.s2));

        float y = 0.f;
        switch (mode)
        {
            case LowPass:  y = lp;           break;
            case BandPass: y = bp * k;       break;   // unity gain at the centre
            case HighPass: y = hp;           break;
            default:       y = x - k * bp;   break;   // notch
        }
        // Transparent below about 1.0 (-0.1 dB), hard-bounded at 6 for any Q.
        return 6.f * std::tanh (fxdetail::clean (y) * (1.f / 6.f));
    }

    double sampleRate = 44100.0;
    bool prepared = false, snap = true;
    float smoothCoef = 0.01f, envAttack = 0.01f, envRelease = 0.001f;
    float cutS = 1200.f, resS = 0.25f, mixS = 1.f;
    float env = 0.f;
    float g = 0.1f, k = 1.f, a1 = 1.f;
    unsigned coefCounter = 0;
    State ch[2];
};

// ---------------------------------------------------------------------------
//  CLOT
// ---------------------------------------------------------------------------
class Clot
{
public:
    static constexpr int kTimeFree = 8;
    static constexpr double kMaxSeconds = 4.0;

    void prepare (double sr)
    {
        sampleRate = juce::jmax (1000.0, sr);
        capacity = (int) std::ceil (kMaxSeconds * sampleRate) + 8;
        for (auto& l : lines)
            l.assign ((size_t) capacity, 0.f);
        glideCoef = fxdetail::onePoleCoef (sampleRate, 0.060);
        smoothCoef = fxdetail::onePoleCoef (sampleRate, 0.020);
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& l : lines)
            std::fill (l.begin(), l.end(), 0.f);
        widx = 0;
        lpState[0] = lpState[1] = 0.f;
        snap = true;
        lineClean = true;
    }

    // Delay in samples for a Time choice (before the 4 s clamp).
    static double delaySamples (int choice, float freeMs, double samplesPerBeat, double sr) noexcept
    {
        static constexpr double beats[8] { 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 };
        if (choice >= 0 && choice < 8)
            return beats[choice] * samplesPerBeat;
        return (double) juce::jlimit (20.f, 2000.f, freeMs) * 0.001 * sr;
    }

    void process (juce::AudioBuffer<float>& buf, int n, double samplesPerBeat, int timeChoice,
                  float freeMs, float feedback, float toneHz, float spread, float mix) noexcept
    {
        if (! prepared || n <= 0 || buf.getNumChannels() < 1)
            return;

        mix      = std::isfinite (mix) ? juce::jlimit (0.f, 1.f, mix) : 0.f;
        feedback = std::isfinite (feedback) ? juce::jlimit (0.f, 0.95f, feedback) : 0.f;
        spread   = std::isfinite (spread) ? juce::jlimit (0.f, 1.f, spread) : 0.f;
        toneHz   = std::isfinite (toneHz) ? juce::jlimit (200.f, 16000.f, toneHz) : 5000.f;
        freeMs   = std::isfinite (freeMs) ? freeMs : 375.f;
        if (! (samplesPerBeat > 1.0) || ! std::isfinite (samplesPerBeat))
            samplesPerBeat = 0.5 * sampleRate;                    // 120 BPM fallback

        const double maxD = kMaxSeconds * sampleRate;
        const double targetD = juce::jlimit (1.0, maxD, delaySamples (timeChoice, freeMs, samplesPerBeat, sampleRate));

        if (snap) { delayS = targetD; fbS = feedback; mixS = mix; spreadS = spread; toneS = toneHz; snap = false; }

        if (mix <= 1.0e-4f && mixS <= 1.0e-4f)
        {
            mixS = mix;
            if (! lineClean)
                reset();                   // exact bypass; the line is forgotten (once)
            return;
        }

        lineClean = false;
        const int nch = juce::jmin (2, buf.getNumChannels());
        float* io[2] { buf.getWritePointer (0), buf.getWritePointer (nch > 1 ? 1 : 0) };

        for (int i = 0; i < n; ++i)
        {
            delayS += (targetD - delayS) * (double) glideCoef;
            if (std::abs (targetD - delayS) < 0.02) delayS = targetD;
            fbS     += (feedback - fbS) * smoothCoef;
            mixS    += (mix - mixS) * smoothCoef;
            spreadS += (spread - spreadS) * smoothCoef;
            toneS   += (toneHz - toneS) * smoothCoef;

            const float dl = fxdetail::clean (io[0][i]);
            const float dr = nch > 1 ? fxdetail::clean (io[1][i]) : dl;
            const float in = 0.5f * (dl + dr);

            const float wl = readLine (0, delayS);
            const float wr = readLine (1, delayS);

            const float s = spreadS;
            const float cl = (1.f - s) * wl + s * wr;
            const float cr = (1.f - s) * wr + s * wl;

            const float a = 1.f - std::exp (-juce::MathConstants<float>::twoPi
                                            * juce::jmin (toneS, (float) (0.45 * sampleRate)) / (float) sampleRate);
            lpState[0] = fxdetail::clean (lpState[0] + a * (cl - lpState[0]));
            lpState[1] = fxdetail::clean (lpState[1] + a * (cr - lpState[1]));

            // soft clipper in the loop: bounded for any feedback, forever
            const float fl = std::tanh (lpState[0] * fbS);
            const float fr = std::tanh (lpState[1] * fbS);

            lines[0][(size_t) widx] = fxdetail::clean (in + fl);
            lines[1][(size_t) widx] = fxdetail::clean (in * (1.f - s) + fr);
            if (++widx >= capacity) widx = 0;

            const float m = mixS;
            io[0][i] = dl * (1.f - m) + wl * m;
            if (nch > 1)
                io[1][i] = dr * (1.f - m) + wr * m;
        }
    }

    int capacitySamples() const noexcept { return capacity; }

private:
    float readLine (int c, double delay) const noexcept
    {
        double pos = (double) widx - delay;
        while (pos < 0.0) pos += (double) capacity;
        const int i0 = (int) pos;
        const float frac = (float) (pos - (double) i0);
        const int i1 = i0 + 1 >= capacity ? 0 : i0 + 1;
        const auto& l = lines[(size_t) c];
        return l[(size_t) i0] + (l[(size_t) i1] - l[(size_t) i0]) * frac;
    }

    double sampleRate = 44100.0;
    bool prepared = false, snap = true, lineClean = true;
    int capacity = 0, widx = 0;
    float glideCoef = 0.001f, smoothCoef = 0.01f;
    double delayS = 0.0;
    float fbS = 0.f, mixS = 0.f, spreadS = 0.f, toneS = 5000.f;
    float lpState[2] { 0.f, 0.f };
    std::vector<float> lines[2];
};

} // namespace vsx
