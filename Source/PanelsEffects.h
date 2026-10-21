#pragma once
#include <JuceHeader.h>
#include <memory>
#include "Theme.h"
#include "Ui.h"

// ============================================================================
//  VIVISECT — effects panel (0.10.0): TOURNIQUET (filter) + CLOT (delay).
//  Self-contained: owns its controls and parameter attachments, and takes only
//  a ParamHost (for the shared right-click menu, MIDI learn and locks).
// ============================================================================
namespace vsx
{
class EffectsPanel : public juce::Component, private juce::Timer
{
public:
    explicit EffectsPanel (ParamHost&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateFreeState();

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    // TOURNIQUET
    VsxButton tqOn;
    VsxComboBox tqMode;
    VsxSlider tqCutoff, tqRes, tqEnv, tqMix;
    // CLOT
    VsxButton clotOn;
    VsxComboBox clotTime;
    VsxSlider clotFree, clotFeedback, clotTone, clotSpread, clotMix;

    std::unique_ptr<BA> tqOnA, clotOnA;
    std::unique_ptr<CA> tqModeA, clotTimeA;
    juce::OwnedArray<SA> knobA;
    bool freeActive = true;
    juce::Rectangle<int> tqArea, clotArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectsPanel)
};

} // namespace vsx
