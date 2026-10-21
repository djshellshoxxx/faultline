// ============================================================================
//  0.10.0 final audit: regression tests for the defects the independent audit
//  found (docs/AUDIT-0.10.0-final.md). Included by TestMain.cpp after the shared
//  helpers (check, beginCase, runBlocks, fillTestSignal, snapshotParams) and after
//  TestsUndoCapture.inc, whose ucTests helpers (FakePlayHead, gesture, pump...) it reuses.
// ============================================================================

namespace auditTests
{
using namespace ucTests;

inline void collect (Component& c, Array<Component*>& out)
{
    for (auto* ch : c.getChildren())
    {
        out.add (ch);
        collect (*ch, out);
    }
}

inline Button* findButton (Component& root, const String& text)
{
    Array<Component*> all;
    collect (root, all);
    for (auto* c : all)
        if (auto* b = dynamic_cast<Button*> (c))
            if (b->getButtonText() == text)
                return b;
    return nullptr;
}

inline void setReal (VivisectProcessor& p, const char* pid, float real)
{
    auto* par = p.apvts.getParameter (pid);
    par->setValueNotifyingHost (par->convertTo0to1 (real));
}

inline int indexOfParam (VivisectProcessor& p, const char* pid)
{
    auto* par = p.apvts.getParameter (pid);
    return par != nullptr ? par->getParameterIndex() : -1;
}

// Every 0.10.0 parameter id (effects, scale/dose, route amounts).
inline StringArray newParamIds()
{
    StringArray ids;
    for (const char* e : vsx::effectsParamIds()) ids.add (e);
    for (const char* m : { vsx::id::scaleOn, vsx::id::scaleRoot, vsx::id::scaleType,
                           vsx::id::scaleRange, vsx::id::dose })
        ids.add (m);
    for (int s = 0; s < vsx::kNumSurgeons; ++s) ids.add (vsx::routeAmtRaw (s));
    return ids;
}

// A state / preset tree as an old (0.9.3) build would have written it: no entry for any new parameter.
inline std::unique_ptr<XmlElement> stripNewParams (const MemoryBlock& state)
{
    auto xml = AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    if (xml == nullptr) return nullptr;
    const auto ids = newParamIds();
    for (int i = xml->getNumChildElements(); --i >= 0;)
        if (auto* e = xml->getChildElement (i))
            if (e->hasTagName ("PARAM") && ids.contains (e->getStringAttribute ("id")))
                xml->removeChildElement (e, true);
    return xml;
}

// Energy of one stereo channel, in a sample range.
inline double energy (const AudioBuffer<float>& b, int ch, int from, int to)
{
    double e = 0.0;
    for (int i = from; i < to && i < b.getNumSamples(); ++i)
        e += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return e;
}

// Runs `numBlocks` blocks of `gen`-filled audio through the processor and returns the whole output.
template <typename Gen>
inline AudioBuffer<float> render (VivisectProcessor& p, int numBlocks, int block, Gen gen)
{
    AudioBuffer<float> out (2, numBlocks * block), buf (2, block);
    MidiBuffer midi;
    for (int b = 0; b < numBlocks; ++b)
    {
        for (int i = 0; i < block; ++i)
            for (int ch = 0; ch < 2; ++ch)
                buf.setSample (ch, i, gen (b * block + i, ch));
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, b * block, buf, ch, 0, block);
    }
    return out;
}

// All surgeons off and fully dry: the effects stages are all that is left to hear.
inline void effectsOnlyRig (VivisectProcessor& p)
{
    for (int s = 0; s < vsx::kNumSurgeons; ++s)
        p.apvts.getParameter (vsx::sid (s, "on"))->setValueNotifyingHost (0.f);
    p.apvts.getParameter (vsx::id::dryWet)->setValueNotifyingHost (0.f);
}
} // namespace auditTests

static void run_audit_tests (double kRate, int kBlock)
{
    using namespace auditTests;

    // =======================================================================
    beginCase ("Audit: 0.10.0 parameters are appended, so no 0.9.3 parameter index shifts");
    {
        VivisectProcessor p;
        const int firstNew = indexOfParam (p, vsx::id::tqOn);
        const int lastOld = indexOfParam (p, "mm4_depth");
        check (firstNew == lastOld + 1, "effects start right after the last 0.9.3 parameter");
        bool allAfter = true;
        for (const auto& id : newParamIds())
            allAfter = allAfter && indexOfParam (p, id.toRawUTF8()) >= firstNew;
        check (allAfter, "scale, dose and route amounts sit after the 0.9.3 layout");
        check (indexOfParam (p, vsx::id::flatOn) < firstNew && indexOfParam (p, "fz_route") < firstNew,
               "FLATLINE and the surgeon parameters keep their 0.9.3 positions");
    }

    // =======================================================================
    beginCase ("Audit: sessions, presets and A/B slots from before 0.10.0 restore EVERY new parameter to its default");
    {
        VivisectProcessor src;
        src.prepareToPlay (kRate, kBlock);
        MemoryBlock oldState;
        src.getStateInformation (oldState);
        auto oldXml = stripNewParams (oldState);
        check (oldXml != nullptr, "state parsed");
        if (oldXml != nullptr)
        {
            auto dirty = [] (VivisectProcessor& q)
            {
                for (const auto& id : newParamIds())
                    q.apvts.getParameter (id)->setValueNotifyingHost (0.83f);
            };
            auto allDefault = [] (VivisectProcessor& q)
            {
                String bad;
                for (const auto& id : newParamIds())
                {
                    auto* par = q.apvts.getParameter (id);
                    if (std::abs (par->getValue() - par->getDefaultValue()) > 1.0e-4f) bad += id + " ";
                }
                return bad;
            };

            MemoryBlock stripped;
            AudioProcessor::copyXmlToBinary (*oldXml, stripped);

            VivisectProcessor a;
            a.prepareToPlay (kRate, kBlock);
            dirty (a);
            check (allDefault (a).isNotEmpty(), "test setup: parameters are dirty");
            a.setStateInformation (stripped.getData(), (int) stripped.getSize());
            const auto bad = allDefault (a);
            check (bad.isEmpty(), "session load defaults every new parameter (" + bad + ")");

            // the same through the preset / A-B / history path (restoreSoundState)
            const auto f = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx_audit_old.vsxpreset");
            oldXml->writeTo (f);
            VivisectProcessor b;
            b.prepareToPlay (kRate, kBlock);
            dirty (b);
            check (b.loadPresetFromFile (f), "old preset file loads");
            const auto bad2 = allDefault (b);
            check (bad2.isEmpty(), "old preset defaults every new parameter (" + bad2 + ")");
            f.deleteFile();
        }
    }

    // =======================================================================
    beginCase ("Audit: right-click Reset / Set value are undoable (one gesture, one step)");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto* chaos = p.apvts.getParameter (vsx::id::chaos);
        const float start = chaos->getValue();
        vsx::setParameterAsGesture (*chaos, 0.9f);
        check (p.undoDepth() == 1, "a menu 'Set value' adds exactly one undo step");
        vsx::setParameterAsGesture (*chaos, chaos->getDefaultValue());
        check (p.undoDepth() == 2, "a menu 'Reset to default' adds exactly one undo step");
        p.undo();
        check (std::abs (chaos->getValue() - 0.9f) < 1.0e-5f, "undo returns to the typed value");
        p.undo();
        check (std::abs (chaos->getValue() - start) < 1.0e-5f, "undo returns to the original value");
        const int d = p.undoDepth();
        vsx::setParameterAsGesture (*chaos, chaos->getValue());
        check (p.undoDepth() == d, "a menu edit that changes nothing adds no step");
    }

    beginCase ("Audit: a drag cut short by closing the editor still lands as one undo step, and the stack is not wedged");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        auto* swing = p.apvts.getParameter (vsx::id::swing);
        swing->beginChangeGesture();
        swing->setValueNotifyingHost (0.9f);              // no endChangeGesture: the window closed
        check (p.undoDepth() == 0, "gesture still open");
        p.commitOpenUndoGesture();
        check (p.undoDepth() == 1, "the cut-short drag is kept as one step");
        gesture (p, vsx::id::chaos, 0.77f);
        check (p.undoDepth() == 2, "later gestures record normally");
        p.commitOpenUndoGesture();
        check (p.undoDepth() == 2, "committing with nothing open does nothing");
    }

    beginCase ("Audit: MUTATE at amount 0 changes nothing and records no undo step");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.apvts.getParameter (vsx::id::mutationAmount)->setValueNotifyingHost (0.f);
        p.mutateCurrent();
        check (p.undoDepth() == 0, "no step for a no-op MUTATE");
        p.apvts.getParameter (vsx::id::mutationAmount)->setValueNotifyingHost (0.5f);
        p.mutateCurrent();
        check (p.undoDepth() == 1, "a real MUTATE is exactly one step");
    }

    // =======================================================================
    beginCase ("Audit: troubleshooting report carries the 0.10.0 feature state");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        gesture (p, vsx::id::chaos, 0.6f);
        setReal (p, vsx::id::scaleType, 3.f);
        p.apvts.getParameter (vsx::id::scaleOn)->setValueNotifyingHost (1.f);
        p.apvts.getParameter (vsx::id::dose)->setValueNotifyingHost (0.4f);
        p.apvts.getParameter (vsx::id::tqOn)->setValueNotifyingHost (1.f);
        p.apvts.getParameter (vsx::id::clotOn)->setValueNotifyingHost (1.f);
        p.setFirstRunHintDismissed (true);
        const auto r = p.buildTroubleshootingReport();
        check (r.contains ("FEATURES (0.10.0)"), "report has the feature section");
        for (const char* needle : { "scale lock", "Dorian", "dose", "tourniquet", "clot", "simple view",
                                    "first-run hint dismissed: yes", "undo / redo : 1 / 0", "capture", "captures" })
            check (r.containsIgnoreCase (needle), String ("report mentions '") + needle + "'");
        const auto f = File::getSpecialLocation (File::tempDirectory).getChildFile ("vsx_audit_report.txt");
        check (p.writeTroubleshootingFile (f) && f.loadFileAsString().contains ("FEATURES (0.10.0)"),
               "the exported file has it too");
        f.deleteFile();
    }

    // =======================================================================
    beginCase ("Audit: Hard Reset clears undo, hint and settings but never touches captured takes");
    {
        const auto dir = freshDir ("hard");
        dir.createDirectory();
        const auto take = dir.getChildFile ("Vivisect_keep_me.wav");
        take.replaceWithText ("not really a wav");
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.setCaptureDirectory (dir);
        gesture (p, vsx::id::chaos, 0.7f);
        p.setFirstRunHintDismissed (true);
        p.setCaptureBars (8);
        p.setCrashLogEnabled (true);
        check (p.isCrashLogEnabled() && p.undoDepth() > 0, "setup");
        p.hardResetAllSettings();
        check (p.undoDepth() == 0 && p.redoDepth() == 0, "undo and redo are empty");
        check (! p.firstRunHintDismissed() && p.simpleViewEnabled(), "first-run hint and Simple view are back to fresh-install");
        check (p.captureBars() == 4, "capture length setting is back to default");
        check (! p.isCrashLogEnabled(), "crash log is off");
        check (take.existsAsFile(), "the captured take is still there");
        dir.deleteRecursively();
    }

    beginCase ("Audit: first-run hint dismissal and Simple view persist with the session; crash log never does");
    {
        VivisectProcessor a;
        a.prepareToPlay (kRate, kBlock);
        a.setFirstRunHintDismissed (true);
        a.setSimpleViewEnabled (false);
        a.setCrashLogEnabled (true);
        MemoryBlock mb;
        a.getStateInformation (mb);
        a.setCrashLogEnabled (false);
        VivisectProcessor b;
        b.prepareToPlay (kRate, kBlock);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        check (b.firstRunHintDismissed(), "hint stays dismissed after a session reload");
        check (! b.simpleViewEnabled(), "Simple view setting comes back");
        check (! b.isCrashLogEnabled(), "crash logging is off after every load");
    }

    // =======================================================================
    // =======================================================================
    beginCase ("Audit: every new mod destination index changes the DSP value it names");
    {
        auto setup = [&] (VivisectProcessor& p, int dest, bool macroUp)
        {
            effectsOnlyRig (p);
            p.apvts.getParameter ("mm1_src")->setValueNotifyingHost (
                p.apvts.getParameter ("mm1_src")->convertTo0to1 (4.f));          // Macro 1
            p.apvts.getParameter ("mm1_dst")->setValueNotifyingHost (
                p.apvts.getParameter ("mm1_dst")->convertTo0to1 ((float) dest));
            p.apvts.getParameter ("mm1_depth")->setValueNotifyingHost (1.f);
            p.apvts.getParameter (vsx::id::macro1)->setValueNotifyingHost (macroUp ? 1.f : 0.f);
        };
        const int block = 512, blocks = 120;

        // TQ Cutoff (13): low-pass at 300 Hz on noise; mod opens it to 20 kHz
        auto tqNoise = [&] (bool up, int dest)
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, block);
            setup (p, dest, up);
            p.apvts.getParameter (vsx::id::tqOn)->setValueNotifyingHost (1.f);
            setReal (p, vsx::id::tqMode, 0.f);
            setReal (p, vsx::id::tqCutoff, 300.f);
            setReal (p, vsx::id::tqRes, 0.f);
            p.apvts.getParameter (vsx::id::tqEnv)->setValueNotifyingHost (0.5f);
            Random r (3);
            auto out = render (p, blocks, block, [&] (int, int) { return 0.3f * (r.nextFloat() * 2.f - 1.f); });
            return energy (out, 0, block * 40, block * blocks);
        };
        check (tqNoise (true, vsx::MD_TQ_CUTOFF) > 4.0 * tqNoise (false, vsx::MD_TQ_CUTOFF),
               "TQ Cutoff mod opens the filter");

        // TQ Resonance (14): a tone at the cutoff rings louder at high Q
        auto tqTone = [&] (bool up)
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, block);
            setup (p, vsx::MD_TQ_RES, up);
            p.apvts.getParameter (vsx::id::tqOn)->setValueNotifyingHost (1.f);
            setReal (p, vsx::id::tqMode, 0.f);
            setReal (p, vsx::id::tqCutoff, 1200.f);
            p.apvts.getParameter (vsx::id::tqRes)->setValueNotifyingHost (0.f);
            p.apvts.getParameter (vsx::id::tqEnv)->setValueNotifyingHost (0.5f);
            auto out = render (p, blocks, block, [&] (int n, int) {
                return 0.05f * (float) std::sin (MathConstants<double>::twoPi * 1200.0 * n / kRate); });
            return energy (out, 0, block * 60, block * blocks);
        };
        check (tqTone (true) > 3.0 * tqTone (false), "TQ Resonance mod raises the peak");

        // Clot Feedback (15): the tail after an impulse is longer
        auto clotTail = [&] (bool up, int dest, float baseFb, float baseMix)
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, block);
            setup (p, dest, up);
            p.apvts.getParameter (vsx::id::clotOn)->setValueNotifyingHost (1.f);
            setReal (p, vsx::id::clotTime, 0.f);                      // 1/16 at 120 BPM = 125 ms
            setReal (p, vsx::id::clotFeedback, baseFb);
            setReal (p, vsx::id::clotMix, baseMix);
            setReal (p, vsx::id::clotTone, 16000.f);
            auto out = render (p, 200, block, [&] (int n, int) { return n == 100 ? 0.8f : 0.f; });
            return energy (out, 0, block * 8, block * 200);
        };
        check (clotTail (true, vsx::MD_CLOT_FB, 0.2f, 0.5f) > 2.0 * clotTail (false, vsx::MD_CLOT_FB, 0.2f, 0.5f),
               "Clot Feedback mod lengthens the tail");
        check (clotTail (false, vsx::MD_CLOT_MIX, 0.5f, 0.f) < 1.0e-12 && clotTail (true, vsx::MD_CLOT_MIX, 0.5f, 0.f) > 1.0e-4,
               "Clot Mix mod brings the echoes in from silence");
    }

    // =======================================================================
    beginCase ("Audit: effects + capture stay finite across sample rates, block sizes, BPM extremes and a mono layout");
    {
        for (double rate : { 22050.0, 44100.0, 192000.0})
            for (int block : { 1, 7, 64, 8192 })
            {
                VivisectProcessor p;
                p.prepareToPlay (rate, block);
                p.setCaptureDirectory (freshDir ("sr"));
                p.apvts.getParameter (vsx::id::tqOn)->setValueNotifyingHost (1.f);
                p.apvts.getParameter (vsx::id::clotOn)->setValueNotifyingHost (1.f);
                setReal (p, vsx::id::clotTime, 7.f);                           // 1/1
                setReal (p, vsx::id::clotFeedback, 0.95f);
                setReal (p, vsx::id::tqRes, 1.f);
                check (p.captureEngine().capacityFrames() >= (size_t) (64.0 * rate), "capture ring covers 64 s at " + String (rate, 0));
                const int blocks = block >= 64 ? 6 : 400;
                const auto v = runBlocks (p, blocks, block, rate, true);
                check (v.finite && v.peak < kCeiling, "finite at " + String (rate, 0) + " Hz / " + String (block));
            }

        for (double bpm : { 20.0, 300.0 })
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            FakePlayHead ph;
            ph.bpm = bpm; ph.playing = true;
            p.setPlayHead (&ph);
            p.apvts.getParameter (vsx::id::clotOn)->setValueNotifyingHost (1.f);
            setReal (p, vsx::id::clotTime, 7.f);                               // 1/1 at 20 BPM = 12 s > the 4 s line
            setReal (p, vsx::id::clotFeedback, 0.95f);
            int64 pos = 0;
            std::vector<float> l, r;
            run (p, &ph, 400, kBlock, kRate, pos, &l, &r);
            bool fin = true; float peak = 0.f;
            for (float s : l) { fin = fin && std::isfinite (s); peak = jmax (peak, std::abs (s)); }
            check (fin && peak < kCeiling, "CLOT 1/1 finite and bounded at " + String (bpm, 0) + " BPM");
            p.setPlayHead (nullptr);
        }

        // mono in / mono out
        {
            VivisectProcessor p;
            AudioProcessor::BusesLayout lay;
            lay.inputBuses.add (AudioChannelSet::mono());
            lay.inputBuses.add (AudioChannelSet::disabled());
            lay.outputBuses.add (AudioChannelSet::mono());
            const bool set = p.setBusesLayout (lay);
            check (set, "mono layout accepted");
            p.prepareToPlay (kRate, kBlock);
            p.apvts.getParameter (vsx::id::tqOn)->setValueNotifyingHost (1.f);
            p.apvts.getParameter (vsx::id::clotOn)->setValueNotifyingHost (1.f);
            p.setCaptureDirectory (freshDir ("mono"));
            check (p.startCapture (1), "capture arms on a mono layout");
            AudioBuffer<float> buf (1, kBlock);
            MidiBuffer midi;
            bool fin = true;
            for (int b = 0; b < 220; ++b)
            {
                fillTestSignal (buf, (int64) b * kBlock, kRate);
                p.processBlock (buf, midi);
                for (int i = 0; i < kBlock; ++i) fin = fin && std::isfinite (buf.getSample (0, i));
            }
            check (fin, "mono output finite");
            check (waitIdle (p), "mono capture completes");
            const auto f = p.captureState().lastFile;
            const auto w = readWav (f);
            check (w.ok && w.finite && w.length == expectedFrames (1, 120.0, kRate), "mono capture file is complete");
            p.captureDirectory().deleteRecursively();
        }
    }

    beginCase ("Audit: NaN / Inf / denormals through TOURNIQUET and CLOT stay finite");
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            vsx::Tourniquet tq;
            tq.prepare (kRate);
            AudioBuffer<float> b (2, 256);
            bool fin = true;
            for (int round = 0; round < 20; ++round)
            {
                for (int i = 0; i < 256; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        b.setSample (ch, i, i % 11 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                          : i % 13 == 0 ? std::numeric_limits<float>::infinity()
                                          : i % 5 == 0 ? 1.0e-39f : 0.5f);
                tq.process (b, 256, mode, 800.f, 1.f, 1.f, 1.f);
                fin = fin && inspect (b).finite;
            }
            check (fin, "TOURNIQUET mode " + String (mode));
        }
        vsx::Clot cl;
        cl.prepare (kRate);
        AudioBuffer<float> b (2, 256);
        bool fin = true;
        for (int round = 0; round < 60; ++round)
        {
            for (int i = 0; i < 256; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    b.setSample (ch, i, i % 17 == 0 ? std::numeric_limits<float>::quiet_NaN() : i % 7 == 0 ? 1.0e-39f : 0.9f);
            cl.process (b, 256, 24000.0, round % 9, 300.f, 0.95f, 5000.f, 1.f, 0.8f);
            fin = fin && inspect (b).finite;
        }
        check (fin, "CLOT with every time choice, NaN and denormal input");
    }

    // =======================================================================
    beginCase ("Audit: the editor - every parameter has a control, every control a tooltip, UNDO / REDO / CAPTURE / CANCEL and the keyboard are wired");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.setCaptureDirectory (freshDir ("ui"));
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        check (ed != nullptr, "editor created");
        if (ed != nullptr)
        {
            ed->setSize (VivisectView::designWidth(), VivisectView::designHeight());
            Array<Component*> all;
            collect (*ed, all);

            auto controlState = [&]
            {
                std::vector<double> s;
                for (auto* c : all)
                {
                    if (auto* sl = dynamic_cast<Slider*> (c))        s.push_back (sl->getValue());
                    else if (auto* cb = dynamic_cast<ComboBox*> (c)) s.push_back (cb->getSelectedItemIndex());
                    else if (auto* bt = dynamic_cast<Button*> (c))   s.push_back (bt->getToggleState() ? 1.0 : 0.0);
                }
                return s;
            };

            // orphan parameters: moving each one must move some control
            String orphans;
            for (auto* par : p.getParameters())
            {
                auto* wid = dynamic_cast<AudioProcessorParameterWithID*> (par);
                if (wid == nullptr) continue;
                const float keep = par->getValue();
                const auto before = controlState();
                par->setValueNotifyingHost (keep < 0.5f ? 1.f : 0.f);
                if (controlState() == before) orphans += wid->paramID + " ";
                par->setValueNotifyingHost (keep);
            }
            check (orphans.isEmpty(), "no orphan parameters (" + orphans + ")");

            // tooltips on every interactive control
            String bare;
            for (auto* c : all)
            {
                String tip;
                if (auto* sl = dynamic_cast<Slider*> (c))        tip = sl->getTooltip();
                else if (auto* cb = dynamic_cast<ComboBox*> (c)) tip = cb->getTooltip();
                else if (auto* bt = dynamic_cast<Button*> (c))   tip = bt->getTooltip();
                else continue;
                if (tip.isEmpty())
                    bare += (c->getName().isNotEmpty() ? c->getName() : String (typeid (*c).name())) + "[" + c->getTitle() + "] ";
            }
            check (bare.isEmpty(), "every slider, combo and button has a tooltip (" + bare + ")");

            // UNDO / REDO buttons
            auto* undoB = findButton (*ed, "UNDO");
            auto* redoB = findButton (*ed, "REDO");
            check (undoB != nullptr && redoB != nullptr, "UNDO and REDO buttons exist");
            if (undoB != nullptr && redoB != nullptr)
            {
                check (! undoB->isEnabled() && ! redoB->isEnabled(), "both grey out when empty");
                const float c0 = p.apvts.getParameter (vsx::id::chaos)->getValue();
                gesture (p, vsx::id::chaos, 0.91f);
                check (p.canUndo() && p.undoLabel().contains ("Chaos"), "the processor names the next undo action");
                check ((bool) undoB->onClick && (bool) redoB->onClick, "both buttons have handlers");
                undoB->onClick();
                check (std::abs (p.apvts.getParameter (vsx::id::chaos)->getValue() - c0) < 1.0e-5f, "UNDO button undoes");
                check (p.canRedo(), "and arms REDO");
                redoB->onClick();
                check (std::abs (p.apvts.getParameter (vsx::id::chaos)->getValue() - 0.91f) < 1.0e-5f, "REDO button redoes");
            }

            // keyboard
            VivisectView* view = nullptr;
            for (auto* c : all)
                if (auto* v = dynamic_cast<VivisectView*> (c)) view = v;
            check (view != nullptr, "view found");
            if (view != nullptr)
            {
                const float now = p.apvts.getParameter (vsx::id::chaos)->getValue();
                check (view->keyPressed (KeyPress ('z', ModifierKeys::ctrlModifier, 0)), "Ctrl+Z handled");
                check (p.apvts.getParameter (vsx::id::chaos)->getValue() != now, "Ctrl+Z undoes");
                check (view->keyPressed (KeyPress ('z', ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier, 0)),
                       "Ctrl+Shift+Z handled");
                check (std::abs (p.apvts.getParameter (vsx::id::chaos)->getValue() - now) < 1.0e-5f, "Ctrl+Shift+Z redoes");
                view->keyPressed (KeyPress ('z', ModifierKeys::ctrlModifier, 0));
                check (view->keyPressed (KeyPress ('y', ModifierKeys::ctrlModifier, 0)), "Ctrl+Y handled");
                check (std::abs (p.apvts.getParameter (vsx::id::chaos)->getValue() - now) < 1.0e-5f, "Ctrl+Y redoes");
                check (! view->keyPressed (KeyPress ('z', ModifierKeys(), 0)), "plain Z is not swallowed");
            }

            // CAPTURE / CANCEL, and the shelf refreshing when a take finishes
            auto* capB = findButton (*ed, "CAPTURE");
            auto* canB = findButton (*ed, "CANCEL");
            check (capB != nullptr && canB != nullptr, "CAPTURE and CANCEL buttons exist");
            vsx::CapturePanel* panel = nullptr;
            for (auto* c : all)
                if (auto* cp = dynamic_cast<vsx::CapturePanel*> (c)) panel = cp;
            check (panel != nullptr, "capture panel found");
            if (capB != nullptr && canB != nullptr && panel != nullptr)
            {
                capB->onClick();
                check (p.captureState().phase != vsx::CapturePhase::idle, "CAPTURE button arms the engine");
                panel->poll();
                check (! capB->isEnabled() && canB->isEnabled(), "while armed: CAPTURE greyed, CANCEL live");
                canB->onClick();
                check (p.captureState().phase == vsx::CapturePhase::idle, "CANCEL button stops it");
                check (countFiles (p.captureDirectory(), "*.wav") == 0 && countFiles (p.captureDirectory(), "*.part") == 0,
                       "cancel leaves nothing behind");

                panel->poll();
                check (panel->getShelf().numTakes() == 0, "shelf starts empty");
                const auto take = captureOnce (p, nullptr, 1, kRate, kBlock);
                check (take.existsAsFile(), "a capture finished");
                panel->poll();                                              // what the 10 Hz timer does
                check (panel->getShelf().numTakes() == 1 && panel->getShelf().takeFile (0) == take,
                       "the shelf picks the finished take up when the panel notices it");
            }
        }
        p.captureDirectory().deleteRecursively();
    }

    beginCase ("Audit: closing the editor in the middle of a capture, then destroying the processor, is safe");
    {
        const auto dir = freshDir ("close");
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            p.setCaptureDirectory (dir);
            {
                std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
                check (p.startCapture (1), "capture armed");
                int64 pos = 0;
                run (p, nullptr, 20, kBlock, kRate, pos);
                check (p.captureState().phase == vsx::CapturePhase::recording, "recording while the editor is open");
            }                                                               // editor destroyed mid-capture
            int64 pos = 0;
            run (p, nullptr, 400, kBlock, kRate, pos);
            check (waitIdle (p), "the take still finishes without an editor");
            check (p.captureState().completedCount == 1, "and is saved");
        }
        {
            VivisectProcessor p;                                            // processor destroyed while writing / recording
            p.prepareToPlay (kRate, kBlock);
            p.setCaptureDirectory (dir);
            check (p.startCapture (2), "second capture armed");
            int64 pos = 0;
            run (p, nullptr, 30, kBlock, kRate, pos);
        }
        check (countFiles (dir, "*.part") == 0, "no half-written file after the processor went away");
        dir.deleteRecursively();
    }
}
