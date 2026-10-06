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
    // ---- layout, all multiples of the 8px base grid -----------------------
    constexpr int kW          = 1180;
    constexpr int kHeaderH    = metric::header;    // 32
    constexpr int kMonitorH   = 160;
    constexpr int kMasterH    = 104;
    constexpr int kIoH        = 88;
    constexpr int kStripH     = 78;
    constexpr int kBtnRowH    = 40;
    constexpr int kModHeadH   = 76;
    constexpr int kModRowH    = 26;
    constexpr int kHistoryH   = 24;
    constexpr int kFooterH    = 16;
    constexpr int kG          = metric::grid;      // 8
    constexpr int kPad        = metric::pad;       // 16

    constexpr int kH = kHeaderH + kG + kMonitorH + kG + kMasterH + kG + kIoH + kG
                     + kNumSurgeons * kStripH + kG + kBtnRowH + kG
                     + kModHeadH + kNumModSlots * kModRowH + kG + kHistoryH + kG + kFooterH;
}

// ===========================================================================
//  Control factories
// ===========================================================================
VsxSlider& VivisectEditor::addKnob (const char* pid, const juce::String& caption,
                                    const juce::String& tip, VsxSlider::Size size)
{
    auto* s = new VsxSlider (*this, pid, caption, size);
    s->setTooltip (tip);
    addAndMakeVisible (s);
    sAtt.add (new SA (proc.apvts, pid, *s));
    sliders.add (s);
    return *s;
}

VsxComboBox& VivisectEditor::addCombo (const char* pid, const juce::StringArray& items,
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

VsxButton& VivisectEditor::addParamButton (const char* pid, const juce::String& text, const juce::String& tip)
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
VivisectEditor::VivisectEditor (VivisectProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p),
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
                        "nearest transient - instead of by the grid.");
    kMorph  = &addKnob (id::morph, "MORPH A/B", "Morph position between Sample A and Sample B.");
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
    kInTrim  = &addKnob (id::inputTrim, "IN TRIM", "Gain into the plugin, in dB.");
    kOutTrim = &addKnob (id::outputTrim, "OUT TRIM", "Gain out of the plugin, in dB.");
    kDecayT  = &addKnob (id::decayTime, "DECAY T", "How long Decay Over Time takes to reach maximum, in seconds.");

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
    kL1r  = &addKnob (id::lfo1Rate, "LFO 1", "LFO 1 rate, in Hz.");
    kL2r  = &addKnob (id::lfo2Rate, "LFO 2", "LFO 2 rate, in Hz.");
    kMac1 = &addKnob (id::macro1, "MACRO 1", "Macro 1. CC20 by default.");
    kMac2 = &addKnob (id::macro2, "MACRO 2", "Macro 2. CC21 by default.");
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

    setSize (kW, kH);
    setResizable (false, false);
    startTimerHz (8);
}

VivisectEditor::~VivisectEditor() { setLookAndFeel (nullptr); }

// ===========================================================================
void VivisectEditor::beginMidiLearnFor (const juce::String& paramID)
{
    proc.beginMidiLearn (paramID);
    learnTarget = paramID;
    repaint();
}

void VivisectEditor::timerCallback()
{

    // the audio thread clears the learn target as soon as it sees a CC
    const auto t = proc.midiLearnTarget();
    if (t != learnTarget) { learnTarget = t; repaint(); }
}

void VivisectEditor::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);
    int i = 1;
    for (const auto& n : proc.getPresetNames())
        presetBox.addItem (n, i++);
}

void VivisectEditor::showOverlay (juce::Component* which)
{
    help.setVisible (which == &help);
    options.setVisible (which == &options);
    debug.setVisible (which == &debug);

    // Hand the telemetry ring to whichever view is actually on screen.
    stream.setPaused (which != nullptr);
    if (which != nullptr) { which->setBounds (getLocalBounds()); which->toFront (true); }
}

void VivisectEditor::chooseSample (int slot)
{
    chooser = std::make_unique<juce::FileChooser> ("Load specimen into " + juce::String (slot == 0 ? "A" : "B"),
                                                   juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, slot] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadSampleInto (slot, f);
                          });
}

void VivisectEditor::doSaveAs()
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

void VivisectEditor::doOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open preset", proc.getUserPresetDir(), "*.vsxpreset");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadPresetFromFile (f);
                          });
}

void VivisectEditor::doExportSpecimen (int bitDepth)
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

void VivisectEditor::showAudioMidiSettings()
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
void VivisectEditor::mouseDown (const juce::MouseEvent& e)
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
bool VivisectEditor::isAudioFile (const juce::String& path)
{
    static const juce::StringArray exts { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg" };
    for (const auto& e : exts)
        if (path.endsWithIgnoreCase (e))
            return true;
    return false;
}

int VivisectEditor::dropSlotFor (juce::Point<int> p) const
{
    return p.x < getWidth() / 2 ? 0 : 1;
}

bool VivisectEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void VivisectEditor::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dragSlot = dropSlotFor ({ x, y });
    repaint();
}

void VivisectEditor::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int s = dropSlotFor ({ x, y });
    if (s != dragSlot) { dragSlot = s; repaint(); }
}

void VivisectEditor::fileDragExit (const juce::StringArray&)
{
    dragSlot = -1;
    repaint();
}

void VivisectEditor::filesDropped (const juce::StringArray& files, int x, int y)
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
            proc.loadSampleInto (slot, file);
        return;
    }
}

void VivisectEditor::showMainMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Save", proc.getLastPresetFile().existsAsFile());
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
                             case 1:  proc.savePresetToFile (proc.getLastPresetFile()); refreshPresetList(); break;
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
void VivisectEditor::paint (juce::Graphics& g)
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
        auto b = c.first->getBounds();
        draw::label (g, c.second, { b.getX() - 6, b.getY() - 13, b.getWidth() + 12, 12 },
                     juce::Justification::centredLeft, col::textMuted, 10.f);
    }

    // -- section headers + separators ---------------------------------------
    if (strips.size() > 0)
    {
        const int y = strips[0]->getY() - 14;
        draw::sectionHeader (g, "SURGEONS", { kPad, y, 240, 12 });
    }
    if (mmSrc[0] != nullptr)
    {
        const int y = kL1r->getY() - 14;
        draw::sectionHeader (g, "MODULATION", { kPad, y, 240, 12 });
        g.setColour (col::textMuted);
        g.setFont (font::ui (11.f));
        g.drawText ("MIDI  notes C3-F3 trigger surgeons 1-6     right-click any control to map, reset or type a value",
                    kPad + 160, y, getWidth() - kPad - 176, 12, juce::Justification::centredLeft);
        draw::separator (g, { kPad, y + 16, getWidth() - 2 * kPad, 1 });
    }
    if (history.getY() > 0)
        draw::separator (g, { kPad, history.getY() - 6, getWidth() - 2 * kPad, 1 });

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
void VivisectEditor::resized()
{
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

    // -- monitor -------------------------------------------------------------
    r.removeFromTop (kG);
    monitor.setBounds (r.removeFromTop (kMonitorH).reduced (kPad, 0));

    // -- master row ----------------------------------------------------------
    r.removeFromTop (kG);
    auto master = r.removeFromTop (kMasterH).reduced (kPad, 0);
    pulse.setBounds (master.removeFromLeft (272).reduced (0, kG));
    master.removeFromLeft (kPad);

    auto place = [&] (VsxSlider* s, int w) { s->setBounds (master.removeFromLeft (w)); };
    place (kChaos, 88);
    master.removeFromLeft (kG);
    for (auto* s : { kPull, kSwing, kDryWet, kTrig, kReinj, kAnalys, kMorph })
        place (s, 76);

    auto col2 = master.withTrimmedLeft (kG);
    const int rowH = metric::buttonH - 4;
    const int stack = rowH * 3 + kG;
    col2 = col2.withSizeKeepingCentre (col2.getWidth(), stack);
    cGrid->setBounds   (col2.removeFromTop (rowH));
    col2.removeFromTop (kG / 2);
    cSource->setBounds (col2.removeFromTop (rowH));
    col2.removeFromTop (kG / 2);
    auto scRow = col2.removeFromTop (rowH);
    kScAmt->setBounds  (scRow.removeFromRight (56).withSizeKeepingCentre (56, kIoH - kG * 2));
    cScMode->setBounds (scRow.withTrimmedRight (kG / 2));

    // -- i/o row -------------------------------------------------------------
    r.removeFromTop (kG);
    auto io = r.removeFromTop (kIoH).reduced (kPad, 0);
    cBuffer->setBounds (io.removeFromLeft (160).withSizeKeepingCentre (160, metric::buttonH));
    io.removeFromLeft (kPad);
    for (auto* s : { kInTrim, kOutTrim, kDecayT })
    {
        s->setBounds (io.removeFromLeft (76));
        io.removeFromLeft (kG / 2);
    }
    io.removeFromLeft (kPad);
    meter.setBounds (io.removeFromLeft (juce::jmin (320, io.getWidth()))
                       .withSizeKeepingCentre (juce::jmin (320, io.getWidth()), 48));

    // -- surgeons ------------------------------------------------------------
    r.removeFromTop (kG);
    auto stripArea = r.removeFromTop (kNumSurgeons * kStripH).reduced (kPad, 0);
    for (auto* st : strips)
        st->setBounds (stripArea.removeFromTop (kStripH));

    // -- action buttons ------------------------------------------------------
    r.removeFromTop (kG);
    auto btnRow = r.removeFromTop (kBtnRowH).reduced (kPad, 0);
    auto pb = [&] (juce::Component* c, int w) { c->setBounds (btnRow.removeFromLeft (w)); btnRow.removeFromLeft (kG); };
    pb (&randomBtn, 96);
    pb (&mutateBtn, 88);
    kMutationAmt->setBounds (btnRow.removeFromLeft (72).withSizeKeepingCentre (72, kBtnRowH));
    btnRow.removeFromLeft (kG / 2);
    pb (&resetBtn, 88);
    pb (bMidi, 112);
    pb (bPanic, 128);
    pb (bDecay, 152);
    pb (bScar, 72);
    kScarDrive->setBounds (btnRow.removeFromLeft (64));
    btnRow.removeFromLeft (kG / 2);
    kScarMix->setBounds (btnRow.removeFromLeft (64));
    saveBtn.setBounds (btnRow.removeFromRight (136));

    // -- modulation ----------------------------------------------------------
    r.removeFromTop (kG);
    auto modHead = r.removeFromTop (kModHeadH - kG).reduced (kPad, 0);
    auto ph = [&] (VsxSlider* s) { s->setBounds (modHead.removeFromLeft (76)); modHead.removeFromLeft (kG / 2); };
    auto pc = [&] (VsxComboBox* c)
    {
        c->setBounds (modHead.removeFromLeft (96).withSizeKeepingCentre (96, metric::buttonH - 4));
        modHead.removeFromLeft (kPad);
    };
    const int modHeadTop = modHead.getY();
    ph (kL1r); pc (cL1s); ph (kL2r); pc (cL2s); ph (kMac1); ph (kMac2);
    const int headFreeX = modHead.getX();          // whatever the macros did not use

    auto mod = r.removeFromTop (kNumModSlots * kModRowH).reduced (kPad, 0);
    const int modBottom = mod.getBottom();
    int slotFreeX = mod.getRight();
    for (int m = 0; m < kNumModSlots; ++m)
    {
        auto row = mod.removeFromTop (kModRowH);
        mmSrc[m]->setBounds (row.removeFromLeft (144).withSizeKeepingCentre (144, metric::buttonH - 6));
        row.removeFromLeft (kG);
        mmDst[m]->setBounds (row.removeFromLeft (160).withSizeKeepingCentre (160, metric::buttonH - 6));
        row.removeFromLeft (kG);
        const int dw = juce::jmin (360, row.getWidth());
        mmDepth[m]->setBounds (row.removeFromLeft (dw).withSizeKeepingCentre (dw, 20));
        slotFreeX = juce::jmin (slotFreeX, row.getX());
    }

    // The feed takes the rectangle both the macro row and the slot rows leave
    // clear, so it can never overlap a control however the layout is retuned.
    // If that gap is too small to be worth it, the spec says drop the feed.
    {
        const int x = juce::jmax (headFreeX, slotFreeX) + kG;
        const int w = getWidth() - kPad - x;
        const int hgt = modBottom - modHeadTop;
        const bool worthIt = w >= 220 && hgt >= 60;
        stream.setVisible (worthIt);
        if (worthIt)
            stream.setBounds (x, modHeadTop, w, hgt);
    }

    // -- history -------------------------------------------------------------
    r.removeFromTop (kG);
    history.setBounds (r.removeFromTop (kHistoryH).reduced (kPad, 0));

    // The secret panel slides out from the corner it lives behind.
    if (secret.isVisible())
    {
        const int w = 300, hgt = 116;
        const float t = secret.openAmount();
        const int y = (int) (-hgt + (kHeaderH + 4 + hgt) * t);
        secret.setBounds (kPad, y, w, hgt);
    }

    if (help.isVisible())    help.setBounds (getLocalBounds());
    if (options.isVisible()) options.setBounds (getLocalBounds());
    if (debug.isVisible())   debug.setBounds (getLocalBounds());
}
