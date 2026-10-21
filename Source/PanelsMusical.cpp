#include "PanelsMusical.h"

namespace vsx
{
// ===========================================================================
ScaleLockPanel::ScaleLockPanel (ParamHost& host)
    : onBtn ("SCALE LOCK", &host, id::scaleOn),
      root  (host, id::scaleRoot),
      type  (host, id::scaleType),
      range (host, id::scaleRange, "RANGE", VsxSlider::Size::small)
{
    onBtn.setClickingTogglesState (true);
    onBtn.setTooltip ("Scale Lock. STUTTER repeats, GRANULAR grains and REVERSE passes pick a note "
                      "of the chosen scale instead of drifting freely, so the butchery stays in tune.");
    addAndMakeVisible (onBtn);
    onA = std::make_unique<BA> (host.params(), id::scaleOn, onBtn);

    root.addItemList (scaleRootNames(), 1);
    root.setTooltip ("Scale root note (the key of your song).");
    addAndMakeVisible (root);
    rootA = std::make_unique<CA> (host.params(), id::scaleRoot, root);

    type.addItemList (scaleTypeNames(), 1);
    type.setTooltip ("Scale type. Chromatic allows every semitone.");
    addAndMakeVisible (type);
    typeA = std::make_unique<CA> (host.params(), id::scaleType, type);

    range.setTooltip ("How far the pitch scatter may reach, from 0 to plus or minus 12 semitones. "
                      "More chaos widens the pick, but it never leaves the scale.");
    addAndMakeVisible (range);
    rangeA = std::make_unique<SA> (host.params(), id::scaleRange, range);

    setSize (preferredWidth, preferredHeight);
}

void ScaleLockPanel::resized()
{
    auto r = getLocalBounds();
    onBtn.setBounds (r.removeFromLeft (96).withSizeKeepingCentre (96, metric::buttonH + 4));
    r.removeFromLeft (metric::grid);
    range.setBounds (r.removeFromRight (64));
    r.removeFromRight (metric::grid);
    auto combos = r.withSizeKeepingCentre (r.getWidth(), 2 * 14 + metric::buttonH - 4);
    // captions are painted 13px above each combo
    combos.removeFromTop (14);
    auto rootArea = combos.removeFromLeft (juce::jmin (64, combos.getWidth() / 3));
    root.setBounds (rootArea.removeFromTop (metric::buttonH - 4));
    combos.removeFromLeft (metric::gridFine);
    type.setBounds (combos.removeFromTop (metric::buttonH - 4));
}

void ScaleLockPanel::paint (juce::Graphics& g)
{
    draw::label (g, "ROOT", { root.getX() - 2, root.getY() - 13, root.getWidth() + 4, 12 },
                 juce::Justification::centredLeft, col::textMuted, 10.f);
    draw::label (g, "SCALE", { type.getX() - 2, type.getY() - 13, type.getWidth() + 4, 12 },
                 juce::Justification::centredLeft, col::textMuted, 10.f);
}

// ===========================================================================
DosePanel::DosePanel (ParamHost& host)
    : dose (host, id::dose, "DOSE", VsxSlider::Size::large)
{
    dose.setChaosKnob (true);
    dose.setTooltip ("DOSE pushes the whole rack harder with one knob: more chaos, more triggers, "
                     "more reinject, crunchier CORRUPT, harder SCAR and a looser grid. At zero it does nothing.");
    addAndMakeVisible (dose);
    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (host.params(), id::dose, dose);
}
void DosePanel::resized() { dose.setBounds (getLocalBounds()); }

// ===========================================================================
SimpleViewButton::SimpleViewButton() : VsxButton ("SIMPLE")
{
    setClickingTogglesState (true);
    setTooltip ("Simple view hides the mod matrix, routing, sidechain and effect detail panels. "
                "Every hidden control keeps working and nothing is reset.");
    onClick = [this] { if (onChanged) onChanged (getToggleState()); };
}

// ===========================================================================
juce::String FirstRunHint::hintText()
{
    return "NEW HERE?  DOSE pushes the whole rack harder with one knob.  RANDOM rolls a new sound, "
           "MUTATE nudges the current one.  SIMPLE hides the advanced panels.";
}

FirstRunHint::FirstRunHint()
{
    gotIt.setTooltip ("Hide this hint for good.");
    gotIt.onClick = [this] { if (onDismiss) onDismiss(); };
    addAndMakeVisible (gotIt);
}
void FirstRunHint::resized()
{
    auto r = getLocalBounds().reduced (metric::grid, 3);
    gotIt.setBounds (r.removeFromRight (80));
}
void FirstRunHint::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (col::panel.withAlpha (0.94f));
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (col::accent2);
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusPanel, 1.f);
    g.setColour (col::text);
    g.setFont (font::ui (11.f));
    g.drawText (hintText(), getLocalBounds().withTrimmedLeft (metric::pad).withTrimmedRight (96),
                juce::Justification::centredLeft);
}
} // namespace vsx
