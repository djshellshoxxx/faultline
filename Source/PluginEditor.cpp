#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #define VSX_HAS_STANDALONE_SETTINGS 1
#endif

using namespace vsx;

namespace
{
    // ---- layout, all multiples of the 8px base grid (4px for fine detail) -----
    //
    //  Full view is two columns under the header, then two full-width rows:
    //
    //    header 32
    //    LEFT 700                      | RIGHT 612
    //    rack: 6 surgeon strips        | monitor, chaos pulse
    //    modulation (+ data stream)    | master knobs incl. DOSE, sidechain, snap, source
    //    RANDOM / MUTATE / RESET / SAVE| I/O + level meter, MIDI / PANIC / DECAY buttons
    //                                  | post-rack row: SCAR, TOURNIQUET, CLOT
    //                                  | Scale Lock
    //    capture shelf            (full width)
    //    history bar              (full width)
    //    footer
    //
    //  Simple view keeps the same window size and re-flows into a narrow rack on
    //  the left and a large monitor + master row on the right (layoutSimple).
    constexpr int kW          = 1360;
    constexpr int kHeaderH    = metric::header;    // 32
    constexpr int kG          = metric::grid;      // 8
    constexpr int kF          = metric::gridFine;  // 4
    constexpr int kPad        = metric::pad;       // 16
    constexpr int kSectionH   = 20;      // band for a section header + its hint text
    constexpr int kStripH     = 72;      // small knob: 12 value row + 36 body + 14 arc + 12 caption = 74 incl. margins
    constexpr int kModHeadH   = 74;
    constexpr int kModRowH    = 24;
    constexpr int kActionH    = 74;      // MUTATE AMT is a small knob
    constexpr int kHistoryH   = 24;
    constexpr int kFooterH    = 16;
    constexpr int kCaptureH   = 48;

    constexpr int kColL       = 700;
    constexpr int kColGap     = kPad;
    constexpr int kColR       = kW - 2 * kPad - kColL - kColGap;   // 612
    constexpr int kSimpleColL = 330;

    constexpr int kLargeKnobH = 102;     // 12 + (64 + 14) + 12
    constexpr int kSmallKnobH = 74;      // 12 + (36 + 14) + 12
    constexpr int kComboH     = metric::buttonH - 4;               // 24
    constexpr int kBtnH       = metric::buttonH + 4;               // 32

    // The left column's height decides the body height.
    constexpr int kBodyH = kSectionH + kNumSurgeons * kStripH + kG
                         + kSectionH + kModHeadH + kNumModSlots * kModRowH + kG + kActionH;   // 732
    constexpr int kBodyTop = kHeaderH + kG;

    constexpr int kH = kBodyTop + kBodyH + kG + kCaptureH + kG + kHistoryH + kG + kFooterH;
    static_assert (kH <= 900, "default window must stay at or under 900 px tall");
}

// ===========================================================================
//  Control factories
// ===========================================================================
VsxSlider& VivisectView::addKnob (const char* pid, const juce::String& caption,
                                    const juce::String& tip, VsxSlider::Size size)
{
    auto* s = new VsxSlider (*this, pid, caption, size);
    s->setTooltip (tip);
    addAndMakeVisible (s);
    sAtt.add (new SA (proc.apvts, pid, *s));
    sliders.add (s);
    return *s;
}

VsxComboBox& VivisectView::addCombo (const char* pid, const juce::StringArray& items,
                                       const juce::String& caption, const juce::String& tip)
{
    auto* c = new VsxComboBox (*this, pid);
    c->addItemList (items, 1);
    c->setTooltip (tip);
    addAndMakeVisible (c);
    cAtt.add (new CA (proc.apvts, pid, *c));
    combos.add (c);
    if (caption.isNotEmpty()) captions.push_back ({ c, caption });
    return *c;
}

VsxButton& VivisectView::addParamButton (const char* pid, const juce::String& text, const juce::String& tip)
{
    auto* b = new VsxButton (text, this, pid);
    b->setClickingTogglesState (true);
    b->setTooltip (tip);
    addAndMakeVisible (b);
    bAtt.add (new BA (proc.apvts, pid, *b));
    buttons.add (b);
    return *b;
}

// ===========================================================================
VivisectView::VivisectView (VivisectProcessor& p)
    : proc (p),
      led ([&p] (int ch) { return p.outputPeak (ch); }),
      stream ([&p] (vsx::TelemetryEvent* dest, int max) { return p.telemetry().drain (dest, max); },
              [&p] (int idx) { return p.describeParam (idx); }),
      monitor ([&p] (MonitorSnapshot& s) { p.buildMonitorSnapshot (s); }),
      pulse ([&p] { return p.buildPulse(); }, [&p] { return p.apvts.getRawParameterValue (id::chaos)->load(); }),
      meter ([&p] (int ch) { return p.outputPeak (ch); }),
      history ([&p] { return p.historyCount(); }, [&p] (float x) { p.rewindToNormalized (x); })
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (monitor);
    addAndMakeVisible (pulse);
    addAndMakeVisible (meter);
    addAndMakeVisible (history);

    for (int s = 0; s < kNumSurgeons; ++s)
        strips.add (new SurgeonStrip (*this, s, [&p, s] { return p.surgeonActivity (s); }));
    for (auto* st : strips) addAndMakeVisible (st);

    // -- master --------------------------------------------------------------
    kChaos  = &addKnob (id::chaos, "CHAOS",
                        "The single order-to-chaos axis. Left: grid-locked and repeatable. "
                        "Right: randomised positions, open feedback, no grid.",
                        VsxSlider::Size::large);
    kChaos->setChaosKnob (true);

    kPull   = &addKnob (id::gravityPull, "GRAVITY", "How hard slice positions are dragged back onto the grid.");
    kSwing  = &addKnob (id::swing, "SWING", "Pushes odd steps late. Below centre pushes them early.");
    kDryWet = &addKnob (id::dryWet, "DRY / WET", "Wet mix. The dry signal ducks under surgeon activity.");
    kTrig   = &addKnob (id::triggerRate, "TRIGGER", "Global surgeon activity - how often anything fires at all.");
    kReinj  = &addKnob (id::reinject, "REINJECT", "How much butchered output is written back into the specimen.");
    kAnalys = &addKnob (id::analysisInform, "ANALYSIS",
                        "Chance a slice is picked by analysis - brightest, loudest, most tonal, "
                        "nearest transient - instead of by the grid.", VsxSlider::Size::small);
    kMorph  = &addKnob (id::morph, "MORPH A/B", "Morph position between Sample A and Sample B.",
                        VsxSlider::Size::small);
    kScAmt  = &addKnob (id::scAmount, "SC AMT", "How often a sidechain transient triggers a surgeon.",
                        VsxSlider::Size::small);

    cGrid   = &addCombo (id::gravityGrid, { "1/16", "1/32", "1/8 Triplet", "1/8 Dotted", "Free" },
                         "SNAP", "The grid slices are pulled towards.");
    cSource = &addCombo (id::sourceSel, { "Live", "Sample A", "Sample B", "Morph A/B" },
                         "SOURCE", "What the surgeons read: the live buffer, a loaded sample, or a morph.");
    cScMode = &addCombo (id::scMode, { "SC to Main", "Main to B" },
                         "SIDECHAIN", "SC to Main: sidechain transients glitch the main signal. "
                                      "Main to B: main transients fire surgeons reading Sample B.");

    // -- i/o -----------------------------------------------------------------
    cBuffer  = &addCombo (id::bufferBars, { "4 Bars", "8 Bars", "16 Bars" },
                          "BUFFER", "How much of the incoming signal the specimen buffer holds.");
    kInTrim  = &addKnob (id::inputTrim, "IN TRIM", "Gain into the plugin, in dB.", VsxSlider::Size::small);
    kOutTrim = &addKnob (id::outputTrim, "OUT TRIM", "Gain out of the plugin, in dB.", VsxSlider::Size::small);
    kDecayT  = &addKnob (id::decayTime, "DECAY T", "How long Decay Over Time takes to reach maximum, in seconds.",
                         VsxSlider::Size::small);

    // -- action buttons ------------------------------------------------------
    bMidi  = &addParamButton (id::midiMode, "MIDI MODE",
                              "Suppresses the auto-scheduler. Surgeons only fire from MIDI notes C3-F3.");
    bPanic = &addParamButton (id::panicFreeze, "PANIC FREEZE",
                              "Freezes the specimen buffer and holds it on the Freeze surgeon.");
    bDecay = &addParamButton (id::decayArm, "DECAY OVER TIME",
                              "Arms a slow drive of chaos and corruption to maximum over Decay T seconds.");
    bScar  = &addParamButton (id::scarOn, "SCAR",
                              "Master texture effect. Adds bounded soft saturation and torn transient edges.");
    kScarDrive = &addKnob (id::scarDrive, "SCAR DRIVE",
                           "How hard SCAR pushes the post-rack signal into its soft clipper.",
                           VsxSlider::Size::small);
    kScarMix = &addKnob (id::scarMix, "SCAR MIX",
                         "Dry/wet amount for the SCAR master texture effect.",
                         VsxSlider::Size::small);

    // -- modulation ----------------------------------------------------------
    kL1r  = &addKnob (id::lfo1Rate, "LFO 1", "LFO 1 rate, in Hz.", VsxSlider::Size::small);
    kL2r  = &addKnob (id::lfo2Rate, "LFO 2", "LFO 2 rate, in Hz.", VsxSlider::Size::small);
    kMac1 = &addKnob (id::macro1, "MACRO 1", "Macro 1. CC20 by default.", VsxSlider::Size::small);
    kMac2 = &addKnob (id::macro2, "MACRO 2", "Macro 2. CC21 by default.", VsxSlider::Size::small);
    cL1s  = &addCombo (id::lfo1Shape, { "Sine", "Tri", "Saw", "Square", "S&H" }, {}, "LFO 1 shape.");
    cL2s  = &addCombo (id::lfo2Shape, { "Sine", "Tri", "Saw", "Square", "S&H" }, {}, "LFO 2 shape.");

    for (int m = 0; m < kNumModSlots; ++m)
    {
        const juce::String pre = "mm" + juce::String (m + 1) + "_";
        const juce::String n = juce::String (m + 1);
        mmSrc[m]   = &addCombo ((pre + "src").toRawUTF8(), modSourceNames(), {}, "Mod slot " + n + " source.");
        mmDst[m]   = &addCombo ((pre + "dst").toRawUTF8(), modDestNames(),   {}, "Mod slot " + n + " destination.");
        mmDepth[m] = &addKnob  ((pre + "depth").toRawUTF8(), {}, "Mod slot " + n + " depth. Bipolar - centre is off.");
        mmDepth[m]->setSliderStyle (juce::Slider::LinearHorizontal);
    }

    // -- header --------------------------------------------------------------
    addAndMakeVisible (presetBox);
    presetBox.setTextWhenNothingSelected ("PRESET / VIBE");
    presetBox.setTooltip ("Factory characters, then your own presets marked with an asterisk.");
    refreshPresetList();
    presetBox.onChange = [this]
    {
        // Load by the name on screen, not by row number: the folder may have
        // gained or lost files since this menu was built.
        const auto name = presetBox.getText();
        if (name.isNotEmpty() && ! proc.loadPresetByName (name))
        {
            // The file is gone. Rebuild the list rather than leave the box
            // showing a preset that is not loaded.
            refreshPresetList();
        }
    };

    addAndMakeVisible (menuBtn);
    menuBtn.setTooltip ("Save, Save As, Open, Export, Options and Help.");
    menuBtn.onClick = [this] { showMainMenu(); };

    addAndMakeVisible (led);
    addAndMakeVisible (effectsPanel);   // ==== EFFECTS PANEL hook ====

    // The specimen feed lives in the dead space beside the mod matrix. It is
    // decorative, so it must never steal a click or a tooltip from a control.
    addAndMakeVisible (stream);
    stream.toBack();

    addAndMakeVisible (gearBtn);
    gearBtn.onClick = [this] { showOverlay (&options); };

    for (auto* b : { &aBtn, &bBtn })
    {
        addAndMakeVisible (b);
        b->setClickingTogglesState (false);
        b->setTooltip ("A / B compare. Switch between two snapshots of the sound controls.");
    }
    addAndMakeVisible (copyBtn);
    copyBtn.setTooltip ("Copy the current snapshot across to the other slot.");
    aBtn.onClick    = [this] { proc.selectABSlot (0); aBtn.setToggleState (true, juce::dontSendNotification);
                               bBtn.setToggleState (false, juce::dontSendNotification); };
    bBtn.onClick    = [this] { proc.selectABSlot (1); bBtn.setToggleState (true, juce::dontSendNotification);
                               aBtn.setToggleState (false, juce::dontSendNotification); };
    copyBtn.onClick = [this] { proc.copyABSlot(); };
    aBtn.setToggleState (proc.currentABSlot() == 0, juce::dontSendNotification);
    bBtn.setToggleState (proc.currentABSlot() == 1, juce::dontSendNotification);

    // ==== UNDO/CAPTURE hook ====
    setWantsKeyboardFocus (true);
    addAndMakeVisible (undoBtn);
    addAndMakeVisible (redoBtn);
    undoBtn.setTooltip ("Undo the last change to the sound controls (Ctrl/Cmd+Z).");
    redoBtn.setTooltip ("Redo the change you just undid (Ctrl/Cmd+Shift+Z or Ctrl+Y).");
    undoBtn.onClick = [this] { proc.undo(); };
    redoBtn.onClick = [this] { proc.redo(); };
    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
    addAndMakeVisible (capturePanel);
    capturePanel.setLoadIntoSlotHandler ([this] (int slot, const juce::File& f) { loadSampleWithFeedback (slot, f); });
    // ==== end UNDO/CAPTURE hook ====

    // -- random / reset / save ----------------------------------------------
    addAndMakeVisible (randomBtn);
    randomBtn.setColour (juce::TextButton::buttonOnColourId, col::accent2);
    randomBtn.setTooltip ("A completely new set of settings. The first press randomises from here; "
                          "every press after that wipes back to defaults first.");
    randomBtn.onClick = [this] { proc.randomizeAll(); };

    addAndMakeVisible (mutateBtn);
    mutateBtn.setColour (juce::TextButton::buttonOnColourId, col::accent);
    mutateBtn.setTooltip ("Create a nearby variation using MUTATE AMT. Locked controls stay unchanged.");
    mutateBtn.onClick = [this] { proc.mutateCurrent(); };
    kMutationAmt = &addKnob (id::mutationAmount, "MUTATE AMT",
                             "How far MUTATE moves creative controls. Low values make subtle variations; high values make larger changes.",
                             VsxSlider::Size::small);

    addAndMakeVisible (resetBtn);
    resetBtn.setTooltip ("Put every control back to its default.");
    resetBtn.onClick = [this] { proc.resetAllToDefaults(); };

    addAndMakeVisible (saveBtn);
    saveBtn.setTooltip ("Save the current state as a timestamped user preset.");
    saveBtn.onClick = [this]
    {
        proc.saveUserPreset ("Frozen " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"));
        refreshPresetList();
    };

    // -- overlays ------------------------------------------------------------
    addChildComponent (help);
    addChildComponent (options);
    addChildComponent (debug);
    help.onClose    = [this] { showOverlay (nullptr); };
    options.onClose = [this] { showOverlay (nullptr); };
    debug.onClose   = [this] { showOverlay (&help); };   // back to where it came from

    // -- debug window --------------------------------------------------------
    help.debugBtn.onClick = [this] { showOverlay (&debug); };

    debug.drain     = [this] (vsx::TelemetryEvent* d, int m) { return proc.telemetry().drain (d, m); };
    debug.nameParam = [this] (int i) { return proc.describeParam (i); };
    debug.warnings  = [this] { return proc.detectMisconfiguration(); };

    debug.rawState = [this]
    {
        juce::String s;
        s << "sample rate  " << juce::String (proc.getSampleRate(), 1) << " Hz\n"
          << "block size   " << proc.getBlockSize() << "\n"
          << "wrapper      " << juce::AudioProcessor::getWrapperTypeDescription (proc.wrapperType) << "\n"
          << "sample A     " << (proc.slotHasSample (0) ? "loaded" : "empty") << "\n"
          << "sample B     " << (proc.slotHasSample (1) ? "loaded" : "empty") << "\n"
          << "A/B slot     " << (proc.currentABSlot() == 0 ? "A" : "B") << "\n"
          << "history      " << proc.historyCount() << " frames\n"
          // ==== UNDO/CAPTURE hook ====
          << "undo / redo  " << proc.undoDepth() << " / " << proc.redoDepth() << " steps\n"
          << "capture      " << vsx::describeCaptureState (proc.captureState()) << "\n"
          << "capture dir  " << proc.captureDirectory().getFullPathName() << "\n"
          // ==== end UNDO/CAPTURE hook ====
          << "\n";
        for (auto* parameter : proc.getParameters())
            if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
                s << wid->paramID.paddedRight (' ', 18) << " " << parameter->getCurrentValueAsText()
                  << "   [" << juce::String (parameter->getValue(), 4) << "]\n";
        return s;
    };

    debug.onCrashLogToggled = [this] (bool on)
    {
        proc.setCrashLogEnabled (on);
        debug.setCrashLogState (proc.isCrashLogEnabled(),
                                proc.currentCrashLogFile().getFullPathName());
    };

    debug.onOpenLogFolder = [this]
    {
        auto d = vsx::userLogDir();
        d.createDirectory();
        d.revealToUser();
    };

    debug.onExportTroubleshooting = [this]
    {
        const auto f = proc.suggestedTroubleshootingFile();
        if (proc.writeTroubleshootingFile (f))
        {
            f.revealToUser();
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::InfoIcon)
                    .withTitle ("Troubleshooting file written")
                    .withMessage ("Saved to:\n" + f.getFullPathName()
                                  + "\n\nSend this to " + juce::String (vsx::product::support)
                                  + " along with a description of the problem.")
                    .withButton ("OK"),
                nullptr);
        }
        else
        {
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Could not write the troubleshooting file")
                    .withMessage ("Vivisect could not write to:\n" + f.getFullPathName()
                                  + "\n\nCheck that the folder is writable.")
                    .withButton ("OK"),
                nullptr);
        }
    };

    debug.onHardReset = [this]
    {
        proc.hardResetAllSettings();
        refreshPresetList();
        options.setTooltipsState (proc.tooltipsEnabled());
        tips.setTipsEnabled (proc.tooltipsEnabled());
        debug.setCrashLogState (proc.isCrashLogEnabled(),
                                proc.currentCrashLogFile().getFullPathName());
    };

    debug.setCrashLogState (proc.isCrashLogEnabled(),
                            proc.currentCrashLogFile().getFullPathName());

    // ==== MUSICAL/DOSE hook ====
    scalePanel = std::make_unique<ScaleLockPanel> (*this);
    dosePanel  = std::make_unique<DosePanel> (*this);
    simpleBtn  = std::make_unique<SimpleViewButton>();
    hintStrip  = std::make_unique<FirstRunHint>();
    addAndMakeVisible (*scalePanel);
    addAndMakeVisible (*dosePanel);
    addAndMakeVisible (*simpleBtn);
    addChildComponent (*hintStrip);
    scaleToggle.setClickingTogglesState (true);
    scaleToggle.setTooltip ("Scale Lock on / off. The root, scale and range live in the full view.");
    addChildComponent (scaleToggle);
    scaleToggleA = std::make_unique<BA> (proc.apvts, id::scaleOn, scaleToggle);
    simpleBtn->onChanged = [this] (bool on) { proc.setSimpleViewEnabled (on); applySimpleView (on); };
    hintStrip->onDismiss = [this] { proc.setFirstRunHintDismissed (true); hintStrip->setVisible (false); };
    hintStrip->setVisible (! proc.firstRunHintDismissed());
    // ==== end MUSICAL/DOSE hook ====

    // -- the hidden effect ---------------------------------------------------
    addChildComponent (secret);
    secret.onClose   = [this] { secret.reveal (false); };
    secret.onAnimate = [this] { resized(); };
    options.setTooltipsState (proc.tooltipsEnabled());
    options.onTooltipsChanged = [this] (bool on)
    {
        proc.setTooltipsEnabled (on);
        tips.setTipsEnabled (on);
    };
   #if defined (VSX_HAS_STANDALONE_SETTINGS)
    options.setAudioSettingsAvailable (juce::StandalonePluginHolder::getInstance() != nullptr);
    options.onAudioMidiSettings = [this] { showAudioMidiSettings(); };
   #else
    options.setAudioSettingsAvailable (false);
   #endif

    tips.setTipsEnabled (proc.tooltipsEnabled());

    applySimpleView (proc.simpleViewEnabled());     // MUSICAL/DOSE hook

    setSize (kW, kH);
    startTimerHz (8);
}

VivisectView::~VivisectView() { vsx::dismissValueEntry(); setLookAndFeel (nullptr); }

// ==== UNDO/CAPTURE hook ====
bool VivisectView::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    if (! (m.isCommandDown() || m.isCtrlDown()) || m.isAltDown())
        return false;

    int code = k.getKeyCode();
    if (code >= 'a' && code <= 'z') code -= 'a' - 'A';
    if (code == 'Z')
    {
        if (m.isShiftDown()) proc.redo(); else proc.undo();
        return true;
    }
    if (code == 'Y' && ! m.isShiftDown())
    {
        proc.redo();
        return true;
    }
    return false;
}
// ==== end UNDO/CAPTURE hook ====

// ===========================================================================
void VivisectView::beginMidiLearnFor (const juce::String& paramID)
{
    proc.beginMidiLearn (paramID);
    learnTarget = paramID;
    repaint();
}


void VivisectView::timerCallback()
{
    // ==== UNDO/CAPTURE hook ====
    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
    undoBtn.setTooltip (proc.undoLabel());
    redoBtn.setTooltip (proc.redoLabel());
    // ==== end UNDO/CAPTURE hook ====
    // ==== MUSICAL/DOSE hook ==== follow a session restored while the editor is open
    if (proc.simpleViewEnabled() != simpleView) applySimpleView (proc.simpleViewEnabled());
    // ==== end MUSICAL/DOSE hook ====

    // the audio thread clears the learn target as soon as it sees a CC
    const auto t = proc.midiLearnTarget();
    if (t != learnTarget) { learnTarget = t; repaint(); }
}

void VivisectView::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);
    int i = 1;
    for (const auto& n : proc.getPresetNames())
        presetBox.addItem (n, i++);
}

void VivisectView::showOverlay (juce::Component* which)
{
    help.setVisible (which == &help);
    options.setVisible (which == &options);
    debug.setVisible (which == &debug);

    // Hand the telemetry ring to whichever view is actually on screen.
    stream.setPaused (which != nullptr);
    if (which != nullptr) { which->setBounds (getLocalBounds()); which->toFront (true); }
}

void VivisectView::chooseSample (int slot)
{
    chooser = std::make_unique<juce::FileChooser> ("Load specimen into " + juce::String (slot == 0 ? "A" : "B"),
                                                   juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, slot] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) loadSampleWithFeedback (slot, f);
                          });
}

void VivisectView::loadSampleWithFeedback (int slot, const juce::File& f)
{
    if (proc.loadSampleInto (slot, f))
        return;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Could not load sample")
            .withMessage ("Vivisect could not read:\n" + f.getFullPathName()
                          + "\n\nUse a WAV, AIFF, FLAC, MP3 or OGG file that is not empty.")
            .withButton ("OK"),
        nullptr);
}

void VivisectView::doSaveAs()
{
    auto dir = proc.getUserPresetDir();
    dir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save preset", dir, "*.vsxpreset");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              proc.savePresetToFile (f.withFileExtension ("vsxpreset"));
                              refreshPresetList();
                          });
}

void VivisectView::doOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open preset", proc.getUserPresetDir(), "*.vsxpreset");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadPresetFromFile (f);
                          });
}

void VivisectView::doExportSpecimen (int bitDepth)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Export specimen buffer", dir, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, bitDepth] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              const auto wavFile = f.withFileExtension ("wav");
                              double seconds = 0.0;
                              const bool ok = proc.exportSpecimenToWav (wavFile, bitDepth, &seconds);
                              const auto success = "Saved successfully.\n\n"
                                                   "Name: " + wavFile.getFileName() + "\n"
                                                   "Location: " + wavFile.getParentDirectory().getFullPathName() + "\n"
                                                   "Length: " + juce::String (seconds, 2) + " seconds\n"
                                                   "Quality: " + juce::String (bitDepth) + "-bit PCM WAV";
                              juce::NativeMessageBox::showAsync (
                                  juce::MessageBoxOptions()
                                      .withIconType (ok ? juce::MessageBoxIconType::InfoIcon
                                                        : juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Export specimen")
                                      .withMessage (ok ? success
                                                       : "Nothing in the specimen buffer to export yet, "
                                                         "or the destination could not be written.")
                                      .withButton ("OK"),
                                  nullptr);
                          });
}

void VivisectView::showAudioMidiSettings()
{
   #if defined (VSX_HAS_STANDALONE_SETTINGS)
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->showAudioSettingsDialog();
   #endif
}

// ===========================================================================
//  The hidden effect. include.md asks for a specific pixel in the GUI to
//  reveal it; the identity notch in the top-left corner is that spot. It is
//  drawn on every build as the brand mark, so it hides in plain sight.
// ===========================================================================
void VivisectView::mouseDown (const juce::MouseEvent& e)
{
    if (secretHotspot().contains (e.getPosition()))
    {
        secret.reveal (! secret.isRevealed());
        if (secret.isRevealed())
            secret.toFront (false);      // false: do not steal keyboard focus
        resized();
    }
}

// ===========================================================================
//  Drag and drop — drop any audio file to load it as a specimen. Left half of
//  the window is slot A, right half is slot B, so a drop is aimed, not random.
// ===========================================================================
bool VivisectView::isAudioFile (const juce::String& path)
{
    static const juce::StringArray exts { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg" };
    for (const auto& e : exts)
        if (path.endsWithIgnoreCase (e))
            return true;
    return false;
}

int VivisectView::dropSlotFor (juce::Point<int> p) const
{
    return p.x < getWidth() / 2 ? 0 : 1;
}

bool VivisectView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void VivisectView::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dragSlot = dropSlotFor ({ x, y });
    repaint();
}

void VivisectView::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int s = dropSlotFor ({ x, y });
    if (s != dragSlot) { dragSlot = s; repaint(); }
}

void VivisectView::fileDragExit (const juce::StringArray&)
{
    dragSlot = -1;
    repaint();
}

void VivisectView::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int slot = dropSlotFor ({ x, y });
    dragSlot = -1;
    repaint();

    // Take the first audio file in the drop; loading a folder of them into one
    // slot would just mean the last one silently wins.
    for (const auto& f : files)
    {
        if (! isAudioFile (f))
            continue;

        const juce::File file (f);
        if (file.existsAsFile())
            loadSampleWithFeedback (slot, file);
        return;
    }
}

void VivisectView::showMainMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Save");
    m.addItem (2, "Save As...");
    m.addItem (3, "Open...");
    m.addSeparator();
    m.addItem (4, "Load Sample A...");
    m.addItem (5, "Load Sample B...");
    m.addItem (6, "Clear Sample A", proc.slotHasSample (0));
    m.addItem (7, "Clear Sample B", proc.slotHasSample (1));
    m.addSeparator();
    juce::PopupMenu exportMenu;
    exportMenu.addItem (81, "16-bit PCM WAV");
    exportMenu.addItem (82, "24-bit PCM WAV");
    exportMenu.addItem (83, "32-bit PCM WAV");
    m.addSubMenu ("Export Specimen to WAV", exportMenu);
    m.addSeparator();
    m.addItem (11, "Open Preset Folder");
    m.addItem (12, "Show Last Saved Preset", proc.getLastPresetFile().existsAsFile());
    m.addSeparator();
    m.addItem (9,  "Options...");
    m.addItem (10, "Help...");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn),
                     [this] (int choice)
                     {
                         switch (choice)
                         {
                             case 1:  // No file yet: Save behaves like Save As rather than doing nothing.
                                      if (proc.getLastPresetFile() == juce::File()) doSaveAs();
                                      else { proc.savePresetToFile (proc.getLastPresetFile()); refreshPresetList(); }
                                      break;
                             case 2:  doSaveAs();   break;
                             case 3:  doOpen();     break;
                             case 4:  chooseSample (0); break;
                             case 5:  chooseSample (1); break;
                             case 6:  proc.clearSampleSlot (0); break;
                             case 7:  proc.clearSampleSlot (1); break;
                             case 81: doExportSpecimen (16); break;
                             case 82: doExportSpecimen (24); break;
                             case 83: doExportSpecimen (32); break;
                             case 9:  showOverlay (&options); break;
                             case 10: showOverlay (&help);    break;
                             case 11: { auto d = proc.getUserPresetDir();
                                        d.createDirectory(); d.revealToUser(); } break;
                             case 12: proc.getLastPresetFile().revealToUser(); break;
                             default: break;
                         }
                     });
}

// ===========================================================================
void VivisectView::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // -- header strip --------------------------------------------------------
    auto top = getLocalBounds().removeFromTop (kHeaderH);
    g.setColour (col::panel);
    g.fillRect (top);
    g.setColour (col::text);
    g.setFont (font::ui (14.f, true));
    g.drawText ("VIVISECT", top.withTrimmedLeft (kPad + 22).withWidth (120), juce::Justification::centredLeft);

    if (learnTarget.isNotEmpty())
    {
        auto banner = top.withTrimmedLeft (kPad + 150).withWidth (360);
        g.setColour (col::accent);
        g.setFont (font::ui (11.f, true));
        auto* p = proc.apvts.getParameter (learnTarget);
        g.drawText ("MIDI LEARN: " + (p != nullptr ? p->getName (32).toUpperCase() : learnTarget)
                        + "  -  MOVE A CONTROLLER",
                    banner, juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (col::textMuted);
        g.setFont (font::ui (11.f));
        g.drawText ("sample butchery  -  the incoming signal is a specimen, not a signal",
                    top.withTrimmedLeft (kPad + 150).withWidth (392), juce::Justification::centredLeft);
    }

    draw::separator (g, { 0, kHeaderH, getWidth(), 1 });

    // -- combo captions ------------------------------------------------------
    for (auto& c : captions)
    {
        if (! c.first->isVisible()) continue;      // MUSICAL/DOSE hook: hidden by Simple view
        auto b = c.first->getBounds();
        draw::label (g, c.second, { b.getX() - 6, b.getY() - 13, b.getWidth() + 12, 12 },
                     juce::Justification::centredLeft, col::textMuted, 10.f);
    }

    // -- section headers + separators (laid out in resized()) ----------------
    for (auto& h : headers)
    {
        draw::sectionHeader (g, h.text, { h.area.getX(), h.area.getY(), 240, 12 });
        if (h.hint.isNotEmpty())
        {
            g.setColour (col::textMuted);
            g.setFont (font::ui (11.f));
            g.drawText (h.hint, h.area.getX() + 128, h.area.getY(), h.area.getWidth() - 128, 12,
                        juce::Justification::centredLeft);
        }
        draw::separator (g, { h.area.getX(), h.area.getY() + 16, h.area.getWidth(), 1 });
    }
    draw::separator (g, { kPad, history.getY() - 4, getWidth() - 2 * kPad, 1 });

    // -- footer : version, 9px muted mono, bottom right ---------------------
    g.setColour (col::textMuted);
    g.setFont (font::mono (9.f));
    g.drawText ("v" JucePlugin_VersionString,
                getLocalBounds().removeFromBottom (kFooterH).withTrimmedRight (kPad),
                juce::Justification::centredRight);

    // -- drag and drop hint --------------------------------------------------
    if (dragSlot >= 0)
    {
        auto b = getLocalBounds().toFloat().reduced (2.f);
        g.setColour (col::accent2.withAlpha (0.10f));
        g.fillRoundedRectangle (b, (float) metric::radiusWindow);
        g.setColour (col::accent2);
        g.drawRoundedRectangle (b.reduced (1.f), (float) metric::radiusWindow, 2.f);

        auto tag = juce::Rectangle<int> (0, 0, 300, 40).withCentre (getLocalBounds().getCentre());
        draw::panelShadow (g, tag.toFloat(), (float) metric::radiusPanel);
        g.setColour (col::panel);
        g.fillRoundedRectangle (tag.toFloat(), (float) metric::radiusPanel);
        g.setColour (col::accent2);
        g.drawRoundedRectangle (tag.toFloat().reduced (0.5f), (float) metric::radiusPanel, 1.f);
        draw::label (g, dragSlot == 0 ? "DROP TO LOAD SPECIMEN A" : "DROP TO LOAD SPECIMEN B",
                     tag, juce::Justification::centred, col::text, 11.f);
    }

    // -- window edge + identity notch ---------------------------------------
    g.setColour (col::edge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), (float) metric::radiusWindow, 1.f);
    draw::signatureNotch (g);
}

// ===========================================================================
namespace
{
    // Spread widths over a row: equal gaps, first item flush left, last flush
    // right. Each item is centred vertically in the row at the given height.
    void justify (juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Component*, int>> items,
                  int height)
    {
        int total = 0;
        for (auto& it : items) total += it.second;
        const int n = (int) items.size();
        const int gap = n > 1 ? juce::jmax (kF, (row.getWidth() - total) / (n - 1)) : 0;
        int x = row.getX();
        for (auto& it : items)
        {
            it.first->setBounds (juce::Rectangle<int> (x, row.getY(), it.second, row.getHeight())
                                     .withSizeKeepingCentre (it.second, height));
            x += it.second + gap;
        }
    }

    void resize (juce::Component* c, int w, int h, int dy = 0)
    {
        c->setBounds (c->getBounds().withSizeKeepingCentre (w, h).translated (0, dy));
    }
}

void VivisectView::resized()
{
    headers.clear();
    auto r = getLocalBounds();

    // -- header --------------------------------------------------------------
    auto top = r.removeFromTop (kHeaderH).reduced (kPad, 4);
    led.setBounds (top.getX(), top.getY() + (top.getHeight() - 12) / 2, 12, 12);
    auto right = top;
    gearBtn.setBounds (right.removeFromRight (24).withSizeKeepingCentre (24, 24));
    right.removeFromRight (kG);
    bBtn.setBounds    (right.removeFromRight (32).withSizeKeepingCentre (32, 24));
    right.removeFromRight (2);
    aBtn.setBounds    (right.removeFromRight (32).withSizeKeepingCentre (32, 24));
    right.removeFromRight (kG / 2);
    copyBtn.setBounds (right.removeFromRight (56).withSizeKeepingCentre (56, 24));
    right.removeFromRight (kG);
    presetBox.setBounds (right.removeFromRight (208).withSizeKeepingCentre (208, 24));
    right.removeFromRight (kG / 2);
    menuBtn.setBounds (right.removeFromRight (72).withSizeKeepingCentre (72, 24));
    // ==== UNDO/CAPTURE hook ====
    right.removeFromRight (kG);
    redoBtn.setBounds (right.removeFromRight (52).withSizeKeepingCentre (52, 24));
    right.removeFromRight (2);
    undoBtn.setBounds (right.removeFromRight (52).withSizeKeepingCentre (52, 24));
    // ==== end UNDO/CAPTURE hook ====
    // ==== MUSICAL/DOSE hook ====
    right.removeFromRight (kG / 2);
    simpleBtn->setBounds (right.removeFromRight (72).withSizeKeepingCentre (72, 24));
    // ==== end MUSICAL/DOSE hook ====

    // -- body: one function per view ----------------------------------------
    const juce::Rectangle<int> body (kPad, kBodyTop, getWidth() - 2 * kPad, kBodyH);
    if (simpleView) layoutSimple (body);
    else            layoutFull (body);

    // -- full-width rows under the body: capture shelf, history -------------
    int y = body.getBottom() + kG;
    capturePanel.setBounds (kPad, y, body.getWidth(), kCaptureH);    // UNDO/CAPTURE hook
    y += kCaptureH + kG;
    history.setBounds (kPad, y, body.getWidth(), kHistoryH);

    // MUSICAL/DOSE hook: the first-run hint rides on the monitor's lower edge
    // (two lines of text; it is a dismissable overlay on a display, not on a control).
    {
        auto m = monitor.getBounds();
        hintStrip->setBounds (m.removeFromBottom (40).reduced (kG, kF));
        hintStrip->toFront (false);
    }

    // The secret panel slides out from the corner it lives behind.
    if (secret.isVisible())
    {
        const int w = 300, hgt = 116;
        const float t = secret.openAmount();
        const int sy = (int) (-hgt + (kHeaderH + 4 + hgt) * t);
        secret.setBounds (kPad, sy, w, hgt);
    }

    if (help.isVisible())    help.setBounds (getLocalBounds());
    if (options.isVisible()) options.setBounds (getLocalBounds());
    if (debug.isVisible())   debug.setBounds (getLocalBounds());
}

// ---------------------------------------------------------------------------
void VivisectView::layoutFull (juce::Rectangle<int> body)
{
    auto L = body.removeFromLeft (kColL);
    const int leftEdge = L.getRight();
    body.removeFromLeft (kColGap);
    auto R = body;

    // ======================= LEFT : rack, modulation, actions ===============
    headers.push_back ({ "SURGEONS", {}, L.removeFromTop (kSectionH) });
    for (auto* st : strips)
        st->setBounds (L.removeFromTop (kStripH));
    L.removeFromTop (kG);

    headers.push_back ({ "MODULATION",
                         "MIDI notes C3-F3 trigger surgeons 1-6  -  right-click any control to map, reset or type a value",
                         L.removeFromTop (kSectionH) });
    auto modHead = L.removeFromTop (kModHeadH);
    int x = modHead.getX();
    {
        auto put  = [&] (VsxSlider* k) { k->setBounds (x, modHead.getY(), 64, kModHeadH); x += 64 + kF; };
        auto putC = [&] (VsxComboBox* c)
        {
            c->setBounds (x, modHead.getY() + (kModHeadH - kComboH) / 2, 80, kComboH);
            x += 80 + kPad;
        };
        put (kL1r); putC (cL1s); put (kL2r); putC (cL2s); put (kMac1); put (kMac2);
    }
    const int headRight = x - kF;
    auto mod = L.removeFromTop (kNumModSlots * kModRowH);
    const int modTop = modHead.getY(), modBottom = mod.getBottom();
    int rowsRight = mod.getX();
    for (int m = 0; m < kNumModSlots; ++m)
    {
        auto row = mod.removeFromTop (kModRowH);
        mmSrc[m]->setBounds (row.removeFromLeft (128).withSizeKeepingCentre (128, kComboH - 2));
        row.removeFromLeft (kG);
        mmDst[m]->setBounds (row.removeFromLeft (144).withSizeKeepingCentre (144, kComboH - 2));
        row.removeFromLeft (kG);
        mmDepth[m]->setBounds (row.removeFromLeft (160).withSizeKeepingCentre (160, 20));
        rowsRight = row.getX();
    }

    // The feed takes the block to the right of both the macro row and the slot
    // rows, so it can never overlap a control however the layout is retuned.
    // If that gap is too small to be worth it, the spec says drop the feed.
    {
        const int sx = juce::jmax (headRight, rowsRight) + kG;
        const int sw = leftEdge - sx, sh = modBottom - modTop;
        const bool worthIt = sw >= 220 && sh >= 60;
        stream.setVisible (worthIt);
        if (worthIt)
            stream.setBounds (sx, modTop, sw, sh);
    }

    L.removeFromTop (kG);
    {
        auto row = L;      // kActionH tall
        saveBtn.setBounds (row.removeFromRight (152).withSizeKeepingCentre (152, kBtnH));
        auto bt = [&] (juce::Component* c, int w)
        {
            c->setBounds (row.removeFromLeft (w).withSizeKeepingCentre (w, kBtnH));
            row.removeFromLeft (kG);
        };
        bt (&randomBtn, 120);
        bt (&mutateBtn, 112);
        kMutationAmt->setBounds (row.removeFromLeft (80).withSizeKeepingCentre (80, kSmallKnobH));
        row.removeFromLeft (kG);
        bt (&resetBtn, 104);
    }

    // ======================= RIGHT : monitor, master, I/O, post-rack ========
    constexpr int kPostH = 136, kScaleH = 72, kPulseH = 40;
    constexpr int fixedR = kG + kPulseH + kG + kSectionH + kLargeKnobH + kG + kSmallKnobH + kG + kSmallKnobH
                         + kG + kBtnH + kG + kSectionH + kPostH + kG + kScaleH;
    monitor.setBounds (R.removeFromTop (kBodyH - fixedR));
    R.removeFromTop (kG);
    pulse.setBounds (R.removeFromTop (kPulseH));
    R.removeFromTop (kG);

    headers.push_back ({ "MASTER", {}, R.removeFromTop (kSectionH) });
    {
        // two large knobs and five normal ones, equal gaps
        justify (R.removeFromTop (kLargeKnobH),
                 { { kChaos, 80 }, { dosePanel.get(), 80 }, { kPull, 64 }, { kSwing, 64 },
                   { kDryWet, 64 }, { kTrig, 64 }, { kReinj, 64 } }, kLargeKnobH);
        for (auto* s : { kPull, kSwing, kDryWet, kTrig, kReinj })
            resize (s, s->getWidth(), 12 + 62 + 12);
    }
    R.removeFromTop (kG);
    {
        // combo captions are painted 13px above each combo
        justify (R.removeFromTop (kSmallKnobH),
                 { { cGrid, 112 }, { cSource, 128 }, { cScMode, 128 }, { kScAmt, 64 },
                   { kAnalys, 72 }, { kMorph, 72 } }, kSmallKnobH);
        for (auto* c : { cGrid, cSource, cScMode })
            resize (c, c->getWidth(), kComboH, 1);
    }
    R.removeFromTop (kG);
    {
        auto row = R.removeFromTop (kSmallKnobH);
        cBuffer->setBounds (row.removeFromLeft (120).withSizeKeepingCentre (120, kComboH).translated (0, 1));
        row.removeFromLeft (kG);
        for (auto* s : { kInTrim, kOutTrim, kDecayT })
        {
            s->setBounds (row.removeFromLeft (64));
            row.removeFromLeft (kF);
        }
        row.removeFromLeft (kG);
        meter.setBounds (row.withSizeKeepingCentre (row.getWidth(), 48));
    }
    R.removeFromTop (kG);
    justify (R.removeFromTop (kBtnH), { { bMidi, 160 }, { bPanic, 176 }, { bDecay, 200 } }, kBtnH);
    R.removeFromTop (kG);

    headers.push_back ({ "POST-RACK", "SCAR, then TOURNIQUET, then CLOT", R.removeFromTop (kSectionH) });
    {
        auto row = R.removeFromTop (kPostH);
        auto scar = row.removeFromLeft (124);
        row.removeFromLeft (kG);
        effectsPanel.setBounds (row);
        auto sc = scar.withSizeKeepingCentre (scar.getWidth(), kComboH + kF + kSmallKnobH);
        bScar->setBounds (sc.removeFromTop (kComboH));
        sc.removeFromTop (kF);
        kScarDrive->setBounds (sc.removeFromLeft (60));
        kScarMix->setBounds (sc.removeFromRight (60));
    }
    R.removeFromTop (kG);
    scalePanel->setBounds (R.removeFromTop (kScaleH));
}

// ---------------------------------------------------------------------------
void VivisectView::layoutSimple (juce::Rectangle<int> body)
{
    auto L = body.removeFromLeft (kSimpleColL);
    body.removeFromLeft (kColGap);
    auto R = body;

    // ======================= LEFT : the six surgeons, ON + MIX ==============
    headers.push_back ({ "SURGEONS", {}, L.removeFromTop (kSectionH) });
    const int sh = L.getHeight() / kNumSurgeons;
    for (int i = 0; i < kNumSurgeons; ++i)
        strips[i]->setBounds (i == kNumSurgeons - 1 ? L : L.removeFromTop (sh));

    // ======================= RIGHT : monitor, master, actions ===============
    constexpr int kPulseH = 40;
    constexpr int fixedR = kG + kPulseH + kG + kSectionH + kLargeKnobH + kG + kActionH;
    monitor.setBounds (R.removeFromTop (kBodyH - fixedR));
    R.removeFromTop (kG);
    pulse.setBounds (R.removeFromTop (kPulseH));
    R.removeFromTop (kG);

    headers.push_back ({ "MASTER", {}, R.removeFromTop (kSectionH) });
    justify (R.removeFromTop (kLargeKnobH),
             { { kChaos, 80 }, { dosePanel.get(), 80 }, { kPull, 64 }, { kDryWet, 64 },
               { cSource, 200 }, { &scaleToggle, 88 }, { &meter, 220 } }, kLargeKnobH);
    for (auto* s : { kPull, kDryWet })
        resize (s, s->getWidth(), 12 + 62 + 12);
    resize (cSource, 200, kComboH, 1);
    resize (&scaleToggle, 88, kBtnH);
    resize (&meter, 220, 48);

    R.removeFromTop (kG);
    justify (R.removeFromTop (kActionH),
             { { &randomBtn, 160 }, { &mutateBtn, 152 }, { kMutationAmt, 80 },
               { &resetBtn, 128 }, { &saveBtn, 184 } }, kBtnH);
    resize (kMutationAmt, 80, kSmallKnobH);
}

// ==== MUSICAL/DOSE hook ====
void VivisectView::applySimpleView (bool simple)
{
    simpleView = simple;
    simpleBtn->setSimple (simple);

    // The one place that lists what Simple view hides: everything outside F4's
    // list (Dose, Chaos, Gravity, Dry/Wet, surgeon On/Mix, source, presets,
    // RANDOM / MUTATE / UNDO / REDO / CAPTURE). Null is skipped.
    const juce::Array<juce::Component*> hidden {
        // mod matrix and the data stream (resized() decides whether the stream fits)
        kL1r, kL2r, kMac1, kMac2, cL1s, cL2s, &stream,
        mmSrc[0], mmSrc[1], mmSrc[2], mmSrc[3], mmDst[0], mmDst[1], mmDst[2], mmDst[3],
        mmDepth[0], mmDepth[1], mmDepth[2], mmDepth[3],
        // sidechain
        cScMode, kScAmt,
        // master detail
        kSwing, kTrig, kReinj, kAnalys, kMorph, cGrid,
        // I/O and utility
        cBuffer, kInTrim, kOutTrim, kDecayT, bMidi, bPanic, bDecay,
        // post-rack effects
        bScar, kScarDrive, kScarMix, &effectsPanel,
        // musical detail
        scalePanel.get()
    };
    for (auto* c : hidden)
        if (c != nullptr) c->setVisible (! simple);
    scaleToggle.setVisible (simple);
    for (auto* st : strips) st->setSimple (simple);      // PROB, parameter knobs and routing
    resized();      // re-flows the whole window for the chosen view
    repaint();
}
// ==== end MUSICAL/DOSE hook ====

// ===========================================================================
int VivisectView::designWidth() noexcept  { return kW; }
int VivisectView::designHeight() noexcept { return kH; }

VivisectEditor::VivisectEditor (VivisectProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p), view (p)
{
    addAndMakeVisible (view);

    const int w = kW, h = kH;
    float scale = proc.uiScale();                // <= 0: never chosen yet
    if (scale <= 0.f)
    {
        scale = 1.f;
        // Leave room for the host's own window chrome and the taskbar.
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            scale = juce::jmin (1.f, (float) (d->userArea.getHeight() - 96) / (float) h);
    }
    scale = juce::jlimit (minScale, maxScale, scale);

    setResizable (true, true);
    setResizeLimits (juce::roundToInt (w * minScale), juce::roundToInt (h * minScale),
                     juce::roundToInt (w * maxScale), juce::roundToInt (h * maxScale));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) w / (double) h);
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
}

void VivisectEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
}

void VivisectEditor::resized()
{
    const float s = juce::jlimit (minScale, maxScale, (float) getWidth() / (float) kW);
    view.setTransform (juce::AffineTransform::scale (s));
    view.setBounds (0, 0, kW, kH);
    proc.setUiScale (s);
}
