// ============================================================================
//  Editor layout tests (0.10.0 final layout pass).
//  Included by TestMain.cpp after the shared helpers (check, beginCase).
//
//  For Full and Simple view, at 50 / 100 / 150 % window size:
//    * no two visible interactive components overlap (the first-run hint strip,
//      the data stream and the tooltip window are intentional overlays);
//    * every visible component lies inside the editor;
//    * every interactive component is at least 16 px each way (design size);
//    * no visible component has an empty rectangle.
//  Also: the default window is at most 900 px tall, and toggling SIMPLE at run
//  time re-lays the window out cleanly.
//
//  Setting the environment variable VSX_SHOTS_DIR also writes PNG snapshots of
//  the editor (Full and Simple, 100 %) into that folder.
// ============================================================================
#include <cstdlib>
#include <functional>
#include <typeinfo>

namespace
{
struct LayoutNode
{
    Component* c = nullptr;
    String name;
    Rectangle<int> viewRect;       // design-size coordinates (inside VivisectView)
    Rectangle<int> editorRect;     // real editor pixels (after the zoom transform)
    bool interactive = false;      // button, knob, combo, shelf, history bar
    bool overlay = false;          // first-run hint (intentional overlay on the monitor)
};

String layoutName (Component& c)
{
    String n = typeid (c).name();
    if (auto* b = dynamic_cast<Button*> (&c))   n << " \"" << b->getButtonText() << "\"" << (b->getName().isNotEmpty() ? " " + b->getName() : String());
    if (auto* s = dynamic_cast<vsx::VsxSlider*> (&c)) n << " \"" << s->getCaption() << "\"";
    return n;
}

bool layoutIsInteractive (Component& c)
{
    return dynamic_cast<Button*> (&c) != nullptr || dynamic_cast<Slider*> (&c) != nullptr
        || dynamic_cast<ComboBox*> (&c) != nullptr || dynamic_cast<vsx::CaptureShelf*> (&c) != nullptr
        || dynamic_cast<vsx::HistoryBar*> (&c) != nullptr;
}

// Everything visible under `view`, not descending into leaves (a knob's or a
// combo's own internals are not layout).
void layoutCollect (AudioProcessorEditor& ed, Component& view, Component& c, bool underOverlay,
                    std::vector<LayoutNode>& out)
{
    for (auto* ch : c.getChildren())
    {
        if (! ch->isVisible()) continue;
        const bool ov = underOverlay || dynamic_cast<vsx::FirstRunHint*> (ch) != nullptr;
        LayoutNode n;
        n.c = ch;
        n.name = layoutName (*ch);
        n.viewRect = view.getLocalArea (ch, ch->getLocalBounds());
        n.editorRect = ed.getLocalArea (ch, ch->getLocalBounds());
        n.interactive = layoutIsInteractive (*ch);
        n.overlay = ov;
        out.push_back (n);
        if (! n.interactive)
            layoutCollect (ed, view, *ch, ov, out);
    }
}

// Lets the message thread run so timers (knob animation) settle before a snapshot.
void layoutPump (int ms)
{
    Timer::callAfterDelay (ms, [] { MessageManager::getInstance()->stopDispatchLoop(); });
    MessageManager::getInstance()->runDispatchLoop();
}

// Returns the number of violations found (each one is also reported).
int layoutAudit (AudioProcessorEditor& ed, const String& tag)
{
    int bad = 0;
    Component* view = ed.getNumChildComponents() > 0 ? ed.getChildComponent (0) : nullptr;
    if (view == nullptr) { check (false, tag + ": editor has a view"); return 1; }

    std::vector<LayoutNode> nodes;
    layoutCollect (ed, *view, *view, false, nodes);
    check (nodes.size() > 40, tag + ": the view has its controls (" + String ((int) nodes.size()) + " visible components)");

    const auto edBounds = ed.getLocalBounds().expanded (1);   // 1px for the zoom transform's rounding
    auto fail = [&] (const String& msg) { ++bad; check (false, tag + ": " + msg); };

    for (auto& n : nodes)
    {
        if (n.viewRect.isEmpty())
            fail ("zero-size visible component " + n.name);
        if (! edBounds.contains (n.editorRect))
            fail (n.name + " leaves the editor: " + n.editorRect.toString() + " in " + ed.getLocalBounds().toString());
        if (n.interactive && (n.viewRect.getWidth() < 16 || n.viewRect.getHeight() < 16))
            fail (n.name + " is smaller than 16 px: " + n.viewRect.toString());
    }

    // Overlap: interactive controls against each other, and against the display
    // widgets (monitor, pulse, level meter, LED). The data stream and the hint
    // strip are overlays by design.
    auto isDisplay = [] (Component* c)
    {
        return dynamic_cast<vsx::HeartMonitorDisplay*> (c) != nullptr || dynamic_cast<vsx::PulseMeter*> (c) != nullptr
            || dynamic_cast<vsx::LevelMeter*> (c) != nullptr || dynamic_cast<vsx::OutputLed*> (c) != nullptr;
    };
    for (size_t i = 0; i < nodes.size(); ++i)
        for (size_t j = i + 1; j < nodes.size(); ++j)
        {
            auto& a = nodes[i]; auto& b = nodes[j];
            if (a.overlay || b.overlay) continue;
            const bool ai = a.interactive || isDisplay (a.c), bi = b.interactive || isDisplay (b.c);
            if (! ai || ! bi) continue;
            if (a.c->isParentOf (b.c) || b.c->isParentOf (a.c)) continue;
            const auto in = a.viewRect.getIntersection (b.viewRect);
            if (! in.isEmpty())
                fail ("overlap between " + a.name + " " + a.viewRect.toString() + " and " + b.name + " " + b.viewRect.toString());
        }

    // The stream may only sit in genuinely empty space.
    for (auto& n : nodes)
        if (dynamic_cast<vsx::DataStream*> (n.c) != nullptr)
            for (auto& m : nodes)
                if (&m != &n && (m.interactive || isDisplay (m.c)) && ! m.overlay
                    && ! n.viewRect.getIntersection (m.viewRect).isEmpty())
                    fail ("data stream overlaps " + m.name);
    return bad;
}

void layoutSnapshot (AudioProcessorEditor& ed, const File& dir, const String& name)
{
    layoutPump (400);
    ed.repaint();
    auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
    dir.createDirectory();
    auto f = dir.getChildFile (name + ".png");
    f.deleteFile();
    FileOutputStream out (f);
    if (out.openedOk())
    {
        PNGImageFormat png;
        png.writeImageToStream (img, out);
    }
}

vsx::SimpleViewButton* layoutFindSimpleButton (Component& c)
{
    if (auto* x = dynamic_cast<vsx::SimpleViewButton*> (&c)) return x;
    for (auto* ch : c.getChildren())
        if (auto* r = layoutFindSimpleButton (*ch)) return r;
    return nullptr;
}
} // namespace

static void run_layout_tests (double kRate, int kBlock)
{
    beginCase ("Layout: default window is at most 900 px tall and keeps its aspect");
    {
        check (VivisectView::designHeight() <= 900, "design height " + String (VivisectView::designHeight()) + " <= 900");
        check (VivisectView::designWidth() >= 1000, "design width " + String (VivisectView::designWidth()));
    }

    const char* shotsEnv = std::getenv ("VSX_SHOTS_DIR");

    for (bool simple : { false, true })
    {
        const String view = simple ? "Simple" : "Full";
        beginCase ("Layout: " + view + " view at 50 / 100 / 150 %, no overlaps, nothing outside, controls >= 16 px");

        for (float scale : { 0.5f, 1.0f, 1.5f })
        {
            VivisectProcessor p;
            p.prepareToPlay (kRate, kBlock);
            p.setSimpleViewEnabled (simple);
            std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
            check (ed != nullptr, "editor created");
            if (ed == nullptr) continue;

            ed->setSize (roundToInt (VivisectView::designWidth() * scale), roundToInt (VivisectView::designHeight() * scale));
            const String tag = view + " " + String (roundToInt (scale * 100)) + "%";
            layoutAudit (*ed, tag);

            if (shotsEnv != nullptr && scale == 1.0f)
                layoutSnapshot (*ed, File (shotsEnv), simple ? "simple" : "full");
        }
    }

    beginCase ("Layout: toggling SIMPLE at run time re-lays the window out cleanly, both ways");
    {
        VivisectProcessor p;
        p.prepareToPlay (kRate, kBlock);
        p.setSimpleViewEnabled (false);
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        if (ed != nullptr)
        {
            ed->setSize (VivisectView::designWidth(), VivisectView::designHeight());
            auto* btn = layoutFindSimpleButton (*ed);
            check (btn != nullptr, "SIMPLE button found");
            if (btn != nullptr)
            {
                auto click = [&] { btn->setToggleState (! btn->getToggleState(), dontSendNotification); btn->onClick(); };
                layoutAudit (*ed, "Full before toggle");
                click();
                layoutAudit (*ed, "Simple after toggle");
                click();
                layoutAudit (*ed, "Full after toggling back");
                click();
                if (shotsEnv != nullptr)
                    layoutSnapshot (*ed, File (shotsEnv), "simple-toggled");
            }
        }
    }
}
