#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include "Ui.h"

// ============================================================================
//  VIVISECT - "more musical / easier to use" UI pieces (spec F2 + F4)
//    ScaleLockPanel   scale on / root / type / range
//    DosePanel        the DOSE knob
//    SimpleViewButton header toggle for Simple view (a user setting, not a param)
//    FirstRunHint     dismissable strip explaining Dose, RANDOM, MUTATE, Simple
//  The editor only positions these and wires their callbacks.
// ============================================================================
namespace vsx
{
class ScaleLockPanel : public juce::Component
{
public:
    explicit ScaleLockPanel (ParamHost&);
    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int preferredWidth  = 344;
    static constexpr int preferredHeight = 72;

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

public:
    VsxButton   detectBtn { "DETECT" };       // H2: listens to the specimen and sets root + type
    std::function<void()> onDetect;
private:
    VsxButton   onBtn;
    VsxComboBox root, type;
    VsxSlider   range;
    std::unique_ptr<BA> onA;
    std::unique_ptr<CA> rootA, typeA;
    std::unique_ptr<SA> rangeA;
};

// ---------------------------------------------------------------------------
//  H1: header tempo readout. Click = tap tempo; right-click = double / half / clear.
class TempoButton : public VsxButton
{
public:
    TempoButton();
    void mouseDown (const juce::MouseEvent&) override;
    void setTempo (double bpm, bool fromHost);
    std::function<void()> onTap;
    std::function<void (int)> onMenu;          // 1 = double, 2 = half, 3 = clear tapped tempo
    static juce::String labelFor (double bpm, bool fromHost);
};

// ---------------------------------------------------------------------------
class DosePanel : public juce::Component
{
public:
    explicit DosePanel (ParamHost&);
    void resized() override;
    VsxSlider& knob() noexcept { return dose; }

private:
    VsxSlider dose;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
};

// ---------------------------------------------------------------------------
class SimpleViewButton : public VsxButton
{
public:
    SimpleViewButton();
    void setSimple (bool on) { setToggleState (on, juce::dontSendNotification); }
    bool isSimple() const noexcept { return getToggleState(); }
    std::function<void (bool)> onChanged;      // fired from the user's click only
};

// ---------------------------------------------------------------------------
class FirstRunHint : public juce::Component
{
public:
    FirstRunHint();
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void()> onDismiss;
    static juce::String hintText();

private:
    VsxButton gotIt { "GOT IT" };
};
} // namespace vsx
