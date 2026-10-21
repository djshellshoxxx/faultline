#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// ============================================================================
//  VIVISECT - F3 capture engine (header only).
//
//  Resamples the final post-FX output into a 24-bit WAV.
//
//    * The audio thread only ever copies samples into a buffer that was
//      allocated in prepare() (64 s of stereo, enough for 8 bars at 40 BPM).
//      It takes no lock, allocates nothing and touches no file.
//    * A background thread turns a finished take into
//      <dir>/Vivisect_<yyyyMMdd-HHmmss>_<bpm>bpm_<bars>bar.wav. The file is
//      written under a ".part" name and renamed when complete, so a cancelled
//      or failed capture never leaves a half-written .wav behind.
//    * Phase moves through idle -> armed -> recording -> writing -> idle. Every
//      transition is a compare-exchange, so a cancel from the message thread
//      and the audio thread finishing a take at the same instant cannot both
//      win.
// ============================================================================
namespace vsx
{
enum class CapturePhase { idle = 0, armed, recording, writing };

struct CaptureInfo
{
    CapturePhase phase = CapturePhase::idle;
    int    bars = 0;                    // length of the take in progress
    juce::int64 framesWritten = 0;      // recorded so far
    juce::int64 framesTarget  = 0;      // length of the take in progress
    double progress = 0.0;              // 0..1 through the recording
    double seconds  = 0.0;              // length of the take in progress, seconds
    double bpm      = 0.0;              // tempo the take was started at
    int    completedCount = 0;          // takes finished since the engine was created
    int    failedCount    = 0;
    juce::File lastFile;                // most recent finished take
    double lastSeconds = 0.0;
    int    lastBars = 0;
    bool   lastOk = false;
    juce::String lastError;
};

namespace capture
{
    constexpr double kRingSeconds = 64.0;

    inline bool isValidBars (int bars) noexcept
    {
        return bars == 1 || bars == 2 || bars == 4 || bars == 8;
    }

    // Length of one bar in quarter notes (the unit of the host's ppq position).
    inline double barQuarters (int numerator, int denominator) noexcept
    {
        if (numerator < 1 || denominator < 1) return 4.0;
        return (double) numerator * 4.0 / (double) denominator;
    }

    // Exact length of a take in sample frames.
    inline juce::int64 framesForBars (int bars, double bpm, double sampleRate, double barQ = 4.0) noexcept
    {
        bpm = juce::jlimit (20.0, 300.0, bpm);
        return (juce::int64) std::llround ((double) bars * barQ * 60.0 / bpm * sampleRate);
    }

    // Bar alignment. `ppq` is the host position (quarter notes) of the first
    // sample of a block of `numSamples`. Returns the sample offset inside the
    // block at which the next bar line falls, or -1 when none does. A block
    // that starts exactly on a bar line returns 0.
    inline int barStartOffset (double ppq, double bpm, double sampleRate, int numSamples,
                               double barQ = 4.0) noexcept
    {
        if (numSamples <= 0 || barQ <= 0.0) return -1;
        bpm = juce::jlimit (20.0, 300.0, bpm);
        const double samplesPerQuarter = 60.0 / bpm * sampleRate;
        const double blockQuarters = (double) numSamples / samplesPerQuarter;

        // The small tolerance makes a block that starts a hair before a bar
        // line (float noise in the host's ppq) count as starting on it.
        const double next = std::ceil (ppq / barQ - 1.0e-7) * barQ;
        const double dq = juce::jmax (0.0, next - ppq);
        if (dq >= blockQuarters) return -1;

        const auto off = (juce::int64) std::llround (dq * samplesPerQuarter);
        return off >= numSamples ? -1 : (int) off;
    }
}

// ----------------------------------------------------------------------------
class CaptureEngine : private juce::Thread
{
public:
    struct Clock
    {
        bool   playing = false;
        double ppq = 0.0;           // quarter notes at the start of the block
        double bpm = 120.0;
        double barQuarters = 4.0;
    };

    CaptureEngine() : juce::Thread ("Vivisect capture writer") {}
    ~CaptureEngine() override { shutdown(); }

    // -- message thread -------------------------------------------------------
    // Allocates the take buffer. Any capture in progress is cancelled (the
    // sample rate may have changed under it).
    void prepare (double newSampleRate)
    {
        cancel();
        waitUntilIdle (5000);
        sampleRate = juce::jmax (8000.0, newSampleRate);
        const auto frames = (size_t) std::ceil (capture::kRingSeconds * sampleRate);
        if (buf[0].size() != frames)
            for (auto& b : buf) { b.assign (frames, 0.f); b.shrink_to_fit(); }
        ready.store (true);
    }

    void setDirectory (const juce::File& d) { const juce::ScopedLock sl (lock); dir = d; }
    juce::File directory() const            { const juce::ScopedLock sl (lock); return dir; }

    // Arms a capture of `bars` bars. False if the length is not 1/2/4/8, the
    // engine is not prepared, or a capture is already in progress.
    bool start (int bars)
    {
        if (! capture::isValidBars (bars) || ! ready.load()) return false;
        if (phase.load() != (int) CapturePhase::idle) return false;

        abortWrite.store (false);
        reqBars.store (bars);
        written.store (0);
        target.store (0);
        if (! isThreadRunning())
            startThread (juce::Thread::Priority::background);

        int expected = (int) CapturePhase::idle;
        return phase.compare_exchange_strong (expected, (int) CapturePhase::armed);
    }

    void cancel()
    {
        int p = phase.load();
        if (p == (int) CapturePhase::armed || p == (int) CapturePhase::recording)
        {
            if (phase.compare_exchange_strong (p, (int) CapturePhase::idle))
                return;
        }
        if (phase.load() == (int) CapturePhase::writing)
        {
            abortWrite.store (true);
            notify();
        }
    }

    CaptureInfo info() const
    {
        CaptureInfo i;
        i.phase = (CapturePhase) phase.load();
        i.bars = reqBars.load();
        i.framesWritten = written.load();
        i.framesTarget = target.load();
        i.bpm = startBpm.load();
        i.seconds = sampleRate > 0.0 ? (double) i.framesTarget / sampleRate : 0.0;
        i.progress = i.framesTarget > 0
                         ? juce::jlimit (0.0, 1.0, (double) i.framesWritten / (double) i.framesTarget) : 0.0;
        i.completedCount = completed.load();
        i.failedCount = failed.load();
        const juce::ScopedLock sl (lock);
        i.lastFile = lastFile;
        i.lastSeconds = lastSeconds;
        i.lastBars = lastBars;
        i.lastOk = lastOk;
        i.lastError = lastError;
        return i;
    }

    // Newest first. maxCount <= 0 lists everything.
    juce::Array<juce::File> list (int maxCount = 0) const
    {
        juce::Array<juce::File> files;
        const auto d = directory();
        if (d.isDirectory())
            files = d.findChildFiles (juce::File::findFiles, false, "*.wav");

        struct Newest
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
                if (ta != tb) return ta > tb ? -1 : 1;
                return b.getFileName().compare (a.getFileName());
            }
        };
        Newest cmp;
        files.sort (cmp);
        if (maxCount > 0 && files.size() > maxCount)
            files.removeRange (maxCount, files.size() - maxCount);
        return files;
    }

    // Blocks until the writer has gone idle (or the timeout passes).
    bool waitUntilIdle (int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (phase.load() == (int) CapturePhase::writing)
        {
            if (juce::Time::getMillisecondCounter() > end) return false;
            juce::Thread::sleep (2);
        }
        return true;
    }

    // Joins the writer thread. Safe to call twice; the editor never owns it,
    // so closing the UI mid-capture does not touch the thread at all.
    void shutdown()
    {
        abortWrite.store (true);
        signalThreadShouldExit();
        notify();
        stopThread (5000);
    }

    // For the capacity assertions: constant after prepare().
    size_t capacityFrames() const noexcept { return buf[0].size(); }
    const float* bufferData (int ch) const noexcept { return buf[(size_t) (ch & 1)].data(); }
    double preparedRate() const noexcept { return sampleRate; }

    // -- audio thread ---------------------------------------------------------
    void process (const float* l, const float* r, int n, const Clock& clk) noexcept
    {
        const int ph = phase.load (std::memory_order_acquire);
        if (ph != (int) CapturePhase::armed && ph != (int) CapturePhase::recording)
            return;
        if (n <= 0 || l == nullptr) return;
        if (r == nullptr) r = l;

        int off = 0;
        if (ph == (int) CapturePhase::armed)
        {
            // Playing: wait for the next bar line. Stopped or standalone: go now.
            if (clk.playing)
            {
                off = capture::barStartOffset (clk.ppq, clk.bpm, sampleRate, n, clk.barQuarters);
                if (off < 0) return;
            }
            const auto cap = (juce::int64) buf[0].size();
            auto frames = capture::framesForBars (reqBars.load(), clk.bpm, sampleRate, clk.barQuarters);
            frames = juce::jlimit ((juce::int64) 1, cap, frames);
            target.store (frames);
            written.store (0);
            startBpm.store (juce::jlimit (20.0, 300.0, clk.bpm));

            int expected = (int) CapturePhase::armed;
            if (! phase.compare_exchange_strong (expected, (int) CapturePhase::recording))
                return;                                    // cancelled meanwhile
        }

        const auto w = written.load();
        const auto t = target.load();
        const auto todo = (int) juce::jmax ((juce::int64) 0, juce::jmin ((juce::int64) (n - off), t - w));
        if (todo > 0)
        {
            std::memcpy (buf[0].data() + w, l + off, sizeof (float) * (size_t) todo);
            std::memcpy (buf[1].data() + w, r + off, sizeof (float) * (size_t) todo);
            written.store (w + todo);
        }
        if (w + todo >= t)
        {
            int expected = (int) CapturePhase::recording;
            phase.compare_exchange_strong (expected, (int) CapturePhase::writing,
                                           std::memory_order_release);
        }
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            if (phase.load (std::memory_order_acquire) == (int) CapturePhase::writing)
                writeTake();
            wait (20);
        }
    }

    void finish (bool ok, const juce::File& f, const juce::String& err, juce::int64 frames, int bars)
    {
        {
            const juce::ScopedLock sl (lock);
            lastOk = ok;
            lastFile = ok ? f : juce::File();
            lastError = err;
            lastBars = bars;
            lastSeconds = sampleRate > 0.0 ? (double) frames / sampleRate : 0.0;
        }
        if (ok) completed.fetch_add (1); else failed.fetch_add (1);
        phase.store ((int) CapturePhase::idle, std::memory_order_release);
    }

    void writeTake()
    {
        const auto frames = target.load();
        const int bars = reqBars.load();
        const int bpm = juce::roundToInt (startBpm.load());
        const auto d = directory();

        if (abortWrite.load())
        {
            phase.store ((int) CapturePhase::idle, std::memory_order_release);
            return;
        }
        d.createDirectory();
        if (! d.isDirectory())
            return finish (false, {}, "could not create " + d.getFullPathName(), frames, bars);

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        const auto base = "Vivisect_" + stamp + "_" + juce::String (bpm) + "bpm_" + juce::String (bars) + "bar";
        auto finalFile = d.getChildFile (base + ".wav");
        for (int n = 2; finalFile.exists() && n < 1000; ++n)
            finalFile = d.getChildFile (base + "_" + juce::String (n) + ".wav");
        const auto part = finalFile.withFileExtension ("part");

        part.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (part.createOutputStream());
        if (os == nullptr || ! os->openedOk())
            return finish (false, {}, "could not open " + part.getFullPathName(), frames, bars);

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sampleRate, 2, 24, {}, 0));
        if (w == nullptr)
        {
            os.reset();
            part.deleteFile();
            return finish (false, {}, "could not create the WAV writer", frames, bars);
        }
        os.release();                                       // the writer owns the stream now

        constexpr int kChunk = 8192;
        std::vector<float> scratchL ((size_t) kChunk), scratchR ((size_t) kChunk);
        bool ok = true;
        for (juce::int64 pos = 0; pos < frames && ok; pos += kChunk)
        {
            if (abortWrite.load()) { ok = false; break; }
            const int n = (int) juce::jmin ((juce::int64) kChunk, frames - pos);
            for (int i = 0; i < n; ++i)
            {
                const float a = buf[0][(size_t) (pos + i)], b = buf[1][(size_t) (pos + i)];
                scratchL[(size_t) i] = std::isfinite (a) ? a : 0.f;
                scratchR[(size_t) i] = std::isfinite (b) ? b : 0.f;
            }
            const float* chans[2] { scratchL.data(), scratchR.data() };
            ok = w->writeFromFloatArrays (chans, 2, n);
        }
        const bool aborted = abortWrite.load();
        w.reset();                                          // flush + close

        if (! ok || aborted)
        {
            part.deleteFile();
            if (aborted)
            {
                phase.store ((int) CapturePhase::idle, std::memory_order_release);
                return;
            }
            return finish (false, {}, "write failed (disk full or read-only?)", frames, bars);
        }
        if (! part.moveFileTo (finalFile))
        {
            part.deleteFile();
            return finish (false, {}, "could not finalise " + finalFile.getFullPathName(), frames, bars);
        }
        finish (true, finalFile, {}, frames, bars);
    }

    std::vector<float> buf[2];
    double sampleRate = 48000.0;
    std::atomic<bool> ready { false };

    std::atomic<int> phase { (int) CapturePhase::idle };
    std::atomic<int> reqBars { 0 };
    std::atomic<juce::int64> written { 0 }, target { 0 };
    std::atomic<double> startBpm { 120.0 };
    std::atomic<bool> abortWrite { false };
    std::atomic<int> completed { 0 }, failed { 0 };

    mutable juce::CriticalSection lock;     // guards dir and the last-result fields; never taken on the audio thread
    juce::File dir, lastFile;
    double lastSeconds = 0.0;
    int lastBars = 0;
    bool lastOk = false;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureEngine)
};
} // namespace vsx
