#pragma once
#include <JuceHeader.h>
#include <cmath>

// ============================================================================
//  VIVISECT — VISUAL IDENTITY (see theme.md)
//
//  The neutrals, metrics and control shapes in here are the shared house style
//  and must not drift per-plugin. A plugin may re-tint ONE accent for its own
//  identity; Vivisect keeps the house orange/teal pair and takes its identity
//  from the corner notch, the surgeon iconography and the monitor.
// ============================================================================
namespace vsx
{

// ---------------------------------------------------------------------------
//  Colour palette
// ---------------------------------------------------------------------------
namespace col
{
    const juce::Colour bg        { 0xff0e1116 };   // background base
    const juce::Colour panel     { 0xff171b22 };   // panel surface
    const juce::Colour edge      { 0xff2a303a };   // panel edge / bevel / separators
    const juce::Colour accent    { 0xffe8532a };   // primary  — active values, peaks, selection
    const juce::Colour accent2   { 0xff4fb6c4 };   // secondary — mod / LFO / links
    const juce::Colour text      { 0xffe6e8ec };   // labels, values
    const juce::Colour textMuted { 0xff8a929e };   // units, hints, inactive
    const juce::Colour ok        { 0xff7bc96f };   // success / signal-on
    const juce::Colour warn      { 0xfff2c14e };   // warning / near clip
    const juce::Colour clip      { 0xffe5484d };   // clip

    const juce::Colour knobTop   { 0xff232833 };   // knob body gradient, top
    const juce::Colour knobBot   { 0xff14181f };   // knob body gradient, bottom
    const juce::Colour shadow    { 0x8c000000 };   // rgba(0,0,0,0.55)

    // panel surface under a hovered control (spec: hover brightens ~8%)
    inline juce::Colour hovered (juce::Colour c) { return c.brighter (0.08f); }

    // Per-surgeon identity hues. Categorical, drawn from the palette family so
    // they sit at the same saturation / value as the two accents.
    inline juce::Colour surgeon (int i)
    {
        static const juce::Colour a[] {
            juce::Colour (0xffe8532a),   // 1 STUTTER  — primary accent
            juce::Colour (0xff4fb6c4),   // 2 GRANULAR — secondary accent
            juce::Colour (0xff7bc96f),   // 3 REVERSE
            juce::Colour (0xfff2c14e),   // 4 CORRUPT
            juce::Colour (0xff9a8cf0),   // 5 REORDER
            juce::Colour (0xffe06c9f) }; // 6 FREEZE
        return a[juce::jlimit (0, 5, i)];
    }
}

// ---------------------------------------------------------------------------
//  Product identity — the strings the help page, the about box and the
//  troubleshooting export all quote. Kept in one place so a rebrand or a
//  change of support address is a one-line edit.
// ---------------------------------------------------------------------------
namespace product
{
    inline constexpr const char* name    = "Vivisect";
    inline constexpr const char* vendor  = "Specimen Audio";
    inline constexpr const char* home    = "https://specimenaudio.com/vivisect";
    inline constexpr const char* github  = "https://github.com/specimenaudio/vivisect";
    inline constexpr const char* support = "support@specimenaudio.com";
}

// The one true location for user presets, settings and logs. Both the
// processor and the help page ask for it here so they can never disagree.
juce::File userDataDir();
juce::File userPresetDir();
juce::String userPresetDirPath();
juce::File userLogDir();        // crash logs + troubleshooting reports
juce::File userCacheDir();      // scratch the hard reset is allowed to wipe

// Open a URL, or a mail composer for the support address.
void openHomePage();
void openGitHub();
void openSupportMail (const juce::String& subject);

// ---------------------------------------------------------------------------
//  Metrics — 8px base grid, 4px for fine detail
// ---------------------------------------------------------------------------
namespace metric
{
    constexpr int grid        = 8;
    constexpr int gridFine    = 4;
    constexpr int pad         = 16;   // min window edge -> control
    constexpr int header      = 32;   // plugin header strip
    constexpr int knobSmall   = 36;
    constexpr int knob        = 48;
    constexpr int knobLarge   = 64;
    constexpr int buttonH     = 28;
    constexpr int radiusWindow= 6;
    constexpr int radiusPanel = 4;
    constexpr int radiusSmall = 2;
    constexpr int tooltipDelay= 400;  // ms hover before a tooltip appears
    constexpr float animMs    = 80.f; // value changes ease out over 80ms
}

// ---------------------------------------------------------------------------
//  Typography — Inter (fallback Space Grotesk, then system sans) for UI,
//  JetBrains Mono (fallback IBM Plex Mono, then Consolas) for numerics.
// ---------------------------------------------------------------------------
namespace font
{
    inline juce::String pick (const juce::StringArray& wanted, const juce::String& fallback)
    {
        static const juce::StringArray installed = juce::Font::findAllTypefaceNames();
        for (const auto& w : wanted)
            if (installed.contains (w, true))
                return w;
        return fallback;
    }
    inline const juce::String& uiFamily()
    {
        static const juce::String f = pick ({ "Inter", "Space Grotesk", "Segoe UI Variable Text",
                                              "Segoe UI" }, juce::Font::getDefaultSansSerifFontName());
        return f;
    }
    inline const juce::String& monoFamily()
    {
        static const juce::String f = pick ({ "JetBrains Mono", "IBM Plex Mono", "Cascadia Mono",
                                              "Consolas" }, juce::Font::getDefaultMonospacedFontName());
        return f;
    }

    // UI text. Weight 500 for labels, 600 (bold here) for section headers.
    inline juce::Font ui (float h, bool semibold = false)
    {
        return juce::Font (juce::FontOptions (uiFamily(), h, semibold ? juce::Font::bold : juce::Font::plain));
    }
    // Numeric readouts, tabular.
    inline juce::Font mono (float h, bool semibold = false)
    {
        return juce::Font (juce::FontOptions (monoFamily(), h, semibold ? juce::Font::bold : juce::Font::plain));
    }
    // 11px uppercase, letter-spacing +0.08em -> applied by the caller via drawLabel().
    inline juce::Font label() { return ui (11.f); }
}

// ---------------------------------------------------------------------------
//  Shared drawing helpers
// ---------------------------------------------------------------------------
namespace draw
{
    // 11px uppercase label with +0.08em tracking.
    void label (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                juce::Justification just = juce::Justification::centred,
                juce::Colour c = col::textMuted, float height = 11.f);

    // Section header: uppercase text with a 2px x 12px accent bar to its left.
    void sectionHeader (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                        juce::Colour accent = col::accent);

    // 1px separator. Sections are separated by lines, never nested boxes.
    void separator (juce::Graphics&, juce::Rectangle<int> area);

    // Identity marker: 2px accent diagonal notch, 12px long, top-left corner.
    void signatureNotch (juce::Graphics&, juce::Colour accent = col::accent);

    // Soft drop shadow under a rounded rect (8px blur, y+2).
    void panelShadow (juce::Graphics&, juce::Rectangle<float> r, float radius);

    // Meter gradient colour for a 0..1 level: teal -> orange -> yellow -> red.
    juce::Colour meterColour (float level01);

    // 80ms ease-out interpolation towards a target, given a frame delta in ms.
    float easeTowards (float current, float target, float deltaMs);
}

} // namespace vsx
