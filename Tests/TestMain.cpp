// ============================================================================
//  VIVISECT — headless conformance / stability harness.
//
//  include.md asks for every feature and every combination of settings to be
//  exercised and for logic errors to be hunted down. Doing that by hand in a
//  DAW is not repeatable, so the checks live here and run from the build.
//
//  The bar every audio test holds the plugin to:
//    * output is always finite (no NaN, no Inf) under any setting;
//    * output never exceeds a sane ceiling, so no runaway feedback;
//    * silence in stays silence out once the tail has drained;
//    * nothing reads or writes out of bounds (asserts + sanitisers catch it).
//
//  Build:  cmake --build build --config Release --target VivisectTests
//  Run:    build/VivisectTests_artefacts/Release/VivisectTests.exe
// ============================================================================
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include <cmath>
#include <cstdio>

using namespace juce;

// ---------------------------------------------------------------------------
namespace
{
int  checksRun = 0;
int  checksFailed = 0;
String currentCase;

void beginCase (const String& name)
{
    currentCase = name;
    std::printf ("\n-- %s\n", name.toRawUTF8());
}

bool check (bool ok, const String& what)
{
    ++checksRun;
    if (! ok)
    {
        ++checksFailed;
        std::printf ("   FAIL  %s\n", what.toRawUTF8());
    }
    return ok;
}

// The ceiling is deliberately generous: the plugin is a distortion effect and
// is allowed to be loud, but it must never diverge.
constexpr float kCeiling = 24.0f;

// OUT TRIM exists to add up to +24 dB, so a fixed ceiling would flag the
// parameter for doing its job. Scale the bar by whatever trim is dialled in.
float ceilingFor (VivisectProcessor& p)
{
    float gain = 1.f;
    if (auto* par = p.apvts.getParameter (vsx::id::outputTrim))
        gain = Decibels::decibelsToGain (par->getNormalisableRange()
                                             .convertFrom0to1 (par->getValue()));
    return kCeiling * jmax (1.f, gain);
}

struct BufferVerdict
{
    bool  finite = true;
    float peak = 0.f;
};

BufferVerdict inspect (const AudioBuffer<float>& b)
{
    BufferVerdict v;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* d = b.getReadPointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float s = d[i];
            if (! std::isfinite (s)) { v.finite = false; return v; }
            v.peak = jmax (v.peak, std::abs (s));
        }
    }
    return v;
}

// A signal with transients, tone and noise — the analyser has something to
// find, so slice selection actually exercises its branches.
void fillTestSignal (AudioBuffer<float>& b, int64 samplePos, double sampleRate)
{
    Random rng ((int) (samplePos & 0x7fffffff) ^ 0x51ec1);
    const int n = b.getNumSamples();
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) (samplePos + i) / sampleRate;
            float s = 0.35f * (float) std::sin (MathConstants<double>::twoPi * 220.0 * t);
            s += 0.12f * (float) std::sin (MathConstants<double>::twoPi * 1330.0 * t);
            s += 0.05f * (rng.nextFloat() * 2.f - 1.f);
            // a click every 0.25s so the transient detector has work to do
            if (((samplePos + i) % (int64) (sampleRate * 0.25)) < 32)
                s += 0.8f;
            d[i] = s;
        }
    }
}

// Runs blocks through the processor and reports the worst it saw.
BufferVerdict runBlocks (VivisectProcessor& p, int numBlocks, int blockSize,
                         double sampleRate, bool withMidi = false, bool silentInput = false)
{
    AudioBuffer<float> buf (2, blockSize);
    MidiBuffer midi;
    BufferVerdict worst;
    int64 pos = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        if (silentInput) buf.clear();
        else             fillTestSignal (buf, pos, sampleRate);

        midi.clear();
        if (withMidi)
        {
            if (b % 3 == 0) midi.addEvent (MidiMessage::noteOn (1, 48 + (b % 6), (uint8) 100), 0);
            if (b % 5 == 0) midi.addEvent (MidiMessage::controllerEvent (1, 20 + (b % 4), b % 128), 1);
            if (b % 7 == 0) midi.addEvent (MidiMessage::noteOff (1, 48 + (b % 6)), blockSize / 2);
        }

        p.processBlock (buf, midi);

        const auto v = inspect (buf);
        if (! v.finite) { worst.finite = false; return worst; }
        worst.peak = jmax (worst.peak, v.peak);
        pos += blockSize;
    }
    return worst;
}

std::map<String, float> snapshotParams (VivisectProcessor& p)
{
    std::map<String, float> out;
    for (auto* par : p.getParameters())
        if (auto* wid = dynamic_cast<AudioProcessorParameterWithID*> (par))
            out[wid->paramID] = par->getValue();
    return out;
}
} // namespace

// ===========================================================================
int main()
{
    ScopedJuceInitialiser_GUI juceInit;
    std::printf ("VIVISECT test harness\n=====================\n");

    const double kRate = 48000.0;
    const int    kBlock = 512;

    // -----------------------------------------------------------------------
    beginCase ("Instantiation and defaults");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        check (p.getParameters().size() > 0, "processor exposes parameters");

        const auto defaults = snapshotParams (p);
        p.randomizeAll();
        p.resetAllToDefaults();
        const auto after = snapshotParams (p);

        bool same = true;
        String firstBad;
        for (const auto& kv : defaults)
        {
            const auto it = after.find (kv.first);
            if (it == after.end() || std::abs (it->second - kv.second) > 1.0e-4f)
            {
                same = false;
                if (firstBad.isEmpty())
                    firstBad = kv.first + " expected " + String (kv.second, 5)
                             + " got " + String (it == after.end() ? -1.f : it->second, 5);
            }
        }
        check (same, "RESET restores every parameter to its default (" + firstBad + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("Sample rate and block size matrix");
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        for (int block : { 16, 64, 128, 512, 2048 })
        {
            VivisectProcessor p;
            p.prepareToPlay (rate, block);
            // switch every surgeon on so nothing is skipped
            for (int s = 0; s < vsx::kNumSurgeons; ++s)
                if (auto* par = p.apvts.getParameter (vsx::sid (s, "on")))
                    par->setValueNotifyingHost (1.f);

            const auto v = runBlocks (p, 40, block, rate, true);
            check (v.finite, "finite output at " + String (rate, 0) + "Hz / " + String (block));
            check (v.peak < kCeiling, "bounded output at " + String (rate, 0) + "Hz / "
                                      + String (block) + " (peak " + String (v.peak, 2) + ")");
        }

    // -----------------------------------------------------------------------
    beginCase ("Every parameter, swept across its range");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto params = p.getParameters();

        for (auto* par : params)
        {
            auto* wid = dynamic_cast<AudioProcessorParameterWithID*> (par);
            const String pid = wid != nullptr ? wid->paramID : par->getName (24);
            const float restore = par->getValue();

            bool ok = true;
            float worstPeak = 0.f;
            for (float v : { 0.f, 0.25f, 0.5f, 0.75f, 1.f })
            {
                par->setValueNotifyingHost (v);
                const auto r = runBlocks (p, 12, kBlock, kRate, true);
                if (! r.finite) { ok = false; break; }
                worstPeak = jmax (worstPeak, r.peak);
            }
            check (ok, "finite output sweeping " + pid);
            const float bar = ceilingFor (p);
            check (worstPeak < bar, "bounded output sweeping " + pid
                                    + " (peak " + String (worstPeak, 2)
                                    + " vs ceiling " + String (bar, 1) + ")");
            par->setValueNotifyingHost (restore);
        }
    }

    // -----------------------------------------------------------------------
    //  The real combination hunt. Randomising hits combinations no hand-written
    //  matrix would reach, and randomizeAll is itself a shipped feature.
    // -----------------------------------------------------------------------
    beginCase ("Randomised full-state fuzz (300 states)");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        int nonFinite = 0, tooLoud = 0;
        float worst = 0.f;
        for (int i = 0; i < 300; ++i)
        {
            p.randomizeAll();
            const auto v = runBlocks (p, 10, kBlock, kRate, true);
            if (! v.finite) ++nonFinite;
            if (v.peak >= kCeiling) ++tooLoud;
            worst = jmax (worst, v.finite ? v.peak : worst);
        }
        check (nonFinite == 0, "no randomised state produced NaN/Inf (" + String (nonFinite) + " bad)");
        check (tooLoud == 0, "no randomised state ran away (" + String (tooLoud)
                             + " over, worst peak " + String (worst, 2) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("Feedback stress: every surgeon routed, reinject wide open");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
        {
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "on")))   par->setValueNotifyingHost (1.f);
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "mix")))  par->setValueNotifyingHost (1.f);
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "prob"))) par->setValueNotifyingHost (1.f);
            // route each surgeon from itself — the worst case for feedback
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "route")))
                par->setValueNotifyingHost (par->getNormalisableRange().convertTo0to1 ((float) (s + 1)));
        }
        for (auto pid : { vsx::id::reinject, vsx::id::chaos, vsx::id::triggerRate, vsx::id::dryWet })
            if (auto* par = p.apvts.getParameter (pid)) par->setValueNotifyingHost (1.f);
        // OUT TRIM stays at its default here on purpose: this case is about
        // whether feedback diverges, and +24 dB of make-up would just scale the
        // answer and muddy what the number means.

        const auto v = runBlocks (p, 600, kBlock, kRate, true);
        check (v.finite, "self-routed feedback stays finite");
        check (v.peak < kCeiling, "self-routed feedback stays bounded (peak " + String (v.peak, 2) + ")");
    }

    // -----------------------------------------------------------------------
    //  Vivisect replays a rolling 4-16 bar buffer, so it is SUPPOSED to keep
    //  producing sound after the input stops - that is the instrument, not a
    //  leak. What it must not do is sustain forever once the buffer has been
    //  overwritten with silence and nothing is feeding output back in.
    beginCase ("Buffer drains to silence once nothing is feeding it");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();

        // Shortest buffer, full wet, and crucially no reinject: with reinject
        // open the plugin can legitimately sustain itself indefinitely.
        if (auto* par = p.apvts.getParameter (vsx::id::bufferBars)) par->setValueNotifyingHost (0.f);
        if (auto* par = p.apvts.getParameter (vsx::id::reinject))   par->setValueNotifyingHost (0.f);
        // CHAOS feeds the reinject amount too (processBlock adds
        // deriveChaos(chaos).feedback * 0.4), so it is zeroed as well to make
        // this a genuine nothing-is-feeding-it case.
        if (auto* par = p.apvts.getParameter (vsx::id::chaos))      par->setValueNotifyingHost (0.f);
        if (auto* par = p.apvts.getParameter (vsx::id::dryWet))     par->setValueNotifyingHost (1.f);
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "on")))
                par->setValueNotifyingHost (1.f);

        runBlocks (p, 400, kBlock, kRate, true, false);    // prime the specimen

        // The plugin is SUPPOSED to keep sounding for a while after the input
        // stops - it is replaying a buffer. So drain first and measure after,
        // rather than taking the worst peak across the whole period, which
        // would always catch the legitimate tail at the start.
        const int drain   = (int) (30.0 * kRate / kBlock);   // longer than any buffer
        const int measure = (int) (5.0  * kRate / kBlock);

        const auto during = runBlocks (p, drain, kBlock, kRate, false, true);
        check (during.finite, "silence stays finite while the buffer drains");

        const auto after = runBlocks (p, measure, kBlock, kRate, false, true);
        check (after.finite, "silence stays finite once drained");
        check (after.peak < 0.05f, "buffer is silent 30s after the input stops (peak "
                                   + String (after.peak, 5) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("State save / restore round trip");
    {
        VivisectProcessor a, b;
        a.prepareToPlay (kRate, kBlock);
        b.prepareToPlay (kRate, kBlock);

        for (int trial = 0; trial < 20; ++trial)
        {
            a.randomizeAll();
            MemoryBlock blob;
            a.getStateInformation (blob);
            check (blob.getSize() > 0, "state blob is non-empty");
            b.setStateInformation (blob.getData(), (int) blob.getSize());

            const auto sa = snapshotParams (a);
            const auto sb = snapshotParams (b);
            bool same = sa.size() == sb.size();
            String bad;
            for (const auto& kv : sa)
            {
                const auto it = sb.find (kv.first);
                if (it == sb.end() || std::abs (it->second - kv.second) > 1.0e-4f)
                {
                    same = false;
                    if (bad.isEmpty()) bad = kv.first;
                }
            }
            if (! check (same, "round trip preserves every parameter (first mismatch: " + bad + ")"))
                break;
        }
    }

    // -----------------------------------------------------------------------
    beginCase ("Preset file save / load round trip");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto tmp = File::getSpecialLocation (File::tempDirectory)
                     .getChildFile ("vivisect_test_preset.vsxpreset");
        tmp.deleteFile();

        p.randomizeAll();
        const auto before = snapshotParams (p);
        check (p.savePresetToFile (tmp), "preset writes to disk");
        check (tmp.existsAsFile(), "preset file exists after save");

        p.resetAllToDefaults();
        check (p.loadPresetFromFile (tmp), "preset loads from disk");
        const auto after = snapshotParams (p);

        bool same = true;
        String bad;
        for (const auto& kv : before)
        {
            const auto it = after.find (kv.first);
            if (it == after.end() || std::abs (it->second - kv.second) > 1.0e-4f)
            {
                same = false;
                if (bad.isEmpty()) bad = kv.first;
            }
        }
        check (same, "preset round trip preserves every parameter (first mismatch: " + bad + ")");
        tmp.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("Every factory preset loads and plays");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto names = p.getPresetNames();
        check (names.size() >= 6, "at least the six factory presets are listed");

        for (int i = 0; i < names.size(); ++i)
        {
            p.loadPreset (i);
            const auto v = runBlocks (p, 24, kBlock, kRate, true);
            check (v.finite, "preset '" + names[i] + "' produces finite output");
            check (v.peak < kCeiling, "preset '" + names[i] + "' stays bounded (peak "
                                      + String (v.peak, 2) + ")");
        }
    }

    // -----------------------------------------------------------------------
    //  Regression: the preset menu and the preset loader used to enumerate the
    //  folder separately, so a folder that changed between building the menu
    //  and picking from it could load the wrong preset.
    // -----------------------------------------------------------------------
    beginCase ("Preset list and loader agree, even when the folder changes");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto dir = p.getUserPresetDir();
        dir.createDirectory();

        // Names chosen so directory order and sorted order are unlikely to match.
        const StringArray made { "zzz_last", "aaa_first", "mmm_middle" };
        std::map<String, std::map<String, float>> expected;
        for (const auto& n : made)
        {
            p.randomizeAll();
            expected[n] = snapshotParams (p);
            p.saveUserPreset (n);
        }

        // Two independent enumerations must agree.
        const auto firstPass  = p.getUserPresetFiles();
        const auto secondPass = p.getUserPresetFiles();
        bool stable = firstPass.size() == secondPass.size();
        for (int i = 0; stable && i < firstPass.size(); ++i)
            stable = firstPass[i] == secondPass[i];
        check (stable, "two enumerations of the preset folder return the same order");

        // Sorted, so the menu reads predictably.
        bool sorted = true;
        for (int i = 1; i < firstPass.size(); ++i)
            if (firstPass[i - 1].getFileName().compareIgnoreCase (firstPass[i].getFileName()) > 0)
                sorted = false;
        check (sorted, "preset files come back sorted by name");

        // Name-based loading must fetch the preset that name was saved with.
        for (const auto& n : made)
        {
            p.resetAllToDefaults();
            const bool loaded = p.loadPresetByName ("* " + n);
            check (loaded, "loadPresetByName finds '" + n + "'");
            if (loaded)
            {
                const auto got = snapshotParams (p);
                bool same = true;
                for (const auto& kv : expected[n])
                {
                    const auto it = got.find (kv.first);
                    if (it == got.end() || std::abs (it->second - kv.second) > 1.0e-4f)
                        same = false;
                }
                check (same, "'" + n + "' loads the state it was saved with");
            }
        }

        check (! p.loadPresetByName ("* no_such_preset"), "a missing preset reports failure");
        check (p.loadPresetByName ("Clean Specimen"), "factory presets resolve by name too");

        for (const auto& n : made)
            dir.getChildFile (File::createLegalFileName (n) + ".vsxpreset").deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("A/B compare and history scrub");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        p.selectABSlot (0);
        p.randomizeAll();
        const auto slotA = snapshotParams (p);

        p.selectABSlot (1);
        p.randomizeAll();
        const auto slotB = snapshotParams (p);

        p.selectABSlot (0);
        const auto backToA = snapshotParams (p);

        bool restored = true;
        for (const auto& kv : slotA)
        {
            const auto it = backToA.find (kv.first);
            if (it == backToA.end() || std::abs (it->second - kv.second) > 1.0e-4f)
                restored = false;
        }
        check (restored, "switching back to slot A restores slot A's settings");
        check (slotA != slotB, "the two A/B slots actually differ");

        // history scrub must not fault at either extreme
        runBlocks (p, 60, kBlock, kRate, true);
        for (float x : { 0.f, 0.5f, 1.f })
            p.rewindToNormalized (x);
        const auto v = runBlocks (p, 24, kBlock, kRate, true);
        check (v.finite, "output still finite after a history scrub");
    }

    // -----------------------------------------------------------------------
    beginCase ("MIDI: every note, every CC, no mapping assumptions");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        AudioBuffer<float> buf (2, kBlock);
        MidiBuffer midi;
        bool finite = true;

        for (int note = 0; note < 128 && finite; ++note)
        {
            fillTestSignal (buf, note * kBlock, kRate);
            midi.clear();
            midi.addEvent (MidiMessage::noteOn (1, note, (uint8) 127), 0);
            midi.addEvent (MidiMessage::noteOff (1, note), kBlock - 1);
            p.processBlock (buf, midi);
            finite = inspect (buf).finite;
        }
        check (finite, "all 128 notes handled without NaN");

        finite = true;
        for (int cc = 0; cc < 128 && finite; ++cc)
            for (int val : { 0, 64, 127 })
            {
                fillTestSignal (buf, cc * kBlock, kRate);
                midi.clear();
                midi.addEvent (MidiMessage::controllerEvent (1, cc, val), 0);
                p.processBlock (buf, midi);
                if (! inspect (buf).finite) { finite = false; break; }
            }
        check (finite, "all 128 CCs at three values handled without NaN");
    }

    // -----------------------------------------------------------------------
    beginCase ("Telemetry ring under load");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.randomizeAll();
        runBlocks (p, 50, kBlock, kRate, true);

        vsx::TelemetryEvent evs[64];
        int total = 0;
        for (int i = 0; i < 40; ++i)
            total += p.telemetry().drain (evs, 64);
        check (total > 0, "telemetry produced events (" + String (total) + ")");

        // Draining an idle ring must terminate and yield nothing.
        const int idle = p.telemetry().drain (evs, 64);
        check (idle == 0, "draining an idle ring returns nothing");

        // Overflow: push far more than capacity, then confirm the reader
        // recovers instead of spinning or handing back stale traffic.
        for (int i = 0; i < vsx::TelemetryRing::capacity * 4; ++i)
            p.telemetry().pushFire (i % vsx::kNumSurgeons, 0.5f, 10.f);
        const int after = p.telemetry().drain (evs, 64);
        check (after > 0 && after <= 64, "reader recovers after a ring overflow ("
                                         + String (after) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("Specimen export");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.randomizeAll();
        runBlocks (p, 120, kBlock, kRate, true);

        auto wav = File::getSpecialLocation (File::tempDirectory)
                     .getChildFile ("vivisect_test_export.wav");
        wav.deleteFile();
        check (p.exportSpecimenToWav (wav), "specimen exports to wav");
        check (wav.existsAsFile() && wav.getSize() > 1024, "exported wav is non-trivial");
        wav.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("Process without prepareToPlay, and zero-length blocks");
    {
        VivisectProcessor p;              // deliberately not prepared
        AudioBuffer<float> empty (2, 0);
        MidiBuffer midi;
        p.processBlock (empty, midi);     // must not fault
        check (true, "zero-length block is survivable");

        p.prepareToPlay (kRate, kBlock);
        AudioBuffer<float> one (2, 1);
        one.clear();
        midi.clear();
        p.processBlock (one, midi);
        check (inspect (one).finite, "single-sample block is finite");
    }

    // -----------------------------------------------------------------------
    beginCase ("Diagnostics: troubleshooting report and misconfiguration hints");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.randomizeAll();

        const auto report = p.buildTroubleshootingReport();
        check (report.isNotEmpty(), "report is non-empty");
        for (auto* section : { "PRODUCT", "HOST / DAW", "AUDIO", "MIDI", "SYSTEM",
                               "LIGHT DIAGNOSTIC", "FOLDERS", "CURRENT SETTINGS" })
            check (report.contains (section), String ("report contains the ") + section + " section");
        check (report.contains (JucePlugin_VersionString), "report states the version");
        check (report.contains ("licence"), "report states the licence");

        // A deliberately broken setup must be spotted.
        p.resetAllToDefaults();
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "on")))
                par->setValueNotifyingHost (0.f);
        if (auto* par = p.apvts.getParameter (vsx::id::dryWet)) par->setValueNotifyingHost (0.f);

        const auto notes = p.detectMisconfiguration();
        check (notes.size() >= 2, "an obviously broken setup raises hints ("
                                  + String (notes.size()) + ")");

        // A sane setup should not cry wolf.
        p.resetAllToDefaults();
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            if (auto* par = p.apvts.getParameter (vsx::sid (s, "on")))
                par->setValueNotifyingHost (1.f);
        check (p.detectMisconfiguration().isEmpty(), "a sane default setup raises no hints");

        auto f = File::getSpecialLocation (File::tempDirectory)
                   .getChildFile ("vivisect_test_report.txt");
        f.deleteFile();
        check (p.writeTroubleshootingFile (f), "report writes to disk");
        check (f.existsAsFile() && f.getSize() > 512, "written report is substantial");
        f.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("Diagnostics: crash log lifecycle");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        check (! p.isCrashLogEnabled(), "crash logging is OFF on a fresh instance");

        p.setCrashLogEnabled (true);
        check (p.isCrashLogEnabled(), "crash logging switches on");
        const auto logFile = p.currentCrashLogFile();
        check (logFile.existsAsFile(), "a log file is created immediately");

        // include.md: the troubleshooting report is copied to the TOP of it.
        const auto head = logFile.loadFileAsString();
        check (head.contains ("VIVISECT TROUBLESHOOTING REPORT"),
               "the crash log opens with the troubleshooting report");

        p.appendToCrashLog ("test line one");
        p.setCrashLogEnabled (false);
        check (! p.isCrashLogEnabled(), "crash logging switches off");

        const auto body = logFile.loadFileAsString();
        check (body.contains ("test line one"), "appended lines are written");
        check (body.contains ("switched off"), "the closing line is written before the flag clears");

        // A state round trip must NOT bring crash logging back on.
        p.setCrashLogEnabled (true);
        const auto second = p.currentCrashLogFile();
        MemoryBlock blob;
        p.getStateInformation (blob);
        VivisectProcessor q;
        q.prepareToPlay (kRate, kBlock);
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        check (! q.isCrashLogEnabled(), "crash logging is never restored from saved state");

        p.setCrashLogEnabled (false);
        logFile.deleteFile();
        second.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("Hard reset clears settings but keeps the user's presets");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        auto dir = p.getUserPresetDir();
        dir.createDirectory();
        p.randomizeAll();
        p.saveUserPreset ("hard_reset_survivor");
        auto preset = dir.getChildFile ("hard_reset_survivor.vsxpreset");
        check (preset.existsAsFile(), "a user preset exists before the reset");

        auto cache = vsx::userCacheDir();
        cache.createDirectory();
        auto junk = cache.getChildFile ("scratch.tmp");
        junk.replaceWithText ("delete me");

        p.randomizeAll();
        p.setTooltipsEnabled (false);
        p.hardResetAllSettings();

        const auto after = snapshotParams (p);
        VivisectProcessor fresh;
        const auto defaults = snapshotParams (fresh);
        bool same = true;
        for (const auto& kv : defaults)
        {
            const auto it = after.find (kv.first);
            if (it == after.end() || std::abs (it->second - kv.second) > 1.0e-4f)
                same = false;
        }
        check (same, "hard reset returns every parameter to its default");
        check (p.tooltipsEnabled(), "hard reset restores the default tool tip setting");
        check (! p.slotHasSample (0) && ! p.slotHasSample (1), "hard reset clears both sample slots");
        check (! junk.existsAsFile(), "hard reset wipes the cache folder");
        check (preset.existsAsFile(), "hard reset does NOT delete the user's presets");

        preset.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("Euclidean reorder pattern has exact pulse counts and spacing");
    {
        for (int steps = 2; steps <= 16; ++steps)
            for (int pulses = 1; pulses <= steps; ++pulses)
            {
                std::array<int, 64> pattern {};
                const int n = vsx::buildEuclideanPattern (steps, pulses, pattern);
                check (n == steps, "Euclidean pattern returns every requested step");

                int ones = 0;
                for (int i = 0; i < n; ++i)
                    ones += pattern[(size_t) i] != 0 ? 1 : 0;
                check (ones == pulses, "Euclidean pattern preserves pulse count");

                if (pulses > 1 && pulses < steps)
                {
                    std::vector<int> gaps;
                    int last = -1, first = -1;
                    for (int i = 0; i < n; ++i)
                        if (pattern[(size_t) i] != 0)
                        {
                            if (first < 0) first = i;
                            if (last >= 0) gaps.push_back (i - last);
                            last = i;
                        }
                    gaps.push_back (first + n - last);

                    const auto mm = std::minmax_element (gaps.begin(), gaps.end());
                    check (*mm.second - *mm.first <= 1,
                           "Euclidean pulse gaps differ by at most one step");
                }
            }
    }

    // -----------------------------------------------------------------------
    beginCase ("Random Walk modulation is independent of host block size");
    {
        vsx::ModMatrix a, b;
        a.prepare (kRate);
        b.prepare (kRate);

        // Same elapsed audio time, radically different host block sizes.
        for (int i = 0; i < (int) kRate / 64; ++i)  a.process (64, 0.2f);
        for (int i = 0; i < (int) kRate / 512; ++i) b.process (512, 0.2f);

        check (std::abs (a.sourceValue (6) - b.sourceValue (6)) < 0.03f,
               "random-walk value is effectively block-size invariant after one second");
        check (std::abs (a.sourceValue (6)) <= 1.f && std::abs (b.sourceValue (6)) <= 1.f,
               "random-walk output remains bounded");
    }

    // -----------------------------------------------------------------------
    beginCase ("FREEZE remains finite and reasonably level across blur");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();

        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            if (auto* q = p.apvts.getParameter (vsx::sid (s, "on")))
                q->setValueNotifyingHost (s == vsx::S_FREEZE ? 1.f : 0.f);
        if (auto* q = p.apvts.getParameter (vsx::id::dryWet)) q->setValueNotifyingHost (1.f);
        if (auto* q = p.apvts.getParameter (vsx::sid (vsx::S_FREEZE, "mix"))) q->setValueNotifyingHost (1.f);

        float minPeak = 1000.f, maxPeak = 0.f;
        for (float blur : { 0.f, 0.25f, 0.5f, 0.75f, 1.f })
        {
            if (auto* q = p.apvts.getParameter (vsx::sid (vsx::S_FREEZE, "p3")))
                q->setValueNotifyingHost (blur);

            runBlocks (p, 40, kBlock, kRate, false);
            p.triggerSurgeonManual (vsx::S_FREEZE);
            const auto v = runBlocks (p, 30, kBlock, kRate, false, true);
            check (v.finite, "FREEZE output remains finite across blur");
            minPeak = jmin (minPeak, v.peak);
            maxPeak = jmax (maxPeak, v.peak);
        }

        check (maxPeak < kCeiling, "FREEZE stays bounded at all blur settings");
        check (minPeak > 0.001f, "FREEZE does not collapse to silence after a valid capture");
    }

    // -----------------------------------------------------------------------
    beginCase ("SCAR master texture is bounded and automatable");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();

        auto* on    = p.apvts.getParameter (vsx::id::scarOn);
        auto* drive = p.apvts.getParameter (vsx::id::scarDrive);
        auto* mix   = p.apvts.getParameter (vsx::id::scarMix);
        check (on != nullptr && drive != nullptr && mix != nullptr,
               "SCAR exposes on, drive and mix parameters");
        check (on != nullptr && on->getValue() < 0.5f, "SCAR is off by default");

        if (on != nullptr) on->setValueNotifyingHost (1.f);

        float worst = 0.f;
        bool finite = true;
        for (float d : { 0.f, 0.25f, 0.5f, 0.75f, 1.f })
            for (float m : { 0.f, 0.5f, 1.f })
            {
                if (drive != nullptr) drive->setValueNotifyingHost (d);
                if (mix != nullptr) mix->setValueNotifyingHost (m);
                const auto v = runBlocks (p, 30, kBlock, kRate, true);
                finite = finite && v.finite;
                worst = jmax (worst, v.peak);
            }

        check (finite, "SCAR never produces NaN/Inf");
        check (worst < ceilingFor (p), "SCAR remains bounded (worst peak "
                                       + String (worst, 2) + ")");

        if (on != nullptr) on->setValueNotifyingHost (0.f);
        const auto v = runBlocks (p, 12, kBlock, kRate, true);
        check (v.finite, "switching SCAR off leaves the main signal finite");
    }

    // -----------------------------------------------------------------------
    beginCase ("Specimen export supports every advertised WAV quality");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        runBlocks (p, 80, kBlock, kRate, true);

        for (int bits : { 16, 24, 32 })
        {
            auto wav = File::getSpecialLocation (File::tempDirectory)
                         .getChildFile ("vivisect_export_" + String (bits) + ".wav");
            wav.deleteFile();

            double seconds = 0.0;
            check (p.exportSpecimenToWav (wav, bits, &seconds),
                   String ("export writes ") + String (bits) + "-bit wav");
            check (seconds > 0.0, "export reports a positive duration");

            WavAudioFormat fmt;
            std::unique_ptr<AudioFormatReader> reader (fmt.createReaderFor (wav.createInputStream().release(), true));
            check (reader != nullptr, "exported wav opens again");
            if (reader != nullptr)
            {
                check ((int) reader->bitsPerSample == bits,
                       "exported wav reports the selected bit depth");
                check (reader->lengthInSamples > 0, "exported wav contains samples");
            }
            wav.deleteFile();
        }

        auto invalid = File::getSpecialLocation (File::tempDirectory)
                         .getChildFile ("vivisect_export_invalid.wav");
        invalid.deleteFile();
        check (! p.exportSpecimenToWav (invalid, 12), "unsupported WAV bit depth is rejected");
        check (! invalid.existsAsFile(), "rejected export does not leave a file behind");
    }

    // -----------------------------------------------------------------------
    //  The hidden effect is a real signal path, so it is held to the same bar
    //  as everything else - a secret that can blow up a mix is not a feature.
    // -----------------------------------------------------------------------
    beginCase ("FLATLINE (hidden effect) stays bounded at every setting");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.resetAllToDefaults();

        auto* on = p.apvts.getParameter (vsx::id::flatOn);
        check (on != nullptr && on->getValue() < 0.5f, "FLATLINE is off by default");

        if (on != nullptr) on->setValueNotifyingHost (1.f);

        float worst = 0.f;
        bool finite = true;
        for (float tone : { 0.f, 0.3f, 0.6f, 1.f })
            for (float bleed : { 0.f, 0.5f, 1.f })
                for (float mix : { 0.f, 0.5f, 1.f })
                {
                    if (auto* q = p.apvts.getParameter (vsx::id::flatTone))  q->setValueNotifyingHost (tone);
                    if (auto* q = p.apvts.getParameter (vsx::id::flatBleed)) q->setValueNotifyingHost (bleed);
                    if (auto* q = p.apvts.getParameter (vsx::id::flatMix))   q->setValueNotifyingHost (mix);

                    const auto v = runBlocks (p, 30, kBlock, kRate, true);
                    if (! v.finite) { finite = false; break; }
                    worst = jmax (worst, v.peak);
                }
        check (finite, "FLATLINE never produces NaN/Inf");
        check (worst < kCeiling, "FLATLINE never diverges (worst peak " + String (worst, 2) + ")");

        // Maximum resonance for a long run: a comb at unity feedback would
        // self-oscillate, so this is the case that matters.
        if (auto* q = p.apvts.getParameter (vsx::id::flatBleed)) q->setValueNotifyingHost (1.f);
        if (auto* q = p.apvts.getParameter (vsx::id::flatMix))   q->setValueNotifyingHost (1.f);
        const auto v = runBlocks (p, 800, kBlock, kRate, true);
        check (v.finite, "FLATLINE at full resonance stays finite");
        check (v.peak < kCeiling, "FLATLINE at full resonance does not self-oscillate (peak "
                                  + String (v.peak, 2) + ")");

        // And it must fall silent again once switched off.
        if (on != nullptr) on->setValueNotifyingHost (0.f);
        runBlocks (p, 400, kBlock, kRate, false, true);
        const auto quiet = runBlocks (p, 60, kBlock, kRate, false, true);
        check (quiet.peak < 0.05f, "FLATLINE stops ringing when switched off (peak "
                                   + String (quiet.peak, 5) + ")");
    }

    // -----------------------------------------------------------------------
    beginCase ("RANDOM never reveals the hidden effect");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        bool everOn = false;
        for (int i = 0; i < 200; ++i)
        {
            p.randomizeAll();
            if (auto* on = p.apvts.getParameter (vsx::id::flatOn))
                everOn = everOn || on->getValue() > 0.5f;
        }
        check (! everOn, "200 randomisations never switch FLATLINE on");
    }

    // -----------------------------------------------------------------------
    std::printf ("\n=====================\n%d checks, %d failed\n", checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}
