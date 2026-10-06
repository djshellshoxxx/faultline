#include "Theme.h"
#include <algorithm>
#include <cmath>

namespace vsx::draw
{

// ---------------------------------------------------------------------------
//  11px uppercase label, letter-spacing +0.08em
// ---------------------------------------------------------------------------
void label (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
            juce::Justification just, juce::Colour c, float height)
{
    if (text.isEmpty() || area.isEmpty()) return;

    const auto f = font::ui (height);
    const auto up = text.toUpperCase();
    const float tracking = height * 0.08f;

    juce::GlyphArrangement ga;
    ga.addLineOfText (f, up, 0.f, 0.f);
    for (int i = 1; i < ga.getNumGlyphs(); ++i)
        ga.moveRangeOfGlyphs (i, 1, tracking * (float) i, 0.f);

    const auto gb = ga.getBoundingBox (0, -1, true);
    const auto a  = area.toFloat();

    float x = a.getX();
    if (just.testFlags (juce::Justification::horizontallyCentred)) x = a.getCentreX() - gb.getWidth() * 0.5f;
    else if (just.testFlags (juce::Justification::right))          x = a.getRight() - gb.getWidth();

    const float y = a.getCentreY() + f.getAscent() * 0.5f - f.getDescent() * 0.25f;

    ga.moveRangeOfGlyphs (0, -1, x, y);
    g.setColour (c);
    ga.draw (g);
}

// ---------------------------------------------------------------------------
void sectionHeader (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                    juce::Colour accent)
{
    auto r = area;
    auto bar = r.removeFromLeft (2).withSizeKeepingCentre (2, 12);
    g.setColour (accent);
    g.fillRect (bar);
    r.removeFromLeft (metric::gridFine + 2);
    label (g, text, r, juce::Justification::centredLeft, col::text, 11.f);
}

// ---------------------------------------------------------------------------
void separator (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (col::edge);
    g.fillRect (area.getX(), area.getCentreY(), area.getWidth(), 1);
}

// ---------------------------------------------------------------------------
void signatureNotch (juce::Graphics& g, juce::Colour accent)
{
    // 12px long, 45deg, 2px wide, tucked into the top-left corner.
    const float d = 12.f / juce::MathConstants<float>::sqrt2;   // 8.49px per axis
    g.setColour (accent);
    g.drawLine (4.f, 4.f + d, 4.f + d, 4.f, 2.f);
}

// ---------------------------------------------------------------------------
void panelShadow (juce::Graphics& g, juce::Rectangle<float> r, float radius)
{
    juce::Path p;
    p.addRoundedRectangle (r, radius);
    juce::DropShadow (col::shadow, 8, { 0, 2 }).drawForPath (g, p);
}

// ---------------------------------------------------------------------------
juce::Colour meterColour (float level01)
{
    const float v = juce::jlimit (0.f, 1.f, level01);
    if (v < 0.55f) return col::accent2.interpolatedWith (col::accent, v / 0.55f);
    if (v < 0.80f) return col::accent .interpolatedWith (col::warn,  (v - 0.55f) / 0.25f);
    return             col::warn     .interpolatedWith (col::clip,  (v - 0.80f) / 0.20f);
}

// ---------------------------------------------------------------------------
float easeTowards (float current, float target, float deltaMs)
{
    if (deltaMs <= 0.f) return current;
    // exponential approach; tau chosen so ~95% of the distance is covered in 80ms
    const float tau = metric::animMs / 3.f;
    const float k = 1.f - std::exp (-deltaMs / tau);
    const float out = current + (target - current) * k;
    return std::abs (target - out) < 1.0e-5f ? target : out;
}

} // namespace vsx::draw

namespace vsx
{
juce::File userDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile (product::name);
}
juce::File userPresetDir()      { return userDataDir().getChildFile ("Presets"); }
juce::File userLogDir()         { return userDataDir().getChildFile ("Logs"); }
juce::File userCacheDir()       { return userDataDir().getChildFile ("Cache"); }
juce::String userPresetDirPath() { return userPresetDir().getFullPathName(); }

void openHomePage() { juce::URL (product::home).launchInDefaultBrowser(); }
void openGitHub()   { juce::URL (product::github).launchInDefaultBrowser(); }

void openSupportMail (const juce::String& subject)
{
    juce::URL ("mailto:" + juce::String (product::support))
        .withParameter ("subject", subject)
        .launchInDefaultBrowser();
}
} // namespace vsx

