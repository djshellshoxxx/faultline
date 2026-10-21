#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
//  VIVISECT - Scale Lock (spec F2)
//
//  Pure, allocation-free helpers. A pitch offset in semitones is treated as an
//  absolute pitch class with C = 0: an offset `o` is "in scale" when
//  ((round (o) - root) mod 12) is one of the scale's intervals. Root rotates
//  the scale around the circle, so D Major allows +2, +4, +6 ... and so on.
// ============================================================================
namespace vsx
{
static constexpr int kNumScaleTypes = 12;
static constexpr int kNumScaleRoots = 12;

struct ScaleQuantizer
{
    // interval bitmasks relative to the root (bit n = n semitones above root)
    static constexpr unsigned maskFor (int type) noexcept
    {
        constexpr unsigned masks[kNumScaleTypes] {
            0xFFFu,                                                  // Chromatic
            (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<11),  // Major
            (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10),  // Minor
            (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10),  // Dorian
            (1u<<0)|(1u<<1)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10),  // Phrygian
            (1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<9)|(1u<<11),  // Lydian
            (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10),  // Mixolydian
            (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<11),  // Harmonic minor
            (1u<<0)|(1u<<2)|(1u<<4)|(1u<<7)|(1u<<9),                   // Major pentatonic
            (1u<<0)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<10),                  // Minor pentatonic
            (1u<<0)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<10),          // Blues
            (1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8)|(1u<<10)           // Whole tone
        };
        return masks[type < 0 ? 0 : (type >= kNumScaleTypes ? kNumScaleTypes - 1 : type)];
    }

    static constexpr int wrap12 (int v) noexcept { return ((v % 12) + 12) % 12; }

    static constexpr bool isInScale (int semis, int root, int type) noexcept
    {
        return (maskFor (type) >> wrap12 (semis - root)) & 1u;
    }

    // Nearest in-scale offset (in semitones, integer valued). Ties resolve upward.
    // Chromatic returns the rounded value (half up).
    static float quantize (float semis, int root, int type) noexcept
    {
        if (! std::isfinite (semis)) semis = 0.f;
        semis = std::min (std::max (semis, -96.f), 96.f);
        if (type <= 0) return std::floor (semis + 0.5f);

        const int base = (int) std::floor (semis);
        float best = 0.f, bestDist = 1.0e9f;
        // scan outward; in-scale notes are never more than 3 semitones apart
        for (int d = -6; d <= 7; ++d)
        {
            const int cand = base + d;
            if (! isInScale (cand, root, type)) continue;
            const float dist = std::abs (semis - (float) cand);
            // `<=` while scanning upward makes the higher candidate win ties
            if (dist <= bestDist) { bestDist = dist; best = (float) cand; }
        }
        return best;
    }

    static float ratioFromSemitones (float semis) noexcept
    {
        semis = std::min (std::max (semis, -24.f), 24.f);
        return std::exp2 (semis / 12.f);
    }

    // Pick a random scale degree within +-rangeSemis. `u` is uniform in [0,1).
    // Candidates are the in-scale integers in [-R, +R] with R = floor(range);
    // with none available the nearest in-scale offset to 0 is used.
    static float pickDegree (float u, float rangeSemis, int root, int type) noexcept
    {
        const int R = (int) std::floor (std::min (std::max (rangeSemis, 0.f), 12.f) + 1.0e-4f);
        std::array<int, 25> cands {};
        int n = 0;
        for (int o = -R; o <= R; ++o)
            if (isInScale (o, root, type)) cands[(size_t) n++] = o;
        if (n == 0) return quantize (0.f, root, type);
        u = std::min (std::max (u, 0.f), 0.99999994f);
        return (float) cands[(size_t) std::min (n - 1, (int) (u * (float) n))];
    }
};

// Snapshot of the four Scale Lock parameters, handed to the surgeons.
struct ScaleLock
{
    bool  on    = false;
    int   root  = 0;
    int   type  = 2;
    float range = 0.35f;          // 0..1 -> +-0..12 semitones

    // Range in semitones, widened by chaos but always capped at an octave.
    float effectiveRangeSemis (float chaosWiden) const noexcept
    {
        const float r = std::min (std::max (range, 0.f), 1.f) * 12.f;
        return std::min (12.f, r * (1.f + std::min (std::max (chaosWiden, 0.f), 1.f)));
    }
    float pickRatio (float u, float chaosWiden) const noexcept
    {
        return ScaleQuantizer::ratioFromSemitones (
            ScaleQuantizer::pickDegree (u, effectiveRangeSemis (chaosWiden), root, type));
    }
};
} // namespace vsx
