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

// A session, preset, A/B slot or history frame saved by an older version has no entry
// for the 0.10.0 parameters (effects, Scale Lock, Dose, Route Amt). They must come back
// at their defaults even when this instance currently holds other values, because
// replaceState leaves absent parameters untouched.
static void defaultAbsentNewParams (juce::AudioProcessorValueTreeState& apvts,
                                    const juce::ValueTree& incoming)
{
    juce::StringArray wanted;
    for (const char* e : effectsParamIds()) wanted.add (e);
    for (const char* m : { id::scaleOn, id::scaleRoot, id::scaleType, id::scaleRange, id::dose }) wanted.add (m);
    for (int s = 0; s < kNumSurgeons; ++s) wanted.add (routeAmtRaw (s));

    for (const auto& child : incoming)
        if (child.hasType ("PARAM"))
            wanted.removeString (child.getProperty ("id").toString());

    for (const auto& pid : wanted)
        if (auto* rp = apvts.getParameter (pid))
            rp->setValueNotifyingHost (rp->getDefaultValue());
}

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

    // ==== EFFECTS (E1/E2) ====
    fxTqOn = fxRef (id::tqOn);           fxTqMode = fxRef (id::tqMode);
    fxTqCutoff = fxRef (id::tqCutoff);   fxTqRes = fxRef (id::tqRes);
    fxTqEnv = fxRef (id::tqEnv);         fxTqMix = fxRef (id::tqMix);
    fxClotOn = fxRef (id::clotOn);       fxClotTime = fxRef (id::clotTime);
    fxClotFree = fxRef (id::clotFree);   fxClotFeedback = fxRef (id::clotFeedback);
    fxClotTone = fxRef (id::clotTone);   fxClotSpread = fxRef (id::clotSpread);
    fxClotMix = fxRef (id::clotMix);
    // ==== END EFFECTS (E1/E2) ====
    capture.setDirectory (vsx::userCapturesDir());
    for (auto* prm : getParameters())
        prm->addListener (&undoListener);       // F1: UI gestures coalesce into one undo step
    setSimpleViewEnabled (true);        // MUSICAL (F4): fresh instance = fresh install default
}

// ==== EFFECTS (E1/E2) ====
VivisectProcessor::FxRef VivisectProcessor::fxRef (const char* paramID) const
{
    FxRef r;
    r.par = apvts.getParameter (paramID);
    r.raw = apvts.getRawParameterValue (paramID);
    return r;
}

// Base value plus the mod matrix offset, in the parameter's normalized space
// (same behaviour as pvMod, without any string lookup).
float VivisectProcessor::fxMod (const FxRef& r, int destEnum) const noexcept
{
    if (r.par == nullptr || r.raw == nullptr) return r.get();
    const float base = r.par->convertTo0to1 (r.raw->load());
    return r.par->convertFrom0to1 (juce::jlimit (0.f, 1.f, base + modMatrix.destOffset (destEnum)));
}
// ==== END EFFECTS (E1/E2) ====
VivisectProcessor::~VivisectProcessor()
{
    for (auto* prm : getParameters())
        prm->removeListener (&undoListener);
    stopTimer();
    capture.shutdown();                         // F3: join the writer before anything it reads goes away
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
    block = juce::jmax (1, block);
    preparedBlock = block;
    scar.reset();
    scarWasOn = false;
    // ==== EFFECTS (E1/E2) ====
    tourniquet.prepare (sr);
    tourniquetWasOn = false;
    clot.prepare (sr);
    clotWasOn = false;
    // ==== END EFFECTS (E1/E2) ====
    flatline.prepare (sr, block);
    sampleRate = sr;
    capture.prepare (sr);                       // F3: allocate the take buffer here, never on the audio thread
    specimen.prepare (sr, 45.0);
    keyBuf.assign ((size_t) kKeyWindow, 0.f);
    keyState.store (0);
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
                                          (int) pv (id::sourceSel), pv (id::morph),
                                          doseUp (pv (id::chaos), pv (id::dose), dosek::chaos),   // MUSICAL (F4)
                                          60.0 / lastBpm * sampleRate, specimen, analysis,
                                          bufferRegionSamples (60.0 / lastBpm * sampleRate));
    rack[i].trigger (c);
    telem.pushFire (i, (float) std::fmod (c.sourcePos, 1000.0) / 1000.f,
                    (float) (c.lengthSamples * 1000.0 / juce::jmax (1.0, sampleRate)));
}

void VivisectProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Hosts may send more samples than prepareToPlay promised (offline render,
    // variable-size hosts). Every internal buffer is sized to the prepared
    // block, so split oversized blocks instead of overrunning them. MIDI is
    // handled per block (event positions are not used), so it goes to the
    // first chunk.
    const int total = buffer.getNumSamples();
    if (preparedBlock > 0 && total > preparedBlock)
    {
        for (int off = 0; off < total; off += preparedBlock)
        {
            const int len = juce::jmin (preparedBlock, total - off);
            juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), off, len);
            chunkSampleOffset = off;
            processChunk (chunk, off == 0 ? midi : emptyMidi);
            emptyMidi.clear();
        }
        chunkSampleOffset = 0;
        return;
    }
    chunkSampleOffset = 0;
    processChunk (buffer, midi);
}

void VivisectProcessor::processChunk (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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
    // One NaN/Inf from upstream would otherwise live forever in the specimen
    // ring, the reverb tails and the envelope follower.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = workBuf.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (d[i])) d[i] = 0.f;
    }
    dryBuf.copyFrom (0, 0, workBuf, 0, 0, n);
    dryBuf.copyFrom (1, 0, workBuf, 1, 0, n);

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
    bool hostBpm = false;
    bool playing = false;
    double barQuarters = 4.0;                       // capture: bar length in quarter notes
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (pos->getBpm())         { bpm = *pos->getBpm(); hostBpm = true; }
            if (pos->getPpqPosition()) ppq = *pos->getPpqPosition();
            playing = pos->getIsPlaying();
            if (auto ts = pos->getTimeSignature())
                barQuarters = vsx::capture::barQuarters (ts->numerator, ts->denominator);
        }
    if (! hostBpm)                                  // H1: tapped tempo when the host gives none
    {
        const double fb = tempoFallbackBpm.load (std::memory_order_relaxed);
        if (fb > 0.0) bpm = fb;
        else if (! hostTempoSeen.load (std::memory_order_relaxed)) bpm = 120.0;   // never had a host tempo: default
    }
    bpm = juce::jlimit (20.0, 300.0, bpm);
    lastBpm = bpm;
    shownBpm.store ((float) bpm, std::memory_order_relaxed);
    hostTempoSeen.store (hostBpm, std::memory_order_relaxed);
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
    // ==== MUSICAL (F4) ==== DOSE is applied after modulation; identity at 0.
    const float dose = pv (id::dose);
    float chaos = juce::jmax (doseUp (pvMod (id::chaos, 1), dose, dosek::chaos), decayPhase);
    const float gPull   = doseDown (pvMod (id::gravityPull, 2), dose, dosek::gravityPull);
    const float swing   = pvMod (id::swing, 3);
    const int   grid    = (int) pv (id::gravityGrid);
    const float trigRate = doseUp (pvMod (id::triggerRate, 4), dose, dosek::triggerRate);
    float reinject = juce::jmax (doseUp (pvMod (id::reinject, 5), dose, dosek::reinject), decayPhase * 0.7f);
    // ==== end MUSICAL (F4) ====
    reinject = juce::jmin (1.f, reinject + deriveChaos (chaos).feedback * 0.4f);
    const float analysisInform = pv (id::analysisInform);
    const int   sourceSel = (int) pv (id::sourceSel);
    const float morph = pvMod (id::morph, 6);
    const bool  midiMode = pv (id::midiMode) > 0.5f;
    const bool  panic = pv (id::panicFreeze) > 0.5f;

    // ==== MUSICAL (F2) ====
    ScaleLock scaleLock;
    scaleLock.on    = pv (id::scaleOn) > 0.5f;
    scaleLock.root  = juce::jlimit (0, 11, (int) pv (id::scaleRoot));
    scaleLock.type  = juce::jlimit (0, kNumScaleTypes - 1, (int) pv (id::scaleType));
    scaleLock.range = pv (id::scaleRange);
    // ==== end MUSICAL (F2) ====

    std::array<bool,  kNumSurgeons> on {};
    std::array<float, kNumSurgeons> mixA {}, probA {}, p1A {};
    std::array<int,   kNumSurgeons> routeA {};
    std::array<float, kNumSurgeons> routeAmtA {};   // G1
    for (int s = 0; s < kNumSurgeons; ++s)
    {
        on[(size_t) s]     = pv (sidRaw (s, SP_ON)) > 0.5f;
        mixA[(size_t) s]   = pv (sidRaw (s, SP_MIX));
        probA[(size_t) s]  = pv (sidRaw (s, SP_PROB));
        p1A[(size_t) s]    = pv (sidRaw (s, SP_P1));
        routeA[(size_t) s] = (int) pv (sidRaw (s, SP_ROUTE));
        routeAmtA[(size_t) s] = pv (routeAmtRaw (s));   // G1

        float q1 = p1A[(size_t) s];
        float q2 = pv (sidRaw (s, SP_P2));
        float q3 = pv (sidRaw (s, SP_P3));
        if (s == S_STUTTER)  q3 = pvMod (sidRaw (0, SP_P3), 7);
        if (s == S_GRANULAR) { q2 = pvMod (sidRaw (1, SP_P2), 8); q3 = pvMod (sidRaw (1, SP_P3), 9); }
        // MUSICAL (F4): fewer bits = crunchier, so DOSE pulls BITS down
        if (s == S_CORRUPT)  { q1 = doseDown (pvMod (sidRaw (3, SP_P1), 10), dose, dosek::corruptBits);
                               q2 = pvMod (sidRaw (3, SP_P2), 11); }
        if (s == S_FREEZE)   q3 = pvMod (sidRaw (5, SP_P3), 12);
        rack[s].setParams (q1, q2, q3);
        rack[s].setScale (scaleLock);                       // MUSICAL (F2)
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
    // workBuf is sized for the prepared block; hosts may send fewer samples.
    // View exactly this block's n samples (no allocation: external data).
    const juce::AudioBuffer<float> inBlock (workBuf.getArrayOfWritePointers(), 2, n);
    const i64 before = specimen.writePos();
    specimen.push (inBlock);
    if (keyState.load (std::memory_order_acquire) == 1) captureKeyWindow();   // H2
    analysis.transients.process (inBlock, before);
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
    rack.process (specimen, wetBuf, n, mixA, routeA, routeAmtA, reinject);
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
        scar.process (workBuf, n, doseUp (pv (id::scarDrive), dose, dosek::scarDrive),   // MUSICAL (F4)
                      pv (id::scarMix));
        scarWasOn = true;
    }
    else if (scarWasOn)
    {
        scar.reset();
        scarWasOn = false;
    }

    // ==== EFFECTS (E1/E2) ====
    // ---- TOURNIQUET (resonant filter) ---------------------------------
    if (fxTqOn.get() > 0.5f)
    {
        tourniquet.process (workBuf, n, (int) fxTqMode.get(), fxMod (fxTqCutoff, MD_TQ_CUTOFF),
                            fxMod (fxTqRes, MD_TQ_RES), fxTqEnv.get(), fxTqMix.get());
        tourniquetWasOn = true;
    }
    else if (tourniquetWasOn)
    {
        tourniquet.reset();
        tourniquetWasOn = false;
    }

    // ---- CLOT (tempo-synced stereo delay) -----------------------------
    if (fxClotOn.get() > 0.5f)
    {
        clot.process (workBuf, n, spb, (int) fxClotTime.get(), fxClotFree.get(),
                      fxMod (fxClotFeedback, MD_CLOT_FB), fxClotTone.get(), fxClotSpread.get(),
                      fxMod (fxClotMix, MD_CLOT_MIX));
        clotWasOn = true;
    }
    else if (clotWasOn)
    {
        clot.reset();
        clotWasOn = false;
    }
    // ==== END EFFECTS (E1/E2) ====

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

    // ---- capture (F3): copy only; the file is written by a background thread
    if (! isSuspended())
    {
        vsx::CaptureEngine::Clock cc;
        cc.playing = playing;
        cc.bpm = bpm;
        cc.barQuarters = barQuarters;
        cc.ppq = ppq + (double) chunkSampleOffset / spb;
        capture.process (main.getReadPointer (0), main.getReadPointer (mainCh > 1 ? 1 : 0), n, cc);
    }

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
            UndoScope undoStep (*this);                 // F1: one step per rewind
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
                              "Warp 1995", "Modern Hyperpop", "SOPHIE-adjacent",
                              // 0.10.0: showcase the new stages
                              "Tourniquet Sweep", "Clot Dub Smear", "Minor Key Shrapnel",
                              "Dose Ramp", "Feedback Ward", "Notch Ghost",
                              "Telephone Gate", "Whole Tone Haze" };
    jassert (names.size() == numFactoryPresets);
    for (const auto& f : getUserPresetFiles())
        names.add ("* " + f.getFileNameWithoutExtension());
    return names;
}
void VivisectProcessor::applyPresetMap (const std::map<juce::String, float>& m)
{
    UndoScope undoStep (*this);                         // F1: one step per preset load
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
    // Real (un-normalised) value, for the 0.10.0 presets: Hz, ms, choice index.
    auto real = [&] (const juce::String& k, float v)
    {
        if (auto* p = apvts.getParameter (k)) m[k] = p->convertTo0to1 (v);
    };
    auto only = [&] (std::initializer_list<int> on)       // surgeons on, everything else off
    {
        for (int s = 0; s < kNumSurgeons; ++s) set (S (s, "on"), 0.f);
        for (int s : on) set (S (s, "on"), 1.f);
    };

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
        case 6: // Tourniquet Sweep - low-pass that opens on every hit
            set (id::chaos, 0.38f); set (id::gravityPull, 0.8f); set (id::dryWet, 0.85f);
            set (id::triggerRate, 0.5f);
            only ({ S_STUTTER, S_GRANULAR });
            set (S (S_STUTTER, "mix"), 0.9f); set (S (S_STUTTER, "p2"), 0.45f);
            set (S (S_GRANULAR, "mix"), 0.5f); set (S (S_GRANULAR, "prob"), 0.35f);
            set (id::tqOn, 1.f); real (id::tqMode, 0); real (id::tqCutoff, 650.f);
            set (id::tqRes, 0.6f); set (id::tqEnv, 0.75f); set (id::tqMix, 1.f);
            real (id::outputTrim, -7.5f);   // level-matched to the other presets
            break;
        case 7: // Clot Dub Smear - dotted-quarter echoes smeared over reversed slices
            set (id::chaos, 0.32f); set (id::gravityPull, 0.7f); set (id::dryWet, 0.7f);
            set (id::triggerRate, 0.3f); set (id::reinject, 0.15f);
            only ({ S_REVERSE, S_GRANULAR, S_FREEZE });
            set (S (S_REVERSE, "mix"), 0.8f); set (S (S_REVERSE, "prob"), 0.45f); set (S (S_REVERSE, "p3"), 0.7f);
            set (S (S_GRANULAR, "mix"), 0.45f); set (S (S_GRANULAR, "prob"), 0.3f);
            set (S (S_FREEZE, "mix"), 0.4f); set (S (S_FREEZE, "prob"), 0.15f);
            set (id::clotOn, 1.f); real (id::clotTime, 5); set (id::clotFeedback, 0.62f);
            real (id::clotTone, 2800.f); set (id::clotSpread, 0.9f); set (id::clotMix, 0.42f);
            set (id::tqOn, 1.f); real (id::tqMode, 0); real (id::tqCutoff, 5200.f); set (id::tqRes, 0.15f);
            real (id::outputTrim, -3.0f);   // level-matched to the other presets
            break;
        case 8: // Minor Key Shrapnel - A minor pentatonic scatter, stays in tune
            set (id::chaos, 0.5f); set (id::gravityPull, 0.75f); set (id::dryWet, 0.8f);
            set (id::triggerRate, 0.5f);
            only ({ S_STUTTER, S_GRANULAR, S_REVERSE });
            set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "prob"), 0.65f);
            set (S (S_STUTTER, "p2"), 0.7f); set (S (S_STUTTER, "p3"), 0.9f);
            set (S (S_GRANULAR, "mix"), 0.6f); set (S (S_GRANULAR, "p1"), 0.2f); set (S (S_GRANULAR, "p2"), 0.65f);
            set (S (S_REVERSE, "mix"), 0.45f); set (S (S_REVERSE, "prob"), 0.3f); set (S (S_REVERSE, "p3"), 0.8f);
            set (id::scaleOn, 1.f); real (id::scaleRoot, 9); real (id::scaleType, 9); set (id::scaleRange, 0.75f);
            real (id::outputTrim, -2.5f);   // level-matched: in-key first repeats made the stutter hotter
            break;
        case 9: // Dose Ramp - tame at zero, nasty at full; ride the DOSE knob
            set (id::chaos, 0.12f); set (id::gravityPull, 0.9f); set (id::dryWet, 0.85f);
            set (id::triggerRate, 0.15f); set (id::reinject, 0.05f);
            only ({ S_STUTTER, S_CORRUPT, S_REORDER });
            set (S (S_STUTTER, "mix"), 0.9f); set (S (S_STUTTER, "p2"), 0.3f);
            set (S (S_CORRUPT, "mix"), 0.6f); set (S (S_CORRUPT, "prob"), 0.35f); set (S (S_CORRUPT, "p1"), 0.3f);
            set (S (S_REORDER, "mix"), 0.7f); set (S (S_REORDER, "prob"), 0.3f);
            set (id::scarOn, 1.f); set (id::scarDrive, 0.3f); set (id::scarMix, 0.4f);
            set (id::dose, 0.45f);
            real (id::outputTrim, -2.0f);   // level-matched to the other presets
            break;
        case 10: // Feedback Ward - surgeons feed each other in a ring, kept bounded by Route Amt
            set (id::chaos, 0.42f); set (id::gravityPull, 0.65f); set (id::dryWet, 0.88f);
            set (id::triggerRate, 0.4f); set (id::reinject, 0.2f);
            only ({ S_STUTTER, S_GRANULAR, S_REORDER });
            set (S (S_STUTTER, "mix"), 0.8f);
            set (S (S_GRANULAR, "mix"), 0.55f); set (S (S_GRANULAR, "prob"), 0.4f);
            set (S (S_REORDER, "mix"), 0.7f); set (S (S_REORDER, "prob"), 0.4f);
            real (S (S_GRANULAR, "route"), 1); real (S (S_GRANULAR, "routeAmt"), 0.55f);
            real (S (S_REORDER, "route"), 2);  real (S (S_REORDER, "routeAmt"), 0.7f);
            real (S (S_STUTTER, "route"), 5);  real (S (S_STUTTER, "routeAmt"), 0.35f);
            break;
        case 11: // Notch Ghost - sparse freeze pads with a swept notch and triplet echoes
            set (id::chaos, 0.25f); set (id::gravityPull, 0.6f); set (id::dryWet, 0.65f);
            set (id::triggerRate, 0.2f);
            only ({ S_FREEZE, S_REVERSE });
            set (S (S_FREEZE, "mix"), 0.7f); set (S (S_FREEZE, "prob"), 0.3f); set (S (S_FREEZE, "p3"), 0.5f);
            set (S (S_REVERSE, "mix"), 0.5f); set (S (S_REVERSE, "prob"), 0.25f);
            set (id::tqOn, 1.f); real (id::tqMode, 3); real (id::tqCutoff, 1800.f);
            set (id::tqRes, 0.7f); set (id::tqEnv, -0.5f); set (id::tqMix, 1.f);
            set (id::clotOn, 1.f); real (id::clotTime, 1); set (id::clotFeedback, 0.5f);
            real (id::clotTone, 3500.f); set (id::clotSpread, 0.75f); set (id::clotMix, 0.3f);
            real (id::outputTrim, -2.5f);   // level-matched to the other presets
            break;
        case 12: // Telephone Gate - band-limited crunch with tight 1/16 slap
            set (id::chaos, 0.55f); set (id::gravityPull, 0.85f); set (id::dryWet, 0.9f);
            set (id::triggerRate, 0.55f);
            only ({ S_STUTTER, S_CORRUPT });
            set (S (S_STUTTER, "mix"), 1.f); set (S (S_STUTTER, "prob"), 0.7f); set (S (S_STUTTER, "p2"), 0.8f);
            set (S (S_CORRUPT, "mix"), 0.85f); set (S (S_CORRUPT, "prob"), 0.5f);
            set (S (S_CORRUPT, "p1"), 0.55f); set (S (S_CORRUPT, "p3"), 0.3f);
            set (id::tqOn, 1.f); real (id::tqMode, 1); real (id::tqCutoff, 1700.f);
            set (id::tqRes, 0.45f); set (id::tqMix, 1.f);
            set (id::clotOn, 1.f); real (id::clotTime, 0); set (id::clotFeedback, 0.25f);
            real (id::clotTone, 3000.f); set (id::clotSpread, 0.3f); set (id::clotMix, 0.25f);
            break;
        case 13: // Whole Tone Haze - dense dreamy grains in D whole tone, long echoes
            set (id::chaos, 0.4f); set (id::gravityPull, 0.5f); set (id::dryWet, 0.75f);
            set (id::triggerRate, 0.45f); set (id::reinject, 0.3f);
            only ({ S_GRANULAR, S_FREEZE, S_STUTTER });
            set (S (S_GRANULAR, "mix"), 0.8f); set (S (S_GRANULAR, "prob"), 0.7f);
            set (S (S_GRANULAR, "p1"), 0.3f); set (S (S_GRANULAR, "p2"), 0.8f); set (S (S_GRANULAR, "p3"), 0.5f);
            set (S (S_FREEZE, "mix"), 0.5f); set (S (S_FREEZE, "prob"), 0.2f); set (S (S_FREEZE, "p3"), 0.8f);
            set (S (S_STUTTER, "mix"), 0.4f); set (S (S_STUTTER, "prob"), 0.3f); set (S (S_STUTTER, "p3"), 0.8f);
            set (id::scaleOn, 1.f); real (id::scaleRoot, 2); real (id::scaleType, 11); set (id::scaleRange, 0.6f);
            set (id::tqOn, 1.f); real (id::tqMode, 2); real (id::tqCutoff, 250.f); set (id::tqRes, 0.2f);
            set (id::clotOn, 1.f); real (id::clotTime, 6); set (id::clotFeedback, 0.7f);
            real (id::clotTone, 2200.f); set (id::clotSpread, 1.f); set (id::clotMix, 0.45f);
            real (id::outputTrim, -4.0f);   // level-matched to the other presets
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

            defaultAbsentNewParams (apvts, state);

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

// ==== MUSICAL (F4) ==== Simple view + first-run hint are user settings: they live in
// VSX_SETTINGS, so presets, A/B, history, RANDOM and MUTATE never touch them. A
// session saved before 0.10 has no `simpleView` property and therefore restores OFF.
static const juce::Identifier kSimpleViewProp { "simpleView" };
static const juce::Identifier kHintDoneProp   { "hintDismissed" };

bool VivisectProcessor::simpleViewEnabled() const
{
    const auto t = apvts.state.getChildWithName (kSettingsTag);
    return t.isValid() ? (bool) t.getProperty (kSimpleViewProp, false) : false;
}
void VivisectProcessor::setSimpleViewEnabled (bool on)
{
    settingsTree().setProperty (kSimpleViewProp, on, nullptr);
}
bool VivisectProcessor::firstRunHintDismissed() const
{
    const auto t = apvts.state.getChildWithName (kSettingsTag);
    return t.isValid() ? (bool) t.getProperty (kHintDoneProp, false) : false;
}
void VivisectProcessor::setFirstRunHintDismissed (bool done)
{
    settingsTree().setProperty (kHintDoneProp, done, nullptr);
}
// ==== end MUSICAL (F4) ====

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

    // Snapshots from before 0.10.0 carry no entry for the new parameters; load those at
    // their defaults rather than leaving stale values behind.
    defaultAbsentNewParams (apvts, next);
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

    r << "-- FEATURES (0.10.0) ------------------------------------------\n";
    {
        const auto txt = [this] (const char* pid) -> juce::String
        {
            if (auto* p = apvts.getParameter (pid)) return p->getCurrentValueAsText();
            return "?";
        };
        r << "scale lock  : " << txt (id::scaleOn) << ", root " << txt (id::scaleRoot)
          << ", " << txt (id::scaleType) << ", range " << txt (id::scaleRange) << "\n"
          << "dose        : " << txt (id::dose) << "\n"
          << "tourniquet  : " << txt (id::tqOn) << ", " << txt (id::tqMode) << ", cutoff " << txt (id::tqCutoff)
          << ", reso " << txt (id::tqRes) << ", env " << txt (id::tqEnv) << ", mix " << txt (id::tqMix) << "\n"
          << "clot        : " << txt (id::clotOn) << ", time " << txt (id::clotTime) << ", feedback "
          << txt (id::clotFeedback) << ", mix " << txt (id::clotMix) << "\n"
          << "simple view : " << (simpleViewEnabled() ? "on" : "off") << "\n"
          << "first-run hint dismissed: " << (firstRunHintDismissed() ? "yes" : "no") << "\n"
          << "undo / redo : " << undoDepth() << " / " << redoDepth() << " steps\n"
          << "capture     : " << vsx::describeCaptureState (capture.info()) << ", length setting "
          << captureBars() << " bar(s)\n"
          << "captures    : " << capture.directory().getFullPathName() << " ("
          << capture.list().size() << " take(s))\n";
    }
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
    setSimpleViewEnabled (true);        // MUSICAL (F4): back to fresh-install defaults
    setFirstRunHintDismissed (false);

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

    // 6. a clean slate has nothing to undo back into (F1)
    clearUndoHistory();
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
    {
        const auto st = apvts.state.getChildWithName (kSettingsTag);
        const double fb = st.isValid() ? (double) st.getProperty ("tempoFallback", 0.0) : 0.0;
        tempoFallbackBpm.store (fb > 0.0 ? vsx::TapTempo::clampBpm (fb) : 0.0);
    }

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
    UndoScope undoStep (*this);                         // F1
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
    randomisedOnce = false;
}

void VivisectProcessor::randomizeAll()
{
    UndoScope undoStep (*this);                         // F1
    // Utility and routing state are not part of the generated sound. Preserve
    // them when subsequent RANDOM presses reset the creative parameters first.
    std::map<juce::String, float> preserved;
    if (randomisedOnce)
    {
        for (const auto& pid : { id::inputTrim, id::outputTrim, id::bufferBars,
                                 id::sourceSel, id::midiMode, id::panicFreeze,
                                 id::decayArm, id::decayTime, id::flatOn,
                                 id::flatTone, id::flatBleed, id::flatMix,
                                 id::scaleRoot, id::scaleType })       // MUSICAL (F2): the song's key
            if (auto* p = apvts.getParameter (pid))
                preserved[pid] = p->getValue();

        for (const auto& pid : parameterLocks)
            if (auto* p = apvts.getParameter (pid))
                preserved[pid] = p->getValue();
        if (auto* p = apvts.getParameter (id::mutationAmount))
            preserved[id::mutationAmount] = p->getValue();

        // Reset only what is not preserved: locked parameters must never be
        // touched (no transient default value reaching the host or DSP).
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                if (preserved.find (rp->paramID) == preserved.end())
                    rp->setValueNotifyingHost (rp->getDefaultValue());
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
    // ==== MUSICAL (F2/F4) ==== root and type are the song's key: RANDOM leaves them alone
    setNorm   (id::scaleOn,        rng.nextFloat() < 0.30f ? 1.f : 0.f);
    setNorm   (id::scaleRange,     rr (0.10f, 0.80f));
    setNorm   (id::dose,           rr (0.f, 0.60f));
    // ==== end MUSICAL (F2/F4) ====

    // ==== EFFECTS (E1/E2) ====
    setNorm   (id::tqOn,           rng.nextFloat() < 0.30f ? 1.f : 0.f);
    setChoice (id::tqMode,         rng.nextInt (4));
    setNorm   (id::tqCutoff,       rr (0.25f, 0.85f));
    setNorm   (id::tqRes,          rr (0.05f, 0.75f));
    setNorm   (id::tqEnv,          rr (0.30f, 0.80f));
    setNorm   (id::tqMix,          rr (0.60f, 1.f));
    setNorm   (id::clotOn,         rng.nextFloat() < 0.30f ? 1.f : 0.f);
    setChoice (id::clotTime,       rng.nextInt (9));
    setNorm   (id::clotFree,       rr (0.10f, 0.80f));
    setNorm   (id::clotFeedback,   rr (0.10f, 0.70f));
    setNorm   (id::clotTone,       rr (0.35f, 0.90f));
    setNorm   (id::clotSpread,     rr (0.f, 1.f));
    setNorm   (id::clotMix,        rr (0.10f, 0.50f));
    // ==== END EFFECTS (E1/E2) ====

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
        setNorm   (sid (s, "routeAmt"), rr (0.2f, 0.8f));   // G1
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
    if (amount <= 0.f) return;                          // changes nothing, so records nothing
    UndoScope undoStep (*this);                         // F1

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
                      id::lfo2Rate, id::macro1, id::macro2,
                      // ==== EFFECTS (E1/E2) ====
                      id::tqCutoff, id::tqRes, id::tqEnv, id::tqMix,
                      id::clotFree, id::clotFeedback, id::clotTone, id::clotSpread, id::clotMix,
                      id::scaleRange, id::dose })   // MUSICAL (F2/F4)

        mutateContinuous (pid);

    mutateChoice (id::tqMode);        // ==== EFFECTS (E1/E2) ====
    mutateChoice (id::clotTime);
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
        mutateContinuous (sid (s, "routeAmt"));   // G1
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

    // On/off switches (SCAR and the effect stages) flip only at high amounts.
    for (auto onPid : { id::scarOn, id::tqOn, id::clotOn })     // ==== EFFECTS (E1/E2) ====
        if (amount >= 0.70f && ! isParameterLocked (onPid)
            && rng.nextFloat() < (amount - 0.70f) * 0.30f)
            if (auto* p = apvts.getParameter (onPid))
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.f : 1.f);
    // ==== MUSICAL (F2) ====
    if (amount >= 0.70f && ! isParameterLocked (id::scaleOn)
        && rng.nextFloat() < (amount - 0.70f) * 0.30f)
        if (auto* p = apvts.getParameter (id::scaleOn))
            p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.f : 1.f);
    // ==== end MUSICAL (F2) ====

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

    UndoScope undoStep (*this);                         // F1: one step per A/B swap
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
    UndoScope undoStep (*this);                         // F1: one step per preset load
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

// ===========================================================================
//  Undo / redo (F1)
//
//  Same scope as A/B: sound parameters only. Each snapshot keeps the exact
//  normalised value of every parameter next to the sound-state tree, so a
//  restore is bit-exact even where the tree round trip (real value text and
//  back) would not be.
// ===========================================================================
std::vector<float> VivisectProcessor::currentNormalised() const
{
    const auto& all = getParameters();
    std::vector<float> v;
    v.reserve ((size_t) all.size());
    for (auto* p : all)
        v.push_back (p->getValue());
    return v;
}

VivisectProcessor::UndoSnapshot VivisectProcessor::captureUndoSnapshot()
{
    UndoSnapshot s;
    s.norm = currentNormalised();
    s.tree = soundStateSnapshot();
    return s;
}

void VivisectProcessor::applyUndoSnapshot (const UndoSnapshot& snap)
{
    ++undoScopeDepth;                       // our own edits are not new steps
    restoreSoundState (snap.tree);
    const auto& all = getParameters();
    if ((size_t) all.size() == snap.norm.size())
        for (int i = 0; i < all.size(); ++i)
            if (all[i]->getValue() != snap.norm[(size_t) i])
                all[i]->setValueNotifyingHost (snap.norm[(size_t) i]);
    --undoScopeDepth;
    gestureDepth = 0;
    gestureStart.reset();
}

void VivisectProcessor::pushUndoSnapshot()
{
    auto snap = captureUndoSnapshot();
    const juce::ScopedLock sl (undoLock);
    undoStack.push_back (std::move (snap));
    while ((int) undoStack.size() > kUndoDepth)
        undoStack.pop_front();
    redoStack.clear();                      // a new edit ends the redo branch
}

bool VivisectProcessor::undo()
{
    commitOpenUndoGesture();    // Ctrl+Z during a drag undoes that drag, not the step before it
    UndoSnapshot target;
    {
        const juce::ScopedLock sl (undoLock);
        if (undoStack.empty()) return false;
        target = std::move (undoStack.back());
        undoStack.pop_back();
    }
    auto now = captureUndoSnapshot();
    {
        const juce::ScopedLock sl (undoLock);
        redoStack.push_back (std::move (now));
        while ((int) redoStack.size() > kUndoDepth) redoStack.pop_front();
    }
    applyUndoSnapshot (target);
    return true;
}

bool VivisectProcessor::redo()
{
    commitOpenUndoGesture();
    UndoSnapshot target;
    {
        const juce::ScopedLock sl (undoLock);
        if (redoStack.empty()) return false;
        target = std::move (redoStack.back());
        redoStack.pop_back();
    }
    auto now = captureUndoSnapshot();
    {
        const juce::ScopedLock sl (undoLock);
        undoStack.push_back (std::move (now));      // redo must not clear the redo stack
        while ((int) undoStack.size() > kUndoDepth) undoStack.pop_front();
    }
    applyUndoSnapshot (target);
    return true;
}

bool VivisectProcessor::canUndo() const { const juce::ScopedLock sl (undoLock); return ! undoStack.empty(); }
bool VivisectProcessor::canRedo() const { const juce::ScopedLock sl (undoLock); return ! redoStack.empty(); }
int  VivisectProcessor::undoDepth() const { const juce::ScopedLock sl (undoLock); return (int) undoStack.size(); }
int  VivisectProcessor::redoDepth() const { const juce::ScopedLock sl (undoLock); return (int) redoStack.size(); }

void VivisectProcessor::clearUndoHistory()
{
    const juce::ScopedLock sl (undoLock);
    undoStack.clear();
    redoStack.clear();
    gestureDepth = 0;
    gestureStart.reset();
}

void VivisectProcessor::commitOpenUndoGesture()
{
    if (gestureDepth > 0 && gestureStart.has_value() && gestureStart->norm != currentNormalised())
    {
        const juce::ScopedLock sl (undoLock);
        undoStack.push_back (std::move (*gestureStart));
        while ((int) undoStack.size() > kUndoDepth) undoStack.pop_front();
        redoStack.clear();
    }
    gestureDepth = 0;
    gestureStart.reset();
}

// "Undo: Chaos 0.40 -> 0.12" when one control differs, a count when several.
juce::String VivisectProcessor::describeUndoStep (const UndoSnapshot& target, const char* verb) const
{
    const auto now = currentNormalised();
    const auto& all = getParameters();
    int changed = 0, first = -1;
    if (now.size() == target.norm.size())
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i] != target.norm[i]) { if (first < 0) first = (int) i; ++changed; }

    const juce::String head = juce::String (verb) + ": ";
    if (changed == 0)  return head + "last action";
    if (changed > 1)   return head + juce::String (changed) + " controls";
    auto* p = all[first];
    return head + p->getName (24) + " " + p->getText (now[(size_t) first], 12)
         + " -> " + p->getText (target.norm[(size_t) first], 12);
}

juce::String VivisectProcessor::undoLabel() const
{
    const juce::ScopedLock sl (undoLock);
    return undoStack.empty() ? juce::String ("Undo: nothing to undo")
                             : describeUndoStep (undoStack.back(), "Undo");
}
juce::String VivisectProcessor::redoLabel() const
{
    const juce::ScopedLock sl (undoLock);
    return redoStack.empty() ? juce::String ("Redo: nothing to redo")
                             : describeUndoStep (redoStack.back(), "Redo");
}

// One undo step per completed UI gesture. beginChangeGesture arrives before the
// first value change, so that is the state to return to; a gesture that ended
// where it started (a click without a move) adds nothing. Host automation never
// opens a gesture here, and actions that push their own step are skipped.
void VivisectProcessor::UndoGestureListener::parameterGestureChanged (int, bool starting)
{
    auto& o = owner;
    if (o.undoScopeDepth > 0) return;
    if (starting)
    {
        if (o.gestureDepth++ == 0)
            o.gestureStart = o.captureUndoSnapshot();
    }
    else if (o.gestureDepth > 0 && --o.gestureDepth == 0)
    {
        if (o.gestureStart.has_value() && o.gestureStart->norm != o.currentNormalised())
        {
            const juce::ScopedLock sl (o.undoLock);
            o.undoStack.push_back (std::move (*o.gestureStart));
            while ((int) o.undoStack.size() > kUndoDepth) o.undoStack.pop_front();
            o.redoStack.clear();
        }
        o.gestureStart.reset();
    }
}

// ===========================================================================
//  Tempo and key detection (H1/H2)
// ===========================================================================
void VivisectProcessor::setTempoFallback (double bpm)
{
    const double v = bpm > 0.0 ? vsx::TapTempo::clampBpm (bpm) : 0.0;
    tempoFallbackBpm.store (v);
    settingsTree().setProperty ("tempoFallback", v, nullptr);
}

double VivisectProcessor::tapTempo()
{
    const double bpm = tapper.tap (juce::Time::getMillisecondCounterHiRes() * 0.001);
    if (bpm > 0.0) setTempoFallback (bpm);
    return bpm;
}

void VivisectProcessor::requestKeyDetect()
{
    int expected = 0;
    keyState.compare_exchange_strong (expected, 1);
}

void VivisectProcessor::captureKeyWindow() noexcept
{
    const auto& raw = specimen.raw();
    const int cap = specimen.capacitySamples();
    const i64 w = specimen.writePos();
    const int n = (int) juce::jmin ((i64) kKeyWindow, (i64) cap, w);
    if (cap <= 0 || raw.getNumChannels() < 2 || keyBuf.size() < (size_t) n) { keyCount.store (0); keyState.store (2, std::memory_order_release); return; }
    const float* l = raw.getReadPointer (0);
    const float* r = raw.getReadPointer (1);
    for (int i = 0; i < n; ++i)
    {
        const int idx = (int) (((w - n + i) % cap + cap) % cap);
        keyBuf[(size_t) i] = 0.5f * (l[idx] + r[idx]);
    }
    keyCount.store (n);
    keyState.store (2, std::memory_order_release);
}

bool VivisectProcessor::pollKeyDetect()
{
    if (keyState.load (std::memory_order_acquire) != 2) return false;
    keyResult = vsx::keydetect::analyse (keyBuf.data(), keyCount.load(), sampleRate);
    keyState.store (0, std::memory_order_release);
    if (! keyResult.valid) return true;

    UndoScope undoStep (*this);
    auto setChoiceIdx = [this] (const char* pid, int idx)
    {
        if (isParameterLocked (pid)) return;
        if (auto* p = apvts.getParameter (pid))
            p->setValueNotifyingHost (juce::jlimit (0.f, 1.f, p->getNormalisableRange().convertTo0to1 ((float) idx)));
    };
    setChoiceIdx (id::scaleRoot, keyResult.root);
    setChoiceIdx (id::scaleType, keyResult.minor ? 2 : 1);      // Major = 1, Minor = 2
    if (! isParameterLocked (id::scaleOn))
        if (auto* p = apvts.getParameter (id::scaleOn)) p->setValueNotifyingHost (1.f);
    return true;
}

// ===========================================================================
//  Capture (F3)
// ===========================================================================
static const juce::Identifier kCaptureBarsProp { "captureBars" };

int VivisectProcessor::captureBars() const
{
    const auto t = apvts.state.getChildWithName (kSettingsTag);
    const int b = t.isValid() ? (int) t.getProperty (kCaptureBarsProp, 4) : 4;
    return vsx::capture::isValidBars (b) ? b : 4;
}

void VivisectProcessor::setCaptureBars (int bars)
{
    if (vsx::capture::isValidBars (bars) && bars != captureBars())
        settingsTree().setProperty (kCaptureBarsProp, bars, nullptr);
}

bool VivisectProcessor::startCapture (int bars)
{
    if (isSuspended()) return false;        // never records while suspended
    return capture.start (bars);
}

void VivisectProcessor::cancelCapture() { capture.cancel(); }

juce::AudioProcessorEditor* VivisectProcessor::createEditor() { return new VivisectEditor (*this); }

// ===========================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VivisectProcessor(); }
