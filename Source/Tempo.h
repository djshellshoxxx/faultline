#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// ============================================================================
//  VIVISECT - Tap tempo (spec H1)
//
//  Pure and allocation-free. Feed it the time of each tap in seconds; it
//  returns the tempo from the MEDIAN of the recent tap intervals, so one
//  sloppy tap does not drag the result. A gap longer than kResetSeconds starts
//  a new count. The result is clamped to the plug-in's tempo range.
// ============================================================================
namespace vsx
{
static constexpr double kMinBpm = 20.0;
static constexpr double kMaxBpm = 300.0;

struct TapTempo
{
    static constexpr int    kMaxTaps      = 8;
    static constexpr double kResetSeconds = 2.0;

    void reset() noexcept { count = 0; }
    int  taps() const noexcept { return count; }

    // Returns the tempo in BPM, or 0 until two taps have been seen.
    double tap (double nowSeconds) noexcept
    {
        if (count > 0 && (nowSeconds - times[(size_t) (count - 1)] > kResetSeconds
                          || nowSeconds <= times[(size_t) (count - 1)]))
            count = 0;

        if (count == kMaxTaps)
        {
            std::copy (times.begin() + 1, times.end(), times.begin());
            --count;
        }
        times[(size_t) count++] = nowSeconds;
        if (count < 2) return 0.0;

        std::array<double, kMaxTaps - 1> iv {};
        const int n = count - 1;
        for (int i = 0; i < n; ++i) iv[(size_t) i] = times[(size_t) i + 1] - times[(size_t) i];
        std::sort (iv.begin(), iv.begin() + n);
        const double med = (n & 1) ? iv[(size_t) (n / 2)]
                                   : 0.5 * (iv[(size_t) (n / 2 - 1)] + iv[(size_t) (n / 2)]);
        return clampBpm (60.0 / med);
    }

    static double clampBpm (double bpm) noexcept
    {
        if (! std::isfinite (bpm)) return 120.0;
        return std::min (std::max (bpm, kMinBpm), kMaxBpm);
    }

private:
    std::array<double, kMaxTaps> times {};
    int count = 0;
};
} // namespace vsx
