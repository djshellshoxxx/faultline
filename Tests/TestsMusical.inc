// ============================================================================
//  Scale Lock (F2) and Dose / Simple view (F4) tests.
//  Included by TestMain.cpp after the shared helpers (check, beginCase,
//  runBlocks, inspect, fillTestSignal, snapshotParams).
// ============================================================================
#include "Musical.h"
#include <set>

namespace
{
using vsx::ScaleQuantizer;

void musicalSet (VivisectProcessor& p, const char* pid, float normalised)
{
    if (auto* q = p.apvts.getParameter (pid)) q->setValueNotifyingHost (normalised);
}
// set a parameter by its real (denormalised) value
void musicalSetReal (VivisectProcessor& p, const char* pid, float real)
{
    if (auto* q = p.apvts.getParameter (pid))
        q->setValueNotifyingHost (q->convertTo0to1 (real));
}

// Deterministic render: returns an FNV-1a hash of every output sample plus
// finiteness. `setup` configures the processor after RESET.
struct MusicalRender { uint64_t hash = 1469598103934665603ull; bool finite = true; float peak = 0.f; };

template <typename Setup>
MusicalRender musicalRender (double rate, int block, int blocks, Setup&& setup, bool silent = false)
{
    VivisectProcessor p;
    p.prepareToPlay (rate, block);
    p.resetAllToDefaults();
    setup (p);
    AudioBuffer<float> buf (2, block);
    MidiBuffer midi;
    MusicalRender r;
    int64 pos = 0;
    for (int b = 0; b < blocks; ++b)
    {
        if (silent) buf.clear(); else fillTestSignal (buf, pos, rate);
        p.processBlock (buf, midi);
        const auto v = inspect (buf);
        r.finite = r.finite && v.finite;
        r.peak = jmax (r.peak, v.peak);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
            {
                uint32_t u; const float f = buf.getSample (ch, i);
                std::memcpy (&u, &f, 4);
                r.hash = (r.hash ^ u) * 1099511628211ull;
            }
        pos += block;
    }
    return r;
}

// The fixed scenario used for the 0.9.3 reference hash: busy, every surgeon on except FREEZE.
// FREEZE is left off on purpose: 0.10.0 G2 retuned its level compensation, a deliberate change
// to the FREEZE sound, so a render that includes it can no longer equal the 0.9.3 render. Routing
// is off by default (Route In = Off, so Route Amt is inert). The reference hash below was
// produced by the unmodified 0.9.3 sources (commit 84b39f8) for exactly this setup.
void musicalGoldenSetup (VivisectProcessor& p)
{
    musicalSet (p, vsx::id::chaos, 0.7f);
    musicalSet (p, vsx::id::triggerRate, 0.8f);
    musicalSet (p, vsx::id::reinject, 0.3f);
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
    {
        musicalSet (p, vsx::sid (s, "on").toRawUTF8(), s == vsx::S_FREEZE ? 0.f : 1.f);
        musicalSet (p, vsx::sid (s, "prob").toRawUTF8(), 0.9f);
        musicalSet (p, vsx::sid (s, "p3").toRawUTF8(), 0.6f);
    }
}

// Only the three pitch-bearing surgeons, with their free pitch scatter at zero.
void musicalPitchSurgeonsSetup (VivisectProcessor& p, float chaos)
{
    musicalSet (p, vsx::id::chaos, chaos);
    musicalSet (p, vsx::id::triggerRate, 0.9f);
    musicalSet (p, vsx::id::dryWet, 1.f);
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
    {
        const bool pitchy = s == vsx::S_STUTTER || s == vsx::S_GRANULAR || s == vsx::S_REVERSE;
        musicalSet (p, vsx::sid (s, "on").toRawUTF8(), pitchy ? 1.f : 0.f);
        musicalSet (p, vsx::sid (s, "prob").toRawUTF8(), 0.95f);
        musicalSet (p, vsx::sid (s, "mix").toRawUTF8(), 1.f);
    }
    musicalSet (p, vsx::sid (vsx::S_STUTTER, "p3").toRawUTF8(), 0.f);    // DRIFT 0
    musicalSet (p, vsx::sid (vsx::S_GRANULAR, "p3").toRawUTF8(), 0.f);   // SPRAY 0
}

std::unique_ptr<juce::XmlElement> musicalStateXml (VivisectProcessor& p)
{
    MemoryBlock blob;
    p.getStateInformation (blob);
    return AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
}
} // namespace

static void run_musical_tests (double kRate, int kBlock)
{
    // -----------------------------------------------------------------------
    beginCase ("F2 ScaleQuantizer: every output is in scale (12 roots x 12 types, dense sweep)");
    {
        bool allInScale = true, allNearest = true, allFinite = true;
        for (int type = 0; type < vsx::kNumScaleTypes; ++type)
            for (int root = 0; root < vsx::kNumScaleRoots; ++root)
                for (float x = -26.f; x <= 26.f; x += 0.0625f)
                {
                    const float q = ScaleQuantizer::quantize (x, root, type);
                    allFinite = allFinite && std::isfinite (q);
                    const int qi = (int) std::lround (q);
                    allInScale = allInScale && std::abs (q - (float) qi) < 1.0e-6f
                                            && ScaleQuantizer::isInScale (qi, root, type);
                    // nothing in scale may be strictly closer than the answer
                    const float dq = std::abs (x - q);
                    for (int c = qi - 12; c <= qi + 12; ++c)
                        if (ScaleQuantizer::isInScale (c, root, type) && std::abs (x - (float) c) < dq - 1.0e-6f)
                            allNearest = false;
                }
        check (allFinite, "quantize is always finite");
        check (allInScale, "quantize always lands on an in-scale integer offset");
        check (allNearest, "quantize returns the nearest in-scale offset");

        // ties resolve upward
        check (ScaleQuantizer::quantize (1.f, 0, 1) == 2.f, "C major: 1.0 st ties up to 2");
        check (ScaleQuantizer::quantize (-2.f, 0, 1) == -1.f, "C major: -2.0 st ties up to -1");
        check (ScaleQuantizer::quantize (3.f, 0, 8) == 4.f, "C major pentatonic: 3.0 ties up to 4");
        check (ScaleQuantizer::quantize (0.4f, 0, 1) == 0.f && ScaleQuantizer::quantize (0.6f, 0, 1) == 0.f,
               "C major: 0.4 and 0.6 stay at 0");
        check (ScaleQuantizer::quantize (1.5f, 2, 1) == 2.f, "D major: 1.5 st ties up to 2");
        // chromatic = rounded semitones
        bool chromaticRounds = true;
        for (float x = -13.f; x <= 13.f; x += 0.125f)
            chromaticRounds = chromaticRounds && ScaleQuantizer::quantize (x, 5, 0) == std::floor (x + 0.5f);
        check (chromaticRounds, "chromatic returns rounded semitones (half up)");
        check (ScaleQuantizer::quantize (std::nanf (""), 0, 2) == 0.f
               && std::isfinite (ScaleQuantizer::quantize (1.0e30f, 3, 4)),
               "non-finite and huge inputs are handled");
        check (ScaleQuantizer::isInScale (9, 9, 2) && ScaleQuantizer::isInScale (0, 9, 2),
               "A minor contains A and C (root rotates the scale)");
    }

    // -----------------------------------------------------------------------
    beginCase ("F2 pick: scale degree within range, in scale, ratio finite for +-12 st");
    {
        bool inScale = true, inRange = true, ratioOk = true, wide = false;
        for (int type = 0; type < vsx::kNumScaleTypes; ++type)
            for (int root = 0; root < vsx::kNumScaleRoots; ++root)
                for (float range : { 0.f, 0.05f, 0.35f, 0.7f, 1.f })
                    for (float chaos : { 0.f, 0.5f, 1.f })
                    {
                        vsx::ScaleLock lock;
                        lock.on = true; lock.root = root; lock.type = type; lock.range = range;
                        const float lim = lock.effectiveRangeSemis (chaos);
                        for (float u = 0.f; u < 1.f; u += 1.f / 64.f)
                        {
                            const float st = ScaleQuantizer::pickDegree (u, lim, root, type);
                            inScale = inScale && ScaleQuantizer::isInScale ((int) std::lround (st), root, type);
                            bool anyInside = false;
                            for (int o = -(int) std::floor (lim + 1.0e-4f); o <= (int) std::floor (lim + 1.0e-4f); ++o)
                                anyInside = anyInside || ScaleQuantizer::isInScale (o, root, type);
                            // with no scale note inside the range, the nearest one to 0 is used
                            inRange = inRange && std::abs (st) <= 12.f + 1.0e-4f
                                    && (anyInside ? std::abs (st) <= lim + 1.0e-3f
                                                  : st == ScaleQuantizer::quantize (0.f, root, type));
                            const float ratio = lock.pickRatio (u, chaos);
                            ratioOk = ratioOk && std::isfinite (ratio) && ratio >= 0.49f && ratio <= 2.01f;
                            wide = wide || std::abs (st) >= 11.f;
                        }
                    }
        check (inScale, "every picked degree is in scale");
        check (inRange, "picked degrees stay within the (chaos widened) range, never beyond 12 st");
        check (ratioOk, "pitch ratio is finite and within one octave either way");
        check (wide, "the full +-12 range is reachable");

        vsx::ScaleLock a; a.on = true; a.range = 0.2f;
        check (a.effectiveRangeSemis (1.f) > a.effectiveRangeSemis (0.f)
               && a.effectiveRangeSemis (0.f) == 0.2f * 12.f, "chaos widens the pick range");
        a.range = 1.f;
        check (a.effectiveRangeSemis (1.f) == 12.f, "range is capped at an octave");
    }

    // -----------------------------------------------------------------------
    beginCase ("F2/F4 parameters exist, labelled, with the specified defaults");
    {
        VivisectProcessor p;
        auto* on    = dynamic_cast<AudioParameterBool*>   (p.apvts.getParameter (vsx::id::scaleOn));
        auto* root  = dynamic_cast<AudioParameterChoice*> (p.apvts.getParameter (vsx::id::scaleRoot));
        auto* type  = dynamic_cast<AudioParameterChoice*> (p.apvts.getParameter (vsx::id::scaleType));
        auto* range = dynamic_cast<AudioParameterFloat*>  (p.apvts.getParameter (vsx::id::scaleRange));
        auto* dose  = dynamic_cast<AudioParameterFloat*>  (p.apvts.getParameter (vsx::id::dose));
        check (on && root && type && range && dose, "scaleOn/scaleRoot/scaleType/scaleRange/dose all exist");
        if (on && root && type && range && dose)
        {
            check (! on->get(), "Scale Lock is off by default");
            check (root->getIndex() == 0 && root->choices.size() == 12 && root->choices[0] == "C"
                   && root->choices[11] == "B", "root: C..B, default C");
            check (type->choices.size() == 12 && type->getIndex() == 2 && type->choices[2] == "Minor"
                   && type->choices[0] == "Chromatic" && type->choices[11] == "Whole Tone",
                   "type: 12 scales, default Minor");
            check (std::abs (range->get() - 0.35f) < 1.0e-6f, "range default 0.35");
            check (dose->get() == 0.f, "dose default 0");
            check (on->getName (64) == "Scale Lock" && root->getName (64) == "Scale Root"
                   && type->getName (64) == "Scale Type" && range->getName (64) == "Scale Range"
                   && dose->getName (64) == "Dose", "labels match the spec");
        }
        check (p.describeParam (p.apvts.getParameter (vsx::id::dose)->getParameterIndex()) == "DOSE",
               "debug stream names DOSE");

        bool unique = true;
        std::set<String> ids;
        for (auto* par : p.getParameters())
            if (auto* wid = dynamic_cast<AudioProcessorParameterWithID*> (par))
                unique = unique && ids.insert (wid->paramID).second;
        check (unique, "parameter ids remain unique");
    }

    // -----------------------------------------------------------------------
    beginCase ("F4 DOSE mapping: table, identity at 0, monotonic, clamped");
    {
        using namespace vsx;
        // spec k table, probed at x = 0 (full range of motion) and x = 0.5
        check (std::abs (doseUp (0.f, 1.f, dosek::chaos) - 0.60f) < 1.0e-6f, "Chaos k = 0.60");
        check (std::abs (doseUp (0.f, 1.f, dosek::triggerRate) - 0.50f) < 1.0e-6f, "Trigger Rate k = 0.50");
        check (std::abs (doseUp (0.f, 1.f, dosek::reinject) - 0.25f) < 1.0e-6f, "Reinject k = 0.25");
        check (std::abs (doseUp (0.f, 1.f, dosek::scarDrive) - 0.40f) < 1.0e-6f, "SCAR Drive k = 0.40");
        check (std::abs (doseDown (1.f, 1.f, dosek::corruptBits) - 0.50f) < 1.0e-6f,
               "Corrupt Bits k = 0.50 (towards crunchier = fewer bits)");
        check (std::abs (doseDown (1.f, 1.f, dosek::gravityPull) - 0.70f) < 1.0e-6f,
               "Gravity Pull: x - dose*0.30*x");
        check (std::abs (doseUp (0.4f, 0.5f, 0.6f) - (0.4f + 0.5f * 0.6f * 0.6f)) < 1.0e-6f,
               "x_eff = x + dose*k*(1-x)");
        check (std::abs (doseDown (0.8f, 0.5f, 0.3f) - (0.8f - 0.5f * 0.3f * 0.8f)) < 1.0e-6f,
               "x_eff = x - dose*k*x");

        bool identity = true, monoUp = true, monoDown = true, clamped = true;
        for (float x = 0.f; x <= 1.f; x += 0.01f)
        {
            for (float k : { 0.6f, 0.5f, 0.25f, 0.4f, 0.3f })
            {
                uint32_t a, b; const float u0 = doseUp (x, 0.f, k), d0 = doseDown (x, 0.f, k);
                std::memcpy (&a, &u0, 4); std::memcpy (&b, &x, 4);
                identity = identity && a == b;
                std::memcpy (&a, &d0, 4);
                identity = identity && a == b;
                float prevU = x, prevD = x;
                for (float d = 0.f; d <= 1.0001f; d += 0.05f)
                {
                    const float u = doseUp (x, d, k), dn = doseDown (x, d, k);
                    monoUp   = monoUp   && u >= prevU - 1.0e-6f;
                    monoDown = monoDown && dn <= prevD + 1.0e-6f;
                    clamped  = clamped  && u >= 0.f && u <= 1.f && dn >= 0.f && dn <= 1.f;
                    prevU = u; prevD = dn;
                }
                clamped = clamped && doseUp (x, 5.f, k) <= 1.f && doseUp (x, -3.f, k) == x
                                  && doseDown (x, 5.f, k) >= 0.f;
            }
        }
        check (identity, "dose 0 is a bit-exact identity");
        check (monoUp && monoDown, "mapping is monotonic in dose");
        check (clamped, "result is clamped to 0..1 for any dose");
    }

    // -----------------------------------------------------------------------
    beginCase ("F4 DOSE in the DSP: dose 0 is bit-exact, dose > 0 pushes harder, always bounded");
    {
        const int blocks = 300;
        const auto base = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p) { musicalGoldenSetup (p); });
        const auto zero = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
                                         { musicalGoldenSetup (p); musicalSet (p, vsx::id::dose, 0.f); });
        check (zero.hash == base.hash, "dose 0 renders identically to the default");

        const auto dosed = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
                                          { musicalGoldenSetup (p); musicalSet (p, vsx::id::dose, 0.8f); });
        check (dosed.hash != base.hash, "dose 0.8 changes the sound");
        check (dosed.finite && dosed.peak < kCeiling, "dose 0.8 is finite and bounded");

        const auto maxed = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
        {
            musicalGoldenSetup (p);
            musicalSet (p, vsx::id::dose, 1.f);
            musicalSet (p, vsx::id::scarOn, 1.f);
            musicalSet (p, vsx::id::scarDrive, 1.f);
            musicalSet (p, vsx::id::chaos, 1.f);
            musicalSet (p, vsx::id::triggerRate, 1.f);
            musicalSet (p, vsx::id::reinject, 1.f);
        });
        check (maxed.finite && maxed.peak < kCeiling, "everything and dose maxed stays finite and bounded");

        // dose acts on the *modulated* value: a mod slot driving chaos still goes through it
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        musicalSet (p, "mm1_src", p.apvts.getParameter ("mm1_src")->convertTo0to1 (1.f));
        musicalSet (p, "mm1_dst", p.apvts.getParameter ("mm1_dst")->convertTo0to1 (1.f));
        musicalSetReal (p, "mm1_depth", 0.8f);
        musicalSet (p, vsx::id::dose, 1.f);
        const auto v = runBlocks (p, 200, kBlock, kRate, true);
        check (v.finite && v.peak < kCeiling, "dose with an active mod slot is finite and bounded");
    }

    // -----------------------------------------------------------------------
    beginCase ("F2 Scale Lock Off path is bit-exact with the 0.9.3 behaviour");
    {
        const int blocks = 300;
        const auto ref = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p) { musicalGoldenSetup (p); });
       #if JUCE_LINUX
        if (kRate == 48000.0 && kBlock == 512)
            check (ref.hash == 13751873635340590544ull,
                   "render matches the hash recorded from the 0.9.3 build (Linux, 48 kHz, 512)");
       #endif

        // Off: root/type/range must be completely inert.
        const auto tweaked = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
        {
            musicalGoldenSetup (p);
            musicalSet (p, vsx::id::scaleRoot, 0.7f);
            musicalSet (p, vsx::id::scaleType, 0.9f);
            musicalSet (p, vsx::id::scaleRange, 1.f);
        });
        check (tweaked.hash == ref.hash, "with Scale Lock off, root/type/range change nothing");

        const auto on = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
        {
            musicalGoldenSetup (p);
            musicalSet (p, vsx::id::scaleOn, 1.f);
            musicalSet (p, vsx::id::scaleRange, 1.f);
        });
        check (on.hash != ref.hash, "Scale Lock on changes the sound");
        check (on.finite && on.peak < kCeiling, "Scale Lock on is finite and bounded");
    }

    // -----------------------------------------------------------------------
    beginCase ("F2 wiring: with zero range on a chromatic C scale the quantised path equals zero free drift");
    {
        // Free drift is zero at chaos 0 / DRIFT 0 / SPRAY 0; a scale pick of exactly 0 st
        // must therefore be indistinguishable from it in STUTTER, GRANULAR and REVERSE.
        const int blocks = 260;
        const auto off = musicalRender (kRate, kBlock, blocks,
                                        [] (VivisectProcessor& p) { musicalPitchSurgeonsSetup (p, 0.f); });
        const auto lockedZero = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
        {
            musicalPitchSurgeonsSetup (p, 0.f);
            musicalSet (p, vsx::id::scaleOn, 1.f);
            musicalSet (p, vsx::id::scaleType, 0.f);      // Chromatic
            musicalSet (p, vsx::id::scaleRoot, 0.f);
            musicalSet (p, vsx::id::scaleRange, 0.f);
        });
        check (off.hash == lockedZero.hash, "scale pick of 0 st == no drift (all three pitch surgeons)");

        const auto spread = musicalRender (kRate, kBlock, blocks, [] (VivisectProcessor& p)
        {
            musicalPitchSurgeonsSetup (p, 0.f);
            musicalSet (p, vsx::id::scaleOn, 1.f);
            musicalSet (p, vsx::id::scaleType, 0.f);
            musicalSet (p, vsx::id::scaleRange, 0.5f);
        });
        check (spread.hash != off.hash, "a non-zero range actually scatters the pitch");
        check (spread.finite && spread.peak < kCeiling, "scatter output is finite and bounded");

        // each surgeon individually reacts to Scale Lock
        for (int s : { (int) vsx::S_STUTTER, (int) vsx::S_GRANULAR, (int) vsx::S_REVERSE })
        {
            auto only = [s] (VivisectProcessor& p, bool lock)
            {
                musicalPitchSurgeonsSetup (p, 0.f);
                for (int i = 0; i < vsx::kNumSurgeons; ++i)
                    musicalSet (p, vsx::sid (i, "on").toRawUTF8(), i == s ? 1.f : 0.f);
                if (lock)
                {
                    musicalSet (p, vsx::id::scaleOn, 1.f);
                    musicalSet (p, vsx::id::scaleType, 0.f);
                    musicalSet (p, vsx::id::scaleRange, 0.6f);
                }
            };
            const auto a = musicalRender (kRate, kBlock, blocks, [&] (VivisectProcessor& p) { only (p, false); });
            const auto b = musicalRender (kRate, kBlock, blocks, [&] (VivisectProcessor& p) { only (p, true); });
            check (a.hash != b.hash, String ("Scale Lock changes ") + vsx::surgeonName (s));
            check (b.finite && b.peak < kCeiling, String ("Scale Lock output bounded for ") + vsx::surgeonName (s));
        }
    }

    // -----------------------------------------------------------------------
    beginCase ("F2 Scale Lock is finite and bounded for every root/type, any chaos, silence and noise in");
    {
        bool finite = true; float worst = 0.f;
        for (int type = 0; type < vsx::kNumScaleTypes; ++type)
            for (int root : { 0, 5, 11 })
                for (float chaos : { 0.f, 0.6f, 1.f })
                {
                    const auto r = musicalRender (kRate, kBlock, 80, [&] (VivisectProcessor& p)
                    {
                        musicalGoldenSetup (p);
                        musicalSet (p, vsx::id::chaos, chaos);
                        musicalSet (p, vsx::id::scaleOn, 1.f);
                        musicalSetReal (p, vsx::id::scaleType, (float) type);
                        musicalSetReal (p, vsx::id::scaleRoot, (float) root);
                        musicalSet (p, vsx::id::scaleRange, 1.f);
                    });
                    finite = finite && r.finite;
                    worst = jmax (worst, r.peak);
                }
        check (finite, "no NaN/Inf for any scale at full range");
        check (worst < kCeiling, "bounded for any scale at full range (worst " + String (worst, 2) + ")");

        const auto silent = musicalRender (kRate, kBlock, 120, [] (VivisectProcessor& p)
        {
            musicalGoldenSetup (p);
            musicalSet (p, vsx::id::scaleOn, 1.f);
            musicalSet (p, vsx::id::scaleRange, 1.f);
        }, true);
        check (silent.finite && silent.peak < 1.0e-3f, "silence in stays silence out with Scale Lock on");
    }

    // -----------------------------------------------------------------------
    beginCase ("F2/F4 parameters: RESET, RANDOM, MUTATE, locks, A/B, presets");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto val = [&] (const char* pid) { return p.apvts.getParameter (pid)->getValue(); };
        auto setv = [&] (const char* pid, float v) { p.apvts.getParameter (pid)->setValueNotifyingHost (v); };

        // RESET
        setv (vsx::id::scaleOn, 1.f); setv (vsx::id::scaleRoot, 0.5f); setv (vsx::id::scaleType, 0.9f);
        setv (vsx::id::scaleRange, 0.9f); setv (vsx::id::dose, 0.7f);
        p.resetAllToDefaults();
        check (val (vsx::id::scaleOn) < 0.5f && val (vsx::id::dose) == 0.f
               && std::abs (val (vsx::id::scaleRange) - 0.35f) < 1.0e-6f
               && val (vsx::id::scaleRoot) == 0.f
               && p.apvts.getParameter (vsx::id::scaleType)->getValue()
                    == p.apvts.getParameter (vsx::id::scaleType)->getDefaultValue(),
               "RESET restores Scale Lock and Dose defaults");

        // RANDOM treats dose and range as creative, leaves the song's key alone
        setv (vsx::id::scaleRoot, 7.f / 11.f); setv (vsx::id::scaleType, 5.f / 11.f);
        const float rootBefore = val (vsx::id::scaleRoot), typeBefore = val (vsx::id::scaleType);
        bool doseMoved = false, rangeMoved = false, onSeenBoth[2] { false, false }, keyKept = true, inRange = true;
        for (int i = 0; i < 60; ++i)
        {
            p.randomizeAll();
            doseMoved = doseMoved || val (vsx::id::dose) > 0.01f;
            rangeMoved = rangeMoved || std::abs (val (vsx::id::scaleRange) - 0.35f) > 0.01f;
            onSeenBoth[val (vsx::id::scaleOn) > 0.5f ? 1 : 0] = true;
            keyKept = keyKept && std::abs (val (vsx::id::scaleRoot) - rootBefore) < 1.0e-6f
                              && std::abs (val (vsx::id::scaleType) - typeBefore) < 1.0e-6f;
            inRange = inRange && val (vsx::id::dose) <= 0.61f;
        }
        check (doseMoved, "RANDOM moves Dose");
        check (rangeMoved, "RANDOM moves Scale Range");
        check (onSeenBoth[0] && onSeenBoth[1], "RANDOM switches Scale Lock both on and off");
        check (keyKept, "RANDOM never changes Scale Root / Scale Type (the song's key)");
        check (inRange, "RANDOM keeps Dose in a playable range");

        // locks
        p.resetAllToDefaults();
        setv (vsx::id::dose, 0.37f);
        p.setParameterLocked (vsx::id::dose, true);
        setv (vsx::id::scaleOn, 1.f);
        p.setParameterLocked (vsx::id::scaleOn, true);
        for (int i = 0; i < 8; ++i) p.randomizeAll();
        check (std::abs (val (vsx::id::dose) - 0.37f) < 1.0e-5f && val (vsx::id::scaleOn) > 0.5f,
               "locked Dose / Scale Lock survive RANDOM");
        setv (vsx::id::mutationAmount, 1.f);
        for (int i = 0; i < 20; ++i) p.mutateCurrent();
        check (std::abs (val (vsx::id::dose) - 0.37f) < 1.0e-5f && val (vsx::id::scaleOn) > 0.5f,
               "locked Dose / Scale Lock survive MUTATE");
        p.clearParameterLocks();

        // MUTATE moves them
        p.resetAllToDefaults();
        setv (vsx::id::mutationAmount, 1.f);
        setv (vsx::id::scaleRoot, 4.f / 11.f); setv (vsx::id::scaleType, 7.f / 11.f);
        const float r0 = val (vsx::id::scaleRoot), t0 = val (vsx::id::scaleType);
        bool dmoved = false, rmoved = false, keyStill = true;
        for (int i = 0; i < 40; ++i)
        {
            p.mutateCurrent();
            dmoved = dmoved || val (vsx::id::dose) > 0.f;
            rmoved = rmoved || std::abs (val (vsx::id::scaleRange) - 0.35f) > 0.01f;
            keyStill = keyStill && std::abs (val (vsx::id::scaleRoot) - r0) < 1.0e-6f
                                && std::abs (val (vsx::id::scaleType) - t0) < 1.0e-6f;
            check (val (vsx::id::dose) >= 0.f && val (vsx::id::dose) <= 1.f, "mutated dose stays in range");
        }
        check (dmoved && rmoved, "MUTATE moves Dose and Scale Range");
        check (keyStill, "MUTATE leaves Scale Root / Scale Type alone");

        // A/B
        p.resetAllToDefaults();
        setv (vsx::id::dose, 0.4f); setv (vsx::id::scaleOn, 1.f);
        p.selectABSlot (1);
        setv (vsx::id::dose, 0.9f); setv (vsx::id::scaleOn, 0.f);
        p.selectABSlot (0);
        check (std::abs (val (vsx::id::dose) - 0.4f) < 1.0e-5f && val (vsx::id::scaleOn) > 0.5f,
               "A/B swap restores Dose and Scale Lock");
        p.selectABSlot (1);
        check (std::abs (val (vsx::id::dose) - 0.9f) < 1.0e-5f && val (vsx::id::scaleOn) < 0.5f,
               "A/B other slot keeps its own values");
        p.selectABSlot (0);

        // presets: the six original vibes reset dose / scale to defaults
        // (the 0.10.0 presets deliberately use them; see TestMain "0.10.0 factory presets")
        bool presetsClean = true;
        for (int i = 0; i < 6; ++i)
        {
            setv (vsx::id::dose, 0.8f); setv (vsx::id::scaleOn, 1.f); setv (vsx::id::scaleRange, 0.9f);
            p.loadPreset (i);
            presetsClean = presetsClean && val (vsx::id::dose) == 0.f && val (vsx::id::scaleOn) < 0.5f
                && std::abs (val (vsx::id::scaleRange) - 0.35f) < 1.0e-6f;
        }
        check (presetsClean, "original six factory presets carry dose 0 and Scale Lock off");

        // a saved user preset round-trips the new parameters
        setv (vsx::id::dose, 0.55f); setv (vsx::id::scaleOn, 1.f); setv (vsx::id::scaleRoot, 3.f / 11.f);
        setv (vsx::id::scaleType, 8.f / 11.f); setv (vsx::id::scaleRange, 0.8f);
        const auto f = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx-musical-test.vsxpreset");
        check (p.savePresetToFile (f), "user preset saved");
        p.resetAllToDefaults();
        check (p.loadPresetFromFile (f), "user preset loaded");
        check (std::abs (val (vsx::id::dose) - 0.55f) < 1.0e-4f && val (vsx::id::scaleOn) > 0.5f
               && std::abs (val (vsx::id::scaleRoot) - 3.f / 11.f) < 1.0e-4f
               && std::abs (val (vsx::id::scaleType) - 8.f / 11.f) < 1.0e-4f
               && std::abs (val (vsx::id::scaleRange) - 0.8f) < 1.0e-4f,
               "preset file round-trips Dose and Scale Lock");
        f.deleteFile();
    }

    // -----------------------------------------------------------------------
    beginCase ("F2/F4 state: round trip, and 0.9.3 sessions load at defaults and sound identical");
    {
        VivisectProcessor a;
        a.prepareToPlay (kRate, kBlock);
        a.apvts.getParameter (vsx::id::dose)->setValueNotifyingHost (0.42f);
        a.apvts.getParameter (vsx::id::scaleOn)->setValueNotifyingHost (1.f);
        a.apvts.getParameter (vsx::id::scaleRoot)->setValueNotifyingHost (9.f / 11.f);
        a.apvts.getParameter (vsx::id::scaleType)->setValueNotifyingHost (10.f / 11.f);
        a.apvts.getParameter (vsx::id::scaleRange)->setValueNotifyingHost (0.66f);
        MemoryBlock blob;
        a.getStateInformation (blob);

        VivisectProcessor b;
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        bool same = true;
        for (const auto* pid : { vsx::id::dose, vsx::id::scaleOn, vsx::id::scaleRoot, vsx::id::scaleType, vsx::id::scaleRange })
            same = same && std::abs (a.apvts.getParameter (pid)->getValue() - b.apvts.getParameter (pid)->getValue()) < 1.0e-6f;
        check (same, "Dose and Scale Lock survive a state round trip");

        // fabricate a 0.9.3 session: none of the new parameters, no Simple-view setting
        auto xml = musicalStateXml (a);
        check (xml != nullptr, "state serialises");
        if (xml != nullptr)
        {
            for (int i = xml->getNumChildElements() - 1; i >= 0; --i)
            {
                auto* c = xml->getChildElement (i);
                const auto pid = c->getStringAttribute ("id");
                if (pid == vsx::id::dose || pid == vsx::id::scaleOn || pid == vsx::id::scaleRoot
                    || pid == vsx::id::scaleType || pid == vsx::id::scaleRange)
                    xml->removeChildElement (c, true);
                else if (c->hasTagName ("VSX_SETTINGS"))
                {
                    c->removeAttribute ("simpleView");
                    c->removeAttribute ("hintDismissed");
                }
            }
            MemoryBlock old;
            AudioProcessor::copyXmlToBinary (*xml, old);

            // into a fresh instance and into one that currently holds other values
            VivisectProcessor fresh;
            fresh.setStateInformation (old.getData(), (int) old.getSize());
            VivisectProcessor dirty;
            dirty.apvts.getParameter (vsx::id::dose)->setValueNotifyingHost (0.9f);
            dirty.apvts.getParameter (vsx::id::scaleOn)->setValueNotifyingHost (1.f);
            dirty.setStateInformation (old.getData(), (int) old.getSize());

            for (auto* q : { &fresh, &dirty })
            {
                bool defaults = true;
                for (const auto* pid : { vsx::id::dose, vsx::id::scaleOn, vsx::id::scaleRoot, vsx::id::scaleType, vsx::id::scaleRange })
                {
                    auto* par = q->apvts.getParameter (pid);
                    defaults = defaults && std::abs (par->getValue() - par->getDefaultValue()) < 1.0e-6f;
                }
                check (defaults, "0.9.3 session: new parameters at their defaults");
                check (! q->simpleViewEnabled(), "0.9.3 session restores with Simple view OFF");
            }

        }
    }

    // -----------------------------------------------------------------------
    beginCase ("F4 Simple view is a user setting: fresh install ON, old session OFF, never sound state");
    {
        VivisectProcessor fresh;
        check (fresh.simpleViewEnabled(), "fresh install: Simple view defaults ON");
        check (! fresh.firstRunHintDismissed(), "fresh install: first-run hint is shown");

        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        const auto before = snapshotParams (p);
        p.setSimpleViewEnabled (false);
        p.setSimpleViewEnabled (true);
        p.setSimpleViewEnabled (false);
        p.setFirstRunHintDismissed (true);
        check (snapshotParams (p) == before, "toggling Simple view / hint changes no parameter");
        check (! p.simpleViewEnabled() && p.firstRunHintDismissed(), "settings read back");

        // saved with the session...
        MemoryBlock blob;
        p.getStateInformation (blob);
        VivisectProcessor q;
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        check (! q.simpleViewEnabled() && q.firstRunHintDismissed(), "Simple view OFF and dismissed hint survive the session");
        p.setSimpleViewEnabled (true);
        p.getStateInformation (blob);
        q.setStateInformation (blob.getData(), (int) blob.getSize());
        check (q.simpleViewEnabled(), "Simple view ON survives the session");

        // ...but never touched by sound-state operations
        p.setSimpleViewEnabled (false);
        p.setFirstRunHintDismissed (true);
        p.randomizeAll();
        p.mutateCurrent();
        p.resetAllToDefaults();
        p.loadPreset (2);
        p.selectABSlot (1);
        p.selectABSlot (0);
        check (! p.simpleViewEnabled() && p.firstRunHintDismissed(),
               "RANDOM / MUTATE / RESET / presets / A-B never touch Simple view or the hint");
        p.setSimpleViewEnabled (true);
        p.setFirstRunHintDismissed (false);
        const auto f = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx-simple-test.vsxpreset");
        p.savePresetToFile (f);
        p.setSimpleViewEnabled (false);
        p.loadPresetFromFile (f);
        check (! p.simpleViewEnabled(), "loading a preset does not switch Simple view");
        f.deleteFile();

        p.hardResetAllSettings();
        check (p.simpleViewEnabled() && ! p.firstRunHintDismissed(), "Hard Reset returns to fresh-install settings");
    }

    // -----------------------------------------------------------------------
    beginCase ("F4 editor: Simple button hides the advanced panels, changes no parameter, Dose + Scale panel exist");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.setSimpleViewEnabled (false);
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        check (ed != nullptr, "editor created");
        if (ed != nullptr)
        {
            ed->setSize (VivisectView::designWidth(), VivisectView::designHeight());

            vsx::SimpleViewButton* btn = nullptr;
            vsx::FirstRunHint* hint = nullptr;
            vsx::DosePanel* dosePanel = nullptr;
            vsx::ScaleLockPanel* scale = nullptr;
            std::function<void (Component&)> walk = [&] (Component& c)
            {
                if (auto* x = dynamic_cast<vsx::SimpleViewButton*> (&c)) btn = x;
                if (auto* x = dynamic_cast<vsx::FirstRunHint*> (&c)) hint = x;
                if (auto* x = dynamic_cast<vsx::DosePanel*> (&c)) dosePanel = x;
                if (auto* x = dynamic_cast<vsx::ScaleLockPanel*> (&c)) scale = x;
                for (auto* ch : c.getChildren()) walk (*ch);
            };
            walk (*ed);
            check (btn != nullptr && hint != nullptr && dosePanel != nullptr && scale != nullptr,
                   "Simple button, hint strip, Dose knob and Scale Lock panel are in the editor");

            // same path as a mouse click: toggle the state, then fire onClick
            auto clickSimple = [&]
            {
                btn->setToggleState (! btn->getToggleState(), dontSendNotification);
                btn->onClick();
            };
            auto countVisible = [&]
            {
                int n = 0;
                std::function<void (Component&)> cnt = [&] (Component& c)
                {
                    if (! c.isVisible()) return;
                    if (dynamic_cast<vsx::VsxComboBox*> (&c) || dynamic_cast<vsx::VsxSlider*> (&c)) ++n;
                    for (auto* ch : c.getChildren()) cnt (*ch);
                };
                for (auto* ch : ed->getChildren()) cnt (*ch);      // the root itself is never shown
                return n;
            };

            if (btn != nullptr && hint != nullptr && dosePanel != nullptr && scale != nullptr)
            {
                check (hint->isVisible(), "first-run hint is visible until dismissed");
                check (dosePanel->isVisible() && dosePanel->knob().isVisible(), "Dose knob visible");
                const auto before = snapshotParams (p);
                const int fullCount = countVisible();
                check (! btn->isSimple() && scale->isVisible(), "Simple view starts OFF for this instance");

                clickSimple();
                check (p.simpleViewEnabled() && btn->isSimple(), "clicking SIMPLE stores the user setting");
                check (countVisible() < fullCount, "Simple view hides controls");
                check (! scale->isVisible(), "Simple view hides the Scale Lock panel");
                check (dosePanel->isVisible(), "Dose stays visible in Simple view");
                check (snapshotParams (p) == before, "switching to Simple view changes no parameter");

                // hidden controls still work
                p.apvts.getParameter ("mm1_depth")->setValueNotifyingHost (0.9f);
                check (p.apvts.getParameter ("mm1_depth")->getValue() > 0.89f, "hidden controls still accept values");
                p.apvts.getParameter ("mm1_depth")->setValueNotifyingHost (before.at ("mm1_depth"));

                clickSimple();
                check (! p.simpleViewEnabled() && countVisible() == fullCount, "switching back restores every control");
                check (snapshotParams (p) == before, "switching back changes no parameter either");

                hint->onDismiss();
                check (! hint->isVisible() && p.firstRunHintDismissed(), "dismissing the hint hides it and remembers");
            }
        }
        ed.reset();

        // a fresh install opens in Simple view
        VivisectProcessor fresh;
        fresh.prepareToPlay (kRate, kBlock);
        std::unique_ptr<AudioProcessorEditor> ed2 (fresh.createEditor());
        bool simpleOn = false;
        std::function<void (Component&)> walk2 = [&] (Component& c)
        {
            if (auto* x = dynamic_cast<vsx::SimpleViewButton*> (&c)) simpleOn = x->isSimple();
            for (auto* ch : c.getChildren()) walk2 (*ch);
        };
        if (ed2 != nullptr) walk2 (*ed2);
        check (simpleOn, "a fresh install's editor opens in Simple view");
    }
}
