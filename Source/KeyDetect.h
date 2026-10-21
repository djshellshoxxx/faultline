#pragma once
#include <algorithm>
#include <cmath>

// ============================================================================
//  VIVISECT - Key detection (spec H2)
//
//  Chromagram by Goertzel filters on the semitone grid (C2..B6) over a Hann
//  windowed mono block, then Krumhansl-Schmuckler profile matching across the
//  24 major / minor keys. Pure, allocation-free, run on the message thread.
//  `root` is a pitch class with C = 0, the same numbering as Scale Root.
// ============================================================================
namespace vsx
{
struct KeyResult
{
    bool  valid      = false;
    int   root       = 0;        // 0..11, C = 0
    bool  minor      = false;
    float confidence = 0.f;      // 0..1, best profile correlation
};

namespace keydetect
{
static constexpr int kLowestMidi  = 36;   // C2
static constexpr int kHighestMidi = 95;   // B6

// Hann-windowed Goertzel energy summed per pitch class. Returns false when the
// block is too short, silent, or not finite.
inline bool chromagram (const float* x, int n, double sampleRate, float* chroma12) noexcept
{
    for (int i = 0; i < 12; ++i) chroma12[i] = 0.f;
    if (x == nullptr || n < 2048 || sampleRate < 8000.0) return false;

    const double twoPi = 6.283185307179586;
    double total = 0.0;
    for (int midi = kLowestMidi; midi <= kHighestMidi; ++midi)
    {
        const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
        if (f >= 0.45 * sampleRate) break;
        const double coeff = 2.0 * std::cos (twoPi * f / sampleRate);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (twoPi * i / (n - 1));
            const double s = (double) x[i] * w + coeff * s1 - s2;
            s2 = s1; s1 = s;
        }
        const double power = std::max (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2);
        const double mag = std::sqrt (power) / n;                 // amplitude-like
        chroma12[midi % 12] += (float) mag;
        total += mag;
    }
    return std::isfinite (total) && total > 1.0e-6;
}

inline KeyResult detectKey (const float* chroma12) noexcept
{
    static constexpr float major[12] { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr float minor[12] { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    KeyResult best;
    float mean = 0.f;
    for (int i = 0; i < 12; ++i)
    {
        if (! std::isfinite (chroma12[i])) return best;
        mean += chroma12[i];
    }
    mean /= 12.f;
    float varC = 0.f;
    for (int i = 0; i < 12; ++i) varC += (chroma12[i] - mean) * (chroma12[i] - mean);
    if (varC <= 1.0e-12f) return best;

    float bestR = -2.f;
    for (int isMinor = 0; isMinor < 2; ++isMinor)
    {
        const float* prof = isMinor ? minor : major;
        float pm = 0.f;
        for (int i = 0; i < 12; ++i) pm += prof[i];
        pm /= 12.f;
        float varP = 0.f;
        for (int i = 0; i < 12; ++i) varP += (prof[i] - pm) * (prof[i] - pm);

        for (int root = 0; root < 12; ++root)
        {
            float cov = 0.f;
            for (int i = 0; i < 12; ++i)
                cov += (chroma12[(i + root) % 12] - mean) * (prof[i] - pm);
            const float r = cov / std::sqrt (varC * varP);
            if (r > bestR) { bestR = r; best.root = root; best.minor = isMinor != 0; }
        }
    }
    best.confidence = std::min (std::max (bestR, 0.f), 1.f);
    best.valid = true;
    return best;
}

// Convenience: whole pipeline on a mono block.
inline KeyResult analyse (const float* x, int n, double sampleRate) noexcept
{
    float chroma[12];
    if (! chromagram (x, n, sampleRate, chroma)) return {};
    return detectKey (chroma);
}
} // namespace keydetect
} // namespace vsx
