#pragma once
#include <JuceHeader.h>
#include <memory>

// ============================================================================
//  VIVISECT — parameter definitions
//  6 parallel "surgeons" + a single Chaos axis + independent Rhythmic Gravity.
// ============================================================================

namespace vsx
{
static constexpr int kNumSurgeons = 6;
static constexpr int kNumModSlots = 4;

enum SurgeonIndex { S_STUTTER = 0, S_GRANULAR, S_REVERSE, S_CORRUPT, S_REORDER, S_FREEZE };
enum SurgeonParameter { SP_ON = 0, SP_MIX, SP_PROB, SP_P1, SP_P2, SP_P3, SP_ROUTE };

namespace id
{
    // ---- global -----------------------------------------------------------
    constexpr auto bufferBars     = "bufferBars";
    constexpr auto chaos          = "chaos";
    constexpr auto gravityGrid    = "gravityGrid";
    constexpr auto gravityPull    = "gravityPull";
    constexpr auto swing          = "swing";
    constexpr auto dryWet         = "dryWet";
    constexpr auto triggerRate    = "triggerRate";
    constexpr auto reinject       = "reinject";
    constexpr auto analysisInform = "analysisInform";
    constexpr auto sourceSel      = "sourceSel";
    constexpr auto morph          = "morph";
    constexpr auto inputTrim      = "inputTrim";
    constexpr auto outputTrim     = "outputTrim";
    constexpr auto midiMode       = "midiMode";
    constexpr auto mutationAmount = "mutationAmount";

    // ---- SCAR: post-rack master texture effect ----------------------------
    constexpr auto scarOn          = "scarOn";
    constexpr auto scarDrive       = "scarDrive";
    constexpr auto scarMix         = "scarMix";

    // --- FLATLINE: the hidden effect behind the corner notch ----------------
    constexpr auto flatOn         = "flatOn";
    constexpr auto flatTone       = "flatTone";
    constexpr auto flatBleed      = "flatBleed";
    constexpr auto flatMix        = "flatMix";
    constexpr auto panicFreeze    = "panicFreeze";
    constexpr auto decayArm       = "decayArm";
    constexpr auto decayTime      = "decayTime";
    constexpr auto scMode         = "scMode";
    constexpr auto scAmount       = "scAmount";

    // ---- mod matrix -----------------------------------------------------
    constexpr auto lfo1Rate  = "lfo1Rate";
    constexpr auto lfo1Shape = "lfo1Shape";
    constexpr auto lfo2Rate  = "lfo2Rate";
    constexpr auto lfo2Shape = "lfo2Shape";
    constexpr auto macro1    = "macro1";
    constexpr auto macro2    = "macro2";
}

inline juce::String surgeonTag (int i)
{
    static const char* t[] { "st", "gr", "rv", "cr", "ro", "fz" };
    return juce::String (t[juce::jlimit (0, 5, i)]);
}
inline juce::String sid (int i, const char* suffix) { return surgeonTag (i) + "_" + suffix; }

// Audio-thread lookup table. `sid()` is convenient for UI and state code, but
// constructing its juce::String result inside processBlock allocates. Keep the
// real-time path on stable string literals instead.
inline const char* sidRaw (int surgeon, SurgeonParameter parameter) noexcept
{
    static constexpr const char* ids[kNumSurgeons][7] {
        { "st_on", "st_mix", "st_prob", "st_p1", "st_p2", "st_p3", "st_route" },
        { "gr_on", "gr_mix", "gr_prob", "gr_p1", "gr_p2", "gr_p3", "gr_route" },
        { "rv_on", "rv_mix", "rv_prob", "rv_p1", "rv_p2", "rv_p3", "rv_route" },
        { "cr_on", "cr_mix", "cr_prob", "cr_p1", "cr_p2", "cr_p3", "cr_route" },
        { "ro_on", "ro_mix", "ro_prob", "ro_p1", "ro_p2", "ro_p3", "ro_route" },
        { "fz_on", "fz_mix", "fz_prob", "fz_p1", "fz_p2", "fz_p3", "fz_route" }
    };
    const int s = juce::jlimit (0, kNumSurgeons - 1, surgeon);
    const int p = juce::jlimit (0, 6, (int) parameter);
    return ids[s][p];
}

inline const char* surgeonName (int i)
{
    static const char* n[] { "STUTTER", "GRANULAR", "REVERSE", "CORRUPT", "REORDER", "FREEZE" };
    return n[juce::jlimit (0, 5, i)];
}

inline const char* paramLabel (int surgeon, int p)
{
    static const char* L[6][3] = {
        { "LEN",    "RATCHET", "DRIFT"  },   // stutter
        { "SIZE",   "DENSITY", "SPRAY"  },   // granular
        { "LEN",    "BLOOM",   "TAIL"   },   // reverse
        { "BITS",   "RATE",    "MODE"   },   // corrupt
        { "SLICES", "PERM",    "REPEAT" },   // reorder
        { "OFFSET", "LENGTH",  "BLUR"   },   // freeze
    };
    return L[juce::jlimit (0, 5, surgeon)][juce::jlimit (0, 2, p)];
}

inline juce::StringArray modSourceNames()
{
    return { "Off", "LFO 1", "LFO 2", "Env Follow", "Macro 1", "Macro 2", "Rnd Walk" };
}
inline juce::StringArray modDestNames()
{
    return { "Off", "Chaos", "Grav Pull", "Swing", "Trig Rate", "Reinject", "Morph",
             "St Drift", "Gr Density", "Gr Spray", "Cr Bits", "Cr Rate", "Fz Blur" };
}

// ----------------------------------------------------------------------------
inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using F = juce::AudioParameterFloat;
    using C = juce::AudioParameterChoice;
    using B = juce::AudioParameterBool;
    using R = juce::NormalisableRange<float>;
    auto pid = [] (const char* s) { return juce::ParameterID { s, 1 }; };

    juce::AudioProcessorValueTreeState::ParameterLayout p;

    p.add (std::make_unique<C> (pid (id::bufferBars), "Buffer", juce::StringArray { "4 Bars", "8 Bars", "16 Bars" }, 1));
    p.add (std::make_unique<F> (pid (id::chaos), "Chaos", R { 0.f, 1.f, 0.0001f }, 0.12f));
    p.add (std::make_unique<C> (pid (id::gravityGrid), "Grid",
        juce::StringArray { "1/16", "1/32", "1/8 Triplet", "1/8 Dotted", "Free" }, 0));
    p.add (std::make_unique<F> (pid (id::gravityPull), "Gravity Pull", R { 0.f, 1.f, 0.0001f }, 0.8f));
    p.add (std::make_unique<F> (pid (id::swing), "Swing", R { -1.f, 1.f, 0.0001f }, 0.f));
    p.add (std::make_unique<F> (pid (id::dryWet), "Dry/Wet", R { 0.f, 1.f, 0.0001f }, 0.9f));
    p.add (std::make_unique<F> (pid (id::triggerRate), "Trigger Rate", R { 0.f, 1.f, 0.0001f }, 0.32f));
    p.add (std::make_unique<F> (pid (id::reinject), "Reinject", R { 0.f, 1.f, 0.0001f }, 0.f));
    p.add (std::make_unique<F> (pid (id::analysisInform), "Analysis Inform", R { 0.f, 1.f, 0.0001f }, 0.4f));
    p.add (std::make_unique<C> (pid (id::sourceSel), "Source",
        juce::StringArray { "Live", "Sample A", "Sample B", "Morph A/B" }, 0));
    p.add (std::make_unique<F> (pid (id::morph), "Morph", R { 0.f, 1.f, 0.0001f }, 0.5f));
    p.add (std::make_unique<F> (pid (id::inputTrim), "Input Trim", R { -24.f, 24.f, 0.01f }, 0.f));
    p.add (std::make_unique<F> (pid (id::outputTrim), "Output Trim", R { -24.f, 24.f, 0.01f }, 0.f));
    p.add (std::make_unique<B> (pid (id::midiMode), "MIDI Mode", false));
    p.add (std::make_unique<F> (pid (id::mutationAmount), "Mutation Amount",
                                R { 0.f, 1.f, 0.0001f }, 0.20f));

    p.add (std::make_unique<B> (pid (id::scarOn), "Scar", false));
    p.add (std::make_unique<F> (pid (id::scarDrive), "Scar Drive", R { 0.f, 1.f, 0.0001f }, 0.35f));
    p.add (std::make_unique<F> (pid (id::scarMix), "Scar Mix", R { 0.f, 1.f, 0.0001f }, 0.45f));

    // FLATLINE. Hidden in the UI until the corner notch is clicked, but a real
    // parameter set all the same, so it automates and saves like anything else.
    p.add (std::make_unique<B> (pid (id::flatOn), "Flatline", false));
    p.add (std::make_unique<F> (pid (id::flatTone), "Flatline Tone",
                                R { 55.f, 1760.f, 0.01f, 0.35f }, 440.f));
    p.add (std::make_unique<F> (pid (id::flatBleed), "Flatline Bleed", R { 0.f, 1.f, 0.0001f }, 0.75f));
    p.add (std::make_unique<F> (pid (id::flatMix), "Flatline Mix", R { 0.f, 1.f, 0.0001f }, 0.35f));
    p.add (std::make_unique<B> (pid (id::panicFreeze), "Panic Freeze", false));
    p.add (std::make_unique<B> (pid (id::decayArm), "Decay Arm", false));
    p.add (std::make_unique<F> (pid (id::decayTime), "Decay Time", R { 1.f, 120.f, 0.1f, 0.4f }, 30.f));
    p.add (std::make_unique<C> (pid (id::scMode), "SC Mode", juce::StringArray { "SC to Main", "Main to B" }, 0));
    p.add (std::make_unique<F> (pid (id::scAmount), "SC Amount", R { 0.f, 1.f, 0.0001f }, 0.5f));

    p.add (std::make_unique<F> (pid (id::lfo1Rate), "LFO 1 Rate", R { 0.01f, 24.f, 0.001f, 0.3f }, 1.f));
    p.add (std::make_unique<C> (pid (id::lfo1Shape), "LFO 1 Shape", juce::StringArray { "Sine", "Tri", "Saw", "Square", "S&H" }, 0));
    p.add (std::make_unique<F> (pid (id::lfo2Rate), "LFO 2 Rate", R { 0.01f, 24.f, 0.001f, 0.3f }, 0.25f));
    p.add (std::make_unique<C> (pid (id::lfo2Shape), "LFO 2 Shape", juce::StringArray { "Sine", "Tri", "Saw", "Square", "S&H" }, 1));
    p.add (std::make_unique<F> (pid (id::macro1), "Macro 1", R { 0.f, 1.f, 0.0001f }, 0.f));
    p.add (std::make_unique<F> (pid (id::macro2), "Macro 2", R { 0.f, 1.f, 0.0001f }, 0.f));

    for (int s = 0; s < kNumSurgeons; ++s)
    {
        const juce::String nm = surgeonName (s);
        const bool defOn = (s == S_STUTTER);
        p.add (std::make_unique<B> (pid (sid (s, "on").toRawUTF8()), nm + " On", defOn));
        p.add (std::make_unique<F> (pid (sid (s, "mix").toRawUTF8()), nm + " Mix", R { 0.f, 1.f, 0.0001f }, 1.f));
        p.add (std::make_unique<F> (pid (sid (s, "prob").toRawUTF8()), nm + " Prob", R { 0.f, 1.f, 0.0001f }, 0.55f));
        p.add (std::make_unique<F> (pid (sid (s, "p1").toRawUTF8()), nm + " " + paramLabel (s, 0), R { 0.f, 1.f, 0.0001f }, 0.35f));
        p.add (std::make_unique<F> (pid (sid (s, "p2").toRawUTF8()), nm + " " + paramLabel (s, 1), R { 0.f, 1.f, 0.0001f }, 0.4f));
        p.add (std::make_unique<F> (pid (sid (s, "p3").toRawUTF8()), nm + " " + paramLabel (s, 2), R { 0.f, 1.f, 0.0001f }, 0.3f));
        p.add (std::make_unique<C> (pid (sid (s, "route").toRawUTF8()), nm + " Route In",
            juce::StringArray { "Off", "from S1", "from S2", "from S3", "from S4", "from S5", "from S6" }, 0));
    }

    for (int m = 0; m < kNumModSlots; ++m)
    {
        const juce::String pre = "mm" + juce::String (m + 1) + "_";
        p.add (std::make_unique<C> (pid ((pre + "src").toRawUTF8()), "Mod " + juce::String (m + 1) + " Src", modSourceNames(), 0));
        p.add (std::make_unique<C> (pid ((pre + "dst").toRawUTF8()), "Mod " + juce::String (m + 1) + " Dst", modDestNames(), 0));
        p.add (std::make_unique<F> (pid ((pre + "depth").toRawUTF8()), "Mod " + juce::String (m + 1) + " Depth",
            juce::NormalisableRange<float> { -1.f, 1.f, 0.0001f }, 0.f));
    }

    return p;
}
} // namespace vsx
