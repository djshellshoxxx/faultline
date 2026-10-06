#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <deque>
#include "Theme.h"
#include "Dsp.h"

// ============================================================================
//  VIVISECT — visual layer. Built on the shared visual identity in Theme.h.
// ============================================================================
namespace vsx
{
juce::Path surgeonIconPath (int surgeonIndex);   // drawn in a 0..100 unit box
juce::Path gearIconPath();                       // settings gear, 0..100 unit box

// ---------------------------------------------------------------------------
//  Everything a control needs from the plugin in order to offer its right-click
//  menu. Implemented by the editor so the UI layer stays free of the processor.
// ---------------------------------------------------------------------------
struct ParamHost
{
    virtual ~ParamHost() = default;
    virtual juce::AudioProcessorValueTreeState& params() = 0;
    virtual void beginMidiLearnFor (const juce::String& paramID) = 0;
    virtual int  ccForParam (const juce::String& paramID) const = 0;
    virtual void clearCCForParam (const juce::String& paramID) = 0;
};

// Right-click menu shared by every control: MIDI map / reset / type a value.
void showParamContextMenu (ParamHost&, const juce::String& paramID, juce::Component* target);

// ---------------------------------------------------------------------------
class VsxLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VsxLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int, int, int, int, float, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool, bool) override;
    void drawComboBox (juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawTooltip (juce::Graphics&, const juce::String&, int, int) override;
    void drawPopupMenuBackground (juce::Graphics&, int, int) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
};

// ---------------------------------------------------------------------------
//  Controls. All three carry a parameter id so the right-click menu works on
//  "any control", per the house spec.
// ---------------------------------------------------------------------------
class VsxSlider : public juce::Slider,
                  private juce::Timer
{
public:
    enum class Size { small, normal, large };

    VsxSlider (ParamHost&, juce::String paramID, juce::String caption, Size = Size::normal);

    float animatedPos() const noexcept { return anim; }
    bool  isHovered()   const noexcept { return hover; }
    const juce::String& getCaption() const noexcept { return caption; }
    Size  getKnobSize() const noexcept { return size; }
    bool  isChaosKnob() const noexcept { return chaosStyle; }
    void  setChaosKnob (bool b) { chaosStyle = b; repaint(); }

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseDown  (const juce::MouseEvent&) override;

private:
    void valueChanged() override;
    void timerCallback() override;

    ParamHost& host;
    juce::String pid, caption;
    Size size;
    bool hover = false, chaosStyle = false;
    float anim = 0.f;
    juce::uint32 lastTick = 0;
};

// ---------------------------------------------------------------------------
class VsxComboBox : public juce::ComboBox
{
public:
    VsxComboBox (ParamHost&, juce::String paramID);
    void mouseDown (const juce::MouseEvent&) override;
private:
    ParamHost& host;
    juce::String pid;
};

// ---------------------------------------------------------------------------
class VsxButton : public juce::TextButton,
                  private juce::Timer
{
public:
    VsxButton (const juce::String& text, ParamHost* = nullptr, juce::String paramID = {});
    void mouseDown (const juce::MouseEvent&) override;
    void clicked() override;
    float flashAmount() const noexcept { return flash; }
private:
    void timerCallback() override;
    ParamHost* host = nullptr;
    juce::String pid;
    float flash = 0.f;
};

// ---------------------------------------------------------------------------
class VsxToggle : public juce::ToggleButton
{
public:
    VsxToggle (const juce::String& text, ParamHost&, juce::String paramID);
    void mouseDown (const juce::MouseEvent&) override;
private:
    ParamHost& host;
    juce::String pid;
};

// ---------------------------------------------------------------------------
class HeartMonitorDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit HeartMonitorDisplay (std::function<void (MonitorSnapshot&)> filler);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override { repaint(); }
    std::function<void (MonitorSnapshot&)> fill;
    MonitorSnapshot snap;
    float scan = 0.f;
};

// ---------------------------------------------------------------------------
class PulseMeter : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    PulseMeter (std::function<float()> pulse, std::function<float()> chaos);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    std::function<float()> getPulse, getChaos;
    std::vector<float> trace;
    float nextBeat = 0.4f, sinceBeat = 0.f;
    float beatT = 0.f;
    bool  beating = false;
    juce::Random rng;
    float flash = 0.f;
};

// ---------------------------------------------------------------------------
//  Stereo output meter : smooth gradient, 1px peak hold, mono numeric readout.
// ---------------------------------------------------------------------------
class LevelMeter : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit LevelMeter (std::function<float (int)> peakForChannel);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    std::function<float (int)> getPeak;
    float lvl[2] { 0.f, 0.f };
    float hold[2] { 0.f, 0.f };
    float holdAge[2] { 9.f, 9.f };
    float peakDb = -100.f;
};

// ---------------------------------------------------------------------------
//  Output LED — house spec. Dark grey at -inf, brightening to white as the
//  output approaches 0 dBFS, latching red for as long as the signal is over.
// ---------------------------------------------------------------------------
class OutputLed : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit OutputLed (std::function<float (int)> peakForChannel);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    std::function<float (int)> getPeak;
    float glow = 0.f;      // 0 = unlit, 1 = white at 0 dBFS
    float over = 0.f;      // 1 while clipping, decays once the signal drops
};

// ---------------------------------------------------------------------------
class SurgeonStrip : public juce::Component, private juce::Timer
{
public:
    SurgeonStrip (ParamHost&, int surgeonIndex, std::function<float()> activity);
    void resized() override;
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    int idx;
    std::function<float()> getActivity;
    float act = 0.f;
    VsxToggle onBtn;
    std::unique_ptr<VsxSlider> mix, prob, k1, k2, k3;
    VsxComboBox route;
    std::unique_ptr<BA> onA;
    std::unique_ptr<CA> routeA;
    juce::OwnedArray<SA> knobA;
    juce::Path icon;
};

// ---------------------------------------------------------------------------
class HistoryBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    HistoryBar (std::function<int()> count, std::function<void (float)> scrub)
        : getCount (std::move (count)), onScrub (std::move (scrub))
    {
        setTooltip ("60 seconds of plugin history, recorded twice a second. "
                    "Drag to rewind every control at once.");
    }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { doScrub (e); }
    void mouseDrag (const juce::MouseEvent& e) override { doScrub (e); }
private:
    void doScrub (const juce::MouseEvent& e)
    {
        if (auto w = getWidth()) { hover = juce::jlimit (0.f, 1.f, (float) e.x / (float) w); onScrub (hover); repaint(); }
    }
    std::function<int()> getCount;
    std::function<void (float)> onScrub;
    float hover = 1.f;
};

// ---------------------------------------------------------------------------
//  DataStream — the house "specimen feed". Real internal traffic (MIDI, slice
//  scheduling, clock, parameter moves) scrolling in small light-green type,
//  deliberately low contrast so it reads as texture, not as a read-out.
//
//  Per the house spec the top two and bottom two lines fade to nothing, and
//  the scroll only advances while there is new data — a still feed means a
//  still plugin, which is information in itself.
// ---------------------------------------------------------------------------
// One telemetry event as a line of text. Shared by the decorative feed and by
// the debug window, so the two can never describe the same event differently.
juce::String describeTelemetry (const TelemetryEvent&,
                                const std::function<juce::String (int)>& paramNamer,
                                juce::uint64 index);

class DataStream : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    // The filler drains up to maxEvents into dest and returns how many it wrote.
    using Drain = std::function<int (TelemetryEvent* dest, int maxEvents)>;

    DataStream (Drain drain, std::function<juce::String (int)> paramNamer);

    // Only one consumer may drain the telemetry ring at a time; the editor
    // parks this one whenever the debug window takes over.
    void setPaused (bool b) noexcept { paused = b; }

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    juce::String format (const TelemetryEvent&);

    Drain drainEvents;
    std::function<juce::String (int)> nameParam;
    bool paused = false;

    static constexpr int kMaxLines = 24;       // enough for the tallest gap we use
    static constexpr int kMaxPending = 48;     // burst headroom; older lines drop

    std::deque<juce::String> lines;            // on screen, oldest first
    std::deque<juce::String> pending;          // drained but not yet scrolled in
    float scroll = 0.f;                        // 0..1 through the current row
    juce::uint32 lastTick = 0;
    juce::uint64 counter = 0;
};

// ---------------------------------------------------------------------------
//  Overlay panels (help + options). Both are full-editor overlays with a
//  close button; the editor owns them and toggles visibility.
// ---------------------------------------------------------------------------
class OverlayPanel : public juce::Component
{
public:
    explicit OverlayPanel (juce::String title);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override {}   // swallow clicks
    std::function<void()> onClose;
protected:
    virtual juce::Rectangle<int> contentArea() const;
    juce::String title;
    VsxButton closeBtn { "CLOSE" };
};

// ---------------------------------------------------------------------------
class HelpPanel : public OverlayPanel
{
public:
    HelpPanel();
    void resized() override;
private:
    juce::TextEditor body;
    VsxButton homeBtn    { "HOMEPAGE" };
    VsxButton gitBtn     { "SOURCE" };
    VsxButton mailBtn    { "SUPPORT" };
    VsxButton presetsBtn { "PRESET FOLDER" };
public:
    // include.md asks for the debug feature to be reachable from the help
    // section, so the button lives here and the editor supplies the action.
    VsxButton debugBtn { "DEBUG" };
};

// ---------------------------------------------------------------------------
class OptionsPanel : public OverlayPanel
{
public:
    OptionsPanel();
    void resized() override;

    std::function<void (bool)> onTooltipsChanged;
    std::function<void()>      onAudioMidiSettings;
    void setTooltipsState (bool on) { tooltips.setToggleState (on, juce::dontSendNotification); }
    void setAudioSettingsAvailable (bool);

private:
    void paint (juce::Graphics&) override;
    juce::ToggleButton tooltips { "Hover tool tips" };
    VsxButton audioBtn { "AUDIO / MIDI DEVICE SETUP" };
    bool audioAvailable = false;
};

// ---------------------------------------------------------------------------
//  DebugPanel — reached from the help page. Shows the plugin's raw internals
//  live, and owns the two support files: the crash log and the troubleshooting
//  report. Crash logging is OFF on every load, by design.
// ---------------------------------------------------------------------------
class DebugPanel : public OverlayPanel, private juce::Timer
{
public:
    DebugPanel();
    void resized() override;

    std::function<int (TelemetryEvent*, int)> drain;
    std::function<juce::String (int)>         nameParam;
    std::function<juce::String()>             rawState;     // live parameter dump
    std::function<juce::StringArray()>        warnings;     // misconfiguration hints
    std::function<void (bool)>                onCrashLogToggled;
    std::function<juce::String()>             crashLogPath;
    std::function<void()>                     onExportTroubleshooting;
    std::function<void()>                     onHardReset;
    std::function<void()>                     onOpenLogFolder;

    void setCrashLogState (bool on, const juce::String& path);

private:
    void paint (juce::Graphics&) override;
    void timerCallback() override;

    juce::TextEditor feed, state;
    juce::ToggleButton crashLog { "Create log file on crash" };
    VsxButton exportBtn  { "EXPORT TROUBLESHOOTING FILE" };
    VsxButton hardBtn    { "RESET ALL SETTINGS TO DEFAULT" };
    VsxButton logsBtn    { "OPEN LOG FOLDER" };
    juce::String logPath;
    juce::uint64 counter = 0;
    int stateTick = 0;
};

// ---------------------------------------------------------------------------
//  SecretPanel — the hidden FLATLINE tab. Slides out of the corner notch when
//  the notch is clicked, and closes again from its own button. Nothing about
//  it is advertised anywhere else in the interface.
// ---------------------------------------------------------------------------
class SecretPanel : public juce::Component, private juce::Timer
{
public:
    explicit SecretPanel (ParamHost&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override {}   // swallow clicks

    std::function<void()> onClose;
    std::function<void()> onAnimate;   // fired every animation frame

    // Animated reveal, so it reads as something opening rather than appearing.
    void reveal (bool shouldShow);
    bool isRevealed() const noexcept { return want; }
    float openAmount() const noexcept { return open; }

private:
    void timerCallback() override;

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

    VsxToggle onBtn;
    VsxSlider tone, bleed, mix;
    VsxButton closeBtn { "CLOSE" };
    std::unique_ptr<BA> onA;
    juce::OwnedArray<SA> att;

    bool  want = false;
    float open = 0.f;
    juce::uint32 lastTick = 0;
};

// ---------------------------------------------------------------------------
//  Small gear button for the header (settings), per the house header spec.
// ---------------------------------------------------------------------------
class GearButton : public juce::Button
{
public:
    GearButton();
    void paintButton (juce::Graphics&, bool, bool) override;
private:
    juce::Path gear;
};

// ---------------------------------------------------------------------------
//  Tooltip window that can be switched off from the Options page.
// ---------------------------------------------------------------------------
class VsxTooltipWindow : public juce::TooltipWindow
{
public:
    explicit VsxTooltipWindow (juce::Component* parent)
        : juce::TooltipWindow (parent, metric::tooltipDelay) {}

    void setTipsEnabled (bool b) { enabled = b; if (! b) hideTip(); }
    juce::String getTipFor (juce::Component& c) override
        { return enabled ? juce::TooltipWindow::getTipFor (c) : juce::String(); }
private:
    bool enabled = true;
};

} // namespace vsx
