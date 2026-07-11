#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ============================================================================
// CUE RACK visual language: dark warm charcoal panels, cream controls,
// tiny uppercase monospaced labels, vintage modular/test-equipment feel.
// ============================================================================

namespace cue
{
    // Two-theme palette. The FOUNDATION colours (backdrop, plates, wells,
    // outlines, ink) are swapped at runtime by applyTheme(); the ACCENT ramp
    // (the CUE core hues + cool counterpoints) is theme-invariant so each
    // module keeps its identity in both looks. `dark` is the contrasting mark
    // drawn on accent-filled controls (knob pointers, toggle dots) and stays
    // dark in both themes because accents are mid-to-light tones. `chip` is the
    // light surface used behind a toggle's ON dot.
    enum class Theme { dark, light };

    namespace colours
    {
        // ---- foundation (mutable; set by applyTheme) ------------------------
        inline juce::Colour bg       { 0xff171412 };   // behind panels
        inline juce::Colour panel    { 0xff242019 };   // module plate
        inline juce::Colour panelHi  { 0xff2b261e };   // raised details
        inline juce::Colour slot     { 0xff14100d };   // fader slots, wells
        inline juce::Colour line     { 0xff4a4238 };   // outlines
        inline juce::Colour cream    { 0xfff2e7da };   // controls / text (ink)
        inline juce::Colour creamDim { 0xff9c9082 };   // secondary text
        inline juce::Colour dark     { 0xff171009 };   // marks on accent chips
        inline juce::Colour chip     { 0xfff2e7da };   // toggle ON surface

        // ---- CUE core ramp: deep signature orange -> warm white (invariant)
        inline const juce::Colour cueOrange { 0xffd9542f };  // signature deep orange
        inline const juce::Colour cueEmber  { 0xffe2603a };
        inline const juce::Colour cueMid    { 0xffe88a55 };
        inline const juce::Colour cuePeach  { 0xffefa98c };
        inline const juce::Colour cueSand   { 0xfff3b27e };
        inline const juce::Colour cueBlush  { 0xfff0bfa0 };
        inline const juce::Colour cuePale   { 0xfff5c9a8 };
        inline const juce::Colour cueWhite  { 0xfff6e7d8 };  // warm white

        // Cool counterpoints for the space/time/width modules — the orb's
        // environment colours, complementing the warm core ramp
        inline const juce::Colour cueTeal   { 0xff7fc9b4 };  // reverb / atmosphere
        inline const juce::Colour cueViolet { 0xffa48fe0 };  // delay / echoes
        inline const juce::Colour cueSky    { 0xff85bdd8 };  // imager / stereo field
        inline const juce::Colour cueIce    { 0xffcfe3ea };  // halftime / frozen time
        inline const juce::Colour cueTube   { 0xffe8a04c };  // amp / tube glow
    }

    // Current theme + the swap. applyTheme() reassigns the foundation colours;
    // callers must then repaint (and refresh any widget that cached a colour —
    // see CueLookAndFeel::refreshColours / ModulePanel::refreshColours).
    inline Theme currentTheme = Theme::dark;
    inline bool  isLight() { return currentTheme == Theme::light; }

    inline void applyTheme (Theme t, bool warpActive = false, bool halftimeActive = false)
    {
        currentTheme = t;

        if (t == Theme::light)
        {
            colours::bg       = juce::Colour (0xffcfc4b2);   // warm taupe backdrop
            colours::panel    = juce::Colour (0xffe9e0d1);   // cream plate
            colours::panelHi  = juce::Colour (0xfff2ebde);   // raised
            colours::slot     = juce::Colour (0xffd0c4ad);   // recessed wells
            colours::line     = juce::Colour (0xffb3a68f);   // outlines
            colours::cream    = juce::Colour (0xff2b2318);   // ink (text/controls)
            colours::creamDim = juce::Colour (0xff746757);   // secondary ink
            colours::dark     = juce::Colour (0xff171009);   // marks on accent chips
            colours::chip     = juce::Colour (0xfff5eee2);   // toggle ON surface
        }
        else
        {
            colours::bg       = juce::Colour (0xff171412);
            colours::panel    = juce::Colour (0xff242019);
            colours::panelHi  = juce::Colour (0xff2b261e);
            colours::slot     = juce::Colour (0xff14100d);
            colours::line     = juce::Colour (0xff4a4238);
            colours::cream    = juce::Colour (0xfff2e7da);
            colours::creamDim = juce::Colour (0xff9c9082);
            colours::dark     = juce::Colour (0xff171009);
            colours::chip     = juce::Colour (0xfff2e7da);
        }

        // Drastic dynamic tints so the FX rack recolours with the rest of the UI
        // when WARP or HALFTIME is active. Text/mark colours are left untouched.
        const bool isL = (t == Theme::light);
        if (warpActive)
        {
            // Extreme, electric violet/grape — matches the main UI's full-tilt warp read
            colours::bg      = isL ? juce::Colour (0xffc9a6f5) : juce::Colour (0xff2a0d5c);
            colours::panel   = isL ? juce::Colour (0xffd9bdfa) : juce::Colour (0xff3d1785);
            colours::panelHi = isL ? juce::Colour (0xffe8d4fd) : juce::Colour (0xff4e1fa8);
            colours::slot    = isL ? juce::Colour (0xffab7fee) : juce::Colour (0xff1f0847);
            colours::line    = isL ? juce::Colour (0xff7d3fe0) : juce::Colour (0xff7d45d6);
        }
        else if (halftimeActive)
        {
            // Extreme, electric glacial ice/cyan — matches the main UI's full-tilt halftime read
            colours::bg      = isL ? juce::Colour (0xff9fdff0) : juce::Colour (0xff042838);
            colours::panel   = isL ? juce::Colour (0xffbaebf7) : juce::Colour (0xff073f56);
            colours::panelHi = isL ? juce::Colour (0xffd2f5fc) : juce::Colour (0xff0a5170);
            colours::slot    = isL ? juce::Colour (0xff72c8e2) : juce::Colour (0xff031e2c);
            colours::line    = isL ? juce::Colour (0xff28a0c6) : juce::Colour (0xff14657f);
        }
    }

    // In the light theme a raw pale accent (cueWhite/cueIce/…) barely reads as
    // 20pt title text on a light plate; tint it toward ink while keeping hue.
    inline juce::Colour themedTitleColour (juce::Colour accent)
    {
        return isLight() ? accent.interpolatedWith (colours::cream, 0.45f) : accent;
    }

    // Monospaced UI font (JUCE 7/8 compatible)
    juce::Font monoFont (float height, bool bold = false);

    class CueLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        CueLookAndFeel();

        // Re-apply the default colours this LnF caches, after applyTheme().
        void refreshColours();

        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider&) override;

        void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;

        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

        juce::Font getSliderPopupFont (juce::Slider&) override;
        int getSliderPopupPlacement (juce::Slider&) override;
        void drawBubble (juce::Graphics&, juce::BubbleComponent&,
                         const juce::Point<float>& tip,
                         const juce::Rectangle<float>& body) override;

        void drawButtonBackground (juce::Graphics&, juce::Button&,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&,
                             bool shouldDrawButtonAsHighlighted,
                             bool shouldDrawButtonAsDown) override;
    };
}
