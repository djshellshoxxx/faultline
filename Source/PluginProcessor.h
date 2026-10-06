#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include "Parameters.h"
#include "Dsp.h"

// ============================================================================
//  VIVISECT — sample butchery. See README.md.
// ============================================================================
class VivisectProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    VivisectProcessor();
    ~VivisectProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Vivisect"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // -- editor-facing ------------------------------------------------------
    juce::AudioProcessorValueTreeState apvts;
    vsx::TelemetryRing telem;
    vsx::Scar scar;
    bool scarWasOn = false;
    vsx::Flatline flatline;
    bool flatlineWasOn = false;
    bool crashLogOn = false;            // never restored from state, by design
    juce::File crashLogFile;

    void buildMonitorSnapshot (vsx::MonitorSnapshot&);
    float surgeonActivity (int i) const
    {
        return surgAct[(size_t) juce::jlimit (0, vsx::kNumSurgeons - 1, i)].load();
    }
    float buildPulse() const { return pulseSmooth.load(); }
    float outputPeak (int ch) const { return outPeak[(size_t) juce::jlimit (0, 1, ch)].load(); }

    // Raw internal traffic for the on-screen data stream. The editor drains it.
    vsx::TelemetryRing& telemetry() noexcept { return telem; }
    juce::String describeParam (int index) const;

    juce::StringArray getPresetNames() const;
    juce::Array<juce::File> getUserPresetFiles() const;
    void loadPreset (int index);
    // Resolving by the name shown in the menu, rather than by position, keeps
    // the right preset loading even if the folder changed since the menu was
    // built - which the "Open Preset Folder" item actively invites.
    bool loadPresetByName (const juce::String& displayName);
    static constexpr int numFactoryPresets = 6;
    void saveUserPreset (const juce::String& name);
    juce::File getUserPresetDir() const;

    // preset files (menu: Save / Save As / Open)
    bool savePresetToFile (const juce::File&);
    bool loadPresetFromFile (const juce::File&);
    juce::File getLastPresetFile() const { return lastPresetFile; }

    void loadSampleInto (int slot, const juce::File&);
    void clearSampleSlot (int slot);
    bool slotHasSample (int slot) const { return specimen.hasSample (slot); }

    // export the current specimen buffer as a wav (the "special function"
    // export for an effect: the butchered specimen itself)
    bool exportSpecimenToWav (const juce::File&, int bitDepth = 24,
                              double* secondsWritten = nullptr);

    int  historyCount() const { return histCount; }
    void rewindToNormalized (float x);
    void triggerSurgeonManual (int i);

    // -- diagnostics --------------------------------------------------------
    //  include.md: the debug and troubleshooting features. The crash log is
    //  deliberately NOT persisted - it is off on every load, every time.
    juce::String buildTroubleshootingReport() const;
    bool writeTroubleshootingFile (const juce::File&) const;
    juce::File  suggestedTroubleshootingFile() const;

    void setCrashLogEnabled (bool);
    bool isCrashLogEnabled() const noexcept { return crashLogOn; }
    juce::File currentCrashLogFile() const { return crashLogFile; }
    void appendToCrashLog (const juce::String& line);

    // Obvious signs the plugin is set up in a way that will not make sound.
    juce::StringArray detectMisconfiguration() const;

    // Much more destructive than RESET: defaults, cleared mappings, wiped
    // cache. Used when the plugin is misbehaving and needs a clean slate.
    void hardResetAllSettings();

    // -- reset / randomise / mutate -----------------------------------------
    void resetAllToDefaults();
    void randomizeAll();
    void mutateCurrent();

    // Exploration locks protect creative anchors from RANDOM / MUTATE.
    void setParameterLocked (const juce::String& paramID, bool locked);
    bool isParameterLocked (const juce::String& paramID) const;
    void clearParameterLocks();
    int  lockedParameterCount() const { return parameterLocks.size(); }

    // -- A/B compare --------------------------------------------------------
    void selectABSlot (int slot);
    int  currentABSlot() const { return abSlot; }
    void copyABSlot();                       // current slot -> the other one

    // -- options ------------------------------------------------------------
    bool tooltipsEnabled() const;
    void setTooltipsEnabled (bool);

    // -- MIDI learn ---------------------------------------------------------
    void beginMidiLearn (const juce::String& paramID);
    void cancelMidiLearn();
    juce::String midiLearnTarget() const;
    int  getCCForParam (const juce::String& paramID) const;
    void setCCForParam (const juce::String& paramID, int cc);
    void clearCCForParam (const juce::String& paramID);

private:
    void timerCallback() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout layout() { return vsx::createParameterLayout(); }

    float pv (const char* id) const { return apvts.getRawParameterValue (id)->load(); }
    float pvMod (const char* id, int destEnum);
    void  applyPresetMap (const std::map<juce::String, float>&);

    juce::ValueTree settingsTree();
    void writeMidiMapToState();
    void writeParameterLocksToState();
    void syncFromStateTree();

    // Sound snapshots deliberately exclude VSX_SETTINGS. Presets, A/B and
    // history are sonic state; MIDI maps, tooltips and locks are global user
    // settings and must not change when auditioning sound states.
    juce::ValueTree soundStateSnapshot();
    void restoreSoundState (const juce::ValueTree&);                // rebuild cc map + cached settings
    void seedDefaultMidiMap();
    int  paramIndexFor (const juce::String& paramID) const;

    // dsp
    vsx::SpecimenBuffer   specimen;
    vsx::SpecimenAnalysis analysis;
    vsx::SurgeonRack      rack;
    vsx::SliceScheduler   scheduler;
    vsx::ModMatrix        modMatrix;

    std::map<juce::String, float> factoryPreset (int index) const;

    juce::AudioBuffer<float> dryBuf, wetBuf, workBuf;
    juce::AudioFormatManager formatManager;

    double sampleRate = 44100.0;
    double songPos = 0.0;
    double lastBpm = 120.0;

    float  decayPhase = 0.f;
    int    panicTimer = 0;
    float  scEnvPrev = 0.f;

    std::array<std::atomic<float>, vsx::kNumSurgeons> surgAct {};
    std::atomic<float> pulseSmooth { 0.f };
    std::array<std::atomic<float>, 2> outPeak {};

    // history : rolling APVTS xml snapshots (2 Hz, 60 s)
    static constexpr int kHist = 120;
    juce::StringArray histRing;
    int histHead = 0, histCount = 0;

    // midi learn : cc -> parameter index (-1 = unmapped)
    std::array<std::atomic<int>, 128> ccToParam;
    std::atomic<int> learnParamIndex { -1 };
    std::atomic<bool> midiMapDirty { false };   // audio thread learned a CC

    // A/B compare
    juce::ValueTree abState[2];
    int abSlot = 0;

    juce::File lastPresetFile;
    juce::StringArray parameterLocks;
    juce::Random rng { (juce::uint32) juce::Time::currentTimeMillis() };
    bool randomisedOnce = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VivisectProcessor)
};
