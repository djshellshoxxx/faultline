#include "Dsp.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace vsx
{
// ===========================================================================
//  SpecimenBuffer
// ===========================================================================
void SpecimenBuffer::prepare (double sampleRate, double maxSeconds)
{
    sr  = sampleRate;
    cap = juce::jmax (1024, (int) (sampleRate * maxSeconds));
    buffer.setSize (2, cap);
    reset();
}
void SpecimenBuffer::reset()
{
    buffer.clear();
    totalWritten = 0;
    frozen = false;
}
void SpecimenBuffer::push (const juce::AudioBuffer<float>& in) noexcept
{
    if (frozen.load()) return;
    const int n = in.getNumSamples();
    const int inCh = in.getNumChannels();
    const float* l = in.getReadPointer (0);
    const float* r = inCh > 1 ? in.getReadPointer (1) : l;
    float* b0 = buffer.getWritePointer (0);
    float* b1 = buffer.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const int idx = wrap (totalWritten + i);
        b0[idx] = l[i];
        b1[idx] = r[i];
    }
    totalWritten += n;
}
void SpecimenBuffer::reinject (const float* L, const float* R, int n, i64 startAbs, float amt) noexcept
{
    if (amt <= 1.0e-4f) return;
    amt = juce::jlimit (0.0f, 1.2f, amt);
    float* b0 = buffer.getWritePointer (0);
    float* b1 = buffer.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const int idx = wrap (startAbs + i);
        b0[idx] = std::tanh (b0[idx] * (1.0f - 0.2f * amt) + L[i] * amt);
        b1[idx] = std::tanh (b1[idx] * (1.0f - 0.2f * amt) + R[i] * amt);
    }
}
float SpecimenBuffer::readInterp (int ch, double absPos) const noexcept
{
    const double lo = (double) totalWritten - (double) cap + 4.0;
    const double hi = (double) totalWritten - 2.0;
    if (absPos < lo || absPos > hi) return 0.0f;
    const i64 i0 = (i64) std::floor (absPos);
    const float fr = (float) (absPos - (double) i0);
    const int a = wrap (i0);
    const int b = wrap (i0 + 1);
    const float* d = buffer.getReadPointer (juce::jlimit (0, 1, ch));
    return d[a] + (d[b] - d[a]) * fr;
}
void SpecimenBuffer::setSample (int slot, juce::AudioBuffer<float>&& buf, double srcRate)
{
    if (slot < 0 || slot > 1 || buf.getNumSamples() < 2) return;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (2, buf.getNumSamples());
    nb->copyFrom (0, 0, buf, 0, 0, buf.getNumSamples());
    nb->copyFrom (1, 0, buf, buf.getNumChannels() > 1 ? 1 : 0, 0, buf.getNumSamples());
    abBuf[slot]  = std::move (nb);
    abRate[slot] = srcRate > 0.0 ? srcRate : sr;
}
void SpecimenBuffer::clearSample (int slot)
{
    if (slot >= 0 && slot < 2) abBuf[slot].reset();
}
float SpecimenBuffer::readAB (int slot, int ch, double absPos) const noexcept
{
    const auto* b = (slot >= 0 && slot < 2) ? abBuf[slot].get() : nullptr;
    if (b == nullptr) return 0.0f;
    const int len = b->getNumSamples();
    if (len < 2) return 0.0f;
    const double ratio = abRate[slot] / sr;
    double pos = std::fmod (absPos * ratio, (double) len);
    if (pos < 0) pos += len;
    const int i0 = (int) pos;
    const int i1 = (i0 + 1) % len;
    const float fr = (float) (pos - i0);
    const float* d = b->getReadPointer (juce::jlimit (0, b->getNumChannels() - 1, ch));
    return d[i0] + (d[i1] - d[i0]) * fr;
}

// ===========================================================================
//  TransientDetector
// ===========================================================================
void TransientDetector::prepare (double s)
{
    sampleRate = s;
    refractory = juce::jmax (1, (int) (0.03 * s));
    reset();
}
void TransientDetector::reset()
{
    ev.fill ({});
    head = 0;
    prevX = fast = slow = 0.f;
    sinceLast = refractory;
}
void TransientDetector::process (const juce::AudioBuffer<float>& in, i64 blockStartAbs) noexcept
{
    const int n = in.getNumSamples();
    const int chs = in.getNumChannels();
    const float* l = in.getReadPointer (0);
    const float* r = chs > 1 ? in.getReadPointer (1) : l;
    const float fc = 1.f - std::exp (-1.f / (0.002f * (float) sampleRate));
    const float sc = 1.f - std::exp (-1.f / (0.08f  * (float) sampleRate));
    for (int i = 0; i < n; ++i)
    {
        const float x  = 0.5f * (l[i] + r[i]);
        const float hp = x - prevX;
        prevX = x;
        const float e = std::abs (hp);
        fast += (e - fast) * fc;
        slow += (e - slow) * sc;
        if (fast > slow * 1.6f + 1.0e-4f && sinceLast >= refractory)
        {
            ev[head] = { blockStartAbs + i, slow > 1.0e-6f ? fast / slow : 4.f };
            head = (head + 1) % kMax;
            sinceLast = 0;
        }
        else if (sinceLast < (1 << 30))
            ++sinceLast;
    }
}
i64 TransientDetector::nearest (i64 target, i64 minAbs) const noexcept
{
    i64 best = target, bd = std::numeric_limits<i64>::max();
    for (const auto& e : ev)
    {
        if (e.pos <= 0 || e.pos < minAbs) continue;
        const i64 d = std::llabs ((long long) (e.pos - target));
        if (d < bd) { bd = d; best = e.pos; }
    }
    return best;
}
void TransientDetector::copyRecent (std::vector<i64>& out, i64 sinceAbs) const
{
    out.clear();
    for (const auto& e : ev)
        if (e.pos >= sinceAbs) out.push_back (e.pos);
}

// ===========================================================================
//  SpectralAnalyzer
// ===========================================================================
void SpectralAnalyzer::prepare (double s)
{
    sampleRate = s;
    fftBuf.assign ((size_t) (2 * kSize), 0.f);
    timeBuf.assign ((size_t) kSize, 0.f);
    frames.assign ((size_t) kMaxFrames, Frame {});
    reset();
}
void SpectralAnalyzer::reset()
{
    frameHead = frameCount = hopCounter = 0;
    latestC01.store (0.f);
}
void SpectralAnalyzer::process (const SpecimenBuffer& spec, int numSamples) noexcept
{
    hopCounter += numSamples;
    if (hopCounter < kHop) return;
    hopCounter = 0;

    const i64 end = spec.writePos();
    if (end < (i64) (kSize + 8)) return;

    for (int i = 0; i < kSize; ++i)
        timeBuf[(size_t) i] = spec.readInterp (0, (double) (end - kSize + i));

    window.multiplyWithWindowingTable (timeBuf.data(), (size_t) kSize);
    std::fill (fftBuf.begin(), fftBuf.end(), 0.f);
    std::copy (timeBuf.begin(), timeBuf.end(), fftBuf.begin());
    fft.performRealOnlyForwardTransform (fftBuf.data());

    const int bins = kSize / 2;
    double sumM = 0, sumKM = 0, sumLog = 0, sumM2 = 0;
    for (int k = 1; k < bins; ++k)
    {
        const float re = fftBuf[(size_t) (2 * k)];
        const float im = fftBuf[(size_t) (2 * k + 1)];
        const float m  = std::sqrt (re * re + im * im);
        sumM   += m;
        sumKM  += (double) m * k;
        sumLog += std::log (m + 1.0e-9);
        sumM2  += (double) m * m;
    }
    const float centroidHz = sumM > 1.0e-6 ? (float) (sumKM / sumM) * (float) sampleRate / kSize : 0.f;
    const float flat = sumM > 1.0e-6 ? (float) (std::exp (sumLog / (bins - 1)) / (sumM / (bins - 1))) : 1.f;
    const float rms  = (float) std::sqrt (sumM2) / bins;

    frames[(size_t) frameHead] = { end, centroidHz, flat, rms };
    frameHead  = (frameHead + 1) % kMaxFrames;
    frameCount = juce::jmin (frameCount + 1, kMaxFrames);

    latestC01.store (juce::jlimit (0.f, 1.f,
        std::log2 ((centroidHz + 20.f) / 40.f) / std::log2 (18000.f / 40.f)));
}
i64 SpectralAnalyzer::brightest (i64 a, i64 b) const noexcept
{
    i64 best = b; float bv = -1.f;
    for (int i = 0; i < frameCount; ++i)
    {
        const auto& f = frames[(size_t) i];
        if (f.pos < a || f.pos > b) continue;
        if (f.centroid > bv) { bv = f.centroid; best = f.pos; }
    }
    return best;
}
i64 SpectralAnalyzer::loudest (i64 a, i64 b) const noexcept
{
    i64 best = b; float bv = -1.f;
    for (int i = 0; i < frameCount; ++i)
    {
        const auto& f = frames[(size_t) i];
        if (f.pos < a || f.pos > b) continue;
        if (f.rms > bv) { bv = f.rms; best = f.pos; }
    }
    return best;
}
i64 SpectralAnalyzer::mostTonal (i64 a, i64 b) const noexcept
{
    i64 best = b; float bv = 2.f;
    for (int i = 0; i < frameCount; ++i)
    {
        const auto& f = frames[(size_t) i];
        if (f.pos < a || f.pos > b) continue;
        if (f.flat < bv) { bv = f.flat; best = f.pos; }
    }
    return best;
}
void SpectralAnalyzer::fillBrightness (float* out, int n, i64 startAbs, i64 endAbs) const noexcept
{
    for (int i = 0; i < n; ++i) out[i] = -1.f;
    const double span = juce::jmax (1.0, (double) (endAbs - startAbs));
    const float k = std::log2 (18000.f / 40.f);
    for (int i = 0; i < frameCount; ++i)
    {
        const auto& f = frames[(size_t) i];
        if (f.pos < startAbs || f.pos >= endAbs) continue;
        const int c = juce::jlimit (0, n - 1, (int) ((f.pos - startAbs) / span * n));
        out[c] = juce::jmax (out[c], juce::jlimit (0.f, 1.f, std::log2 ((f.centroid + 20.f) / 40.f) / k));
    }
    float last = 0.f;
    for (int i = 0; i < n; ++i)
    {
        if (out[i] < 0.f) out[i] = last;
        else              last   = out[i];
    }
}
float SpectralAnalyzer::centroidAt01 (i64 pos) const noexcept
{
    i64 bd = std::numeric_limits<i64>::max(); float hz = 0.f;
    for (int i = 0; i < frameCount; ++i)
    {
        const auto& f = frames[(size_t) i];
        const i64 d = std::llabs ((long long) (f.pos - pos));
        if (d < bd) { bd = d; hz = f.centroid; }
    }
    return juce::jlimit (0.f, 1.f, std::log2 ((hz + 20.f) / 40.f) / std::log2 (18000.f / 40.f));
}

// ===========================================================================
//  Chaos + grid helpers
// ===========================================================================
ChaosField deriveChaos (float cIn) noexcept
{
    const float c = juce::jlimit (0.f, 1.f, cIn);
    const float sq = c * c;
    ChaosField f;
    f.triggerJitter = sq * 0.9f;
    f.lengthJitter  = juce::jlimit (0.f, 1.f, (c - 0.15f) / 0.85f) * 0.8f;
    f.skipProb      = juce::jlimit (0.f, 0.55f, c - 0.30f);
    f.wrongSlice    = juce::jlimit (0.f, 0.90f, (c - 0.45f) * 1.6f);
    f.routeSwap     = juce::jlimit (0.f, 0.50f, (c - 0.60f) * 1.2f);
    f.feedback      = juce::jlimit (0.f, 1.f,  (c - 0.55f) * 2.0f);
    f.pitchChaos    = sq;
    f.gridBypass    = juce::jlimit (0.f, 1.f,  (c - 0.70f) * 3.0f);
    return f;
}
double gridStepSamples (int gridIndex, double spb) noexcept
{
    double div;
    switch (gridIndex)
    {
        case 1:  div = 0.125;      break;
        case 2:  div = 1.0 / 3.0;  break;
        case 3:  div = 0.375;      break;
        default: div = 0.25;       break;
    }
    return div * spb;
}
double snapToGrid (double pos, int gridIndex, double spb, float pull, float swing) noexcept
{
    if (gridIndex == 4) return pos;
    const double step = gridStepSamples (gridIndex, spb);
    double snapped = std::round (pos / step) * step;
    const long long idx = (long long) std::llround (pos / step);
    if (idx & 1) snapped += swing * step * 0.33;
    return pos + (snapped - pos) * juce::jlimit (0.f, 1.f, pull);
}

// ===========================================================================
//  source read
// ===========================================================================
float readSourceS (const SpecimenBuffer& spec, int sel, float morph, int ch, double absPos) noexcept
{
    switch (sel)
    {
        case 1: return spec.hasSample (0) ? spec.readAB (0, ch, absPos) : spec.readInterp (ch, absPos);
        case 2: return spec.hasSample (1) ? spec.readAB (1, ch, absPos) : spec.readInterp (ch, absPos);
        case 3:
        {
            const float a = spec.hasSample (0) ? spec.readAB (0, ch, absPos) : spec.readInterp (ch, absPos);
            const float b = spec.hasSample (1) ? spec.readAB (1, ch, absPos) : spec.readInterp (ch, absPos);
            return a + (b - a) * morph;
        }
        default: return spec.readInterp (ch, absPos);
    }
}
float readSource (const TriggerContext& c, int ch, double absPos) noexcept
{
    return readSourceS (*c.specimen, c.sourceSelect, c.morph, ch, absPos);
}

// ===========================================================================
//  Surgeons
// ===========================================================================
int buildEuclideanPattern (int steps, int pulses, std::array<int, 64>& pattern) noexcept
{
    pattern.fill (0);
    steps  = juce::jlimit (1, (int) pattern.size(), steps);
    pulses = juce::jlimit (0, steps, pulses);
    if (pulses == 0)
        return steps;
    if (pulses == steps)
    {
        for (int i = 0; i < steps; ++i) pattern[(size_t) i] = 1;
        return steps;
    }

    // Bresenham/Bjorklund-equivalent distribution. Starting with a pulse gives
    // a stable canonical rotation, while every circular gap differs by at most
    // one step.
    int bucket = 0;
    for (int i = 0; i < steps; ++i)
    {
        bucket += pulses;
        if (bucket >= steps)
        {
            bucket -= steps;
            pattern[(size_t) i] = 1;
        }
    }

    // Rotate so the canonical pattern begins on a pulse.
    int first = 0;
    while (first < steps && pattern[(size_t) first] == 0) ++first;
    if (first > 0 && first < steps)
    {
        std::array<int, 64> copy = pattern;
        for (int i = 0; i < steps; ++i)
            pattern[(size_t) i] = copy[(size_t) ((i + first) % steps)];
    }
    return steps;
}

// ---------------------------------------------------------------------------
namespace
{
inline float lerpBuf (const juce::AudioBuffer<float>& b, int ch, double p, int len)
{
    int i0 = (int) p;
    if (i0 < 0) i0 = 0;
    if (i0 > len - 2) i0 = len - 2;
    const float fr = (float) (p - i0);
    return b.getSample (ch, i0) + (b.getSample (ch, i0 + 1) - b.getSample (ch, i0)) * fr;
}

// ---- STUTTER : beat repeat + ratchet + micro pitch drift -------------------
class Stutter final : public Surgeon
{
public:
    void prepare (double s, int) override { sr = s; slice.setSize (2, (int) (s * 2.0)); reset(); }
    void reset() override { slice.clear(); active = false; act = 0.f; repeatsLeft = 0; }
    void trigger (const TriggerContext& c) override
    {
        sliceLen = juce::jlimit (64, slice.getNumSamples(), (int) c.lengthSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sliceLen; ++i)
                slice.setSample (ch, i, readSource (c, ch, c.sourcePos + i));
        const double ratchet = 1.0 + std::floor (pp2 * 7.0 + c.chaos * 4.0);
        repeatLen   = juce::jmax (48.0, sliceLen / ratchet);
        repeatsLeft = (int) ratchet * (1 + (int) std::floor (pp2 * 2.0)) + 1;
        pitch = 1.0; rpos = 0.0; gain = 1.0f; active = true; startOff = c.startOffset;
        drift = (pp3 * 0.05f + c.pitchChaos * 0.04f) * (c.rng->nextFloat() * 2.f - 1.f);
        lastGrabStart.store ((i64) c.sourcePos);
        lastGrabLen.store (sliceLen);
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        if (! active) { decayAct (n); return; }
        float* L = add.getWritePointer (0);
        float* R = add.getWritePointer (1);
        const double fade = 0.002 * sr;
        for (int i = startOff; i < n; ++i)
        {
            if (repeatsLeft <= 0) { active = false; break; }
            float w = (float) juce::jmin (1.0, juce::jmin (rpos, repeatLen - rpos) / fade);
            if (w < 0.f) w = 0.f;
            L[i] += lerpBuf (slice, 0, rpos, sliceLen) * w * gain;
            R[i] += lerpBuf (slice, 1, rpos, sliceLen) * w * gain;
            rpos += pitch;
            if (rpos >= repeatLen)
            {
                rpos -= repeatLen;
                --repeatsLeft;
                pitch *= (1.0 + drift);
                gain  *= 0.9f;
            }
        }
        startOff = 0;
        act = active ? 1.f : act * 0.99f;
    }
private:
    juce::AudioBuffer<float> slice;
    int sliceLen = 0, repeatsLeft = 0, startOff = 0;
    double rpos = 0, repeatLen = 0, pitch = 1;
    float gain = 1.f, drift = 0.f;
    bool active = false;
};

// ---- GRANULAR : grain cloud pulled straight from the specimen -------------
class Granular final : public Surgeon
{
public:
    void prepare (double s, int) override { sr = s; reset(); }
    void reset() override { for (auto& g : grains) g.on = false; cloudLeft = 0; act = 0.f; }
    void trigger (const TriggerContext& c) override
    {
        spec = c.specimen; srcSel = c.sourceSelect; morph = c.morph;
        cloudLeft  = (int) (c.samplesPerBeat * (1.0 + pp2 * 3.0));
        center     = c.sourcePos;
        sizeS      = (0.005 + pp1 * 0.195) * sr;
        spawnPer   = sr / (4.0 + pp2 * 76.0);
        sprayCents = pp3 * 2400.f;
        spread     = (0.02 + pp3 * 0.4 + c.chaos * 0.5) * c.samplesPerBeat * 2.0;
        spawnCnt   = c.startOffset;
        act = 1.f;
        lastGrabStart.store ((i64) center);
        lastGrabLen.store ((int) (sizeS * 2.0));
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        float* L = add.getWritePointer (0);
        float* R = add.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            if (cloudLeft > 0)
            {
                if (--spawnCnt <= 0.0)
                {
                    spawnCnt += spawnPer * (0.7 + 0.6 * rng.nextFloat());
                    for (auto& g : grains)
                        if (! g.on)
                        {
                            g.on = true;
                            g.pos  = center + (rng.nextFloat() * 2.f - 1.f) * spread;
                            g.play = 0.0;
                            g.inc  = std::pow (2.0, (rng.nextFloat() * 2.f - 1.f) * sprayCents / 1200.0);
                            g.len  = sizeS * (0.6 + 0.8 * rng.nextFloat());
                            const float pan = rng.nextFloat();
                            g.aL = std::sqrt (1.f - pan);
                            g.aR = std::sqrt (pan);
                            g.amp = 0.5f * (0.6f + 0.4f * rng.nextFloat());
                            break;
                        }
                }
                --cloudLeft;
            }
            for (auto& g : grains)
                if (g.on)
                {
                    const float w = 0.5f - 0.5f * std::cos (6.2831853f * (float) (g.play / g.len));
                    const double rp = g.pos + g.play * g.inc;
                    const float s0 = spec ? readSourceS (*spec, srcSel, morph, 0, rp) : 0.f;
                    const float s1 = spec ? readSourceS (*spec, srcSel, morph, 1, rp) : 0.f;
                    L[i] += s0 * w * g.amp * g.aL;
                    R[i] += s1 * w * g.amp * g.aR;
                    g.play += 1.0;
                    if (g.play >= g.len) g.on = false;
                }
        }
        bool any = false;
        for (auto& g : grains) any |= g.on;
        act = (cloudLeft > 0 || any) ? 1.f : act * 0.98f;
    }
private:
    struct Grain { bool on = false; double pos = 0, play = 0, inc = 1, len = 1; float aL = .5f, aR = .5f, amp = .5f; };
    std::array<Grain, 64> grains;
    SpecimenBuffer* spec = nullptr;
    int srcSel = 0;
    float morph = .5f, sprayCents = 0.f;
    int cloudLeft = 0;
    double center = 0, sizeS = 0, spawnPer = 1, spawnCnt = 0, spread = 0;
    juce::Random rng { 0x9e779 };
};

// ---- REVERSE : reversed slice + amplitude bloom + reverb tail ------------
class Reverse final : public Surgeon
{
public:
    void prepare (double s, int mb) override
    {
        sr = s;
        slice.setSize (2, (int) (s * 2.0));
        rv.setSize (2, juce::jmax (64, mb));
        reverb.setSampleRate (s);
        reset();
    }
    void reset() override { slice.clear(); active = false; tailLeft = 0; act = 0.f; reverb.reset(); }
    void trigger (const TriggerContext& c) override
    {
        sliceLen = juce::jlimit (64, slice.getNumSamples(), (int) c.lengthSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sliceLen; ++i)
                slice.setSample (ch, i, readSource (c, ch, c.sourcePos + i));
        play = sliceLen - 1;
        active = true;
        tailLeft = (int) (sr * 1.2);
        bloom = pp2;
        startOff = c.startOffset;
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.3f + 0.6f * pp3;
        rp.damping  = 0.35f;
        rp.wetLevel = 0.15f + 0.5f * pp3;
        rp.dryLevel = 0.85f;
        rp.width    = 1.f;
        reverb.setParameters (rp);
        lastGrabStart.store ((i64) c.sourcePos);
        lastGrabLen.store (sliceLen);
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        if (! active && tailLeft <= 0) { decayAct (n); return; }
        if (rv.getNumSamples() < n) rv.setSize (2, n, false, false, true);
        rv.clear();
        float* a = rv.getWritePointer (0);
        float* b = rv.getWritePointer (1);
        for (int i = startOff; i < n; ++i)
        {
            if (play >= 0.0)
            {
                const float env = bloom > 0.001f
                    ? std::pow ((float) ((sliceLen - 1 - play) / juce::jmax (1.0, (double) (sliceLen - 1))), 1.f + bloom * 3.f)
                    : 1.f;
                a[i] = lerpBuf (slice, 0, play, sliceLen) * env;
                b[i] = lerpBuf (slice, 1, play, sliceLen) * env;
                play -= 1.0;
                if (play < 0.0) active = false;
            }
            else --tailLeft;
        }
        reverb.processStereo (a, b, n);
        add.addFrom (0, 0, rv, 0, 0, n);
        add.addFrom (1, 0, rv, 1, 0, n);
        startOff = 0;
        act = play >= 0.0 ? 1.f : (tailLeft > 0 ? juce::jmax (0.25f, act * 0.995f) : act * 0.97f);
    }
private:
    juce::AudioBuffer<float> slice, rv;
    juce::Reverb reverb;
    int sliceLen = 0, tailLeft = 0, startOff = 0;
    double play = 0;
    float bloom = 0.f;
    bool active = false;
};

// ---- CORRUPT : bitcrush / SR reduce / dropouts / clicks / tape+azimuth ---
class Corrupt final : public Surgeon
{
public:
    void prepare (double s, int) override { sr = s; slice.setSize (2, (int) (s * 2.0)); reset(); }
    void reset() override { slice.clear(); active = false; act = 0.f; azlp = 0.f; }
    void trigger (const TriggerContext& c) override
    {
        sliceLen = juce::jlimit (64, slice.getNumSamples(), (int) c.lengthSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sliceLen; ++i)
                slice.setSample (ch, i, readSource (c, ch, c.sourcePos + i));
        durLeft  = (int) (c.samplesPerBeat * 2.0);
        readPos  = 0.0;
        active   = true;
        startOff = c.startOffset;
        chaosAmt = c.chaos;
        holdCnt  = 0; wowPhase = 0.0; dropRun = 0;
        lastGrabStart.store ((i64) c.sourcePos);
        lastGrabLen.store (sliceLen);
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        if (! active) { decayAct (n); return; }
        const float bits = 1.f + pp1 * 15.f;
        const float step = 2.f / std::pow (2.f, bits);
        const int   dsF  = 1 + (int) (pp2 * 49.f);
        const float mDrop  = juce::jlimit (0.f, 1.f, 1.f - 2.f * pp3);
        const float mClick = 1.f - std::abs (2.f * pp3 - 1.f);
        const float mTape  = juce::jlimit (0.f, 1.f, 2.f * pp3 - 1.f);
        float* L = add.getWritePointer (0);
        float* R = add.getWritePointer (1);
        for (int i = startOff; i < n; ++i)
        {
            if (durLeft-- <= 0) { active = false; break; }
            double rp = readPos + mTape * 4.0 * std::sin (wowPhase);
            wowPhase += 6.2831853 / (sr * 0.9);
            float sL = lerpBuf (slice, 0, rp, sliceLen);
            float sR = lerpBuf (slice, 1, rp, sliceLen);
            if (holdCnt <= 0) { heldL = sL; heldR = sR; holdCnt = dsF; }
            --holdCnt;
            float oL = std::round (heldL / step) * step;
            float oR = std::round (heldR / step) * step;
            if (dropRun > 0) { oL = oR = 0.f; --dropRun; }
            else if (rng.nextFloat() < 0.004f * mDrop * (1.f + chaosAmt))
                dropRun = (int) (sr * 0.01 * (0.5 + rng.nextFloat()));
            if (rng.nextFloat() < 0.0006f * mClick * (1.f + chaosAmt))
            {
                const float cl = (rng.nextFloat() * 2.f - 1.f) * 0.8f;
                oL += cl; oR += cl;
            }
            if (mTape > 0.001f) { azlp += (oR - azlp) * (0.5f - 0.35f * mTape); oR = azlp; }
            L[i] += oL;
            R[i] += oR;
            readPos += 1.0;
            if (readPos >= sliceLen) readPos = 0.0;
        }
        startOff = 0;
        act = active ? 1.f : act * 0.98f;
    }
private:
    juce::AudioBuffer<float> slice;
    int sliceLen = 0, durLeft = 0, startOff = 0, holdCnt = 0, dropRun = 0;
    double readPos = 0, wowPhase = 0;
    float heldL = 0, heldR = 0, azlp = 0, chaosAmt = 0;
    bool active = false;
    juce::Random rng { 0x7c001 };
};

// ---- REORDER : slice a region, permute, replay --------------------------
class Reorder final : public Surgeon
{
public:
    void prepare (double s, int) override { sr = s; region.setSize (2, (int) (s * 4.0)); reset(); }
    void reset() override { region.clear(); active = false; act = 0.f; }
    void trigger (const TriggerContext& c) override
    {
        nSlices = 2 + (int) std::round (pp1 * 14.0);
        const double want = c.lengthSamples * nSlices;
        regionLen = juce::jlimit (nSlices * 64, region.getNumSamples(), (int) want);
        subLen = (double) regionLen / nSlices;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < regionLen; ++i)
                region.setSample (ch, i, readSource (c, ch, c.sourcePos + i));
        buildOrder (juce::jlimit (0, 3, (int) (pp2 * 3.999f)));
        repeatsLeft = 1 + (int) std::round (pp3 * 3.0);
        seqPos = 0; subPlay = 0.0; active = true; startOff = c.startOffset;
        lastGrabStart.store ((i64) c.sourcePos);
        lastGrabLen.store (regionLen);
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        if (! active) { decayAct (n); return; }
        float* L = add.getWritePointer (0);
        float* R = add.getWritePointer (1);
        const double fade = 0.003 * sr;
        for (int i = startOff; i < n; ++i)
        {
            if (repeatsLeft <= 0) { active = false; break; }
            const double rp = order[(size_t) seqPos] * subLen + subPlay;
            float w = (float) juce::jmin (1.0, juce::jmin (subPlay, subLen - subPlay) / fade);
            if (w < 0.f) w = 0.f;
            L[i] += lerpBuf (region, 0, rp, regionLen) * w;
            R[i] += lerpBuf (region, 1, rp, regionLen) * w;
            subPlay += 1.0;
            if (subPlay >= subLen)
            {
                subPlay = 0.0;
                if (++seqPos >= orderLen) { seqPos = 0; --repeatsLeft; }
            }
        }
        startOff = 0;
        act = active ? 1.f : act * 0.98f;
    }
private:
    void buildOrder (int type)
    {
        orderLen = 0;
        const int nn = nSlices;
        auto push = [&] (int v) { if (orderLen < 64) order[(size_t) orderLen++] = ((v % nn) + nn) % nn; };
        if (type == 0)            { for (int k = nn - 1; k >= 0; --k) push (k); }                       // retrograde
        else if (type == 1)      { for (int k = 0; k < nn; ++k) push (k); for (int k = nn - 2; k > 0; --k) push (k); } // palindrome
        else if (type == 2)      { int a = 0, b = 1; for (int k = 0; k < 2 * nn; ++k) { push (a); const int c = a + b; a = b; b = c; } } // fibonacci
        else
        {
            std::array<int, 64> pat {};
            const int pulses = nn / 2 + 1;
            buildEuclideanPattern (nn, pulses, pat);

            // Use the Euclidean rhythm to interleave the two halves of the
            // source region. This keeps every slice exactly once while making
            // the audible permutation follow the maximally-even pattern,
            // instead of front-loading all "hit" slices and then all rests.
            int pulseSlice = 0;
            int restSlice = pulses;
            for (int i = 0; i < nn; ++i)
                push (pat[(size_t) i] != 0 ? pulseSlice++ : restSlice++);
        }
        if (orderLen == 0) push (0);
    }
    juce::AudioBuffer<float> region;
    std::array<int, 64> order {};
    int regionLen = 0, nSlices = 4, orderLen = 0, seqPos = 0, repeatsLeft = 0, startOff = 0;
    double subLen = 0, subPlay = 0;
    bool active = false;
};

// ---- FREEZE : phase-vocoder spectral freeze ---------------------------
class Freeze final : public Surgeon
{
public:
    static constexpr int N = 2048, H = 512, RING = 8192;

    void prepare (double s, int) override
    {
        sr = s;
        mag.assign (N / 2 + 1, 0.f);
        runPhase.assign (N / 2 + 1, 0.f);
        freq.assign ((size_t) (2 * N), 0.f);
        tmp.assign ((size_t) N, 0.f);
        ola.assign (RING, 0.f);
        winTab.assign (N, 0.f);
        for (int i = 0; i < N; ++i) winTab[(size_t) i] = 0.5f - 0.5f * std::cos (6.2831853f * i / (N - 1));
        reset();
    }
    void reset() override { std::fill (ola.begin(), ola.end(), 0.f); active = false; act = 0.f; gain = 0.f; }
    void trigger (const TriggerContext& c) override
    {
        const double off = c.sourcePos + pp1 * juce::jmax (0.0, c.lengthSamples - N);
        for (int i = 0; i < N; ++i)
            tmp[(size_t) i] = 0.5f * (readSource (c, 0, off + i) + readSource (c, 1, off + i));
        window.multiplyWithWindowingTable (tmp.data(), (size_t) N);
        std::fill (freq.begin(), freq.end(), 0.f);
        std::copy (tmp.begin(), tmp.end(), freq.begin());
        fft.performRealOnlyForwardTransform (freq.data());
        for (int k = 0; k <= N / 2; ++k)
        {
            const float re = freq[(size_t) (2 * k)];
            const float im = freq[(size_t) (2 * k + 1)];
            mag[(size_t) k] = std::sqrt (re * re + im * im);
            runPhase[(size_t) k] = std::atan2 (im, re);
        }
        durLeft = (int) ((0.2 + pp2 * 3.8) * sr);
        blur = pp3;
        std::fill (ola.begin(), ola.end(), 0.f);
        rp = 0; hopCd = 0; gain = 1.f; active = true; startOff = c.startOffset;
        synthFrame (0);
        lastGrabStart.store ((i64) off);
        lastGrabLen.store (N);
    }
    void process (juce::AudioBuffer<float>& add, int n) override
    {
        if (! active) { decayAct (n); return; }
        float* L = add.getWritePointer (0);
        float* R = add.getWritePointer (1);
        for (int i = startOff; i < n; ++i)
        {
            if (hopCd <= 0) { synthFrame (rp); hopCd = H; }
            const float s = ola[(size_t) (rp % RING)] * gain;
            ola[(size_t) (rp % RING)] = 0.f;
            L[i] += s; R[i] += s;
            ++rp; --hopCd;
            if (--durLeft <= 0)
            {
                gain -= 1.f / (0.08f * (float) sr);
                if (gain <= 0.f) { gain = 0.f; active = false; break; }
            }
        }
        startOff = 0;
        act = active ? 1.f : act * 0.97f;
    }
private:
    void synthFrame (int at)
    {
        for (int k = 0; k <= N / 2; ++k)
        {
            runPhase[(size_t) k] += 6.2831853f * k * H / (float) N
                                  + blur * (rng.nextFloat() * 2.f - 1.f) * 3.1415927f;
            freq[(size_t) (2 * k)]     = mag[(size_t) k] * std::cos (runPhase[(size_t) k]);
            freq[(size_t) (2 * k + 1)] = mag[(size_t) k] * std::sin (runPhase[(size_t) k]);
        }
        fft.performRealOnlyInverseTransform (freq.data());
        const float scale = (2.0f / 3.0f) / (float) N;
        for (int i = 0; i < N; ++i)
            ola[(size_t) ((at + i) % RING)] += freq[(size_t) i] * winTab[(size_t) i] * scale;
    }
    juce::dsp::FFT fft { 11 };
    juce::dsp::WindowingFunction<float> window { (size_t) N, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> mag, runPhase, freq, tmp, ola, winTab;
    int rp = 0, hopCd = 0, durLeft = 0, startOff = 0;
    float blur = 0.f, gain = 0.f;
    bool active = false;
    juce::Random rng { 0x3fee1 };
};
} // anonymous namespace

std::unique_ptr<Surgeon> makeSurgeon (int idx)
{
    switch (idx)
    {
        case S_STUTTER:  return std::make_unique<Stutter>();
        case S_GRANULAR: return std::make_unique<Granular>();
        case S_REVERSE:  return std::make_unique<Reverse>();
        case S_CORRUPT:  return std::make_unique<Corrupt>();
        case S_REORDER:  return std::make_unique<Reorder>();
        default:         return std::make_unique<Freeze>();
    }
}

// ===========================================================================
//  SurgeonRack
// ===========================================================================
void SurgeonRack::prepare (double sr, int maxBlock)
{
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        surg[(size_t) s] = makeSurgeon (s);
        surg[(size_t) s]->prepare (sr, maxBlock);
    }
    const int mb = juce::jmax (64, maxBlock);
    scratch.setSize (2, mb);
    feedBuf.setSize (2, mb);
    reBuf.setSize (2, mb);
    for (auto& o : outs) o.setSize (2, mb);
}
void SurgeonRack::reset()
{
    for (auto& s : surg) if (s) s->reset();
    maxAct = 0.f;
}
void SurgeonRack::process (SpecimenBuffer& spec, juce::AudioBuffer<float>& wetOut, int n,
                           const std::array<float, kNumSurgeons>& mix,
                           const std::array<int,   kNumSurgeons>& routeIn,
                           float reinjectAmt) noexcept
{
    wetOut.clear();
    feedBuf.clear();

    maxAct = 0.f;
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        auto& o = outs[(size_t) s];
        o.clear (0, 0, n);
        o.clear (1, 0, n);
        surg[(size_t) s]->process (o, n);
        wetOut.addFrom (0, 0, o, 0, 0, n, mix[(size_t) s]);
        wetOut.addFrom (1, 0, o, 1, 0, n, mix[(size_t) s]);
        maxAct = juce::jmax (maxAct, surg[(size_t) s]->activity());
    }
    for (int d = 0; d < kNumSurgeons; ++d)
    {
        const int src = routeIn[(size_t) d] - 1;
        if (src >= 0 && src < kNumSurgeons)
        {
            feedBuf.addFrom (0, 0, outs[(size_t) src], 0, 0, n, 0.5f);
            feedBuf.addFrom (1, 0, outs[(size_t) src], 1, 0, n, 0.5f);
        }
    }
    const bool anyFeed = feedBuf.getMagnitude (0, 0, n) > 1.0e-5f;
    if (reinjectAmt > 1.0e-3f || anyFeed)
    {
        reBuf.clear (0, 0, n);
        reBuf.clear (1, 0, n);
        reBuf.addFrom (0, 0, wetOut, 0, 0, n);
        reBuf.addFrom (1, 0, wetOut, 1, 0, n);
        reBuf.addFrom (0, 0, feedBuf, 0, 0, n);
        reBuf.addFrom (1, 0, feedBuf, 1, 0, n);
        spec.reinject (reBuf.getReadPointer (0), reBuf.getReadPointer (1), n,
                       spec.writePos() - n, reinjectAmt > 0.f ? reinjectAmt : 0.35f);
    }
}

// ===========================================================================
//  SliceScheduler
// ===========================================================================
TriggerContext SliceScheduler::makeManualContext (float p1, int sourceSelect, float morph, float chaos,
                                                  double spb, SpecimenBuffer& spec, SpecimenAnalysis& an)
{
    TriggerContext c;
    c.rng = &rng; c.specimen = &spec; c.analysis = &an;
    c.samplesPerBeat = spb;
    c.chaos = chaos;
    c.pitchChaos = deriveChaos (chaos).pitchChaos;
    c.sourceSelect = sourceSelect;
    c.morph = morph;
    c.nowPos = spec.writePos();
    const double len = juce::jlimit (0.01 * sampleRate, 2.0 * sampleRate, spb * (0.25 + p1 * 1.5));
    const i64 minAbs = c.nowPos - spec.capacitySamples() + 8;
    double srcPos = (double) c.nowPos - (0.5 + rng.nextFloat() * 2.0) * spb;
    srcPos = juce::jlimit ((double) minAbs, (double) c.nowPos - len - 4.0, srcPos);
    c.sourcePos = srcPos;
    c.lengthSamples = len;
    c.startOffset = 0;
    return c;
}

// ===========================================================================
//  ModMatrix
// ===========================================================================
void ModMatrix::prepare (double sr)
{
    sampleRate = sr;
    reset();
}
void ModMatrix::reset()
{
    lfoPhase[0] = lfoPhase[1] = 0.f;
    lfoVal[0] = lfoVal[1] = 0.f;
    sh[0] = sh[1] = 0.f;
    shPhase[0] = shPhase[1] = 1.f;
    env = 0.f;
    walk = 0.f;
    walkVelocity = 0.f;
    walkAccumulator = 0.0;
}
void ModMatrix::process (int n, float inputRms) noexcept
{
    for (int L = 0; L < 2; ++L)
    {
        const float inc = lfoRate[L] / (float) sampleRate;
        float ph = lfoPhase[L] + inc * n;
        ph -= std::floor (ph);
        lfoPhase[L] = ph;
        float v = 0.f;
        switch (lfoShape[L])
        {
            case 0:  v = std::sin (6.2831853f * ph);        break;
            case 1:  v = 4.f * std::abs (ph - 0.5f) - 1.f;  break;
            case 2:  v = 2.f * ph - 1.f;                    break;
            case 3:  v = ph < 0.5f ? 1.f : -1.f;            break;
            default:
                shPhase[L] += inc * n;
                if (shPhase[L] >= 1.f) { shPhase[L] -= std::floor (shPhase[L]); sh[L] = rng.nextFloat() * 2.f - 1.f; }
                v = sh[L];
                break;
        }
        lfoVal[L] = v;
    }
    const float target = juce::jlimit (0.f, 1.f, inputRms * 4.f);
    env += (target - env) * juce::jlimit (0.f, 1.f, 8.f * n / (float) sampleRate);

    // Advance the random walk on a fixed 50 Hz internal clock, not once per
    // host block. That makes the modulation character consistent at 32, 64,
    // 512 or 2048 sample buffers.
    walkAccumulator += (double) n / juce::jmax (1.0, sampleRate);
    constexpr double walkTick = 1.0 / 50.0;
    while (walkAccumulator >= walkTick)
    {
        walkAccumulator -= walkTick;
        const float impulse = (rng.nextFloat() * 2.f - 1.f) * 0.075f;
        walkVelocity = juce::jlimit (-0.18f, 0.18f, walkVelocity * 0.82f + impulse);
        walk = juce::jlimit (-1.f, 1.f, walk + walkVelocity);

        // Reflect velocity at the walls rather than pinning there; this avoids
        // long flat shelves at +/-1 while keeping the source strictly bounded.
        if ((walk >= 1.f && walkVelocity > 0.f) || (walk <= -1.f && walkVelocity < 0.f))
            walkVelocity *= -0.65f;
    }
}
float ModMatrix::sourceValue (int src) const noexcept
{
    switch (src)
    {
        case 1:  return lfoVal[0];
        case 2:  return lfoVal[1];
        case 3:  return env;
        case 4:  return macro[0];
        case 5:  return macro[1];
        case 6:  return walk;
        default: return 0.f;
    }
}
float ModMatrix::destOffset (int destEnum) const noexcept
{
    float sum = 0.f;
    for (const auto& s : slot)
        if (s.src != 0 && s.dst == destEnum)
            sum += sourceValue (s.src) * s.depth;
    return sum;
}

} // namespace vsx
