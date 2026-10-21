#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>
#include "Capture.h"
#include "PluginProcessor.h"
#include "Ui.h"

// ============================================================================
//  VIVISECT - F3 capture UI: CAPTURE button, length selector, state read-out
//  and the shelf of the last eight takes.
//
//  The engine lives in the processor, so closing the editor mid-capture only
//  drops this view; the take carries on and finishes on its own.
// ============================================================================
namespace vsx
{
// One line describing the engine, shared by the panel and the Debug window.
juce::String describeCaptureState (const CaptureInfo&);

// ---------------------------------------------------------------------------
//  The shelf: up to eight takes, newest first. Each tile shows a tiny
//  waveform, the file name and the length. Drag a tile out to the DAW or the
//  file manager (or onto Sample A / B in the window); right-click for more.
// ---------------------------------------------------------------------------
class CaptureShelf : public juce::Component,
                     public juce::TooltipClient
{
public:
    static constexpr int kMaxTakes = 8;

    explicit CaptureShelf (VivisectProcessor&);
    ~CaptureShelf() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    // Re-reads the Captures folder. Cheap when nothing changed.
    void refresh();

    int numTakes() const noexcept { return (int) takes.size(); }
    juce::File takeFile (int i) const { return juce::isPositiveAndBelow (i, (int) takes.size()) ? takes[(size_t) i].file : juce::File(); }

    std::function<void (int slot, const juce::File&)> onLoadIntoSlot;

private:
    struct Take
    {
        juce::File file;
        juce::Time modified;
        juce::int64 size = 0;
        double seconds = 0.0;
        std::vector<float> peaks;          // 0..1, one per column; empty until analysed
        bool requested = false;
    };
    struct PeakJob;

    juce::Rectangle<int> tileBounds (int i) const;
    int  tileAt (juce::Point<int>) const;
    void showMenu (int i);
    void askRename (const juce::File&);
    void askDelete (const juce::File&);
    void requestPeaks (Take&);
    void setPeaks (const juce::File&, juce::int64 size, double seconds, std::vector<float> peaks);

    VivisectProcessor& proc;
    std::vector<Take> takes;
    juce::ThreadPool pool { 1 };
    int hovered = -1, pressed = -1;
    juce::Point<int> pressPos;
    bool dragging = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (CaptureShelf)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureShelf)
};

// ---------------------------------------------------------------------------
class CapturePanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit CapturePanel (VivisectProcessor&);
    ~CapturePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The editor routes "load into Sample A / B" through its own feedback path.
    void setLoadIntoSlotHandler (std::function<void (int, const juce::File&)> f) { shelf.onLoadIntoSlot = std::move (f); }

    CaptureShelf& getShelf() noexcept { return shelf; }

private:
    void timerCallback() override;
    void syncLength();
    void showSavedPopup (const CaptureInfo&);

    VivisectProcessor& proc;
    VsxButton captureBtn { "CAPTURE" };
    VsxButton cancelBtn  { "CANCEL" };
    juce::ComboBox lengthBox;
    CaptureShelf shelf;

    int seenCompleted = 0, seenFailed = 0, tick = 0;
    juce::Rectangle<int> statusArea;

    JUCE_DECLARE_WEAK_REFERENCEABLE (CapturePanel)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CapturePanel)
};
} // namespace vsx
