#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "Parameters.h"

// ============================================================================
//  VIVISECT DSP CORE
//  specimen buffer -> analysis -> scheduler -> 6 surgeons -> reinject -> mix
// ============================================================================

namespace vsx
{
using i64 = juce::int64;

// ---------------------------------------------------------------------------
//  Snapshot handed to the UI each frame (built on the message thread)
// ---------------------------------------------------------------------------
struct MonitorSnapshot
{
    static constexpr int COLS = 512;
    float envMax[COLS] {};
    float envMin[COLS] {};
    float bright[COLS] {};
    int   transientCol[96] {};
    int   numTransients = 0;
    struct Grab { int c0 = 0, c1 = 0, surgeon = 0; float age = 9.f; };
    Grab  grabs[kNumSurgeons] {};
    int   numGrabs = 0;
    float chaos = 0.f;
    float pulse = 0.f;
    float surgAct[kNumSurgeons] {};
    bool  frozen = false;
};

// ---------------------------------------------------------------------------
//  SCAR — post-rack master texture. A bounded soft clipper with a small
//  high-frequency "edge" contribution from the sample-to-sample delta.
//  It is deliberately simple, stable and cheap enough to leave automated.
// ---------------------------------------------------------------------------
class Scar
{
public:
    void reset() noexcept { previous.fill (0.f); }

    void process (juce::AudioBuffer<float>& buf, int n, float drive, float mix) noexcept
    {
        mix = juce::jlimit (0.f, 1.f, mix);
        drive = juce::jlimit (0.f, 1.f, drive);
        if (mix <= 1.0e-4f || n <= 0)
            return;

        const float gain = 1.f + drive * 15.f;
        const float edgeAmount = drive * 0.45f;
        const int channels = juce::jmin (2, buf.getNumChannels());

        for (int ch = 0; ch < channels; ++ch)
        {
            float* io = buf.getWritePointer (ch);
            float prev = previous[(size_t) ch];

            for (int i = 0; i < n; ++i)
            {
                const float dry = io[i];
                const float edge = dry - prev;
                prev = dry;

                // tanh keeps the processed branch strictly bounded. The edge
                // term gives transients a torn, papery attack instead of a
                // generic static distortion curve.
                const float shaped = std::tanh (dry * gain + edge * edgeAmount * gain);
                io[i] = dry * (1.f - mix) + shaped * mix;
            }

            previous[(size_t) ch] = prev;
        }
    }

private:
    std::array<float, 2> previous { 0.f, 0.f };
};

// ---------------------------------------------------------------------------
//  FLATLINE — the hidden effect. A tuned feedback comb: the note a monitor
//  makes when the specimen stops. Feedback is clamped below unity and the
//  loop is damped, so it rings rather than runs away.
// ---------------------------------------------------------------------------
class Flatline
{
public:
    void prepare (double sr, int /*maxBlock*/)
    {
        sampleRate = sr;
        const int maxDelay = (int) (sr / 40.0) + 4;      // down to 40 Hz
        for (auto& line : lines)
        {
            line.assign ((size_t) juce::jmax (64, maxDelay), 0.f);
            std::fill (line.begin(), line.end(), 0.f);
        }
        writeIdx = 0;
        for (auto& d : damp) d = 0.f;
        smoothedDelay = 0.f;
    }

    void reset()
    {
        for (auto& line : lines) std::fill (line.begin(), line.end(), 0.f);
        for (auto& d : damp) d = 0.f;
    }

    // tone in Hz, bleed 0..1 (resonance), mix 0..1
    void process (juce::AudioBuffer<float>& buf, int n, float tone, float bleed, float mix) noexcept
    {
        if (lines[0].empty() || mix <= 1.0e-4f)
            return;

        const float target = (float) (sampleRate / juce::jlimit (40.0, 4000.0, (double) tone));
        // Feedback stops short of unity on purpose: at 1.0 a comb is an
        // oscillator, and a secret effect should not be able to blow up a mix.
        const float fb = juce::jlimit (0.f, 0.965f, 0.55f + bleed * 0.415f);
        const float damping = 0.18f + (1.f - bleed) * 0.42f;

        const int cap = (int) lines[0].size();

        for (int i = 0; i < n; ++i)
        {
            // Glide the delay length so tone sweeps do not click.
            smoothedDelay = smoothedDelay <= 0.f
                              ? target
                              : smoothedDelay + (target - smoothedDelay) * 0.0008f;
            const float dly = juce::jlimit (2.f, (float) (cap - 2), smoothedDelay);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto& line = lines[(size_t) ch];
                float* io = buf.getWritePointer (ch);

                // fractional read
                float readPos = (float) writeIdx - dly;
                while (readPos < 0.f) readPos += (float) cap;
                const int i0 = (int) readPos;
                const int i1 = (i0 + 1) % cap;
                const float fr = readPos - (float) i0;
                const float delayed = line[(size_t) i0] + (line[(size_t) i1] - line[(size_t) i0]) * fr;

                // one-pole damping in the loop keeps it from turning to fizz
                damp[(size_t) ch] = delayed * (1.f - damping) + damp[(size_t) ch] * damping;

                const float in = io[i];
                line[(size_t) writeIdx] = std::tanh (in + damp[(size_t) ch] * fb);
                io[i] = in * (1.f - mix) + delayed * mix;
            }

            writeIdx = (writeIdx + 1) % cap;
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> lines;
    std::array<float, 2> damp { 0.f, 0.f };
    int writeIdx = 0;
    float smoothedDelay = 0.f;
};

// ---------------------------------------------------------------------------
//  Telemetry — the raw internal traffic the on-screen data stream renders.
//
//  Producers are the audio thread and the message thread; the only consumer is
//  the editor. Events are plain PODs and are NOT formatted at the push site:
//  turning numbers into text allocates, and the audio thread must not. The UI
//  drains the ring and does the formatting on its own time.
//
//  A single-producer assumption would be wrong here (audio thread + message
//  thread both push), so the write cursor is a fetch_add and each slot is
//  published with a release store of its sequence number. A reader that sees a
//  torn or lapped slot drops it — this is a decorative read-out, and blocking
//  the audio thread to guarantee delivery of a scrolling background effect
//  would be a bad trade.
// ---------------------------------------------------------------------------
struct TelemetryEvent
{
    enum class Kind : juce::uint8
    {
        none = 0,
        midiNote,     // a: note, b: velocity, c: surgeon index
        midiCC,       // a: cc, b: value, c: mapped param index (255 = unmapped)
        surgeonFire,  // a: surgeon, x: slice start (0..1), y: length in ms
        sliceChoice,  // a: surgeon, b: pick mode, x: score
        paramChange,  // a: param index, x: new normalised value
        clock,        // x: bpm, y: ppq
        level,        // x: peak L, y: peak R
        buffer,       // a: bars, b: frozen, x: fill (0..1)
        preset        // a: index
    };

    Kind kind = Kind::none;
    juce::uint8 a = 0, b = 0, c = 0;
    float x = 0.f, y = 0.f;
};

class TelemetryRing
{
public:
    static constexpr int capacity = 512;          // power of two
    static constexpr int mask = capacity - 1;

    // Safe from any thread, allocation-free, never blocks.
    void push (const TelemetryEvent& e) noexcept
    {
        const auto seq = writeSeq.fetch_add (1, std::memory_order_relaxed);
        auto& slot = slots[(size_t) (seq & mask)];
        slot.stamp.store (0, std::memory_order_relaxed);   // mark in-flight
        slot.ev = e;
        slot.stamp.store (seq + 1, std::memory_order_release);
    }

    // Convenience pushes, so call sites stay readable.
    void pushNote (int note, int vel, int surgeon) noexcept
    {
        push ({ TelemetryEvent::Kind::midiNote, (juce::uint8) note, (juce::uint8) vel,
                (juce::uint8) surgeon, 0.f, 0.f });
    }
    void pushCC (int cc, int val, int paramIdx) noexcept
    {
        push ({ TelemetryEvent::Kind::midiCC, (juce::uint8) cc, (juce::uint8) val,
                (juce::uint8) (paramIdx < 0 ? 255 : juce::jmin (254, paramIdx)), 0.f, 0.f });
    }
    void pushFire (int surgeon, float pos01, float lenMs) noexcept
    {
        push ({ TelemetryEvent::Kind::surgeonFire, (juce::uint8) surgeon, 0, 0, pos01, lenMs });
    }
    void pushSimple (TelemetryEvent::Kind k, float x, float y = 0.f,
                     int a = 0, int b = 0) noexcept
    {
        push ({ k, (juce::uint8) a, (juce::uint8) b, 0, x, y });
    }

    // UI thread only. Returns how many events were written into dest.
    int drain (TelemetryEvent* dest, int maxEvents) noexcept
    {
        const auto end = writeSeq.load (std::memory_order_acquire);

        // If we fell more than a ring behind, skip forward rather than emit
        // stale traffic — the stream should always show what is happening now.
        if (end - readSeq > (juce::uint64) capacity)
            readSeq = end - (juce::uint64) capacity;

        int count = 0;
        while (readSeq < end && count < maxEvents)
        {
            auto& slot = slots[(size_t) (readSeq & mask)];
            const auto stamp = slot.stamp.load (std::memory_order_acquire);
            if (stamp != readSeq + 1)      // still being written, or already lapped
            {
                ++readSeq;
                continue;
            }
            dest[count++] = slot.ev;
            ++readSeq;
        }
        return count;
    }

private:
    struct Slot
    {
        std::atomic<juce::uint64> stamp { 0 };
        TelemetryEvent ev;
    };
    std::array<Slot, (size_t) capacity> slots {};
    std::atomic<juce::uint64> writeSeq { 0 };
    juce::uint64 readSeq = 0;          // UI thread only
};

// ---------------------------------------------------------------------------
//  Rolling live "specimen" buffer + A/B sample slots
// ---------------------------------------------------------------------------
class SpecimenBuffer
{
public:
    void prepare (double sampleRate, double maxSeconds);
    void reset();

    void setFrozen (bool f) noexcept { frozen = f; }
    bool isFrozen() const noexcept   { return frozen; }

    void push (const juce::AudioBuffer<float>& in) noexcept;
    void reinject (const float* L, const float* R, int n, i64 startAbs, float amt) noexcept;

    float readInterp (int ch, double absPos) const noexcept;

    i64  writePos() const noexcept       { return totalWritten; }
    int  capacitySamples() const noexcept { return cap; }
    double getSampleRate() const noexcept { return sr; }

    void setSample (int slot, juce::AudioBuffer<float>&& buf, double srcRate);
    void clearSample (int slot);
    bool hasSample (int slot) const noexcept
        { return slot >= 0 && slot < 2 && abBuf[slot] && abBuf[slot]->getNumSamples() > 1; }
    float readAB (int slot, int ch, double absPos) const noexcept;

    const juce::AudioBuffer<float>& raw() const noexcept { return buffer; }

private:
    int wrap (i64 p) const noexcept
    {
        int m = (int) (p % (i64) cap);
        return m < 0 ? m + cap : m;
    }
    juce::AudioBuffer<float> buffer;
    int cap = 0;
    double sr = 44100.0;
    i64 totalWritten = 0;
    std::atomic<bool> frozen { false };
    std::unique_ptr<juce::AudioBuffer<float>> abBuf[2];
    double abRate[2] { 44100.0, 44100.0 };
};

// ---------------------------------------------------------------------------
//  Analysis : transient onsets + spectral frames (centroid / flatness / rms)
// ---------------------------------------------------------------------------
class TransientDetector
{
public:
    void prepare (double sr);
    void reset();
    void process (const juce::AudioBuffer<float>& in, i64 blockStartAbs) noexcept;
    i64  nearest (i64 target, i64 minAbs) const noexcept;
    void copyRecent (std::vector<i64>& out, i64 sinceAbs) const;

private:
    struct Ev { i64 pos = 0; float str = 0.f; };
    static constexpr int kMax = 512;
    std::array<Ev, kMax> ev {};
    int head = 0;
    double sampleRate = 44100.0;
    float prevX = 0.f, fast = 0.f, slow = 0.f;
    int sinceLast = 0, refractory = 1000;
};

class SpectralAnalyzer
{
public:
    void prepare (double sr);
    void reset();
    void process (const SpecimenBuffer& spec, int numSamples) noexcept;

    i64  brightest (i64 a, i64 b) const noexcept;
    i64  loudest   (i64 a, i64 b) const noexcept;
    i64  mostTonal (i64 a, i64 b) const noexcept;
    float latestCentroid01() const noexcept { return latestC01.load(); }
    float centroidAt01 (i64 pos) const noexcept;
    // O(frames + n) : bucket centroid-brightness across [startAbs, endAbs) into out[n]
    void  fillBrightness (float* out, int n, i64 startAbs, i64 endAbs) const noexcept;

private:
    struct Frame { i64 pos = 0; float centroid = 0.f, flat = 1.f, rms = 0.f; };
    static constexpr int kOrder = 10;
    static constexpr int kSize  = 1 << kOrder;      // 1024
    static constexpr int kHop   = 512;
    static constexpr int kMaxFrames = 8192;

    double sampleRate = 44100.0;
    juce::dsp::FFT fft { kOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftBuf, timeBuf;
    int hopCounter = 0;
    std::vector<Frame> frames;
    int frameHead = 0, frameCount = 0;
    std::atomic<float> latestC01 { 0.f };
};

struct SpecimenAnalysis
{
    TransientDetector transients;
    SpectralAnalyzer  spectral;
    void prepare (double sr) { transients.prepare (sr); spectral.prepare (sr); }
    void reset()             { transients.reset();      spectral.reset(); }
};

// ---------------------------------------------------------------------------
//  Chaos engine : one axis -> a bundle of stochastic behaviours
// ---------------------------------------------------------------------------
struct ChaosField
{
    float triggerJitter = 0.f;   // timing smear
    float lengthJitter  = 0.f;   // slice length variance
    float skipProb      = 0.f;   // dropped scheduled hits
    float wrongSlice    = 0.f;   // source position randomised
    float routeSwap     = 0.f;   // surgeon assignment swap
    float feedback      = 0.f;   // extra reinject
    float pitchChaos    = 0.f;   // extra pitch drift
    float gridBypass    = 0.f;   // ignore gravity snap
};
ChaosField deriveChaos (float c) noexcept;

// grid snap with swing + pull. samplesPerBeat > 0. gridIndex 4 == Free.
double snapToGrid (double posSamples, int gridIndex, double samplesPerBeat,
                   float pull, float swing) noexcept;
double gridStepSamples (int gridIndex, double samplesPerBeat) noexcept;

// ---------------------------------------------------------------------------
//  Surgeons
// ---------------------------------------------------------------------------
struct TriggerContext
{
    i64    nowPos          = 0;
    double sourcePos       = 0.0;
    double lengthSamples   = 2000.0;
    double samplesPerBeat  = 22050.0;
    float  chaos           = 0.f;
    float  pitchChaos      = 0.f;
    int    startOffset     = 0;
    int    sourceSelect    = 0;
    float  morph           = 0.5f;
    juce::Random*      rng      = nullptr;
    SpecimenBuffer*    specimen = nullptr;
    SpecimenAnalysis*  analysis = nullptr;
};

float readSource (const TriggerContext& c, int ch, double absPos) noexcept;
float readSourceS (const SpecimenBuffer& spec, int sourceSelect, float morph, int ch, double absPos) noexcept;

class Surgeon
{
public:
    virtual ~Surgeon() = default;
    virtual void prepare (double sr, int maxBlock) = 0;
    virtual void reset() = 0;
    virtual void setParams (float p1, float p2, float p3) { pp1 = p1; pp2 = p2; pp3 = p3; }
    virtual void trigger (const TriggerContext& ctx) = 0;
    virtual void process (juce::AudioBuffer<float>& add, int numSamples) = 0;

    float activity() const noexcept { return act; }
    i64   grabStart() const noexcept { return lastGrabStart.load(); }
    int   grabLen()   const noexcept { return lastGrabLen.load(); }

protected:
    void decayAct (int n) noexcept { act *= std::pow (0.9985f, (float) n); if (act < 1.0e-4f) act = 0.f; }

    double sr = 44100.0;
    float  pp1 = 0.35f, pp2 = 0.4f, pp3 = 0.3f;
    float  act = 0.f;
    std::atomic<i64> lastGrabStart { 0 };
    std::atomic<int> lastGrabLen   { 0 };
};

std::unique_ptr<Surgeon> makeSurgeon (int surgeonIndex);

// Build a maximally-even Euclidean pulse pattern (a rotation of Bjorklund's
// E(pulses, steps)). Exposed for the regression harness and Reorder.
int buildEuclideanPattern (int steps, int pulses, std::array<int, 64>& pattern) noexcept;

// ---------------------------------------------------------------------------
//  Rack : runs the 6 surgeons, sums, handles reinject / feedback routing
// ---------------------------------------------------------------------------
class SurgeonRack
{
public:
    void prepare (double sr, int maxBlock);
    void reset();

    Surgeon& operator[] (int i) { return *surg[(size_t) i]; }

    void process (SpecimenBuffer& spec, juce::AudioBuffer<float>& wetOut, int numSamples,
                  const std::array<float, kNumSurgeons>& mix,
                  const std::array<int,   kNumSurgeons>& routeIn,
                  float reinjectAmt) noexcept;

    float maxActivity() const noexcept { return maxAct; }

private:
    std::array<std::unique_ptr<Surgeon>, kNumSurgeons> surg;
    std::array<juce::AudioBuffer<float>, kNumSurgeons> outs;
    juce::AudioBuffer<float> scratch, feedBuf, reBuf;
    float maxAct = 0.f;
};

// ---------------------------------------------------------------------------
//  Scheduler : musical clock -> per-surgeon trigger decisions
// ---------------------------------------------------------------------------
struct MusicalContext
{
    double songPosSamples = 0.0;
    double samplesPerBeat = 22050.0;
    int    gridIndex = 0;
    float  pull = 0.8f, swing = 0.f;
    bool   playing = false;
};

class SliceScheduler
{
public:
    void prepare (double sr) { sampleRate = sr; }
    void reset() { lastGridIdx = std::numeric_limits<i64>::min(); }

    // callback: (surgeonIndex, TriggerContext&)
    template <typename Fn>
    void advance (int numSamples, const MusicalContext& mc,
                  const std::array<bool,  kNumSurgeons>& on,
                  const std::array<float, kNumSurgeons>& prob,
                  const std::array<float, kNumSurgeons>& p1,
                  const std::array<float, kNumSurgeons>& activity,
                  float triggerRate, float chaos, float analysisInform,
                  bool midiMode, int sourceSelect, float morph,
                  SpecimenBuffer& spec, SpecimenAnalysis& analysis, Fn&& fire)
    {
        if (midiMode) return;
        const ChaosField cf = deriveChaos (chaos);
        const double step = gridStepSamples (mc.gridIndex, mc.samplesPerBeat);
        if (step < 8.0) return;

        const double start = mc.songPosSamples;
        const double end   = start + numSamples;
        i64 firstIdx = (i64) std::floor (start / step) + 1;
        i64 lastIdx  = (i64) std::floor (end   / step);
        if (lastIdx - firstIdx > 8) firstIdx = lastIdx - 8;

        static const float base[kNumSurgeons] = { 1.0f, 0.5f, 0.5f, 0.42f, 0.32f, 0.24f };

        for (i64 gi = firstIdx; gi <= lastIdx; ++gi)
        {
            const double bAbs = gi * step;
            const int off = juce::jlimit (0, numSamples - 1, (int) std::llround (bAbs - start));

            for (int s = 0; s < kNumSurgeons; ++s)
            {
                const auto si = (size_t) s;
                if (! on[si]) continue;
                if (activity[si] > 0.5f && rng.nextFloat() > chaos) continue; // busy
                float pr = 0.55f * triggerRate * prob[si] * base[si];
                pr *= (1.f - cf.skipProb);
                if (rng.nextFloat() >= pr) continue;

                TriggerContext ctx;
                ctx.rng = &rng; ctx.specimen = &spec; ctx.analysis = &analysis;
                ctx.samplesPerBeat = mc.samplesPerBeat;
                ctx.chaos = chaos; ctx.pitchChaos = cf.pitchChaos;
                ctx.sourceSelect = sourceSelect; ctx.morph = morph;
                ctx.nowPos = spec.writePos();

                const i64 minAbs = std::max<i64> (0, ctx.nowPos - spec.capacitySamples() + 8);
                double srcPos = (double) ctx.nowPos - (1.0 + rng.nextFloat() * 3.0) * mc.samplesPerBeat;
                if (rng.nextFloat() >= cf.gridBypass)
                    srcPos = snapToGrid (srcPos, mc.gridIndex, mc.samplesPerBeat, mc.pull, mc.swing);

                if (sourceSelect == 0 && rng.nextFloat() < analysisInform)
                {
                    const i64 barBack = ctx.nowPos - (i64) (4.0 * mc.samplesPerBeat);
                    switch (rng.nextInt (4))
                    {
                        case 0: srcPos = (double) analysis.transients.nearest ((i64) srcPos, minAbs); break;
                        case 1: srcPos = (double) analysis.spectral.brightest (barBack, ctx.nowPos);  break;
                        case 2: srcPos = (double) analysis.spectral.loudest   (barBack, ctx.nowPos);  break;
                        default:srcPos = (double) analysis.spectral.mostTonal (barBack, ctx.nowPos);  break;
                    }
                }
                if (rng.nextFloat() < cf.wrongSlice)
                    srcPos = (double) ctx.nowPos - rng.nextFloat() * spec.capacitySamples() * 0.9;

                double len = step * (0.5 + p1[si] * 3.5);
                len *= 1.0 + (rng.nextFloat() * 2.f - 1.f) * cf.lengthJitter * 0.8;
                const double minLen = 0.002 * sampleRate;
                const double available = juce::jmax (minLen, (double) ctx.nowPos - (double) minAbs - 4.0);
                len = juce::jlimit (minLen, juce::jmin (2.0 * sampleRate, available), len);

                srcPos = juce::jlimit ((double) minAbs,
                                      juce::jmax ((double) minAbs, (double) ctx.nowPos - len - 4.0),
                                      srcPos);

                ctx.sourcePos = srcPos;
                ctx.lengthSamples = len;
                ctx.startOffset = juce::jlimit (0, numSamples - 1,
                                     off + (int) (rng.nextFloat() * cf.triggerJitter * step * 0.4));
                fire (s, ctx);
            }
        }
    }

    TriggerContext makeManualContext (float p1, int sourceSelect, float morph, float chaos,
                                      double samplesPerBeat, SpecimenBuffer& spec, SpecimenAnalysis& an);

    juce::Random rng { 0x5ec7107 };

private:
    double sampleRate = 44100.0;
    i64 lastGridIdx = std::numeric_limits<i64>::min();
};

// ---------------------------------------------------------------------------
//  Mod matrix : 2 LFOs + env follower + 2 macros + random walk -> 4 slots
// ---------------------------------------------------------------------------
class ModMatrix
{
public:
    void prepare (double sr);
    void reset();

    void setLFO (int i, float rateHz, int shape) { lfoRate[i] = rateHz; lfoShape[i] = shape; }
    void setMacro (int i, float v) { macro[i] = v; }
    void setSlot (int i, int src, int dst, float depth) { slot[i] = { src, dst, depth }; }

    void process (int numSamples, float inputRms) noexcept;
    float sourceValue (int src) const noexcept;
    float destOffset (int destEnum) const noexcept;   // sum of active slots

private:
    struct Slot { int src = 0, dst = 0; float depth = 0.f; };
    double sampleRate = 44100.0;
    float  lfoPhase[2] { 0.f, 0.f };
    float  lfoRate[2]  { 1.f, 0.25f };
    int    lfoShape[2] { 0, 1 };
    float  lfoVal[2]   { 0.f, 0.f };
    float  sh[2]       { 0.f, 0.f };
    float  shPhase[2]  { 1.f, 1.f };
    float  env = 0.f;
    float  macro[2]    { 0.f, 0.f };
    float  walk = 0.f;
    float  walkVelocity = 0.f;
    int    walkSamples = 0;
    int    walkTickSamples = 882;
    juce::Random rng { 0x1a2b3c };
    Slot   slot[kNumModSlots];
};

} // namespace vsx
