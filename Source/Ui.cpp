#include "Ui.h"
#include <algorithm>
#include <cmath>
#include "Parameters.h"

namespace vsx
{

// ===========================================================================
//  Icons — drawn in a 0..100 unit box
// ===========================================================================
juce::Path surgeonIconPath (int i)
{
    juce::Path p;
    switch (juce::jlimit (0, 5, i))
    {
        case 0: // scalpel
            p.startNewSubPath (8, 70);  p.lineTo (62, 20); p.lineTo (74, 30);
            p.lineTo (26, 78);          p.closeSubPath();
            p.addRectangle (70, 26, 24, 10);
            p.applyTransform (juce::AffineTransform::rotation (0.35f, 50, 50));
            break;
        case 1: // syringe
            p.addRoundedRectangle (24, 26, 44, 20, 3);   // barrel
            p.addRectangle (16, 30, 10, 12);             // plunger head
            p.addRectangle (10, 33, 8, 6);
            p.addRectangle (68, 34, 16, 4);              // tip
            p.addRectangle (84, 35.5f, 12, 1.6f);        // needle
            p.addRectangle (34, 22, 2.2f, 28);           // graduations
            p.addRectangle (44, 22, 2.2f, 28);
            p.addRectangle (54, 22, 2.2f, 28);
            p.applyTransform (juce::AffineTransform::rotation (-0.5f, 50, 50));
            break;
        case 2: // forceps
        {
            juce::Path arms;
            arms.startNewSubPath (30, 12); arms.cubicTo (8, 42, 40, 62, 50, 92);
            arms.startNewSubPath (70, 12); arms.cubicTo (92, 42, 60, 62, 50, 92);
            arms.startNewSubPath (34, 28); arms.lineTo (66, 28);
            juce::PathStrokeType (7.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
                .createStrokedPath (p, arms);
            break;
        }
        case 3: // bone saw
        {
            p.addRectangle (8, 40, 20, 20);              // handle
            p.addRectangle (28, 44, 58, 12);             // spine
            juce::Path teeth;
            for (int t = 0; t < 10; ++t)
            {
                const float x = 30.f + t * 5.6f;
                teeth.addTriangle (x, 56, x + 5.6f, 56, x + 2.8f, 66);
            }
            p.addPath (teeth);
            break;
        }
        case 4: // bandage
            p.addRoundedRectangle (12, 42, 76, 16, 8);
            p.applyTransform (juce::AffineTransform::rotation (-0.6f, 50, 50));
            {
                juce::Path pad; pad.addRoundedRectangle (38, 38, 24, 24, 3);
                p.addPath (pad);
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        p.addEllipse (43.f + a * 6.f, 43.f + b * 6.f, 2.2f, 2.2f);
            }
            break;
        default: // defibrillator paddles + bolt
        {
            p.addRoundedRectangle (14, 22, 26, 40, 4);
            p.addRoundedRectangle (60, 22, 26, 40, 4);
            p.addRectangle (24, 14, 6, 10);
            p.addRectangle (70, 14, 6, 10);
            juce::Path bolt;
            bolt.startNewSubPath (52, 30); bolt.lineTo (44, 52); bolt.lineTo (50, 52);
            bolt.lineTo (46, 74); bolt.lineTo (60, 46); bolt.lineTo (53, 46); bolt.closeSubPath();
            p.addPath (bolt);
            break;
        }
    }
    return p;
}

juce::Path gearIconPath()
{
    juce::Path p;
    const float cx = 50.f, cy = 50.f, rIn = 22.f, rOut = 40.f;
    const int teeth = 8;
    juce::Path ring;
    for (int t = 0; t < teeth; ++t)
    {
        const float a = juce::MathConstants<float>::twoPi * t / (float) teeth;
        juce::Path tooth;
        tooth.addRoundedRectangle (cx - 7.f, cy - rOut, 14.f, rOut - rIn + 8.f, 2.f);
        tooth.applyTransform (juce::AffineTransform::rotation (a, cx, cy));
        ring.addPath (tooth);
    }
    p.addPath (ring);
    p.addEllipse (cx - rIn - 4.f, cy - rIn - 4.f, (rIn + 4.f) * 2.f, (rIn + 4.f) * 2.f);
    p.setUsingNonZeroWinding (false);
    p.addEllipse (cx - 12.f, cy - 12.f, 24.f, 24.f);
    return p;
}

// ===========================================================================
//  Shared right-click menu — works on knobs, sliders, buttons and combos
// ===========================================================================
namespace
{
    std::unique_ptr<juce::AlertWindow> gValueWindow;

    void showValueEntry (ParamHost& host, const juce::String& paramID)
    {
        auto* param = host.params().getParameter (paramID);
        if (param == nullptr) return;

        gValueWindow = std::make_unique<juce::AlertWindow> (
            "SET VALUE", param->getName (64), juce::MessageBoxIconType::NoIcon);
        gValueWindow->addTextEditor ("v", param->getCurrentValueAsText(), {});
        gValueWindow->addButton ("SET",    1, juce::KeyPress (juce::KeyPress::returnKey));
        gValueWindow->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        gValueWindow->enterModalState (true, juce::ModalCallbackFunction::create (
            [&host, paramID] (int result)
            {
                const juce::String text = gValueWindow != nullptr ? gValueWindow->getTextEditorContents ("v")
                                                                  : juce::String();
                gValueWindow.reset();
                if (result != 1 || text.isEmpty()) return;
                if (auto* p = host.params().getParameter (paramID))
                    p->setValueNotifyingHost (juce::jlimit (0.f, 1.f, p->getValueForText (text)));
            }), false);
    }
}

void showParamContextMenu (ParamHost& host, const juce::String& paramID, juce::Component* target)
{
    auto* param = host.params().getParameter (paramID);
    if (param == nullptr) return;

    const int cc = host.ccForParam (paramID);

    juce::PopupMenu m;
    m.addSectionHeader (param->getName (48).toUpperCase());
    m.addItem (1, "Reset to default");
    m.addItem (2, "Set value...");
    m.addSeparator();
    m.addItem (3, cc >= 0 ? "MIDI learn  (re-map, now CC " + juce::String (cc) + ")"
                          : "MIDI learn  (move a controller)");
    m.addItem (4, cc >= 0 ? "Clear MIDI mapping (CC " + juce::String (cc) + ")" : "Clear MIDI mapping", cc >= 0);
    m.addSeparator();
    const bool locked = host.isParameterLocked (paramID);
    m.addItem (5, locked ? "Unlock from Randomize / Mutate" : "Lock for Randomize / Mutate",
               true, locked);
    m.addItem (6, "Clear all Randomize / Mutate locks", host.lockedParameterCount() > 0);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                     [&host, paramID] (int choice)
                     {
                         auto* p = host.params().getParameter (paramID);
                         if (p == nullptr) return;
                         switch (choice)
                         {
                             case 1: p->setValueNotifyingHost (p->getDefaultValue()); break;
                             case 2: showValueEntry (host, paramID); break;
                             case 3: host.beginMidiLearnFor (paramID); break;
                             case 4: host.clearCCForParam (paramID); break;
                             case 5: host.setParameterLocked (paramID, ! host.isParameterLocked (paramID)); break;
                             case 6: host.clearParameterLocks(); break;
                             default: break;
                         }
                     });
}

// ===========================================================================
//  LookAndFeel
// ===========================================================================
VsxLookAndFeel::VsxLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId,        col::bg);
    setColour (juce::Slider::textBoxTextColourId,                col::text);
    setColour (juce::Slider::textBoxOutlineColourId,             juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId,                        col::text);
    setColour (juce::ComboBox::textColourId,                     col::text);
    setColour (juce::ComboBox::backgroundColourId,               col::panel);
    setColour (juce::ComboBox::outlineColourId,                  col::edge);
    setColour (juce::ComboBox::arrowColourId,                    col::accent);
    setColour (juce::PopupMenu::backgroundColourId,              col::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId,   col::accent.withAlpha (0.20f));
    setColour (juce::PopupMenu::highlightedTextColourId,         col::text);
    setColour (juce::PopupMenu::textColourId,                    col::text);
    setColour (juce::PopupMenu::headerTextColourId,              col::accent);
    setColour (juce::TextEditor::backgroundColourId,             col::bg);
    setColour (juce::TextEditor::textColourId,                   col::text);
    setColour (juce::TextEditor::outlineColourId,                col::edge);
    setColour (juce::TextEditor::focusedOutlineColourId,         col::accent);
    setColour (juce::TextEditor::highlightColourId,              col::accent.withAlpha (0.30f));
    setColour (juce::ScrollBar::thumbColourId,                   col::edge.brighter (0.4f));
    setColour (juce::AlertWindow::backgroundColourId,            col::panel);
    setColour (juce::AlertWindow::textColourId,                  col::text);
    setColour (juce::AlertWindow::outlineColourId,               col::edge);
    setColour (juce::ToggleButton::textColourId,                 col::text);
    setColour (juce::ToggleButton::tickColourId,                 col::accent);
    setColour (juce::TooltipWindow::backgroundColourId,          col::panel);
    setColour (juce::TooltipWindow::textColourId,                col::textMuted);
}

juce::Font VsxLookAndFeel::getComboBoxFont (juce::ComboBox&)          { return font::ui (12.f); }
juce::Font VsxLookAndFeel::getLabelFont (juce::Label&)                { return font::ui (12.f); }
juce::Font VsxLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return font::ui (11.f, true); }
juce::Font VsxLookAndFeel::getPopupMenuFont()                         { return font::ui (13.f); }

// ---------------------------------------------------------------------------
void VsxLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                       float pos, float a0, float a1, juce::Slider& s)
{
    auto* vs = dynamic_cast<VsxSlider*> (&s);
    const float p       = vs != nullptr ? vs->animatedPos() : pos;
    const bool  hovered = vs != nullptr && (vs->isHovered() || s.isMouseButtonDown());
    const bool  isChaos = vs != nullptr && vs->isChaosKnob();

    auto area = juce::Rectangle<int> (x, y, w, h);
    auto valueRow = area.removeFromTop (12);
    auto labelRow = area.removeFromBottom (12);

    float want = (float) metric::knob;
    if (vs != nullptr)
        want = vs->getKnobSize() == VsxSlider::Size::small ? (float) metric::knobSmall
             : vs->getKnobSize() == VsxSlider::Size::large ? (float) metric::knobLarge
                                                           : (float) metric::knob;

    // the value arc lives outside the body: 4px gap + 3px stroke
    const float outer = juce::jmin ((float) area.getWidth(), (float) area.getHeight());
    const float bodyD = juce::jlimit (16.f, want, outer - 14.f);
    const float r     = bodyD * 0.5f;
    const auto  c     = area.toFloat().getCentre();
    const float arcR  = r + 4.f + 1.5f;
    const float ang   = a0 + p * (a1 - a0);

    const auto accent = isChaos ? col::accent : s.findColour (juce::Slider::rotarySliderFillColourId);
    const auto tint   = accent.isTransparent() ? col::accent : accent;

    // -- value arc ----------------------------------------------------------
    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.f, a0, a1, true);
    g.setColour (col::edge.withAlpha (0.6f));
    g.strokePath (track, juce::PathStrokeType (3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const bool bipolar = s.getMinimum() < 0.0;
    const float aMid = a0 + 0.5f * (a1 - a0);
    if (std::abs (ang - (bipolar ? aMid : a0)) > 0.001f)
    {
        juce::Path val;
        val.addCentredArc (c.x, c.y, arcR, arcR, 0.f,
                           bipolar ? juce::jmin (aMid, ang) : a0,
                           bipolar ? juce::jmax (aMid, ang) : ang, true);
        g.setColour (tint);
        g.strokePath (val, juce::PathStrokeType (3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // -- body ---------------------------------------------------------------
    const auto bodyR = juce::Rectangle<float> (c.x - r, c.y - r, bodyD, bodyD);
    draw::panelShadow (g, bodyR, r);
    juce::ColourGradient grad (hovered ? col::hovered (col::knobTop) : col::knobTop, c.x, c.y - r,
                               hovered ? col::hovered (col::knobBot) : col::knobBot, c.x, c.y + r, false);
    g.setGradientFill (grad);
    g.fillEllipse (bodyR);
    g.setColour (col::edge);
    g.drawEllipse (bodyR.reduced (0.5f), 1.f);

    if (isChaos)
    {
        // the chaos field: the body frays as the axis opens up
        const float spread = 0.15f + 0.85f * p;
        for (int i = 0; i < 48; ++i)
        {
            const float t = juce::MathConstants<float>::twoPi * i / 48.f;
            const float jit = (std::sin (i * 12.9898f) * 0.5f + 0.5f) * spread * r * 0.45f;
            g.setColour (tint.withAlpha (0.06f + 0.22f * spread));
            g.drawLine (c.x + std::cos (t) * (r + 1.f),          c.y + std::sin (t) * (r + 1.f),
                        c.x + std::cos (t) * (r + 1.f + jit),    c.y + std::sin (t) * (r + 1.f + jit), 1.f);
        }
    }

    // -- indicator : 2px accent line, centre to rim, rounded cap -------------
    const float dx = std::sin (ang), dy = -std::cos (ang);
    g.setColour (tint);
    juce::Path ind;
    ind.startNewSubPath (c.x, c.y);
    ind.lineTo (c.x + dx * (r - 2.f), c.y + dy * (r - 2.f));
    g.strokePath (ind, juce::PathStrokeType (2.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // -- centre dot : accent when moved off default, muted at default --------
    const bool atDefault = std::abs (s.getValue() - s.getDoubleClickReturnValue()) < 1.0e-4;
    g.setColour (atDefault ? col::textMuted : tint);
    g.fillEllipse (c.x - 2.f, c.y - 2.f, 4.f, 4.f);

    // -- caption below, value above (value only on hover / drag) -------------
    if (vs != nullptr)
    {
        draw::label (g, vs->getCaption(), labelRow, juce::Justification::centred,
                     hovered ? col::text : col::textMuted, 11.f);
        if (hovered)
        {
            g.setColour (col::text);
            g.setFont (font::mono (13.f));
            g.drawText (s.getTextFromValue (s.getValue()), valueRow, juce::Justification::centred, false);
        }
    }
}

// ---------------------------------------------------------------------------
void VsxLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                       float pos, float, float,
                                       juce::Slider::SliderStyle style, juce::Slider& s)
{
    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    auto* vs = dynamic_cast<VsxSlider*> (&s);
    const bool hovered = vs != nullptr && (vs->isHovered() || s.isMouseButtonDown());
    const auto tint = col::accent;

    if (style == juce::Slider::LinearVertical)
    {
        const float cx = b.getCentreX();
        auto trackR = juce::Rectangle<float> (cx - 2.f, b.getY() + 12.f, 4.f, b.getHeight() - 24.f);
        g.setColour (col::edge);
        g.fillRoundedRectangle (trackR, 2.f);
        g.setColour (tint);
        g.fillRoundedRectangle (trackR.withTop (pos).withBottom (trackR.getBottom()), 2.f);

        auto thumb = juce::Rectangle<float> (24.f, 16.f).withCentre ({ cx, pos });
        g.setGradientFill ({ hovered ? col::hovered (col::knobTop) : col::knobTop, 0.f, thumb.getY(),
                             hovered ? col::hovered (col::knobBot) : col::knobBot, 0.f, thumb.getBottom(), false });
        g.fillRoundedRectangle (thumb, 2.f);
        g.setColour (tint);
        g.drawRoundedRectangle (thumb.reduced (0.5f), 2.f, 1.f);
        return;
    }

    // horizontal
    const float cy = b.getCentreY();
    auto trackR = juce::Rectangle<float> (b.getX() + 8.f, cy - 2.f, b.getWidth() - 16.f, 4.f);
    g.setColour (col::edge);
    g.fillRoundedRectangle (trackR, 2.f);

    const bool bipolar = s.getMinimum() < 0.0;
    const float mid = trackR.getCentreX();
    g.setColour (tint);
    if (bipolar) g.fillRoundedRectangle (juce::jmin (mid, pos), cy - 2.f, std::abs (pos - mid), 4.f, 2.f);
    else         g.fillRoundedRectangle (trackR.getX(), cy - 2.f, juce::jmax (0.f, pos - trackR.getX()), 4.f, 2.f);

    // ticks : 1px muted lines outside the track, at the logical quarters
    g.setColour (col::textMuted.withAlpha (0.35f));
    for (int t = 0; t <= 4; ++t)
    {
        const float tx = trackR.getX() + trackR.getWidth() * (float) t / 4.f;
        g.drawLine (tx, cy + 6.f, tx, cy + 9.f, 1.f);
    }

    const float th = juce::jmin (24.f, b.getHeight());
    auto thumb = juce::Rectangle<float> (16.f, th).withCentre ({ pos, cy });
    g.setGradientFill ({ hovered ? col::hovered (col::knobTop) : col::knobTop, 0.f, thumb.getY(),
                         hovered ? col::hovered (col::knobBot) : col::knobBot, 0.f, thumb.getBottom(), false });
    g.fillRoundedRectangle (thumb, 2.f);
    g.setColour (tint);
    g.drawRoundedRectangle (thumb.reduced (0.5f), 2.f, 1.f);
}

// ---------------------------------------------------------------------------
void VsxLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& btn,
                                           const juce::Colour&, bool hl, bool down)
{
    auto b = btn.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = btn.getToggleState();
    const auto tint = btn.findColour (juce::TextButton::buttonOnColourId).isTransparent()
                        ? col::accent : btn.findColour (juce::TextButton::buttonOnColourId);

    auto fill = on ? tint.withAlpha (0.15f) : col::panel;
    if (hl || down) fill = col::hovered (fill);

    g.setColour (fill);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (on ? tint : col::edge);
    g.drawRoundedRectangle (b, (float) metric::radiusPanel, 1.f);

    if (auto* vb = dynamic_cast<VsxButton*> (&btn))
        if (vb->flashAmount() > 0.01f)
        {
            g.setColour (tint.withAlpha (0.45f * vb->flashAmount()));
            g.fillRoundedRectangle (b, (float) metric::radiusPanel);
        }
}

void VsxLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& btn, bool hl, bool)
{
    const bool on = btn.getToggleState();
    const auto tint = btn.findColour (juce::TextButton::buttonOnColourId).isTransparent()
                        ? col::accent : btn.findColour (juce::TextButton::buttonOnColourId);
    draw::label (g, btn.getButtonText(), btn.getLocalBounds(), juce::Justification::centred,
                 on ? tint : (hl ? col::text : col::textMuted), 11.f);
}

void VsxLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& btn, bool hl, bool down)
{
    auto b = btn.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = btn.getToggleState();
    const auto tint = btn.findColour (juce::ToggleButton::tickColourId);

    auto fill = on ? tint.withAlpha (0.15f) : col::panel;
    if (hl || down) fill = col::hovered (fill);
    g.setColour (fill);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (on ? tint : col::edge);
    g.drawRoundedRectangle (b, (float) metric::radiusPanel, 1.f);

    // 2px status pip on the left edge, so on/off reads at a glance
    g.setColour (on ? tint : col::edge);
    g.fillRect (b.getX() + 6.f, b.getCentreY() - 6.f, 2.f, 12.f);

    draw::label (g, btn.getButtonText(), btn.getLocalBounds().withTrimmedLeft (14),
                 juce::Justification::centredLeft, on ? tint : col::textMuted, 11.f);
}

// ---------------------------------------------------------------------------
void VsxLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool,
                                   int, int, int, int, juce::ComboBox& box)
{
    auto b = juce::Rectangle<float> (0.f, 0.f, (float) w, (float) h).reduced (0.5f);
    const bool hovered = box.isMouseOver (true);
    g.setColour (hovered ? col::hovered (col::panel) : col::panel);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (col::edge);
    g.drawRoundedRectangle (b, (float) metric::radiusPanel, 1.f);

    juce::Path tri;
    const float cx = b.getRight() - 12.f, cy = b.getCentreY();
    tri.addTriangle (cx - 4.f, cy - 2.5f, cx + 4.f, cy - 2.5f, cx, cy + 3.f);
    g.setColour (col::accent);
    g.fillPath (tri);
}

void VsxLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 0, box.getWidth() - 26, box.getHeight());
    label.setFont (font::ui (12.f));
    label.setColour (juce::Label::textColourId, col::text);
}

// ---------------------------------------------------------------------------
void VsxLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    auto b = juce::Rectangle<float> ((float) w, (float) h);
    const float radius = juce::jmin (b.getHeight() * 0.5f, 10.f);
    draw::panelShadow (g, b.reduced (1.f), radius);
    g.setColour (col::bg.withAlpha (0.97f));
    g.fillRoundedRectangle (b.reduced (1.f), radius);
    g.setColour (col::edge);
    g.drawRoundedRectangle (b.reduced (1.f), radius, 1.f);

    g.setColour (col::textMuted);
    g.setFont (font::ui (12.f));
    g.drawFittedText (text, b.reduced (10.f, 6.f).toNearestInt(), juce::Justification::centredLeft, 6);
}

void VsxLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    auto b = juce::Rectangle<float> ((float) w, (float) h);
    g.setColour (col::panel);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.setColour (col::edge);
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusPanel, 1.f);
}

// ===========================================================================
//  VsxSlider
// ===========================================================================
VsxSlider::VsxSlider (ParamHost& h, juce::String paramID, juce::String cap, Size sz)
    : host (h), pid (std::move (paramID)), caption (std::move (cap)), size (sz)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                         juce::MathConstants<float>::pi * 2.75f, true);
    setVelocityBasedMode (false);
    setMouseDragSensitivity (250);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setColour (juce::Slider::rotarySliderFillColourId, col::accent);

    if (auto* p = host.params().getParameter (pid))
        setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));

    lastTick = juce::Time::getMillisecondCounter();
    startTimerHz (60);
}

void VsxSlider::valueChanged()
{
    juce::Slider::valueChanged();
    if (! isTimerRunning()) { lastTick = juce::Time::getMillisecondCounter(); startTimerHz (60); }
}

void VsxSlider::timerCallback()
{
    const float target = (float) valueToProportionOfLength (getValue());
    const auto now = juce::Time::getMillisecondCounter();
    const float dt = (float) (now - lastTick);
    lastTick = now;

    const float next = draw::easeTowards (anim, target, dt);
    if (std::abs (next - anim) > 1.0e-4f) { anim = next; repaint(); }
    else if (anim != target)              { anim = target; repaint(); }
    else                                   stopTimer();
}

void VsxSlider::mouseEnter (const juce::MouseEvent& e)
{
    hover = true;
    repaint();
    juce::Slider::mouseEnter (e);
}
void VsxSlider::mouseExit (const juce::MouseEvent& e)
{
    hover = false;
    repaint();
    juce::Slider::mouseExit (e);
}
void VsxSlider::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showParamContextMenu (host, pid, this);
        return;
    }
    // vertical drag is the default; Shift = coarse, Ctrl/Cmd = ultra-fine
    setMouseDragSensitivity (e.mods.isShiftDown()   ? 80
                           : e.mods.isCommandDown() ? 1600
                                                    : 250);
    juce::Slider::mouseDown (e);
}

// ===========================================================================
//  VsxComboBox / VsxButton / VsxToggle
// ===========================================================================
VsxComboBox::VsxComboBox (ParamHost& h, juce::String paramID)
    : host (h), pid (std::move (paramID)) {}

void VsxComboBox::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showParamContextMenu (host, pid, this);
        return;
    }
    juce::ComboBox::mouseDown (e);
}

VsxButton::VsxButton (const juce::String& text, ParamHost* h, juce::String paramID)
    : juce::TextButton (text), host (h), pid (std::move (paramID))
{
    setColour (juce::TextButton::buttonOnColourId, col::accent);
}
void VsxButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && host != nullptr && pid.isNotEmpty())
    {
        showParamContextMenu (*host, pid, this);
        return;
    }
    juce::TextButton::mouseDown (e);
}
void VsxButton::clicked()
{
    flash = 1.f;                 // 100ms accent flash on every press
    startTimerHz (60);
    juce::TextButton::clicked();
}
void VsxButton::timerCallback()
{
    flash -= 1.f / 6.f;          // ~100ms at 60Hz
    if (flash <= 0.f) { flash = 0.f; stopTimer(); }
    repaint();
}

VsxToggle::VsxToggle (const juce::String& text, ParamHost& h, juce::String paramID)
    : juce::ToggleButton (text), host (h), pid (std::move (paramID)) {}

void VsxToggle::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showParamContextMenu (host, pid, this);
        return;
    }
    juce::ToggleButton::mouseDown (e);
}

// ===========================================================================
//  HeartMonitorDisplay
// ===========================================================================
HeartMonitorDisplay::HeartMonitorDisplay (std::function<void (MonitorSnapshot&)> f)
    : fill (std::move (f))
{
    setInterceptsMouseClicks (true, false);
    setTooltip ("The specimen buffer. Teal trace is the rolling audio, orange bands are "
                "slices the surgeons are holding, red ticks are detected transients.");
    startTimerHz (30);
}
void HeartMonitorDisplay::paint (juce::Graphics& g)
{
    if (fill) fill (snap);
    auto b = getLocalBounds().toFloat();

    g.setColour (col::bg);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);
    g.reduceClipRegion (getLocalBounds());

    const int COLS = MonitorSnapshot::COLS;
    const float cw = b.getWidth() / COLS;

    // spectral brightness wash
    for (int c = 0; c < COLS; ++c)
    {
        const float br = snap.bright[c];
        g.setColour (col::accent2.withAlpha (0.04f + 0.10f * br));
        g.fillRect (c * cw, 0.f, cw + 1.f, b.getHeight());
    }

    g.setColour (col::edge.withAlpha (0.45f));
    for (int k = 1; k < 8; ++k)
        g.drawHorizontalLine ((int) (b.getHeight() * k / 8.f), 0.f, b.getWidth());

    for (int gi = 0; gi < snap.numGrabs; ++gi)
    {
        const auto& G = snap.grabs[gi];
        const auto cc = col::surgeon (G.surgeon);
        const float a = juce::jlimit (0.f, 1.f, 1.f - G.age);
        g.setColour (cc.withAlpha (0.05f + 0.20f * a));
        g.fillRect (G.c0 * cw, 0.f, juce::jmax (cw, (G.c1 - G.c0) * cw), b.getHeight());
        g.setColour (cc.withAlpha (0.55f * a));
        g.drawVerticalLine ((int) (G.c0 * cw), 0.f, b.getHeight());
    }

    const float mid = b.getHeight() * 0.5f;
    const float amp = b.getHeight() * 0.46f;
    for (int c = 0; c < COLS; ++c)
    {
        const float x  = c * cw + cw * 0.5f;
        const float y0 = mid - snap.envMax[c] * amp;
        const float y1 = mid - snap.envMin[c] * amp;
        g.setColour (col::accent2.withAlpha (0.20f));
        g.drawLine (x, y0, x, y1, 2.4f);
        g.setColour (col::accent2);
        g.drawLine (x, y0, x, y1, 0.9f);
    }

    g.setColour (col::clip.withAlpha (0.85f));
    for (int t = 0; t < snap.numTransients; ++t)
    {
        const float x = snap.transientCol[t] * cw;
        g.drawLine (x, 0.f, x, b.getHeight() * 0.12f, 1.4f);
        g.drawLine (x, b.getHeight() * 0.88f, x, b.getHeight(), 1.4f);
    }

    // write head
    g.setColour (col::accent.withAlpha (0.9f));
    g.drawVerticalLine ((int) (b.getWidth() - 2.f), 0.f, b.getHeight());
    g.fillEllipse (b.getWidth() - 6.f, mid - 3.f, 6.f, 6.f);

    scan += 0.010f;
    if (scan > 1.f) scan -= 1.f;
    g.setColour (col::accent2.withAlpha (0.05f));
    g.fillRect (scan * b.getWidth(), 0.f, 2.f, b.getHeight());

    if (snap.frozen)
    {
        g.setColour (col::clip);
        g.setFont (font::mono (12.f, true));
        g.drawText ("// SPECIMEN FROZEN", getLocalBounds().reduced (10), juce::Justification::topRight);
    }

    g.setColour (col::edge);
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusPanel, 1.f);
}

// ===========================================================================
//  PulseMeter
// ===========================================================================
PulseMeter::PulseMeter (std::function<float()> p, std::function<float()> c)
    : getPulse (std::move (p)), getChaos (std::move (c))
{
    setInterceptsMouseClicks (true, false);
    setTooltip ("Chaos pulse. The trace beats faster and more erratically as Chaos opens up "
                "- a read-out of how unpredictable the scheduler is right now.");
    startTimerHz (60);
}
void PulseMeter::timerCallback()
{
    const float pulse = getPulse ? getPulse() : 0.f;
    const float ch    = getChaos ? getChaos() : 0.f;
    const int W = juce::jmax (2, getWidth());
    if ((int) trace.size() != W) trace.assign ((size_t) W, 0.f);

    const int step = 3;
    for (int i = 0; i + step < W; ++i) trace[(size_t) i] = trace[(size_t) (i + step)];

    sinceBeat += 0.05f + pulse * 0.05f;
    for (int k = 0; k < step; ++k)
    {
        float v = (rng.nextFloat() * 2.f - 1.f) * 0.03f * (0.3f + ch);
        if (sinceBeat >= nextBeat)
        {
            sinceBeat = 0.f;
            nextBeat = (0.55f + rng.nextFloat() * 0.4f) / (0.4f + pulse * 1.7f);
            if (ch > 0.6f && rng.nextFloat() < ch - 0.4f) nextBeat *= 0.2f + rng.nextFloat();
            beating = true; beatT = 0.f; flash = 1.f;
        }
        if (beating)
        {
            const float t = beatT;
            if      (t < 0.08f) v += -0.15f * (t / 0.08f);
            else if (t < 0.16f) v += -0.15f + 1.45f * ((t - 0.08f) / 0.08f);
            else if (t < 0.26f) v +=  1.30f - 1.75f * ((t - 0.16f) / 0.10f);
            else if (t < 0.40f) v += -0.45f + 0.45f * ((t - 0.26f) / 0.14f);
            else beating = false;
            beatT += 0.05f + ch * 0.02f;
        }
        trace[(size_t) (W - step + k)] = v;
    }
    flash *= 0.85f;
    repaint();
}
void PulseMeter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (col::bg);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);

    const float mid = b.getHeight() * 0.62f;
    const float amp = b.getHeight() * 0.42f;
    g.setColour (col::edge);
    g.drawHorizontalLine ((int) mid, 0.f, b.getWidth());

    juce::Path p;
    for (int i = 0; i < (int) trace.size(); ++i)
    {
        const float x = (float) i;
        const float y = mid - trace[(size_t) i] * amp;
        if (i == 0) p.startNewSubPath (x, y);
        else        p.lineTo (x, y);
    }
    g.setColour (col::accent2.withAlpha (0.22f));
    g.strokePath (p, juce::PathStrokeType (3.f));
    g.setColour (col::accent2);
    g.strokePath (p, juce::PathStrokeType (1.2f));

    if (flash > 0.02f)
    {
        g.setColour (col::accent.withAlpha (flash * 0.6f));
        g.fillRect (b.removeFromRight (6.f));
    }
    draw::label (g, "CHAOS PULSE", getLocalBounds().reduced (8, 6).removeFromTop (12),
                 juce::Justification::centredLeft, col::textMuted, 11.f);

    g.setColour (col::edge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), (float) metric::radiusPanel, 1.f);
}

// ===========================================================================
//  LevelMeter
// ===========================================================================
LevelMeter::LevelMeter (std::function<float (int)> peakForChannel)
    : getPeak (std::move (peakForChannel))
{
    setInterceptsMouseClicks (true, false);
    setTooltip ("Output level. Peak hold sits for 1.5 seconds then falls at 20 dB/s. "
                "The number is the highest peak since the last hold.");
    startTimerHz (30);
}
void LevelMeter::timerCallback()
{
    const float dt = 1.f / 30.f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float raw = getPeak ? juce::jlimit (0.f, 2.f, getPeak (ch)) : 0.f;
        lvl[ch] = juce::jmax (raw, lvl[ch] - dt * 1.6f);

        if (raw >= hold[ch]) { hold[ch] = raw; holdAge[ch] = 0.f; }
        else
        {
            holdAge[ch] += dt;
            if (holdAge[ch] > 1.5f)                     // then fall at 20 dB/s
                hold[ch] = juce::jmax (raw, hold[ch] * std::pow (10.f, -20.f * dt / 20.f));
        }
    }
    const float h = juce::jmax (hold[0], hold[1]);
    peakDb = h > 1.0e-5f ? juce::Decibels::gainToDecibels (h) : -100.f;
    repaint();
}
void LevelMeter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour (col::bg);
    g.fillRoundedRectangle (b.toFloat(), (float) metric::radiusSmall);

    auto readout = b.removeFromTop (12);
    g.setColour (peakDb > -0.1f ? col::clip : col::textMuted);
    g.setFont (font::mono (11.f));
    g.drawText (peakDb <= -99.f ? "-INF" : juce::String (peakDb, 1) + " dB",
                readout.reduced (4, 0), juce::Justification::centredRight, false);

    b.removeFromTop (2);
    const int barH = juce::jmax (4, (b.getHeight() - 4) / 2);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto row = b.removeFromTop (barH).toFloat();
        b.removeFromTop (4);
        g.setColour (col::edge.withAlpha (0.6f));
        g.fillRoundedRectangle (row, (float) metric::radiusSmall);

        // -60..0 dB, smooth gradient, no LED gaps
        auto norm = [] (float lin)
        {
            if (lin <= 1.0e-5f) return 0.f;
            return juce::jlimit (0.f, 1.f, (juce::Decibels::gainToDecibels (lin) + 60.f) / 60.f);
        };
        const float v = norm (lvl[ch]);
        if (v > 0.f)
        {
            juce::ColourGradient grad (draw::meterColour (0.f), row.getX(), 0.f,
                                       draw::meterColour (1.f), row.getRight(), 0.f, false);
            grad.addColour (0.55, draw::meterColour (0.55f));
            grad.addColour (0.80, draw::meterColour (0.80f));
            g.setGradientFill (grad);
            g.fillRoundedRectangle (row.withWidth (row.getWidth() * v), (float) metric::radiusSmall);
        }
        const float hv = norm (hold[ch]);
        if (hv > 0.f)
        {
            g.setColour (draw::meterColour (hv));
            g.drawLine (row.getX() + row.getWidth() * hv, row.getY(),
                        row.getX() + row.getWidth() * hv, row.getBottom(), 1.f);
        }
    }
}

// ===========================================================================
//  SurgeonStrip
// ===========================================================================
SurgeonStrip::SurgeonStrip (ParamHost& hostRef, int surgeonIndex, std::function<float()> activity)
    : idx (surgeonIndex), getActivity (std::move (activity)),
      onBtn (surgeonName (surgeonIndex), hostRef, sid (surgeonIndex, "on")),
      route (hostRef, sid (surgeonIndex, "route"))
{
    icon = surgeonIconPath (idx);
    const auto tint = col::surgeon (idx);

    onBtn.setColour (juce::ToggleButton::tickColourId, tint);
    onBtn.setTooltip (juce::String (surgeonName (idx)) + " on / off.");
    addAndMakeVisible (onBtn);
    onA = std::make_unique<BA> (hostRef.params(), sid (idx, "on"), onBtn);

    auto make = [&] (const char* suffix, const juce::String& cap, const juce::String& tip)
    {
        auto s = std::make_unique<VsxSlider> (hostRef, sid (idx, suffix), cap, VsxSlider::Size::small);
        s->setColour (juce::Slider::rotarySliderFillColourId, tint);
        s->setTooltip (tip);
        addAndMakeVisible (*s);
        knobA.add (new SA (hostRef.params(), sid (idx, suffix), *s));
        return s;
    };

    mix  = make ("mix",  "MIX",  "How much of this surgeon reaches the wet bus.");
    prob = make ("prob", "PROB", "Chance this surgeon acts when the scheduler offers it a slice.");
    k1   = make ("p1",   paramLabel (idx, 0), juce::String (surgeonName (idx)) + " " + paramLabel (idx, 0) + ".");
    k2   = make ("p2",   paramLabel (idx, 1), juce::String (surgeonName (idx)) + " " + paramLabel (idx, 1) + ".");
    k3   = make ("p3",   paramLabel (idx, 2), juce::String (surgeonName (idx)) + " " + paramLabel (idx, 2) + ".");

    route.addItemList ({ "Off", "from S1", "from S2", "from S3", "from S4", "from S5", "from S6" }, 1);
    route.setTooltip ("Route another surgeon's output back through the specimen before this one reads it.");
    addAndMakeVisible (route);
    routeA = std::make_unique<CA> (hostRef.params(), sid (idx, "route"), route);

    startTimerHz (24);
}
void SurgeonStrip::timerCallback()
{
    const float a = getActivity ? getActivity() : 0.f;
    if (std::abs (a - act) > 0.005f || a > 0.f) { act = a; repaint(); }
}
void SurgeonStrip::resized()
{
    auto r = getLocalBounds().reduced (metric::grid, metric::gridFine);
    r.removeFromLeft (48 + metric::grid);          // icon area (painted)

    auto left = r.removeFromLeft (136);
    onBtn.setBounds (left.withSizeKeepingCentre (136, metric::buttonH));

    r.removeFromLeft (metric::grid);
    const int kw = juce::jlimit (56, 72, (r.getWidth() - 112) / 5);
    for (auto* s : { mix.get(), prob.get(), k1.get(), k2.get(), k3.get() })
    {
        s->setBounds (r.removeFromLeft (kw));
        r.removeFromLeft (metric::gridFine);
    }
    r.removeFromLeft (metric::gridFine);
    route.setBounds (r.removeFromLeft (juce::jmin (104, juce::jmax (0, r.getWidth())))
                      .withSizeKeepingCentre (juce::jmin (104, juce::jmax (0, r.getWidth())), metric::buttonH - 4));
}
void SurgeonStrip::paint (juce::Graphics& g)
{
    const auto cc = col::surgeon (idx);

    g.setColour (col::panel);
    g.fillRect (getLocalBounds());

    auto ib = juce::Rectangle<float> ((float) metric::grid, (getHeight() - 44) * 0.5f, 44.f, 44.f);
    if (act > 0.01f)
    {
        g.setColour (cc.withAlpha (0.10f + 0.30f * act));
        g.fillRoundedRectangle (ib.expanded (4.f), (float) metric::radiusPanel);
    }
    juce::Path pp = icon;
    pp.applyTransform (juce::AffineTransform::fromTargetPoints (
        0.f, 0.f,   ib.getX(),     ib.getY(),
        100.f, 0.f, ib.getRight(), ib.getY(),
        0.f, 100.f, ib.getX(),     ib.getBottom()));
    g.setColour (cc.withAlpha (0.35f + 0.6f * juce::jlimit (0.f, 1.f, 0.25f + act)));
    g.fillPath (pp);

    // sections are separated by a 1px line, never by nested boxes
    g.setColour (col::edge);
    g.drawHorizontalLine (getHeight() - 1, (float) metric::pad, (float) getWidth() - metric::pad);
}

// ===========================================================================
//  HistoryBar
// ===========================================================================
void HistoryBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (b, (float) metric::radiusSmall);

    const int cnt = getCount ? getCount() : 0;
    g.setColour (col::edge.brighter (0.25f));
    for (int i = 0; i < cnt; ++i)
    {
        const float x = getWidth() * (float) i / 120.f;
        g.drawVerticalLine ((int) x, getHeight() * 0.35f, getHeight() * 0.65f);
    }
    g.setColour (col::accent);
    g.drawVerticalLine ((int) (hover * getWidth()), 0.f, (float) getHeight());

    draw::label (g, "60s history - drag to rewind the whole plugin",
                 getLocalBounds().reduced (metric::grid, 0), juce::Justification::centredLeft,
                 col::textMuted, 10.f);

    g.setColour (col::edge);
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusSmall, 1.f);
}

// ===========================================================================
//  Overlay panels
// ===========================================================================
OverlayPanel::OverlayPanel (juce::String t) : title (std::move (t))
{
    addAndMakeVisible (closeBtn);
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    setInterceptsMouseClicks (true, true);
}
juce::Rectangle<int> OverlayPanel::contentArea() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (760, getWidth() - 2 * metric::pad),
                                                   juce::jmin (760, getHeight() - 2 * metric::pad));
}
void OverlayPanel::paint (juce::Graphics& g)
{
    g.fillAll (col::bg.withAlpha (0.86f));

    auto card = contentArea().toFloat();
    draw::panelShadow (g, card, (float) metric::radiusWindow);
    g.setColour (col::panel);
    g.fillRoundedRectangle (card, (float) metric::radiusWindow);
    g.setColour (col::edge);
    g.drawRoundedRectangle (card.reduced (0.5f), (float) metric::radiusWindow, 1.f);

    auto head = card.reduced ((float) metric::pad, 0.f).withHeight (32.f)
                    .withY (card.getY() + (float) metric::grid);
    draw::sectionHeader (g, title, head.toNearestInt());
    draw::separator (g, juce::Rectangle<int> ((int) card.getX() + metric::pad,
                                              (int) head.getBottom() + 2,
                                              (int) card.getWidth() - 2 * metric::pad, 1));
}
void OverlayPanel::resized()
{
    auto card = contentArea();
    closeBtn.setBounds (card.getRight() - metric::pad - 88,
                        card.getY() + metric::grid + 2, 88, metric::buttonH - 4);
}

// ---------------------------------------------------------------------------
HelpPanel::HelpPanel() : OverlayPanel ("VIVISECT - HELP")
{
    body.setMultiLine (true, true);
    body.setReadOnly (true);
    body.setScrollbarsShown (true);
    body.setCaretVisible (false);
    body.setPopupMenuEnabled (false);
    body.setFont (font::ui (13.f));
    body.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    body.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    body.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    body.setColour (juce::TextEditor::textColourId, col::text);
    addAndMakeVisible (body);

    homeBtn.setTooltip (juce::String ("Open ") + product::home + " in your browser.");
    gitBtn.setTooltip  (juce::String ("Open the source repository, ") + product::github + ".");
    mailBtn.setTooltip (juce::String ("Start an email to ") + product::support
                        + ". Say which DAW and OS you are on.");
    homeBtn.onClick = [] { openHomePage(); };
    gitBtn.onClick  = [] { openGitHub(); };
    mailBtn.onClick = [] { openSupportMail (juce::String (product::name) + " "
                                            + JucePlugin_VersionString + " - support request"); };
    presetsBtn.setTooltip ("Open the folder Vivisect reads user presets from.");
    presetsBtn.onClick = [] { auto d = userPresetDir(); d.createDirectory(); d.revealToUser(); };
    debugBtn.setTooltip ("Opens the debug window: the plugin's raw internals, the crash log "
                         "switch and the troubleshooting export.");
    for (auto* b : { &homeBtn, &gitBtn, &mailBtn, &presetsBtn, &debugBtn })
        addAndMakeVisible (b);

    const juce::String v = JucePlugin_VersionString;
    body.setText (
        "VIVISECT  v" + v + "   -   sample butchery\n"
        "Specimen Audio\n"
        "\n"
        "WHAT IT IS\n"
        "Vivisect treats the incoming signal as a specimen, not as a signal. Everything you\n"
        "play into it is written to a rolling, bar-synced buffer. A rack of six parallel\n"
        "\"surgeons\" cut slices out of that buffer and reinject them, in real time.\n"
        "\n"
        "THE WORKFLOW\n"
        "  1. Feed it a loop. The monitor at the top fills with the live specimen.\n"
        "  2. Pick a vibe, press RANDOM for a new specimen, or use MUTATE for a nearby variation.\n"
        "  3. Switch surgeons on. STUTTER alone gets you beat-repeat; add GRANULAR and\n"
        "     CORRUPT for texture; FREEZE for held spectral pads.\n"
        "  4. Ride CHAOS. Left is grid-locked and repeatable, right is no-two-bars-alike.\n"
        "  5. Set RHYTHMIC GRAVITY (snap + pull + swing) so the damage lands musically.\n"
        "  6. Drag the history bar at the bottom to rewind to any moment in the last minute.\n"
        "  7. Save what you like with MENU > Save As, or FREEZE / SAVE for a quick preset.\n"
        "\n"
        "THE GUI, TOP TO BOTTOM\n"
        "  HEADER      Plugin name, preset menu, A / B compare, MENU and the gear (options).\n"
        "  LEVEL LED   Far left of the header. Dark grey is silence; it brightens towards\n"
        "              white as the output approaches 0 dBFS and latches RED while you are\n"
        "              over and clipping. If it is red, pull OUT TRIM down.\n"
        "  MONITOR     The specimen buffer as a heart monitor. Teal is the audio, coloured\n"
        "              bands are slices currently held by a surgeon, red ticks are\n"
        "              transients the analyser found.\n"
        "  CHAOS PULSE A read-out of scheduler unpredictability. It beats faster and more\n"
        "              erratically as Chaos opens up.\n"
        "  MASTER ROW  The one-axis controls that shape everything.\n"
        "  I/O ROW     Buffer length, input/output trim, SCAR texture controls, and meter.\n"
        "  SURGEONS    Six identical strips. Icon, on/off, MIX, PROB and three parameters\n"
        "              that mean something different per surgeon, plus a feedback route.\n"
        "  MOD MATRIX  Two LFOs, two macros, and four source -> destination slots.\n"
        "  HISTORY     Sixty seconds of sound settings, recorded twice a second.\n"
        "  SPECIMEN    The green text beside the mod matrix is the plugin's own internal\n"
        "  FEED        traffic - MIDI in, slice scheduling, transport, parameter moves -\n"
        "              scrolling as it happens. It is deliberately low contrast and is\n"
        "              there to be glanced at, not read. It scrolls only while something\n"
        "              is actually happening, so a still feed means a still plugin, which\n"
        "              is useful in itself when you are chasing a dead signal path.\n"
        "  WINDOW SIZE Drag the corner at the bottom right to make the whole window\n"
        "              smaller or larger (50% to 150%). The size is saved with your\n"
        "              session. On a small screen it opens shrunk to fit.\n"
        "\n"
        "MASTER CONTROLS\n"
        "  CHAOS         One automatable axis, order to chaos. Low: grid-locked and\n"
        "                repeatable. Middle: jittered lengths, skipped triggers, occasional\n"
        "                wrong slices. High: randomised positions, open feedback, no grid.\n"
        "  GRAVITY       How hard slice positions are dragged back onto the grid.\n"
        "  SNAP          The grid itself: 1/16, 1/32, 1/8 triplet, 1/8 dotted, or Free.\n"
        "  SWING         Pushes odd steps late (or early, below centre).\n"
        "  TRIGGER       Global surgeon activity - how often anything fires at all.\n"
        "  REINJECT      How much butchered output is written back into the specimen.\n"
        "  ANALYSIS      Chance a slice is chosen by analysis (brightest, loudest, most\n"
        "                tonal, nearest transient) instead of by the grid.\n"
        "  DRY / WET     Wet mix. The dry signal ducks under surgeon activity.\n"
        "  SOURCE        Live, Sample A, Sample B, or a morph between A and B.\n"
        "  MORPH A/B     The morph position, when Source is set to Morph.\n"
        "  BUFFER        Specimen length: 4, 8 or 16 bars.\n"
        "  IN / OUT TRIM Gain into and out of the plugin, in dB.\n"
        "  SCAR          Optional post-rack texture effect. DRIVE pushes into a bounded soft\n"
        "                clipper with extra transient edge; MIX blends it with the clean output.\n"
        "  MIDI MODE     Suppresses the auto-scheduler. Surgeons only fire from MIDI.\n"
        "  PANIC FREEZE  Freezes the buffer and holds it on the Freeze surgeon.\n"
        "  DECAY         Arms a slow drive of chaos and corruption to maximum over DECAY T\n"
        "                seconds. Good for endings.\n"
        "  SIDECHAIN     SC to Main: transients on the sidechain trigger glitches on the\n"
        "                main signal. Main to B: main transients trigger surgeons reading\n"
        "                Sample B. SC AMT sets how often.\n"
        "\n"
        "THE SIX SURGEONS\n"
        "  1 STUTTER  (scalpel)       LEN / RATCHET / DRIFT\n"
        "      Beat-repeat with ratcheting subdivisions and micro pitch-drift per repeat.\n"
        "  2 GRANULAR (syringe)       SIZE / DENSITY / SPRAY\n"
        "      A grain cloud pulled from the buffer. Position spread scales with Chaos.\n"
        "  3 REVERSE  (forceps)       LEN / BLOOM / TAIL\n"
        "      A reversed slice with an amplitude bloom that sucks inward, plus a tail.\n"
        "  4 CORRUPT  (bone saw)      BITS / RATE / MODE\n"
        "      Bitcrush and sample-rate reduction. MODE morphs dropouts to digital clicks\n"
        "      to tape wow and azimuth error.\n"
        "  5 REORDER  (bandage)       SLICES / PERM / REPEAT\n"
        "      Slices a region and permutes it: retrograde, palindrome, Fibonacci,\n"
        "      Euclidean. REPEAT replays the permutation.\n"
        "  6 FREEZE   (defibrillator) OFFSET / LENGTH / BLUR\n"
        "      Phase-vocoder spectral freeze. BLUR randomises bin phase.\n"
        "  Each strip also has MIX, PROB (chance it acts) and ROUTE IN (feed another\n"
        "  surgeon's output back through the specimen for feedback stacks).\n"
        "\n"
        "MOD MATRIX\n"
        "  Four slots. Sources: LFO 1, LFO 2 (each with rate and shape), Env Follow,\n"
        "  Macro 1, Macro 2, Random Walk. Destinations: Chaos, Gravity Pull, Swing,\n"
        "  Trigger Rate, Reinject, Morph, Stutter Drift, Granular Density, Granular Spray,\n"
        "  Corrupt Bits, Corrupt Rate, Freeze Blur. Depth is bipolar - centre is off.\n"
        "\n"
        "LOADING SAMPLES\n"
        "  Sample A and Sample B are the two slots the SOURCE control can read instead of\n"
        "  the live input. Load them either way:\n"
        "    - MENU > Load Sample A / B, or\n"
        "    - drag an audio file straight onto the plug-in window. Drop it on the LEFT\n"
        "      half for slot A, the RIGHT half for slot B. The window shows you which\n"
        "      slot you are about to hit before you let go.\n"
        "  Accepts wav, aif, aiff, flac, mp3 and ogg. MENU > Clear Sample A / B empties a\n"
        "  slot again.\n"
        "\n"
        "MOUSE AND KEYBOARD\n"
        "  Drag              Vertical drag changes a knob or slider.\n"
        "  Shift + drag      Coarse.\n"
        "  Ctrl / Cmd + drag Ultra-fine.\n"
        "  Double-click      Reset that control to its default.\n"
        "  Right-click       Menu on ANY control: reset, type an exact value, MIDI learn,\n"
        "                    clear a MIDI mapping, or lock/unlock it for RANDOM and MUTATE.\n"
        "                    The same menu can clear every exploration lock.\n"
        "  Hover             Tool tips, if they are switched on in Options.\n"
        "\n"
        "MIDI\n"
        "  Notes C3 to F3 trigger surgeons 1 to 6.\n"
        "  CC20 Macro 1, CC21 Macro 2, CC22 Chaos, CC23 Trigger Rate by default. Any of\n"
        "  those can be re-mapped, and any control can be learned, via right-click.\n"
        "  MIDI mappings are saved with the host session. User presets contain sound controls only.\n"
        "\n"
        "MENU\n"
        "  Save              Overwrite the preset file you last saved or opened.\n"
        "  Save As...        Write the current state to a .vsxpreset file.\n"
        "  Open...           Load a .vsxpreset file.\n"
        "  Export Specimen   Write the current specimen buffer as 16, 24 or 32-bit PCM WAV.\n"
        "                    The success message confirms name, location, length and quality.\n"
        "  Open Preset Folder\n"
        "                    Opens the folder Vivisect reads user presets from, in your\n"
        "                    file browser. Drop presets you have been sent in there.\n"
        "  Show Last Saved Preset\n"
        "                    Reveals the file you last saved or opened.\n"
        "  Options...        Tool tips on or off, and audio / MIDI device setup when\n"
        "                    running standalone.\n"
        "  Help              This page.\n"
        "\n"
        "BUTTONS\n"
        "  RANDOM        New sound. Later presses reset unlocked creative controls first;\n"
        "                locks, I/O, routing, MIDI mode and hidden FLATLINE controls stay put.\n"
        "  MUTATE        Nearby variation controlled by MUTATE AMT. Zero makes no changes.\n"
        "  RESET         Every parameter back to its default, including locked parameters.\n"
        "  A / B         Compare sound snapshots. Tool tips, MIDI mappings and locks stay put.\n"
        "  FREEZE / SAVE Saves sound controls as a timestamped user preset.\n"
        "\n"
        "PRESETS\n"
        "  Six factory characters: Clean Specimen, Late 90s Bristol, Berlin Basement,\n"
        "  Warp 1995, Modern Hyperpop, SOPHIE-adjacent. User presets appear below them\n"
        "  marked with an asterisk. Presets restore sound settings and leave your current\n"
        "  MIDI mappings, tool tip preference and exploration locks alone.\n"
        "\n"
        "TROUBLESHOOTING - THE PLUG-IN DOES NOT APPEAR IN MY DAW\n"
        "  Vivisect installs as a VST3 plus a standalone application. If your host cannot\n"
        "  see it, install it by hand:\n"
        "\n"
        "  Windows   Copy the whole Vivisect.vst3 FOLDER (it is a folder, not a file) to\n"
        "            C:\\Program Files\\Common Files\\VST3\\\n"
        "  macOS     Copy Vivisect.vst3 to /Library/Audio/Plug-Ins/VST3/ for every user,\n"
        "            or ~/Library/Audio/Plug-Ins/VST3/ for just you.\n"
        "  Linux     Copy Vivisect.vst3 to ~/.vst3/ or /usr/lib/vst3/.\n"
        "\n"
        "  CLAP      Copy Vivisect.clap to the CLAP folder for your system:\n"
        "            Windows  C:\\Program Files\\Common Files\\CLAP\\\n"
        "            macOS    /Library/Audio/Plug-Ins/CLAP/ or ~/Library/Audio/Plug-Ins/CLAP/\n"
        "            Linux    ~/.clap/ or /usr/lib/clap/\n"
        "  STANDALONE  Vivisect.exe (or Vivisect on Linux) runs on its own - no DAW\n"
        "            needed. Put it anywhere, e.g. C:\\Program Files\\Vivisect\\, and\n"
        "            pick your audio and MIDI devices under MENU > Options.\n"
        "\n"
        "  Then rescan. Most hosts have a rescan or reset-and-rescan button in their\n"
        "  plug-in preferences; some only rescan on start-up, so restart the DAW if in\n"
        "  doubt. If it still does not show, check that your host is 64-bit and that it is\n"
        "  actually scanning the folder you copied to - many DAWs let you add extra scan\n"
        "  paths, and a custom path set years ago is the usual culprit.\n"
        "\n"
        "  To uninstall, delete the Vivisect.vst3 folder and Vivisect.clap you copied in,\n"
        "  and delete the standalone application. To remove settings and presets too, delete\n"
        "  the Vivisect folder described under PRESET FILES below. Nothing is written to\n"
        "  the registry, so there is nothing else to clean up.\n"
        "\n"
        "PRESET FILES - WHERE THEY LIVE\n"
        "  User presets are .vsxpreset files. They are read from, and written to:\n"
        "\n"
        "    " + userPresetDirPath() + "\n"
        "\n"
        "  The quickest way to get there is MENU > Open Preset Folder, which opens that\n"
        "  exact folder in your file browser.\n"
        "\n"
        "  If a preset you were given does not show up in the preset menu:\n"
        "    - Make sure it is in the folder above, not in a sub-folder. Vivisect does not\n"
        "      search sub-folders.\n"
        "    - Make sure the extension really is .vsxpreset. If your system hides known\n"
        "      extensions, a file called pad.vsxpreset may actually be pad.vsxpreset.txt.\n"
        "      Switch extensions back on and rename it.\n"
        "    - The menu is read when the editor opens and after you save. Close and reopen\n"
        "      the plug-in window to force a rescan.\n"
        "    - If the folder does not exist yet, save any preset once and Vivisect will\n"
        "      create it for you.\n"
        "  User presets are listed below the factory ones, marked with an asterisk.\n"
        "\n"
        "LICENSE\n"
        "  Vivisect " + v + "\n"
        "  Copyright (c) 2026 Specimen Audio. All rights reserved.\n"
        "\n"
        "  This software is licensed, not sold. You are granted a non-exclusive licence to\n"
        "  install and use Vivisect on machines you own or control, and to use audio you\n"
        "  make with it for any purpose, commercial or otherwise, with no further payment\n"
        "  and no attribution required. You may not redistribute, resell, rent or\n"
        "  sub-licence the plug-in itself, and you may not reverse engineer, decompile or\n"
        "  disassemble it except where that right cannot be excluded by law.\n"
        "\n"
        "  Vivisect is built on the JUCE framework, which is licensed separately by Raw\n"
        "  Material Software Limited. See https://juce.com/juce-8-licence for its terms.\n"
        "\n"
        "  THE SOFTWARE IS PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
        "  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY AND\n"
        "  FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR\n"
        "  ANY CLAIM, DAMAGES OR OTHER LIABILITY ARISING FROM, OUT OF OR IN CONNECTION\n"
        "  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
        "\n"
        "LINKS AND SUPPORT\n"
        "  Homepage      " + juce::String (product::home) + "\n"
        "  Source        " + juce::String (product::github) + "\n"
        "  Support       " + juce::String (product::support) + "\n"
        "\n"
        "  The buttons at the bottom of this page open those for you. When you write in\n"
        "  about a problem, say which DAW and operating system you are on and quote the\n"
        "  version number below - it saves a round trip.\n"
        "\n"
        "VERSION\n"
        "  Vivisect " + v + "   -   Specimen Audio\n", false);
}
void HelpPanel::resized()
{
    OverlayPanel::resized();
    auto card = contentArea().reduced (metric::pad, metric::pad);
    card.removeFromTop (metric::grid + 32);

    // Link row sits on the 8px grid at the foot of the card.
    auto links = card.removeFromBottom (metric::buttonH);
    card.removeFromBottom (metric::grid);
    body.setBounds (card);

    const int gap = metric::grid;
    const int w = juce::jmax (88, (links.getWidth() - 4 * gap) / 5);
    for (auto* b : { &homeBtn, &gitBtn, &mailBtn, &presetsBtn, &debugBtn })
    {
        b->setBounds (links.removeFromLeft (w));
        links.removeFromLeft (gap);
    }
}

// ---------------------------------------------------------------------------
OptionsPanel::OptionsPanel() : OverlayPanel ("OPTIONS")
{
    tooltips.setColour (juce::ToggleButton::textColourId, col::text);
    tooltips.setColour (juce::ToggleButton::tickColourId, col::accent);
    tooltips.onClick = [this] { if (onTooltipsChanged) onTooltipsChanged (tooltips.getToggleState()); };
    addAndMakeVisible (tooltips);

    audioBtn.onClick = [this] { if (onAudioMidiSettings) onAudioMidiSettings(); };
    addAndMakeVisible (audioBtn);
}
void OptionsPanel::setAudioSettingsAvailable (bool a)
{
    audioAvailable = a;
    audioBtn.setEnabled (a);
    repaint();
}
void OptionsPanel::paint (juce::Graphics& g)
{
    OverlayPanel::paint (g);

    auto card = contentArea().reduced (metric::pad, metric::pad);
    card.removeFromTop (metric::grid + 32);

    auto r = card;
    r.removeFromTop (metric::grid);
    draw::sectionHeader (g, "INTERFACE", r.removeFromTop (16));
    r.removeFromTop (metric::grid + metric::buttonH + metric::grid);

    draw::sectionHeader (g, "AUDIO / MIDI", r.removeFromTop (16));
    r.removeFromTop (metric::grid + metric::buttonH + metric::gridFine);
    draw::label (g, audioAvailable
                     ? "Choose the audio device, sample rate, buffer size and MIDI inputs."
                     : "Running as a plug-in - the host owns the audio and MIDI devices.",
                 r.removeFromTop (14), juce::Justification::centredLeft, col::textMuted, 11.f);
}
void OptionsPanel::resized()
{
    OverlayPanel::resized();
    auto card = contentArea().reduced (metric::pad, metric::pad);
    card.removeFromTop (metric::grid + 32);

    auto r = card;
    r.removeFromTop (metric::grid);
    r.removeFromTop (16);
    r.removeFromTop (metric::grid);
    tooltips.setBounds (r.removeFromTop (metric::buttonH).withWidth (260));
    r.removeFromTop (metric::grid);
    r.removeFromTop (16);
    r.removeFromTop (metric::grid);
    audioBtn.setBounds (r.removeFromTop (metric::buttonH).withWidth (280));
}

// ===========================================================================
//  OutputLed
// ===========================================================================
OutputLed::OutputLed (std::function<float (int)> peakForChannel)
    : getPeak (std::move (peakForChannel))
{
    setInterceptsMouseClicks (false, false);
    setTooltip ("Output level. Grey is silence, white is 0 dBFS, red means you are "
                "over and the output is clipping - pull OUT TRIM down.");
    startTimerHz (30);
}

void OutputLed::timerCallback()
{
    const float peak = juce::jmax (getPeak (0), getPeak (1));
    const float db   = juce::Decibels::gainToDecibels (peak, -100.f);

    // -inf..0 dB mapped over the bottom 60 dB, so the LED spends its travel
    // where the music is rather than crawling out of the noise floor.
    const float target = juce::jlimit (0.f, 1.f, (db + 60.f) / 60.f);
    glow = draw::easeTowards (glow, target, 1000.f / 30.f);

    // Latch red while over, then let it fall away once the signal is back under.
    over = db >= 0.f ? 1.f : draw::easeTowards (over, 0.f, 1000.f / 30.f);

    repaint();
}

void OutputLed::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.f);
    const float d = juce::jmin (b.getWidth(), b.getHeight());
    auto lamp = b.withSizeKeepingCentre (d, d);

    // Unlit body: the same dark grey the spec asks for at -inf.
    const juce::Colour unlit { 0xff3a3f48 };
    const juce::Colour lit   = over > 0.001f ? col::clip.brighter (over * 0.35f)
                                             : juce::Colours::white;
    const float amount = over > 0.001f ? juce::jmax (glow, 0.55f * over) : glow;

    // Halo first, so a hot signal bleeds slightly into the header.
    if (amount > 0.02f)
    {
        g.setColour (lit.withAlpha (amount * 0.22f));
        g.fillEllipse (lamp.expanded (d * 0.45f));
    }

    g.setColour (unlit.interpolatedWith (lit, amount));
    g.fillEllipse (lamp);

    // 1px bevel, consistent with every other control.
    g.setColour (col::edge);
    g.drawEllipse (lamp, 1.f);
}

// ===========================================================================
//  DataStream
// ===========================================================================
DataStream::DataStream (Drain drain, std::function<juce::String (int)> paramNamer)
    : drainEvents (std::move (drain)), nameParam (std::move (paramNamer))
{
    setInterceptsMouseClicks (false, false);
    setTooltip ("Specimen feed: the plugin's own internal traffic - MIDI in, slice "
                "scheduling, transport and parameter moves - as it happens.");
    lastTick = juce::Time::getMillisecondCounter();
    startTimerHz (30);
}

juce::String describeTelemetry (const TelemetryEvent& e,
                                const std::function<juce::String (int)>& paramNamer,
                                juce::uint64 index)
{
    // Every line carries a rolling index so a stalled feed is obvious at a
    // glance, and so the debug log has something to correlate against.
    const auto idx = juce::String ((int) (index & 0xffff)).paddedLeft ('0', 4);
    const auto f2 = [] (float v, int dp = 2) { return juce::String (v, dp); };
    const auto pname = [&paramNamer] (int i)
    {
        return paramNamer != nullptr ? paramNamer (i) : juce::String (i);
    };

    switch (e.kind)
    {
        case TelemetryEvent::Kind::midiNote:
            return idx + "  NOTE " + juce::MidiMessage::getMidiNoteName (e.a, true, true, 3)
                       + " v" + juce::String (e.b)
                       + (e.c < 6 ? "  -> SURG " + juce::String (e.c + 1) : juce::String ("  unmapped"));

        case TelemetryEvent::Kind::midiCC:
            return idx + "  CC" + juce::String (e.a).paddedLeft ('0', 3) + " = " + juce::String (e.b)
                       + (e.c == 255 ? juce::String ("  unmapped") : "  -> " + pname ((int) e.c));

        case TelemetryEvent::Kind::surgeonFire:
            return idx + "  FIRE S" + juce::String (e.a + 1) + "  pos " + f2 (e.x, 3)
                       + "  len " + f2 (e.y, 1) + "ms";

        case TelemetryEvent::Kind::sliceChoice:
            return idx + "  SLICE S" + juce::String (e.a + 1) + "  mode " + juce::String (e.b)
                       + "  q " + f2 (e.x, 3);

        case TelemetryEvent::Kind::paramChange:
            return idx + "  SET " + pname ((int) e.a) + " = " + f2 (e.x, 3);

        case TelemetryEvent::Kind::clock:
            return idx + "  CLK " + f2 (e.x, 1) + " bpm  ppq " + f2 (e.y, 2)
                       + (e.a ? "  PLAY" : "  STOP");

        case TelemetryEvent::Kind::level:
            return idx + "  LVL L " + f2 (e.x, 3) + "  R " + f2 (e.y, 3);

        case TelemetryEvent::Kind::buffer:
            return idx + "  BUF " + juce::String (e.a) + " bars  fill " + f2 (e.x, 2)
                       + (e.b ? "  FROZEN" : "");

        case TelemetryEvent::Kind::preset:
            return idx + "  PRESET " + juce::String (e.a);

        case TelemetryEvent::Kind::none:
        default:
            return idx + "  ...";
    }
}

juce::String DataStream::format (const TelemetryEvent& e)
{
    return describeTelemetry (e, nameParam, counter++);
}

void DataStream::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const float dt = (float) (now - lastTick);
    lastTick = now;

    // The telemetry ring has a single read cursor, so only one consumer may
    // drain it. While the debug window is up it owns the feed; this one would
    // otherwise steal half the events and both views would show gaps.
    if (paused)
        return;

    TelemetryEvent evs[64];
    const int got = drainEvents != nullptr ? drainEvents (evs, juce::numElementsInArray (evs)) : 0;

    if (got > 0)
    {
        // The transport ticks every block, which would drown everything else.
        // Keep it only when it is the only thing happening.
        bool anyInteresting = false;
        for (int i = 0; i < got; ++i)
            if (evs[i].kind != TelemetryEvent::Kind::clock)
                { anyInteresting = true; break; }

        for (int i = 0; i < got; ++i)
        {
            if (anyInteresting && evs[i].kind == TelemetryEvent::Kind::clock)
                continue;
            pending.push_back (format (evs[i]));
        }

        // A long burst must not build a backlog the feed then crawls through
        // for seconds after the event - drop the oldest instead.
        while ((int) pending.size() > kMaxPending)
            pending.pop_front();
    }

    if (pending.empty())
    {
        // No new data: the conveyor parks. Settling the part-scrolled row to a
        // stop rather than freezing mid-travel keeps the last line aligned.
        if (scroll > 0.f)
        {
            scroll = juce::jmax (0.f, scroll - dt / 260.f);
            repaint();
        }
        return;
    }

    // Speed rises with the backlog, so heavy traffic reads as a faster feed
    // without ever letting the queue run away.
    const float msPerRow = juce::jlimit (26.f, 110.f, 110.f - 2.4f * (float) pending.size());
    scroll += dt / msPerRow;

    while (scroll >= 1.f && ! pending.empty())
    {
        scroll -= 1.f;
        lines.push_back (pending.front());
        pending.pop_front();
        while ((int) lines.size() > kMaxLines)
            lines.pop_front();
    }
    if (pending.empty())
        scroll = juce::jmin (scroll, 1.f);

    repaint();
}

void DataStream::paint (juce::Graphics& g)
{
    if (lines.empty())
        return;

    const float lineH = 11.f;
    const int   rows  = juce::jmax (1, (int) (getHeight() / lineH));
    const float baseAlpha = 0.55f;

    g.setFont (font::mono (9.f));

    // Light green, per the house spec — close to col::ok but pushed brighter so
    // it still reads at 9px against the panel.
    const juce::Colour green = col::ok.brighter (0.25f);

    const int first = juce::jmax (0, (int) lines.size() - rows);
    float y = (float) getHeight() - lineH - scroll * lineH;

    for (int i = (int) lines.size() - 1; i >= first; --i)
    {
        const int row = (int) lines.size() - 1 - i;          // 0 = newest, at the bottom

        // Fade the two rows at each end to nothing, so the feed has no hard
        // edges and never competes with the controls around it.
        float edge = 1.f;
        if (row == 0)                 edge = 0.10f;
        else if (row == 1)            edge = 0.45f;
        else if (row == rows - 2)     edge = 0.45f;
        else if (row >= rows - 1)     edge = 0.10f;

        // Newest line is a touch hotter, so the eye finds the live end.
        const float heat = row == 2 ? 1.25f : 1.f;

        g.setColour (green.withAlpha (juce::jlimit (0.f, 1.f, baseAlpha * edge * heat)));
        g.drawText (lines[(size_t) i], 0, juce::roundToInt (y), getWidth(), (int) lineH,
                    juce::Justification::centredLeft, false);

        y -= lineH;
        if (y < -lineH)
            break;
    }
}

// ===========================================================================
//  DebugPanel
// ===========================================================================
namespace
{
    void styleReadout (juce::TextEditor& t)
    {
        t.setMultiLine (true, false);
        t.setReadOnly (true);
        t.setScrollbarsShown (true);
        t.setCaretVisible (false);
        t.setPopupMenuEnabled (true);          // so the user can copy it out
        t.setFont (font::mono (11.f));
        t.setColour (juce::TextEditor::backgroundColourId, col::bg);
        t.setColour (juce::TextEditor::outlineColourId, col::edge);
        t.setColour (juce::TextEditor::focusedOutlineColourId, col::edge);
        t.setColour (juce::TextEditor::textColourId, col::ok.brighter (0.2f));
    }
}

DebugPanel::DebugPanel() : OverlayPanel ("VIVISECT - DEBUG")
{
    styleReadout (feed);
    styleReadout (state);
    state.setColour (juce::TextEditor::textColourId, col::text);
    addAndMakeVisible (feed);
    addAndMakeVisible (state);

    crashLog.setColour (juce::ToggleButton::textColourId, col::text);
    crashLog.setColour (juce::ToggleButton::tickColourId, col::warn);
    crashLog.setTooltip ("Starts a log file now and keeps writing to it. If Vivisect crashes, "
                         "the crash is appended to the end. Off every time the plugin loads.");
    crashLog.onClick = [this]
    {
        if (onCrashLogToggled) onCrashLogToggled (crashLog.getToggleState());
    };
    addAndMakeVisible (crashLog);

    exportBtn.setTooltip ("Writes a troubleshooting report: a light self-diagnostic plus every "
                          "current setting, your audio and MIDI configuration, the version and "
                          "licence, and what it can see of the DAW.");
    exportBtn.onClick = [this] { if (onExportTroubleshooting) onExportTroubleshooting(); };
    addAndMakeVisible (exportBtn);

    logsBtn.setTooltip ("Opens the folder holding the crash logs and troubleshooting reports.");
    logsBtn.onClick = [this] { if (onOpenLogFolder) onOpenLogFolder(); };
    addAndMakeVisible (logsBtn);

    hardBtn.setColour (juce::TextButton::textColourOffId, col::clip);
    hardBtn.setTooltip ("DESTRUCTIVE. Puts every setting back to its factory default, clears MIDI "
                        "mappings, forgets loaded samples and A/B, and deletes cached files. Your "
                        "saved presets are NOT touched. Use this when the plugin will not behave.");
    hardBtn.onClick = [this]
    {
        // Destructive and not undoable, so it asks first.
        auto opts = juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("Reset all settings to default?")
                        .withMessage ("This clears every setting, every MIDI mapping, both sample "
                                      "slots, the A/B snapshots and the cache folder.\n\n"
                                      "Your saved presets will NOT be deleted.\n\n"
                                      "This cannot be undone.")
                        .withButton ("Reset everything")
                        .withButton ("Cancel");

        juce::AlertWindow::showAsync (opts, [this] (int result)
        {
            if (result == 1 && onHardReset)
                onHardReset();
        });
    };
    addAndMakeVisible (hardBtn);

    startTimerHz (12);
}

void DebugPanel::setCrashLogState (bool on, const juce::String& path)
{
    crashLog.setToggleState (on, juce::dontSendNotification);
    logPath = path;
    repaint();
}

void DebugPanel::timerCallback()
{
    if (! isVisible())
        return;

    // The debug view shows EVERYTHING, including the transport ticks the
    // decorative feed suppresses - that is the point of a debug view.
    TelemetryEvent evs[64];
    const int got = drain != nullptr ? drain (evs, juce::numElementsInArray (evs)) : 0;
    if (got > 0)
    {
        juce::String add;
        for (int i = 0; i < got; ++i)
            add << describeTelemetry (evs[i], nameParam, counter++) << "\n";

        feed.moveCaretToEnd();
        feed.insertTextAtCaret (add);

        // Keep the buffer bounded; an hour of traffic would otherwise pin the
        // whole session in memory. Drop the OLDEST half and resume from a line
        // boundary so the first surviving entry is not a fragment.
        if (feed.getTotalNumChars() > 60000)
        {
            auto kept = feed.getText().getLastCharacters (30000);
            const auto nl = kept.indexOfChar ('\n');
            if (nl >= 0)
                kept = kept.substring (nl + 1);
            feed.setText ("[... older entries trimmed ...]\n" + kept, false);
        }
        feed.moveCaretToEnd();
    }

    // The state dump is expensive to build, so it refreshes about twice a second.
    if (++stateTick >= 6)
    {
        stateTick = 0;
        if (rawState != nullptr)
        {
            const auto pos = state.getCaretPosition();
            state.setText (rawState(), false);
            state.setCaretPosition (pos);
        }
    }
}

void DebugPanel::paint (juce::Graphics& g)
{
    OverlayPanel::paint (g);

    auto card = contentArea().reduced (metric::pad, metric::pad);
    card.removeFromTop (metric::grid + 32);

    auto r = card;
    draw::sectionHeader (g, "LIVE INTERNALS", r.removeFromTop (16), col::ok);
    r.removeFromTop (metric::grid);

    const int feedH = juce::jmax (120, (r.getHeight() - 200) / 2);
    r.removeFromTop (feedH);
    r.removeFromTop (metric::grid);
    draw::sectionHeader (g, "CURRENT STATE", r.removeFromTop (16), col::accent2);
    r.removeFromTop (metric::grid);
    r.removeFromTop (feedH);
    r.removeFromTop (metric::grid);

    draw::sectionHeader (g, "SUPPORT FILES", r.removeFromTop (16), col::warn);
    r.removeFromTop (metric::grid);

    auto textRow = r.removeFromTop (metric::buttonH);
    textRow.removeFromLeft (0);
    r.removeFromTop (metric::gridFine);

    // Explain both files, and that a hard crash needs both. include.md is
    // explicit that the user has to be told what each one is for.
    juce::StringArray lines;
    lines.add ("TROUBLESHOOTING FILE  a snapshot: self-diagnostic, every setting, audio and MIDI");
    lines.add ("                      config, version, licence and DAW. Safe to send any time.");
    lines.add ("CRASH LOG             a running record. Switch it on, reproduce the crash, then send");
    lines.add ("                      it. A copy of the troubleshooting report is written at the top.");
    lines.add ("If Vivisect is hard crashing, send BOTH files to " + juce::String (product::support));
    lines.add ("with a description of what you were doing. Off by default on every load.");

    if (logPath.isNotEmpty())
        lines.add ("Current log: " + logPath);

    for (const auto& l : lines)
    {
        draw::label (g, l, r.removeFromTop (13), juce::Justification::centredLeft,
                     l.startsWith ("Current log") ? col::ok : col::textMuted, 10.f);
        r.removeFromTop (1);
    }

    // Live misconfiguration hints, per include.md's "obvious signals of
    // misconfiguration".
    if (warnings != nullptr)
    {
        const auto w = warnings();
        if (! w.isEmpty())
        {
            r.removeFromTop (metric::gridFine);
            draw::label (g, "POSSIBLE MISCONFIGURATION", r.removeFromTop (13),
                         juce::Justification::centredLeft, col::warn, 10.f);
            for (const auto& line : w)
            {
                draw::label (g, "  * " + line, r.removeFromTop (13),
                             juce::Justification::centredLeft, col::warn, 10.f);
                r.removeFromTop (1);
            }
        }
    }
}

void DebugPanel::resized()
{
    OverlayPanel::resized();
    auto card = contentArea().reduced (metric::pad, metric::pad);
    card.removeFromTop (metric::grid + 32);

    auto r = card;
    r.removeFromTop (16);                       // LIVE INTERNALS header
    r.removeFromTop (metric::grid);

    const int feedH = juce::jmax (120, (r.getHeight() - 200) / 2);
    feed.setBounds (r.removeFromTop (feedH));
    r.removeFromTop (metric::grid);
    r.removeFromTop (16);                       // CURRENT STATE header
    r.removeFromTop (metric::grid);
    state.setBounds (r.removeFromTop (feedH));
    r.removeFromTop (metric::grid);
    r.removeFromTop (16);                       // SUPPORT FILES header
    r.removeFromTop (metric::grid);

    auto row = r.removeFromTop (metric::buttonH);
    crashLog.setBounds (row.removeFromLeft (220));
    row.removeFromLeft (metric::grid);
    exportBtn.setBounds (row.removeFromLeft (240));
    row.removeFromLeft (metric::grid);
    logsBtn.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (metric::grid);
    hardBtn.setBounds (row.removeFromRight (juce::jmin (280, row.getWidth())));
}

// ===========================================================================
//  SecretPanel — FLATLINE
// ===========================================================================
SecretPanel::SecretPanel (ParamHost& host)
    : onBtn ("FLATLINE", host, id::flatOn),
      tone  (host, id::flatTone,  "TONE",  VsxSlider::Size::small),
      bleed (host, id::flatBleed, "BLEED", VsxSlider::Size::small),
      mix   (host, id::flatMix,   "MIX",   VsxSlider::Size::small)
{
    // Every tip says plainly that this is the hidden effect, per include.md.
    onBtn.setTooltip ("SECRET: FLATLINE. You found the hidden effect. A tuned resonator "
                      "that rings at the note you set - the sound the monitor makes when "
                      "the specimen stops.");
    tone.setTooltip  ("SECRET FEATURE - FLATLINE TONE. The pitch the resonator rings at, "
                      "55 Hz to 1760 Hz.");
    bleed.setTooltip ("SECRET FEATURE - FLATLINE BLEED. How long the tone sustains. High "
                      "values ring for a long time but stop short of self-oscillating.");
    mix.setTooltip   ("SECRET FEATURE - FLATLINE MIX. How much of the ring is blended into "
                      "the output.");
    closeBtn.setTooltip ("SECRET FEATURE - close the hidden effect. Click the notch in the top-left corner "
                         "to open it again.");

    addAndMakeVisible (onBtn);
    addAndMakeVisible (tone);
    addAndMakeVisible (bleed);
    addAndMakeVisible (mix);
    addAndMakeVisible (closeBtn);

    closeBtn.onClick = [this] { if (onClose) onClose(); };

    onA = std::make_unique<BA> (host.params(), id::flatOn, onBtn);
    att.add (new SA (host.params(), id::flatTone,  tone));
    att.add (new SA (host.params(), id::flatBleed, bleed));
    att.add (new SA (host.params(), id::flatMix,   mix));

    lastTick = juce::Time::getMillisecondCounter();
}

void SecretPanel::reveal (bool shouldShow)
{
    want = shouldShow;
    if (shouldShow)
        setVisible (true);
    lastTick = juce::Time::getMillisecondCounter();
    startTimerHz (60);
}

void SecretPanel::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const float dt = (float) (now - lastTick);
    lastTick = now;

    open = draw::easeTowards (open, want ? 1.f : 0.f, dt);

    // Tell the owner on EVERY frame, including the last one. Sampling this
    // from outside would drop the final step and leave the panel misplaced.
    if (onAnimate)
        onAnimate();

    if (! want && open <= 0.001f)
    {
        setVisible (false);
        stopTimer();
    }
    else if (want && open >= 0.999f)
    {
        stopTimer();
    }
    repaint();
}

void SecretPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    draw::panelShadow (g, b, (float) metric::radiusPanel);
    g.setColour (col::panel);
    g.fillRoundedRectangle (b, (float) metric::radiusPanel);

    // Teal edge rather than the primary accent: this is not part of the normal
    // signal path and should not read as one of the main sections.
    g.setColour (col::accent2.withAlpha (0.85f));
    g.drawRoundedRectangle (b.reduced (0.5f), (float) metric::radiusPanel, 1.f);

    auto r = getLocalBounds().reduced (metric::grid);
    draw::sectionHeader (g, "FLATLINE", r.removeFromTop (14), col::accent2);
    draw::label (g, "hidden", { r.getX() + 96, r.getY() - 14, 80, 12 },
                 juce::Justification::centredLeft, col::textMuted, 9.f);
}

void SecretPanel::resized()
{
    auto r = getLocalBounds().reduced (metric::grid);
    r.removeFromTop (14 + metric::gridFine);

    auto row = r.removeFromTop (metric::buttonH);
    onBtn.setBounds (row.removeFromLeft (110));
    closeBtn.setBounds (row.removeFromRight (72));

    r.removeFromTop (metric::gridFine);
    auto knobs = r.removeFromTop (juce::jmax (52, r.getHeight()));
    const int w = juce::jmax (48, knobs.getWidth() / 3);
    tone.setBounds  (knobs.removeFromLeft (w));
    bleed.setBounds (knobs.removeFromLeft (w));
    mix.setBounds   (knobs.removeFromLeft (w));
}

// ===========================================================================
//  GearButton
// ===========================================================================
GearButton::GearButton() : juce::Button ("OPTIONS")
{
    gear = gearIconPath();
    setTooltip ("Options: tool tips, audio and MIDI device setup.");
}
void GearButton::paintButton (juce::Graphics& g, bool hl, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (2.f);
    if (hl || down)
    {
        g.setColour (col::hovered (col::panel));
        g.fillRoundedRectangle (b, (float) metric::radiusSmall);
    }
    auto sq = b.reduced (3.f).withSizeKeepingCentre (juce::jmin (b.getWidth(), b.getHeight()) - 6.f,
                                                     juce::jmin (b.getWidth(), b.getHeight()) - 6.f);
    juce::Path p = gear;
    p.applyTransform (juce::AffineTransform::fromTargetPoints (
        0.f, 0.f,   sq.getX(),     sq.getY(),
        100.f, 0.f, sq.getRight(), sq.getY(),
        0.f, 100.f, sq.getX(),     sq.getBottom()));
    g.setColour (down ? col::accent : (hl ? col::text : col::textMuted));
    g.fillPath (p);
}

} // namespace vsx
