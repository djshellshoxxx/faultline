#include "PanelsCapture.h"
#include <cmath>

namespace vsx
{
// ===========================================================================
juce::String describeCaptureState (const CaptureInfo& i)
{
    juce::String s;
    switch (i.phase)
    {
        case CapturePhase::idle:      s = "ready"; break;
        case CapturePhase::armed:     s = "armed, waiting for the next bar line"; break;
        case CapturePhase::recording: s = "recording " + juce::String (i.progress * i.seconds, 1) + " / "
                                          + juce::String (i.seconds, 1) + " s (" + juce::String (i.bars) + " bar)"; break;
        case CapturePhase::writing:   s = "writing WAV"; break;
    }
    s << ", " << i.completedCount << " saved";
    if (i.failedCount > 0) s << ", " << i.failedCount << " failed";
    return s;
}

// ===========================================================================
//  Peak analysis runs on a pool thread; the result comes back by callAsync
//  through a weak reference, so a shelf that has gone away is simply skipped.
// ===========================================================================
struct CaptureShelf::PeakJob : public juce::ThreadPoolJob
{
    PeakJob (juce::WeakReference<CaptureShelf> s, juce::File f, juce::int64 sz)
        : juce::ThreadPoolJob ("capture peaks"), shelf (s), file (std::move (f)), size (sz) {}

    JobStatus runJob() override
    {
        constexpr int kBins = 96;
        std::vector<float> peaks;
        double seconds = 0.0;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::InputStream> in (file.createInputStream());
        std::unique_ptr<juce::AudioFormatReader> r;
        if (in != nullptr)
            r.reset (wav.createReaderFor (in.release(), true));
        if (r != nullptr)
        {
            const auto len = r->lengthInSamples;
            seconds = r->sampleRate > 0.0 ? (double) len / r->sampleRate : 0.0;
            if (len > 0)
            {
                const int ch = (int) juce::jmin<unsigned int> (2u, r->numChannels);
                juce::Range<float> lv[2];
                for (int b = 0; b < kBins; ++b)
                {
                    if (shouldExit()) return jobHasFinished;
                    const auto a = len * b / kBins, z = len * (b + 1) / kBins;
                    r->readMaxLevels (a, juce::jmax ((juce::int64) 1, z - a), lv, ch);
                    float m = 0.f;
                    for (int c = 0; c < ch; ++c)
                        m = juce::jmax (m, std::abs (lv[c].getStart()), std::abs (lv[c].getEnd()));
                    peaks.push_back (std::isfinite (m) ? juce::jlimit (0.f, 1.f, m) : 0.f);
                }
            }
        }

        auto sh = shelf;
        auto f = file;
        auto sz = size;
        juce::MessageManager::callAsync ([sh, f, sz, seconds, peaks = std::move (peaks)]() mutable
        {
            if (auto* s = sh.get()) s->setPeaks (f, sz, seconds, std::move (peaks));
        });
        return jobHasFinished;
    }

    juce::WeakReference<CaptureShelf> shelf;
    juce::File file;
    juce::int64 size;
};

// ===========================================================================
CaptureShelf::CaptureShelf (VivisectProcessor& p) : proc (p)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    refresh();
}

CaptureShelf::~CaptureShelf()
{
    masterReference.clear();
    pool.removeAllJobs (true, 3000);
}

void CaptureShelf::requestPeaks (Take& t)
{
    if (t.requested) return;
    t.requested = true;
    pool.addJob (new PeakJob (this, t.file, t.size), true);
}

void CaptureShelf::setPeaks (const juce::File& f, juce::int64 size, double seconds, std::vector<float> peaks)
{
    for (auto& t : takes)
        if (t.file == f && t.size == size)
        {
            t.seconds = seconds;
            t.peaks = std::move (peaks);
            repaint();
            return;
        }
}

void CaptureShelf::refresh()
{
    const auto files = proc.listCaptures (kMaxTakes);

    bool same = files.size() == (int) takes.size();
    for (int i = 0; same && i < files.size(); ++i)
        same = files[i] == takes[(size_t) i].file
               && files[i].getSize() == takes[(size_t) i].size
               && files[i].getLastModificationTime() == takes[(size_t) i].modified;
    if (same) return;

    std::vector<Take> next;
    for (const auto& f : files)
    {
        Take t;
        t.file = f;
        t.size = f.getSize();
        t.modified = f.getLastModificationTime();
        for (auto& old : takes)                         // keep a finished analysis
            if (old.file == f && old.size == t.size && old.modified == t.modified)
            {
                t.seconds = old.seconds;
                t.peaks = old.peaks;
                t.requested = old.requested;
                break;
            }
        next.push_back (std::move (t));
    }
    takes = std::move (next);
    for (auto& t : takes) requestPeaks (t);
    hovered = pressed = -1;
    repaint();
}

juce::Rectangle<int> CaptureShelf::tileBounds (int i) const
{
    const int gap = 4;
    const int w = (getWidth() - gap * (kMaxTakes - 1)) / kMaxTakes;
    return { i * (w + gap), 0, w, getHeight() };
}

int CaptureShelf::tileAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) takes.size(); ++i)
        if (tileBounds (i).contains (p)) return i;
    return -1;
}

void CaptureShelf::paint (juce::Graphics& g)
{
    for (int i = 0; i < kMaxTakes; ++i)
    {
        const auto b = tileBounds (i);
        const bool filled = i < (int) takes.size();
        g.setColour (filled && i == hovered ? col::hovered (col::panel) : col::panel);
        g.fillRoundedRectangle (b.toFloat(), (float) metric::radiusPanel);
        g.setColour (filled && i == hovered ? col::accent2 : col::edge);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), (float) metric::radiusPanel, 1.f);

        if (! filled)
        {
            if (i == 0)
                draw::label (g, "NO CAPTURES YET", b, juce::Justification::centred, col::textMuted, 9.f);
            continue;
        }

        const auto& t = takes[(size_t) i];
        auto inner = b.reduced (4, 3);
        auto wave = inner.removeFromTop (22);
        if (! t.peaks.empty())
        {
            g.setColour (col::accent2);
            const float mid = (float) wave.getCentreY();
            const float half = (float) wave.getHeight() * 0.5f;
            const int n = (int) t.peaks.size();
            for (int x = 0; x < wave.getWidth(); ++x)
            {
                const float pk = t.peaks[(size_t) juce::jmin (n - 1, x * n / juce::jmax (1, wave.getWidth()))];
                const float h = juce::jmax (0.5f, pk * half);
                g.drawVerticalLine (wave.getX() + x, mid - h, mid + h);
            }
        }
        else
        {
            g.setColour (col::edge);
            g.drawHorizontalLine (wave.getCentreY(), (float) wave.getX(), (float) wave.getRight());
        }

        auto nameRow = inner.removeFromTop (11);
        auto lenRow = inner.removeFromTop (11);
        auto stem = t.file.getFileNameWithoutExtension().fromFirstOccurrenceOf ("Vivisect_", false, false);
        g.setColour (col::text);
        g.setFont (font::mono (8.f));
        g.drawText (stem, nameRow, juce::Justification::centredLeft, true);
        g.setColour (col::textMuted);
        g.drawText (t.seconds > 0.0 ? juce::String (t.seconds, 2) + " s" : juce::String ("-"),
                    lenRow, juce::Justification::centredLeft, true);
    }
}

juce::String CaptureShelf::getTooltip()
{
    const int i = tileAt (getMouseXYRelative());
    if (i < 0)
        return "The last eight captures, newest first. Drag one into your DAW or file manager, or "
               "onto Sample A or B in this window. Right-click a take to open its folder, rename or delete it.";
    const auto& t = takes[(size_t) i];
    return t.file.getFileName() + (t.seconds > 0.0 ? "  (" + juce::String (t.seconds, 2) + " s)" : juce::String())
           + "\nDrag out to the DAW, or onto Sample A / B. Right-click for more.";
}

void CaptureShelf::mouseMove (const juce::MouseEvent& e)
{
    const int i = tileAt (e.getPosition());
    if (i != hovered) { hovered = i; repaint(); }
}
void CaptureShelf::mouseExit (const juce::MouseEvent&)
{
    if (hovered != -1) { hovered = -1; repaint(); }
}

void CaptureShelf::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    pressed = tileAt (e.getPosition());
    pressPos = e.getPosition();
    if (pressed >= 0 && e.mods.isPopupMenu())
    {
        const int i = pressed;
        pressed = -1;
        showMenu (i);
    }
}

void CaptureShelf::mouseDrag (const juce::MouseEvent& e)
{
    if (pressed < 0 || dragging || e.mods.isPopupMenu()) return;
    if (e.getPosition().getDistanceFrom (pressPos) < 6) return;
    if (! juce::isPositiveAndBelow (pressed, (int) takes.size())) return;

    const auto f = takes[(size_t) pressed].file;
    if (! f.existsAsFile()) { refresh(); return; }
    dragging = true;
    pressed = -1;
    // Copy, never move: the take stays in the shelf after it lands in the DAW.
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
}

void CaptureShelf::mouseUp (const juce::MouseEvent&)
{
    pressed = -1;
    dragging = false;
}

void CaptureShelf::showMenu (int i)
{
    if (! juce::isPositiveAndBelow (i, (int) takes.size())) return;
    const auto f = takes[(size_t) i].file;

    juce::PopupMenu m;
    m.addItem (1, "Open location");
    m.addItem (2, "Load into Sample A", (bool) onLoadIntoSlot);
    m.addItem (3, "Load into Sample B", (bool) onLoadIntoSlot);
    m.addSeparator();
    m.addItem (4, "Rename...");
    m.addItem (5, "Delete");

    juce::WeakReference<CaptureShelf> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (tileBounds (i))),
                     [self, f] (int choice)
                     {
                         auto* s = self.get();
                         if (s == nullptr || choice == 0) return;
                         switch (choice)
                         {
                             case 1: f.revealToUser(); break;
                             case 2: if (s->onLoadIntoSlot) s->onLoadIntoSlot (0, f); break;
                             case 3: if (s->onLoadIntoSlot) s->onLoadIntoSlot (1, f); break;
                             case 4: s->askRename (f); break;
                             case 5: s->askDelete (f); break;
                             default: break;
                         }
                     });
}

void CaptureShelf::askRename (const juce::File& f)
{
    auto* aw = new juce::AlertWindow ("Rename capture", "New name (the .wav extension is kept):",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", f.getFileNameWithoutExtension());
    aw->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::WeakReference<CaptureShelf> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, f, aw] (int result)
    {
        auto* s = self.get();
        if (result != 1 || s == nullptr) return;

        const auto stem = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
        const auto target = f.getParentDirectory().getChildFile (stem + ".wav");
        if (stem.isEmpty() || (target != f && target.exists()) || ! (target == f || f.moveFileTo (target)))
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("Could not rename")
                                              .withMessage ("The name is empty, already used, or the file could not be renamed.")
                                              .withButton ("OK"),
                                          nullptr);
        s->refresh();
    }), true);
}

void CaptureShelf::askDelete (const juce::File& f)
{
    juce::WeakReference<CaptureShelf> self (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Delete capture")
                                      .withMessage ("Delete " + f.getFileName() + "?")
                                      .withButton ("Delete")
                                      .withButton ("Keep"),
                                  [self, f] (int result)
                                  {
                                      if (result != 1) return;
                                      if (! f.moveToTrash())
                                          f.deleteFile();
                                      if (auto* s = self.get()) s->refresh();
                                  });
}

// ===========================================================================
CapturePanel::CapturePanel (VivisectProcessor& p) : proc (p), shelf (p)
{
    addAndMakeVisible (captureBtn);
    addAndMakeVisible (cancelBtn);
    addAndMakeVisible (lengthBox);
    addAndMakeVisible (shelf);

    lengthBox.addItem ("1 bar", 1);
    lengthBox.addItem ("2 bars", 2);
    lengthBox.addItem ("4 bars", 3);
    lengthBox.addItem ("8 bars", 4);
    syncLength();
    lengthBox.onChange = [this]
    {
        const int id = lengthBox.getSelectedId();
        if (id >= 1 && id <= 4) proc.setCaptureBars (1 << (id - 1));
    };

    captureBtn.setColour (juce::TextButton::buttonOnColourId, col::clip);
    captureBtn.setTooltip ("Record the final output (after every effect) for the chosen number of bars. "
                           "While the host is playing it starts on the next bar line; standalone or stopped "
                           "it starts at once. The take is saved as a 24-bit WAV in the Captures folder.");
    cancelBtn.setTooltip ("Stop the capture in progress and throw it away. No file is left behind.");
    lengthBox.setTooltip ("How many bars each capture records: 1, 2, 4 or 8. Remembered between sessions.");
    captureBtn.onClick = [this]
    {
        const int id = lengthBox.getSelectedId();
        proc.startCapture (id >= 1 && id <= 4 ? 1 << (id - 1) : proc.captureBars());
        repaint();
    };
    cancelBtn.onClick = [this] { proc.cancelCapture(); repaint(); };

    const auto st = proc.captureState();       // do not announce takes finished before this view existed
    seenCompleted = st.completedCount;
    seenFailed = st.failedCount;
    startTimerHz (10);
}

CapturePanel::~CapturePanel()
{
    stopTimer();
}

void CapturePanel::syncLength()
{
    const int id = 1 + (proc.captureBars() == 2 ? 1 : proc.captureBars() == 4 ? 2 : proc.captureBars() == 8 ? 3 : 0);
    if (lengthBox.getSelectedId() != id)
        lengthBox.setSelectedId (id, juce::dontSendNotification);
}

void CapturePanel::showSavedPopup (const CaptureInfo& st)
{
    const auto f = st.lastFile;
    const auto text = "Saved successfully.\n\n"
                      "Name: " + f.getFileName() + "\n"
                      "Location: " + f.getParentDirectory().getFullPathName() + "\n"
                      "Length: " + juce::String (st.lastSeconds, 2) + " seconds\n"
                      "Quality: 24-bit PCM WAV";
    juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                           .withIconType (juce::MessageBoxIconType::InfoIcon)
                                           .withTitle ("Capture")
                                           .withMessage (text)
                                           .withButton ("OK"),
                                       nullptr);
}

void CapturePanel::poll()
{
    const auto st = proc.captureState();
    const bool idle = st.phase == CapturePhase::idle;
    captureBtn.setEnabled (idle);
    cancelBtn.setEnabled (! idle);
    captureBtn.setToggleState (st.phase == CapturePhase::recording, juce::dontSendNotification);

    if (st.completedCount != seenCompleted || st.failedCount != seenFailed)
    {
        const bool ok = st.completedCount != seenCompleted;
        seenCompleted = st.completedCount;
        seenFailed = st.failedCount;
        shelf.refresh();
        if (ok)
            showSavedPopup (st);
        else
            juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                                   .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                   .withTitle ("Capture failed")
                                                   .withMessage ("The capture could not be saved: " + st.lastError)
                                                   .withButton ("OK"),
                                               nullptr);
    }

    if (++tick >= 10)                      // once a second: pick up deletes and renames made outside
    {
        tick = 0;
        syncLength();
        shelf.refresh();
    }
    if (! idle || st.completedCount > 0)
        repaint (statusArea);
}

void CapturePanel::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (332);
    r.removeFromLeft (metric::grid);
    shelf.setBounds (r);

    auto top = left.removeFromTop (metric::buttonH).withTrimmedTop (0);
    captureBtn.setBounds (top.removeFromLeft (104));
    top.removeFromLeft (metric::grid);
    lengthBox.setBounds (top.removeFromLeft (88));
    top.removeFromLeft (metric::grid);
    cancelBtn.setBounds (top.removeFromLeft (88));
    left.removeFromTop (metric::gridFine);
    statusArea = left;
}

void CapturePanel::paint (juce::Graphics& g)
{
    const auto st = proc.captureState();
    juce::String text;
    juce::Colour c = col::textMuted;
    switch (st.phase)
    {
        case CapturePhase::idle:
            text = "READY";
            break;
        case CapturePhase::armed:
            text = "ARMED - WAITING FOR BAR LINE";
            c = (juce::Time::getMillisecondCounter() / 400) % 2 ? col::warn : col::textMuted;
            break;
        case CapturePhase::recording:
            text = "REC  " + juce::String (st.progress * st.seconds, 1) + " / " + juce::String (st.seconds, 1) + " s";
            c = col::clip;
            break;
        case CapturePhase::writing:
            text = "WRITING WAV...";
            c = col::accent2;
            break;
    }
    draw::label (g, text, statusArea.withTrimmedBottom (4), juce::Justification::centredLeft, c, 10.f);

    if (st.phase == CapturePhase::recording)
    {
        auto area = statusArea;
        auto bar = area.removeFromBottom (3).withTrimmedRight (2);
        g.setColour (col::edge);
        g.fillRect (bar);
        g.setColour (col::clip);
        g.fillRect (bar.withWidth (juce::roundToInt ((float) bar.getWidth() * (float) st.progress)));
    }
}
} // namespace vsx
