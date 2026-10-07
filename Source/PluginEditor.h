#pragma once
#include <JuceHeader.h>
#include <memory>
#include <utility>
#include <vector>
#include "PluginProcessor.h"
#include "Ui.h"

// The whole UI, laid out at a fixed design size. VivisectEditor below scales
// it as one piece, which is how JUCE wants a zoomable editor built: the host
// owns the editor's own transform, so the zoom lives on this child instead.
class VivisectView : public juce::Component,
                       public vsx::ParamHost,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer
{
public:
    explicit VivisectView (VivisectProcessor&);
    ~VivisectView() override;

    static int designWidth() noexcept;
    static int designHeight() noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    // Drag and drop — any audio file dropped on the editor loads into a slot.
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragMove  (const juce::StringArray&, int, int) override;
    void fileDragExit  (const juce::StringArray&) override;
    void filesDropped  (const juce::StringArray&, int, int) override;

    // vsx::ParamHost — lets every control offer its own right-click menu
    juce::AudioProcessorValueTreeState& params() override { return proc.apvts; }
    void beginMidiLearnFor (const juce::String& paramID) override;
    int  ccForParam (const juce::String& paramID) const override { return proc.getCCForParam (paramID); }
    void clearCCForParam (const juce::String& paramID) override  { proc.clearCCForParam (paramID); }
    bool isParameterLocked (const juce::String& paramID) const override { return proc.isParameterLocked (paramID); }
    void setParameterLocked (const juce::String& paramID, bool locked) override { proc.setParameterLocked (paramID, locked); }
    void clearParameterLocks() override { proc.clearParameterLocks(); }
    int  lockedParameterCount() const override { return proc.lockedParameterCount(); }

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;

    vsx::VsxSlider&   addKnob (const char* pid, const juce::String& caption, const juce::String& tip,
                               vsx::VsxSlider::Size = vsx::VsxSlider::Size::normal);
    vsx::VsxComboBox& addCombo (const char* pid, const juce::StringArray& items,
                                const juce::String& caption, const juce::String& tip);
    vsx::VsxButton&   addParamButton (const char* pid, const juce::String& text, const juce::String& tip);

    void refreshPresetList();
    void chooseSample (int slot);
    void loadSampleWithFeedback (int slot, const juce::File&);
    void showMainMenu();
    void doSaveAs();
    void doOpen();
    void doExportSpecimen (int bitDepth);
    void showOverlay (juce::Component* which);
    void showAudioMidiSettings();

    VivisectProcessor& proc;
    vsx::VsxLookAndFeel lnf;
    vsx::VsxTooltipWindow tips { this };

    // Which slot a drop at this position would land in, and whether to draw the
    // drop hint at all. -1 means no drag is in progress.
    int dropSlotFor (juce::Point<int>) const;
    static bool isAudioFile (const juce::String& path);

    vsx::OutputLed led;
    vsx::DataStream stream;
    vsx::HeartMonitorDisplay monitor;
    vsx::PulseMeter pulse;
    vsx::LevelMeter meter;
    juce::OwnedArray<vsx::SurgeonStrip> strips;
    vsx::HistoryBar history;

    juce::OwnedArray<vsx::VsxSlider>   sliders;
    juce::OwnedArray<vsx::VsxComboBox> combos;
    juce::OwnedArray<vsx::VsxButton>   buttons;
    juce::OwnedArray<SA> sAtt;
    juce::OwnedArray<BA> bAtt;
    juce::OwnedArray<CA> cAtt;
    std::vector<std::pair<juce::Component*, juce::String>> captions;   // combo captions

    // header
    juce::ComboBox  presetBox;
    vsx::VsxButton  menuBtn { "MENU" };
    vsx::VsxButton  aBtn { "A" }, bBtn { "B" }, copyBtn { "COPY" };
    vsx::GearButton gearBtn;

    // action buttons
    vsx::VsxButton randomBtn { "RANDOM" }, mutateBtn { "MUTATE" }, resetBtn { "RESET" }, saveBtn { "FREEZE / SAVE" };

    // overlays
    vsx::HelpPanel    help;
    vsx::OptionsPanel options;
    vsx::DebugPanel   debug;
    vsx::SecretPanel  secret { *this };

    // positioned individually in resized()
    vsx::VsxSlider *kChaos = nullptr, *kPull = nullptr, *kSwing = nullptr, *kDryWet = nullptr,
                   *kTrig = nullptr, *kReinj = nullptr, *kAnalys = nullptr, *kMorph = nullptr,
                   *kDecayT = nullptr, *kScAmt = nullptr,
                   *kL1r = nullptr, *kL2r = nullptr, *kMac1 = nullptr, *kMac2 = nullptr,
                   *kInTrim = nullptr, *kOutTrim = nullptr, *kMutationAmt = nullptr;
    vsx::VsxComboBox *cGrid = nullptr, *cSource = nullptr, *cScMode = nullptr,
                     *cL1s = nullptr, *cL2s = nullptr, *cBuffer = nullptr;
    vsx::VsxButton *bMidi = nullptr, *bPanic = nullptr, *bDecay = nullptr, *bScar = nullptr;
    vsx::VsxSlider *kScarDrive = nullptr, *kScarMix = nullptr;
    vsx::VsxComboBox *mmSrc[vsx::kNumModSlots] {}, *mmDst[vsx::kNumModSlots] {};
    vsx::VsxSlider   *mmDepth[vsx::kNumModSlots] {};

    juce::String learnTarget;
    int dragSlot = -1;

    // The hidden effect's trigger: the identity notch in the top-left corner.
    juce::Rectangle<int> secretHotspot() const { return { 0, 0, 12, 12 }; }
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VivisectView)
};

// ---------------------------------------------------------------------------
//  The editor the host sees: a resizable, fixed-aspect frame that scales the
//  design-size view to fit. The chosen size is remembered with the session,
//  and the first open shrinks to fit screens shorter than the design height.
// ---------------------------------------------------------------------------
class VivisectEditor : public juce::AudioProcessorEditor
{
public:
    explicit VivisectEditor (VivisectProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr float minScale = 0.5f, maxScale = 1.5f;

private:
    VivisectProcessor& proc;
    VivisectView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VivisectEditor)
};
