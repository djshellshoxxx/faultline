#include "PanelsEffects.h"
#include "Parameters.h"

namespace vsx
{
EffectsPanel::EffectsPanel (ParamHost& h)
    : tqOn ("TOURNIQUET", &h, id::tqOn),
      tqMode (h, id::tqMode),
      tqCutoff (h, id::tqCutoff, "CUTOFF", VsxSlider::Size::small),
      tqRes    (h, id::tqRes,    "RESO",   VsxSlider::Size::small),
      tqEnv    (h, id::tqEnv,    "ENV",    VsxSlider::Size::small),
      tqMix    (h, id::tqMix,    "MIX",    VsxSlider::Size::small),
      clotOn ("CLOT", &h, id::clotOn),
      clotTime (h, id::clotTime),
      clotFree     (h, id::clotFree,     "FREE",   VsxSlider::Size::small),
      clotFeedback (h, id::clotFeedback, "FDBK",   VsxSlider::Size::small),
      clotTone     (h, id::clotTone,     "TONE",   VsxSlider::Size::small),
      clotSpread   (h, id::clotSpread,   "SPREAD", VsxSlider::Size::small),
      clotMix      (h, id::clotMix,      "MIX",    VsxSlider::Size::small)
{
    for (auto* b : { &tqOn, &clotOn })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    tqOnA   = std::make_unique<BA> (h.params(), id::tqOn, tqOn);
    clotOnA = std::make_unique<BA> (h.params(), id::clotOn, clotOn);

    tqMode.addItemList ({ "Low-pass", "Band-pass", "High-pass", "Notch" }, 1);
    clotTime.addItemList ({ "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1/1", "Free" }, 1);
    addAndMakeVisible (tqMode);
    addAndMakeVisible (clotTime);
    tqModeA   = std::make_unique<CA> (h.params(), id::tqMode, tqMode);
    clotTimeA = std::make_unique<CA> (h.params(), id::clotTime, clotTime);

    struct K { VsxSlider* s; const char* pid; };
    for (auto k : { K { &tqCutoff, id::tqCutoff }, K { &tqRes, id::tqRes }, K { &tqEnv, id::tqEnv },
                    K { &tqMix, id::tqMix }, K { &clotFree, id::clotFree },
                    K { &clotFeedback, id::clotFeedback }, K { &clotTone, id::clotTone },
                    K { &clotSpread, id::clotSpread }, K { &clotMix, id::clotMix } })
    {
        addAndMakeVisible (k.s);
        knobA.add (new SA (h.params(), k.pid, *k.s));
    }

    const juce::String post = " Post-rack effect: it processes the whole plug-in output, after SCAR.";
    tqOn.setTooltip ("TOURNIQUET on/off. A resonant filter that clamps the signal off." + post
                     + " Off is a bit-exact bypass.");
    tqMode.setTooltip ("TOURNIQUET filter type: low-pass, band-pass, high-pass or notch." + post);
    tqCutoff.setTooltip ("TOURNIQUET CUTOFF. Filter frequency, 20 Hz to 20 kHz." + post
                         + " Mod matrix destination: TQ Cutoff.");
    tqRes.setTooltip ("TOURNIQUET RESONANCE. Emphasis at the cutoff (Q 0.5 to 18). It rings hard "
                      "at the top but never runs away." + post + " Mod matrix destination: TQ Resonance.");
    tqEnv.setTooltip ("TOURNIQUET ENV. How far the input level pushes the cutoff, up to four "
                      "octaves either way. Centre is off." + post);
    tqMix.setTooltip ("TOURNIQUET MIX. Dry/wet for the filter. At zero the stage is bypassed." + post);

    clotOn.setTooltip ("CLOT on/off. A tempo-synced stereo delay." + post + " Off is a bit-exact "
                       "bypass and empties the delay line.");
    clotTime.setTooltip ("CLOT TIME. Note length from the host tempo (120 BPM when there is none), "
                         "or Free to use the FREE knob." + post);
    clotFree.setTooltip ("CLOT FREE. Delay time in milliseconds, 20 to 2000. Only used when TIME is Free." + post);
    clotFeedback.setTooltip ("CLOT FEEDBACK. How much of each echo is fed back in, 0 to 0.95. A soft "
                             "clipper in the loop keeps it bounded." + post + " Mod matrix destination: Clot Feedback.");
    clotTone.setTooltip ("CLOT TONE. Low-pass damping inside the feedback loop, 200 Hz to 16 kHz. "
                         "Lower values make each repeat darker." + post);
    clotSpread.setTooltip ("CLOT SPREAD. 0 is a mono delay, 1 is a full ping-pong between left and right." + post);
    clotMix.setTooltip ("CLOT MIX. Dry/wet for the delay. At zero the stage is bypassed." + post
                        + " Mod matrix destination: Clot Mix.");

    updateFreeState();
    startTimerHz (10);
}

void EffectsPanel::updateFreeState()
{
    freeActive = clotTime.getSelectedItemIndex() == 8;
    const float a = freeActive ? 1.f : 0.4f;
    if (clotFree.getAlpha() != a)
        clotFree.setAlpha (a);
}

void EffectsPanel::timerCallback() { updateFreeState(); }

void EffectsPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    draw::panelShadow (g, b, (float) metric::radiusPanel);
    g.setColour (col::panel);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (col::edge);
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusPanel, 1.f);

    draw::sectionHeader (g, "TOURNIQUET", tqArea.withHeight (14), col::accent2);
    draw::sectionHeader (g, "CLOT", clotArea.withHeight (14), col::accent);
    draw::label (g, "post-rack", { tqArea.getRight() - 64, tqArea.getY(), 64, 14 },
                 juce::Justification::centredRight, col::textMuted, 9.f);
    draw::label (g, "post-rack", { clotArea.getRight() - 64, clotArea.getY(), 64, 14 },
                 juce::Justification::centredRight, col::textMuted, 9.f);

    g.setColour (col::edge);
    g.fillRect (clotArea.getX() - metric::grid / 2, tqArea.getY(), 1, tqArea.getHeight());
}

void EffectsPanel::resized()
{
    auto r = getLocalBounds().reduced (metric::grid);
    const int half = (r.getWidth() - metric::grid) / 2;
    tqArea = r.removeFromLeft (half);
    r.removeFromLeft (metric::grid);
    clotArea = r;

    auto layout = [] (juce::Rectangle<int> a, VsxButton& on, VsxComboBox& combo,
                      std::initializer_list<VsxSlider*> knobs)
    {
        a.removeFromTop (14 + metric::gridFine);
        auto row = a.removeFromTop (metric::buttonH - 4);
        on.setBounds (row.removeFromLeft (juce::jmin (96, row.getWidth() / 2)));
        row.removeFromLeft (metric::grid / 2);
        combo.setBounds (row);
        a.removeFromTop (metric::gridFine);
        const int w = juce::jmax (36, a.getWidth() / (int) knobs.size());
        for (auto* k : knobs)
            k->setBounds (a.removeFromLeft (w));
    };
    layout (tqArea, tqOn, tqMode, { &tqCutoff, &tqRes, &tqEnv, &tqMix });
    layout (clotArea, clotOn, clotTime, { &clotFree, &clotFeedback, &clotTone, &clotSpread, &clotMix });
}

} // namespace vsx
