#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "VisualizerOrb.h"
#include "CueFxRack/FxRackStrip.h"

#include <BinaryData.h>

#include <cmath>
#include <functional>
#include <map>

namespace cue
{
namespace
{
// One continuous chassis: content spans the full design width inside a
// slim 10px margin (the CUERACK kMargin), no side rails.
constexpr int editorWidth = 1266;
constexpr int editorHeight = 734; // the CORE design space (chassis + keyboard);
                                  // the FX rack band below lays out in real
                                  // window pixels and re-flows CUERACK-style.
                                  // (Was 884 — the old right utility rail is
                                  // dissolved, stems dock into the header, and
                                  // the transport is one condensed band.)

// Minimum height reserved for the FX rack band under the scaled core:
// one row of panels at the rack's minimum usable height (240) + toolbar.
constexpr int fxRackMinBand = 290;
constexpr int fxRackMarginX = 10;
constexpr int fxRackGapY    = 4;
constexpr float minEditorScale = 0.85f;
constexpr float maxEditorScale = 1.5f;
constexpr float defaultWaveformVerticalScale = 0.75f;
constexpr int headerRefreshHz = 20;
constexpr int waveformRefreshHz = 30;
constexpr int transportRefreshHz = 20;
// Unified knob size across the whole UI. Both names kept so call sites read
// in context, but they intentionally hold the same value.
constexpr int effectKnobDiameter = 66;
constexpr int preciseMiniKnobDragSensitivity = 3200;
constexpr int maxZoomedScrollDragSensitivity = 13200;
constexpr float zoomResponseMidpoint = 0.5f;
constexpr float zoomMappedMidpoint = 0.75f;

// --- Animation frame rate ---------------------------------------------------
// Drive UI animations at the display's native refresh rate: 120 Hz on
// ProMotion / high-refresh panels, 60 Hz otherwise, for maximum smoothness.
//
// JUCE only populates Displays::verticalFrequencyHz on Windows/Linux, so on
// macOS we can't read the rate directly. Instead we measure it from the
// editor's vblank callback (backed by CVDisplayLink, which ticks at the real
// display rate). Defaults to 60 Hz until the first vblanks have been timed.
inline int g_animationRefreshHz = 60;

inline int animationFrameRateHz() noexcept { return g_animationRefreshHz; }

// Fold one observed vblank interval (seconds) into the measured refresh rate.
// Called from the editor's VBlankAttachment.
inline void observeVBlankInterval (double dtSeconds)
{
    if (dtSeconds <= 0.0001 || dtSeconds > 0.5) // ignore stalls / first frame
        return;

    static double emaDt = 1.0 / 60.0;
    emaDt += (dtSeconds - emaDt) * 0.1; // smooth out jitter
    g_animationRefreshHz = (1.0 / emaDt) >= 90.0 ? 120 : 60;
}

// Re-express an exponential smoothing factor (authored at baseHz fps) so the
// animation covers the same ground in the same wall-clock time at any rate.
inline float frameRateLerp (float lerpAtBase, int hz, int baseHz = 60)
{
    if (hz <= 0 || hz == baseHz)
        return lerpAtBase;
    return 1.0f - std::pow (1.0f - lerpAtBase, (float) baseHz / (float) hz);
}

// Re-express a per-frame linear step/decay (authored at baseHz fps) for the
// current frame rate, preserving its wall-clock duration.
inline float frameRateStep (float stepAtBase, int hz, int baseHz = 60)
{
    return hz > 0 ? stepAtBase * ((float) baseHz / (float) hz) : stepAtBase;
}

// Hardware-flat corners: CUERACK plates sit at 2px, buttons at 3px, and the
// outer faceplate keeps a barely-there 4px so the two products read as one
// family of test equipment.
constexpr float largeCorner = 4.0f;
constexpr float mediumCorner = 2.0f;
constexpr float smallCorner = 2.0f;

// ============================================================================
// CUE RACK shared visual language: dark warm charcoal plates, cream controls,
// tiny uppercase monospaced labels, vintage modular/test-equipment feel.
// Foundation tokens mirror CUERACK's CueLookAndFeel.h palette exactly so the
// two products stay visually in lockstep. Like the rack, the FOUNDATION
// colours are swapped at runtime by applyTheme(); the ACCENT ramp is
// theme-invariant so controls keep their identity in both looks.
// ============================================================================
enum class Theme { dark, light };
inline Theme currentTheme = Theme::dark;
inline bool  isLight() { return currentTheme == Theme::light; }

inline juce::Colour shellDark      { 0xff171412 };  // bg — behind panels
inline juce::Colour railDark       { 0xff242019 };  // module plate
inline juce::Colour panelDark      { 0xff242019 };  // module plate
inline juce::Colour panelHi        { 0xff2b261e };  // raised details
inline juce::Colour panelInnerDark { 0xff1b1712 };  // recessed screen frames
inline juce::Colour blackPanel     { 0xff14100d };  // slots, wells, screens
inline juce::Colour borderDark     { 0xff2e2921 };  // subtle dark outline
inline juce::Colour borderMid      { 0xff4a4238 };  // outlines (CUERACK line)
inline juce::Colour borderLight    { 0xff5a5044 };  // brighter outlines
const juce::Colour darkInk (0xff171009);            // marks on accent-filled controls (both themes)
const juce::Colour lightCueOrange (0xfff3b27e);     // CUERACK cueSand: light end of the CUE orange ramp
inline juce::Colour textPrimary    { 0xfff2e7da };  // cream ink
inline juce::Colour textMuted      { 0xff9c9082 };  // creamDim
inline juce::Colour textFaint      { 0xff7a7065 };
inline juce::Colour metalGrey      { 0xff4a4238 };
inline juce::Colour glassText      { 0xfff2e7da };  // legacy names, same cream ramp
inline juce::Colour glassTextMuted { 0xff9c9082 };
inline juce::Colour controlGlassTop    { 0xff2b261e };
inline juce::Colour controlGlassBottom { 0xff242019 };

inline void applyTheme (Theme t, bool warpActive = false, bool halftimeActive = false)
{
    currentTheme = t;

    if (t == Theme::light)
    {
        // CUERACK light foundation: warm taupe backdrop, cream plates, ink text.
        shellDark      = juce::Colour (0xffcfc4b2);
        railDark       = juce::Colour (0xffe9e0d1);
        panelDark      = juce::Colour (0xffe9e0d1);
        panelHi        = juce::Colour (0xfff2ebde);
        panelInnerDark = juce::Colour (0xffded2bd);
        blackPanel     = juce::Colour (0xffd0c4ad);
        borderDark     = juce::Colour (0xffc4b79f);
        borderMid      = juce::Colour (0xffb3a68f);
        borderLight    = juce::Colour (0xff9a8d76);
        textPrimary    = juce::Colour (0xff2b2318);
        textMuted      = juce::Colour (0xff746757);
        textFaint      = juce::Colour (0xff8a7d6b);
        metalGrey      = juce::Colour (0xffb3a68f);
        glassText      = juce::Colour (0xff2b2318);
        glassTextMuted = juce::Colour (0xff746757);
        controlGlassTop    = juce::Colour (0xfff2ebde);
        controlGlassBottom = juce::Colour (0xffe9e0d1);
    }
    else
    {
        shellDark      = juce::Colour (0xff171412);
        railDark       = juce::Colour (0xff242019);
        panelDark      = juce::Colour (0xff242019);
        panelHi        = juce::Colour (0xff2b261e);
        panelInnerDark = juce::Colour (0xff1b1712);
        blackPanel     = juce::Colour (0xff14100d);
        borderDark     = juce::Colour (0xff2e2921);
        borderMid      = juce::Colour (0xff4a4238);
        borderLight    = juce::Colour (0xff5a5044);
        textPrimary    = juce::Colour (0xfff2e7da);
        textMuted      = juce::Colour (0xff9c9082);
        textFaint      = juce::Colour (0xff7a7065);
        metalGrey      = juce::Colour (0xff4a4238);
        glassText      = juce::Colour (0xfff2e7da);
        glassTextMuted = juce::Colour (0xff9c9082);
        controlGlassTop    = juce::Colour (0xff2b261e);
        controlGlassBottom = juce::Colour (0xff242019);
    }

    // Apply drastic dynamic tints when WARP or HALFTIME is active:
    const bool isL = (t == Theme::light);
    if (warpActive)
    {
        // Extreme, electric violet/grape theme — full-tilt saturation so the mode read is impossible to miss
        shellDark      = isL ? juce::Colour (0xffc9a6f5) : juce::Colour (0xff2a0d5c);
        railDark       = isL ? juce::Colour (0xffd9bdfa) : juce::Colour (0xff3d1785);
        panelDark      = isL ? juce::Colour (0xffd9bdfa) : juce::Colour (0xff3d1785);
        panelHi        = isL ? juce::Colour (0xffe8d4fd) : juce::Colour (0xff4e1fa8);
        panelInnerDark = isL ? juce::Colour (0xffbf99f2) : juce::Colour (0xff2e0f6b);
        blackPanel     = isL ? juce::Colour (0xffab7fee) : juce::Colour (0xff1f0847);
        borderDark     = isL ? juce::Colour (0xff9560e6) : juce::Colour (0xff5a2bb0);
        borderMid      = isL ? juce::Colour (0xff7d3fe0) : juce::Colour (0xff7d45d6);
        borderLight    = isL ? juce::Colour (0xff6822d6) : juce::Colour (0xff9d63f5);
    }
    else if (halftimeActive)
    {
        // Extreme, electric glacial ice/cyan theme — full-tilt saturation so the mode read is impossible to miss
        shellDark      = isL ? juce::Colour (0xff9fdff0) : juce::Colour (0xff042838);
        railDark       = isL ? juce::Colour (0xffbaebf7) : juce::Colour (0xff073f56);
        panelDark      = isL ? juce::Colour (0xffbaebf7) : juce::Colour (0xff073f56);
        panelHi        = isL ? juce::Colour (0xffd2f5fc) : juce::Colour (0xff0a5170);
        panelInnerDark = isL ? juce::Colour (0xff8fd6ea) : juce::Colour (0xff053244);
        blackPanel     = isL ? juce::Colour (0xff72c8e2) : juce::Colour (0xff031e2c);
        borderDark     = isL ? juce::Colour (0xff54b8d6) : juce::Colour (0xff0b607f);
        borderMid      = isL ? juce::Colour (0xff28a0c6) : juce::Colour (0xff1187ab);
        borderLight    = isL ? juce::Colour (0xff1188b0) : juce::Colour (0xff1fb0d6);
    }
}

inline bool isHalfTimeActive = false;
inline bool isWarpModeActive = false;
inline juce::Colour getOrange()
{
    // CUERACK accent ramp: signature deep orange, with the rack's cool
    // counterpoints for the alternate modes (ice = frozen time, violet = warp).
    if (isWarpModeActive)
        return juce::Colour (0xffa48fe0);

    return isHalfTimeActive ? juce::Colour (0xffcfe3ea) : juce::Colour (0xffd9542f);
}
#define accentOrange cue::getOrange()

// In the light theme a raw pale accent (ice especially) barely reads as text
// on a light plate; tint it toward ink while keeping hue (CUERACK's
// themedTitleColour). Use for accent-coloured TEXT; fills/LEDs stay raw.
inline juce::Colour themedTitleColour (juce::Colour accent)
{
    return isLight() ? accent.interpolatedWith (textPrimary, 0.45f) : accent;
}

// UI preferences (theme choice) live in a small per-user settings file, not in
// plugin state — so the look follows the user, and the processor is untouched.
// The editor owns the PropertiesFile because it uses a JUCE Timer internally
// for delayed saves and must be destroyed before the MessageManager shuts down.
inline juce::PropertiesFile::Options uiSettingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "CUESAMPLER";
    options.filenameSuffix      = ".settings";
    options.folderName          = "CUE/CUESAMPLER";
    options.osxLibrarySubFolder = "Application Support";
    options.millisecondsBeforeSaving = 500;
    return options;
}

// The CUE brand typeface (Syne — same family as the logo wordmark). Loaded once
// from binary data and kept alive for the app's lifetime so it stays registered
// with JUCE's font system, allowing the name-based lookups below to find it (and,
// on macOS, to instantiate the variable font's bold weight to match the logo).
juce::Typeface::Ptr brandTypeface()
{
    static juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor (
        CueSamplerBinaryData::Synewght_ttf, (size_t) CueSamplerBinaryData::Synewght_ttfSize);
    return typeface;
}

const juce::String& brandFontName()
{
    static const juce::String name = []
    {
        auto tf = brandTypeface();
        return tf != nullptr ? tf->getName() : juce::String ("Helvetica");
    }();
    return name;
}

juce::Font brandFont (float height)
{
    return { juce::FontOptions (brandFontName(), height, juce::Font::plain) };
}

juce::Font heavyFont (float height)
{
    return { juce::FontOptions (brandFontName(), height, juce::Font::bold) };
}

juce::Font monoFont (float height)
{
    return { juce::FontOptions ("Menlo", height, juce::Font::bold) };
}

juce::Font cousineFont (float height)
{
    // One mono family across the brand (CUERACK uses the system monospace,
    // which is Menlo on macOS) — the old Cousine readouts join it.
    return { juce::FontOptions ("Menlo", height, juce::Font::bold) };
}

void fillRectGradient (juce::Graphics& g, juce::Rectangle<float> area,
                       juce::Colour top, juce::Colour bottom)
{
    juce::ColourGradient gradient (top, area.getCentreX(), area.getY(),
                                   bottom, area.getCentreX(), area.getBottom(), false);
    gradient.addColour (0.5, top.interpolatedWith (bottom, 0.45f));
    g.setGradientFill (gradient);
    g.fillRect (area);
}

// --- Module plates -------------------------------------------------------------
// Panels are flat warm-charcoal plates with hairline outlines and corner
// screws — the CUERACK vintage test-equipment language. The old smoked-glass
// backdrop (pre-blurred background slices) is gone with the orange faceplate.

// CUERACK plate: flat warm charcoal fill with a hairline outline. The
// signature keeps the component parameter the old glass renderer needed so
// every call site stays untouched.
void fillGlassRounded (juce::Graphics& g, const juce::Component&,
                       juce::Rectangle<float> area, float cornerSize)
{
    g.setColour (panelDark);
    g.fillRoundedRectangle (area.reduced (0.5f), cornerSize);
    g.setColour (borderMid);
    g.drawRoundedRectangle (area.reduced (0.5f), cornerSize, 1.0f);
}

// Path variant for non-rectangular plates (side rails).
void fillGlassPath (juce::Graphics& g, const juce::Path& path, juce::Rectangle<float>)
{
    g.setColour (panelDark);
    g.fillPath (path);
    g.setColour (borderMid);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

float getEffectiveZoomLevel (float zoomControlValue) noexcept
{
    const auto clampedZoom = juce::jlimit (0.0f, 1.0f, zoomControlValue);

    if (clampedZoom <= zoomResponseMidpoint)
        return juce::jmap (clampedZoom, 0.0f, zoomResponseMidpoint, 0.0f, zoomMappedMidpoint);

    return juce::jmap (clampedZoom, zoomResponseMidpoint, 1.0f, zoomMappedMidpoint, 1.0f);
}

juce::String formatSampleTime (double seconds)
{
    const auto clampedSeconds = juce::jmax (0.0, seconds);
    const auto totalCentiseconds = (int) std::round (clampedSeconds * 100.0);
    const auto minutes = totalCentiseconds / 6000;
    const auto secs = (totalCentiseconds / 100) % 60;
    const auto centiseconds = totalCentiseconds % 100;

    return juce::String::formatted ("%02d:%02d:%02d", minutes, secs, centiseconds);
}

juce::String formatDetectedTempo (double bpm)
{
    if (bpm <= 0.0)
        return "--.-";

    return juce::String (bpm, bpm >= 100.0 ? 1 : 2);
}



int getScrollDragSensitivity (float zoomControlValue) noexcept
{
    const float effectiveZoom = getEffectiveZoomLevel (zoomControlValue);
    const float zoomWeight = effectiveZoom * effectiveZoom;

    return preciseMiniKnobDragSensitivity
         + (int) std::round (zoomWeight * (float) (maxZoomedScrollDragSensitivity - preciseMiniKnobDragSensitivity));
}

void configureTextLabel (juce::Label& label, const juce::String& text, float height,
                         juce::Colour colour, juce::Justification justification)
{
    // Control labels are whispered in uppercase monospace, per the CUERACK
    // visual language (titles keep the Syne brand face elsewhere). Mono runs
    // wider than Syne, so scale the requested height down to keep layouts.
    label.setText (text.toUpperCase(), juce::dontSendNotification);
    label.setFont (monoFont (height * 0.78f));
    label.setJustificationType (justification);
    label.setColour (juce::Label::textColourId, colour);
    label.setInterceptsMouseClicks (false, false);
}

void configureButton (juce::TextButton& button, const juce::String& text, juce::Colour textColour)
{
    button.setButtonText (text);
    button.setClickingTogglesState (false);
    button.setColour (juce::TextButton::buttonColourId, panelDark);
    button.setColour (juce::TextButton::buttonOnColourId, panelHi);
    button.setColour (juce::TextButton::textColourOffId, textColour);
    button.setColour (juce::TextButton::textColourOnId, textColour);
    button.setWantsKeyboardFocus (false);
    button.setMouseClickGrabsKeyboardFocus (false);
}

// Defaults to dark-on-glass; pass a light colour for hints inside dark screens.
void drawHelperText (juce::Graphics& g, const juce::String& text,
                     juce::Rectangle<int> bounds,
                     juce::Justification justification = juce::Justification::centred,
                     float fontHeight = 9.6f,
                     juce::Colour colour = glassTextMuted.withAlpha (0.95f))
{
    // Taglines: tiny uppercase monospace, like CUERACK's panel captions.
    g.setColour (colour);
    g.setFont (monoFont (fontHeight * 0.9f));
    g.drawFittedText (text.toUpperCase(), bounds, justification, 2);
}

juce::TextLayout createTooltipLayout (const juce::String& text, juce::Colour colour)
{
    juce::AttributedString attributed;
    attributed.setWordWrap (juce::AttributedString::byWord);
    attributed.setJustification (juce::Justification::centredLeft);
    attributed.append (text, monoFont (12.5f), colour);

    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (attributed, 520.0f);
    return layout;
}

bool shouldRunRealtimeUi (const juce::Component& component) noexcept
{
    return component.isShowing();
}

juce::String getCueStyle (const juce::Component& component)
{
    return component.getProperties().getWithDefault ("cueStyle", {}).toString();
}

juce::Colour getCueAccent (const juce::Component& component, juce::Colour fallback)
{
    const auto rawColour = component.getProperties().getWithDefault ("cueAccent", (int) fallback.getARGB());
    return juce::Colour ((juce::uint32) (int) rawColour);
}

juce::String getCueIcon (const juce::Component& component)
{
    return component.getProperties().getWithDefault ("cueIcon", {}).toString();
}

// CUERACK corner screw: a dim-cream hairline ring with a slash. Same
// signature as the old panel hole so all call sites keep their positions.
void drawPanelHole (juce::Graphics& g, juce::Point<float> centre, float diameter)
{
    const float r = diameter * 0.53f;
    g.setColour (glassTextMuted.withAlpha (0.65f));
    g.drawEllipse (centre.x - r, centre.y - r, r * 2.0f, r * 2.0f, 1.0f);
    g.drawLine (centre.x - r * 0.6f, centre.y - r * 0.6f,
                centre.x + r * 0.6f, centre.y + r * 0.6f, 1.0f);
}

// --- CUERACK rotaries ------------------------------------------------------
// The rack's knob language: a dim-cream tick ring around a flat accent-tinted
// body with a dark pointer — vintage test-equipment, no chrome. Angles follow
// the addCentredArc / getPointOnCircumference convention: radians clockwise
// from 12 o'clock.
void drawMetalKnob (juce::Graphics& g, juce::Rectangle<float> bounds,
                    float pos01, float startAngle, float endAngle,
                    juce::Colour accent, float hover)
{
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    const auto centre = bounds.getCentre();
    const auto radius = juce::jmax (0.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f);
    const float valueAngle = juce::jmap (juce::jlimit (0.0f, 1.0f, pos01), startAngle, endAngle);

    // Tick ring: 11 hairline marks across the sweep.
    g.setColour (glassTextMuted.withAlpha (0.8f + 0.2f * hover));
    const int numTicks = 11;
    for (int i = 0; i < numTicks; ++i)
    {
        const auto a  = startAngle + (float) i / (float) (numTicks - 1) * (endAngle - startAngle);
        const auto p1 = centre.getPointOnCircumference (radius - 3.0f, a);
        const auto p2 = centre.getPointOnCircumference (radius, a);
        g.drawLine ({ p1, p2 }, 1.0f);
    }

    // Flat body in the control's accent; hover warms it a touch.
    const float knobR = juce::jmax (0.0f, radius - 6.5f);
    g.setColour (accent.brighter (0.07f * hover));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    // Subtle rim shading.
    g.setColour (darkInk.withAlpha (0.25f));
    g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.2f);

    // Dark or light pointer based on contrast.
    const auto pIn  = centre.getPointOnCircumference (knobR * 0.25f, valueAngle);
    const auto pOut = centre.getPointOnCircumference (knobR * 0.85f, valueAngle);
    const auto brightness = 0.2126f * accent.getFloatRed() + 0.7152f * accent.getFloatGreen() + 0.0722f * accent.getFloatBlue();
    const auto pointerColour = (brightness > 0.5f) ? darkInk : juce::Colours::white;
    g.setColour (pointerColour.withAlpha (0.9f));
    g.drawLine ({ pIn, pOut }, juce::jmax (2.0f, knobR * 0.09f));
}

class SmoothAnimatedSwitchButton final : public juce::TextButton, private juce::Timer
{
public:
    SmoothAnimatedSwitchButton()
        : juce::TextButton()
    {
        currentPosition = getToggleState() ? 1.0f : 0.0f;
        targetPosition = currentPosition;
    }

    ~SmoothAnimatedSwitchButton() override
    {
        stopTimer();
    }

    float getCurrentAnimationPosition() const noexcept { return currentPosition; }
    float getHoverAlpha() const noexcept { return hoverAlpha; }

    void mouseEnter (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter (e);
        ensureAnimating();
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseExit (e);
        ensureAnimating();
    }

    void buttonStateChanged() override
    {
        juce::TextButton::buttonStateChanged();
        
        const float target = getToggleState() ? 1.0f : 0.0f;
        if (std::abs (target - targetPosition) > 0.001f)
        {
            targetPosition = target;
            if (! isShowing())
            {
                currentPosition = target;
                velocity = 0.0f;
            }
            else
            {
                ensureAnimating();
            }
        }
    }

private:
    void ensureAnimating()
    {
        if (! isTimerRunning())
        {
            animHz = animationFrameRateHz();
            startTimerHz (animHz);
        }
    }

    void timerCallback() override
    {
        constexpr float stiffness = 320.0f;
        constexpr float damping = 28.0f;
        const float dt = 1.0f / (float) animHz; // real frame interval → rate-independent physics

        float force = (targetPosition - currentPosition) * stiffness - velocity * damping;
        velocity += force * dt;
        currentPosition += velocity * dt;

        bool positionDone = std::abs (currentPosition - targetPosition) < 0.002f && std::abs (velocity) < 0.02f;
        if (positionDone)
        {
            currentPosition = targetPosition;
            velocity = 0.0f;
        }

        currentPosition = juce::jlimit (-0.08f, 1.08f, currentPosition);

        const float hoverTarget = isMouseOver() ? 1.0f : 0.0f;
        hoverAlpha += (hoverTarget - hoverAlpha) * frameRateLerp (0.18f, animHz);

        bool hoverDone = std::abs (hoverAlpha - hoverTarget) < 0.005f;
        if (hoverDone)
            hoverAlpha = hoverTarget;

        if (positionDone && hoverDone)
        {
            stopTimer();
        }

        repaint();
        if (auto* parent = getParentComponent())
            parent->repaint();
    }

    float currentPosition = 0.0f;
    float targetPosition = 0.0f;
    float velocity = 0.0f;
    float hoverAlpha = 0.0f;
    int   animHz = 60;
};

class SmoothHoverButton : public juce::TextButton, private juce::Timer
{
public:
    SmoothHoverButton() : juce::TextButton() {}
    ~SmoothHoverButton() override { stopTimer(); }

    float getHoverAlpha() const noexcept { return hoverAlpha; }

    void mouseEnter (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter (e);
        ensureAnimating();
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseExit (e);
        ensureAnimating();
    }

private:
    void ensureAnimating()
    {
        if (! isTimerRunning())
        {
            animHz = animationFrameRateHz();
            startTimerHz (animHz);
        }
    }

    void timerCallback() override
    {
        const float target = isMouseOver() ? 1.0f : 0.0f;
        hoverAlpha += (target - hoverAlpha) * frameRateLerp (0.18f, animHz);

        if (std::abs (hoverAlpha - target) < 0.005f)
        {
            hoverAlpha = target;
            stopTimer();
        }

        repaint();
    }

    float hoverAlpha = 0.0f;
    int   animHz = 60;
};

class OptResetSlider : public juce::Slider, private juce::Timer
{
public:
    OptResetSlider() : juce::Slider() {}
    ~OptResetSlider() override { stopTimer(); }

    void captureCurrentValueAsDefault() noexcept
    {
        defaultValue = getValue();
        hasDefaultValue = true;
    }

    float getHoverAlpha() const noexcept { return hoverAlpha; }

    void mouseEnter (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseEnter (e);
        ensureAnimating();
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        juce::Slider::mouseExit (e);
        ensureAnimating();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (hasDefaultValue && event.mods.isAltDown())
        {
            setValue (defaultValue, juce::sendNotificationSync);
            return;
        }

        juce::Slider::mouseDown (event);
    }

private:
    void ensureAnimating()
    {
        if (! isTimerRunning())
        {
            animHz = animationFrameRateHz();
            startTimerHz (animHz);
        }
    }

    void timerCallback() override
    {
        const float target = isMouseOverOrDragging() ? 1.0f : 0.0f;
        hoverAlpha += (target - hoverAlpha) * frameRateLerp (0.18f, animHz);

        if (std::abs (hoverAlpha - target) < 0.005f)
        {
            hoverAlpha = target;
            stopTimer();
        }

        repaint();
    }

    double defaultValue = 0.0;
    bool hasDefaultValue = false;
    float hoverAlpha = 0.0f;
    int   animHz = 60;
};
} // namespace

// --- Tactile keycaps ---------------------------------------------------------
// Flat CUERACK button plates shared by all push buttons: warm charcoal fill,
// hairline outline that brightens to cream on hover, pressed state nudges the
// cap down a pixel and lifts the fill to the raised plate tone. Toggles warm
// the plate toward the accent as they switch on (accentAmount 0..1).
constexpr float keycapTravel = 1.0f;

// Cap at rest is the component bounds minus a bottom sliver reserved for the
// contact shadow (painting is clipped to the component); pressing translates
// it down by keycapTravel.
static juce::Rectangle<float> getKeycapBounds (juce::Rectangle<float> bounds, bool isDown)
{
    return bounds.withTrimmedBottom (keycapTravel + 1.0f)
                 .translated (0.0f, isDown ? keycapTravel : 0.0f);
}

// The top-face plateau: inset and shifted up inside the cap so the front
// wall reads thicker, like a key seen slightly from the front. Labels and
// icons are laid out against this rect so they sit on (and ride) the face.
static juce::Rectangle<float> getKeycapFaceBounds (juce::Rectangle<float> bounds, bool isDown)
{
    const auto cap = getKeycapBounds (bounds, isDown);
    const float inset = juce::jlimit (2.0f, 5.0f, cap.getHeight() * 0.11f);
    return cap.reduced (inset).translated (0.0f, -inset * 0.30f);
}

static void drawKeycap (juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize,
                 float hover, bool isDown, float accentAmount = 0.0f,
                 juce::Colour base = panelDark)
{
    const auto cap = getKeycapBounds (bounds, isDown);
    const float corner = juce::jmin (3.0f, cornerSize);

    if (accentAmount > 0.001f)
        base = base.interpolatedWith (accentOrange, 0.16f * accentAmount);

    // Flat plate: pressed lifts the fill to the raised tone (CUERACK button).
    g.setColour (isDown ? panelHi.interpolatedWith (base, 0.4f) : base);
    g.fillRoundedRectangle (cap.reduced (0.5f), corner);

    // Hairline outline, dim cream brightening to cream on hover.
    g.setColour (glassTextMuted.interpolatedWith (glassText, hover)
                     .withAlpha (0.75f + 0.25f * hover));
    g.drawRoundedRectangle (cap.reduced (0.5f), corner, 1.0f);
}

// Shared chassis for the toggle switches (HALF TIME, SYNC TO DAW): a flat
// plate that warms toward the accent as the switch animates on, with a
// CUERACK power-dot riding the cap — hairline ring when off, accent-filled
// with a dark centre mark when on. One code path keeps hover/press/on
// states consistent between the switches.
static void drawGlassToggle (juce::Graphics& g, juce::Rectangle<float> bounds,
                             float position, float hover, bool isDown,
                             float ledRadius, float ledOffsetY,
                             bool isHalfTime = false)
{
    const float pos = juce::jlimit (0.0f, 1.0f, position);

    drawKeycap (g, bounds, 3.0f, hover, isDown, pos);

    // Power dot — positioned from the (possibly pressed) cap so it rides the key.
    const auto cap = getKeycapBounds (bounds, isDown);
    auto ledCentre = juce::Point<float> (cap.getCentreX(), cap.getY() + ledOffsetY);
    auto ledBounds = juce::Rectangle<float> (ledRadius * 2.0f, ledRadius * 2.0f).withCentre (ledCentre);

    if (pos > 0.01f)
    {
        const auto activeColour = isHalfTime ? juce::Colour (0xff38bdf8) : accentOrange;
        g.setColour (activeColour.withAlpha (pos));
        g.fillEllipse (ledBounds);
        g.setColour (darkInk.withAlpha (pos));
        g.fillEllipse (ledBounds.reduced (ledRadius * 0.60f));
    }

    if (pos < 0.99f)
    {
        g.setColour (glassTextMuted.withAlpha (0.8f * (1.0f - pos)));
        g.drawEllipse (ledBounds.reduced (0.5f), 1.0f);
    }
}

class CueSamplerLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    CueSamplerLookAndFeel()
    {
        // Any component that doesn't set its own font now falls back to the CUE
        // brand typeface (Syne) instead of the system sans-serif, so all UI text
        // matches the logo. (Monospaced numeric readouts opt out by naming their
        // own font explicitly.)
        if (auto tf = brandTypeface())
            setDefaultSansSerifTypeface (tf);

        refreshColours();
    }

    // (Re-)apply the CUERACK foundation colours this L&F hands to stock
    // widgets (combos, alert dialogs, text editors, slider value bubbles,
    // unstyled buttons). Called again after cue::applyTheme() swaps the
    // foundation, mirroring CUERACK's CueLookAndFeel::refreshColours().
    void refreshColours()
    {
        setColour (juce::ComboBox::backgroundColourId, panelDark);
        setColour (juce::ComboBox::outlineColourId, borderMid);
        setColour (juce::ComboBox::textColourId, textPrimary);
        setColour (juce::ComboBox::arrowColourId, textMuted);
        setColour (juce::ComboBox::buttonColourId, panelHi);
        setColour (juce::ComboBox::focusedOutlineColourId, borderLight);
        setColour (juce::AlertWindow::backgroundColourId, panelDark);
        setColour (juce::AlertWindow::textColourId, textPrimary);
        setColour (juce::AlertWindow::outlineColourId, borderMid);
        setColour (juce::TextEditor::backgroundColourId, blackPanel);
        setColour (juce::TextEditor::textColourId, textPrimary);
        setColour (juce::TextEditor::outlineColourId, borderMid);
        setColour (juce::TextEditor::focusedOutlineColourId, borderLight);
        setColour (juce::TextEditor::highlightColourId, juce::Colour (0xffd9542f).withAlpha (0.4f));
        setColour (juce::TextEditor::highlightedTextColourId, textPrimary);
        setColour (juce::CaretComponent::caretColourId, juce::Colour (0xffd9542f));
        setColour (juce::BubbleComponent::backgroundColourId, blackPanel);
        setColour (juce::BubbleComponent::outlineColourId, borderMid);
        setColour (juce::Label::textColourId, textMuted);
        setColour (juce::TextButton::buttonColourId, panelDark);
        setColour (juce::TextButton::buttonOnColourId, panelHi);
        setColour (juce::TextButton::textColourOffId, textPrimary);
        setColour (juce::TextButton::textColourOnId, textPrimary);
    }

    // Slider value bubbles in the CUERACK voice: mono digits in a flat well.
    juce::Font getSliderPopupFont (juce::Slider&) override { return monoFont (12.5f); }

    void drawBubble (juce::Graphics& g, juce::BubbleComponent& bubble,
                     const juce::Point<float>&, const juce::Rectangle<float>& body) override
    {
        if (body.getWidth() <= 0.0f || body.getHeight() <= 0.0f)
            return;
        g.setColour (bubble.findColour (juce::BubbleComponent::backgroundColourId));
        g.fillRoundedRectangle (body, 3.0f);
        g.setColour (bubble.findColour (juce::BubbleComponent::outlineColourId));
        g.drawRoundedRectangle (body, 3.0f, 1.0f);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                           juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override
    {
        const auto layout = createTooltipLayout (tipText, textPrimary);
        const auto width = (int) std::ceil (layout.getWidth() + 24.0f);
        const auto height = (int) std::ceil (layout.getHeight() + 18.0f);

        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (width + 16)
                                                                           : screenPos.x + 28,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - (height + 10)
                                                                           : screenPos.y + 12,
                                     width,
                                     height)
            .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);

        // CUERACK bubble: flat recessed well with a hairline outline.
        g.setColour (blackPanel);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (borderMid);
        g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

        auto layout = createTooltipLayout (text, textPrimary.withAlpha (0.94f));
        layout.draw (g, bounds.reduced (11.0f, 8.0f));
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar,
                        int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        juce::ignoreUnused (scrollbar);
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

        // Recessed CUERACK slot
        auto track = isScrollbarVertical ? bounds.reduced (bounds.getWidth() * 0.30f, 0.0f)
                                         : bounds.reduced (0.0f, bounds.getHeight() * 0.30f);
        const float trackRadius = juce::jmin (track.getWidth(), track.getHeight()) * 0.5f;
        g.setColour (blackPanel);
        g.fillRoundedRectangle (track, trackRadius);
        g.setColour (borderMid);
        g.drawRoundedRectangle (track, trackRadius, 1.0f);

        if (thumbSize <= 0)
            return;

        auto thumb = isScrollbarVertical
            ? bounds.withY ((float) thumbStartPosition).withHeight ((float) thumbSize).reduced (1.0f, 0.0f)
            : bounds.withX ((float) thumbStartPosition).withWidth ((float) thumbSize).reduced (0.0f, 1.0f);
        const float thumbRadius = juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f;

        // Flat cream cap, warming toward the accent on hover.
        const float hover = isMouseDown ? 1.0f : (isMouseOver ? 0.6f : 0.0f);
        g.setColour (glassTextMuted.interpolatedWith (accentOrange, 0.25f + 0.55f * hover));
        g.fillRoundedRectangle (thumb, thumbRadius);
        g.setColour (darkInk.withAlpha (0.35f));
        g.drawRoundedRectangle (thumb, thumbRadius, 1.0f);
    }

    // --- Popup / drop menus --------------------------------------------------
    // Drop menus (CHOP @ TRANS. sensitivity, warp-marker snap, key override...)
    // are drawn in the CUERACK voice: a flat recessed well with a hairline
    // outline, monospace rows, and accent ticks / hover rims. The mode-aware
    // accent (orange / ice / violet) follows the rest of the UI.

    int getPopupMenuBorderSize() override { return 7; }

    juce::Font getPopupMenuFont() override
    {
        return monoFont (12.5f);
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
        const float corner = 3.0f;

        g.setColour (blackPanel);
        g.fillRoundedRectangle (bounds, corner);
        g.setColour (borderMid);
        g.drawRoundedRectangle (bounds, corner, 1.0f);
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight, int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 60;
            idealHeight = 11;
            return;
        }

        idealHeight = standardMenuItemHeight > 0 ? juce::jmax (30, standardMenuItemHeight) : 30;
        idealWidth = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 56; // tick gutter + chevron/padding
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override
    {
        auto r = area.toFloat().reduced (12.0f, 0.0f);

        g.setColour (themedTitleColour (accentOrange).withAlpha (0.92f));
        g.setFont (monoFont (10.5f));
        g.drawText (sectionName.toUpperCase(), r.withTrimmedBottom (3.0f).toNearestInt(),
                    juce::Justification::bottomLeft, false);

        // Hairline under the header.
        g.setColour (borderMid);
        g.fillRect (juce::Rectangle<float> (r.getX(), r.getBottom() - 2.0f, r.getWidth(), 1.0f));
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu,
                            const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon,
                            const juce::Colour* textColourToUse) override
    {
        if (isSeparator)
        {
            auto line = area.toFloat().reduced (14.0f, 0.0f);
            g.setColour (borderMid);
            g.fillRect (juce::Rectangle<float> (line.getX(), line.getCentreY(), line.getWidth(), 1.0f));
            return;
        }

        auto r = area.reduced (4, 1).toFloat();
        const float corner = 2.0f;

        // Hover / highlight row: flat plate lift with an accent hairline.
        if (isHighlighted && isActive)
        {
            g.setColour (panelHi);
            g.fillRoundedRectangle (r, corner);
            g.setColour (accentOrange.withAlpha (0.75f));
            g.drawRoundedRectangle (r, corner, 1.0f);
        }

        const float tickGutter = 26.0f;

        // Tick: an accent check mark in the left gutter.
        if (isTicked)
        {
            auto c = r.withWidth (tickGutter).getCentre();
            const float s = 4.0f;
            juce::Path check;
            check.startNewSubPath (c.x - s,          c.y + s * 0.1f);
            check.lineTo          (c.x - s * 0.25f,  c.y + s * 0.8f);
            check.lineTo          (c.x + s,          c.y - s * 0.9f);
            g.setColour (accentOrange.brighter (0.25f));
            g.strokePath (check, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }
        else if (icon != nullptr)
        {
            icon->drawWithin (g, r.withWidth (tickGutter).reduced (5.0f),
                              juce::RectanglePlacement::centred, 1.0f);
        }

        // Submenu chevron / shortcut on the right.
        auto textRight = 12.0f;
        if (hasSubMenu)
        {
            auto c = r.removeFromRight (22.0f).getCentre();
            const float h = 3.6f, w = 3.0f;
            juce::Path arrow;
            arrow.startNewSubPath (c.x - w * 0.5f, c.y - h);
            arrow.lineTo          (c.x + w * 0.5f, c.y);
            arrow.lineTo          (c.x - w * 0.5f, c.y + h);
            g.setColour ((isHighlighted ? textPrimary : glassTextMuted).withAlpha (0.85f));
            g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            textRight = 0.0f;
        }
        else if (shortcutKeyText.isNotEmpty())
        {
            g.setColour (glassTextMuted.withAlpha (0.7f));
            g.setFont (getPopupMenuFont().withHeight (12.5f));
            g.drawText (shortcutKeyText, r.withTrimmedRight (12.0f).toNearestInt(),
                        juce::Justification::centredRight, false);
        }

        // Label.
        juce::Colour textColour = textColourToUse != nullptr ? *textColourToUse : glassText;
        if (! isActive)
            textColour = textMuted;
        else if (isHighlighted)
            textColour = textPrimary;

        g.setColour (textColour);
        g.setFont (getPopupMenuFont());
        auto textArea = r.withTrimmedLeft (tickGutter).withTrimmedRight (textRight);
        g.drawFittedText (text, textArea.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    float getHoverAlpha (juce::Button& button, bool isMouseOverButton)
    {
        if (auto* hb = dynamic_cast<SmoothHoverButton*> (&button))
            return hb->getHoverAlpha();
        if (auto* ab = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
            return ab->getHoverAlpha();
        return isMouseOverButton ? 1.0f : 0.0f;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override
    {
        const auto style = getCueStyle (button);

        if (style == "transportSquare" || style == "halfTime"
            || style == "flatAction" || style == "separateAction" || style == "utilitySync" || style == "effectSwitch")
        {
            // Keycap travel is applied inside drawKeycap; effectSwitch is a
            // left/right toggle, not a press button, and never dips.
            auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);

            if (style == "transportSquare")
            {
                drawKeycap (g, bounds, 11.0f, getHoverAlpha (button, isMouseOverButton), isButtonDown);
                return;
            }

            if (style == "halfTime" || style == "utilitySync")
            {
                float position = button.getToggleState() ? 1.0f : 0.0f;
                float hover = getHoverAlpha (button, isMouseOverButton);
                if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                    position = animatedButton->getCurrentAnimationPosition();

                const bool isHalfTime = style == "halfTime";
                drawGlassToggle (g, bounds, position, hover, isButtonDown,
                                 isHalfTime ? 3.5f : 4.0f,   // LED radius
                                 isHalfTime ? 11.0f : 14.0f, // LED y-offset
                                 isHalfTime);
                return;
            }


            if (style == "effectSwitch")
            {
                auto trackBounds = bounds.reduced (1.0f);

                // Recessed CUERACK well.
                g.setColour (blackPanel);
                g.fillRoundedRectangle (trackBounds, 3.0f);
                g.setColour (borderMid);
                g.drawRoundedRectangle (trackBounds.reduced (0.5f), 3.0f, 1.0f);

                float position = button.getToggleState() ? 1.0f : 0.0f;
                float hover = getHoverAlpha (button, isMouseOverButton);
                if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                {
                    position = animatedButton->getCurrentAnimationPosition();
                }

                float clampedPos = juce::jlimit (0.0f, 1.0f, position);

                if (clampedPos > 0.0f)
                {
                    g.setColour (accentOrange.withAlpha (0.15f * clampedPos));
                    g.fillRoundedRectangle (trackBounds.reduced (1.0f), 2.5f);
                }

                auto thumbWidth = trackBounds.getWidth() * 0.5f;
                auto thumbBounds = trackBounds.withWidth (thumbWidth).reduced (1.0f);

                // Allow a tiny bounce beyond track bounds (mechanical impact feel)
                float drawPosForThumb = juce::jlimit (-0.03f, 1.03f, position);
                float startX = trackBounds.getX() + 1.0f;
                float endX = trackBounds.getRight() - thumbWidth + 1.0f;
                float currentX = startX + (endX - startX) * drawPosForThumb;
                thumbBounds.setX (currentX);

                // CUERACK fader cap: cream, warming to the accent as it
                // switches on, with a dark centre line.
                auto thumbColour = glassText.interpolatedWith (accentOrange, clampedPos)
                                       .brighter (0.06f * hover);
                g.setColour (thumbColour);
                g.fillRoundedRectangle (thumbBounds, 2.0f);
                g.setColour (darkInk.withAlpha (0.85f));
                g.drawLine (thumbBounds.getCentreX(), thumbBounds.getY() + 2.5f,
                            thumbBounds.getCentreX(), thumbBounds.getBottom() - 2.5f, 1.4f);

                return;
            }

            // flatAction
            drawKeycap (g, bounds, juce::jlimit (6.0f, 10.0f, bounds.getHeight() * 0.26f),
                        getHoverAlpha (button, isMouseOverButton), isButtonDown);
            return;
        }

        // Generic fallback (helpButton, waveScaleStep, anything unstyled):
        // same keycap chassis as the styled buttons so the family stays uniform.
        juce::ignoreUnused (backgroundColour);
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto corner = juce::jlimit (5.0f, 10.0f, bounds.getHeight() * 0.24f);
        drawKeycap (g, bounds, corner, getHoverAlpha (button, isMouseOverButton), isButtonDown);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool, bool isButtonDown) override
    {
        const auto style = getCueStyle (button);

        if (style == "effectSwitch")
            return; // Handled entirely in drawButtonBackground

        // Lay everything out against the keycap's top face so labels sit on
        // the plateau and ride the key as it travels down.
        const auto localBounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto face = getKeycapFaceBounds (localBounds, isButtonDown);
        const auto cap = getKeycapBounds (localBounds, isButtonDown);
        const auto bounds = face.toNearestInt();

        if (style == "transportSquare")
        {
            juce::Path icon;
            const auto iconName = getCueIcon (button);
            auto iconBounds = face.withSizeKeepingCentre (20.0f, 20.0f);

            if (iconName == "play")
            {
                icon.startNewSubPath (iconBounds.getX() + 4.0f, iconBounds.getY() + 2.0f);
                icon.lineTo (iconBounds.getRight() - 2.0f, iconBounds.getCentreY());
                icon.lineTo (iconBounds.getX() + 4.0f, iconBounds.getBottom() - 2.0f);
                icon.closeSubPath();
            }
            else if (iconName == "pause")
            {
                icon.addRoundedRectangle (iconBounds.removeFromLeft (7.0f), 1.0f);
                iconBounds.removeFromLeft (6.0f);
                icon.addRoundedRectangle (iconBounds.removeFromLeft (7.0f), 1.0f);
            }
            else if (iconName == "stop")
            {
                icon.addRoundedRectangle (iconBounds.reduced (3.0f), 2.0f);
            }

            g.setColour (glassText);
            g.fillPath (icon);
            return;
        }

        if (style == "halfTime")
        {
            float position = button.getToggleState() ? 1.0f : 0.0f;
            if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                position = animatedButton->getCurrentAnimationPosition();
            float clampedPos = juce::jlimit (0.0f, 1.0f, position);

            auto textOff = textPrimary.withAlpha (0.75f);
            auto textOn  = juce::Colour (0xff38bdf8); // vibrant ice-blue
            g.setColour (textOff.interpolatedWith (textOn, clampedPos));
            g.setFont (heavyFont (10.5f).withExtraKerningFactor (0.05f));
            // Below the status LED inside the face.
            g.drawFittedText (button.getButtonText(),
                              bounds.withTop (juce::roundToInt (cap.getY() + 15.0f)),
                              juce::Justification::centred, 2);
            return;
        }

        if (style == "flatAction")
        {
            float hover = getHoverAlpha (button, false);
            auto textOff = textMuted;
            auto textOn = textPrimary;
            g.setColour (textOff.interpolatedWith (textOn, hover));
            g.setFont (monoFont (juce::jlimit (8.0f, 15.0f, face.getHeight() * 0.44f)));
            g.drawFittedText (button.getButtonText().toUpperCase(), bounds.reduced (3, 0), juce::Justification::centred, 1);
            return;
        }

        if (style == "separateAction")
        {
            float hover = getHoverAlpha (button, false);
            auto textOff = textPrimary.withAlpha (0.92f);
            auto textOn  = themedTitleColour (accentOrange);
            g.setColour (textOff.interpolatedWith (textOn, hover));
            g.setFont (heavyFont (14.0f).withExtraKerningFactor (0.08f));
            g.drawFittedText (button.getButtonText().toUpperCase(), bounds, juce::Justification::centred, 1);
            return;
        }

        if (style == "helpButton")
        {
            float hover = getHoverAlpha (button, false);
            auto baseColour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                         : juce::TextButton::textColourOffId);
            g.setColour (baseColour.interpolatedWith (themedTitleColour (accentOrange), hover));
            g.setFont (monoFont (juce::jlimit (12.5f, 18.0f, face.getHeight() * 0.52f)));
            g.drawFittedText (button.getButtonText().toUpperCase(), bounds, juce::Justification::centred, 1);
            return;
        }

        if (style == "utilitySync")
        {
            float position = button.getToggleState() ? 1.0f : 0.0f;
            if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                position = animatedButton->getCurrentAnimationPosition();
            float clampedPos = juce::jlimit (0.0f, 1.0f, position);

            auto textOff = textPrimary.withAlpha (0.75f);
            auto textOn  = textPrimary;
            g.setColour (textOff.interpolatedWith (textOn, clampedPos));
            g.setFont (heavyFont (12.5f).withExtraKerningFactor (0.06f));
            // Below the status LED (radius 4 at capY + 14), inside the face.
            g.drawFittedText (button.getButtonText().toUpperCase(),
                              bounds.withTop (juce::roundToInt (cap.getY() + 19.0f)),
                              juce::Justification::centred, 1);
            return;
        }

        if (style == "waveScaleStep")
        {
            float hover = getHoverAlpha (button, false);
            g.setColour (textPrimary.withAlpha (0.9f).interpolatedWith (accentOrange, hover));
            g.setFont (monoFont (15.0f));
            g.drawFittedText (button.getButtonText(), bounds, juce::Justification::centred, 1);
            return;
        }

        auto fontSize = juce::jlimit (7.0f, 13.0f, face.getHeight() * 0.36f);
        float hover = getHoverAlpha (button, false);
        auto baseColor = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                     : juce::TextButton::textColourOffId);
        g.setColour (baseColor.interpolatedWith (accentOrange, hover));
        g.setFont (monoFont (fontSize));
        g.drawFittedText (button.getButtonText().toUpperCase(), bounds.reduced (2, 0), juce::Justification::centred, 2);
    }

    float getSliderHoverAlpha (juce::Slider& slider, bool isMouseOverOrDragging)
    {
        if (auto* optSlider = dynamic_cast<OptResetSlider*> (&slider))
            return optSlider->getHoverAlpha();
        return isMouseOverOrDragging ? 1.0f : 0.0f;
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        const float hover = getSliderHoverAlpha (slider, slider.isMouseOverOrDragging());

        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
        const float diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
        bounds = bounds.withSizeKeepingCentre (diameter, diameter);

        // All knob styles share the CUERACK rotary look; per-knob accents
        // (CUE / GAIN / PITCH minis, sand effect knobs) come through the
        // cueAccent property. Knobs without one wear the foundation cream,
        // which swaps to ink in the light theme like the rack's controls.
        drawMetalKnob (g, bounds, sliderPosProportional, rotaryStartAngle, rotaryEndAngle,
                       getCueAccent (slider, glassText), hover);
    }
};

class LabelledKnob final : public juce::Component
{
public:
    LabelledKnob (const juce::String& text, int knobDiameter, float labelHeight,
                  juce::String style = {}, juce::Colour accent = juce::Colour (0))
        : diameter (knobDiameter)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (0.0, 1.0, 0.0);
        slider.setValue (0.5, juce::dontSendNotification);
        slider.getProperties().set ("cueStyle", style);
        if (accent != juce::Colour (0))
            slider.getProperties().set ("cueAccent", (int) accent.getARGB());
        slider.setPopupDisplayEnabled (true, false, nullptr);
        slider.captureCurrentValueAsDefault();
        addAndMakeVisible (slider);

        configureTextLabel (label, text, labelHeight, glassTextMuted, juce::Justification::centred);
        addAndMakeVisible (label);
    }

    juce::Slider& getSlider() noexcept { return slider; }
    void captureCurrentValueAsDefault() noexcept { slider.captureCurrentValueAsDefault(); }

    // Labels cache their colour at construction; re-apply after a theme swap.
    void refreshColours()
    {
        label.setColour (juce::Label::textColourId, glassTextMuted);
        repaint();
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto knobBounds = bounds.removeFromTop (juce::jmin (diameter, bounds.getHeight()));
        slider.setBounds (knobBounds.withSizeKeepingCentre (diameter, diameter));
        label.setBounds (bounds.removeFromTop (juce::jmin (20, bounds.getHeight())));
    }

private:
    OptResetSlider slider;
    juce::Label label;
    int diameter = 44;
};

// On-screen MIDI keyboard along the bottom strip: lights up with the notes
// driving the chops (host MIDI or clicks — clicked keys are injected into the
// processor's MIDI stream, so they trigger chops too). Pressed-key overlays
// track the accent so HALF TIME / WARP recolour them with the rest of the UI.
class GlassKeyboard final : public juce::MidiKeyboardComponent
{
public:
    explicit GlassKeyboard (juce::MidiKeyboardState& state)
        : juce::MidiKeyboardComponent (state, juce::MidiKeyboardComponent::horizontalKeyboard)
    {
        setOctaveForMiddleC (4); // note 36 reads "C2", matching the chop mapping
        setAvailableRange (24, 107); // C1..B7
        setScrollButtonsVisible (false);
        // CUERACK ivory: cream naturals, warm near-black sharps, dark ink text.
        setColour (juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour (0xfff2e7da));
        setColour (juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour (0xff171009));
        setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff171009).withAlpha (0.35f));
        setColour (juce::MidiKeyboardComponent::shadowColourId, juce::Colour (0xff171009).withAlpha (0.35f));
        setColour (juce::MidiKeyboardComponent::textLabelColourId, juce::Colour (0xff746757));
    }

    // Lights a key for a previewed chop without going through the shared
    // MidiKeyboardState (which would inject a note and re-trigger the chop).
    // Drawn with the same pressed-overlay as a real key-down.
    void setHighlightedNote (int note)
    {
        if (note == highlightedNote)
            return;
        highlightedNote = note;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // Refreshed per paint so mode changes recolour the overlays. setColour
        // is a no-op (no repaint loop) when the value is unchanged.
        setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, accentOrange.withAlpha (0.8f));
        setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accentOrange.withAlpha (0.28f));
        juce::MidiKeyboardComponent::paint (g);
    }

    void drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override
    {
        juce::MidiKeyboardComponent::drawWhiteNote (midiNoteNumber, g, area,
                                                    isDown || midiNoteNumber == highlightedNote,
                                                    isOver, lineColour, textColour);
    }

    void drawBlackNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour noteFillColour) override
    {
        juce::MidiKeyboardComponent::drawBlackNote (midiNoteNumber, g, area,
                                                    isDown || midiNoteNumber == highlightedNote,
                                                    isOver, noteFillColour);
    }

private:
    int highlightedNote = -1;
};

class DisplayBox final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    DisplayBox (juce::String displayCaption, juce::String displayValue, float valueHeight,
                juce::String displayStyle = {})
        : caption (std::move (displayCaption)),
          value (std::move (displayValue)),
          fontHeight (valueHeight),
          style (std::move (displayStyle))
    {
    }

    void setValueText (juce::String newValue)
    {
        if (isScanning)
            return;

        if (value == newValue)
            return;

        value = std::move (newValue);
        repaint();
    }

    void setScanning (bool scanning)
    {
        if (scanning == isScanning)
        {
            if (scanning)
            {
                ++scanFrame;
                repaint();
            }
            return;
        }
        isScanning = scanning;
        scanFrame = 0;
        repaint();
    }

    void setClickHandler (std::function<void()> handler)
    {
        clickHandler = std::move (handler);
        setMouseCursor (clickHandler ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
    }

    void setWheelHandler (std::function<void(float)> handler)
    {
        wheelHandler = std::move (handler);
    }

    void paint (juce::Graphics& g) override
    {
        if (style == "keyBox" || style == "tempoBox" || style == "timeBox")
        {
            auto bounds = getLocalBounds().toFloat();

            // Flat recessed CUERACK well with a hairline outline.
            g.setColour (blackPanel);
            g.fillRoundedRectangle (bounds.reduced (0.5f), smallCorner);
            g.setColour (borderMid);
            g.drawRoundedRectangle (bounds.reduced (0.5f), smallCorner, 1.0f);

            if (style == "timeBox")
            {
                g.setColour (themedTitleColour (accentOrange));
                g.setFont (cousineFont (18.0f).withExtraKerningFactor (0.05f));
                g.drawText (value, getLocalBounds(), juce::Justification::centred, false);
            }
            else if (isScanning)
            {
                // Caption
                g.setColour (themedTitleColour (accentOrange).withAlpha (0.8f));
                g.setFont (monoFont (9.0f).withExtraKerningFactor (0.05f));
                g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (22).translated (0, 3), juce::Justification::centred, false);

                // Three-dot pulse: each dot cycles through a brightness wave 120° apart
                const int   numDots   = 3;
                const float dotR      = 3.5f;
                const float spacing   = 13.0f;
                const float totalW    = (numDots - 1) * spacing;
                const float startX    = bounds.getCentreX() - totalW * 0.5f;
                const float dotY      = bounds.getBottom() - 14.0f;
                const float speed     = 0.18f; // fraction of full cycle per frame
                const float phase     = std::fmod ((float) scanFrame * speed, 1.0f);

                for (int i = 0; i < numDots; ++i)
                {
                    const float t       = std::fmod (phase + (float) i / (float) numDots, 1.0f);
                    const float bright  = 0.5f * (1.0f + std::cos (t * juce::MathConstants<float>::twoPi));
                    const float alpha   = 0.25f + 0.75f * bright;
                    g.setColour (themedTitleColour (accentOrange).withAlpha (alpha));
                    g.fillEllipse (startX + (float) i * spacing - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);
                }
            }
            else
            {
                g.setColour (themedTitleColour (accentOrange).withAlpha (0.8f));
                g.setFont (monoFont (9.0f).withExtraKerningFactor (0.05f));
                g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop(22).translated (0, 3), juce::Justification::centred, false);

                g.setColour (themedTitleColour (accentOrange));
                g.setFont (cousineFont (15.0f));
                g.drawText (value, getLocalBounds().removeFromBottom(26).translated (0, -2), juce::Justification::centred, false);
            }
            return;
        }

        auto bounds = getLocalBounds().toFloat();
        g.setColour (blackPanel);
        g.fillRoundedRectangle (bounds.reduced (0.5f), smallCorner);
        g.setColour (borderMid);
        g.drawRoundedRectangle (bounds.reduced (0.5f), smallCorner, 1.0f);

        if (caption.isNotEmpty())
        {
            g.setColour (textMuted);
            g.setFont (monoFont (7.0f));
            g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (12), juce::Justification::centredLeft);
        }

        g.setColour (themedTitleColour (accentOrange));
        g.setFont (monoFont (fontHeight));
        g.drawFittedText (value, getLocalBounds().reduced (8, 10), juce::Justification::centred, 1);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (clickHandler && contains (event.getPosition()))
            clickHandler();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (wheelHandler == nullptr || wheel.deltaY == 0.0f)
            return;

        wheelHandler (wheel.deltaY);
    }

private:
    juce::String caption;
    juce::String value;
    float fontHeight = 16.0f;
    juce::String style;
    std::function<void()> clickHandler;
    std::function<void(float)> wheelHandler;
    bool isScanning = false;
    int  scanFrame  = 0;
};

class HelpOverlayComponent final : public juce::Component,
                                   public juce::KeyListener
{
public:
    using juce::Component::keyPressed;

    HelpOverlayComponent() { setInterceptsMouseClicks (true, true); }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Flat CUERACK plate, near-opaque so it reads as a dedicated screen.
        g.setColour (panelDark.withAlpha (0.97f));
        g.fillRoundedRectangle (bounds.reduced (0.5f), mediumCorner);
        g.setColour (borderMid);
        g.drawRoundedRectangle (bounds.reduced (0.5f), mediumCorner, 1.0f);

        // Accent stripe at top
        g.setColour (accentOrange);
        juce::Path stripe;
        stripe.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), 4.0f,
                                    mediumCorner, mediumCorner, true, true, false, false);
        g.fillPath (stripe);

        // Dismiss hint
        g.setColour (textMuted);
        g.setFont (monoFont (10.0f).withExtraKerningFactor (0.06f));
        g.drawText ("PRESS ? OR CLICK ANYWHERE TO CLOSE",
                    bounds.withTrimmedTop (12.0f).withHeight (18.0f),
                    juce::Justification::centred, false);

        paintContent (g, bounds.reduced (30.0f, 44.0f));
    }

    void mouseDown (const juce::MouseEvent&) override { setVisible (false); }

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (isVisible() && (key.getTextCharacter() == '?' || key == juce::KeyPress::escapeKey))
        {
            setVisible (false);
            return true;
        }
        return false;
    }

private:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float colW  = (area.getWidth() - 34.0f) * 0.5f;
        auto leftCol  = area.withWidth (colW);
        auto rightCol = area.withX (area.getX() + colW + 34.0f).withWidth (colW);

        paintSection (g, leftCol, "QUICK START",
        {
            "1.  Drag an audio file onto the waveform, or press LOAD SAMPLE.",
            "2.  The plugin detects BPM and slices the sample into chops.",
            "3.  Click any chop on the waveform to select and preview it.",
            "4.  Chops play via MIDI - C2 = chop 1, D2 = chop 2, and so on.",
            "5.  Use BARS to set chop size: 1, 2, 4, or 8 bars.",
            "6.  Tweak CUE, GAIN, and PITCH to customise each chop.",
            "7.  Use the TEMPO trim knob if chop boundaries feel off-beat.",
        });

        const float afterGestures = paintSection (g, rightCol, "MOUSE GESTURES",
        {
            "Click chop                     Select + preview",
            "Double-click chop           Toggle favourite (pink)",
            "Drag chop edge              Resize grid + update tempo",
            "Shift-drag chop edge       Snap edge to nearest bar",
            "Drag audio file onto view   Load new sample",
            "Alt-click any knob           Reset to default value",
            "Scroll wheel                   Zoom in / out to cursor",
            "Horizontal scroll              Pan left / right",
        });

        paintSection (g, rightCol.withY (afterGestures + 22.0f), "WARP MARKERS",
        {
            "Press WARP to enter warp-edit mode.",
            "Click the waveform to place a time-stretch marker.",
            "Drag a marker left / right to shift local timing.",
            "Right-click a marker to snap it to a grid division or clear it.",
            "BAR / BEAT / 1-2 / 16 combo sets the snap grid.",
            "Press WARP again (or Escape) to exit warp-edit mode.",
        });

        // Footer glossary
        auto footer = area.withY (area.getBottom() - 82.0f).withHeight (82.0f);
        g.setColour (borderMid.withAlpha (0.6f));
        g.drawLine (footer.getX(), footer.getY(), footer.getRight(), footer.getY(), 1.0f);
        g.setColour (textMuted);
        g.setFont (monoFont (10.0f));
        const juce::StringArray defs {
            "CHOP = auto-sliced bar segment   |   MIDI C2 = chop 1,  D2 = chop 2 ...",
            "CUE = loop-start point inside a chop   |   GRID = beat-grid anchor offset (ms)",
            "PITCH (under waveform) = global shift   |   PITCH (chop controls) = per-chop shift",
            "SYNC = auto-match speed to DAW BPM   |   TEMPO trim = nudge grid +/-10 BPM",
        };
        float fy = footer.getY() + 12.0f;
        for (const auto& d : defs)
        {
            g.drawText (d, juce::Rectangle<float> (footer.getX(), fy, footer.getWidth(), 16.0f),
                        juce::Justification::centred, false);
            fy += 18.0f;
        }
    }

    float paintSection (juce::Graphics& g, juce::Rectangle<float> col,
                        const juce::String& heading,
                        std::initializer_list<const char*> lines)
    {
        float y = col.getY();
        g.setColour (themedTitleColour (accentOrange));
        g.setFont (heavyFont (14.0f).withExtraKerningFactor (0.08f));
        g.drawText (heading, juce::Rectangle<float> (col.getX(), y, col.getWidth(), 22.0f),
                    juce::Justification::centredLeft, false);
        y += 27.0f;
        g.setColour (borderMid.withAlpha (0.7f));
        g.drawLine (col.getX(), y, col.getRight(), y, 1.0f);
        y += 10.0f;
        g.setFont (monoFont (11.0f));
        for (const auto* line : lines)
        {
            g.setColour (textPrimary.withAlpha (0.88f));
            g.drawText (line, juce::Rectangle<float> (col.getX(), y, col.getWidth(), 20.0f),
                        juce::Justification::centredLeft, false);
            y += 22.0f;
        }
        return y;
    }
};

class WarpHelpOverlayComponent final : public juce::Component,
                                       public juce::KeyListener
{
public:
    using juce::Component::keyPressed;

    WarpHelpOverlayComponent()
    {
        setInterceptsMouseClicks (true, true);

        okButton.setButtonText ("OK");
        okButton.getProperties().set ("cueStyle", "flatAction");
        okButton.onClick = [this] { setVisible (false); };
        addAndMakeVisible (okButton);
    }

    void resized() override
    {
        const auto card = getLocalBounds().toFloat().withSizeKeepingCentre (680.0f, 440.0f);
        okButton.setBounds (juce::roundToInt (card.getCentreX() - 70.0f),
                            juce::roundToInt (card.getBottom() - 62.0f),
                            140, 44);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        // Dark translucent overlay backdrop behind popup
        g.setColour (juce::Colours::black.withAlpha (0.50f));
        g.fillAll();

        // Dialog card frame centered in bounds (large card: 680x440)
        const auto card = bounds.withSizeKeepingCentre (680.0f, 440.0f);

        // Card background
        g.setColour (panelDark.withAlpha (0.98f));
        g.fillRoundedRectangle (card, mediumCorner);
        g.setColour (borderMid);
        g.drawRoundedRectangle (card, mediumCorner, 1.5f);

        // WARP accent stripe (glowing purple 0xffa855f7)
        g.setColour (juce::Colour (0xffa855f7));
        juce::Path stripe;
        stripe.addRoundedRectangle (card.getX(), card.getY(), card.getWidth(), 5.0f,
                                    mediumCorner, mediumCorner, true, true, false, false);
        g.fillPath (stripe);

        // Header Title
        g.setColour (themedTitleColour (juce::Colour (0xffa855f7)));
        g.setFont (heavyFont (22.0f).withExtraKerningFactor (0.10f));
        g.drawText ("WARP MODE GUIDE", card.withTrimmedTop (20.0f).withHeight (32.0f),
                    juce::Justification::centred, false);

        // Separator line
        g.setColour (borderMid.withAlpha (0.75f));
        g.drawLine (card.getX() + 24.0f, card.getY() + 60.0f,
                    card.getRight() - 24.0f, card.getY() + 60.0f, 1.2f);

        // Feature instructions list
        struct Bullet { const char* label; const char* desc; };
        const Bullet bullets[] = {
            { "DROP MARKERS",  "Click inside any chop on the waveform to add warp markers." },
            { "TIME STRETCH",  "Drag markers left or right to shift local audio timing." },
            { "GRID SNAP",     "Markers auto-snap to your grid. Right-click to snap or remove." },
            { "CLEAR ALL",     "Use the CLEAR ALL button to wipe markers for the active chop." },
            { "EXITING WARP",  "Click WARP again or press Escape anytime to exit WARP mode." }
        };

        float y = card.getY() + 78.0f;
        const float x = card.getX() + 32.0f;
        const float labelW = 145.0f;
        const float textW  = card.getWidth() - 64.0f - labelW;

        for (const auto& b : bullets)
        {
            // Accent label (prominent purple)
            g.setFont (monoFont (13.5f).withExtraKerningFactor (0.06f));
            g.setColour (juce::Colour (0xffc084fc));
            g.drawText (b.label, juce::Rectangle<float> (x, y, labelW, 26.0f),
                        juce::Justification::centredLeft, false);

            // Description (large clear text)
            g.setFont (brandFont (15.5f));
            g.setColour (textPrimary.withAlpha (0.95f));
            g.drawText (b.desc, juce::Rectangle<float> (x + labelW, y, textW, 26.0f),
                        juce::Justification::centredLeft, false);

            y += 38.0f;
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Dismiss if clicking backdrop outside the dialog card
        const auto card = getLocalBounds().toFloat().withSizeKeepingCentre (680.0f, 440.0f);
        if (! card.contains (e.position))
            setVisible (false);
    }

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (isVisible() && (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey))
        {
            setVisible (false);
            return true;
        }
        return false;
    }

private:
    cue::SmoothHoverButton okButton;
};

// The "CUE." brand wordmark (the bundled cue_logo_white.svg, including its
// trailing square period), recoloured to CUERACK cream ink. Rendered as vector
// so it stays crisp at any UI scale and sits on the top line of the lockup.
static const char* const cueWordmarkSvg =
R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="20 -700 4123 760"><path fill="#F2E7DA" d="M1255.0 -267Q1250.0 -172 1186.0 -108.0Q1122.0 -44 997.5 -12.0Q873.0 20 683.0 20Q538.0 20 423.0 4.5Q308.0 -11 227.0 -48.5Q146.0 -86 103.0 -151.0Q60.0 -216 60.0 -315Q60.0 -414 103.0 -480.5Q146.0 -547 227.0 -586.5Q308.0 -626 423.0 -643.0Q538.0 -660 683.0 -660Q873.0 -660 998.0 -625.5Q1123.0 -591 1187.0 -524.0Q1251.0 -457 1256.0 -361H996.0Q984.0 -393 952.0 -417.5Q920.0 -442 856.0 -456.0Q792.0 -470 683.0 -470Q555.0 -470 475.0 -454.5Q395.0 -439 358.0 -405.0Q321.0 -371 321.0 -315Q321.0 -264 358.0 -232.0Q395.0 -200 475.0 -185.0Q555.0 -170 683.0 -170Q792.0 -170 855.5 -183.0Q919.0 -196 951.0 -218.5Q983.0 -241 995.0 -267Z M2303.0 -365V-640H2553.0V-320Q2553.0 -235 2521.5 -175.5Q2490.0 -116 2434.0 -77.5Q2378.0 -39 2305.0 -18.0Q2232.0 3 2148.5 11.5Q2065.0 20 1979.0 20Q1888.0 20 1803.0 11.5Q1718.0 3 1645.5 -18.0Q1573.0 -39 1519.0 -77.5Q1465.0 -116 1434.5 -175.5Q1404.0 -235 1404.0 -320V-640H1654.0V-365Q1654.0 -285 1693.0 -243.0Q1732.0 -201 1804.5 -185.5Q1877.0 -170 1979.0 -170Q2078.0 -170 2151.0 -185.5Q2224.0 -201 2263.5 -243.0Q2303.0 -285 2303.0 -365Z M2961.0 -270V-190H3721.0V0H2711.0V-640H3719.0V-450H2961.0V-370H3581.0V-270Z M4103.0 -151V0H3849.0V-151Z"/></svg>)SVG";

class HeaderComponent final : public juce::Component,
                              public juce::SettableTooltipClient,
                              private juce::Timer
{
public:
    ~HeaderComponent() override { stopTimer(); }

    explicit HeaderComponent (AudioPluginAudioProcessor& p)
        : processor (p)
    {
        setBufferedToImage (false);
        startTimerHz (headerRefreshHz);

        const juce::String svg (cueWordmarkSvg);
        cueWordmark = juce::Drawable::createFromImageData (svg.toRawUTF8(),
                                                           (size_t) svg.getNumBytesAsUTF8());

        configureButton (helpButton, "?", textPrimary);
        helpButton.getProperties().set ("cueStyle", "helpButton");
        helpButton.setTooltip ("Open the quick-start reference guide.");
        helpButton.onClick = [this] { if (onHelpRequested) onHelpRequested(); };
        addAndMakeVisible (helpButton);

        // Steps back through chop/grid edits (add/delete/resize/transient-chop,
        // cue/gain/pitch, warp markers, tempo trim & start). Disabled when there
        // is nothing to undo.
        configureButton (undoButton, "UNDO", textPrimary);
        undoButton.getProperties().set ("cueStyle", "helpButton");
        undoButton.setTooltip ("Undo the last chop edit.");
        undoButton.setEnabled (processor.canUndoEdit());
        undoButton.onClick = [this] { processor.undoLastEdit(); };
        addAndMakeVisible (undoButton);

        // Light/dark theme toggle, mirroring CUERACK's header button. The
        // label names the CURRENT theme; the editor owns the actual swap.
        configureButton (themeButton, isLight() ? "LIGHT" : "DARK", textPrimary);
        themeButton.getProperties().set ("cueStyle", "flatAction");
        themeButton.setTooltip ("Switch between light and dark themes.");
        themeButton.onClick = [this] { if (onThemeToggled) onThemeToggled(); };
        addAndMakeVisible (themeButton);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        windowDragger.startDraggingComponent (getTopLevelComponent(), e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        windowDragger.dragComponent (getTopLevelComponent(), e, nullptr);
    }

    void resized() override
    {
        constexpr int buttonSize = 38;
        constexpr int rightMargin = 14;
        constexpr int topMargin = 12;
        constexpr int undoWidth = 76;
        constexpr int themeWidth = 86;
        constexpr int gap = 8;
        helpButton.setBounds (getWidth() - rightMargin - buttonSize, topMargin, buttonSize, buttonSize);
        undoButton.setBounds (helpButton.getX() - gap - undoWidth, topMargin, undoWidth, buttonSize);
        themeButton.setBounds (undoButton.getX() - gap - themeWidth, topMargin, themeWidth, buttonSize);
    }

    int utilityControlsLeft() const noexcept { return themeButton.getX(); }

    // Right edge of the CUE. wordmark in header-local coordinates, so the
    // editor can seat the orb flush beside the lockup.
    int lockupRight() const
    {
        float lockupW = 210.0f;
        if (cueWordmark != nullptr)
        {
            const auto db = cueWordmark->getDrawableBounds();
            if (db.getHeight() > 0.0f)
                lockupW = 38.0f * db.getWidth() / db.getHeight();
        }
        return 10 + juce::roundToInt (lockupW);
    }

    // Re-apply cached colours + the theme button's label after a theme swap.
    void refreshColours()
    {
        for (auto* b : { &helpButton, &undoButton, &themeButton })
        {
            b->setColour (juce::TextButton::textColourOffId, textPrimary);
            b->setColour (juce::TextButton::textColourOnId, textPrimary);
        }
        themeButton.setButtonText (isLight() ? "LIGHT" : "DARK");

        // The wordmark SVG is baked cream; re-ink it to the foundation text
        // colour so it stays legible on light plates.
        if (cueWordmark != nullptr && wordmarkColour != textPrimary)
        {
            cueWordmark->replaceColour (wordmarkColour, textPrimary);
            wordmarkColour = textPrimary;
        }

        repaint();
    }

    std::function<void()> onHelpRequested;
    std::function<void()> onThemeToggled;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour (borderLight);
        g.drawLine ((float) bounds.getX(), (float) bounds.getBottom() - 1.0f,
                    (float) bounds.getRight(), (float) bounds.getBottom() - 1.0f, 1.0f);

        // --- Stacked brand lockup: "CUE." over a wide-tracked "SAMPLER" ---
        const float logoX = 10.0f;
        const float logoY = 4.0f;
        const float logoH = 38.0f;
        float lockupW = 210.0f; // fallback if the wordmark fails to parse

        if (cueWordmark != nullptr)
        {
            const auto db = cueWordmark->getDrawableBounds();
            if (db.getHeight() > 0.0f)
                lockupW = logoH * db.getWidth() / db.getHeight();
            cueWordmark->drawWithin (g, { logoX, logoY, lockupW, logoH },
                                     juce::RectanglePlacement::stretchToFit, 1.0f);
        }

        // "SAMPLER" sits beneath, letter-spaced to span the wordmark's width.
        const float samplerSize = 17.0f;
        auto samplerFont = juce::Font (juce::FontOptions (brandFontName(), samplerSize, juce::Font::plain));
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (samplerFont, "SAMPLER", 0.0f, 0.0f);
            const float naturalW = ga.getBoundingBox (0, -1, true).getWidth();
            const float kern = juce::jmax (0.0f, (lockupW - naturalW) / (samplerSize * 7.0f));
            samplerFont = samplerFont.withExtraKerningFactor (kern);
        }

        const int samplerTop = (int) std::round (logoY + logoH + 1.0f);
        g.setColour (textPrimary);
        g.setFont (samplerFont);
        g.drawText ("SAMPLER",
                    juce::Rectangle<int> ((int) std::round (logoX), samplerTop,
                                          (int) std::ceil (lockupW) + 60, 20),
                    juce::Justification::centredLeft, false);

        // Version number sits just beyond the compact CUE orb (an editor-level
        // sibling, so this buffered header stays static while the orb animates).
        // Orb placement: 14px gap + 56px body + 10px version gap.
        g.setColour (textMuted);
        g.setFont (monoFont (9.0f));
        g.drawText ("v" + juce::String (CUE_VERSION_STRING),
                    juce::Rectangle<int> ((int) std::round (logoX + lockupW + 80.0f), 24,
                                          70, 16),
                    juce::Justification::centredLeft, false);

    }

private:
    void timerCallback() override
    {
        // The header is static now that the output meter is gone; the only live
        // state is the undo button's enabled flag.
        if (const bool canUndo = processor.canUndoEdit(); canUndo != undoButton.isEnabled())
            undoButton.setEnabled (canUndo);
    }

    AudioPluginAudioProcessor& processor;
    juce::ComponentDragger windowDragger;
    SmoothHoverButton helpButton;
    SmoothHoverButton undoButton;
    SmoothHoverButton themeButton;
    std::unique_ptr<juce::Drawable> cueWordmark;
    juce::Colour wordmarkColour { 0xfff2e7da };
};

class WaveformDisplayComponent final : public juce::Component,
                                        public juce::ChangeListener,
                                        public juce::FileDragAndDropTarget,
                                        public juce::SettableTooltipClient,
                                        private juce::ScrollBar::Listener,
                                        private juce::Timer
{
public:
    WaveformDisplayComponent (AudioPluginAudioProcessor& p)
        : processor (p),
          horizontalScrollBar (false)
    {
        setTooltip ("Click a chop to select and preview it.  Double-click to toggle favourite (pink highlight).  "
                    "Drag a selected chop edge to resize the grid and update tempo.  "
                    "Shift-drag a selected chop edge to snap that edge to the nearest bar without changing tempo.  "
                    "Drag an audio file here to load it.");
        processor.sampleChangeBroadcaster.addChangeListener (this);
        processor.editChangeBroadcaster.addChangeListener (this);
        lastObservedChopTriggerRevision = processor.getChopTriggerRevision();
        updatePeakCache();
        rebuildWaveformPath();
        animHz = animationFrameRateHz(); // 60/120 Hz for smooth playhead, scroll & zoom
        startTimerHz (animHz);

        verticalMinusButton.setButtonText ("-");
        verticalPlusButton .setButtonText ("+");
        verticalMinusButton.setTooltip ("Decrease waveform height.");
        verticalPlusButton .setTooltip ("Increase waveform height.");
        verticalMinusButton.onClick = [this] { stepWaveformVerticalScale (-0.25f); };
        verticalPlusButton .onClick = [this] { stepWaveformVerticalScale (+0.25f); };
        verticalMinusButton.getProperties().set ("cueStyle", "waveScaleStep");
        verticalPlusButton .getProperties().set ("cueStyle", "waveScaleStep");
        addAndMakeVisible (verticalMinusButton);
        addAndMakeVisible (verticalPlusButton);

        horizontalScrollBar.addListener (this);
        horizontalScrollBar.setAlwaysOnTop (true);
        addAndMakeVisible (horizontalScrollBar);
    }

    ~WaveformDisplayComponent() override
    {
        stopTimer();
        processor.sampleChangeBroadcaster.removeChangeListener (this);
        processor.editChangeBroadcaster.removeChangeListener (this);
        horizontalScrollBar.removeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &processor.sampleChangeBroadcaster)
        {
            isSelectingAnalysisRegion = false;
            targetZoomLevel = zoomLevel = 0.07f;
            targetScrollPosition = scrollPosition = 0.0f;
            targetWaveformVerticalScale = waveformVerticalScale = defaultWaveformVerticalScale;
            updatePeakCache();
            rebuildWaveformPath();
            updateHorizontalScrollBar();

            // Kick the load reveal for genuinely new material only — a stem
            // remix rebroadcasts with the same file identity and must not
            // replay the animation.
            juce::String sampleId;
            if (const auto s = processor.getLoadedSample())
                sampleId = s->filePath + ":" + juce::String (s->buffer.getNumSamples())
                         + ":" + juce::String (s->sampleRate);

            if (sampleId.isNotEmpty() && sampleId != loadAnimSampleId)
            {
                loadAnimSampleId = sampleId;
                loadAnimPhase = 0.0f;
            }
        }
        repaint();
    }

    juce::String getTooltip() override
    {
        if (exportButtonHovered)
            return "Drag into your DAW to export this chop as audio, or click to save it to disk.";
        return juce::SettableTooltipClient::getTooltip();
    }

    // ---- FileDragAndDropTarget ----
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (exportDragFired)
            return false;

        for (const auto& f : files)
        {
            auto ext = juce::File (f).getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".aif" || ext == ".aiff"
                || ext == ".mp3" || ext == ".flac" || ext == ".ogg")
                return true;
        }
        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override
    {
        isDragOver = true;
        repaint();
    }

    void fileDragExit (const juce::StringArray&) override
    {
        isDragOver = false;
        repaint();
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        isDragOver = false;
        if (! files.isEmpty())
            processor.loadAudioFile (juce::File (files[0]));
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        updateHoverState (event.position);
        updateCursorForMode (event.position);
    }

    void mouseEnter (const juce::MouseEvent& event) override
    {
        updateHoverState (event.position);
        updateCursorForMode (event.position);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        isHoveringDisplay = false;
        hoveredChopId = -1;
        if (edgeDragChopId < 0)
            edgeHoverKind = 0;
        if (exportButtonHovered)
        {
            exportButtonHovered = false;
            repaint();
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! isPositionInsideDisplay (event.position))
            return;

        // Floating export button takes priority over selecting/auditioning the
        // chop underneath it. We arm here; mouseDrag vs mouseUp decides whether
        // this becomes a drag-into-DAW or a click-to-save.
        if (hitTestExportButton (event.position))
        {
            exportButtonPressed   = true;
            exportButtonDragArmed = false;
            exportButtonPressPos  = event.position;
            repaint();
            return;
        }

        if (processor.isWarpModeActive())
        {
            handleWarpMouseDown (event);
            return;
        }

        // Edge-resize takes priority over normal click-to-select. Only the
        // currently selected chop is draggable, so this never overrides a
        // first click on a different chop.
        {
            int hitKind = 0;
            int hitChopId = -1;
            if (hitTestSelectedChopEdge (event.position, hitKind, hitChopId))
            {
                edgeDragChopId    = hitChopId;
                edgeDragKind      = hitKind;
                edgeDragBarSnap   = event.mods.isShiftDown();
                edgeDragLiveSample = sampleForDisplayPosition (event.position.x);
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                repaint();
                return;
            }
        }

        const auto targetSample = sampleForDisplayPosition (event.position.x);
        processor.selectChopAtSample (targetSample);
        isHoldingToPlay = true;
        processor.startPlayback();
        updateHoverState (event.position);

        // Begin hold-for-export detection
        holdChopId     = -1;
        holdTickCount  = 0;
        exportDragReady = false;
        exportDragFired = false;

        if (const auto cs = processor.getChopState())
        {
            for (const auto& c : cs->chops)
            {
                if (targetSample >= (double) c.startSample && targetSample < (double) c.endSample)
                {
                    holdChopId = c.id;
                    break;
                }
            }
        }
    }

    void updateCursorForMode (juce::Point<float> pos)
    {
        // The floating export button owns the cursor when hovered.
        const bool overExport = hitTestExportButton (pos);
        if (overExport != exportButtonHovered)
        {
            exportButtonHovered = overExport;
            repaint();
        }
        if (overExport)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            return;
        }

        if (! isPositionInsideDisplay (pos))
        {
            edgeHoverKind = 0;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }

        if (processor.isWarpModeActive())
        {
            edgeHoverKind = 0;
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            return;
        }

        // Refresh the hover-edge probe so the resize cursor follows the
        // pointer as it moves near the selected chop's edges.
        if (edgeDragChopId < 0)
        {
            int hitKind = 0;
            int hitChopId = -1;
            edgeHoverKind = hitTestSelectedChopEdge (pos, hitKind, hitChopId) ? hitKind : 0;
        }

        if (edgeDragChopId >= 0 || edgeHoverKind != 0)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    // Tests the cursor against the selected chop's left and right edges.
    // Sets edgeKind to 1 (left) or 2 (right) and returns true if within
    // kEdgeHitTestPixels of either edge of the currently selected chop.
    bool hitTestSelectedChopEdge (juce::Point<float> mousePos, int& edgeKind, int& chopId) const
    {
        edgeKind = 0;
        chopId   = -1;

        if (! isPositionInsideDisplay (mousePos))
            return false;

        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr || chopState->selectedChopId < 0)
            return false;

        const AudioPluginAudioProcessor::ChopDefinition* selected = nullptr;
        for (const auto& c : chopState->chops)
        {
            if (c.id == chopState->selectedChopId)
            {
                selected = &c;
                break;
            }
        }
        if (selected == nullptr)
            return false;

        const auto displayBounds = getDisplayBounds();
        const auto visibleRange  = getVisibleRange (sampleData->buffer.getNumSamples());
        const float startX = displayXForSamplePosition ((double) selected->startSample, visibleRange, displayBounds);
        const float endX   = displayXForSamplePosition ((double) selected->endSample,   visibleRange, displayBounds);

        const float dxLeft  = std::abs (mousePos.x - startX);
        const float dxRight = std::abs (mousePos.x - endX);

        if (dxLeft <= kEdgeHitTestPixels && dxLeft <= dxRight)
        {
            edgeKind = 1;
            chopId   = selected->id;
            return true;
        }
        if (dxRight <= kEdgeHitTestPixels)
        {
            edgeKind = 2;
            chopId   = selected->id;
            return true;
        }
        return false;
    }

    void updateEdgeDragLiveSample (juce::Point<float> mousePos)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const AudioPluginAudioProcessor::ChopDefinition* chop = nullptr;
        for (const auto& c : chopState->chops)
            if (c.id == edgeDragChopId) { chop = &c; break; }

        if (chop == nullptr)
        {
            edgeDragChopId = -1;
            edgeDragKind   = 0;
            edgeDragBarSnap = false;
            return;
        }

        const int    totalSamples = sampleData->buffer.getNumSamples();
        const double sr           = sampleData->sampleRate > 0.0 ? sampleData->sampleRate : 48000.0;
        const int    minLen       = juce::jmax (1, (int) std::round (sr * 0.05)); // 50 ms minimum chop

        double newSample = sampleForDisplayPosition (mousePos.x);

        if (edgeDragBarSnap)
            newSample = sampleForNearestBarLine (newSample);

        if (edgeDragKind == 1) // left edge
        {
            const double maxStart = (double) juce::jmax (0, chop->endSample - minLen);
            newSample = juce::jlimit (0.0, maxStart, newSample);
        }
        else if (edgeDragKind == 2) // right edge
        {
            const double minEnd = (double) juce::jmin (totalSamples, chop->startSample + minLen);
            newSample = juce::jlimit (minEnd, (double) totalSamples, newSample);
        }

        edgeDragLiveSample = newSample;
        repaint();
    }

    void commitEdgeDrag()
    {
        if (edgeDragChopId < 0)
            return;

        const auto chopState = processor.getChopState();
        const AudioPluginAudioProcessor::ChopDefinition* chop = nullptr;
        if (chopState != nullptr)
            for (const auto& c : chopState->chops)
                if (c.id == edgeDragChopId) { chop = &c; break; }

        if (chop != nullptr)
        {
            int newStart = chop->startSample;
            int newEnd   = chop->endSample;
            const int dragSample = (int) std::round (edgeDragLiveSample);

            if      (edgeDragKind == 1) newStart = dragSample;
            else if (edgeDragKind == 2) newEnd   = dragSample;

            if (newEnd - newStart >= 2)
            {
                if (edgeDragBarSnap)
                    processor.setChopBounds (edgeDragChopId, newStart, newEnd);
                else
                    processor.resizeChopBoundary (edgeDragChopId, newStart, newEnd);
            }
        }

        edgeDragChopId     = -1;
        edgeDragKind       = 0;
        edgeDragLiveSample = 0.0;
        edgeDragBarSnap = false;
    }

    // ---- Warp-mode interaction helpers (step 9) ---------------------------

    // Snaps a clip-local time to the nearest grid division (or returns the raw
    // value if snap is disabled). Clamped into the open interval (0, chopDur).
    double snapLocalTime (double rawLocalTime, double chopDurationSec, bool useSnap) const
    {
        const double minLocal = 1.0 / juce::jmax (1.0, processor.getLoadedSample()
                                                            ? processor.getLoadedSample()->sampleRate
                                                            : 48000.0);
        const double maxLocal = chopDurationSec - minLocal;

        double t = rawLocalTime;
        if (useSnap)
        {
            const double divisionSec = processor.getWarpDivisionSeconds();
            if (divisionSec > 0.0)
                t = std::round (rawLocalTime / divisionSec) * divisionSec;
        }
        return juce::jlimit (minLocal, maxLocal, t);
    }

    // Hit-tests warp marker LINES (rendered at local-time-x along chop width).
    // Sets warpDragChopId / warpDragMarkerIndex if a marker line is within
    // kWarpHitTestPixels of the cursor. Returns true if a marker was hit.
    bool hitTestWarpMarker (juce::Point<float> mousePos)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return false;

        const auto sr = sampleData->sampleRate;
        if (sr <= 0.0)
            return false;

        const auto displayBounds = getDisplayBounds();
        const auto visibleRange  = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto cursorSample  = sampleForDisplayPosition (mousePos.x);

        for (const auto& chop : chopState->chops)
        {
            if (cursorSample < (double) chop.startSample || cursorSample >= (double) chop.endSample)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX   = displayXForSamplePosition ((double) chop.endSample,   visibleRange, displayBounds);
            const float chopWidth = std::abs (endX - startX);
            const float chopLeft  = juce::jmin (startX, endX);
            if (chopWidth <= 0.0f)
                return false;

            const double chopDurationSec = (double) (chop.endSample - chop.startSample) / sr;
            if (chopDurationSec <= 0.0)
                return false;

            for (size_t i = 0; i < chop.warpMarkers.size(); ++i)
            {
                const double localFrac = juce::jlimit (0.0, 1.0,
                    chop.warpMarkers[i].localTimeSeconds / chopDurationSec);
                const float markerX = chopLeft + (float) localFrac * chopWidth;
                if (std::abs (mousePos.x - markerX) <= kWarpHitTestPixels)
                {
                    warpDragChopId      = chop.id;
                    warpDragMarkerIndex = (int) i;
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    void handleWarpMouseDown (const juce::MouseEvent& event)
    {
        warpDragChopId      = -1;
        warpDragMarkerIndex = -1;
        warpDragRetarget    = event.mods.isCommandDown() || event.mods.isCtrlDown();
        warpDragSnap        = ! event.mods.isShiftDown();

        // Right-click in warp mode shows a per-marker context menu when the
        // click lands on an existing marker. No menu, no action otherwise.
        if (event.mods.isPopupMenu())
        {
            if (hitTestWarpMarker (event.position))
            {
                const int chopId      = warpDragChopId;
                const int markerIndex = warpDragMarkerIndex;
                // Reset drag state — we don't want a subsequent drag to act on
                // the menu click.
                warpDragChopId      = -1;
                warpDragMarkerIndex = -1;
                showWarpMarkerContextMenu (chopId, markerIndex);
            }
            return;
        }

        if (hitTestWarpMarker (event.position))
            return; // existing marker — drag begins on next mouseDrag

        // No marker hit → drop a new one. The cursor x within the chop is the
        // PLAYBACK-time axis (local-time-x), so we derive localTime from it
        // and then ask the warp map "what source sample plays at that local
        // time?" — that's the audio under the cursor in the visually warped
        // view, which is what the user expects to pin.
        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const double sampleRate = sampleData->sampleRate;
        if (sampleRate <= 0.0)
            return;

        const auto cursorSample = sampleForDisplayPosition (event.position.x);
        const AudioPluginAudioProcessor::ChopDefinition* targetChop = nullptr;
        for (const auto& c : chopState->chops)
        {
            if (cursorSample >= (double) c.startSample && cursorSample < (double) c.endSample)
            {
                targetChop = &c;
                break;
            }
        }
        if (targetChop == nullptr)
            return;

        const double chopDurationSec = (double) (targetChop->endSample - targetChop->startSample) / sampleRate;
        if (chopDurationSec <= 0.0)
            return;

        const auto displayBounds = getDisplayBounds();
        const auto visibleRange  = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto chopStartX    = displayXForSamplePosition ((double) targetChop->startSample, visibleRange, displayBounds);
        const auto chopEndX      = displayXForSamplePosition ((double) targetChop->endSample,   visibleRange, displayBounds);
        const float chopWidth    = chopEndX - chopStartX;
        if (chopWidth <= 0.0f)
            return;

        const double frac = juce::jlimit (0.0f, 1.0f, (event.position.x - chopStartX) / chopWidth);
        const double rawLocal    = frac * chopDurationSec;
        const double targetLocal = snapLocalTime (rawLocal, chopDurationSec, warpDragSnap);

        // Pin sourceSample to the audio that visually sits at the cursor's
        // local-time position — query the warp map's forward lookup.
        cuesampler::WarpMap warpMap;
        warpMap.build (targetChop->startSample, targetChop->endSample, targetChop->warpMarkers, sampleRate);
        const double pinSrc = warpMap.sourceSampleAtLocalTime (rawLocal);
        const int sourceSample = juce::jlimit (targetChop->startSample + 1,
                                                targetChop->endSample - 1,
                                                (int) std::round (pinSrc));

        const int markerIndex = processor.addOrUpdateChopWarpMarker (
            targetChop->id, sourceSample, targetLocal, warpDragSnap);

        if (markerIndex >= 0)
        {
            warpDragChopId      = targetChop->id;
            warpDragMarkerIndex = markerIndex;
        }
    }

    void showWarpMarkerContextMenu (int chopId, int markerIndex)
    {
        if (chopId < 0 || markerIndex < 0)
            return;

        enum MenuId : int
        {
            ItemClear        = 1,
            ItemSnapBar,
            ItemSnapBeat,
            ItemSnapHalfBeat,
            ItemSnapSixteenth
        };

        juce::PopupMenu menu;
        menu.addItem (ItemClear, "Clear marker");
        menu.addSeparator();
        menu.addSectionHeader ("Snap to nearest");
        menu.addItem (ItemSnapBar,       "Bar");
        menu.addItem (ItemSnapBeat,      "Beat");
        menu.addItem (ItemSnapHalfBeat,  "1/2 beat");
        menu.addItem (ItemSnapSixteenth, "1/16 beat");

        juce::PopupMenu::Options options;
        options = options.withTargetComponent (this);

        menu.setLookAndFeel (&getLookAndFeel());

        // Capture by value — async callback must outlive this stack frame.
        menu.showMenuAsync (options,
                            [this, chopId, markerIndex] (int result)
                            {
                                switch (result)
                                {
                                    case 0: return; // user dismissed
                                    case ItemClear:
                                        processor.removeChopWarpMarker (chopId, markerIndex);
                                        break;
                                    case ItemSnapBar:
                                        processor.snapChopWarpMarkerToDivision (chopId, markerIndex,
                                            AudioPluginAudioProcessor::WarpDivision_Bar);
                                        break;
                                    case ItemSnapBeat:
                                        processor.snapChopWarpMarkerToDivision (chopId, markerIndex,
                                            AudioPluginAudioProcessor::WarpDivision_Beat);
                                        break;
                                    case ItemSnapHalfBeat:
                                        processor.snapChopWarpMarkerToDivision (chopId, markerIndex,
                                            AudioPluginAudioProcessor::WarpDivision_HalfBeat);
                                        break;
                                    case ItemSnapSixteenth:
                                        processor.snapChopWarpMarkerToDivision (chopId, markerIndex,
                                            AudioPluginAudioProcessor::WarpDivision_Sixteenth);
                                        break;
                                    default: break;
                                }
                            });
    }

    void handleWarpMouseDrag (const juce::MouseEvent& event)
    {
        // Re-evaluate modifiers each frame so the user can change strategy
        // mid-drag.
        warpDragRetarget = event.mods.isCommandDown() || event.mods.isCtrlDown();
        warpDragSnap     = ! event.mods.isShiftDown();

        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const double sampleRate = sampleData->sampleRate;
        if (sampleRate <= 0.0)
            return;

        const AudioPluginAudioProcessor::ChopDefinition* targetChop = nullptr;
        for (const auto& c : chopState->chops)
        {
            if (c.id == warpDragChopId)
            {
                targetChop = &c;
                break;
            }
        }
        if (targetChop == nullptr || warpDragMarkerIndex < 0
            || warpDragMarkerIndex >= (int) targetChop->warpMarkers.size())
            return;

        const double chopDurationSec = (double) (targetChop->endSample - targetChop->startSample) / sampleRate;
        if (chopDurationSec <= 0.0)
            return;

        const auto cursorSample  = sampleForDisplayPosition (event.position.x);
        const auto displayBounds = getDisplayBounds();
        const auto visibleRange  = getVisibleRange (sampleData->buffer.getNumSamples());

        if (warpDragRetarget)
        {
            // Cmd/Ctrl-drag: re-pin to a new source position; local time stays.
            const int newSource = juce::jlimit (targetChop->startSample + 1,
                                                targetChop->endSample - 1,
                                                (int) std::round (cursorSample));
            processor.setChopWarpMarkerSourceSample (warpDragChopId, warpDragMarkerIndex, newSource);
            return;
        }

        // Default drag: cursor x along the chop width is the local-time axis.
        const auto chopStartX = displayXForSamplePosition ((double) targetChop->startSample,
                                                            visibleRange, displayBounds);
        const auto chopEndX   = displayXForSamplePosition ((double) targetChop->endSample,
                                                            visibleRange, displayBounds);
        const float chopWidth = chopEndX - chopStartX;
        if (chopWidth <= 0.0f)
            return;

        const double frac = juce::jlimit (0.0f, 1.0f, (event.position.x - chopStartX) / chopWidth);
        const double rawLocal = frac * chopDurationSec;
        const double targetLocal = snapLocalTime (rawLocal, chopDurationSec, warpDragSnap);

        processor.setChopWarpMarkerLocalTime (warpDragChopId, warpDragMarkerIndex,
                                              targetLocal, warpDragSnap);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        // Dragging the floating export button arms the OS file drag immediately
        // (no 2 s wait). A tiny threshold distinguishes a drag from a click.
        if (exportButtonPressed)
        {
            if (! exportButtonDragArmed
                && event.position.getDistanceFrom (exportButtonPressPos) > 4.0f)
            {
                exportButtonDragArmed = true;
                const int chopId = exportTargetChopId;
                exportButtonPressed = false;
                if (chopId >= 0)
                {
                    exportDragFired = true; // suppress our own FileDragAndDropTarget
                    initiateChopExportDrag (chopId);
                    exportDragFired = false;
                }
                repaint();
            }
            return;
        }

        if (processor.isWarpModeActive() && warpDragChopId >= 0 && warpDragMarkerIndex >= 0)
        {
            handleWarpMouseDrag (event);
            return;
        }

        if (edgeDragChopId >= 0)
        {
            updateEdgeDragLiveSample (event.position);
            return;
        }

        // Trigger OS file drag when the 2-second hold is ready
        if (exportDragReady && holdChopId >= 0 && ! exportDragFired)
        {
            exportDragFired = true;
            initiateChopExportDrag (holdChopId);
            return;
        }

    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        // Released on the export button without dragging => click-to-save.
        if (exportButtonPressed)
        {
            exportButtonPressed = false;
            if (! exportButtonDragArmed && hitTestExportButton (event.position))
            {
                if (exportTargetChopId >= 0)
                    saveChopToFile (exportTargetChopId);
            }
            exportButtonDragArmed = false;
            repaint();
            return;
        }

        if (warpDragChopId >= 0 || warpDragMarkerIndex >= 0)
        {
            warpDragChopId      = -1;
            warpDragMarkerIndex = -1;
            warpDragRetarget    = false;
            warpDragSnap        = true;
            updateCursorForMode (event.position);
            return;
        }

        if (edgeDragChopId >= 0)
        {
            commitEdgeDrag();
            updateCursorForMode (event.position);
            repaint();
            return;
        }

        if (isHoldingToPlay)
        {
            isHoldingToPlay = false;
            processor.stopPlayback();
        }

        // Reset hold-export state
        holdChopId     = -1;
        holdTickCount  = 0;
        exportDragReady = false;
        exportDragFired = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (! isPositionInsideDisplay (event.position))
            return;

        // Warp mode reserves double-click for marker actions (wired in step 9).
        if (processor.isWarpModeActive())
        {
            if (hitTestWarpMarker (event.position))
            {
                processor.removeChopWarpMarker (warpDragChopId, warpDragMarkerIndex);
                warpDragChopId      = -1;
                warpDragMarkerIndex = -1;
            }
            return;
        }

        // Plain double-click: select chop under cursor and toggle its favorite state
        processor.selectChopAtSample (sampleForDisplayPosition (event.position.x));
        processor.toggleSelectedChopFavorite();
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (! isPositionInsideDisplay (event.position))
            return;

        if (wheel.deltaY != 0.0f)
        {
            const float newZoom = juce::jlimit (0.0f, 1.0f, zoomLevel + wheel.deltaY * 0.08f);

            const auto sampleData = processor.getLoadedSample();
            if (sampleData != nullptr && sampleData->buffer.getNumSamples() > 0)
            {
                const int   numSamples    = sampleData->buffer.getNumSamples();
                const auto  displayBounds = getDisplayBounds();
                const double relPos       = juce::jlimit (0.0, 1.0,
                                                (double) (event.position.x - displayBounds.getX())
                                                / (double) displayBounds.getWidth());
                const double cursorSample = sampleForDisplayPosition (event.position.x);

                const float effectiveZoom = getEffectiveZoomLevel (newZoom);
                const int   newVisible    = juce::jmax (32, (int) ((float) numSamples / std::pow (10000.0f, effectiveZoom)));
                const int   newMaxOffset  = juce::jmax (1, numSamples - newVisible);
                const double newOffset    = cursorSample - relPos * (double) juce::jmax (1, newVisible - 1);
                const float newScroll     = juce::jlimit (0.0f, 1.0f, (float) (newOffset / (double) newMaxOffset));

                setZoom (newZoom);
                setScroll (newScroll);
                // Handled smoothly by timerCallback

                if (onZoomChanged)   onZoomChanged   (newZoom);
                if (onScrollChanged) onScrollChanged (newScroll);
            }
            else
            {
                setZoom (newZoom);
                if (onZoomChanged)
                    onZoomChanged (newZoom);
            }
        }

        if (wheel.deltaX != 0.0f)
        {
            const float newScroll = juce::jlimit (0.0f, 1.0f, scrollPosition - wheel.deltaX * 0.02f);
            setScroll (newScroll);
            if (onScrollChanged)
                onScrollChanged (newScroll);
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        // The panel art was designed at 780x409, but resized() later condensed
        // this component (411 -> 330) to make room for the stem strip. The height
        // must track the component or the panel/frame/display clip top & bottom.
        // Width is unchanged (780 in a 782-wide component); only height-derived
        // values are panelH-relative so the original insets are preserved.
        auto panelBounds = bounds.reduced (1.0f);
        const auto panelH = panelBounds.getHeight();

        fillGlassRounded (g, *this, panelBounds, mediumCorner);

        const auto panelX = panelBounds.getX();
        const auto panelY = panelBounds.getY();
        drawPanelHole (g, { panelX + 13.0f, panelY + 13.0f }, 6.0f);
        drawPanelHole (g, { panelBounds.getRight() - 14.0f, panelY + 13.0f }, 6.0f);
        drawPanelHole (g, { panelX + 13.0f, panelBounds.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { panelBounds.getRight() - 14.0f, panelBounds.getBottom() - 13.0f }, 6.0f);

        // Recessed screen frame: flat CUERACK layering — raised frame tone,
        // then the near-black slot the waveform lives in. No glass sheens.
        auto frameBounds = juce::Rectangle<float> (panelX + 16.0f, panelY + 16.0f,
                                                   panelBounds.getWidth() - 32.0f, panelH - 32.0f);
        g.setColour (panelInnerDark);
        g.fillRoundedRectangle (frameBounds.reduced (0.5f), 3.0f);
        g.setColour (borderMid);
        g.drawRoundedRectangle (frameBounds.reduced (0.5f), 3.0f, 1.0f);

        auto displayBounds = juce::Rectangle<float> (panelX + 22.0f, panelY + 22.0f,
                                                     panelBounds.getWidth() - 44.0f, panelH - 46.0f);
        g.setColour (blackPanel);
        g.fillRoundedRectangle (displayBounds.reduced (0.5f), 2.0f);
        g.setColour (borderDark);
        g.drawRoundedRectangle (displayBounds.reduced (0.5f), 2.0f, 1.0f);

        // ---- Waveform or placeholder ----
        const bool  loadReveal = isLoadRevealActive() && ! waveformPath.isEmpty();
        const bool  loadSweeping = loadReveal && loadAnimPhase < 1.0f;
        const float loadSweepX = loadReveal ? loadRevealSweepX (displayBounds) : 0.0f;

        if (! waveformPath.isEmpty())
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clipPath;
            clipPath.addRoundedRectangle (displayBounds.reduced (4.0f), 4.0f);
            g.reduceClipRegion (clipPath);

            // During the load reveal only the swept region shows signal — the
            // waveform materialises behind the scan head.
            if (loadSweeping)
                g.reduceClipRegion (juce::Rectangle<int> (
                    (int) std::floor (displayBounds.getX()),
                    (int) std::floor (displayBounds.getY()),
                    juce::jmax (0, (int) std::ceil (loadSweepX - displayBounds.getX())),
                    (int) std::ceil (displayBounds.getHeight())));

            // Subtle glow behind waveform (skip during zoom/scroll animations for performance)
            if (! isAnimating)
            {
                g.setColour (glassText.withAlpha (0.06f));
                g.strokePath (waveformPath, juce::PathStrokeType (4.0f));
            }

            // Filled waveform — cream ink, per the CUERACK palette
            g.setColour (glassText.withAlpha (0.55f));
            g.fillPath (waveformPath);

            // Bright outline
            g.setColour (glassText.withAlpha (0.85f));
            g.strokePath (waveformPath, juce::PathStrokeType (1.0f));

            // Centre line
            auto centreY = displayBounds.getCentreY();
            g.setColour (glassText.withAlpha (0.15f));
            g.drawHorizontalLine ((int) centreY, displayBounds.getX() + 4.0f, displayBounds.getRight() - 4.0f);

        }
        else
        {
            auto messageBounds = displayBounds.toNearestInt().reduced (68, 132).withSizeKeepingCentre (560, 96);
            g.setColour (panelDark.withAlpha (0.72f));
            g.fillRoundedRectangle (messageBounds.toFloat(), 2.0f);
            g.setColour (borderMid.withAlpha (0.6f));
            g.drawRoundedRectangle (messageBounds.toFloat().reduced (0.5f), 2.0f, 1.0f);
            g.setColour (textMuted.brighter (0.22f).withAlpha (0.92f));
            g.setFont (monoFont (11.5f));
            g.drawFittedText ("DRAG AN AUDIO FILE HERE - OR PRESS LOAD SAMPLE IN THE TRANSPORT BELOW",
                              messageBounds.reduced (18, 12), juce::Justification::centred, 2);
        }

        // ---- One-shot load reveal: "trace acquire" sweep --------------------
        if (loadReveal)
        {
            juce::Graphics::ScopedSaveState revealState (g);
            juce::Path revealClip;
            revealClip.addRoundedRectangle (displayBounds.reduced (4.0f), 4.0f);
            g.reduceClipRegion (revealClip);

            const auto inner = displayBounds.reduced (4.0f);
            const auto centreY = displayBounds.getCentreY();

            if (loadSweeping)
            {
                // Idle trace ahead of the head — the scope has no signal yet.
                g.setColour (glassText.withAlpha (0.30f));
                g.drawLine (loadSweepX, centreY, inner.getRight(), centreY, 1.2f);

                // Trailing glow behind the head.
                const float glowW = 70.0f;
                juce::ColourGradient trail (glassText.withAlpha (0.0f),  loadSweepX - glowW, centreY,
                                            glassText.withAlpha (0.22f), loadSweepX,         centreY, false);
                g.setGradientFill (trail);
                g.fillRect (loadSweepX - glowW, inner.getY(), glowW, inner.getHeight());

                // Bright cream head with an accent core.
                g.setColour (glassText.withAlpha (0.95f));
                g.fillRect (loadSweepX - 1.5f, inner.getY(), 3.0f, inner.getHeight());
                g.setColour (accentOrange.withAlpha (0.85f));
                g.fillRect (loadSweepX - 0.5f, inner.getY(), 1.0f, inner.getHeight());

                // Flickering data ticks just behind the head.
                const float t = juce::jlimit (0.0f, 1.0f, loadAnimPhase);
                for (int i = 0; i < 3; ++i)
                {
                    const float off = 12.0f + 26.0f * (float) i
                                      + 6.0f * std::sin (t * 40.0f + (float) i * 2.1f);
                    g.setColour (accentOrange.withAlpha (juce::jmax (0.0f, 0.30f - 0.09f * (float) i)));
                    g.fillRect (loadSweepX - off, inner.getY(), 1.0f, inner.getHeight());
                }
            }
            else
            {
                // Lock flash: quick cream pop that decays once the sweep lands.
                const float flash = juce::jmax (0.0f, 1.0f - (loadAnimPhase - 1.0f) * 4.0f);
                g.setColour (glassText.withAlpha (0.12f * flash));
                g.fillRect (inner);
            }
        }

        // ---- BPM scan animation (waits for the load reveal to finish) ----
        if (processor.isTempoAnalysisInProgress() && ! loadReveal)
        {
            juce::Graphics::ScopedSaveState scanState (g);
            juce::Path scanClip;
            scanClip.addRoundedRectangle (displayBounds.reduced (4.0f), 4.0f);
            g.reduceClipRegion (scanClip);

            const auto inner = displayBounds.reduced (4.0f);

            // Subtle accent tint over entire display
            g.setColour (accentOrange.withAlpha (0.04f));
            g.fillRect (inner);

            // Sweep line: full left-to-right cycle over 120 frames (2 s at 60 fps)
            const float cyclePeriod = 120.0f;
            const float phase       = std::fmod ((float) scanAnimFrame, cyclePeriod) / cyclePeriod;
            const float lineX       = inner.getX() + phase * inner.getWidth();

            // Trailing glow gradient
            const float glowWidth = 60.0f;
            juce::ColourGradient trailGrad (accentOrange.withAlpha (0.0f),  lineX - glowWidth, inner.getCentreY(),
                                            accentOrange.withAlpha (0.18f), lineX,              inner.getCentreY(), false);
            g.setGradientFill (trailGrad);
            g.fillRect (lineX - glowWidth, inner.getY(), glowWidth, inner.getHeight());

            // Bright leading edge
            g.setColour (accentOrange.withAlpha (0.75f));
            g.drawVerticalLine (juce::roundToInt (lineX), inner.getY(), inner.getBottom());

            // Soft glow ahead of the line
            juce::ColourGradient leadGrad (accentOrange.withAlpha (0.12f), lineX,             inner.getCentreY(),
                                           accentOrange.withAlpha (0.0f),  lineX + 20.0f,     inner.getCentreY(), false);
            g.setGradientFill (leadGrad);
            g.fillRect (lineX, inner.getY(), 20.0f, inner.getHeight());
        }

        const auto waveformBounds = getDisplayBounds();
        paintWarpedChopAudio (g, waveformBounds);
        paintChops (g, waveformBounds);
        paintWarpMarkers (g, waveformBounds);
        paintTempoGrid (g, waveformBounds);
        paintHoverGuide (g, waveformBounds);
        paintEdgeDragGhost (g, waveformBounds);
        paintPlayhead (g, waveformBounds);

        // ---- Hold-to-export loading animation ----
        if (holdChopId >= 0 && ! exportDragReady && holdTickCount > 0)
        {
            if (const auto cs = processor.getChopState())
            {
                for (const auto& c : cs->chops)
                {
                    if (c.id != holdChopId)
                        continue;

                    const auto numSamples = [&]() -> int
                    {
                        if (const auto s = processor.getLoadedSample())
                            return s->buffer.getNumSamples();
                        return 0;
                    }();
                    if (numSamples <= 0)
                        break;

                    const auto visRange = getVisibleRange (numSamples);
                    const auto startX   = displayXForSamplePosition ((double) c.startSample, visRange, waveformBounds);
                    const auto endX     = displayXForSamplePosition ((double) c.endSample,   visRange, waveformBounds);
                    auto chopRect = juce::Rectangle<float> (
                        std::min (startX, endX), waveformBounds.getY(),
                        std::abs (endX - startX), waveformBounds.getHeight());

                    const float progress = (float) holdTickCount / (float) holdTicksRequired();
                    const juce::Colour amber (0xffffb300);

                    juce::Graphics::ScopedSaveState ss (g);
                    juce::Path clip;
                    clip.addRoundedRectangle (waveformBounds, 4.0f);
                    g.reduceClipRegion (clip);

                    // Amber fill deepening with progress
                    g.setColour (amber.withAlpha (0.05f + 0.20f * progress));
                    g.fillRect (chopRect);

                    // Circular progress ring
                    const float cx     = chopRect.getCentreX();
                    const float cy     = chopRect.getCentreY();
                    const float radius = juce::jlimit (8.0f, 18.0f,
                                                       juce::jmin (chopRect.getWidth() * 0.35f,
                                                                   chopRect.getHeight() * 0.32f));
                    const float ringW  = juce::jmax (1.5f, radius * 0.22f);

                    // Background ring
                    g.setColour (juce::Colours::black.withAlpha (0.40f));
                    g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, ringW);

                    // Sweeping amber arc (clockwise from 12 o'clock)
                    juce::Path arc;
                    const float startAngle = -juce::MathConstants<float>::halfPi;
                    const float endAngle   = startAngle + progress * juce::MathConstants<float>::twoPi;
                    arc.addArc (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f,
                                startAngle, endAngle, true);
                    juce::PathStrokeType strokeType (ringW,
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded);
                    g.setColour (amber.withAlpha (0.92f));
                    g.strokePath (arc, strokeType);

                    break;
                }
            }
        }

        // ---- Export-ready amber overlay ----
        if (exportDragReady && holdChopId >= 0)
        {
            if (const auto cs = processor.getChopState())
            {
                for (const auto& c : cs->chops)
                {
                    if (c.id != holdChopId)
                        continue;

                    const auto numSamples = [&]() -> int
                    {
                        if (const auto s = processor.getLoadedSample())
                            return s->buffer.getNumSamples();
                        return 0;
                    }();
                    if (numSamples <= 0)
                        break;

                    const auto visRange = getVisibleRange (numSamples);
                    const auto startX   = displayXForSamplePosition ((double) c.startSample, visRange, waveformBounds);
                    const auto endX     = displayXForSamplePosition ((double) c.endSample,   visRange, waveformBounds);
                    auto chopRect = juce::Rectangle<float> (
                        std::min (startX, endX), waveformBounds.getY(),
                        std::abs (endX - startX), waveformBounds.getHeight());

                    juce::Graphics::ScopedSaveState ss (g);
                    juce::Path clip;
                    clip.addRoundedRectangle (waveformBounds, 4.0f);
                    g.reduceClipRegion (clip);

                    g.setColour (juce::Colour (0xffffb300).withAlpha (0.28f));
                    g.fillRect (chopRect);
                    g.setColour (juce::Colour (0xffffb300).withAlpha (0.85f));
                    g.drawRect (chopRect.expanded (0.0f, 0.0f), 2.0f);

                    if (chopRect.getWidth() > 50.0f)
                    {
                        g.setColour (juce::Colour (0xffffb300));
                        g.setFont (monoFont (13.0f));
                        g.drawFittedText ("DRAG TO EXPORT",
                                          chopRect.withSizeKeepingCentre (chopRect.getWidth() - 8.0f, 22.0f).toNearestInt(),
                                          juce::Justification::centred, 1);
                    }
                    break;
                }
            }
        }

        // ---- Drag-over highlight ----
        if (isDragOver)
        {
            g.setColour (accentOrange.withAlpha (0.12f));
            g.fillRoundedRectangle (displayBounds, 4.0f);
            g.setColour (accentOrange.withAlpha (0.5f));
            g.drawRoundedRectangle (displayBounds.reduced (2.0f), 3.0f, 2.0f);
        }

        // ---- Floating export affordance (drawn on top) ----
        paintExportButton (g);
    }

    void resized() override
    {
        rebuildWaveformPath();

        // Top-right corner of the display: two 28x28 buttons.
        const auto display = getDisplayBounds().toNearestInt();
        constexpr int btnSize = 28;
        constexpr int btnGap  = 4;
        const int yTop = display.getY() + 6;
        const int xPlus  = display.getRight() - 6 - btnSize;
        const int xMinus = xPlus - btnGap - btnSize;
        verticalMinusButton.setBounds (xMinus, yTop, btnSize, btnSize);
        verticalPlusButton .setBounds (xPlus,  yTop, btnSize, btnSize);

        horizontalScrollBar.setBounds (getHorizontalScrollBarBounds().toNearestInt());
        updateHorizontalScrollBar();
    }

    void setZoom (float newZoom)
    {
        targetZoomLevel = juce::jlimit (0.0f, 1.0f, newZoom);
        if (! isShowing())
            zoomLevel = targetZoomLevel;
    }

    void setScroll (float newScroll)
    {
        targetScrollPosition = juce::jlimit (0.0f, 1.0f, newScroll);
        if (! isShowing())
            scrollPosition = targetScrollPosition;
    }

    std::function<void(float)> onZoomChanged;
    std::function<void(float)> onScrollChanged;

private:
    juce::Rectangle<float> getHorizontalScrollBarBounds() const
    {
        auto display = getDisplayBounds();
        // Sits beautifully at the bottom of the display bounds
        return { display.getX() + 4.0f, display.getBottom() - 14.0f, display.getWidth() - 8.0f, 10.0f };
    }

    void updateHorizontalScrollBar()
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() <= 0)
        {
            horizontalScrollBar.setVisible (false);
            return;
        }

        const auto numSamples = sampleData->buffer.getNumSamples();
        const auto visibleRange = getVisibleRange (numSamples);
        const double visibleProportion = juce::jlimit (0.02, 1.0,
            (double) visibleRange.visibleSamples / (double) numSamples);

        const bool shouldBeVisible = visibleProportion < 0.99;
        horizontalScrollBar.setVisible (shouldBeVisible);

        if (shouldBeVisible)
        {
            // dontSendNotification is load-bearing: the default async
            // notification fires after the guard below is cleared, so the
            // smoothed (lagging) position would echo back through
            // scrollBarMoved and fight the SCROLL knob mid-drag.
            updatingHorizontalScrollBar = true;
            const double maxRangeStart = juce::jmax (0.0, 1.0 - visibleProportion);
            const double currentRangeStart = scrollPosition * maxRangeStart;
            horizontalScrollBar.setRangeLimits (0.0, 1.0, juce::dontSendNotification);
            horizontalScrollBar.setCurrentRange (currentRangeStart, visibleProportion, juce::dontSendNotification);
            updatingHorizontalScrollBar = false;
        }
    }

    void stepWaveformVerticalScale (float delta)
    {
        const float next = juce::jlimit (0.25f, 4.0f, targetWaveformVerticalScale + delta);
        if (std::abs (next - targetWaveformVerticalScale) < 1.0e-4f)
            return;
        targetWaveformVerticalScale = next;
        if (! isShowing())
            waveformVerticalScale = targetWaveformVerticalScale;
    }

    void initiateChopExportDrag (int chopId)
    {
        const bool applySync = processor.getSyncToHost();
        const auto tempFile  = processor.renderChopToTempWav (chopId, applySync);

        if (! tempFile.existsAsFile())
        {
            exportDragReady = false;
            exportDragFired = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }

        const auto path = tempFile.getFullPathName();
        const bool dragStarted = juce::DragAndDropContainer::performExternalDragDropOfFiles (
            juce::StringArray { path },
            /*canMoveFiles=*/ false,
            /*sourceComponent=*/ this,
            /*callback=*/ [path]()
            {
                juce::Timer::callAfterDelay (60000, [path]() { juce::File (path).deleteFile(); });
            });

        if (! dragStarted)
            juce::File (path).deleteFile();

        exportDragReady = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    // ---- Floating export button (drag handle + click-to-save) ----

    // Repoints the button at a chop, replaying the pop-in when the target
    // actually changes. The target is the selected chop or the chop most
    // recently triggered from a MIDI key (see timerCallback).
    void setExportTarget (int chopId)
    {
        if (chopId != exportTargetChopId)
        {
            exportTargetChopId = chopId;
            exportButtonAppear = 0.0f; // replay pop-in for the new chop
        }
    }

    // The button is shown whenever there's a target chop and we're not in the
    // middle of another gesture that owns the display (edge resize, warp,
    // or an in-progress hold-to-export).
    bool isExportButtonActive() const
    {
        if (processor.isWarpModeActive())                            return false;
        if (edgeDragChopId >= 0 || edgeHoverKind != 0)               return false;
        if (holdTickCount > 0 || exportDragReady || exportDragFired)  return false;
        if (exportTargetChopId < 0 || processor.getLoadedSample() == nullptr) return false;

        const auto cs = processor.getChopState();
        if (cs == nullptr)
            return false;
        for (const auto& c : cs->chops)
            if (c.id == exportTargetChopId)
                return true;
        return false;
    }

    // On-screen rect of the floating export button, or an empty rect when it
    // should not be shown. Anchored to the top-centre of the target chop's
    // visible span, clamped inside the display and clear of the +/- buttons.
    juce::Rectangle<float> getExportButtonBounds() const
    {
        if (! isExportButtonActive())
            return {};

        const auto sampleData = processor.getLoadedSample();
        const auto cs         = processor.getChopState();
        if (sampleData == nullptr || cs == nullptr)
            return {};

        const AudioPluginAudioProcessor::ChopDefinition* sel = nullptr;
        for (const auto& c : cs->chops)
            if (c.id == exportTargetChopId) { sel = &c; break; }
        if (sel == nullptr)
            return {};

        const auto display  = getDisplayBounds();
        const auto visRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const float startX = displayXForSamplePosition ((double) sel->startSample, visRange, display);
        const float endX   = displayXForSamplePosition ((double) sel->endSample,   visRange, display);

        const float visL = juce::jmax (startX, display.getX());
        const float visR = juce::jmin (endX,   display.getRight());
        if (visR - visL < 2.0f) // target chop scrolled out of view
            return {};

        constexpr float w = 94.0f;
        constexpr float h = 22.0f;
        const float leftLimit  = display.getX() + 6.0f;
        const float rightLimit = display.getRight() - 70.0f; // keep clear of +/- buttons
        float cx = juce::jlimit (leftLimit + w * 0.5f,
                                 juce::jmax (leftLimit + w * 0.5f, rightLimit - w * 0.5f),
                                 (visL + visR) * 0.5f);
        const float y = display.getY() + 8.0f;
        return juce::Rectangle<float> (cx - w * 0.5f, y, w, h);
    }

    bool hitTestExportButton (juce::Point<float> p) const
    {
        const auto b = getExportButtonBounds();
        return ! b.isEmpty() && b.contains (p);
    }

    // Click (no drag) path: render the chop and offer a Save-As dialog.
    void saveChopToFile (int chopId)
    {
        const bool applySync = processor.getSyncToHost();
        const auto tempFile  = processor.renderChopToTempWav (chopId, applySync);
        if (! tempFile.existsAsFile())
            return;

        auto suggested = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                             .getChildFile (tempFile.getFileName());

        chopExportChooser = std::make_unique<juce::FileChooser> (
            "Export Chop As", suggested, "*.wav");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting;

        chopExportChooser->launchAsync (flags, [tempFile] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest != juce::File())
            {
                dest.deleteFile();
                tempFile.copyFileTo (dest);
            }
            tempFile.deleteFile();
        });
    }

    void paintExportButton (juce::Graphics& g)
    {
        const auto b = getExportButtonBounds();
        if (b.isEmpty())
            return;

        const float appear = juce::jlimit (0.0f, 1.0f, exportButtonAppear);
        if (appear <= 0.01f)
            return;

        const float pulse = 0.5f + 0.5f * std::sin (exportButtonPulse); // 0..1
        const juce::Colour amber (0xffffb300);

        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        clip.addRoundedRectangle (getDisplayBounds().expanded (0.0f, 8.0f), 4.0f);
        g.reduceClipRegion (clip);

        // Pop-in: tight scale 0.9->1.0 plus fade (snappy, minimal travel).
        const float scale = 0.9f + 0.1f * appear;
        auto rect = b.withSizeKeepingCentre (b.getWidth() * scale, b.getHeight() * scale);
        const float corner = rect.getHeight() * 0.5f;

        // Breathing glow behind the pill to draw the eye to it.
        const float glowAlpha = (0.16f + 0.20f * pulse)
                              * appear
                              * (exportButtonHovered ? 1.4f : 1.0f);
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (amber.withAlpha (glowAlpha / (float) (i * 2)));
            g.fillRoundedRectangle (rect.expanded ((float) i * 3.0f), corner + (float) i * 3.0f);
        }

        // Drop shadow.
        g.setColour (juce::Colours::black.withAlpha (0.38f * appear));
        g.fillRoundedRectangle (rect.translated (0.0f, 1.5f), corner);

        // Pill body.
        const juce::Colour base = exportButtonPressed ? amber.darker (0.18f)
                                : exportButtonHovered ? amber.brighter (0.12f)
                                                      : amber;
        juce::ColourGradient grad (base.brighter (0.10f), rect.getX(), rect.getY(),
                                   base.darker (0.18f),   rect.getX(), rect.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (rect, corner);
        g.setColour (juce::Colours::white.withAlpha (0.22f * appear));
        g.drawRoundedRectangle (rect.reduced (0.5f), corner, 1.0f);

        // Content: a download-to-tray glyph + "EXPORT".
        const juce::Colour ink (0xff1a1205);
        g.setColour (ink.withAlpha (appear));

        auto content  = rect.reduced (10.0f, 0.0f);
        auto iconArea = content.removeFromLeft (12.0f);
        const float ax = iconArea.getCentreX();
        const float ay = iconArea.getCentreY() - 1.0f;

        juce::Path arrow;
        arrow.startNewSubPath (ax, ay - 5.0f);        // shaft
        arrow.lineTo          (ax, ay + 2.5f);
        arrow.startNewSubPath (ax - 3.4f, ay - 1.0f); // head
        arrow.lineTo          (ax, ay + 2.5f);
        arrow.lineTo          (ax + 3.4f, ay - 1.0f);
        arrow.startNewSubPath (ax - 4.2f, ay + 5.0f); // tray
        arrow.lineTo          (ax + 4.2f, ay + 5.0f);
        g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));

        g.setFont (monoFont (9.5f).withExtraKerningFactor (0.06f));
        g.drawText ("EXPORT", content, juce::Justification::centred);
    }

    void timerCallback() override
    {
        if (const int hz = animationFrameRateHz(); hz != animHz)
        {
            animHz = hz;
            startTimerHz (animHz); // follow the measured display rate (60/120)
        }

        if (! shouldRunRealtimeUi (*this))
            return;

        if (isHoldingToPlay && ! processor.isPlaying())
            processor.startPlayback();

        // Accumulate hold ticks for drag-export detection
        if (holdChopId >= 0 && ! exportDragFired)
        {
            if (++holdTickCount >= holdTicksRequired() && ! exportDragReady)
            {
                exportDragReady = true;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                repaint();
            }
        }

        // Trigger pulse animation check. A MIDI key press lands here, so we
        // select the struck chop and repoint its export button (it instantly
        // scrolls into view). This keeps MIDI and mouse selection behaviour
        // aligned without doing any UI/state allocation on the audio thread.
        const auto triggerRevision = processor.getChopTriggerRevision();
        if (triggerRevision != lastObservedChopTriggerRevision)
        {
            lastObservedChopTriggerRevision = triggerRevision;
            const int triggeredId = processor.getLastTriggeredChopId();
            if (triggeredId >= 0)
            {
                chopAnimations[triggeredId].currentTriggerPulse = 1.0f; // Strike pulse!
                processor.selectChopById (triggeredId);
                setExportTarget (triggeredId);
            }

            scrollToChop (triggeredId, true); // Scroll exactly and instantly
        }

        // Drive the floating export button: snappy pop-in when the target chop
        // changes plus a gentle continuous breathing pulse so it reads as
        // interactive. Keyed off on-screen visibility so a chop scrolled out of
        // view doesn't keep the UI repainting needlessly. Runs after the
        // trigger block above so a MIDI-struck chop pops in on the same frame.
        {
            const auto cs    = processor.getChopState();
            const int  selId = (cs != nullptr) ? cs->selectedChopId : -1;
            if (selId != lastSeenSelectedId)
            {
                lastSeenSelectedId = selId;
                setExportTarget (selId); // click-select moves the button too
            }

            const bool  visible      = ! getExportButtonBounds().isEmpty();
            const float targetAppear = visible ? 1.0f : 0.0f;
            exportButtonAppear += (targetAppear - exportButtonAppear)
                                * frameRateLerp (0.55f, animHz, waveformRefreshHz);
            if (std::abs (targetAppear - exportButtonAppear) < 0.01f)
                exportButtonAppear = targetAppear;

            if (visible || exportButtonAppear > 0.01f)
                exportButtonPulse += frameRateStep (0.10f, animHz, waveformRefreshHz);
        }

        // Animate scroll, zoom, and vertical scale (warning-free)
        bool zoomChanged = false;
        if (std::abs (zoomLevel - targetZoomLevel) > 0.0001f)
        {
            zoomLevel += (targetZoomLevel - zoomLevel) * frameRateLerp (0.18f, animHz, waveformRefreshHz);
            if (std::abs (zoomLevel - targetZoomLevel) <= 0.0001f)
                zoomLevel = targetZoomLevel;
            zoomChanged = true;
        }

        bool scrollChanged = false;
        if (std::abs (scrollPosition - targetScrollPosition) > 0.0001f)
        {
            scrollPosition += (targetScrollPosition - scrollPosition) * frameRateLerp (0.18f, animHz, waveformRefreshHz);
            if (std::abs (scrollPosition - targetScrollPosition) <= 0.0001f)
                scrollPosition = targetScrollPosition;
            scrollChanged = true;
        }

        bool vertScaleChanged = false;
        if (std::abs (waveformVerticalScale - targetWaveformVerticalScale) > 0.0001f)
        {
            waveformVerticalScale += (targetWaveformVerticalScale - waveformVerticalScale) * frameRateLerp (0.18f, animHz, waveformRefreshHz);
            if (std::abs (waveformVerticalScale - targetWaveformVerticalScale) <= 0.0001f)
                waveformVerticalScale = targetWaveformVerticalScale;
            vertScaleChanged = true;
        }

        isAnimating = zoomChanged || scrollChanged || vertScaleChanged;

        if (isAnimating)
        {
            rebuildWaveformPath (true);
            updateHorizontalScrollBar();
        }
        else if (wasAnimating)
        {
            rebuildWaveformPath (false);
            updateHorizontalScrollBar();
            repaint();
        }
        wasAnimating = isAnimating;

        // Update chop states (hover, selection, trigger decay)
        bool anyChopAnimationActive = false;
        if (const auto cs = processor.getChopState())
        {
            for (const auto& chop : cs->chops)
            {
                auto& anim = chopAnimations[chop.id];
                
                const float targetHover = (chop.id == hoveredChopId) ? 1.0f : 0.0f;
                const float prevHover = anim.currentHoverAlpha;
                anim.currentHoverAlpha += (targetHover - anim.currentHoverAlpha) * frameRateLerp (0.20f, animHz, waveformRefreshHz);
                if (std::abs (anim.currentHoverAlpha - targetHover) < 0.005f)
                    anim.currentHoverAlpha = targetHover;

                const float targetSelect = (chop.id == cs->selectedChopId) ? 1.0f : 0.0f;
                const float prevSelect = anim.currentSelectAlpha;
                anim.currentSelectAlpha += (targetSelect - anim.currentSelectAlpha) * frameRateLerp (0.20f, animHz, waveformRefreshHz);
                if (std::abs (anim.currentSelectAlpha - targetSelect) < 0.005f)
                    anim.currentSelectAlpha = targetSelect;

                const float prevPulse = anim.currentTriggerPulse;
                if (anim.currentTriggerPulse > 0.0f)
                {
                    anim.currentTriggerPulse -= frameRateStep (0.05f, animHz, waveformRefreshHz); // ~0.67 s decay
                    if (anim.currentTriggerPulse < 0.0f)
                        anim.currentTriggerPulse = 0.0f;
                }

                if (std::abs (anim.currentHoverAlpha - prevHover) > 0.005f
                    || std::abs (anim.currentSelectAlpha - prevSelect) > 0.005f
                    || std::abs (anim.currentTriggerPulse - prevPulse) > 0.005f)
                {
                    anyChopAnimationActive = true;
                }
            }
        }

        const auto currentPlayheadPosition = processor.getPlaybackSamplePosition();
        
        const bool isUserInteracting = (edgeDragChopId >= 0)
                                    || (warpDragChopId >= 0)
                                    || isSelectingAnalysisRegion
                                    || isHoldingToPlay
                                    || exportDragReady
                                    || exportDragFired;

        const auto targetHoverAlpha = (isHoveringDisplay && ! isUserInteracting) ? 1.0f : 0.0f;
        hoverAnimationAlpha += (targetHoverAlpha - hoverAnimationAlpha) * frameRateLerp (0.22f, animHz, waveformRefreshHz);
        if (std::abs (targetHoverAlpha - hoverAnimationAlpha) < 0.01f)
            hoverAnimationAlpha = targetHoverAlpha;

        const bool isScanning = processor.isTempoAnalysisInProgress();
        if (isScanning)
            scanAnimFrame += frameRateStep (1.0f, animHz, waveformRefreshHz);

        // Advance the one-shot load reveal (~0.75 s sweep + 0.2 s lock flash).
        const bool loadRevealRunning = isLoadRevealActive();
        if (loadRevealRunning)
        {
            loadAnimPhase += frameRateStep (1.0f / 45.0f, animHz);
            if (loadAnimPhase > 1.25f)
                loadAnimPhase = 2.0f;
        }

        const auto shouldRepaint = processor.isPlaying() || wasPlayingLastTick
                                || loadRevealRunning
                                || std::abs (currentPlayheadPosition - lastPaintedPlayheadSample) > 0.5
                                || std::abs (hoverAnimationAlpha - lastPaintedHoverAlpha) > 0.01f
                                || (isHoveringDisplay && std::abs (hoveredDisplayX - lastPaintedHoverX) > 0.5f)
                                || lastTempoUiRevision != processor.getTempoUiRevision()
                                || isScanning
                                || exportDragReady
                                || (holdChopId >= 0 && ! exportDragReady && holdTickCount > 0)
                                || ! getExportButtonBounds().isEmpty() || exportButtonAppear > 0.01f
                                || zoomChanged || scrollChanged || vertScaleChanged
                                || anyChopAnimationActive;

        wasPlayingLastTick = processor.isPlaying();
        lastTempoUiRevision = processor.getTempoUiRevision();

        if (shouldRepaint)
            repaint (getDisplayBounds().toNearestInt().expanded (24));
    }

    struct VisibleRange
    {
        int sampleOffset = 0;
        int visibleSamples = 0;
    };

    VisibleRange getVisibleRange (int numSamples) const noexcept
    {
        const float effectiveZoomLevel = getEffectiveZoomLevel (zoomLevel);
        const float zoomFactor = std::pow (10000.0f, effectiveZoomLevel);
        const int visibleSamples = juce::jmax (32, (int) ((float) numSamples / zoomFactor));
        const int maxOffset = juce::jmax (0, numSamples - visibleSamples);
        const int sampleOffset = (int) (scrollPosition * (float) maxOffset);
        return { sampleOffset, visibleSamples };
    }

    juce::Rectangle<float> getDisplayBounds() const
    {
        auto bounds = getLocalBounds().toFloat();
        // Match paint(): the panel tracks the (condensed) component height so the
        // display area isn't clipped top & bottom. Same insets as the 780x409 art.
        auto panelBounds = bounds.reduced (1.0f);
        return { panelBounds.getX() + 26.0f, panelBounds.getY() + 26.0f,
                 panelBounds.getWidth() - 56.0f, panelBounds.getHeight() - 54.0f };
    }

    bool isPositionInsideDisplay (juce::Point<float> position) const
    {
        return getDisplayBounds().contains (position);
    }

    double sampleForDisplayPosition (float displayX) const
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
            return 0.0;

        const auto displayBounds = getDisplayBounds();
        const auto clampedX = juce::jlimit (displayBounds.getX(), displayBounds.getRight(), displayX);
        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto relativePosition = (double) ((clampedX - displayBounds.getX()) / displayBounds.getWidth());

        return (double) visibleRange.sampleOffset
             + relativePosition * (double) juce::jmax (1, visibleRange.visibleSamples - 1);
    }

    float getSamplesPerDisplayPixel() const
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
            return 0.0f;

        const auto displayBounds = getDisplayBounds();
        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        return (float) visibleRange.visibleSamples / juce::jmax (1.0f, displayBounds.getWidth());
    }

    float displayXForSamplePosition (double samplePosition, const VisibleRange& visibleRange, juce::Rectangle<float> displayBounds) const
    {
        const auto relativePosition = (float) ((samplePosition - (double) visibleRange.sampleOffset)
                                             / (double) juce::jmax (1, visibleRange.visibleSamples));
        return displayBounds.getX() + relativePosition * displayBounds.getWidth();
    }

    double sampleForNearestBarLine (double samplePosition) const
    {
        const auto sampleData = processor.getLoadedSample();
        const auto analysis = processor.getTempoAnalysis();
        if (sampleData == nullptr || analysis == nullptr || analysis->beatPeriodSeconds <= 0.0)
            return samplePosition;

        const double sr = sampleData->sampleRate;
        if (sr <= 0.0)
            return samplePosition;

        const auto trimBpm = (double) processor.getGridBpmTrim();
        const auto adjustedBpm = analysis->estimatedBpm + trimBpm;
        const auto scaleFactor = (adjustedBpm > 0.0 && analysis->estimatedBpm > 0.0)
                               ? analysis->estimatedBpm / adjustedBpm : 1.0;
        const auto beatPeriod = analysis->beatPeriodSeconds * scaleFactor;
        const auto barPeriod = beatPeriod * 4.0;
        if (barPeriod <= 0.0)
            return samplePosition;

        const auto anchor = processor.getResolvedGridAnchorSeconds();
        const auto sampleSeconds = samplePosition / sr;
        const auto nearestBarSeconds = anchor + std::round ((sampleSeconds - anchor) / barPeriod) * barPeriod;
        return juce::jlimit (0.0,
                             (double) juce::jmax (0, sampleData->buffer.getNumSamples()),
                             nearestBarSeconds * sr);
    }

    void followTriggeredChopIfNeeded() {}

    void scrollToChop (int chopId, bool forceInstant = false)
    {
        if (chopId < 0)
            return;

        const auto sampleData = processor.getLoadedSample();
        const auto chopState = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const int numSamples = sampleData->buffer.getNumSamples();
        if (numSamples <= 0)
            return;

        const AudioPluginAudioProcessor::ChopDefinition* targetChop = nullptr;
        for (const auto& chop : chopState->chops)
        {
            if (chop.id == chopId)
            {
                targetChop = &chop;
                break;
            }
        }

        if (targetChop == nullptr)
            return;

        const auto visibleRange = getVisibleRange (numSamples);
        const int maxOffset = juce::jmax (0, numSamples - visibleRange.visibleSamples);
        if (maxOffset <= 0)
            return;

        const double chopStart = (double) juce::jlimit (0, numSamples, targetChop->startSample);
        const double chopEnd = (double) juce::jlimit (0, numSamples, targetChop->endSample);
        const double visibleSamples = (double) juce::jmax (1, visibleRange.visibleSamples);
        const double chopLength = juce::jmax (1.0, chopEnd - chopStart);

        const double visibleStart = (double) visibleRange.sampleOffset;
        const double visibleEnd = visibleStart + visibleSamples;
        const bool isFullyVisible = (chopStart >= visibleStart && chopEnd <= visibleEnd);
        const bool isPartiallyVisible = (chopStart < visibleEnd && chopEnd > visibleStart);

        // If the chop is already visible in the viewport, no scrolling is necessary!
        if (isFullyVisible || (chopLength > visibleSamples && isPartiallyVisible))
            return;

        const double targetOffset = chopLength >= visibleSamples * 0.8
                                  ? chopStart - visibleSamples * 0.1
                                  : ((chopStart + chopEnd) * 0.5) - visibleSamples * 0.5;
        const float targetScroll = juce::jlimit (0.0f, 1.0f, (float) (targetOffset / (double) maxOffset));

        targetScrollPosition = targetScroll;
        if (forceInstant)
        {
            scrollPosition = targetScroll;
            rebuildWaveformPath();
            updateHorizontalScrollBar();
        }

        if (onScrollChanged)
            onScrollChanged (targetScroll);
    }

    void updateHoverState (juce::Point<float> position)
    {
        const auto displayBounds = getDisplayBounds();
        isHoveringDisplay = displayBounds.contains (position);
        
        if (isHoveringDisplay)
            hoveredDisplayX = juce::jlimit (displayBounds.getX(), displayBounds.getRight(), position.x);

        hoveredChopId = -1;
        if (isHoveringDisplay)
        {
            const auto hoveredSample = sampleForDisplayPosition (position.x);
            if (const auto chopState = processor.getChopState(); chopState != nullptr)
            {
                for (const auto& chop : chopState->chops)
                {
                    if (hoveredSample >= (double) chop.startSample && hoveredSample < (double) chop.endSample)
                    {
                        hoveredChopId = chop.id;
                        break;
                    }
                }
            }
        }

        setMouseCursor (isHoveringDisplay ? juce::MouseCursor::PointingHandCursor
                                          : juce::MouseCursor::NormalCursor);
    }

    void paintAnalysisRegion (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        double startSample = 0.0;
        double endSample = 0.0;
        bool hasRegion = false;

        if (isSelectingAnalysisRegion)
        {
            startSample = analysisSelectionStartSample;
            endSample = analysisSelectionCurrentSample;
            hasRegion = true;
        }
        else if (const auto editState = processor.getTempoEditState();
                 editState != nullptr && editState->regionStartSample >= 0 && editState->regionEndSample > editState->regionStartSample)
        {
            startSample = (double) editState->regionStartSample;
            endSample = (double) editState->regionEndSample;
            hasRegion = true;
        }

        if (! hasRegion)
            return;

        const auto regionStart = juce::jmin (startSample, endSample);
        const auto regionEnd = juce::jmax (startSample, endSample);

        if (regionEnd < (double) visibleRange.sampleOffset
            || regionStart > (double) (visibleRange.sampleOffset + visibleRange.visibleSamples))
            return;

        const auto startX = displayXForSamplePosition (regionStart, visibleRange, displayBounds);
        const auto endX = displayXForSamplePosition (regionEnd, visibleRange, displayBounds);
        const auto regionBounds = juce::Rectangle<float> (juce::jmin (startX, endX), displayBounds.getY(),
                                                          std::abs (endX - startX), displayBounds.getHeight());

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        fillRectGradient (g, regionBounds, juce::Colour (0xff3da5ff).withAlpha (0.14f),
                          juce::Colour (0xff3da5ff).withAlpha (0.06f));
        g.setColour (juce::Colour (0xff3da5ff).withAlpha (0.55f));
        g.drawRect (regionBounds, 1.5f);
    }

    void paintEdgeDragGhost (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        if (edgeDragChopId < 0)
            return;

        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const AudioPluginAudioProcessor::ChopDefinition* chop = nullptr;
        for (const auto& c : chopState->chops)
            if (c.id == edgeDragChopId) { chop = &c; break; }
        if (chop == nullptr)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const double ghostStartSample = juce::jmin ((double) (edgeDragKind == 1 ? chop->endSample : chop->startSample),
                                                    edgeDragLiveSample);
        const double ghostEndSample = juce::jmax ((double) (edgeDragKind == 1 ? chop->endSample : chop->startSample),
                                                  edgeDragLiveSample);
        const float ghostStartX = displayXForSamplePosition (ghostStartSample, visibleRange, displayBounds);
        const float ghostEndX = displayXForSamplePosition (ghostEndSample, visibleRange, displayBounds);

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        const auto ghostBounds = juce::Rectangle<float> (juce::jmin (ghostStartX, ghostEndX),
                                                          displayBounds.getY(),
                                                          std::abs (ghostEndX - ghostStartX),
                                                          displayBounds.getHeight());

        fillRectGradient (g, ghostBounds, juce::Colour (0xff00f57a).withAlpha (0.24f),
                          juce::Colour (0xff00f57a).withAlpha (0.10f));

        g.setColour ((edgeDragBarSnap ? juce::Colour (0xffff6900) : juce::Colour (0xff00f57a)).withAlpha (0.95f));
        g.drawLine (ghostBounds.getX(), displayBounds.getY() + 2.0f, ghostBounds.getX(), displayBounds.getBottom() - 2.0f, 2.0f);
        g.drawLine (ghostBounds.getRight(), displayBounds.getY() + 2.0f, ghostBounds.getRight(), displayBounds.getBottom() - 2.0f, 2.0f);
    }

    void paintChops (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto chopState = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto visibleStart = (double) visibleRange.sampleOffset;
        const auto visibleEnd = visibleStart + (double) visibleRange.visibleSamples;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        int chopIndex = 0;
        for (const auto& chop : chopState->chops)
        {
            const bool isAlternativeChop = (chopIndex % 2 == 1);
            ++chopIndex;
            const int displayNumber = chopIndex; // 1-based, matches the MIDI pad order

            if ((double) chop.endSample < visibleStart || (double) chop.startSample > visibleEnd)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX = displayXForSamplePosition ((double) chop.endSample, visibleRange, displayBounds);
            auto chopBounds = juce::Rectangle<float> (juce::jmin (startX, endX), displayBounds.getY(),
                                                      std::abs (endX - startX), displayBounds.getHeight());

            // During the load reveal chops pop in as the scan head passes them.
            if (isLoadRevealActive() && loadAnimPhase < 1.0f
                && chopBounds.getX() > loadRevealSweepX (displayBounds))
                continue;

            auto& anim = chopAnimations[chop.id];
            const float hoverVal = anim.currentHoverAlpha;
            const float selectVal = anim.currentSelectAlpha;
            const float pulseVal = anim.currentTriggerPulse;
            const bool isSelected = (chop.id == chopState->selectedChopId);
            const bool isHovered = (chop.id == hoveredChopId);

            const bool isLight = (currentTheme == Theme::light);

            const auto baseFill = isAlternativeChop
                ? (isLight ? juce::Colour (0xffc82046).withAlpha (0.18f) : juce::Colour (0xffff4a6b).withAlpha (0.20f))
                : (isLight ? juce::Colour (0xff1c72b8).withAlpha (0.13f) : juce::Colour (0xff3da5ff).withAlpha (0.12f));
            const auto hoverFill = isLight
                ? juce::Colour (0xff1c72b8).withAlpha (0.14f)
                : juce::Colour (0xff8fd9ff).withAlpha (0.14f);
            const auto selectFill = isLight
                ? juce::Colour (0xff00a33c).withAlpha (0.20f)
                : juce::Colour (0xff00c950).withAlpha (0.22f);
            const auto fillColour = baseFill.interpolatedWith (hoverFill, hoverVal).interpolatedWith (selectFill, selectVal);

            const auto baseLine = isAlternativeChop
                ? (isLight ? juce::Colour (0xffc82046).withAlpha (0.75f) : juce::Colour (0xffff4a6b).withAlpha (0.75f))
                : (isLight ? juce::Colour (0xff1c72b8).withAlpha (0.65f) : juce::Colour (0xff3da5ff).withAlpha (0.55f));
            const auto hoverLine = isLight
                ? juce::Colour (0xff1c72b8).withAlpha (0.85f)
                : juce::Colour (0xff8fd9ff).withAlpha (0.82f);
            const auto selectLine = isLight
                ? juce::Colour (0xff00b844).withAlpha (0.98f)
                : juce::Colour (0xff00f57a).withAlpha (0.98f);
            const auto lineColour = baseLine.interpolatedWith (hoverLine, hoverVal).interpolatedWith (selectLine, selectVal);

            fillRectGradient (g, chopBounds, fillColour.brighter (0.35f), fillColour.darker (0.25f));

            if (selectVal > 0.01f)
            {
                fillRectGradient (g, chopBounds,
                                  juce::Colour (0xff00f57a).withAlpha (0.16f * selectVal),
                                  juce::Colour (0xff00f57a).withAlpha (0.07f * selectVal));
            }
            else if (hoverVal > 0.01f)
            {
                fillRectGradient (g, chopBounds.expanded (0.5f * hoverVal, 0.0f),
                                  juce::Colour (0xff8fd9ff).withAlpha (0.15f * hoverVal),
                                  juce::Colour (0xff8fd9ff).withAlpha (0.06f * hoverVal));
            }

            // Real-Time Trigger Ripple Effect
            if (pulseVal > 0.01f)
            {
                const auto pulseColour = chop.favorite ? juce::Colour (0xffff2db1) : accentOrange;
                g.setColour (pulseColour.withAlpha (0.35f * pulseVal));
                g.fillRect (chopBounds);

                g.setColour (pulseColour.withAlpha (0.85f * pulseVal));
                g.drawRect (chopBounds.expanded (3.0f * (1.0f - pulseVal), 0.0f), 1.5f + 2.0f * pulseVal);
            }

            if (hoverVal > 0.01f)
            {
                const float cx = chopBounds.getCentreX();
                const float cy = chopBounds.getCentreY();
                const float maxR = juce::jmin (18.0f, chopBounds.getWidth() * 0.38f) * (0.85f + 0.15f * hoverVal);
                if (maxR >= 4.0f)
                {
                    const float triH = maxR * 1.4f;
                    const float triW = maxR * 1.2f;
                    g.setColour (juce::Colours::black.withAlpha (0.52f * hoverVal));
                    g.fillEllipse (cx - maxR, cy - maxR, maxR * 2.0f, maxR * 2.0f);
                    juce::Path tri;
                    tri.addTriangle (cx - triW * 0.33f, cy - triH * 0.5f,
                                     cx - triW * 0.33f, cy + triH * 0.5f,
                                     cx + triW * 0.67f, cy);
                    g.setColour (juce::Colours::white.withAlpha (0.92f * hoverVal));
                    g.fillPath (tri);
                }
            }

            {
                const auto centreY = displayBounds.getCentreY();
                const auto lanePadding = 2.0f;
                const auto laneGap = 4.0f;
                const auto availableWidth = juce::jmax (8.0f, chopBounds.getWidth() - lanePadding * 2.0f - laneGap);
                const auto laneWidth = juce::jmax (3.0f, availableWidth * 0.5f);
                const auto maxHalfHeight = juce::jmax (4.0f, chopBounds.getHeight() * 0.44f);
                const auto gainNormalizedValue = chop.gainDecibels >= 0.0f
                    ? juce::jlimit (0.0f, 1.0f, chop.gainDecibels / 12.0f)
                    : juce::jlimit (-1.0f, 0.0f, chop.gainDecibels / 24.0f);
                const auto pitchNormalizedValue = juce::jlimit (-1.0f, 1.0f, chop.pitchSemitones / 12.0f);

                const float fillAlpha = 0.18f + 0.08f * hoverVal + 0.14f * selectVal;
                const float borderAlpha = 0.38f + 0.17f * hoverVal + 0.25f * selectVal;

                auto paintOverlayBar = [&] (float normalizedValue, juce::Colour colour, float x)
                {
                    if (std::abs (normalizedValue) <= 0.001f)
                        return;

                    const auto halfHeight = juce::jmax (2.0f, maxHalfHeight * std::abs (normalizedValue));
                    const bool isPositive = normalizedValue >= 0.0f;
                    const auto bounds = juce::Rectangle<float> (x,
                                                                isPositive ? (centreY - halfHeight) : centreY,
                                                                laneWidth,
                                                                halfHeight);
                    fillRectGradient (g, bounds, colour.withAlpha (fillAlpha + 0.06f),
                                      colour.withAlpha (fillAlpha * 0.55f));
                    g.setColour (colour.withAlpha (borderAlpha));
                    g.drawRect (bounds, 1.0f);
                };

                const auto laneX = chopBounds.getX() + lanePadding;
                paintOverlayBar (gainNormalizedValue, juce::Colour (0xfffb2c36), laneX);
                paintOverlayBar (pitchNormalizedValue, juce::Colour (0xfff6339a), laneX + laneWidth + laneGap);
            }

            // ---- Chop identity: full-height boundary lines + a numbered
            // header tab along the top of every chop. The tab carries the
            // chop's 1-based number (matching the MIDI pad order), so slices
            // read as discrete, labelled segments instead of tint changes.
            g.setColour (lineColour);
            g.drawLine (chopBounds.getX(), chopBounds.getY(), chopBounds.getX(), chopBounds.getBottom(), isSelected ? 2.2f : isHovered ? 1.9f : 1.5f);
            g.drawLine (chopBounds.getRight(), chopBounds.getY(), chopBounds.getRight(), chopBounds.getBottom(), isSelected ? 2.2f : isHovered ? 1.9f : 1.5f);

            // ---- Selection: bold corner brackets, test-equipment style ----
            if (selectVal > 0.05f)
            {
                const auto sel = selectLine.withAlpha (0.95f * selectVal);
                const float arm = juce::jmin (16.0f, chopBounds.getWidth() * 0.30f);
                const float t   = 2.5f;
                auto b = chopBounds.reduced (1.0f);
                g.setColour (sel);
                // top-left
                g.fillRect (juce::Rectangle<float> (b.getX(), b.getY(), arm, t));
                g.fillRect (juce::Rectangle<float> (b.getX(), b.getY(), t, arm));
                // top-right
                g.fillRect (juce::Rectangle<float> (b.getRight() - arm, b.getY(), arm, t));
                g.fillRect (juce::Rectangle<float> (b.getRight() - t, b.getY(), t, arm));
                // bottom-left
                g.fillRect (juce::Rectangle<float> (b.getX(), b.getBottom() - t, arm, t));
                g.fillRect (juce::Rectangle<float> (b.getX(), b.getBottom() - arm, t, arm));
                // bottom-right
                g.fillRect (juce::Rectangle<float> (b.getRight() - arm, b.getBottom() - t, arm, t));
                g.fillRect (juce::Rectangle<float> (b.getRight() - t, b.getBottom() - arm, t, arm));
            }

            if (isSelected)
            {
                const auto cueStartSample = chop.startSample + chop.cueOffsetSamples;
                const auto cueX = displayXForSamplePosition ((double) cueStartSample, visibleRange, displayBounds);
                g.setColour (accentOrange.withAlpha (0.9f));
                g.drawLine (cueX, displayBounds.getY() + 10.0f, cueX, displayBounds.getBottom() - 10.0f, 1.5f);
            }

            if (chop.favorite)
            {
                const juce::Colour favColour (0xffff2db1);
                // Fill
                fillRectGradient (g, chopBounds, favColour.withAlpha (isSelected ? 0.5f : 0.36f),
                                  favColour.withAlpha (isSelected ? 0.24f : 0.18f));
                // Side border lines
                g.setColour (favColour.withAlpha (isSelected ? 1.0f : 0.88f));
                g.drawLine (chopBounds.getX(), chopBounds.getY() + 4.0f,
                            chopBounds.getX(), chopBounds.getBottom() - 4.0f, isSelected ? 3.0f : 2.5f);
                g.drawLine (chopBounds.getRight(), chopBounds.getY() + 4.0f,
                            chopBounds.getRight(), chopBounds.getBottom() - 4.0f, isSelected ? 3.0f : 2.5f);
                // Solid top-edge stripe so favorites are obvious even at narrow zoom
                g.setColour (favColour);
                g.fillRect (chopBounds.getX(), chopBounds.getY(), chopBounds.getWidth(), 3.0f);
            }

            // ---- Numbered header tab (drawn last so favorites can't wash it
            // out). Every chop carries its 1-based number — the same order the
            // MIDI pads use — so slices read as discrete labelled segments.
            if (chopBounds.getWidth() >= 18.0f)
            {
                const float headerTop = chopBounds.getY() + (chop.favorite ? 3.0f : 0.0f);
                const float headerH   = 15.0f;
                auto header = juce::Rectangle<float> (chopBounds.getX(), headerTop,
                                                      chopBounds.getWidth(), headerH);

                const auto tabColour = chop.favorite ? juce::Colour (0xffff2db1) : lineColour;
                g.setColour (tabColour.withAlpha (isSelected ? 0.50f : 0.22f + 0.12f * hoverVal));
                g.fillRect (header);
                g.setColour (tabColour.withAlpha (0.60f + 0.35f * selectVal));
                g.drawLine (header.getX(), header.getBottom(), header.getRight(), header.getBottom(), 1.0f);

                g.setColour ((isLight ? juce::Colour (0xff2b2318) : juce::Colour (0xfff2e7da))
                                 .withAlpha (isSelected ? 1.0f : 0.78f + 0.2f * hoverVal));
                g.setFont (monoFont (9.5f).withExtraKerningFactor (0.05f));
                g.drawText (juce::String (displayNumber),
                            header.reduced (5.0f, 1.0f).toNearestInt(),
                            juce::Justification::centredLeft, false);
            }
        }

    }

    // Renders the warped audio waveform inside chops that have warp markers.
    // For each pixel column in the chop, we look up the source sample range
    // that maps to that column via the WarpMap and draw min/max from the peak
    // cache. As markers are dragged, the waveform visually compresses and
    // stretches between them — Logic-Flex-Audio style.
    void paintWarpedChopAudio (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const auto sr = sampleData->sampleRate;
        if (sr <= 0.0)
            return;

        const auto& buffer = sampleData->buffer;
        const int numChannels = buffer.getNumChannels();
        const int numSamples  = buffer.getNumSamples();
        if (numChannels <= 0 || numSamples <= 0)
            return;

        const auto visibleRange = getVisibleRange (numSamples);
        const auto visibleStart = (double) visibleRange.sampleOffset;
        const auto visibleEnd   = visibleStart + (double) visibleRange.visibleSamples;
        const auto centreY      = displayBounds.getCentreY();
        const auto halfHeight   = displayBounds.getHeight() * 0.48f * waveformVerticalScale;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        for (const auto& chop : chopState->chops)
        {
            if (chop.warpMarkers.empty())
                continue;
            if ((double) chop.endSample < visibleStart || (double) chop.startSample > visibleEnd)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX   = displayXForSamplePosition ((double) chop.endSample,   visibleRange, displayBounds);
            const auto chopBounds = juce::Rectangle<float> (juce::jmin (startX, endX),
                                                            displayBounds.getY(),
                                                            std::abs (endX - startX),
                                                            displayBounds.getHeight());
            if (chopBounds.getWidth() < 2.0f)
                continue;

            cuesampler::WarpMap warpMap;
            warpMap.build (chop.startSample, chop.endSample, chop.warpMarkers, sr);
            const double chopDurationSec = warpMap.totalLocalDurationSeconds();
            if (chopDurationSec <= 0.0)
                continue;

            // Mask the linear waveform inside the chop bounds before drawing
            // the warped one over it.
            fillRectGradient (g, chopBounds, panelInnerDark.brighter (0.1f), panelInnerDark.darker (0.16f));

            const int leftPx  = (int) std::floor (chopBounds.getX());
            const int rightPx = (int) std::ceil  (chopBounds.getRight());
            const float widthPx = (float) (rightPx - leftPx);
            if (widthPx <= 0.0f)
                continue;

            juce::Path topPath;
            juce::Array<float> bottomYs;
            bottomYs.ensureStorageAllocated (rightPx - leftPx);
            bool topStarted = false;

            for (int px = leftPx; px < rightPx; ++px)
            {
                const double leftLocal  = ((double) (px      - leftPx) / (double) widthPx) * chopDurationSec;
                const double rightLocal = ((double) (px + 1  - leftPx) / (double) widthPx) * chopDurationSec;
                const double srcA = warpMap.sourceSampleAtLocalTime (leftLocal);
                const double srcB = warpMap.sourceSampleAtLocalTime (rightLocal);
                const int srcStart = juce::jlimit (0, numSamples - 1, (int) std::floor (juce::jmin (srcA, srcB)));
                const int srcEnd   = juce::jlimit (srcStart + 1, numSamples, (int) std::ceil (juce::jmax (srcA, srcB)) + 1);

                float maxVal = 0.0f;
                float minVal = 0.0f;

                if (srcEnd - srcStart < cacheBlockSize)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* data = buffer.getReadPointer (ch);
                        for (int s = srcStart; s < srcEnd; ++s)
                        {
                            const auto v = data[s];
                            if (v > maxVal) maxVal = v;
                            if (v < minVal) minVal = v;
                        }
                    }
                }
                else
                {
                    const int startBlock = srcStart / cacheBlockSize;
                    const int endBlock   = (srcEnd  + cacheBlockSize - 1) / cacheBlockSize;
                    for (int b = startBlock; b < endBlock && b < peakCache.size(); ++b)
                    {
                        const auto range = peakCache.getReference (b);
                        if (range.getEnd()   > maxVal) maxVal = range.getEnd();
                        if (range.getStart() < minVal) minVal = range.getStart();
                    }
                }

                // Clamp to unity (matches the linear waveform path) so hot
                // samples can't overflow the window vertically.
                maxVal = juce::jlimit (-1.0f, 1.0f, maxVal);
                minVal = juce::jlimit (-1.0f, 1.0f, minVal);

                const auto xPos    = (float) px;
                const auto topY    = centreY - maxVal * halfHeight;
                const auto bottomY = centreY - minVal * halfHeight;

                bottomYs.add (bottomY);
                if (! topStarted)
                {
                    topPath.startNewSubPath (xPos, topY);
                    topStarted = true;
                }
                else
                {
                    topPath.lineTo (xPos, topY);
                }
            }

            juce::Path combined;
            combined.addPath (topPath);
            for (int i = bottomYs.size() - 1; i >= 0; --i)
                combined.lineTo ((float) (leftPx + i), bottomYs.getReference (i));
            combined.closeSubPath();

            g.setColour (glassText.withAlpha (0.06f));
            g.strokePath (combined, juce::PathStrokeType (4.0f));
            g.setColour (glassText.withAlpha (0.55f));
            g.fillPath (combined);
            g.setColour (glassText.withAlpha (0.85f));
            g.strokePath (combined, juce::PathStrokeType (1.0f));

            g.setColour (glassText.withAlpha (0.15f));
            g.drawHorizontalLine ((int) centreY, chopBounds.getX(), chopBounds.getRight());
        }
    }

    // Vertical Logic-style flex-marker lines drawn at each marker's playback
    // position (= linearly along chop width). No diagonal connectors. The
    // waveform underneath stretches because paintWarpedChopAudio re-samples
    // through the warp map.
    void paintWarpMarkers (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto chopState  = processor.getChopState();
        if (sampleData == nullptr || chopState == nullptr)
            return;

        const auto sr = sampleData->sampleRate;
        if (sr <= 0.0)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto visibleStart = (double) visibleRange.sampleOffset;
        const auto visibleEnd   = visibleStart + (double) visibleRange.visibleSamples;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        const juce::Colour markerColour      (0xff3da5ff); // bright blue — flex marker
        const juce::Colour markerStaleColour (0xfff5a623); // amber — grid drifted
        const double currentFingerprint = processor.getCurrentGridFingerprint();
        constexpr double kFingerprintEpsilon = 1.0e-6;

        for (const auto& chop : chopState->chops)
        {
            if (chop.warpMarkers.empty())
                continue;
            if ((double) chop.endSample < visibleStart || (double) chop.startSample > visibleEnd)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX   = displayXForSamplePosition ((double) chop.endSample,   visibleRange, displayBounds);
            const auto chopBounds = juce::Rectangle<float> (juce::jmin (startX, endX),
                                                            displayBounds.getY(),
                                                            std::abs (endX - startX),
                                                            displayBounds.getHeight());
            if (chopBounds.getWidth() < 4.0f)
                continue;

            const double chopDurationSec = (double) (chop.endSample - chop.startSample) / sr;
            if (chopDurationSec <= 0.0)
                continue;

            const float chopWidth = chopBounds.getWidth();
            const float topY = chopBounds.getY() + 4.0f;
            const float botY = chopBounds.getBottom() - 4.0f;

            for (const auto& marker : chop.warpMarkers)
            {
                if (marker.sourceSample <= chop.startSample || marker.sourceSample >= chop.endSample)
                    continue;

                // Local-time-x: linear along the chop's playback timeline.
                const double localFrac = juce::jlimit (0.0, 1.0, marker.localTimeSeconds / chopDurationSec);
                const float x = chopBounds.getX() + (float) localFrac * chopWidth;
                if (x < chopBounds.getX() - 4.0f || x > chopBounds.getRight() + 4.0f)
                    continue;

                const bool isStale = marker.snappedToGrid
                                     && currentFingerprint > 0.0
                                     && marker.gridFingerprint > 0.0
                                     && std::abs (marker.gridFingerprint - currentFingerprint) > kFingerprintEpsilon;
                const auto col = isStale ? markerStaleColour : markerColour;

                // Soft halo behind the line for grab affordance.
                g.setColour (col.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, topY, 6.0f, botY - topY));

                // Bright vertical line.
                g.setColour (col);
                g.drawLine (x, topY, x, botY, 1.5f);

                // Tiny grab handle at the top so the marker is hit-testable
                // even at deep zoom-out where the line is thin.
                juce::Path handle;
                handle.addTriangle (x,        topY + 6.0f,
                                    x - 5.0f, topY,
                                    x + 5.0f, topY);
                g.setColour (col);
                g.fillPath (handle);
                g.setColour (col.brighter (0.4f));
                g.strokePath (handle, juce::PathStrokeType (1.0f));
            }
        }
    }

    void paintTempoGrid (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        const auto analysis = processor.getTempoAnalysis();
        if (sampleData == nullptr || analysis == nullptr || analysis->beatPeriodSeconds <= 0.0)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const auto visibleStart = (double) visibleRange.sampleOffset;
        const auto visibleEnd = visibleStart + (double) visibleRange.visibleSamples;
        const auto sr = sampleData->sampleRate;

        const auto trimBpm = (double) processor.getGridBpmTrim();
        const auto adjustedBpm = analysis->estimatedBpm + trimBpm;
        const auto scaleFactor = (adjustedBpm > 0.0 && analysis->estimatedBpm > 0.0)
                               ? analysis->estimatedBpm / adjustedBpm : 1.0;
        const auto anchor = processor.getResolvedGridAnchorSeconds();
        const auto beatPeriod = analysis->beatPeriodSeconds * scaleFactor;
        const auto barPeriod  = beatPeriod * 4.0;
        if (beatPeriod <= 0.0)
            return;

        const auto visibleStartSec = visibleStart / sr;
        const auto visibleEndSec   = visibleEnd   / sr;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        auto drawGridLine = [&] (float x,
                                 float y1,
                                 float y2,
                                 juce::Colour colour,
                                 float thickness)
        {
            g.setColour (juce::Colours::black.withAlpha (0.42f));
            g.drawLine (x, y1, x, y2, thickness + 1.4f);
            g.setColour (colour);
            g.drawLine (x, y1, x, y2, thickness);
        };

        const bool isLightMode = (currentTheme == Theme::light);
        const auto gridColour = isLightMode ? juce::Colour (0xff4a3f35) : juce::Colour (0xff8fd9ff);
        const auto barColour  = isLightMode ? juce::Colour (0xffb83e00) : juce::Colour (0xffff6900);

        // 16th-note lines — fade in above zoom 0.60
        {
            const float alpha = juce::jlimit (0.0f, 1.0f, (zoomLevel - 0.60f) / 0.20f);
            if (alpha > 0.0f)
            {
                const auto period = beatPeriod / 4.0;
                double t = anchor;
                if (t > visibleStartSec)
                    t -= std::ceil ((t - visibleStartSec) / period) * period;
                for (; t <= visibleEndSec; t += period)
                {
                    const auto sample = t * sr;
                    if (sample < visibleStart || sample > visibleEnd) continue;
                    const auto x = displayXForSamplePosition (sample, visibleRange, displayBounds);
                    drawGridLine (x, displayBounds.getY() + 16.0f, displayBounds.getBottom() - 16.0f,
                                  gridColour.withAlpha (0.52f * alpha), 0.9f);
                }
            }
        }

        // 8th-note lines — fade in above zoom 0.35
        {
            const float alpha = juce::jlimit (0.0f, 1.0f, (zoomLevel - 0.35f) / 0.20f);
            if (alpha > 0.0f)
            {
                const auto period = beatPeriod / 2.0;
                double t = anchor;
                if (t > visibleStartSec)
                    t -= std::ceil ((t - visibleStartSec) / period) * period;
                for (; t <= visibleEndSec; t += period)
                {
                    const auto sample = t * sr;
                    if (sample < visibleStart || sample > visibleEnd) continue;
                    const auto x = displayXForSamplePosition (sample, visibleRange, displayBounds);
                    drawGridLine (x, displayBounds.getY() + 12.0f, displayBounds.getBottom() - 12.0f,
                                  gridColour.withAlpha (0.62f * alpha), 1.0f);
                }
            }
        }

        // Beat lines — start from first beat at or before visibleStartSec
        {
            double t = anchor;
            if (t > visibleStartSec)
                t -= std::ceil ((t - visibleStartSec) / beatPeriod) * beatPeriod;
            for (; t <= visibleEndSec; t += beatPeriod)
            {
                const auto sample = t * sr;
                if (sample < visibleStart || sample > visibleEnd) continue;
                const auto x = displayXForSamplePosition (sample, visibleRange, displayBounds);
                drawGridLine (x,
                              displayBounds.getY() + 8.0f,
                              displayBounds.getBottom() - 8.0f,
                              gridColour.withAlpha (0.42f),
                              1.15f);
            }
        }

        // Bar lines — same anchor, 4-beat period
        {
            double t = anchor;
            if (t > visibleStartSec)
                t -= std::ceil ((t - visibleStartSec) / barPeriod) * barPeriod;
            for (; t <= visibleEndSec; t += barPeriod)
            {
                const auto sample = t * sr;
                if (sample < visibleStart || sample > visibleEnd) continue;
                const auto x = displayXForSamplePosition (sample, visibleRange, displayBounds);
                drawGridLine (x,
                              displayBounds.getY() + 4.0f,
                              displayBounds.getBottom() - 4.0f,
                              barColour.withAlpha (0.82f),
                              1.9f);
            }
        }
    }

    void paintHoverGuide (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        if (hoverAnimationAlpha <= 0.001f)
        {
            lastPaintedHoverAlpha = hoverAnimationAlpha;
            lastPaintedHoverX = hoveredDisplayX;
            return;
        }

        const auto hoverX = juce::jlimit (displayBounds.getX(), displayBounds.getRight(), hoveredDisplayX);
        // Vibrant glowing neon red — but follows the WARP accent and turns purple in warp-edit mode.
        const auto guideColour = cue::isWarpModeActive ? juce::Colour (0xffa855f7)  // WARP purple
                                                       : juce::Colour (0xffff2233); // neon red
        lastPaintedHoverAlpha = hoverAnimationAlpha;
        lastPaintedHoverX = hoverX;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        const float topY = displayBounds.getY() + 2.0f;
        const float arrowHeight = 11.0f;
        const float arrowWidth = 13.0f;
        const float lineStartY = topY + arrowHeight;

        // Wide soft neon red laser glow
        g.setColour (guideColour.withAlpha (0.05f * hoverAnimationAlpha));
        g.fillRect (juce::Rectangle<float> (hoverX - 3.0f, lineStartY, 6.0f, displayBounds.getBottom() - 4.0f - lineStartY));

        // High-opacity core neon red line
        g.setColour (guideColour.withAlpha (0.75f * hoverAnimationAlpha));
        g.drawLine (hoverX, lineStartY, hoverX, displayBounds.getBottom() - 4.0f, 1.2f);

        // Sleek modern notched arrowhead pointing downwards at the top of the line
        juce::Path arrowPath;
        arrowPath.startNewSubPath (hoverX - arrowWidth * 0.5f, topY);
        arrowPath.lineTo (hoverX, topY + arrowHeight * 0.25f);
        arrowPath.lineTo (hoverX + arrowWidth * 0.5f, topY);
        arrowPath.lineTo (hoverX, topY + arrowHeight);
        arrowPath.closeSubPath();

        // Subtle drop-shadow under the arrowhead for separation
        g.setColour (juce::Colours::black.withAlpha (0.45f * hoverAnimationAlpha));
        g.fillPath (arrowPath, juce::AffineTransform::translation (0.0f, 1.0f));

        // Deep red fill
        g.setColour (guideColour.withAlpha (0.95f * hoverAnimationAlpha));
        g.fillPath (arrowPath);

        // Bright white outline for crisp contrast and premium look
        g.setColour (juce::Colours::white.withAlpha (0.9f * hoverAnimationAlpha));
        g.strokePath (arrowPath, juce::PathStrokeType (1.0f));
    }

    void paintPlayhead (juce::Graphics& g, juce::Rectangle<float> displayBounds)
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr)
            return;

        const auto numSamples = sampleData->buffer.getNumSamples();
        if (numSamples == 0)
            return;

        const auto visibleRange = getVisibleRange (numSamples);
        const auto rawPlayheadSample = processor.getPlaybackSamplePosition();
        lastPaintedPlayheadSample = rawPlayheadSample;

        // For warped chops the audio thread reads from the cache linearly in
        // clip-local time; the playhead value advances linearly through the
        // chop slot. Remap through the warp map so the visible playhead tracks
        // the source-x where the audio actually originated.
        double playheadSample = rawPlayheadSample;
        if (const auto chopState = processor.getChopState(); chopState != nullptr)
        {
            for (const auto& chop : chopState->chops)
            {
                if (chop.warpMarkers.empty())
                    continue;
                if (rawPlayheadSample < (double) chop.startSample
                    || rawPlayheadSample >= (double) chop.endSample)
                    continue;

                cuesampler::WarpMap warpMap;
                warpMap.build (chop.startSample, chop.endSample, chop.warpMarkers,
                               sampleData->sampleRate);
                if (warpMap.isIdentity())
                    break;

                const double localTimeSec = (rawPlayheadSample - (double) chop.startSample)
                                            / sampleData->sampleRate;
                playheadSample = warpMap.sourceSampleAtLocalTime (localTimeSec);
                break;
            }
        }

        if (playheadSample < (double) visibleRange.sampleOffset
            || playheadSample > (double) (visibleRange.sampleOffset + visibleRange.visibleSamples))
            return;

        const auto visibleDenominator = juce::jmax (1, visibleRange.visibleSamples);
        const auto relativePosition = (float) ((playheadSample - (double) visibleRange.sampleOffset)
                                             / (double) visibleDenominator);
        const auto x = displayBounds.getX() + relativePosition * displayBounds.getWidth();

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        const bool isLight = (currentTheme == Theme::light);
        const auto playheadColour = isLight ? juce::Colour (0xff00a33c) : juce::Colour (0xff00c950);
        g.setColour (playheadColour.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (x - 3.0f, displayBounds.getY() + 4.0f, 6.0f, displayBounds.getHeight() - 8.0f));
        g.setColour (playheadColour);
        g.drawLine (x, displayBounds.getY() + 4.0f, x, displayBounds.getBottom() - 4.0f, 2.0f);
    }

    void scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart) override
    {
        if (scrollBar != &horizontalScrollBar || updatingHorizontalScrollBar)
            return;

        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() <= 0)
            return;

        const auto visibleRange = getVisibleRange (sampleData->buffer.getNumSamples());
        const double visibleProportion = juce::jlimit (0.02, 1.0,
            (double) visibleRange.visibleSamples / (double) sampleData->buffer.getNumSamples());
        const double maxRangeStart = juce::jmax (0.0, 1.0 - visibleProportion);
        const float newScroll = maxRangeStart <= 0.0
                              ? 0.0f
                              : juce::jlimit (0.0f, 1.0f, (float) (newRangeStart / maxRangeStart));

        scrollPosition = targetScrollPosition = newScroll;
        rebuildWaveformPath();
        updateHorizontalScrollBar();
        repaint();
        if (onScrollChanged)
            onScrollChanged (newScroll);
    }

    void rebuildWaveformPath (bool simplified = false)
    {
        waveformPath.clear();

        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
            return;

        auto displayBounds = getDisplayBounds();

        const auto& buffer = sampleData->buffer;
        auto numSamples = buffer.getNumSamples();
        auto numChannels = buffer.getNumChannels();
        auto displayWidth = displayBounds.getWidth();
        auto displayHeight = displayBounds.getHeight();
        auto centreY = displayBounds.getCentreY();

        int samplesPerPixel = juce::jmax (1, numSamples / (int) displayWidth);

        const auto visibleRange = getVisibleRange (numSamples);
        const int visibleSamples = visibleRange.visibleSamples;
        const int sampleOffset = visibleRange.sampleOffset;

        samplesPerPixel = juce::jmax (1, visibleSamples / (int) displayWidth);

        const int step = simplified ? 2 : 1;

        juce::Path topPath;
        juce::Array<float> bottomYs;
        bottomYs.ensureStorageAllocated ((int) displayWidth / step);

        bool topStarted = false;

        for (int pixel = 0; pixel < (int) displayWidth; pixel += step)
        {
            int startSample = sampleOffset + pixel * samplesPerPixel;
            int endSample = juce::jmin (startSample + step * samplesPerPixel, numSamples);

            float maxVal = 0.0f;
            float minVal = 0.0f;

            if (samplesPerPixel < cacheBlockSize)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int s = startSample; s < endSample; ++s)
                    {
                        auto sample = data[s];
                        if (sample > maxVal) maxVal = sample;
                        if (sample < minVal) minVal = sample;
                    }
                }
            }
            else
            {
                int startBlock = startSample / cacheBlockSize;
                int endBlock = endSample / cacheBlockSize;

                if (startBlock == endBlock)
                {
                    if (startBlock < peakCache.size())
                    {
                        auto range = peakCache.getReference (startBlock);
                        maxVal = range.getEnd();
                        minVal = range.getStart();
                    }
                }
                else
                {
                    for (int b = startBlock; b < endBlock && b < peakCache.size(); ++b)
                    {
                        auto range = peakCache.getReference (b);
                        if (range.getEnd() > maxVal) maxVal = range.getEnd();
                        if (range.getStart() < minVal) minVal = range.getStart();
                    }
                }
            }

            // Clamp to unity so over-0dBFS (hot) samples can't overflow the
            // display window vertically at the default zoom.
            maxVal = juce::jlimit (-1.0f, 1.0f, maxVal);
            minVal = juce::jlimit (-1.0f, 1.0f, minVal);

            auto xPos = displayBounds.getX() + (float) pixel;
            const auto verticalSpan = displayHeight * 0.48f * waveformVerticalScale;
            auto topY = centreY - maxVal * verticalSpan;
            auto bottomY = centreY - minVal * verticalSpan;
            
            bottomYs.add (bottomY);

            if (! topStarted)
            {
                topPath.startNewSubPath (xPos, topY);
                topStarted = true;
            }
            else
            {
                topPath.lineTo (xPos, topY);
            }
        }

        juce::Path combined;
        combined.addPath (topPath);

        for (int i = bottomYs.size() - 1; i >= 0; --i)
        {
            auto xPos = displayBounds.getX() + (float) (i * step);
            combined.lineTo (xPos, bottomYs.getReference (i));
        }

        combined.closeSubPath();
        waveformPath = std::move (combined);
    }
    
    void updatePeakCache()
    {
        peakCache.clearQuick();
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
            return;

        const auto& buffer = sampleData->buffer;
        int numSamples = buffer.getNumSamples();
        int numChannels = buffer.getNumChannels();
        int numBlocks = (numSamples + cacheBlockSize - 1) / cacheBlockSize;
        
        peakCache.ensureStorageAllocated (numBlocks);

        for (int b = 0; b < numBlocks; ++b)
        {
            int startTarget = b * cacheBlockSize;
            int endTarget = juce::jmin (startTarget + cacheBlockSize, numSamples);
            float maxV = 0.0f;
            float minV = 0.0f;
            
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buffer.getReadPointer (ch);
                for (int s = startTarget; s < endTarget; ++s)
                {
                    auto v = data[s];
                    if (v > maxV) maxV = v;
                    if (v < minV) minV = v;
                }
            }
            peakCache.add ({ minV, maxV });
        }
    }

    AudioPluginAudioProcessor& processor;
    juce::Path waveformPath;
    juce::Array<juce::Range<float>> peakCache;
    const int cacheBlockSize = 256;
    bool isDragOver = false;
    bool wasPlayingLastTick = false;
    bool isHoveringDisplay = false;
    bool isSelectingAnalysisRegion = false;
    bool isHoldingToPlay = false;
    float zoomLevel = 0.0f;
    float targetZoomLevel = 0.0f;
    float scrollPosition = 0.0f;
    float targetScrollPosition = 0.0f;
    float waveformVerticalScale = defaultWaveformVerticalScale;
    float targetWaveformVerticalScale = defaultWaveformVerticalScale;

    struct ChopAnimationState
    {
        float currentHoverAlpha = 0.0f;
        float currentSelectAlpha = 0.0f;
        float currentTriggerPulse = 0.0f;
    };
    std::map<int, ChopAnimationState> chopAnimations;
    SmoothHoverButton verticalMinusButton;
    SmoothHoverButton verticalPlusButton;
    juce::ScrollBar horizontalScrollBar;
    bool updatingHorizontalScrollBar = false;
    float hoveredDisplayX = 0.0f;
    int hoveredChopId = -1;
    float hoverAnimationAlpha = 0.0f;
    float lastPaintedHoverAlpha = -1.0f;
    float lastPaintedHoverX = -1.0f;
    double analysisSelectionStartSample = 0.0;
    double analysisSelectionCurrentSample = 0.0;
    double lastPaintedPlayheadSample = -1.0;
    uint64_t lastTempoUiRevision = 0;
    uint64_t lastObservedChopTriggerRevision = 0;
    float scanAnimFrame = 0.0f;

    // One-shot "trace acquire" reveal when a genuinely new sample lands:
    // 0..1 = sweep (waveform materialises behind a bright scan head),
    // 1..1.25 = lock flash, 2 = idle. Identity-guarded so a stem-mute remix
    // (same file/length/rate, new buffer) doesn't replay it.
    float loadAnimPhase = 2.0f;
    juce::String loadAnimSampleId;

    bool isLoadRevealActive() const noexcept { return loadAnimPhase <= 1.25f; }

    float loadRevealSweepX (juce::Rectangle<float> bounds) const noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, loadAnimPhase);
        const float eased = 1.0f - std::pow (1.0f - t, 3.0f);   // ease-out cubic
        return bounds.getX() + 4.0f + eased * (bounds.getWidth() - 8.0f);
    }
    int   animHz = waveformRefreshHz;
    bool isAnimating = false;
    bool wasAnimating = false;

    // Hold-to-export drag state
    int  holdChopId      = -1;
    int  holdTickCount   = 0;
    bool exportDragReady = false;
    bool exportDragFired = false;
    int holdTicksRequired() const noexcept { return 2 * animHz; } // 2 s hold at the active frame rate

    // Floating export button (drag handle + click-to-save). This is the
    // discoverable affordance: it appears over the selected chop so users
    // don't have to know about the 2 s hold gesture. Dragging it starts the
    // OS file drag into the DAW; a plain click opens a Save-As dialog.
    bool  exportButtonHovered   = false;
    bool  exportButtonPressed   = false;
    bool  exportButtonDragArmed = false;
    juce::Point<float> exportButtonPressPos;
    float exportButtonAppear    = 0.0f; // 0->1 pop-in when the target chop changes
    float exportButtonPulse     = 0.0f; // continuous breathing phase
    int   exportTargetChopId    = -1;   // chop the button acts on: selection OR last MIDI trigger
    int   lastSeenSelectedId    = -1;   // detects selection changes
    std::unique_ptr<juce::FileChooser> chopExportChooser;

    // Warp-mode drag state (step 9). markerIndex < 0 means nothing being dragged.
    int  warpDragChopId      = -1;
    int  warpDragMarkerIndex = -1;
    bool warpDragRetarget    = false;
    bool warpDragSnap        = true;
    static constexpr float kWarpHitTestPixels = 8.0f;

    // Edge-resize state for the selected chop. edgeKind: 0 = none, 1 = left,
    // 2 = right. While a drag is active, edgeDragChopId stays >= 0; the live
    // drag target is in source samples.
    int    edgeHoverKind     = 0;
    int    edgeDragChopId    = -1;
    int    edgeDragKind      = 0;
    double edgeDragLiveSample = 0.0;
    bool   edgeDragBarSnap = false;
    static constexpr float kEdgeHitTestPixels = 6.0f;
};

class TransportSectionComponent final : public juce::Component,
                                        private juce::Timer
{
public:
    ~TransportSectionComponent() override { stopTimer(); }

    // Knob size for the condensed band's chop row (label + knob fit ~70px).
    static constexpr int bandKnobDiameter = 48;

    explicit TransportSectionComponent (AudioPluginAudioProcessor& p)
        : processor (p),
          timeDisplay ("", "00:00:00", 16.0f, "timeBox"),
          tempoDisplay ("BPM", "--.-", 14.0f, "tempoBox"),
          keyDisplay ("KEY", "--", 13.0f, "keyBox"),
          cueKnob ("CUE", bandKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xff00c950)),
          gainKnob ("GAIN", bandKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xfffb2c36)),
          pitchKnob ("PITCH", bandKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xfff6339a))
    {
        setBufferedToImage (false);
        startTimerHz (transportRefreshHz);

        configureButton (playButton, "", textPrimary);
        configureButton (pauseButton, "", textPrimary);
        configureButton (stopButton, "", textPrimary);
        configureButton (halfSpeedButton, "HALF\nTIME", textPrimary.withAlpha (0.90f));
        configureButton (chopTransientsButton, "CHOP @ TRANS.", textPrimary.withAlpha (0.75f));
        configureButton (barsButton, "# OF BARS", textPrimary.withAlpha (0.75f));
        configureButton (loadButton, "LOAD SAMPLE", textPrimary.withAlpha (0.75f));
        playButton.getProperties().set ("cueStyle", "transportSquare");
        pauseButton.getProperties().set ("cueStyle", "transportSquare");
        stopButton.getProperties().set ("cueStyle", "transportSquare");
        playButton.getProperties().set ("cueIcon", "play");
        pauseButton.getProperties().set ("cueIcon", "pause");
        stopButton.getProperties().set ("cueIcon", "stop");
        halfSpeedButton.getProperties().set ("cueStyle", "halfTime");
        halfSpeedButton.setClickingTogglesState (true);
        halfSpeedButton.onClick = [this]
        {
            const auto isEnabled = halfSpeedButton.getToggleState();
            cue::isHalfTimeActive = isEnabled;
            processor.setHalfTimeEnabled (isEnabled);
            if (onModeThemeChanged)
                onModeThemeChanged();
            else if (auto* parent = getParentComponent())
            {
                parent->repaint();
                for (auto* c : parent->getChildren())
                {
                    c->repaint();
                    for (auto* cc : c->getChildren())
                        cc->repaint();
                }
            }
        };
        chopTransientsButton.getProperties().set ("cueStyle", "flatAction");
        chopTransientsButton.onClick = [this]
        {
            using Sensitivity = AudioPluginAudioProcessor::TransientSensitivity;

            juce::PopupMenu menu;
            menu.addSectionHeader ("Chop at transients");

            // id == sensitivity + 1; tick the last-used level.
            const auto addLevel = [&] (Sensitivity level, const juce::String& label)
            {
                const int id = (int) level + 1;
                const bool ticked = lastTransientSensitivity == (int) level;
                menu.addItem (id, label, true, ticked);
            };

            addLevel (Sensitivity::Light,  "Light  -  heavy hits only");
            addLevel (Sensitivity::Medium, "Medium  -  kicks + snares");
            addLevel (Sensitivity::Fine,   "Fine  -  busy / fills");

            // PopupMenus don't inherit a component's L&F; point it at ours so
            // the smoked-glass styling applies (default falls back to global).
            menu.setLookAndFeel (&getLookAndFeel());
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetComponent (&chopTransientsButton),
                                [this] (int chosen)
                                {
                                    if (chosen <= 0)
                                        return;

                                    const auto level = (Sensitivity) (chosen - 1);
                                    lastTransientSensitivity = chosen - 1;
                                    processor.chopAtTransients (level);
                                });
        };
        barsButton.getProperties().set ("cueStyle", "flatAction");
        loadButton.getProperties().set ("cueStyle", "flatAction");

        playButton.setTooltip ("Play the selected chop from its cue point. Click a chop on the waveform first to pick which one plays.");
        pauseButton.setTooltip ("Pause playback - press Play to resume from the same spot.");
        stopButton.setTooltip ("Stop playback and return to the beginning of the current chop.");
        halfSpeedButton.setTooltip ("Half-Time: plays at half speed while preserving pitch. Active when lit.");
        chopTransientsButton.setTooltip ("Chop at transients: scans the sample for hits and places a chop marker at each onset. Click to choose sensitivity - Light (heavy hits only), Medium (kicks + snares), or Fine (busy / fills).");
        barsButton.setTooltip ("Sets how many bars each chop covers - cycles 1 / 2 / 4 / 8. Larger = fewer, longer chops.");
        loadButton.setTooltip ("Open a file browser to load a new audio sample (WAV, AIFF, MP3, FLAC, OGG). You can also drag a file onto the waveform.");

        for (juce::TextButton* button : { static_cast<juce::TextButton*> (&playButton),
                                          static_cast<juce::TextButton*> (&pauseButton),
                                          static_cast<juce::TextButton*> (&stopButton),
                                          static_cast<juce::TextButton*> (&halfSpeedButton),
                                          static_cast<juce::TextButton*> (&chopTransientsButton),
                                          static_cast<juce::TextButton*> (&barsButton),
                                          static_cast<juce::TextButton*> (&loadButton) })
            addAndMakeVisible (*button);

        halfSpeedButton.setToggleState (processor.getHalfTimeEnabled(), juce::dontSendNotification);
        cue::isHalfTimeActive = processor.getHalfTimeEnabled();

        configureButton (warpButton, "WARP", textPrimary.withAlpha (0.85f));
        warpButton.getProperties().set ("cueStyle", "flatAction");
        warpButton.setClickingTogglesState (true);
        warpButton.setToggleState (processor.isWarpModeActive(), juce::dontSendNotification);
        cue::isWarpModeActive = processor.isWarpModeActive();
        warpButton.setTooltip ("WARP mode: click inside a chop to drop a warp marker on the audio. "
                                "The marker auto-snaps to the nearest grid division.");
        warpButton.onClick = [this]
        {
            const auto active = warpButton.getToggleState();
            processor.setWarpModeActive (active);
            cue::isWarpModeActive = active;
            if (onModeThemeChanged)
                onModeThemeChanged();
            else if (auto* parent = getParentComponent())
            {
                parent->repaint();
                for (auto* c : parent->getChildren())
                    c->repaint();
            }
            if (onWarpToggled)
                onWarpToggled (active);
        };
        addAndMakeVisible (warpButton);

        configureButton (clearWarpButton, "CLEAR ALL", textPrimary.withAlpha (0.85f));
        clearWarpButton.getProperties().set ("cueStyle", "flatAction");
        clearWarpButton.setTooltip ("Clear all warp markers for the currently selected chop.");
        clearWarpButton.onClick = [this]
        {
            if (const auto state = processor.getChopState())
            {
                if (state->selectedChopId >= 0)
                {
                    processor.clearChopWarpMarkers (state->selectedChopId);
                    if (auto* parent = getParentComponent())
                    {
                        parent->repaint();
                        for (auto* c : parent->getChildren())
                            c->repaint();
                    }
                }
            }
        };
        addAndMakeVisible (clearWarpButton);

        configureButton (octDownButton, "OCT -", textPrimary.withAlpha (0.85f));
        configureButton (octUpButton,   "OCT +", textPrimary.withAlpha (0.85f));
        octDownButton.getProperties().set ("cueStyle", "flatAction");
        octUpButton.getProperties().set ("cueStyle", "flatAction");
        octDownButton.setTooltip ("Shift the MIDI note mapping down one octave. Use this to reach the chops if your keyboard has no octave buttons.");
        octUpButton.setTooltip ("Shift the MIDI note mapping up one octave. Use this to reach the chops if your keyboard has no octave buttons.");
        octDownButton.onClick = [this]
        {
            processor.setMidiOctaveOffset (processor.getMidiOctaveOffset() + 1);
            updateOctaveControls();
            repaint();
        };
        octUpButton.onClick = [this]
        {
            processor.setMidiOctaveOffset (processor.getMidiOctaveOffset() - 1);
            updateOctaveControls();
            repaint();
        };
        addAndMakeVisible (octDownButton);
        addAndMakeVisible (octUpButton);

        warpDivisionCombo.setTooltip ("Snap newly placed warp markers to this grid division.");
        warpDivisionCombo.addItem ("BAR",  AudioPluginAudioProcessor::WarpDivision_Bar       + 1);
        warpDivisionCombo.addItem ("BEAT", AudioPluginAudioProcessor::WarpDivision_Beat      + 1);
        warpDivisionCombo.addItem ("1/2",  AudioPluginAudioProcessor::WarpDivision_HalfBeat  + 1);
        warpDivisionCombo.addItem ("1/16", AudioPluginAudioProcessor::WarpDivision_Sixteenth + 1);
        warpDivisionCombo.setSelectedId (processor.getWarpDivision() + 1, juce::dontSendNotification);
        warpDivisionCombo.onChange = [this]
        {
            const auto id = warpDivisionCombo.getSelectedId();
            if (id > 0)
                processor.setWarpDivision (id - 1);
        };
        addAndMakeVisible (warpDivisionCombo);

        addAndMakeVisible (timeDisplay);
        timeDisplay.setTooltip ("Shows total sample length before playback starts; shows elapsed time while playing.");
        addAndMakeVisible (tempoDisplay);
        tempoDisplay.setClickHandler ([this] { showTempoEntryDialog(); });
        addAndMakeVisible (keyDisplay);
        keyDisplay.setTooltip ("Detected musical key + Camelot code. Click to correct it.");
        keyDisplay.setClickHandler ([this] { showKeyOverrideMenu(); });
        // Host-sync toggle sits with the displays it relates to.
        configureButton (syncButton, "SYNC TO DAW", textMuted);
        syncButton.getProperties().set ("cueStyle", "utilitySync");
        syncButton.setClickingTogglesState (true);
        syncButton.setTooltip ("SYNC: locks playback speed to your DAW's host BPM automatically. Disable for free-running playback.");
        addAndMakeVisible (syncButton);

        // Per-chop voicing knobs — they belong in the CHOP CONTROLS panel.
        cueKnob.getSlider().setRange (0.0, 100.0, 0.1);
        cueKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        cueKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        cueKnob.getSlider().setTextValueSuffix (" %");
        cueKnob.captureCurrentValueAsDefault();
        cueKnob.getSlider().setTooltip ("CUE: sets where playback starts inside this chop - 0% = chop start, 100% = chop end. Looping always returns to this point. Alt-click to reset.");
        addAndMakeVisible (cueKnob);

        gainKnob.getSlider().setRange (-24.0, 12.0, 0.1);
        gainKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        gainKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        gainKnob.getSlider().setTextValueSuffix (" dB");
        gainKnob.captureCurrentValueAsDefault();
        gainKnob.getSlider().setTooltip ("GAIN: volume for this chop only (-24 to +12 dB). Does not affect other chops. Alt-click to reset to 0 dB.");
        addAndMakeVisible (gainKnob);

        pitchKnob.getSlider().setRange (-12.0, 12.0, 0.1);
        pitchKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        pitchKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        pitchKnob.getSlider().setTextValueSuffix (" st");
        pitchKnob.captureCurrentValueAsDefault();
        pitchKnob.getSlider().setTooltip ("PITCH (per-chop): pitch shift for this chop only, -12 to +12 semitones. Stacks on top of the global PITCH knob under the waveform. Alt-click to reset.");
        addAndMakeVisible (pitchKnob);

        updateDisplays();
    }

    void paint (juce::Graphics& g) override
    {
        auto chopPanel = getChopPanelBounds().toFloat();

        fillGlassRounded (g, *this, chopPanel, mediumCorner);

        drawPanelHole (g, { chopPanel.getX() + 13.0f, chopPanel.getCentreY() }, 6.0f);
        drawPanelHole (g, { chopPanel.getRight() - 13.0f, chopPanel.getCentreY() }, 6.0f);

        auto transportPanel = getTransportPanelBounds().toFloat();

        fillGlassRounded (g, *this, transportPanel, mediumCorner);

        drawPanelHole (g, { transportPanel.getX() + 13.0f, transportPanel.getCentreY() }, 6.0f);
        drawPanelHole (g, { transportPanel.getRight() - 13.0f, transportPanel.getCentreY() }, 6.0f);

        // Keep the section badge in its own left-hand bay instead of
        // straddling the panel seam above the knob tick rings.
        auto badgeBounds = juce::Rectangle<float> (chopPanel.getX() + 34.0f,
                                                   chopPanel.getCentreY() - 9.0f,
                                                   122.0f,
                                                   18.0f);
        fillGlassRounded (g, *this, badgeBounds, 6.0f);

        g.setColour (themedTitleColour (accentOrange));
        g.setFont (heavyFont (10.8f).withExtraKerningFactor (0.08f));
        g.drawText ("CHOP CONTROLS",
                    badgeBounds.toNearestInt().withY ((int) std::round (badgeBounds.getY() - 1.0f)),
                    juce::Justification::centred, false);

        g.setColour (glassTextMuted.withAlpha (0.85f));
        g.setFont (monoFont (8.5f).withExtraKerningFactor (0.06f));
        {
            // Centre the mapping hint under the CHOP/BARS pair on the left.
            constexpr int centerBlockWidth = 140 + 12 + 120;
            constexpr int centerBlockX = 34 + 122 + 24;
            g.drawText (getMidiMappingText(),
                        juce::Rectangle<int> (centerBlockX,
                                              getChopPanelBounds().getBottom() - 13,
                                              centerBlockWidth, 13),
                        juce::Justification::centred, false);
        }

        // Recessed CUERACK well around the warp edit cluster.
        auto warpGroupBounds = warpClusterBounds().toFloat().expanded (6.0f, 4.0f);

        g.setColour (blackPanel.withAlpha (0.6f));
        g.fillRoundedRectangle (warpGroupBounds, 2.0f);
        g.setColour (borderMid);
        g.drawRoundedRectangle (warpGroupBounds.reduced (0.5f), 2.0f, 1.0f);

        // Accent rim (turns violet when warp mode is active)
        g.setColour (accentOrange.withAlpha (0.35f));
        g.drawRoundedRectangle (warpGroupBounds, 2.0f, 1.0f);
    }

    void resized() override
    {
        // ---- Row 1: chop controls directly beneath the global knobs --------
        const auto chopPanel = getChopPanelBounds();

        // Keep clear of the corner rivets (drawn at x = 13 +- 6 in paint()).
        const int sideMargin = 34;

        const int knobH = bandKnobDiameter + 19;
        const int knobY = chopPanel.getY() + juce::jmax (0, (chopPanel.getHeight() - knobH) / 2);
        constexpr int knobGap = 22;
        constexpr int knobClusterWidth = bandKnobDiameter * 3 + knobGap * 2;

        int knobX = (getWidth() - knobClusterWidth) / 2;
        cueKnob.setBounds (knobX, knobY, bandKnobDiameter, knobH);
        knobX += bandKnobDiameter + knobGap;
        gainKnob.setBounds (knobX, knobY, bandKnobDiameter, knobH);
        knobX += bandKnobDiameter + knobGap;
        pitchKnob.setBounds (knobX, knobY, bandKnobDiameter, knobH);

        const int buttonH = 44;
        const int buttonY = chopPanel.getY() + (chopPanel.getHeight() - buttonH) / 2;

        // Chop-generation tools occupy the left bay, leaving the centred
        // per-chop knobs aligned beneath the centred global-knob cluster.
        constexpr int chopBadgeWidth = 122;
        constexpr int badgeToToolsGap = 24;
        const int centerBlockX = sideMargin + chopBadgeWidth + badgeToToolsGap;
        chopTransientsButton.setBounds (centerBlockX, buttonY, 140, buttonH);
        barsButton.setBounds (centerBlockX + 152, buttonY, 120, buttonH);

        // Warp cluster, right-aligned, with stacked octave buttons at the edge.
        const auto warp = warpClusterBounds();
        warpButton.setBounds (warp.getX(), warp.getY(), 88, buttonH);
        clearWarpButton.setBounds (warp.getX() + 96, warp.getY(), 88, buttonH);
        warpDivisionCombo.setBounds (warp.getX() + 192, warp.getY(), 84, buttonH);

        const int octX = getWidth() - sideMargin - 44;
        octDownButton.setBounds (octX, buttonY, 44, 20);
        octUpButton.setBounds (octX, buttonY + 24, 44, 20);

        // ---- Row 2: load | transport | half-time + sync | displays ---------
        const auto transportPanel = getTransportPanelBounds();
        const int transportCenterY = transportPanel.getCentreY();
        const int totalW = getWidth() - (sideMargin * 2);

        constexpr int loadW = 120;
        constexpr int innerTransportGap = 8;
        constexpr int transportGroupW = 48 * 3 + innerTransportGap * 2; // 160
        constexpr int halfW = 64;
        constexpr int syncW = 84;
        constexpr int modeGroupW = halfW + innerTransportGap + syncW; // half-time + sync pair
        constexpr int innerDisplayGap = 8;
        constexpr int displaysGroupW = 150 + innerDisplayGap + 76 + innerDisplayGap + 76; // 318

        const int sumBlockWidths = loadW + transportGroupW + modeGroupW + displaysGroupW;
        const float blockGap = juce::jmax (10.0f, (float) (totalW - sumBlockWidths) / 3.0f);

        float currentX = (float) sideMargin;

        loadButton.setBounds (juce::roundToInt (currentX), transportCenterY - 24, loadW, 48);
        currentX += (float) loadW + blockGap;

        playButton.setBounds (juce::roundToInt (currentX), transportCenterY - 24, 48, 48);
        pauseButton.setBounds (juce::roundToInt (currentX + 48 + innerTransportGap), transportCenterY - 24, 48, 48);
        stopButton.setBounds (juce::roundToInt (currentX + 2 * (48 + innerTransportGap)), transportCenterY - 24, 48, 48);
        currentX += (float) transportGroupW + blockGap;

        // Playback-mode pair: HALF TIME with SYNC TO DAW right beside it.
        halfSpeedButton.setBounds (juce::roundToInt (currentX), transportCenterY - 24, halfW, 48);
        syncButton.setBounds (juce::roundToInt (currentX + halfW + innerTransportGap), transportCenterY - 24, syncW, 48);
        currentX += (float) modeGroupW + blockGap;

        timeDisplay.setBounds (juce::roundToInt (currentX), transportCenterY - 24, 150, 48);
        tempoDisplay.setBounds (juce::roundToInt (currentX + 150 + innerDisplayGap), transportCenterY - 24, 76, 48);
        keyDisplay.setBounds (juce::roundToInt (currentX + 150 + innerDisplayGap + 76 + innerDisplayGap), transportCenterY - 24, 76, 48);
    }

    juce::TextButton& getLoadButton() noexcept { return loadButton; }
    juce::TextButton& getPlayButton() noexcept { return playButton; }
    juce::TextButton& getPauseButton() noexcept { return pauseButton; }
    juce::TextButton& getStopButton() noexcept { return stopButton; }
    juce::TextButton& getBarsButton() noexcept { return barsButton; }
    juce::Slider& getCueSlider() noexcept { return cueKnob.getSlider(); }
    juce::Slider& getGainSlider() noexcept { return gainKnob.getSlider(); }
    juce::Slider& getPitchSlider() noexcept { return pitchKnob.getSlider(); }
    juce::TextButton& getSyncButton() noexcept { return syncButton; }
    void refreshDisplays() { updateDisplays(); }

    // Re-apply cached colours after a theme swap.
    void refreshColours()
    {
        cueKnob.refreshColours();
        gainKnob.refreshColours();
        pitchKnob.refreshColours();
        repaint();
    }
    std::function<void (double)> onTempoEntered;

    // Fired when HALF-TIME or WARP toggles, so the editor can re-apply the
    // theme (including the dynamic warp/halftime tints) across the whole UI.
    std::function<void()> onModeThemeChanged;
    std::function<void (bool)> onWarpToggled;

private:
    void timerCallback() override
    {
        if (! shouldRunRealtimeUi (*this))
            return;

        updateDisplays();
    }

    void updateDisplays()
    {
        syncWarpAccentState();
        updateTimeDisplay();
        updateTempoDisplay();
        updateKeyDisplay();
        updateChopControls();
        updateOctaveControls();
    }

    // Mirrors the selected chop's cue/gain/pitch into the knobs and greys
    // them out when nothing is selected.
    void updateChopControls()
    {
        const auto chopState = processor.getChopState();
        const auto* selectedChop = [&chopState] () -> const AudioPluginAudioProcessor::ChopDefinition*
        {
            if (chopState == nullptr || chopState->selectedChopId < 0)
                return nullptr;

            for (const auto& chop : chopState->chops)
            {
                if (chop.id == chopState->selectedChopId)
                    return &chop;
            }

            return nullptr;
        }();

        const bool hasSelectedChop = selectedChop != nullptr;
        cueKnob.getSlider().setEnabled (hasSelectedChop);
        gainKnob.getSlider().setEnabled (hasSelectedChop);
        pitchKnob.getSlider().setEnabled (hasSelectedChop);

        if (! hasSelectedChop)
        {
            cueKnob.getSlider().setValue (0.0, juce::dontSendNotification);
            gainKnob.getSlider().setValue (0.0, juce::dontSendNotification);
            pitchKnob.getSlider().setValue (0.0, juce::dontSendNotification);
            return;
        }

        const auto chopLength = juce::jmax (1, selectedChop->endSample - selectedChop->startSample - 1);
        const auto cuePercent = (double) selectedChop->cueOffsetSamples / (double) chopLength * 100.0;
        cueKnob.getSlider().setValue (cuePercent, juce::dontSendNotification);
        gainKnob.getSlider().setValue ((double) selectedChop->gainDecibels, juce::dontSendNotification);
        pitchKnob.getSlider().setValue ((double) selectedChop->pitchSemitones, juce::dontSendNotification);
    }

    void syncWarpAccentState()
    {
        const auto active = processor.isWarpModeActive();
        if (warpButton.getToggleState() != active)
            warpButton.setToggleState (active, juce::dontSendNotification);

        if (cue::isWarpModeActive == active)
            return;

        cue::isWarpModeActive = active;

        if (auto* parent = getParentComponent())
        {
            parent->repaint();
            for (auto* c : parent->getChildren())
            {
                c->repaint();
                for (auto* cc : c->getChildren())
                    cc->repaint();
            }
        }
        else
        {
            repaint();
        }
    }

    void updateTimeDisplay()
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0 || sampleData->sampleRate <= 0.0)
        {
            timeDisplay.setValueText ("00:00:00");
            return;
        }

        const auto durationSeconds = (double) sampleData->buffer.getNumSamples() / sampleData->sampleRate;
        const auto playbackSeconds = processor.getPlaybackSamplePosition() / sampleData->sampleRate;
        const auto displaySeconds = (processor.isPlaying() || playbackSeconds > 0.0) ? playbackSeconds
                                                                                      : durationSeconds;

        timeDisplay.setValueText (formatSampleTime (displaySeconds));
    }

    void updateTempoDisplay()
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
        {
            tempoDisplay.setScanning (false);
            tempoDisplay.setValueText ("--.-");
            tempoDisplay.setTooltip ("Load a sample - the plugin will auto-detect its BPM and build the chop grid.");
            return;
        }

        if (processor.isTempoAnalysisInProgress())
        {
            tempoDisplay.setScanning (true);
            tempoDisplay.setTooltip ("Scanning for beat positions - this takes a moment.");
            return;
        }

        tempoDisplay.setScanning (false);

        const auto analysis = processor.getTempoAnalysis();
        if (analysis == nullptr || analysis->estimatedBpm <= 0.0)
        {
            tempoDisplay.setValueText ("--.-");
            tempoDisplay.setTooltip ("Tempo detection was not confident.");
            return;
        }

        const auto adjustedBpm = analysis->estimatedBpm + (double) processor.getGridBpmTrim();
        tempoDisplay.setValueText (formatDetectedTempo (adjustedBpm));

        juce::String tooltip = "Detected: " + juce::String (analysis->estimatedBpm, 2)
                             + " BPM  Adjusted: " + juce::String (adjustedBpm, 2)
                             + " BPM\nConfidence: " + juce::String (juce::roundToInt (analysis->confidence * 100.0f)) + "%";

        if (analysis->likelyDrifting)
            tooltip << "\nFeel: drifting live tempo";
        else
            tooltip << "\nFeel: mostly steady";

        tooltip << "\nUse the TEMPO trim knob (under the waveform) to nudge the grid if chops feel slightly off-beat.";

        tempoDisplay.setTooltip (tooltip);
    }

    void showKeyOverrideMenu()
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };

        const auto current = processor.getDetectedKey();

        juce::PopupMenu major, minor;
        for (int i = 0; i < 12; ++i)
        {
            const bool tickMaj = current.valid && current.isMajor && current.rootIndex == i;
            major.addItem (i + 1, names[i], true, tickMaj);

            const bool tickMin = current.valid && ! current.isMajor && current.rootIndex == i;
            minor.addItem (i + 13, juce::String (names[i]) + "m", true, tickMin);
        }

        juce::PopupMenu menu;
        menu.addSectionHeader ("Correct key");
        menu.addSubMenu ("Major", major);
        menu.addSubMenu ("Minor", minor);

        menu.setLookAndFeel (&getLookAndFeel());

        juce::Component::SafePointer<TransportSectionComponent> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (keyDisplay),
                            [safeThis] (int result)
                            {
                                if (result <= 0 || safeThis == nullptr)
                                    return;

                                const bool isMajor = result <= 12;
                                const int rootIndex = isMajor ? result - 1 : result - 13;
                                safeThis->processor.setUserKeyOverride (rootIndex, isMajor);
                            });
    }

    void showTempoEntryDialog()
    {
        const auto analysis = processor.getTempoAnalysis();
        if (analysis == nullptr || analysis->estimatedBpm <= 0.0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                    "No Tempo",
                                                    "Load a sample and wait for BPM detection first.",
                                                    "OK",
                                                    this);
            return;
        }

        const auto currentBpm = analysis->estimatedBpm + (double) processor.getGridBpmTrim();
        auto* alert = new juce::AlertWindow ("Set BPM",
                                             "Enter the tempo for the loaded sample:",
                                             juce::AlertWindow::NoIcon,
                                             this);

        alert->addTextEditor ("manualBpm", juce::String (currentBpm, 2), {});
        alert->addButton ("Set", 1, juce::KeyPress (juce::KeyPress::returnKey));
        alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<TransportSectionComponent> safeThis (this);
        juce::Component::SafePointer<juce::AlertWindow> safeAlert (alert);

        alert->enterModalState (true,
                                juce::ModalCallbackFunction::create (
                                    [safeThis, safeAlert] (int result)
                                    {
                                        if (result != 1 || safeThis == nullptr || safeAlert == nullptr)
                                            return;

                                        const auto requestedBpm = safeAlert->getTextEditorContents ("manualBpm").getDoubleValue();
                                        if (requestedBpm < 20.0 || requestedBpm > 300.0)
                                        {
                                            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                                                    "Invalid BPM",
                                                                                    "Enter a BPM between 20 and 300.",
                                                                                    "OK",
                                                                                    safeThis);
                                            return;
                                        }

                                        if (safeThis->onTempoEntered != nullptr)
                                            safeThis->onTempoEntered (requestedBpm);
                                    }),
                                true);
    }

    void updateKeyDisplay()
    {
        const auto sampleData = processor.getLoadedSample();
        if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
        {
            keyDisplay.setScanning (false);
            keyDisplay.setValueText ("--");
            keyDisplay.setTooltip ("Load a sample - the plugin will auto-detect its key.");
            return;
        }

        if (processor.isKeyDetectionInProgress())
        {
            keyDisplay.setScanning (true);
            keyDisplay.setTooltip ("Scanning for musical key.");
            return;
        }

        keyDisplay.setScanning (false);

        const auto result = processor.getDetectedKey();
        if (! result.valid)
        {
            keyDisplay.setValueText ("--");
            keyDisplay.setTooltip ("Key detection was not confident.");
            return;
        }

        const auto displayText = juce::String (result.key.c_str()) + " | " + juce::String (result.camelot.c_str());
        keyDisplay.setValueText (displayText);
        keyDisplay.setTooltip ("Detected key: " + displayText);
    }

    // Note name in the plugin's convention (MIDI 36 = C2, i.e. octave = note / 12 - 1).
    static juce::String midiNoteName (int noteNumber)
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        const int octave = noteNumber / 12 - 1;
        return juce::String (names[((noteNumber % 12) + 12) % 12]) + juce::String (octave);
    }

    juce::String getMidiMappingText() const
    {
        const int root = processor.getMidiRootNote();
        return "MIDI: " + midiNoteName (root) + " = chop 1,  "
                        + midiNoteName (root + 2) + " = chop 2 ...";
    }

    void updateOctaveControls()
    {
        // Functions are reversed: "OCT -" raises the offset, "OCT +" lowers it,
        // so each button greys out at the opposite limit.
        octDownButton.setEnabled (processor.getMidiOctaveOffset() < AudioPluginAudioProcessor::midiOctaveOffsetMax);
        octUpButton.setEnabled   (processor.getMidiOctaveOffset() > AudioPluginAudioProcessor::midiOctaveOffsetMin);
    }

    juce::Rectangle<int> getChopPanelBounds() const
    {
        return getLocalBounds().removeFromTop (70);
    }

    juce::Rectangle<int> getTransportPanelBounds() const
    {
        constexpr int bottomPanelY = 74;
        return { 0, bottomPanelY, getWidth(), juce::jmax (0, getHeight() - bottomPanelY) };
    }

    // Warp cluster (WARP / CLEAR / division), right-aligned in the chop row with room
    // for the stacked octave buttons at the edge. Shared by paint()/resized().
    juce::Rectangle<int> warpClusterBounds() const
    {
        const auto chopPanel = getChopPanelBounds();
        constexpr int clusterW = 88 + 8 + 88 + 8 + 84; // 276
        constexpr int buttonH = 44;
        const int y = chopPanel.getY() + (chopPanel.getHeight() - buttonH) / 2;
        return { getWidth() - 34 - 44 - 16 - clusterW, y, clusterW, buttonH };
    }

    AudioPluginAudioProcessor& processor;

    // Last sensitivity chosen from the CHOP @ TRANS. menu, so the menu can tick
    // the active choice. -1 = none picked yet (matches no TransientSensitivity).
    int lastTransientSensitivity = -1;

    SmoothHoverButton playButton;
    SmoothHoverButton pauseButton;
    SmoothHoverButton stopButton;
    SmoothAnimatedSwitchButton halfSpeedButton;
    SmoothHoverButton chopTransientsButton;
    SmoothHoverButton barsButton;
    SmoothHoverButton loadButton;
    SmoothHoverButton warpButton;
    SmoothHoverButton clearWarpButton;
    SmoothHoverButton octDownButton;
    SmoothHoverButton octUpButton;
    juce::ComboBox   warpDivisionCombo;

    DisplayBox timeDisplay;
    DisplayBox tempoDisplay;
    DisplayBox keyDisplay;
    LabelledKnob cueKnob;
    LabelledKnob gainKnob;
    LabelledKnob pitchKnob;
    SmoothAnimatedSwitchButton syncButton;
};

// Slim glass strip directly beneath the waveform. Its centred row holds the
// four global waveform/sample controls (ZOOM / SCROLL / TEMPO / PITCH). The
// centred per-chop CUE / GAIN / PITCH row sits directly beneath it.
class WaveformFooterComponent final : public juce::Component
{
public:
    static constexpr int footerKnobDiameter = 52;

    WaveformFooterComponent()
        : zoomKnob ("ZOOM", footerKnobDiameter, 13.0f, "miniColourKnob", lightCueOrange),
          scrollKnob ("SCROLL", footerKnobDiameter, 13.0f, "miniColourKnob", lightCueOrange),
          tempoKnob ("TEMPO", footerKnobDiameter, 13.0f, "miniColourKnob", lightCueOrange),
          globalPitchKnob ("PITCH", footerKnobDiameter, 13.0f, "miniColourKnob", lightCueOrange)
    {
        setBufferedToImage (false);

        zoomKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        zoomKnob.getSlider().setMouseDragSensitivity (preciseMiniKnobDragSensitivity);
        zoomKnob.getSlider().setTooltip ("ZOOM: zoom into the waveform for detail. Use SCROLL to pan when zoomed in. Alt-click to reset.");
        zoomKnob.captureCurrentValueAsDefault();

        scrollKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        scrollKnob.getSlider().setMouseDragSensitivity (getScrollDragSensitivity (0.0f));
        scrollKnob.getSlider().setTooltip ("SCROLL: pan the waveform left/right when zoomed in. Has no effect at zero zoom. Alt-click to reset.");
        scrollKnob.captureCurrentValueAsDefault();

        tempoKnob.getSlider().setRange (-10.0, 10.0, 0.1);
        tempoKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        tempoKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        tempoKnob.getSlider().setTextValueSuffix (" BPM");
        tempoKnob.getSlider().setTooltip ("TEMPO trim: adds a fine BPM offset (-10 to +10 BPM) to shift where chop boundaries fall. Use when chops feel slightly early or late. Alt-click to reset to 0.");
        tempoKnob.captureCurrentValueAsDefault();

        globalPitchKnob.getSlider().setRange (-12.0, 12.0, 0.1);
        globalPitchKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        globalPitchKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        globalPitchKnob.getSlider().setTextValueSuffix (" st");
        globalPitchKnob.getSlider().setTooltip ("PITCH (global): shifts pitch of every chop together, -12 to +12 semitones. The per-chop PITCH knob (in CHOP CONTROLS) adds on top of this. Alt-click to reset to 0.");
        globalPitchKnob.captureCurrentValueAsDefault();

        for (auto* knob : { &zoomKnob, &scrollKnob, &tempoKnob, &globalPitchKnob })
            addAndMakeVisible (*knob);
    }

    // Re-apply cached colours after a theme swap.
    void refreshColours()
    {
        for (auto* knob : { &zoomKnob, &scrollKnob, &tempoKnob, &globalPitchKnob })
            knob->refreshColours();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        fillGlassRounded (g, *this, bounds, mediumCorner);

        const auto centreY = bounds.getCentreY();
        drawPanelHole (g, { bounds.getX() + 13.0f, centreY }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, centreY }, 6.0f);
    }

    void resized() override
    {
        constexpr int topInset = 5;
        const int knobH = juce::jmax (0, juce::jmin (footerKnobDiameter + 19,
                                                     getHeight() - topInset));
        const int knobY = topInset;
        constexpr int gap = 28;
        constexpr int knobCount = 4;
        constexpr int clusterWidth = knobCount * footerKnobDiameter + (knobCount - 1) * gap;

        int x = (getWidth() - clusterWidth) / 2;
        for (auto* knob : { &zoomKnob, &scrollKnob, &tempoKnob, &globalPitchKnob })
        {
            knob->setBounds (x, knobY, footerKnobDiameter, knobH);
            x += footerKnobDiameter + gap;
        }
    }

    juce::Slider& getZoomSlider() noexcept { return zoomKnob.getSlider(); }
    juce::Slider& getScrollSlider() noexcept { return scrollKnob.getSlider(); }
    juce::Slider& getTempoSlider() noexcept { return tempoKnob.getSlider(); }
    juce::Slider& getGlobalPitchSlider() noexcept { return globalPitchKnob.getSlider(); }
    void updateScrollSensitivityForZoom (float zoomControlValue)
    {
        scrollKnob.getSlider().setMouseDragSensitivity (getScrollDragSensitivity (zoomControlValue));
    }

private:
    LabelledKnob zoomKnob;
    LabelledKnob scrollKnob;
    LabelledKnob tempoKnob;
    LabelledKnob globalPitchKnob;
};

// Slim glass strip beneath the waveform with three stem-mute toggles
// (BASS / DRUMS / VOCALS) and a status line. Toggled == muted (the keycap LED
// lights, via the shared "utilitySync" style). Buttons are disabled and dimmed
// until the processor reports stems are ready; the status line shows separation
// progress meanwhile. Reads/writes the processor's setMute*/getMute* +
// isSeparatingStems/areStemsReady/getStemProgress; refresh() is pumped by the
// editor's 30 Hz timer.
class StemRackComponent final : public juce::Component,
                                private juce::Timer
{
public:
    ~StemRackComponent() override { stopTimer(); }

    explicit StemRackComponent (AudioPluginAudioProcessor& p)
        : processorRef (p)
    {
        setBufferedToImage (false);

        auto configureMute = [this] (SmoothAnimatedSwitchButton& b,
                                     const juce::String& label,
                                     const juce::String& tip,
                                     std::function<void (bool)> apply)
        {
            configureButton (b, label, glassTextMuted);
            b.getProperties().set ("cueStyle", "utilitySync");
            b.setClickingTogglesState (true);
            b.setTooltip (tip);
            b.onClick = [&b, fn = std::move (apply)] { fn (b.getToggleState()); };
            addAndMakeVisible (b);
        };

        // The toggle state represents the stem being ACTIVE (playing): LED lit =
        // the stem plays, LED off = muted. So the light stays on while unmuted and
        // turns off to mute. apply() inverts the toggle into the processor's mute flag.
        configureMute (bassBtn, "BASS",
                       "BASS stem. Lit = playing - click to mute (removes it from playback).",
                       [this] (bool active) { processorRef.setMuteBass (! active); });
        configureMute (drumsBtn, "DRUMS",
                       "DRUMS stem. Lit = playing - click to mute (removes it from playback).",
                       [this] (bool active) { processorRef.setMuteDrums (! active); });
        configureMute (vocalsBtn, "VOCALS",
                       "VOCALS stem. Lit = playing - click to mute (removes it from playback).",
                       [this] (bool active) { processorRef.setMuteVocals (! active); });

        // Initialise toggle states from the processor (state recall). Inverted:
        // a stem that is NOT muted shows its LED lit.
        bassBtn.setToggleState (! processorRef.getMuteBass(),   juce::dontSendNotification);
        drumsBtn.setToggleState (! processorRef.getMuteDrums(), juce::dontSendNotification);
        vocalsBtn.setToggleState (! processorRef.getMuteVocals(), juce::dontSendNotification);

        // Manual trigger: separation no longer runs automatically on load (kept the
        // plugin fast to instantiate). One click splits the loaded sample; the heavy
        // ONNX model is built lazily on this first request, off the message thread.
        // Visibility is state-driven in refresh() — it sits in the info column and is
        // hidden while separating, once stems are ready, or when no model is installed.
        configureButton (separateButton, "SEPARATE", textPrimary.withAlpha (0.9f));
        separateButton.getProperties().set ("cueStyle", "separateAction");
        separateButton.setTooltip ("Split this sample into DRUMS / BASS / VOCALS stems so you can mute each one. "
                                   "Runs in the background - the first run takes a few extra seconds while the model loads.");
        separateButton.onClick = [this] { processorRef.requestStemSeparation(); };
        addChildComponent (separateButton); // visibility managed in refresh()

        lastReady = ! processorRef.areStemsReady(); // force the first refresh to apply
        refresh();
    }

    // Pumped by the editor timer: syncs toggle states, button enablement/dim, and
    // the status line to the processor's current separation state.
    void refresh()
    {
        const bool  ready      = processorRef.areStemsReady();
        const bool  separating = processorRef.isSeparatingStems();
        const float progress   = processorRef.getStemProgress();

        // Show the manual SEPARATE trigger only when it can actually do something:
        // a model is installed, a sample is loaded, and we're idle (no stems yet,
        // not already running, and the sample wasn't rejected as too long). The
        // status line is empty in exactly this state, so the button replaces it.
        loadingModel = processorRef.isLoadingStemModel();
        const bool showSeparate = processorRef.areStemModelsAvailable()
                               && processorRef.getLoadedSample() != nullptr
                               && ! ready && ! separating
                               && ! processorRef.wasStemSeparationSkipped();
        if (showSeparate != separateButton.isVisible())
            separateButton.setVisible (showSeparate);

        // Keep toggles in sync with the processor (state restore, fresh-load reset)
        // without firing onClick. setToggleState no-ops when unchanged. Inverted:
        // lit = NOT muted (the stem is playing).
        bassBtn.setToggleState (! processorRef.getMuteBass(),   juce::dontSendNotification);
        drumsBtn.setToggleState (! processorRef.getMuteDrums(), juce::dontSendNotification);
        vocalsBtn.setToggleState (! processorRef.getMuteVocals(), juce::dontSendNotification);

        if (ready != lastReady)
        {
            for (auto* b : { &bassBtn, &drumsBtn, &vocalsBtn })
            {
                b->setEnabled (ready);
                b->setAlpha (ready ? 1.0f : 0.4f);
            }
            lastReady = ready;
        }

        // Drive the running progress animation: store the real target and start/stop
        // the internal smoothing timer as separation begins/ends. Easing of the
        // visible fill and the shimmer sweep happen in timerCallback().
        targetProgress = juce::jlimit (0.0f, 1.0f, progress);
        if (separating != separatingNow)
        {
            separatingNow = separating;
            if (separating)
            {
                displayProgress = targetProgress; // begin from the current value
                sweepPhase      = 0.0f;
                animHz          = animationFrameRateHz();
                startTimerHz (animHz);
            }
            else
            {
                stopTimer();
                repaint();
            }
        }

        juce::String text;
        juce::Colour colour;
        if (! processorRef.areStemModelsAvailable())
        {
            text = "NO MODEL";
            colour = glassTextMuted.withAlpha (0.55f);
        }
        else if (separating)
        {
            text   = "SEPARATING"; // live % and bar are drawn in paint() from displayProgress
            colour = accentOrange;
        }
        else if (ready)
        {
            text = "READY";
            colour = juce::Colour (0xff6ad48a);
        }
        else if (processorRef.wasStemSeparationSkipped())
        {
            text = "SAMPLE TOO LONG";
            colour = glassTextMuted.withAlpha (0.7f);
        }
        else
        {
            text = {}; // idle / no sample — neutral (just the STEMS title)
            colour = glassTextMuted.withAlpha (0.55f);
        }

        if (text != statusText)
        {
            statusText   = text;
            statusColour = colour;
            repaint();
        }
    }

    // Internal pump, live only while separating: eases the visible fill toward the
    // real progress (so chunky per-segment jumps read as one smooth climb) and
    // advances the shimmer sweep so the bar always looks alive.
    void timerCallback() override
    {
        displayProgress += (targetProgress - displayProgress) * frameRateLerp (0.16f, animHz);
        if (std::abs (targetProgress - displayProgress) < 0.0005f)
            displayProgress = targetProgress;

        sweepPhase += frameRateStep (0.015f, animHz); // ~0.9 sweeps/sec at 60 Hz
        if (sweepPhase >= 1.0f)
            sweepPhase -= 1.0f;

        repaint (0, 0, 250, getHeight()); // only the title/status/progress column
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        fillGlassRounded (g, *this, bounds.reduced (0.5f), mediumCorner);

        // Panel title: Syne brand face in the CUERACK accent-title voice.
        g.setColour (themedTitleColour (accentOrange));
        g.setFont (heavyFont (12.0f).withExtraKerningFactor (0.10f));
        g.drawText ("STEMS", juce::Rectangle<int> (14, 6, 150, 16), juce::Justification::centredLeft);

        if (separatingNow)
        {
            paintProgress (g);
            return;
        }

        g.setColour (statusColour);
        g.setFont (monoFont (8.5f).withExtraKerningFactor (0.06f));
        g.drawText (statusText, juce::Rectangle<int> (15, 24, 150, 14), juce::Justification::centredLeft);
    }

    // The "running" loading state: a smoothly-easing orange fill with a moving
    // shimmer sweep, plus a live percentage that counts up alongside the fill.
    void paintProgress (juce::Graphics& g)
    {
        const float p   = juce::jlimit (0.0f, 1.0f, displayProgress);
        const int   pct = juce::roundToInt (p * 100.0f);

        // Label row: state on the left, live % right-aligned over the bar. While the
        // model is still building there is no real progress, so it reads "PREPARING"
        // with no percentage.
        g.setFont (monoFont (8.5f).withExtraKerningFactor (0.06f));
        g.setColour (themedTitleColour (accentOrange));
        g.drawText (loadingModel ? "PREPARING" : "SEPARATING",
                    juce::Rectangle<int> (15, 23, 140, 13), juce::Justification::centredLeft);
        if (! loadingModel)
        {
            g.setColour (textPrimary);
            g.drawText (juce::String (pct) + "%", juce::Rectangle<int> (15, 23, 140, 13), juce::Justification::centredRight);
        }

        // Recessed CUERACK slot.
        const juce::Rectangle<float> track (15.0f, 42.0f, 140.0f, 5.0f);
        const float r = track.getHeight() * 0.5f;
        g.setColour (blackPanel);
        g.fillRoundedRectangle (track, r);
        g.setColour (borderMid);
        g.drawRoundedRectangle (track, r, 1.0f);

        // Model-load phase: no determinate value yet, so sweep an indeterminate
        // highlight across the whole track (gated determinate fill below would draw
        // nothing at 0% and look frozen during the multi-second build).
        if (loadingModel)
        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addRoundedRectangle (track, r);
            g.reduceClipRegion (clip);

            const float sweepW = 70.0f;
            const float sx     = -sweepW + (track.getWidth() + sweepW) * sweepPhase;
            juce::Rectangle<float> sweepR (track.getX() + sx, track.getY(), sweepW, track.getHeight());
            juce::ColourGradient grad (accentOrange.withAlpha (0.0f), sweepR.getX(),     sweepR.getCentreY(),
                                       accentOrange.withAlpha (0.0f), sweepR.getRight(), sweepR.getCentreY(), false);
            grad.addColour (0.5, accentOrange.withAlpha (0.85f));
            g.setGradientFill (grad);
            g.fillRect (sweepR);
            return;
        }

        // Determinate fill + shimmer, clipped to the rounded track so corners stay
        // clean and the sweep never spills past the ends.
        const float fillW = track.getWidth() * p;
        if (fillW > 0.5f)
        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addRoundedRectangle (track, r);
            g.reduceClipRegion (clip);

            // Flat accent fill with a brighter leading edge (no glass shimmer).
            auto fill = track.withWidth (fillW);
            g.setColour (accentOrange);
            g.fillRect (fill);

            g.setColour (accentOrange.brighter (0.5f).withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (fill.getRight() - 2.0f, fill.getY(), 2.0f, fill.getHeight()));
        }
    }

    void resized() override
    {
        // Compact header-dock layout: title/status column on the left, three
        // mute keys filling the rest.
        constexpr int y = 6, gap = 8;
        constexpr int leftEdge = 170;
        const int h = getHeight() - 2 * y;
        const int rightEdge = getWidth() - 12;
        const int w = (rightEdge - leftEdge - 2 * gap) / 3;

        bassBtn.setBounds   (leftEdge,                   y, w, h);
        drumsBtn.setBounds  (leftEdge + (w + gap),       y, w, h);
        vocalsBtn.setBounds (leftEdge + 2 * (w + gap),   y, w, h);

        // SEPARATE replaces the status column while idle.
        separateButton.setBounds (12, 22, 146, 28);
    }

private:
    AudioPluginAudioProcessor& processorRef;
    SmoothAnimatedSwitchButton bassBtn, drumsBtn, vocalsBtn;
    juce::TextButton separateButton; // manual trigger; visibility managed in refresh()
    juce::String statusText;
    juce::Colour statusColour { glassTextMuted };
    bool lastReady = false;
    bool loadingModel = false; // ONNX session building on first request (indeterminate bar)

    // Running-progress animation state (driven by the internal Timer while separating).
    bool  separatingNow   = false;
    float targetProgress  = 0.0f; // real progress reported by the processor [0,1]
    float displayProgress = 0.0f; // eased value actually drawn
    float sweepPhase      = 0.0f; // 0..1 position of the moving shimmer highlight
    int   animHz          = 60;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemRackComponent)
};

// Slim notification strip shown at the top of the editor when the update checker
// finds a newer GitHub Release. "Update" opens the download in the browser (a
// loaded plugin can't replace itself); "Later" suppresses this version.
class UpdateBannerComponent final : public juce::Component
{
public:
    std::function<void()> onDownload;
    std::function<void()> onDismiss;

    UpdateBannerComponent()
    {
        downloadButton.setButtonText ("UPDATE");
        downloadButton.onClick = [this] { if (onDownload) onDownload(); };
        addAndMakeVisible (downloadButton);

        laterButton.setButtonText ("LATER");
        laterButton.onClick = [this] { if (onDismiss) onDismiss(); };
        addAndMakeVisible (laterButton);
    }

    void setVersion (const juce::String& version)
    {
        message = "New version " + version + " available";
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);

        // Flat CUERACK plate with an accent spine.
        g.setColour (cue::panelDark);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (accentOrange.withAlpha (0.85f));
        g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

        g.setColour (accentOrange);
        g.fillRoundedRectangle (bounds.removeFromLeft (4.0f), 2.0f);

        g.setColour (cue::textPrimary.withAlpha (0.92f));
        g.setFont (cue::monoFont (11.0f));
        auto textArea = getLocalBounds().reduced (14, 0)
                            .withTrimmedRight (laterButton.getWidth() + downloadButton.getWidth() + 24);
        g.drawText (message, textArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 6);
        downloadButton.setBounds (area.removeFromRight (78));
        area.removeFromRight (6);
        laterButton.setBounds (area.removeFromRight (64));
    }

private:
    juce::String message { "Update available" };
    cue::SmoothHoverButton downloadButton;
    cue::SmoothHoverButton laterButton;
};
} // namespace cue

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    uiSettingsFile = std::make_unique<juce::PropertiesFile> (cue::uiSettingsOptions());
    fxRackMinimized = uiSettingsFile->getBoolValue ("fxRackMinimized", false);

    processorRef.sampleChangeBroadcaster.addChangeListener (this);
    processorRef.editChangeBroadcaster.addChangeListener (this);

    // Restore the saved UI theme BEFORE anything paints or caches colours —
    // the theme is a per-user preference (settings file), not plugin state.
    cue::applyTheme (uiSettingsFile->getValue ("uiTheme") == "light" ? cue::Theme::light
                                                                      : cue::Theme::dark);

    lookAndFeel = std::make_unique<cue::CueSamplerLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());
    addAndMakeVisible (contentComponent);

    headerComponent = std::make_unique<cue::HeaderComponent> (processorRef);
    waveformDisplayComponent = std::make_unique<cue::WaveformDisplayComponent> (processorRef);
    transportSectionComponent = std::make_unique<cue::TransportSectionComponent> (processorRef);
    waveformFooterComponent = std::make_unique<cue::WaveformFooterComponent>();
    {
        // Mini CUE RACK: panels attach straight to the processor's APVTS;
        // live meters route through the FX engine. Theme the rack subtree
        // to match the sampler before any panel paints.
        cue::FxRackStrip::applyRackTheme (cue::isLight());

        cue::FxRackMeterHooks hooks;
        hooks.compGainReductionDb = [this] { return processorRef.getFxCompGainReductionDb(); };
        hooks.limiterNumBands     = [this] { return processorRef.getFxLimiterNumBands(); };
        hooks.limiterBandGRDb     = [this] (int band) { return processorRef.getFxLimiterBandGRDb (band); };
        hooks.imagerMidSide       = [this] { return processorRef.getFxImagerMidSide(); };
        fxRackStrip = std::make_unique<cue::FxRackStrip> (processorRef.apvts, hooks);
        fxRackStrip->setMinimized (fxRackMinimized);
        fxRackStrip->onMinimizedChanged = [this] (bool minimized)
        {
            setFxRackMinimized (minimized);
        };
        addAndMakeVisible (*fxRackStrip);   // editor-level: lays out in window pixels
    }
    stemRackComponent = std::make_unique<cue::StemRackComponent> (processorRef);

    // Measure the real display refresh rate from the vblank so animations run
    // at the display's native frame rate (120 Hz on ProMotion, 60 Hz otherwise).
    vblankRateMeter = juce::VBlankAttachment (this, [this] (double nowSeconds)
    {
        if (lastVBlankSeconds > 0.0)
            cue::observeVBlankInterval (nowSeconds - lastVBlankSeconds);
        lastVBlankSeconds = nowSeconds;
    });

    transportSectionComponent->onModeThemeChanged = [this] { applyThemeToUi(); };
    transportSectionComponent->getLoadButton().onClick = [this] { loadSampleFromFile(); };
    transportSectionComponent->getPlayButton().onClick = [this] { processorRef.startPlayback(); };
    transportSectionComponent->getPauseButton().onClick = [this] { processorRef.pausePlayback(); };
    transportSectionComponent->getStopButton().onClick = [this] { processorRef.stopPlayback(); };
    transportSectionComponent->getBarsButton().onClick = [this]
    {
        const int current = processorRef.getChopBarsCount();
        const int next = (current == 1) ? 2 : (current == 2) ? 4 : (current == 4) ? 8 : 1;
        processorRef.setChopBarsCount (next);
        const auto label = juce::String (next) + (next == 1 ? " BAR" : " BARS");
        transportSectionComponent->getBarsButton().setButtonText (label);
    };
    transportSectionComponent->onTempoEntered = [this] (double requestedBpm)
    {
        const auto analysis = processorRef.getTempoAnalysis();
        if (analysis == nullptr || analysis->estimatedBpm <= 0.0)
            return;

        const auto trimBpm = (float) (requestedBpm - analysis->estimatedBpm);
        processorRef.setGridBpmTrim (trimBpm);
        waveformFooterComponent->getTempoSlider().setValue ((double) trimBpm, juce::dontSendNotification);
        transportSectionComponent->refreshDisplays();
        waveformDisplayComponent->repaint();
    };
    transportSectionComponent->getCueSlider().onValueChange = [this]
    {
        processorRef.setSelectedChopCueNormalized ((float) transportSectionComponent->getCueSlider().getValue() / 100.0f);
    };
    transportSectionComponent->getGainSlider().onValueChange = [this]
    {
        processorRef.setSelectedChopGainDecibels ((float) transportSectionComponent->getGainSlider().getValue());
    };
    transportSectionComponent->getPitchSlider().onValueChange = [this]
    {
        processorRef.setSelectedChopPitchSemitones ((float) transportSectionComponent->getPitchSlider().getValue());
    };

    waveformFooterComponent->getZoomSlider().onValueChange = [this]
    {
        const auto zoomValue = (float) waveformFooterComponent->getZoomSlider().getValue();
        processorRef.setWaveformZoom (zoomValue);
        waveformFooterComponent->updateScrollSensitivityForZoom (zoomValue);
        waveformDisplayComponent->setZoom (zoomValue);
    };

    waveformFooterComponent->getScrollSlider().onValueChange = [this]
    {
        const auto scrollValue = (float) waveformFooterComponent->getScrollSlider().getValue();
        processorRef.setWaveformScroll (scrollValue);
        waveformDisplayComponent->setScroll (scrollValue);
    };

    waveformDisplayComponent->onZoomChanged = [this] (float z)
    {
        processorRef.setWaveformZoom (z);
        waveformFooterComponent->updateScrollSensitivityForZoom (z);
        waveformFooterComponent->getZoomSlider().setValue ((double) z, juce::dontSendNotification);
    };

    waveformDisplayComponent->onScrollChanged = [this] (float s)
    {
        processorRef.setWaveformScroll (s);
        waveformFooterComponent->getScrollSlider().setValue ((double) s, juce::dontSendNotification);
    };

    waveformFooterComponent->getTempoSlider().onValueChange = [this]
    {
        processorRef.setGridBpmTrim ((float) waveformFooterComponent->getTempoSlider().getValue());
    };

    transportSectionComponent->getSyncButton().setToggleState (processorRef.getSyncToHost(), juce::dontSendNotification);
    transportSectionComponent->getSyncButton().onClick = [this]
    {
        processorRef.setSyncToHost (transportSectionComponent->getSyncButton().getToggleState());
    };

    waveformFooterComponent->getGlobalPitchSlider().onValueChange = [this]
    {
        processorRef.setPitchSemitones ((float) waveformFooterComponent->getGlobalPitchSlider().getValue());
    };

    waveformFooterComponent->getZoomSlider().setValue ((double) processorRef.getWaveformZoom(),
                                                       juce::dontSendNotification);
    waveformFooterComponent->updateScrollSensitivityForZoom (processorRef.getWaveformZoom());
    waveformDisplayComponent->setZoom (processorRef.getWaveformZoom());
    waveformFooterComponent->getScrollSlider().setValue ((double) processorRef.getWaveformScroll(),
                                                         juce::dontSendNotification);
    waveformDisplayComponent->setScroll (processorRef.getWaveformScroll());
    waveformFooterComponent->getTempoSlider().setValue ((double) processorRef.getGridBpmTrim(),
                                                        juce::dontSendNotification);
    waveformFooterComponent->getGlobalPitchSlider().setValue ((double) processorRef.getPitchSemitones(),
                                                              juce::dontSendNotification);
    transportSectionComponent->getBarsButton().setButtonText (juce::String (processorRef.getChopBarsCount())
                                                              + (processorRef.getChopBarsCount() == 1 ? " BAR" : " BARS"));

    juce::Component* sections[] = { headerComponent.get(),
                                    waveformDisplayComponent.get(),
                                    transportSectionComponent.get(),
                                    stemRackComponent.get(),
                                    waveformFooterComponent.get() };

    for (auto* component : sections)
        contentComponent.addAndMakeVisible (*component);

    midiKeyboardComponent = std::make_unique<cue::GlassKeyboard> (processorRef.keyboardState);
    contentComponent.addAndMakeVisible (*midiKeyboardComponent);

    // The CUE orb — CUERACK's GL-raymarched orb — sits beside the header's
    // wordmark as an editor-level sibling (its GL layer composites above the
    // buffered header, which stays static).
    cueOrbComponent = std::make_unique<cue::VisualizerOrb> (processorRef.apvts);
    cueOrbComponent->setBackgroundColour (cue::shellDark);
    contentComponent.addAndMakeVisible (*cueOrbComponent);

    panelShadowEffect.setShadowProperties (defaultShadow);
    waveformDisplayComponent->setComponentEffect (&panelShadowEffect);
    transportSectionComponent->setComponentEffect (&panelShadowEffect);
    stemRackComponent->setComponentEffect (&panelShadowEffect);
    waveformFooterComponent->setComponentEffect (&panelShadowEffect);

    helpOverlayComponent = std::make_unique<cue::HelpOverlayComponent>();
    contentComponent.addChildComponent (*helpOverlayComponent); // invisible by default
    addKeyListener (helpOverlayComponent.get()); // so ? and Escape reach it

    warpHelpOverlayComponent = std::make_unique<cue::WarpHelpOverlayComponent>();
    addChildComponent (*warpHelpOverlayComponent); // invisible by default
    addKeyListener (warpHelpOverlayComponent.get()); // so Escape/Return reach it

    transportSectionComponent->onWarpToggled = [this] (bool active)
    {
        if (warpHelpOverlayComponent != nullptr)
        {
            if (active)
            {
                warpHelpOverlayComponent->setBounds (getLocalBounds());
                warpHelpOverlayComponent->setVisible (true);
                warpHelpOverlayComponent->toFront (true);
            }
            else
            {
                warpHelpOverlayComponent->setVisible (false);
            }
        }
    };

    headerComponent->onHelpRequested = [this]
    {
        const bool nowVisible = ! helpOverlayComponent->isVisible();
        helpOverlayComponent->setVisible (nowVisible);
    };

    headerComponent->onThemeToggled = [this] { toggleTheme(); };

    // A restored light theme must re-skin construction-time colours too
    // (notably the baked-cream wordmark). No-op in the default dark theme.
    if (cue::isLight())
        applyThemeToUi();

    // Software-update banner: a direct child of the editor (drawn over the
    // scaled content), hidden until the checker reports a newer release.
    updateBannerComponent = std::make_unique<cue::UpdateBannerComponent>();
    updateBannerComponent->onDownload = [this]
    {
        const auto info = processorRef.getUpdateChecker().getResult();
        const auto target = info.downloadUrl.isNotEmpty() ? info.downloadUrl : info.pageUrl;
        if (target.isNotEmpty())
            juce::URL (target).launchInDefaultBrowser();
    };
    updateBannerComponent->onDismiss = [this]
    {
        processorRef.getUpdateChecker().skipCurrentVersion();
        updateBannerComponent->setVisible (false);
    };
    addChildComponent (*updateBannerComponent); // invisible by default

    setOpaque (true);
    // CUERACK-style smart resize: free-form (no aspect lock) — drag any edge
    // or corner; the whole chassis re-scales and the FX rack re-flows.
    updateFxRackResizeLimits();
    setResizable (true, true);
    setSize (cue::editorWidth,
             cue::editorHeight + (fxRackMinimized ? cue::FxRackStrip::minimizedHeight
                                                   : cue::fxRackMinBand));
    transportSectionComponent->refreshDisplays();
    startTimerHz (30);

    // Show immediately if a cached result already flags an update; otherwise the
    // 30 Hz timer will reveal it once the background check finishes.
    showUpdateBannerIfNeeded();
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
    stopTimer();
    processorRef.sampleChangeBroadcaster.removeChangeListener (this);
    processorRef.editChangeBroadcaster.removeChangeListener (this);
    setLookAndFeel (nullptr);
    uiSettingsFile->saveIfNeeded();
    uiSettingsFile.reset();
}

void AudioPluginAudioProcessorEditor::timerCallback()
{
    // Cheap atomic poll, always — reveals the banner when the background update
    // check completes after the editor opened.
    if (updateBannerComponent != nullptr && ! updateBannerComponent->isVisible()
        && processorRef.getUpdateChecker().isUpdateAvailable())
        showUpdateBannerIfNeeded();

    if (! cue::shouldRunRealtimeUi (*this))
        return;

    // Feed the CUE orb: output level with the same sqrt mapping CUERACK
    // uses, plus the states that shape and colour it.
    if (cueOrbComponent != nullptr)
    {
        // Output level with the rack's sqrt mapping; the FX fingerprint
        // arrives via the orb's own APVTS listeners. Transport HALF TIME
        // drives the ice palette.
        const auto peak = juce::jlimit (0.0f, 1.0f, processorRef.getOutputMeterLevel());
        cueOrbComponent->setLevel (std::sqrt (peak));
        cueOrbComponent->setHalfTimeActive (processorRef.getHalfTimeEnabled());
    }

    // Poll separation progress/ready → button enablement + status line.
    if (stemRackComponent != nullptr)
        stemRackComponent->refresh();
}

void AudioPluginAudioProcessorEditor::showUpdateBannerIfNeeded()
{
    if (updateBannerComponent == nullptr || updateBannerComponent->isVisible())
        return;

    auto& checker = processorRef.getUpdateChecker();
    if (! checker.isUpdateAvailable())
        return;

    updateBannerComponent->setVersion (checker.getResult().latestVersion);
    updateBannerComponent->setVisible (true);
    updateBannerComponent->toFront (false);
    resized(); // position it now that it's visible
}

float AudioPluginAudioProcessorEditor::getUiScale() const noexcept
{
    // Fully free-form resize, CUERACK-style, with one uniform zoom:
    //  - HEIGHT sets the zoom (clamped so the design width always fits);
    //    extra height beyond the core flows into the FX rack band.
    //  - WIDTH beyond the zoomed design width STRETCHES the content in
    //    design units (see getUiFluidWidth): the waveform column, keyboard,
    //    and rack row all widen, so every drag direction does real work.
    const auto widthScale  = (float) getWidth() / (float) cue::editorWidth;
    const auto rackHeight = fxRackMinimized ? cue::FxRackStrip::minimizedHeight
                                            : cue::fxRackMinBand;
    const auto heightScale = (float) getHeight() / (float) (cue::editorHeight + rackHeight);
    return juce::jlimit (cue::minEditorScale, cue::maxEditorScale, juce::jmin (widthScale, heightScale));
}

void AudioPluginAudioProcessorEditor::updateFxRackResizeLimits()
{
    const auto minimumDesignHeight = cue::editorHeight
                                   + (fxRackMinimized ? cue::FxRackStrip::minimizedHeight
                                                      : cue::fxRackMinBand);
    const auto maximumDesignHeight = fxRackMinimized ? minimumDesignHeight
                                                     : cue::editorHeight + 900;
    setResizeLimits (juce::roundToInt ((float) cue::editorWidth * cue::minEditorScale),
                     juce::roundToInt ((float) minimumDesignHeight * cue::minEditorScale),
                     3400,
                     juce::roundToInt ((float) maximumDesignHeight * cue::maxEditorScale));
}

void AudioPluginAudioProcessorEditor::setFxRackMinimized (bool shouldBeMinimized)
{
    if (fxRackMinimized == shouldBeMinimized)
        return;

    const auto previousRackHeight = fxRackMinimized ? cue::FxRackStrip::minimizedHeight
                                                     : cue::fxRackMinBand;
    const auto previousHeightScale = (float) getHeight()
                                   / (float) (cue::editorHeight + previousRackHeight);
    const auto currentScale = juce::jlimit (cue::minEditorScale, cue::maxEditorScale,
                                            juce::jmin ((float) getWidth() / (float) cue::editorWidth,
                                                        previousHeightScale));

    if (shouldBeMinimized)
        expandedEditorHeight = getHeight();

    fxRackMinimized = shouldBeMinimized;
    uiSettingsFile->setValue ("fxRackMinimized", fxRackMinimized);
    updateFxRackResizeLimits();

    const auto newRackHeight = fxRackMinimized ? cue::FxRackStrip::minimizedHeight
                                                : cue::fxRackMinBand;
    const auto scaledDefaultHeight = juce::roundToInt (
        (float) (cue::editorHeight + newRackHeight) * currentScale);
    const auto targetHeight = ! fxRackMinimized && expandedEditorHeight > 0
                            ? expandedEditorHeight
                            : scaledDefaultHeight;
    setSize (getWidth(), targetHeight);
}

int AudioPluginAudioProcessorEditor::getUiFluidWidth() const noexcept
{
    // The design-space width the layout should fill: at least the base
    // design, wider when the window outgrows the zoomed base width.
    return juce::jmax (cue::editorWidth, juce::roundToInt ((float) getWidth() / getUiScale()));
}

void AudioPluginAudioProcessorEditor::loadSampleFromFile()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Load Sample",
        juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
        "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg");

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file.existsAsFile())
            processorRef.loadAudioFile (file);
    });
}

void AudioPluginAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    // The broadcast already arrives on the message thread, but we defer the UI
    // re-sync one more hop so the setValue() calls below (some with a synchronous
    // notification) can't re-enter the broadcaster mid-callback. That deferral
    // outlives this function, so the lambda must NOT capture a raw `this`: if the
    // host tears the editor down (plugin removed, or DAW closed) while the hop is
    // still sitting in the message queue, running it against the freed editor is a
    // use-after-free that crashes the host. A SafePointer makes the queued lambda
    // a no-op once the editor is gone. (Mirrors the SafePointer pattern already
    // used in TransportSectionComponent's deferred callbacks.)
    juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis, source]
    {
        auto* editor = safeThis.getComponent();
        if (editor == nullptr)
            return; // editor was destroyed before this hop ran — nothing to update

        auto& processor = editor->processorRef;

        if (source == &processor.sampleChangeBroadcaster)
        {
            editor->waveformFooterComponent->getZoomSlider().setValue ((double) processor.getWaveformZoom(),
                                                                       juce::sendNotificationSync);
            editor->waveformFooterComponent->getScrollSlider().setValue ((double) processor.getWaveformScroll(),
                                                                         juce::sendNotificationSync);
        }

        // Always re-sync the global controls from the processor so the UI
        // stays correct after a host-initiated state restore (e.g. FL Studio
        // undo / auto-save recall).
        editor->waveformFooterComponent->getGlobalPitchSlider().setValue ((double) processor.getPitchSemitones(),
                                                                          juce::dontSendNotification);
        editor->waveformFooterComponent->getTempoSlider().setValue ((double) processor.getGridBpmTrim(),
                                                                    juce::dontSendNotification);
        editor->transportSectionComponent->getSyncButton().setToggleState (processor.getSyncToHost(),
                                                                           juce::dontSendNotification);

        editor->transportSectionComponent->refreshDisplays();

        // Light the on-screen keyboard key that maps to the previewed
        // (selected) chop; -1 clears it when nothing is selected.
        if (editor->midiKeyboardComponent != nullptr)
            editor->midiKeyboardComponent->setHighlightedNote (processor.getSelectedChopMidiNote());
    });
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Underlay for any letterboxing: hosts don't always honour the fixed
    // aspect ratio, and getUiScale() fits the smaller axis, so the faceplate
    // can fall short of the window edge. The CUERACK backdrop is a flat warm
    // near-black, so the underlay simply matches it.
    g.fillAll (cue::shellDark);

    juce::Graphics::ScopedSaveState scaleState (g);
    g.addTransform (juce::AffineTransform::scale (getUiScale()));
    const auto fluidW = (float) getUiFluidWidth();

    // One continuous CUERACK chassis: the flat backdrop already covers the
    // whole window (fillAll above); no faceplate plate, no side rails — the
    // core and the FX rack share one surface and one scale.

    // Chassis plate for the on-screen MIDI keyboard along the bottom strip.
    {
        auto keyboardPanel = juce::Rectangle<float> (10.0f, 658.0f, fluidW - 20.0f, 68.0f);
        juce::Path panelPath;
        panelPath.addRoundedRectangle (keyboardPanel, cue::mediumCorner);
        cue::fillGlassPath (g, panelPath, keyboardPanel);
    }

    // Section title: Syne brand face in the CUERACK accent-title voice.
    g.setColour (cue::themedTitleColour (accentOrange));
    g.setFont (cue::heavyFont (12.0f));
    g.drawText ("CHOP STATION", juce::Rectangle<int> (10, 86, 150, 16), juce::Justification::centredLeft, false);

    // Keep interaction guidance outside the waveform screen so chop fills,
    // markers, and grid lines can never obscure it.
    cue::drawHelperText (g,
                         "Click chop: preview/select   Drag edge: resize tempo   Shift-drag edge: snap to bar",
                         juce::Rectangle<int> (170, 84, juce::jmax (0, (int) fluidW - 180), 20),
                         juce::Justification::centred, 10.8f,
                         cue::textMuted.withAlpha (0.9f));
}

void AudioPluginAudioProcessorEditor::resized()
{
    const auto scale = getUiScale();
    const auto fluidW = getUiFluidWidth();
    contentComponent.setTransform (juce::AffineTransform::scale (scale));
    contentComponent.setBounds (0, 0, fluidW, cue::editorHeight);

    // The FX rack band shares the exact same transform as the core, sitting
    // flush beneath it: one surface, one zoom. All leftover window height
    // (in design units) flows into the band, which re-flows CUERACK-style —
    // taller window = taller panels, tall enough = the rack's two-row split.
    if (fxRackStrip != nullptr)
    {
        const auto bandDesignH = fxRackMinimized
            ? cue::FxRackStrip::minimizedHeight
            : juce::jmax (cue::fxRackMinBand,
                          juce::roundToInt ((float) getHeight() / scale) - cue::editorHeight);
        fxRackStrip->setTransform (juce::AffineTransform::scale (scale)
                                       .translated (0.0f, (float) cue::editorHeight * scale));
        fxRackStrip->setBounds (0, 0, fluidW, bandDesignH);
    }

    headerComponent->setBounds (10, 16, fluidW - 20, 64);

    // Treat the header as three non-overlapping zones: brand/orb/version,
    // centred STEMS, then theme/undo/help utilities. The GL orb must remain
    // wholly above the header divider because its backing surface is opaque.
    constexpr int orbSize = 56;
    const int orbX = 10 + headerComponent->lockupRight() + 14;
    const int headerControlY = 20;
    if (cueOrbComponent != nullptr)
        cueOrbComponent->setBounds (orbX, headerControlY, orbSize, orbSize);

    {
        const int stemW = 440;
        const int brandZoneRight = orbX + orbSize + 86; // room for version text
        const int utilityZoneLeft = 10 + headerComponent->utilityControlsLeft();
        const int availableW = juce::jmax (0, utilityZoneLeft - brandZoneRight);
        const int stemX = brandZoneRight + juce::jmax (12, (availableW - stemW) / 2);
        stemRackComponent->setBounds (stemX, headerControlY, stemW, 56);
    }

    // Full-width column: waveform, global-control footer, per-chop controls,
    // then transport. No side rail.
    const int coreW = fluidW - 20;

    waveformDisplayComponent->setBounds (10, 106, coreW, 316);
    waveformFooterComponent->setBounds (10, 428, coreW, 72);
    transportSectionComponent->setBounds (10, 506, coreW, 144);
    helpOverlayComponent->setBounds (10, 106, coreW, 544);

    if (midiKeyboardComponent != nullptr)
    {
        midiKeyboardComponent->setKeyWidth ((float) (fluidW - 36) / 49.0f); // 49 white keys, C1..B7
        midiKeyboardComponent->setBounds (18, 664, fluidW - 36, 56);
    }

    // Update banner: centred strip near the top, drawn over the content. Sized
    // in editor (unscaled) pixels since it is a direct child of the editor.
    if (updateBannerComponent != nullptr && updateBannerComponent->isVisible())
    {
        const int bannerW = juce::jmin (460, getWidth() - 40);
        const int bannerH = 34;
        updateBannerComponent->setBounds ((getWidth() - bannerW) / 2, 10, bannerW, bannerH);
        updateBannerComponent->toFront (false);
    }

    if (warpHelpOverlayComponent != nullptr && warpHelpOverlayComponent->isVisible())
    {
        warpHelpOverlayComponent->setBounds (getLocalBounds());
        warpHelpOverlayComponent->toFront (true);
    }
}

//==============================================================================
// Light/dark theme swap, mirroring CUERACK's toggleTheme/applyThemeToUi. The
// choice persists in the per-user settings file — the processor is untouched.
void AudioPluginAudioProcessorEditor::toggleTheme()
{
    cue::applyTheme (cue::isLight() ? cue::Theme::dark : cue::Theme::light);
    uiSettingsFile->setValue ("uiTheme", cue::isLight() ? "light" : "dark");
    uiSettingsFile->saveIfNeeded();
    applyThemeToUi();
}

void AudioPluginAudioProcessorEditor::applyThemeToUi()
{
    // Re-apply current theme + dynamic warp/halftime tints
    cue::applyTheme (cue::currentTheme, cue::isWarpModeActive, cue::isHalfTimeActive);

    // Foundation colours already swapped by applyTheme(); refresh everything
    // that cached one at construction time, then repaint the whole tree.
    lookAndFeel->refreshColours();
    headerComponent->refreshColours();
    transportSectionComponent->refreshColours();
    waveformFooterComponent->refreshColours();

    // The FX rack subtree runs CUERACK's own foundation palette; swap it in
    // lockstep, then re-skin its panels and toolbar.
    cue::FxRackStrip::applyRackTheme (cue::isLight(), cue::isWarpModeActive, cue::isHalfTimeActive);
    if (fxRackStrip != nullptr)
        fxRackStrip->refreshColours();

    if (cueOrbComponent != nullptr)
        cueOrbComponent->setBackgroundColour (cue::shellDark);

    sendLookAndFeelChange();
    repaint();
}
