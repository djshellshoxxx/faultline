// ============================================================================
//  0.10.0 G1 (per-surgeon Route Amt) and G2 (FREEZE level tuning) tests.
//  Included by TestMain.cpp after the shared helpers (check/beginCase/...).
// ============================================================================
namespace routing_tests
{
// ---- G2 helpers ------------------------------------------------------------
enum class Material { noise, sine, drums };

// Mono material, deterministic. Drums: kick + snare + hat bursts, dense enough
// that every 2048-sample grab contains transients.
inline void makeMaterial (Material m, double sr, int numSamples, AudioBuffer<float>& out)
{
    out.setSize (2, numSamples);
    Random rng (0x5eed + (int) m);
    const double twoPi = MathConstants<double>::twoPi;
    float hatEnv = 0.f, lp = 0.f;
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = (double) i / sr;
        float s = 0.f;
        switch (m)
        {
            case Material::noise: s = 0.3f * (rng.nextFloat() * 2.f - 1.f); break;
            case Material::sine:  s = 0.5f * (float) std::sin (twoPi * 441.0 * t); break;
            case Material::drums:
            {
                const double beat = 0.125;                    // 16ths at 120 BPM
                const double ph = std::fmod (t, beat);
                const int step = (int) (t / beat) % 8;
                if (step % 4 == 0)   s += 0.7f * (float) (std::sin (twoPi * (50.0 + 90.0 * std::exp (-ph * 40.0)) * ph) * std::exp (-ph * 18.0));
                if (step % 4 == 2)   s += 0.5f * (rng.nextFloat() * 2.f - 1.f) * (float) std::exp (-ph * 25.0);
                if (ph < 1.0e-4)     hatEnv = 1.f;
                hatEnv *= 0.9985f;
                lp += 0.6f * ((rng.nextFloat() * 2.f - 1.f) - lp);
                s += 0.25f * hatEnv * ((rng.nextFloat() * 2.f - 1.f) - lp);
                break;
            }
        }
        out.setSample (0, i, s);
        out.setSample (1, i, s);
    }
}

struct FreezeResult
{
    double specimenDb = 0.0;   // Hann-weighted RMS of the grabbed window (what the freeze analysed)
    double outDb      = 0.0;   // RMS of the frozen output over the steady hold
    float  peak       = 0.f;   // largest |sample| anywhere, including ramp-in and fade-out
    float  edgePeak   = 0.f;   // largest |sample| in the ramp-in (first 2 frames) and the fade-out
    float  steadyPeak = 0.f;   // largest |sample| in the steady hold
    bool   finite     = true;
};

// Triggers the FREEZE surgeon on a 2048 sample grab of `m` and measures it.
// LENGTH 0.5 holds for 2.1 s; the RMS is taken over 4 hops .. 1.5 s so it
// averages enough hops to be stable for narrow-band material.
inline FreezeResult measureFreeze (Material m, double sr, float blur, float grabFrac)
{
    constexpr int N = 2048;
    const int total = (int) (sr * 3.0);
    AudioBuffer<float> src;
    makeMaterial (m, sr, total, src);

    vsx::SpecimenBuffer spec;
    spec.prepare (sr, 6.0);
    spec.push (src);

    auto fz = vsx::makeSurgeon (vsx::S_FREEZE);
    fz->prepare (sr, 512);
    fz->setParams (0.f, 0.5f, blur);         // OFFSET 0, LENGTH 0.5, BLUR

    Random rng (7);
    vsx::TriggerContext c;
    c.rng = &rng; c.specimen = &spec; c.nowPos = spec.writePos();
    c.sourcePos = (double) c.nowPos - sr * (0.5 + 1.5 * grabFrac);
    c.lengthSamples = (double) N;
    c.startOffset = 0;
    fz->trigger (c);

    FreezeResult r;
    double ew = 0.0, wsum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        const double x = 0.5 * (vsx::readSource (c, 0, c.sourcePos + i) + vsx::readSource (c, 1, c.sourcePos + i));
        const double w = 0.5 - 0.5 * std::cos (MathConstants<double>::twoPi * i / (N - 1));
        ew += w * w * x * x; wsum += w * w;
    }
    r.specimenDb = 10.0 * std::log10 (jmax (1.0e-20, ew / wsum));

    const int holdSamples = (int) (1.5 * sr);
    const int endHold = (int) (2.1 * sr);
    const int blockSize = 256;
    AudioBuffer<float> blk (2, blockSize);
    double es = 0.0; long cnt = 0;
    int pos = 0;
    while (pos < endHold + (int) (0.15 * sr))
    {
        blk.clear();
        fz->process (blk, blockSize);
        const auto v = inspect (blk);
        r.finite = r.finite && v.finite;
        r.peak = jmax (r.peak, v.peak);
        for (int i = 0; i < blockSize; ++i)
        {
            const int at = pos + i;
            const float a = std::abs (blk.getSample (0, i));
            if (at < 2 * 1024 || at >= endHold) r.edgePeak = jmax (r.edgePeak, a);
            else                                r.steadyPeak = jmax (r.steadyPeak, a);
            if (at >= 4 * 512 && at < holdSamples)
            {
                const double y = blk.getSample (0, i);
                es += y * y; ++cnt;
            }
        }
        pos += blockSize;
    }
    r.outDb = 10.0 * std::log10 (jmax (1.0e-20, es / (double) jmax (1L, cnt)));
    return r;
}

// ---- G1 helpers ------------------------------------------------------------
inline void setP (VivisectProcessor& p, const String& id, float actual)
{
    if (auto* par = p.apvts.getParameter (id))
        par->setValueNotifyingHost (par->getNormalisableRange().convertTo0to1 (actual));
}
inline float getP (VivisectProcessor& p, const String& id)
{
    auto* par = p.apvts.getParameter (id);
    return par != nullptr ? par->getNormalisableRange().convertFrom0to1 (par->getValue()) : -999.f;
}

// every surgeon on and busy, so routing has something to carry
inline void busyRack (VivisectProcessor& p)
{
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
    {
        setP (p, vsx::sid (s, "on"), 1.f);
        setP (p, vsx::sid (s, "mix"), 1.f);
        setP (p, vsx::sid (s, "prob"), 1.f);
    }
    for (auto pid : { vsx::id::triggerRate, vsx::id::dryWet })
        setP (p, pid, 1.f);
    setP (p, vsx::id::chaos, 0.5f);
}

// Surgeon d is fed by surgeon d-1 (wrapping): a ring through all six.
inline void routeRing (VivisectProcessor& p, float amt)
{
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
    {
        setP (p, vsx::sid (s, "route"), (float) (((s + vsx::kNumSurgeons - 1) % vsx::kNumSurgeons) + 1));
        setP (p, vsx::sid (s, "routeAmt"), amt);
    }
}

inline std::vector<float> render (VivisectProcessor& p, int numBlocks, int blockSize, double sr)
{
    std::vector<float> out;
    out.reserve ((size_t) numBlocks * (size_t) blockSize * 2);
    AudioBuffer<float> buf (2, blockSize);
    MidiBuffer midi;
    int64 pos = 0;
    for (int b = 0; b < numBlocks; ++b)
    {
        fillTestSignal (buf, pos, sr);
        midi.clear();
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.insert (out.end(), buf.getReadPointer (ch), buf.getReadPointer (ch) + blockSize);
        pos += blockSize;
    }
    return out;
}

inline void collectSliders (Component& c, std::vector<vsx::VsxSlider*>& out)
{
    if (auto* s = dynamic_cast<vsx::VsxSlider*> (&c)) out.push_back (s);
    for (auto* child : c.getChildren()) collectSliders (*child, out);
}
} // namespace routing_tests

static void run_routing_tests (double kRate, int kBlock)
{
    using namespace routing_tests;

    // =======================================================================
    //  G1
    // =======================================================================
    beginCase ("G1: Route Amt parameter exists for every surgeon with the right shape");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        bool all = true, defaults = true, ranges = true, names = true, audioIds = true, automatable = true;
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
        {
            const String id = vsx::sid (s, "routeAmt");
            auto* par = p.apvts.getParameter (id);
            if (par == nullptr) { all = false; continue; }
            defaults    = defaults && std::abs (getP (p, id) - 0.5f) < 1.0e-4f;
            auto* rp = dynamic_cast<RangedAudioParameter*> (par);
            ranges      = ranges && rp != nullptr && rp->getNormalisableRange().start == 0.f && rp->getNormalisableRange().end == 1.f;
            names       = names && par->getName (64).containsIgnoreCase ("Route Amt")
                                 && par->getName (64).containsIgnoreCase (vsx::surgeonName (s));
            audioIds    = audioIds && String (vsx::routeAmtRaw (s)) == id
                                   && p.apvts.getRawParameterValue (vsx::routeAmtRaw (s)) != nullptr;
            automatable = automatable && par->isAutomatable();
        }
        check (all, "all six <surgeon>_routeAmt parameters exist");
        check (defaults, "default is 0.5 for every surgeon");
        check (ranges, "range is 0..1");
        check (names, "label reads '<SURGEON> Route Amt'");
        check (audioIds, "audio-thread id table matches sid() and resolves");
        check (automatable, "automatable");
    }

    beginCase ("G1: cycle of all six routed around at amt 1 stays bounded for 60 s");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        busyRack (p);
        routeRing (p, 1.f);
        setP (p, vsx::id::reinject, 1.f);
        setP (p, vsx::id::chaos, 1.f);
        const int blocks = (int) (60.0 * kRate / kBlock);
        const auto v = runBlocks (p, blocks, kBlock, kRate, true);
        check (v.finite, "60 s ring feedback stays finite");
        check (v.peak < kCeiling, "60 s ring feedback stays bounded (peak " + String (v.peak, 2) + ")");

        const auto tail = runBlocks (p, (int) (40.0 * kRate / kBlock), kBlock, kRate, false, true);
        check (tail.finite && tail.peak < kCeiling, "ring feedback stays bounded with silent input");
    }

    beginCase ("G1: every surgeon routed from itself at amt 1 with reinject 0 stays bounded");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        busyRack (p);
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
        {
            setP (p, vsx::sid (s, "route"), (float) (s + 1));
            setP (p, vsx::sid (s, "routeAmt"), 1.f);
        }
        setP (p, vsx::id::reinject, 0.f);
        const auto v = runBlocks (p, (int) (20.0 * kRate / kBlock), kBlock, kRate, true);
        check (v.finite && v.peak < kCeiling, "self feedback bounded (peak " + String (v.peak, 2) + ")");
    }

    beginCase ("G1: amt 0 equals no routing, Route Off ignores amt (bit-exact)");
    {
        const int blocks = 400;
        VivisectProcessor off, zero, offHi, ringHalf, ringFull;
        for (auto* p : { &off, &zero, &offHi, &ringHalf, &ringFull })
        {
            p->prepareToPlay (kRate, kBlock);
            busyRack (*p);
        }
        routeRing (zero, 0.f);                                   // full ring, amt 0
        for (int s = 0; s < vsx::kNumSurgeons; ++s)              // route Off, amt wide open
            setP (offHi, vsx::sid (s, "routeAmt"), 1.f);
        routeRing (ringHalf, 0.5f);
        routeRing (ringFull, 1.f);

        const auto a = render (off, blocks, kBlock, kRate);
        const auto b = render (zero, blocks, kBlock, kRate);
        const auto c = render (offHi, blocks, kBlock, kRate);
        const auto d = render (ringHalf, blocks, kBlock, kRate);
        const auto e = render (ringFull, blocks, kBlock, kRate);

        check (a == b, "route ring at amt 0 is bit-identical to route Off");
        check (a == c, "route Off is bit-identical whatever amt is (default sessions do not regress)");
        check (a != d, "route ring at amt 0.5 is audible (differs from Off)");
        check (d != e, "amt changes the result (0.5 vs 1.0)");
        bool finite = true;
        for (float x : d) finite = finite && std::isfinite (x);
        for (float x : e) finite = finite && std::isfinite (x);
        check (finite, "routed renders are finite");
    }

    beginCase ("G1: old sessions without routeAmt load at the default");
    {
        VivisectProcessor src;
        src.prepareToPlay (kRate, kBlock);
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            setP (src, vsx::sid (s, "route"), (float) (s + 1));
        MemoryBlock mb;
        src.getStateInformation (mb);

        // strip every routeAmt node, as a 0.9.x session would have none
        auto xml = AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        check (xml != nullptr, "state serialises");
        if (xml != nullptr)
        {
            int removed = 0;
            for (int i = xml->getNumChildElements(); --i >= 0;)
            {
                auto* ch = xml->getChildElement (i);
                if (ch->getStringAttribute ("id").endsWith ("_routeAmt")) { xml->removeChildElement (ch, true); ++removed; }
            }
            check (removed == vsx::kNumSurgeons, "state carried all six routeAmt nodes (removed " + String (removed) + ")");
            MemoryBlock old;
            AudioProcessor::copyXmlToBinary (*xml, old);

            VivisectProcessor dst;
            dst.prepareToPlay (kRate, kBlock);
            dst.setStateInformation (old.getData(), (int) old.getSize());
            bool amtDefault = true, routeKept = true;
            for (int s = 0; s < vsx::kNumSurgeons; ++s)
            {
                amtDefault = amtDefault && std::abs (getP (dst, vsx::sid (s, "routeAmt")) - 0.5f) < 1.0e-4f;
                routeKept  = routeKept  && (int) std::lround (getP (dst, vsx::sid (s, "route"))) == s + 1;
            }
            check (amtDefault, "missing routeAmt loads as 0.5");
            check (routeKept, "Route In still loads from the old session");
        }

        VivisectProcessor a;
        a.prepareToPlay (kRate, kBlock);
        setP (a, "gr_routeAmt", 0.83f);
        MemoryBlock mb2;
        a.getStateInformation (mb2);
        VivisectProcessor b;
        b.prepareToPlay (kRate, kBlock);
        b.setStateInformation (mb2.getData(), (int) mb2.getSize());
        check (std::abs (getP (b, "gr_routeAmt") - 0.83f) < 1.0e-3f, "state round trip keeps routeAmt");
    }

    beginCase ("G1: RESET, RANDOM, MUTATE, locks, presets and A/B cover Route Amt");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);

        for (int s = 0; s < vsx::kNumSurgeons; ++s) setP (p, vsx::sid (s, "routeAmt"), 0.9f);
        p.resetAllToDefaults();
        bool reset = true;
        for (int s = 0; s < vsx::kNumSurgeons; ++s)
            reset = reset && std::abs (getP (p, vsx::sid (s, "routeAmt")) - 0.5f) < 1.0e-4f;
        check (reset, "RESET returns Route Amt to 0.5");

        bool moved = false, inRange = true;
        for (int i = 0; i < 12; ++i)
        {
            p.randomizeAll();
            for (int s = 0; s < vsx::kNumSurgeons; ++s)
            {
                const float v = getP (p, vsx::sid (s, "routeAmt"));
                moved   = moved || std::abs (v - 0.5f) > 0.02f;
                inRange = inRange && v >= 0.f && v <= 1.f;
            }
        }
        check (moved && inRange, "RANDOM changes Route Amt and keeps it in range");

        p.resetAllToDefaults();
        setP (p, "st_routeAmt", 0.321f);
        p.setParameterLocked ("st_routeAmt", true);
        setP (p, vsx::id::mutationAmount, 1.f);
        for (int i = 0; i < 10; ++i) { p.randomizeAll(); p.mutateCurrent(); }
        check (std::abs (getP (p, "st_routeAmt") - 0.321f) < 1.0e-4f, "a locked Route Amt survives RANDOM and MUTATE");
        p.setParameterLocked ("st_routeAmt", false);

        p.resetAllToDefaults();
        setP (p, vsx::id::mutationAmount, 1.f);
        bool mutated = false;
        for (int i = 0; i < 10 && ! mutated; ++i)
        {
            p.mutateCurrent();
            for (int s = 0; s < vsx::kNumSurgeons; ++s)
                mutated = mutated || std::abs (getP (p, vsx::sid (s, "routeAmt")) - 0.5f) > 1.0e-3f;
        }
        check (mutated, "MUTATE moves Route Amt");

        setP (p, "fz_routeAmt", 0.95f);
        p.loadPreset (1);
        check (std::abs (getP (p, "fz_routeAmt") - 0.5f) < 1.0e-4f, "factory preset load restores default Route Amt");

        p.resetAllToDefaults();
        setP (p, "cr_routeAmt", 0.77f);
        const auto file = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx_routeamt_test.vsxpreset");
        file.deleteFile();
        const bool saved = p.savePresetToFile (file);
        setP (p, "cr_routeAmt", 0.1f);
        const bool loaded = saved && p.loadPresetFromFile (file);
        check (saved && loaded, "preset file saves and loads");
        check (std::abs (getP (p, "cr_routeAmt") - 0.77f) < 1.0e-3f, "preset file keeps Route Amt");
        file.deleteFile();

        p.resetAllToDefaults();
        p.selectABSlot (0);
        setP (p, "ro_routeAmt", 0.8f);
        p.selectABSlot (1);
        setP (p, "ro_routeAmt", 0.2f);
        p.selectABSlot (0);
        check (std::abs (getP (p, "ro_routeAmt") - 0.8f) < 1.0e-3f, "A/B slot A keeps its Route Amt");
        p.selectABSlot (1);
        check (std::abs (getP (p, "ro_routeAmt") - 0.2f) < 1.0e-3f, "A/B slot B keeps its Route Amt");
        p.selectABSlot (0);
    }

    beginCase ("G1: every strip shows a labelled Route Amt knob with a tooltip, inside the strip");
    {
        VivisectProcessor p;
        p.setSimpleViewEnabled (false);      // a fresh install opens in Simple view, which hides the AMT knobs
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        check (ed != nullptr, "editor builds");
        if (ed != nullptr)
        {
            std::vector<vsx::VsxSlider*> sliders;
            collectSliders (*ed, sliders);
            int amt = 0; bool tips = true, fits = true;
            for (auto* sl : sliders)
                if (sl->getCaption() == "AMT")
                {
                    ++amt;
                    tips = tips && sl->getTooltip().containsIgnoreCase ("Route In");
                    auto* par = sl->getParentComponent();
                    fits = fits && par != nullptr && sl->getWidth() > 20 && sl->getHeight() > 20
                                && par->getLocalBounds().contains (sl->getBounds());
                }
            check (amt == vsx::kNumSurgeons, "six AMT knobs, one per surgeon strip (found " + String (amt) + ")");
            check (tips, "AMT knobs carry a tooltip that names Route In");
            check (fits, "AMT knobs have real size and sit inside their strip");
        }
    }

    // =======================================================================
    //  G2
    // =======================================================================
    beginCase ("G2: FREEZE output RMS within +-1.5 dB of the frozen specimen (all BLUR, 44.1/48/96 kHz)");
    {
        double worst = 0.0;
        String worstAt;
        int combos = 0, levelFails = 0, spikeFails = 0, nonFinite = 0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (auto m : { Material::noise, Material::sine, Material::drums })
                for (float blur : { 0.f, 0.25f, 0.5f, 0.75f, 1.f })
                    for (float grab : { 0.f, 0.5f })
                    {
                        const auto r = measureFreeze (m, sr, blur, grab);
                        ++combos;
                        const double d = r.outDb - r.specimenDb;
                        if (std::abs (d) > std::abs (worst))
                        {
                            worst = d;
                            worstAt = String (sr, 0) + " Hz, material " + String ((int) m) + ", blur " + String (blur, 2);
                        }
                        if (std::abs (d) > 1.5)
                        {
                            ++levelFails;
                            std::printf ("   level  sr=%.0f mat=%d blur=%.2f grab=%.1f diff=%.2f dB\n", sr, (int) m, blur, grab, d);
                        }
                        if (! r.finite) ++nonFinite;

                        // no edge spikes: the ramp-in and the fade-out never poke above the steady hold,
                        // and no sample is wild relative to the output's own level
                        const double rms = std::pow (10.0, r.outDb / 20.0);
                        const bool edgeOk  = r.edgePeak <= 1.1f * r.steadyPeak + 1.0e-3f;
                        const bool crestOk = r.peak <= 6.0 * rms;
                        if (! edgeOk || ! crestOk)
                        {
                            ++spikeFails;
                            std::printf ("   spike  sr=%.0f mat=%d blur=%.2f grab=%.1f edge=%.3f steady=%.3f peak=%.3f rms=%.3f\n",
                                         sr, (int) m, blur, grab, r.edgePeak, r.steadyPeak, r.peak, rms);
                        }
                    }
        std::printf ("   %d combinations, worst deviation %.2f dB (%s)\n", combos, worst, worstAt.toRawUTF8());
        check (nonFinite == 0, "freeze output finite everywhere");
        check (levelFails == 0, "freeze RMS within +-1.5 dB of the specimen (" + String (levelFails) + " out)");
        check (spikeFails == 0, "no edge spikes in ramp-in, hold or fade-out (" + String (spikeFails) + " flagged)");
    }
}
