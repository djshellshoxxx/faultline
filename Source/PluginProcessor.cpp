#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <csignal>
#endif
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include "PluginEditor.h"
#include "Theme.h"

using namespace vsx;

static const juce::Identifier kSamplesTag { "VSX_SAMPLES" };

// ===========================================================================
VivisectProcessor::VivisectProcessor()
    : juce::AudioProcessor (BusesProperties()
        .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
        .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "VIVISECT", layout())
{
    formatManager.registerBasicFormats();
    for (auto& a : surgAct) a.store (0.f);
    for (auto& c : ccToParam) c.store (-1);
    for (auto& v : pendingCCValue) v.store (-1.f);
    for (auto& v : pendingCCParam) v.store (-1);
    seedDefaultMidiMap();
}
VivisectProcessor::~VivisectProcessor()
{
    stopTimer();
    // The crash handler is process-wide and points into this binary. Leaving it
    // installed after the host unloads the plug-in would turn the next host
    // crash into a jump into freed code.
    setCrashLogEnabled (false);
}

bool VivisectProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    if (in != out) return false;
    const int sc = layouts.getNumChannels (true, 1);
    return sc == 0 || sc == 1 || sc == 2;
}

void VivisectProcessor::prepareToPlay (double sr, int block)
{
    scar.reset();
    scarWasOn = false;
    flatline.prepare (sr, block);
    sampleRate = sr;
    specimen.prepare (sr, 45.0);
    analysis.prepare (sr);
    rack.prepare (sr, block);
    scheduler.prepare (sr);
    modMatrix.prepare (sr);

    dryBuf.setSize (2, block);
    wetBuf.setSize (2, block);
    workBuf.setSize (2, block);

    songPos = 0.0;
    decayPhase = 0.f;
    scEnvPrev = 0.f;
    panicTimer = 0;

    histRing.clearQuick();
    for (int i = 0; i < kHist; ++i) histRing.add ({});
    histHead = histCount = 0;

    // 30 Hz: MIDI-CC parameter changes reach the host promptly; the history
    // snapshot still runs at 2 Hz (every 15th tick).
    timerTicks = 0;
    startTimerHz (30);
}

// ===========================================================================
float VivisectProcessor::pvMod (const char* pid, int destEnum)
{
    auto* p = apvts.getParameter (pid);
    if (p == nullptr) return pv (pid);
    // Start from the raw value the rest of the DSP reads, so a MIDI CC move
    // that has not been flushed to the host yet is still heard consistently.
    const float base = p->convertTo0to1 (pv (pid));
    const float v = juce::jlimit (0.f, 1.f, base + modMatrix.destOffset (destEnum));
    return p->convertFrom0to1 (v);
}

double VivisectProcessor::bufferRegionSamples (double samplesPerBeat) const
{
    // BUFFER: 4 / 8 / 16 bars of 4/4. The scheduler clamps to the ring size.
    const int bars = 4 << juce::jlimit (0, 2, (int) pv (id::bufferBars));
    return bars * 4.0 * samplesPerBeat;
}

void VivisectProcessor::triggerSurgeonManual (int i)
{
    if (specimen.capacitySamples() < 1024)      // not prepared yet: nothing to cut
        return;
    i = juce::jlimit (0, kNumSurgeons - 1, i);
    rack[i].setParams (pv (sidRaw (i, SP_P1)),
                       pv (sidRaw (i, SP_P2)),
                       pv (sidRaw (i, SP_P3)));
    auto c = scheduler.makeManualContext (pv (sidRaw (i, SP_P1)),
                                          (int) pv (id::sourceSel), pv (id::morph), pv (id::chaos),
                                          60.0 / lastBpm * sampleRate, specimen, analysis,
                                          bufferRegionSamples (60.0 / lastBpm * sampleRate));
    rack[i].trigger (c);
    telem.pushFire (i, (float) std::fmod (c.sourcePos, 1000.0) / 1000.f,
                    (float) (c.lengthSamples * 1000.0 / juce::jmax (1.0, sampleRate)));
}

void VivisectProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals _;
    auto main = getBusBuffer (buffer, true, 0);
    const int n = main.getNumSamples();
    const int mainCh = main.getNumChannels();
    if (n <= 0) return;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);

    if (workBuf.getNumSamples() < n) { workBuf.setSize (2, n, false, false, true);
                                       dryBuf.setSize (2, n, false, false, true);
                                       wetBuf.setSize (2, n, false, false, true); }

    workBuf.copyFrom (0, 0, main, 0, 0, n);
    workBuf.copyFrom (1, 0, main, mainCh > 1 ? 1 : 0, 0, n);
    workBuf.applyGain (juce::Decibels::decibelsToGain (pv (id::inputTrim)));
    dryBuf.makeCopyOf (workBuf);

    // ---- MIDI ---------------------------------------------------------------
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
        {
            const int note = m.getNoteNumber();
            const bool mapped = note >= 48 && note <= 53;
            telem.pushNote (note, (int) (m.getVelocity()), mapped ? note - 48 : 255);
            if (mapped) triggerSurgeonManual (note - 48);
        }
        else if (m.isController())
        {
            const int cc = m.getControllerNumber();
            const float v = m.getControllerValue() / 127.f;

            if (const int learn = learnParamIndex.load(); learn >= 0)
            {
                // arm-and-wiggle: the next CC seen claims the armed parameter
                for (auto& slot : ccToParam)
                    if (slot.load() == learn) slot.store (-1);
                ccToParam[(size_t) juce::jlimit (0, 127, cc)].store (learn);
                learnParamIndex.store (-1);
                midiMapDirty.store (true);
            }

            const int pIdx = ccToParam[(size_t) juce::jlimit (0, 127, cc)].load();
            telem.pushCC (cc, m.getControllerValue(), pIdx);
            if (pIdx >= 0 && pIdx < getParameters().size())
            {
                // Audible now via the raw value the DSP reads; the host is
                // told from the message thread by the timer.
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (getParameters()[pIdx]))
                    if (auto* raw = apvts.getRawParameterValue (rp->paramID))
                        raw->store (rp->convertFrom0to1 (v));
                pendingCCParam[(size_t) cc].store (pIdx);
                pendingCCValue[(size_t) cc].store (v);
                telem.pushSimple (vsx::TelemetryEvent::Kind::paramChange, v, 0.f,
                                  juce::jmin (254, pIdx));
            }
        }
    }
    midi.clear();

    // ---- musical clock ----------------------------------------------------
    double bpm = lastBpm, ppq = 0.0;
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (pos->getBpm())         bpm = *pos->getBpm();
            if (pos->getPpqPosition()) ppq = *pos->getPpqPosition();
            playing = pos->getIsPlaying();
        }
    bpm = juce::jlimit (20.0, 300.0, bpm);
    lastBpm = bpm;
    telem.pushSimple (vsx::TelemetryEvent::Kind::clock, (float) bpm, (float) ppq,
                      playing ? 1 : 0);
    const double spb = 60.0 / bpm * sampleRate;
    if (playing) songPos = ppq * spb;
    else         songPos += n;

    // ---- mod matrix -----------------------------------------------------
    modMatrix.setLFO (0, pv (id::lfo1Rate), (int) pv (id::lfo1Shape));
    modMatrix.setLFO (1, pv (id::lfo2Rate), (int) pv (id::lfo2Shape));
    modMatrix.setMacro (0, pv (id::macro1));
    modMatrix.setMacro (1, pv (id::macro2));
    static constexpr const char* modSourceIds[kNumModSlots] { "mm1_src", "mm2_src", "mm3_src", "mm4_src" };
    static constexpr const char* modDestIds[kNumModSlots]   { "mm1_dst", "mm2_dst", "mm3_dst", "mm4_dst" };
    static constexpr const char* modDepthIds[kNumModSlots]  { "mm1_depth", "mm2_depth", "mm3_depth", "mm4_depth" };
    for (int mIdx = 0; mIdx < kNumModSlots; ++mIdx)
    {
        modMatrix.setSlot (mIdx, (int) pv (modSourceIds[mIdx]),
                                 (int) pv (modDestIds[mIdx]),
                                       pv (modDepthIds[mIdx]));
    }
    modMatrix.process (n, workBuf.getRMSLevel (0, 0, n));

    // ---- decay-over-time ------------------------------------------------
    const bool decayArm = pv (id::decayArm) > 0.5f;
    const double decayT = juce::jmax (0.1, (double) pv (id::decayTime));
    if (decayArm) decayPhase = juce::jmin (1.f, decayPhase + (float) (n / (decayT * sampleRate)));
    else          decayPhase = juce::jmax (0.f, decayPhase - (float) (n / (2.0 * sampleRate)));

    // ---- resolved globals ---------------------------------------------
    float chaos = juce::jmax (pvMod (id::chaos, 1), decayPhase);
    const float gPull   = pvMod (id::gravityPull, 2);
    const float swing   = pvMod (id::swing, 3);
    const int   grid    = (int) pv (id::gravityGrid);
    const float trigRate = pvMod (id::triggerRate, 4);
    float reinject = juce::jmax (pvMod (id::reinject, 5), decayPhase * 0.7f);
    reinject = juce::jmin (1.f, reinject + deriveChaos (chaos).feedback * 0.4f);
    const float analysisInform = pv (id::analysisInform);
    const int   sourceSel = (int) pv (id::sourceSel);
    const float morph = pvMod (id::morph, 6);
    const bool  midiMode = pv (id::midiMode) > 0.5f;
    const bool  panic = pv (id::panicFreeze) > 0.5f;

    std::array<bool,  kNumSurgeons> on {};
    std::array<float, kNumSurgeons> mixA {}, probA {}, p1A {};
    std::array<int,   kNumSurgeons> routeA {};
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        on[(size_t) s]     = pv (sidRaw (s, SP_ON)) > 0.5f;
        mixA[(size_t) s]   = pv (sidRaw (s, SP_MIX));
        probA[(size_t) s]  = pv (sidRaw (s, SP_PROB));
        p1A[(size_t) s]    = pv (sidRaw (s, SP_P1));
        routeA[(size_t) s] = (int) pv (sidRaw (s, SP_ROUTE));

        float q1 = p1A[(size_t) s];
        float q2 = pv (sidRaw (s, SP_P2));
        float q3 = pv (sidRaw (s, SP_P3));
        if (s == S_STUTTER)  q3 = pvMod (sidRaw (0, SP_P3), 7);
        if (s == S_GRANULAR) { q2 = pvMod (sidRaw (1, SP_P2), 8); q3 = pvMod (sidRaw (1, SP_P3), 9); }
        if (s == S_CORRUPT)  { q1 = pvMod (sidRaw (3, SP_P1), 10); q2 = pvMod (sidRaw (3, SP_P2), 11); }
        if (s == S_FREEZE)   q3 = pvMod (sidRaw (5, SP_P3), 12);
        rack[s].setParams (q1, q2, q3);
    }

    if (decayPhase > 0.05f)
    {
        on[S_CORRUPT] = true;
        mixA[S_CORRUPT] = juce::jmax (mixA[S_CORRUPT], decayPhase);
    }

    specimen.setFrozen (panic);
    if (panic)
    {
        on.fill (false);
        on[S_FREEZE] = true;
        mixA[S_FREEZE] = 1.f;
        panicTimer += n;
        if (panicTimer > (int) (sampleRate * 0.4) && rack[S_FREEZE].activity() < 0.2f)
        {
            triggerSurgeonManual (S_FREEZE);
            panicTimer = 0;
        }
    }

    // ---- write to specimen + analyse --------------------------------
    const i64 before = specimen.writePos();
    specimen.push (workBuf);
    analysis.transients.process (workBuf, before);
    analysis.spectral.process (specimen, n);

    // ---- schedule --------------------------------------------------
    MusicalContext mc;
    mc.songPosSamples = songPos;
    mc.samplesPerBeat = spb;
    mc.gridIndex = grid;
    mc.pull = gPull;
    mc.swing = swing;
    mc.playing = playing;
    mc.regionSamples = bufferRegionSamples (spb);

    std::array<float, kNumSurgeons> actArr {};
    for (int s = 0; s < kNumSurgeons; ++s) actArr[(size_t) s] = rack[s].activity();

    int triggers = 0;
    scheduler.advance (n, mc, on, probA, p1A, actArr, trigRate, chaos, analysisInform,
                       midiMode, sourceSel, morph, specimen, analysis,
                       [&] (int s, TriggerContext& ctx) { rack[s].trigger (ctx); ++triggers; });

    // ---- sidechain --------------------------------------------------
    if (auto* scBus = getBus (true, 1))
        if (scBus->isEnabled())
        {
            auto sc = getBusBuffer (buffer, true, 1);
            const int scMode = (int) pv (id::scMode);
            const float scAmt = pv (id::scAmount);
            const float e = sc.getRMSLevel (0, 0, n);
            if (scAmt > 0.05f && e > scEnvPrev * 1.8f + 0.01f)
            {
                for (int s = 0; s < kNumSurgeons; ++s)
                {
                    if (! on[(size_t) s] || scheduler.rng.nextFloat() >= scAmt) continue;
                    if (scMode == 0)
                        triggerSurgeonManual (s);
                    else
                    {
                        auto c = scheduler.makeManualContext (p1A[(size_t) s], 2, morph, chaos, spb, specimen, analysis,
                                                             mc.regionSamples);
                        rack[s].trigger (c);
                    }
                }
            }
            scEnvPrev = e;
        }

    // ---- run surgeons + reinject ---------------------------------
    if (wetBuf.getNumSamples() < n) wetBuf.setSize (2, n, false, false, true);
    rack.process (specimen, wetBuf, n, mixA, routeA, reinject);
    float maxAct = panic ? 1.f : rack.maxActivity();

    // ---- mix ----------------------------------------------------
    const float dw = pv (id::dryWet);
    const float outG = juce::Decibels::decibelsToGain (pv (id::outputTrim));
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* d = dryBuf.getReadPointer (ch);
        const float* w = wetBuf.getReadPointer (ch);
        float* o = workBuf.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            o[i] = (d[i] * (1.f - dw * maxAct) + w[i] * dw) * outG;
    }

    // ---- SCAR (master texture effect) --------------------------------
    if (pv (id::scarOn) > 0.5f)
    {
        scar.process (workBuf, n, pv (id::scarDrive), pv (id::scarMix));
        scarWasOn = true;
    }
    else if (scarWasOn)
    {
        scar.reset();
        scarWasOn = false;
    }

    // ---- FLATLINE (hidden effect) ------------------------------------
    // Clearing the delay line is O(line length); do it once on the way out
    // rather than every block while the effect sits idle.
    if (pv (id::flatOn) > 0.5f)
    {
        flatline.process (workBuf, n, pv (id::flatTone), pv (id::flatBleed), pv (id::flatMix));
        flatlineWasOn = true;
    }
    else if (flatlineWasOn)
    {
        flatline.reset();
        flatlineWasOn = false;
    }

    main.copyFrom (0, 0, workBuf, 0, 0, n);
    if (mainCh > 1) main.copyFrom (1, 0, workBuf, 1, 0, n);

    // ---- meters ----------------------------------------------
    for (int ch = 0; ch < 2; ++ch)
        outPeak[(size_t) ch].store (workBuf.getMagnitude (ch, 0, n));
    for (int s = 0; s < kNumSurgeons; ++s) surgAct[(size_t) s].store (rack[s].activity());
    pulseSmooth.store (pulseSmooth.load() * 0.90f + juce::jmin (1.f, triggers * 0.5f) * 0.10f);
}

// ===========================================================================
void VivisectProcessor::buildMonitorSnapshot (MonitorSnapshot& snap)
{
    const int cap = specimen.capacitySamples();
    const i64 wp = specimen.writePos();
    const double spb = 60.0 / lastBpm * sampleRate;
    const int COLS = MonitorSnapshot::COLS;

    snap.frozen = specimen.isFrozen();
    snap.chaos  = pv (id::chaos);
    snap.pulse  = pulseSmooth.load();
    for (int s = 0; s < kNumSurgeons; ++s) snap.surgAct[s] = surgAct[(size_t) s].load();

    // The editor can paint before the host has ever called prepareToPlay
    // (standalone with no audio device, or a DAW that opens the window
    // first). Nothing is allocated yet, so show a flat line.
    if (cap < 1024 + 8)
    {
        std::fill (std::begin (snap.envMin), std::end (snap.envMin), 0.f);
        std::fill (std::begin (snap.envMax), std::end (snap.envMax), 0.f);
        std::fill (std::begin (snap.bright), std::end (snap.bright), 0.f);
        snap.numTransients = 0;
        snap.numGrabs = 0;
        return;
    }
    const double region = juce::jlimit (1024.0, (double) cap - 8.0, bufferRegionSamples (spb));

    const double base = (double) wp - region;
    for (int c = 0; c < COLS; ++c)
    {
        const double a = base + region * c / COLS;
        const double b = base + region * (c + 1) / COLS;
        float mn = 1.0e9f, mx = -1.0e9f;
        for (int k = 0; k <= 6; ++k)
        {
            const float v = specimen.readInterp (0, a + (b - a) * k / 6.0);
            mn = juce::jmin (mn, v);
            mx = juce::jmax (mx, v);
        }
        if (mn > mx) { mn = mx = 0.f; }
        snap.envMin[c] = mn;
        snap.envMax[c] = mx;
    }
    analysis.spectral.fillBrightness (snap.bright, COLS, (i64) base, wp);

    std::vector<i64> tr;
    analysis.transients.copyRecent (tr, (i64) base);
    snap.numTransients = 0;
    for (auto tp : tr)
    {
        if (snap.numTransients >= 96) break;
        const int col = (int) ((tp - base) / region * COLS);
        if (col >= 0 && col < COLS) snap.transientCol[snap.numTransients++] = col;
    }

    snap.numGrabs = 0;
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        const i64 gs = rack[s].grabStart();
        const int gl = rack[s].grabLen();
        if (gl <= 0) continue;
        int c0 = (int) ((gs - base) / region * COLS);
        int c1 = (int) ((gs + gl - base) / region * COLS);
        c0 = juce::jlimit (0, COLS - 1, c0);
        c1 = juce::jlimit (0, COLS - 1, c1);
        auto& G = snap.grabs[snap.numGrabs++];
        G.c0 = c0;
        G.c1 = juce::jmax (c0 + 1, c1);
        G.surgeon = s;
        G.age = 1.f - rack[s].activity();
    }
}

// ===========================================================================
void VivisectProcessor::flushPendingMidiParameterChanges()
{
    const auto& all = getParameters();
    for (int cc = 0; cc < 128; ++cc)
    {
        const float v = pendingCCValue[(size_t) cc].exchange (-1.f);
        if (v < 0.f) continue;
        const int idx = pendingCCParam[(size_t) cc].load();
        if (idx >= 0 && idx < all.size())
            all[idx]->setValueNotifyingHost (v);
    }
}

void VivisectProcessor::timerCallback()
{
    flushPendingMidiParameterChanges();
    if (midiMapDirty.exchange (false)) writeMidiMapToState();

    if (++timerTicks < 15) return;
    timerTicks = 0;

    const auto state = soundStateSnapshot();
    juce::String s;
    if (auto xml = state.createXml()) s = xml->toString (juce::XmlElement::TextFormat().singleLine());
    histRing.set (histHead, s);
    histHead = (histHead + 1) % kHist;
    histCount = juce::jmin (histCount + 1, kHist);
}

void VivisectProcessor::rewindToNormalized (float x)
{
    if (histCount < 2) return;
    const int idx = juce::jlimit (0, histCount - 1, (int) std::round (x * (histCount - 1)));
    const int oldest = (histHead - histCount + kHist) % kHist;
    const auto& s = histRing.getReference ((oldest + idx) % kHist);
    if (s.isNotEmpty())
        if (auto xml = juce::XmlDocument::parse (s))
        {
            restoreSoundState (juce::ValueTree::fromXml (*xml));
        }
}

// Short, stable label for a parameter index — used by the data stream, which
// has about twenty characters of room per line.
juce::String VivisectProcessor::describeParam (int index) const
{
    const auto& all = getParameters();
    if (index < 0 || index >= all.size())
        return "?";
    if (auto* p = dynamic_cast<const juce::AudioProcessorParameterWithID*> (all[index]))
        return p->paramID.toUpperCase();
    return all[index]->getName (12).toUpperCase();
}

// ===========================================================================
//  Presets
// ===========================================================================
juce::File VivisectProcessor::getUserPresetDir() const
{
    return vsx::userPresetDir();
}
// findChildFiles hands back directory order, which is neither sorted nor
// stable across two calls if the folder changed in between. Everything that
// needs the user preset list goes through here so the menu and the loader can
// never disagree about which file is at which index.
juce::Array<juce::File> VivisectProcessor::getUserPresetFiles() const
{
    juce::Array<juce::File> files;
    const auto dir = getUserPresetDir();
    if (dir.isDirectory())
        files = dir.findChildFiles (juce::File::findFiles, false, "*.vsxpreset");

    struct ByName
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareIgnoreCase (b.getFileName());
        }
    };
    ByName cmp;
    files.sort (cmp);
    return files;
}

juce::StringArray VivisectProcessor::getPresetNames() const
{
    juce::StringArray names { "Clean Specimen", "Late 90s Bristol", "Berlin Basement",
                              "Warp 1995", "Modern Hyperpop", "SOPHIE-adjacent" };
    jassert (names.size() == numFactoryPresets);
    for (const auto& f : getUserPresetFiles())
        names.add ("* " + f.getFileNameWithoutExtension());
    return names;
}
void VivisectProcessor::applyPresetMap (const std::map<juce::String, float>& m)
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
    for (const auto& kv : m)
        if (auto* p = apvts.getParameter (kv.first))
            p->setValueNotifyingHost (juce::jlimit (0.f, 1.f, kv.second));
}
void VivisectProcessor::loadPreset (int index)
{
    if (index < 0) return;
    if (index < numFactoryPresets) { applyPresetMap (factoryPreset (index)); return; }
    const auto files = getUserPresetFiles();
    const int u = index - numFactoryPresets;
    if (u >= 0 && u < files.size())
        loadPresetFromFile (files[u]);
}

bool VivisectProcessor::loadPresetByName (const juce::String& displayName)
{
    const auto names = getPresetNames();
    for (int i = 0; i < numFactoryPresets && i < names.size(); ++i)
        if (names[i] == displayName)
        {
            applyPresetMap (factoryPreset (i));
            return true;
        }

    // User presets are shown with a leading "* "; match on the bare file name
    // so a stale menu entry still finds the file it was named after.
    const auto bare = displayName.startsWith ("* ") ? displayName.substring (2) : displayName;
    for (const auto& f : getUserPresetFiles())
        if (f.getFileNameWithoutExtension().equalsIgnoreCase (bare))
            return loadPresetFromFile (f);

    return false;
}
void VivisectProcessor::saveUserPreset (const juce::String& name)
{
    auto dir = getUserPresetDir();
    dir.createDirectory();
    const auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".vsxpreset");
    if (auto xml = soundStateSnapshot().createXml()) xml->writeTo (file);
}

std::map<juce::String, float> VivisectProcessor::factoryPreset (int index) const
{
    std::map<juce::String, float> m;
    auto set = [&] (const juce::String& k, float v) { m[k] = v; };
    auto S = [] (int i, const char* s) { return sid (i, s); };

    switch (index)
    {
        case 1: // Late 90s Bristol — trip hop, quantised, swung
            set (id::chaos, 0.28f); set (id::gravityGrid, 0.0f); set (id::gravityPull, 0.85f);
            set (id::swing, 0.62f); set (id::dryWet, 0.80f); set (id::triggerRate, 0.30f);
            set (id::reinject, 0.10f); set (id::analysisInform, 0.55f);
            set (S (S_STUTTER, "on"), 1); set (S (S_STUTTER, "mix"), 0.9f); set (S (S_STUTTER, "prob"), 0.5f);
            set (S (S_STUTTER, "p1"), 0.4f); set (S (S_STUTTER, "p2"), 0.2f); set (S (S_STUTTER, "p3"), 0.15f);
            set (S (S_REVERSE, "on"), 1); set (S (S_REVERSE, "mix"), 0.55f); set (S (S_REVERSE, "prob"), 0.35f);
            set (S (S_REVERSE, "p2"), 0.6f); set (S (S_REVERSE, "p3"), 0.45f);
            set (S (S_GRANULAR, "on"), 1); set (S (S_GRANULAR, "mix"), 0.3f); set (S (S_GRANULAR, "prob"), 0.2f);
            set (S (S_CORRUPT, "on"), 1); set (S (S_CORRUPT, "mix"), 0.28f); set (S (S_CORRUPT, "prob"), 0.22f);
            set (S (S_CORRUPT, "p1"), 0.7f); set (S (S_CORRUPT, "p3"), 1.0f);
            break;
        case 2: // Berlin Basement — dub techno / industrial
            set (id::chaos, 0.55f); set (id::gravityGrid, 0.0f); set (id::gravityPull, 0.6f);
            set (id::dryWet, 0.92f); set (id::triggerRate, 0.40f); set (id::reinject, 0.35f);
            set (S (S_STUTTER, "on"), 1); set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "p2"), 0.5f);
            set (S (S_CORRUPT, "on"), 1); set (S (S_CORRUPT, "mix"), 0.8f); set (S (S_CORRUPT, "prob"), 0.5f);
            set (S (S_CORRUPT, "p1"), 0.35f); set (S (S_CORRUPT, "p3"), 0.4f);
            set (S (S_REORDER, "on"), 1); set (S (S_REORDER, "mix"), 0.6f); set (S (S_REORDER, "prob"), 0.4f);
            set (S (S_REORDER, "p1"), 0.4f); set (S (S_REORDER, "p2"), 0.1f);
            set (S (S_REVERSE, "on"), 1); set (S (S_REVERSE, "mix"), 0.5f); set (S (S_REVERSE, "prob"), 0.3f);
            break;
        case 3: // Warp 1995 — Autechre / Aphex, all surgeons live
            set (id::chaos, 0.72f); set (id::gravityGrid, 0.5f); set (id::gravityPull, 0.4f);
            set (id::dryWet, 0.95f); set (id::triggerRate, 0.5f); set (id::reinject, 0.4f);
            set (id::analysisInform, 0.6f);
            for (int s = 0; s < kNumSurgeons; ++s) set (S (s, "on"), 1);
            set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "prob"), 0.6f); set (S (S_STUTTER, "p2"), 0.7f); set (S (S_STUTTER, "p3"), 0.5f);
            set (S (S_GRANULAR, "mix"), 0.8f); set (S (S_GRANULAR, "prob"), 0.5f); set (S (S_GRANULAR, "p2"), 0.6f); set (S (S_GRANULAR, "p3"), 0.5f);
            set (S (S_REVERSE, "mix"), 0.6f); set (S (S_REVERSE, "prob"), 0.4f);
            set (S (S_CORRUPT, "mix"), 0.7f); set (S (S_CORRUPT, "prob"), 0.5f); set (S (S_CORRUPT, "p1"), 0.4f); set (S (S_CORRUPT, "p3"), 0.5f);
            set (S (S_REORDER, "mix"), 0.8f); set (S (S_REORDER, "prob"), 0.5f); set (S (S_REORDER, "p1"), 0.6f); set (S (S_REORDER, "p2"), 0.5f);
            set (S (S_FREEZE, "mix"), 0.6f); set (S (S_FREEZE, "prob"), 0.3f); set (S (S_FREEZE, "p3"), 0.5f);
            set (S (S_REORDER, "route"), 1.f / 6.f);
            break;
        case 4: // Modern Hyperpop — fast, ratcheted, bright
            set (id::chaos, 0.45f); set (id::gravityGrid, 0.25f); set (id::gravityPull, 0.9f);
            set (id::dryWet, 0.85f); set (id::triggerRate, 0.55f);
            set (S (S_STUTTER, "on"), 1); set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "prob"), 0.7f);
            set (S (S_STUTTER, "p1"), 0.25f); set (S (S_STUTTER, "p2"), 0.85f); set (S (S_STUTTER, "p3"), 0.6f);
            set (S (S_CORRUPT, "on"), 1); set (S (S_CORRUPT, "mix"), 0.6f); set (S (S_CORRUPT, "prob"), 0.4f);
            set (S (S_CORRUPT, "p1"), 0.4f); set (S (S_CORRUPT, "p2"), 0.3f); set (S (S_CORRUPT, "p3"), 0.5f);
            set (S (S_REVERSE, "on"), 1); set (S (S_REVERSE, "mix"), 0.4f); set (S (S_REVERSE, "prob"), 0.3f);
            set (S (S_FREEZE, "on"), 1); set (S (S_FREEZE, "mix"), 0.4f); set (S (S_FREEZE, "prob"), 0.2f); set (S (S_FREEZE, "p3"), 0.6f);
            break;
        case 5: // SOPHIE-adjacent — metallic, plastic, reinjected
            set (id::chaos, 0.5f); set (id::gravityGrid, 0.0f); set (id::gravityPull, 0.92f);
            set (id::dryWet, 0.9f); set (id::triggerRate, 0.45f); set (id::reinject, 0.5f);
            set (S (S_CORRUPT, "on"), 1); set (S (S_CORRUPT, "mix"), 0.8f); set (S (S_CORRUPT, "prob"), 0.5f);
            set (S (S_CORRUPT, "p1"), 0.85f); set (S (S_CORRUPT, "p2"), 0.45f); set (S (S_CORRUPT, "p3"), 1.0f);
            set (S (S_GRANULAR, "on"), 1); set (S (S_GRANULAR, "mix"), 0.6f); set (S (S_GRANULAR, "prob"), 0.4f);
            set (S (S_GRANULAR, "p1"), 0.15f); set (S (S_GRANULAR, "p2"), 0.6f); set (S (S_GRANULAR, "p3"), 0.35f);
            set (S (S_FREEZE, "on"), 1); set (S (S_FREEZE, "mix"), 0.6f); set (S (S_FREEZE, "prob"), 0.3f);
            set (S (S_FREEZE, "p1"), 0.3f); set (S (S_FREEZE, "p3"), 0.2f);
            set (S (S_STUTTER, "on"), 1); set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "p2"), 0.6f);
            set (S (S_REVERSE, "on"), 1); set (S (S_REVERSE, "mix"), 0.3f); set (S (S_REVERSE, "prob"), 0.2f);
            break;
        default: // Clean Specimen — defaults
            break;
    }
    return m;
}

// ===========================================================================
bool VivisectProcessor::loadSampleInto (int slot, const juce::File& file)
{
    if (slot < 0 || slot > 1) return false;
    std::unique_ptr<juce::AudioFormatReader> r (formatManager.createReaderFor (file));
    if (r == nullptr) return false;
    const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (sampleRate * 30.0));
    if (len < 2) return false;
    juce::AudioBuffer<float> tmp ((int) juce::jmax (1u, r->numChannels), len);
    r->read (&tmp, 0, len, 0, true, true);
    suspendProcessing (true);
    specimen.setSample (slot, std::move (tmp), r->sampleRate);
    suspendProcessing (false);
    sampleFiles[slot] = file;
    return true;
}
void VivisectProcessor::clearSampleSlot (int slot)
{
    if (slot < 0 || slot > 1) return;
    suspendProcessing (true);
    specimen.clearSample (slot);
    suspendProcessing (false);
    sampleFiles[slot] = juce::File();
}

// ===========================================================================
void VivisectProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    // Host project/session state includes both sonic parameters and global
    // VSX_SETTINGS (tooltips, MIDI mappings and exploration locks).
    // Loaded samples are referenced by path so the session reopens with the
    // same specimens. They ride outside apvts.state, so presets, A/B and
    // history never carry them.
    auto state = apvts.copyState();
    juce::ValueTree samples (kSamplesTag);
    for (int slot = 0; slot < 2; ++slot)
        if (sampleFiles[slot] != juce::File())
            samples.setProperty (slot == 0 ? "a" : "b", sampleFiles[slot].getFullPathName(), nullptr);
    state.appendChild (samples, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}
void VivisectProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            const auto samples = state.getChildWithName (kSamplesTag);
            if (samples.isValid())
                state.removeChild (samples, nullptr);

            apvts.replaceState (state);
            syncFromStateTree();

            for (int slot = 0; slot < 2; ++slot)
            {
                const auto path = samples.getProperty (slot == 0 ? "a" : "b").toString();
                const juce::File f = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
                if (f == sampleFiles[slot] && slotHasSample (slot)) continue;
                if (f.existsAsFile()) loadSampleInto (slot, f);
                else if (path.isEmpty() && slotHasSample (slot)) clearSampleSlot (slot);
                // a path that no longer exists keeps the reference, so saving
                // again does not silently lose it
                else if (path.isNotEmpty()) sampleFiles[slot] = f;
            }
        }
}


// ===========================================================================
//  Settings tree  (rides along inside the APVTS state, so it travels with
//  the session and with saved presets)
// ===========================================================================
static const juce::Identifier kSettingsTag  { "VSX_SETTINGS" };
static const juce::Identifier kTooltipsProp { "tooltips" };
static const juce::Identifier kMidiMapProp  { "midiMap" };
static const juce::Identifier kLocksProp    { "randomLocks" };
static const juce::Identifier kUiScaleProp  { "uiScale" };

juce::ValueTree VivisectProcessor::settingsTree()
{
    return apvts.state.getOrCreateChildWithName (kSettingsTag, nullptr);
}

bool VivisectProcessor::tooltipsEnabled() const
{
    const auto t = apvts.state.getChildWithName (kSettingsTag);
    return t.isValid() ? (bool) t.getProperty (kTooltipsProp, true) : true;
}
void VivisectProcessor::setTooltipsEnabled (bool on)
{
    settingsTree().setProperty (kTooltipsProp, on, nullptr);
}

float VivisectProcessor::uiScale() const
{
    const auto t = apvts.state.getChildWithName (kSettingsTag);
    return t.isValid() ? (float) t.getProperty (kUiScaleProp, 0.f) : 0.f;
}
void VivisectProcessor::setUiScale (float s)
{
    if (std::abs (uiScale() - s) > 1.0e-3f)
        settingsTree().setProperty (kUiScaleProp, juce::jlimit (0.5f, 1.5f, s), nullptr);
}

juce::ValueTree VivisectProcessor::soundStateSnapshot()
{
    auto state = apvts.copyState();
    const auto settings = state.getChildWithName (kSettingsTag);
    if (settings.isValid())
        state.removeChild (settings, nullptr);
    return state;
}

void VivisectProcessor::restoreSoundState (const juce::ValueTree& source)
{
    if (! source.isValid())
        return;

    auto next = source.createCopy();
    const auto incomingSettings = next.getChildWithName (kSettingsTag);
    if (incomingSettings.isValid())
        next.removeChild (incomingSettings, nullptr);

    const auto currentSettings = apvts.state.getChildWithName (kSettingsTag);
    if (currentSettings.isValid())
        next.addChild (currentSettings.createCopy(), -1, nullptr);

    apvts.replaceState (next);
}

// ===========================================================================
//  Diagnostics — troubleshooting report, crash log, hard reset
// ===========================================================================
namespace
{
    // The crash handler runs in a process that is already falling over, so it
    // touches nothing but a path captured up front and a raw append.
    juce::File g_crashLogPath;
    int g_crashLoggers = 0;      // instances with logging on (message thread only)

    void vivisectCrashHandler (void*)
    {
        if (g_crashLogPath == juce::File())
            return;

        juce::String out;
        out << "\n=== CRASH ===\n"
            << "when: " << juce::Time::getCurrentTime().toString (true, true) << "\n"
            << juce::SystemStats::getStackBacktrace() << "\n";
        g_crashLogPath.appendText (out, false, false, "\n");
    }

    // JUCE can install a crash handler but not remove one (passing nullptr
    // asserts, then leaves a handler that calls through a null pointer). Save
    // whatever the OS had before installing and put exactly that back after.
   #if defined (_WIN32)
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
   #else
    constexpr int kCrashSignals[] { SIGFPE, SIGILL, SIGSEGV, SIGBUS, SIGABRT, SIGSYS };
    struct sigaction g_prevActions[std::size (kCrashSignals)] {};
   #endif

    void installCrashHandler()
    {
       #if defined (_WIN32)
        g_prevFilter = SetUnhandledExceptionFilter (nullptr);
       #else
        for (size_t i = 0; i < std::size (kCrashSignals); ++i)
            sigaction (kCrashSignals[i], nullptr, &g_prevActions[i]);
       #endif
        juce::SystemStats::setApplicationCrashHandler (vivisectCrashHandler);
    }

    void uninstallCrashHandler()
    {
       #if defined (_WIN32)
        SetUnhandledExceptionFilter (g_prevFilter);
        g_prevFilter = nullptr;
       #else
        for (size_t i = 0; i < std::size (kCrashSignals); ++i)
            sigaction (kCrashSignals[i], &g_prevActions[i], nullptr);
       #endif
    }
}

juce::StringArray VivisectProcessor::detectMisconfiguration() const
{
    juce::StringArray notes;

    const auto pvc = [this] (const char* pid) -> float
    {
        if (auto* p = apvts.getRawParameterValue (pid)) return p->load();
        return 0.f;
    };

    int surgeonsOn = 0;
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
        if (pvc (vsx::sid (s, "on").toRawUTF8()) > 0.5f)
            ++surgeonsOn;

    if (surgeonsOn == 0)
    {
        bool everySurgeonLockedOff = true;
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
        {
            const auto onID = vsx::sid (s, "on");
            const auto* parameter = apvts.getParameter (onID);
            if (parameter != nullptr && (parameter->getValue() > 0.5f || ! isParameterLocked (onID)))
                everySurgeonLockedOff = false;
        }

        notes.add (everySurgeonLockedOff
            ? "All surgeons are off and locked. RANDOM will preserve those locks, so no surgeon can be enabled until a lock is removed."
            : "No surgeons are switched on - the plugin will pass audio through untouched.");
    }

    if (pvc (vsx::id::dryWet) < 0.01f)
        notes.add ("DRY / WET is fully dry, so none of the processing is audible.");

    if (pvc (vsx::id::triggerRate) < 0.01f)
        notes.add ("TRIGGER is at zero, so the scheduler will never fire a surgeon.");

    if (pvc (vsx::id::midiMode) > 0.5f)
        notes.add ("MIDI MODE is on: surgeons only fire from incoming MIDI notes C3-F3. "
                   "If the host is not sending MIDI, nothing will happen.");

    if (pvc (vsx::id::panicFreeze) > 0.5f)
        notes.add ("PANIC FREEZE is engaged - the specimen buffer is held and will not update.");

    if (pvc (vsx::id::inputTrim) <= -23.9f)
        notes.add ("IN TRIM is at minimum, so almost nothing is reaching the plugin.");

    if (pvc (vsx::id::outputTrim) <= -23.9f)
        notes.add ("OUT TRIM is at minimum, so almost nothing is leaving the plugin.");

    const int src = (int) pvc (vsx::id::sourceSel);
    if (src == 1 && ! slotHasSample (0))
        notes.add ("SOURCE is set to Sample A but slot A is empty.");
    if (src == 2 && ! slotHasSample (1))
        notes.add ("SOURCE is set to Sample B but slot B is empty.");
    if (src == 3 && ! (slotHasSample (0) && slotHasSample (1)))
        notes.add ("SOURCE is set to Morph A/B but one of the sample slots is empty.");

    return notes;
}

juce::String VivisectProcessor::buildTroubleshootingReport() const
{
    juce::String r;
    const auto nl = [&r] { r << "\n"; };

    r << "VIVISECT TROUBLESHOOTING REPORT\n"
      << "===============================\n"
      << "generated: " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    nl();

    r << "-- PRODUCT ----------------------------------------------------\n"
      << "name        : " << vsx::product::name << "\n"
      << "version     : " << JucePlugin_VersionString << "\n"
      << "vendor      : " << vsx::product::vendor << "\n"
      << "support     : " << vsx::product::support << "\n"
      << "licence     : single-user licence, not for redistribution. "
         "See the LICENSE section of the help page for the full terms.\n"
      << "built on    : " << __DATE__ << " " << __TIME__ << "\n";
    nl();

    r << "-- HOST / DAW -------------------------------------------------\n"
      << "wrapper     : " << juce::AudioProcessor::getWrapperTypeDescription (wrapperType) << "\n";
    if (auto* ph = const_cast<VivisectProcessor*> (this)->getPlayHead())
        if (auto pos = ph->getPosition())
        {
            r << "bpm         : " << (pos->getBpm() ? juce::String (*pos->getBpm(), 3)
                                                    : juce::String ("(host did not say)")) << "\n";
            r << "playing     : " << (pos->getIsPlaying() ? "yes" : "no") << "\n";
        }
    r << "host name   : " << juce::PluginHostType().getHostDescription() << "\n";
    nl();

    r << "-- AUDIO ------------------------------------------------------\n"
      << "sample rate : " << juce::String (getSampleRate(), 1) << " Hz\n"
      << "block size  : " << getBlockSize() << "\n"
      << "in  channels: " << getTotalNumInputChannels() << "\n"
      << "out channels: " << getTotalNumOutputChannels() << "\n"
      << "sidechain   : " << (getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled()
                                ? "connected" : "not connected") << "\n"
      << "latency     : " << getLatencySamples() << " samples\n";
    nl();

    r << "-- MIDI -------------------------------------------------------\n"
      << "accepts midi: " << (acceptsMidi() ? "yes" : "no") << "\n";
    {
        juce::StringArray maps;
        for (int cc = 0; cc < 128; ++cc)
        {
            const int idx = ccToParam[(size_t) cc].load();
            if (idx >= 0 && idx < getParameters().size())
                maps.add ("CC" + juce::String (cc) + " -> " + describeParam (idx));
        }
        r << "cc mappings : " << (maps.isEmpty() ? juce::String ("(none)")
                                                 : maps.joinIntoString (", ")) << "\n";
    }
    nl();

    r << "-- SAMPLE SLOTS -----------------------------------------------\n"
      << "slot A      : " << (slotHasSample (0) ? "loaded" : "empty") << "\n"
      << "slot B      : " << (slotHasSample (1) ? "loaded" : "empty") << "\n";
    nl();

    r << "-- SYSTEM -----------------------------------------------------\n"
      << "os          : " << juce::SystemStats::getOperatingSystemName() << "\n"
      << "cpu         : " << juce::SystemStats::getCpuModel() << "\n"
      << "cpu cores   : " << juce::SystemStats::getNumCpus() << "\n"
      << "memory      : " << juce::SystemStats::getMemorySizeInMegabytes() << " MB\n"
      << "cpu vendor  : " << juce::SystemStats::getCpuVendor() << "\n";
    nl();

    r << "-- LIGHT DIAGNOSTIC -------------------------------------------\n";
    {
        const auto notes = detectMisconfiguration();
        if (notes.isEmpty())
            r << "No obvious misconfiguration found.\n";
        else
            for (const auto& n : notes)
                r << "* " << n << "\n";
    }
    nl();

    r << "-- FOLDERS ----------------------------------------------------\n"
      << "presets     : " << vsx::userPresetDir().getFullPathName() << "\n"
      << "logs        : " << vsx::userLogDir().getFullPathName() << "\n"
      << "cache       : " << vsx::userCacheDir().getFullPathName() << "\n";
    nl();

    r << "-- CURRENT SETTINGS -------------------------------------------\n";
    for (auto* p : getParameters())
        if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            r << wid->paramID.paddedRight (' ', 18) << " = "
              << p->getCurrentValueAsText() << "   [norm " << juce::String (p->getValue(), 4) << "]\n";
    nl();

    return r;
}

juce::File VivisectProcessor::suggestedTroubleshootingFile() const
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    return vsx::userLogDir().getChildFile ("Vivisect troubleshooting " + stamp + ".txt");
}

bool VivisectProcessor::writeTroubleshootingFile (const juce::File& f) const
{
    f.getParentDirectory().createDirectory();
    return f.replaceWithText (buildTroubleshootingReport());
}

void VivisectProcessor::setCrashLogEnabled (bool on)
{
    if (on == crashLogOn)
        return;

    if (! on)
    {
        // Write the closing line BEFORE clearing the flag: appendToCrashLog
        // checks it and would otherwise drop this silently.
        appendToCrashLog ("--- crash logging switched off by the user at "
                          + juce::Time::getCurrentTime().toString (true, true) + " ---");
        crashLogOn = false;
        // Several instances can log at once; the handler goes only when the
        // last of them switches off.
        if (--g_crashLoggers <= 0)
        {
            g_crashLoggers = 0;
            g_crashLogPath = juce::File();
            uninstallCrashHandler();
        }
        crashLogFile = juce::File();
        return;
    }

    crashLogOn = true;

    auto dir = vsx::userLogDir();
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    crashLogFile = dir.getChildFile ("Vivisect crash log " + stamp + ".txt");

    // include.md: a copy of the troubleshooting report goes at the START of the
    // crash log, so one file is enough to see the state the plugin was in.
    crashLogFile.replaceWithText (buildTroubleshootingReport());
    crashLogFile.appendText ("\n-- LIVE LOG ---------------------------------------------------\n"
                             "Everything below happened after logging was switched on.\n\n",
                             false, false, "\n");

    // Another instance may already own the handler; then just retarget the
    // path rather than saving JUCE's handler as the "previous" one.
    g_crashLogPath = crashLogFile;
    if (g_crashLoggers++ == 0)
        installCrashHandler();
}

void VivisectProcessor::appendToCrashLog (const juce::String& line)
{
    if (! crashLogOn || crashLogFile == juce::File())
        return;
    crashLogFile.appendText (line + "\n", false, false, "\n");
}

void VivisectProcessor::hardResetAllSettings()
{
    // 1. every parameter back to its default
    resetAllToDefaults();

    // 2. drop MIDI learn mappings
    for (auto& c : ccToParam) c.store (-1);
    learnParamIndex.store (-1);
    midiMapDirty.store (true);

    // 3. drop the settings subtree (tool tips and friends) so it rebuilds
    //    from defaults rather than carrying a bad value forward
    apvts.state.removeChild (apvts.state.getChildWithName (kSettingsTag), nullptr);
    parameterLocks.clear();

    // Rebuild documented defaults immediately. A hard reset should leave the
    // plugin usable now, not only after the next state reload.
    seedDefaultMidiMap();

    // 4. forget loaded samples and the A/B snapshots
    clearSampleSlot (0);
    clearSampleSlot (1);
    abState[0] = {};
    abState[1] = {};

    // 5. stop crash logging and wipe the cache folder. Presets are the user's
    //    own work and are deliberately NOT touched.
    setCrashLogEnabled (false);
    auto cache = vsx::userCacheDir();
    if (cache.isDirectory())
        cache.deleteRecursively();

    lastPresetFile = juce::File();
}

// ===========================================================================
//  MIDI learn
// ===========================================================================
int VivisectProcessor::paramIndexFor (const juce::String& paramID) const
{
    if (auto* p = apvts.getParameter (paramID)) return p->getParameterIndex();
    return -1;
}

void VivisectProcessor::seedDefaultMidiMap()
{
    // documented defaults: CC20 Macro1, CC21 Macro2, CC22 Chaos, CC23 Trigger
    const std::pair<int, const char*> def[] {
        { 20, id::macro1 }, { 21, id::macro2 }, { 22, id::chaos }, { 23, id::triggerRate } };
    for (auto& d : def)
    {
        const int idx = paramIndexFor (d.second);
        if (idx >= 0) ccToParam[(size_t) d.first].store (idx);
    }
    writeMidiMapToState();
}

void VivisectProcessor::writeMidiMapToState()
{
    juce::StringArray entries;
    for (int cc = 0; cc < 128; ++cc)
    {
        const int idx = ccToParam[(size_t) cc].load();
        if (idx < 0 || idx >= getParameters().size()) continue;
        if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (getParameters()[idx]))
            entries.add (juce::String (cc) + ":" + wid->paramID);
    }
    settingsTree().setProperty (kMidiMapProp, entries.joinIntoString (";"), nullptr);
}

void VivisectProcessor::syncFromStateTree()
{
    for (auto& c : ccToParam) c.store (-1);

    const auto t = apvts.state.getChildWithName (kSettingsTag);
    parameterLocks.clear();
    juce::StringArray locks;
    if (t.isValid())
        locks.addTokens (t.getProperty (kLocksProp).toString(), ";", "");
    for (const auto& pid : locks)
        if (apvts.getParameter (pid) != nullptr && ! parameterLocks.contains (pid))
            parameterLocks.add (pid);

    if (! t.isValid() || ! t.hasProperty (kMidiMapProp))
    {
        seedDefaultMidiMap();
        return;
    }

    juce::StringArray entries;
    entries.addTokens (t.getProperty (kMidiMapProp).toString(), ";", "");
    for (const auto& e : entries)
    {
        const int cc = e.upToFirstOccurrenceOf (":", false, false).getIntValue();
        const auto pid = e.fromFirstOccurrenceOf (":", false, false);
        const int idx = paramIndexFor (pid);
        if (idx >= 0 && cc >= 0 && cc < 128) ccToParam[(size_t) cc].store (idx);
    }

}

void VivisectProcessor::beginMidiLearn (const juce::String& paramID)
{
    learnParamIndex.store (paramIndexFor (paramID));
}
void VivisectProcessor::cancelMidiLearn() { learnParamIndex.store (-1); }

juce::String VivisectProcessor::midiLearnTarget() const
{
    const int idx = learnParamIndex.load();
    if (idx < 0 || idx >= getParameters().size()) return {};
    if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (getParameters()[idx]))
        return wid->paramID;
    return {};
}

int VivisectProcessor::getCCForParam (const juce::String& paramID) const
{
    const int idx = paramIndexFor (paramID);
    if (idx < 0) return -1;
    for (int cc = 0; cc < 128; ++cc)
        if (ccToParam[(size_t) cc].load() == idx) return cc;
    return -1;
}
void VivisectProcessor::setCCForParam (const juce::String& paramID, int cc)
{
    const int idx = paramIndexFor (paramID);
    if (idx < 0) return;
    for (auto& slot : ccToParam)
        if (slot.load() == idx) slot.store (-1);
    if (cc >= 0 && cc < 128) ccToParam[(size_t) cc].store (idx);
    writeMidiMapToState();
}
void VivisectProcessor::clearCCForParam (const juce::String& paramID) { setCCForParam (paramID, -1); }

void VivisectProcessor::writeParameterLocksToState()
{
    settingsTree().setProperty (kLocksProp, parameterLocks.joinIntoString (";"), nullptr);
}

void VivisectProcessor::setParameterLocked (const juce::String& paramID, bool locked)
{
    if (apvts.getParameter (paramID) == nullptr)
        return;

    if (locked)
    {
        if (! parameterLocks.contains (paramID))
            parameterLocks.add (paramID);
    }
    else
    {
        parameterLocks.removeString (paramID);
    }
    writeParameterLocksToState();
}

bool VivisectProcessor::isParameterLocked (const juce::String& paramID) const
{
    return parameterLocks.contains (paramID);
}

void VivisectProcessor::clearParameterLocks()
{
    parameterLocks.clear();
    writeParameterLocksToState();
}

// ===========================================================================
//  Reset + randomise + mutate
// ===========================================================================
void VivisectProcessor::resetAllToDefaults()
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
    randomisedOnce = false;
}

void VivisectProcessor::randomizeAll()
{
    // Utility and routing state are not part of the generated sound. Preserve
    // them when subsequent RANDOM presses reset the creative parameters first.
    std::map<juce::String, float> preserved;
    if (randomisedOnce)
    {
        for (const auto& pid : { id::inputTrim, id::outputTrim, id::bufferBars,
                                 id::sourceSel, id::midiMode, id::panicFreeze,
                                 id::decayArm, id::decayTime, id::flatOn,
                                 id::flatTone, id::flatBleed, id::flatMix })
            if (auto* p = apvts.getParameter (pid))
                preserved[pid] = p->getValue();

        for (const auto& pid : parameterLocks)
            if (auto* p = apvts.getParameter (pid))
                preserved[pid] = p->getValue();
        if (auto* p = apvts.getParameter (id::mutationAmount))
            preserved[id::mutationAmount] = p->getValue();

        resetAllToDefaults();

        for (const auto& kv : preserved)
            if (auto* p = apvts.getParameter (kv.first))
                p->setValueNotifyingHost (kv.second);
    }
    randomisedOnce = true;

    auto setNorm = [this] (const juce::String& pid, float v)
    {
        if (isParameterLocked (pid) || pid == id::mutationAmount) return;
        if (auto* p = apvts.getParameter (pid))
            p->setValueNotifyingHost (juce::jlimit (0.f, 1.f, v));
    };
    auto setChoice = [this] (const juce::String& pid, int idx)
    {
        if (isParameterLocked (pid)) return;
        if (auto* p = apvts.getParameter (pid))
            p->setValueNotifyingHost (
                juce::jlimit (0.f, 1.f, p->getNormalisableRange().convertTo0to1 ((float) idx)));
    };
    auto rr = [this] (float lo, float hi) { return lo + rng.nextFloat() * (hi - lo); };

    setNorm   (id::chaos,          rr (0.05f, 0.95f));
    setChoice (id::gravityGrid,    rng.nextInt (5));
    setNorm   (id::gravityPull,    rr (0.f, 1.f));
    setNorm   (id::swing,          rr (0.35f, 0.75f));
    setNorm   (id::dryWet,         rr (0.55f, 1.f));
    setNorm   (id::triggerRate,    rr (0.12f, 0.80f));
    setNorm   (id::reinject,       rng.nextFloat() < 0.4f ? rr (0.05f, 0.45f) : 0.f);
    setNorm   (id::analysisInform, rr (0.f, 1.f));
    setNorm   (id::morph,          rr (0.f, 1.f));
    setNorm   (id::scAmount,       rr (0.f, 1.f));
    setNorm   (id::scarOn,         rng.nextFloat() < 0.35f ? 1.f : 0.f);
    setNorm   (id::scarDrive,      rr (0.08f, 0.82f));
    setNorm   (id::scarMix,        rr (0.15f, 0.72f));

    bool anyOn = false;
    juce::Array<int> eligibleOn;
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        const auto onID = sid (s, "on");
        if (isParameterLocked (onID))
        {
            if (auto* p = apvts.getParameter (onID))
                anyOn = anyOn || p->getValue() > 0.5f;
        }
        else
        {
            eligibleOn.add (s);
            const bool on = rng.nextFloat() < 0.5f;
            anyOn = anyOn || on;
            setNorm (onID, on ? 1.f : 0.f);
        }

        setNorm   (sid (s, "mix"),  rr (0.45f, 1.f));
        setNorm   (sid (s, "prob"), rr (0.20f, 0.90f));
        setNorm   (sid (s, "p1"),   rr (0.f, 1.f));
        setNorm   (sid (s, "p2"),   rr (0.f, 1.f));
        setNorm   (sid (s, "p3"),   rr (0.f, 1.f));
        setChoice (sid (s, "route"), rng.nextFloat() < 0.18f ? 1 + rng.nextInt (kNumSurgeons) : 0);
    }
    if (! anyOn && ! eligibleOn.isEmpty())
        setNorm (sid (eligibleOn[rng.nextInt (eligibleOn.size())], "on"), 1.f);

    setNorm   (id::lfo1Rate, rr (0.f, 1.f));
    setNorm   (id::lfo2Rate, rr (0.f, 1.f));
    setChoice (id::lfo1Shape, rng.nextInt (5));
    setChoice (id::lfo2Shape, rng.nextInt (5));
    setNorm   (id::macro1, rr (0.f, 1.f));
    setNorm   (id::macro2, rr (0.f, 1.f));

    const int nSrc = modSourceNames().size(), nDst = modDestNames().size();
    for (int m = 0; m < kNumModSlots; ++m)
    {
        const juce::String pre = "mm" + juce::String (m + 1) + "_";
        const bool live = rng.nextFloat() < 0.5f;
        setChoice (pre + "src", live ? 1 + rng.nextInt (nSrc - 1) : 0);
        setChoice (pre + "dst", live ? 1 + rng.nextInt (nDst - 1) : 0);
        setNorm   (pre + "depth", live ? rr (0.1f, 0.9f) : 0.5f);
    }
}

void VivisectProcessor::mutateCurrent()
{
    auto* amountParam = apvts.getParameter (id::mutationAmount);
    if (amountParam == nullptr) return;
    const float amount = juce::jlimit (0.f, 1.f, amountParam->getValue());
    if (amount <= 0.f) return;

    auto mutateContinuous = [this, amount] (const juce::String& pid, float scale = 0.35f)
    {
        if (isParameterLocked (pid)) return;
        if (auto* p = apvts.getParameter (pid))
        {
            const float delta = (rng.nextFloat() * 2.f - 1.f) * amount * scale;
            p->setValueNotifyingHost (juce::jlimit (0.f, 1.f, p->getValue() + delta));
        }
    };

    auto mutateChoice = [this, amount] (const juce::String& pid)
    {
        if (amount < 0.35f || isParameterLocked (pid)) return;
        const float chance = juce::jlimit (0.f, 0.30f, (amount - 0.35f) * 0.45f);
        if (rng.nextFloat() >= chance) return;

        if (auto* p = apvts.getParameter (pid))
        {
            const auto range = p->getNormalisableRange();
            const float actual = range.convertFrom0to1 (p->getValue());
            const float step = range.interval > 0.f ? range.interval : 1.f;
            const float moved = juce::jlimit (range.start, range.end,
                                               actual + (rng.nextBool() ? step : -step));
            p->setValueNotifyingHost (range.convertTo0to1 (moved));
        }
    };

    for (auto pid : { id::chaos, id::gravityPull, id::swing, id::dryWet,
                      id::triggerRate, id::reinject, id::analysisInform, id::morph,
                      id::scAmount, id::scarDrive, id::scarMix, id::lfo1Rate,
                      id::lfo2Rate, id::macro1, id::macro2 })
        mutateContinuous (pid);

    mutateChoice (id::gravityGrid);
    mutateChoice (id::lfo1Shape);
    mutateChoice (id::lfo2Shape);

    bool anyOn = false;
    juce::Array<int> eligibleOn;
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        const auto onID = sid (s, "on");
        if (! isParameterLocked (onID))
        {
            eligibleOn.add (s);
            if (amount >= 0.55f && rng.nextFloat() < (amount - 0.55f) * 0.22f)
                if (auto* p = apvts.getParameter (onID))
                    p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.f : 1.f);
        }

        if (auto* p = apvts.getParameter (onID))
            anyOn = anyOn || p->getValue() > 0.5f;

        mutateContinuous (sid (s, "mix"));
        mutateContinuous (sid (s, "prob"));
        mutateContinuous (sid (s, "p1"));
        mutateContinuous (sid (s, "p2"));
        mutateContinuous (sid (s, "p3"));
        mutateChoice (sid (s, "route"));
    }

    if (! anyOn && ! eligibleOn.isEmpty())
        if (auto* p = apvts.getParameter (sid (eligibleOn[rng.nextInt (eligibleOn.size())], "on")))
            p->setValueNotifyingHost (1.f);

    for (int m = 0; m < kNumModSlots; ++m)
    {
        const juce::String pre = "mm" + juce::String (m + 1) + "_";
        mutateChoice (pre + "src");
        mutateChoice (pre + "dst");
        mutateContinuous (pre + "depth");
    }

    if (amount >= 0.70f && ! isParameterLocked (id::scarOn)
        && rng.nextFloat() < (amount - 0.70f) * 0.30f)
        if (auto* p = apvts.getParameter (id::scarOn))
            p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.f : 1.f);
}

// ===========================================================================
//  A / B compare
// ===========================================================================
void VivisectProcessor::selectABSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == abSlot) return;

    abState[abSlot] = soundStateSnapshot();        // park sonic state only
    if (abState[slot].isValid())
    {
        restoreSoundState (abState[slot]);
    }
    else
    {
        abState[slot] = soundStateSnapshot();      // first visit: seed from current
    }
    abSlot = slot;
}
void VivisectProcessor::copyABSlot()
{
    abState[1 - abSlot] = soundStateSnapshot();
}

// ===========================================================================
//  Preset files
// ===========================================================================
bool VivisectProcessor::savePresetToFile (const juce::File& f)
{
    if (f == juce::File()) return false;
    f.getParentDirectory().createDirectory();
    if (auto xml = soundStateSnapshot().createXml())
        if (xml->writeTo (f))
        {
            lastPresetFile = f;
            return true;
        }
    return false;
}
bool VivisectProcessor::loadPresetFromFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return false;
    restoreSoundState (juce::ValueTree::fromXml (*xml));
    lastPresetFile = f;
    return true;
}

// ===========================================================================
//  Export : write the current specimen buffer out as a wav.
//  Vivisect is an effect, so this is the "special function" export -- the
//  specimen itself, exactly as the surgeons currently see it.
// ===========================================================================
bool VivisectProcessor::exportSpecimenToWav (const juce::File& f, int bitDepth,
                                                   double* secondsWritten)
{
    if (secondsWritten != nullptr)
        *secondsWritten = 0.0;

    const int cap = specimen.capacitySamples();
    if (cap <= 1) return false;

    // WAV export supports the common PCM depths exposed by the UI. Rejecting
    // anything else keeps callers from silently getting a different quality.
    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32)
        return false;

    suspendProcessing (true);
    const double sr = specimen.getSampleRate();
    const juce::int64 wp = specimen.writePos();
    const int n = (int) juce::jmin ((juce::int64) cap, wp);
    const juce::int64 start = wp - n;

    juce::AudioBuffer<float> out;
    if (n > 0)
    {
        out.setSize (2, n);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = out.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                d[i] = specimen.readInterp (ch, (double) (start + i));
        }
    }
    suspendProcessing (false);

    if (n <= 0) return false;

    f.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr || ! os->openedOk()) return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, bitDepth, {}, 0));
    if (w == nullptr) return false;
    os.release();                                  // the writer owns the stream now

    const bool ok = w->writeFromAudioSampleBuffer (out, 0, n);
    if (ok && secondsWritten != nullptr)
        *secondsWritten = (double) n / juce::jmax (1.0, sr);
    return ok;
}

juce::AudioProcessorEditor* VivisectProcessor::createEditor() { return new VivisectEditor (*this); }

// ===========================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VivisectProcessor(); }
