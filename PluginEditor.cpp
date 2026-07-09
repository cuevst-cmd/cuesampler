#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <BinaryData.h>

#include <cmath>
#include <functional>
#include <map>

namespace cue
{
namespace
{
constexpr int editorWidth = 1438;
constexpr int editorHeight = 884; // 798 chassis + 86 keyboard strip
constexpr float minEditorScale = 0.75f;
constexpr float maxEditorScale = 1.5f;
constexpr float defaultWaveformVerticalScale = 0.75f;
constexpr int sideRailWidth = 64;
constexpr int headerRefreshHz = 20;
constexpr int waveformRefreshHz = 30;
constexpr int transportRefreshHz = 20;
// Unified knob size across the whole UI. Both names kept so call sites read
// in context, but they intentionally hold the same value.
constexpr int smallKnobDiameter = 66;
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

constexpr float largeCorner = 16.0f;
constexpr float mediumCorner = 14.0f;
constexpr float smallCorner = 10.0f;

const juce::Colour shellDark (0xff2d2d2d);
const juce::Colour railDark (0xff222222);
const juce::Colour panelDark (0xff222222);
const juce::Colour panelInnerDark (0xff151515);
const juce::Colour blackPanel (0xff0a0a0a);
const juce::Colour borderDark (0xff111111);
const juce::Colour borderMid (0xff333333);
const juce::Colour borderLight (0xff444444);
inline bool isHalfTimeActive = false;
inline bool isWarpModeActive = false;
inline juce::Colour getOrange()
{
    if (isWarpModeActive)
        return juce::Colour (0xffa855f7);

    return isHalfTimeActive ? juce::Colour (0xff0088ff) : juce::Colour (0xffff6900);
}
#define accentOrange cue::getOrange()
const juce::Colour textPrimary (0xffffffff);
const juce::Colour textMuted (0xff777777);
const juce::Colour textFaint (0xff6a7282);
const juce::Colour metalGrey (0xff555555);

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

// Brand text font (Syne). heavyFont() is the bold weight used for nearly all UI
// text — labels, buttons, headers, the stacked title. brandFont() is the plain
// weight for lighter/secondary text. Numeric readouts deliberately stay
// monospaced (monoFont / cousineFont) so live-updating digits keep their width.
juce::Font heavyFont (float height)
{
    return { juce::FontOptions (brandFontName(), height, juce::Font::bold) };
}

juce::Font brandFont (float height)
{
    return { juce::FontOptions (brandFontName(), height, juce::Font::plain) };
}

juce::Font monoFont (float height)
{
    return { juce::FontOptions ("Menlo", height, juce::Font::bold) };
}

juce::Font cousineFont (float height)
{
    return { juce::FontOptions ("Cousine", height, juce::Font::bold) };
}

void fillRoundedGradient (juce::Graphics& g, juce::Rectangle<float> area,
                          juce::Colour top, juce::Colour bottom, float cornerSize)
{
    juce::ColourGradient gradient (top, area.getCentreX(), area.getY(),
                                   bottom, area.getCentreX(), area.getBottom(), false);
    gradient.addColour (0.48, top.interpolatedWith (bottom, 0.38f));
    gradient.addColour (0.82, top.interpolatedWith (bottom, 0.82f));
    g.setGradientFill (gradient);
    g.fillRoundedRectangle (area, cornerSize);
}

void fillEllipseGradient (juce::Graphics& g, juce::Rectangle<float> area,
                          juce::Colour top, juce::Colour bottom)
{
    juce::ColourGradient gradient (top, area.getCentreX(), area.getY(),
                                   bottom, area.getCentreX(), area.getBottom(), false);
    gradient.addColour (0.35, top.interpolatedWith (bottom, 0.24f));
    gradient.addColour (0.78, top.interpolatedWith (bottom, 0.78f));
    g.setGradientFill (gradient);
    g.fillEllipse (area);
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

void fillPathGradient (juce::Graphics& g, const juce::Path& path, juce::Rectangle<float> area,
                       juce::Colour top, juce::Colour bottom)
{
    juce::ColourGradient gradient (top, area.getCentreX(), area.getY(),
                                   bottom, area.getCentreX(), area.getBottom(), false);
    gradient.addColour (0.5, top.interpolatedWith (bottom, 0.4f));
    g.setGradientFill (gradient);
    g.fillPath (path);
}

// --- Smoked glass panels -----------------------------------------------------
// Panels are dark-grey "liquid glass" sheets over the orange gradient
// faceplate. True backdrop blur is too expensive per-frame, so each panel
// draws the slice of a pre-blurred copy of the background that sits behind it,
// then smokes it with a translucent grey tint and a subtle rim light.

// Text colours for labels painted directly on glass surfaces.
const juce::Colour glassText (0xfff2f3f5);
const juce::Colour glassTextMuted (0xffb9bdc6);

// Control glass (buttons): deeper and more opaque than the panels so
// controls still read as a layer above them.
const juce::Colour controlGlassTop (0xd824262a);
const juce::Colour controlGlassBottom (0xe2131417);

const juce::Image& getGlassBlurImage()
{
    static const juce::Image image = juce::ImageCache::getFromMemory (
        CueSamplerBinaryData::cue_background_blur_png,
        CueSamplerBinaryData::cue_background_blur_pngSize);
    return image;
}

// --- Mode-tinted backgrounds -------------------------------------------------
// HALF TIME shifts the accent to blue and WARP to purple (see getOrange()).
// The orange faceplate follows by hue-rotating the background images toward
// the active accent. Variants are built lazily on first use and cached for
// the process lifetime; the main image is rebuilt at design resolution so the
// rotation stays fast and the cache small.

int accentModeIndex()
{
    return isWarpModeActive ? 2 : (isHalfTimeActive ? 1 : 0);
}

float hueRotationForMode (int mode)
{
    const auto orange = juce::Colour (0xffff6900);
    const auto target = mode == 2 ? juce::Colour (0xffa855f7)
                      : mode == 1 ? juce::Colour (0xff0088ff)
                                  : orange;
    return target.getHue() - orange.getHue();
}

juce::Image createHueRotatedCopy (const juce::Image& source, float hueDelta,
                                  int targetWidth, int targetHeight)
{
    // rescaled() returns the source itself when the size matches, so copy
    // explicitly: BitmapData below must never mutate the shared original.
    auto image = (source.getWidth() == targetWidth && source.getHeight() == targetHeight)
                     ? source.createCopy()
                     : source.rescaled (targetWidth, targetHeight, juce::Graphics::highResamplingQuality);

    juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);

    for (int py = 0; py < data.height; ++py)
        for (int px = 0; px < data.width; ++px)
            data.setPixelColour (px, py, data.getPixelColour (px, py).withRotatedHue (hueDelta));

    return image;
}

// Background for the current accent mode; 'base' is the full-res orange image.
const juce::Image& getModeTintedBackground (const juce::Image& base)
{
    const int mode = accentModeIndex();

    if (mode == 0 || ! base.isValid())
        return base;

    static juce::Image cache[3];
    auto& slot = cache[mode];

    if (! slot.isValid())
        slot = createHueRotatedCopy (base, hueRotationForMode (mode), editorWidth, editorHeight);

    return slot;
}

const juce::Image& getGlassBlurImageForMode()
{
    const auto& base = getGlassBlurImage();
    const int mode = accentModeIndex();

    if (mode == 0 || ! base.isValid())
        return base;

    static juce::Image cache[3];
    auto& slot = cache[mode];

    if (! slot.isValid())
        slot = createHueRotatedCopy (base, hueRotationForMode (mode), base.getWidth(), base.getHeight());

    return slot;
}

// Maps a component-local area into the editor's design space (the 1438x798
// coordinate system the background image covers). Walks up the parent chain,
// so it works for nested children of contentComponent too.
juce::Rectangle<float> designSpaceArea (const juce::Component& component, juce::Rectangle<float> localArea)
{
    auto area = localArea;

    for (const auto* c = &component; c->getParentComponent() != nullptr; c = c->getParentComponent())
        area += c->getPosition().toFloat();

    return area;
}

// Draws the blurred-background slice behind a panel. Assumes the caller has
// already clipped to the panel shape. localArea/designArea describe the same
// rectangle in the two coordinate spaces.
void drawGlassBackdrop (juce::Graphics& g, juce::Rectangle<float> localArea, juce::Rectangle<float> designArea)
{
    const auto& image = getGlassBlurImageForMode();

    if (! image.isValid())
        return;

    const auto transform = juce::AffineTransform::scale ((float) editorWidth / (float) image.getWidth(),
                                                         (float) editorHeight / (float) image.getHeight())
                               .translated (localArea.getX() - designArea.getX(),
                                            localArea.getY() - designArea.getY());
    g.drawImageTransformed (image, transform);
}

// Smoke tint over an already-clipped, already-backdropped glass shape.
void finishGlassSurface (juce::Graphics& g, juce::Rectangle<float> area)
{
    juce::ColourGradient smoke (juce::Colour (0xff3c3f45).withAlpha (0.55f), area.getCentreX(), area.getY(),
                                juce::Colour (0xff202226).withAlpha (0.68f), area.getCentreX(), area.getBottom(), false);
    smoke.addColour (0.18, juce::Colour (0xff35383d).withAlpha (0.58f));
    g.setGradientFill (smoke);
    g.fillRect (area);
}

void fillGlassRounded (juce::Graphics& g, const juce::Component& component,
                       juce::Rectangle<float> area, float cornerSize)
{
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (area, cornerSize);
        g.reduceClipRegion (clip);

        drawGlassBackdrop (g, area, designSpaceArea (component, area));
        finishGlassSurface (g, area);
    }

    g.setColour (juce::Colours::white.withAlpha (0.26f));
    g.drawRoundedRectangle (area.reduced (0.5f), cornerSize, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.30f));
    g.drawRoundedRectangle (area.expanded (0.5f), cornerSize + 0.5f, 1.0f);
}

// Path variant for non-rectangular glass (side rails). Assumes the graphics
// context is already in design space, i.e. localArea == designArea.
void fillGlassPath (juce::Graphics& g, const juce::Path& path, juce::Rectangle<float> designArea)
{
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (path);

        drawGlassBackdrop (g, designArea, designArea);
        finishGlassSurface (g, designArea);
    }

    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

float getEffectiveZoomLevel (float zoomControlValue) noexcept
{
    const auto clampedZoom = juce::jlimit (0.0f, 1.0f, zoomControlValue);

    if (clampedZoom <= zoomResponseMidpoint)
        return juce::jmap (clampedZoom, 0.0f, zoomResponseMidpoint, 0.0f, zoomMappedMidpoint);

    return juce::jmap (clampedZoom, zoomResponseMidpoint, 1.0f, zoomMappedMidpoint, 1.0f);
}

juce::Colour getMeterColourForLevel (float meterLevel) noexcept
{
    const auto clampedLevel = juce::jlimit (0.0f, 1.0f, meterLevel);
    const auto safeColour = juce::Colour (0xff00c950);
    const auto cautionColour = juce::Colour (0xffffc83d);
    const auto hotColour = juce::Colour (0xffff4d4f);

    if (clampedLevel < 0.72f)
        return safeColour.interpolatedWith (cautionColour, clampedLevel / 0.72f * 0.35f);

    if (clampedLevel < 0.9f)
        return safeColour.interpolatedWith (cautionColour, juce::jmap (clampedLevel, 0.72f, 0.9f, 0.35f, 1.0f));

    return cautionColour.interpolatedWith (hotColour, juce::jmap (clampedLevel, 0.9f, 1.0f, 0.0f, 1.0f));
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
    label.setText (text, juce::dontSendNotification);
    label.setFont (heavyFont (height));
    label.setJustificationType (justification);
    label.setColour (juce::Label::textColourId, colour);
    label.setInterceptsMouseClicks (false, false);
}

void configureButton (juce::TextButton& button, const juce::String& text, juce::Colour textColour)
{
    button.setButtonText (text);
    button.setClickingTogglesState (false);
    button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xe017191c));
    button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xe62b2d31));
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
    g.setColour (colour);
    g.setFont (heavyFont (fontHeight));
    g.drawFittedText (text, bounds, justification, 2);
}

juce::TextLayout createTooltipLayout (const juce::String& text, juce::Colour colour)
{
    juce::AttributedString attributed;
    attributed.setWordWrap (juce::AttributedString::byWord);
    attributed.setJustification (juce::Justification::centredLeft);
    attributed.append (text, heavyFont (15.0f), colour);

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

// Recessed details in glass surfaces: translucent shading instead of the old
// solid-black hardware look, so the orange backdrop keeps glowing through.
void drawSlot (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour (juce::Colours::black.withAlpha (0.24f));
    g.fillRoundedRectangle (bounds, bounds.getWidth() * 0.5f);
    g.setColour (juce::Colours::black.withAlpha (0.40f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), bounds.getWidth() * 0.5f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawRoundedRectangle (bounds.translated (0.0f, 1.0f), bounds.getWidth() * 0.5f, 1.0f);
}

void drawPanelHole (juce::Graphics& g, juce::Point<float> centre, float diameter)
{
    auto bounds = juce::Rectangle<float> (diameter, diameter).withCentre (centre);
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.fillEllipse (bounds.translated (0.0f, 1.0f));
    g.setColour (juce::Colours::black.withAlpha (0.32f));
    g.fillEllipse (bounds);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawEllipse (bounds, 1.0f);
}

void drawSoftDropShadow (juce::Graphics& g, const juce::Rectangle<float>& bounds,
                         float cornerSize, bool isEllipse,
                         float alphaMultiplier, float offsetY, float maxSpread,
                         juce::Colour shadowColour = juce::Colours::black)
{
    juce::DropShadow shadow;
    shadow.colour = shadowColour.withAlpha (juce::jlimit (0.0f, 1.0f, shadowColour.getFloatAlpha() * alphaMultiplier * 0.4f));
    shadow.radius = juce::roundToInt (maxSpread);
    shadow.offset = { 0, juce::roundToInt (offsetY) };

    juce::Path p;
    if (isEllipse)
        p.addEllipse (bounds);
    else
        p.addRoundedRectangle (bounds, cornerSize);

    shadow.drawForPath (g, p);
}

juce::Path createRailPath (juce::Rectangle<float> area, bool isLeftRail, float radius)
{
    juce::Path path;

    if (isLeftRail)
    {
        path.startNewSubPath (area.getRight(), area.getY());
        path.lineTo (area.getX() + radius, area.getY());
        path.quadraticTo (area.getX(), area.getY(), area.getX(), area.getY() + radius);
        path.lineTo (area.getX(), area.getBottom() - radius);
        path.quadraticTo (area.getX(), area.getBottom(), area.getX() + radius, area.getBottom());
        path.lineTo (area.getRight(), area.getBottom());
    }
    else
    {
        path.startNewSubPath (area.getX(), area.getY());
        path.lineTo (area.getRight() - radius, area.getY());
        path.quadraticTo (area.getRight(), area.getY(), area.getRight(), area.getY() + radius);
        path.lineTo (area.getRight(), area.getBottom() - radius);
        path.quadraticTo (area.getRight(), area.getBottom(), area.getRight() - radius, area.getBottom());
        path.lineTo (area.getX(), area.getBottom());
    }

    path.closeSubPath();
    return path;
}

// --- Spun-metal knobs ----------------------------------------------------------
// Reference-style rotary: a segmented LED value ring around a dark gunmetal
// body with a spun/brushed face and a recessed indicator dot. JUCE has no
// conic gradients, so the face sheen is rendered per-pixel once per size and
// cached (at 2x, so it stays crisp at maxEditorScale).
const juce::Image& getSpunMetalFace (int diameterPx)
{
    static std::map<int, juce::Image> cache;

    if (auto it = cache.find (diameterPx); it != cache.end())
        return it->second;

    const int size = juce::jmax (8, diameterPx * 2);
    juce::Image image (juce::Image::ARGB, size, size, true);
    juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

    const float radius = (float) size * 0.5f;

    for (int py = 0; py < size; ++py)
    {
        for (int px = 0; px < size; ++px)
        {
            const float dx = (float) px - radius + 0.5f;
            const float dy = (float) py - radius + 0.5f;
            const float r = std::sqrt (dx * dx + dy * dy) / radius;

            if (r > 1.0f)
            {
                data.setPixelColour (px, py, juce::Colours::transparentBlack);
                continue;
            }

            const float theta = std::atan2 (dy, dx);

            // Anisotropic sheen: two highlight lobes plus a soft top light,
            // darkening toward the rim like spun aluminium.
            float b = 0.27f
                    + 0.17f * std::pow (std::abs (std::cos (theta - 0.95f)), 3.0f)
                    + 0.11f * std::pow (std::abs (std::cos (theta + 1.85f)), 7.0f)
                    + 0.05f * (-dy / radius)
                    + 0.015f * std::sin (r * 60.0f); // faint radial grooves

            b *= 1.0f - 0.38f * juce::jlimit (0.0f, 1.0f, (r - 0.80f) / 0.20f);
            b = juce::jlimit (0.0f, 1.0f, b);

            const float alpha = juce::jlimit (0.0f, 1.0f, (1.0f - r) * radius); // ~1px edge AA
            data.setPixelColour (px, py, juce::Colour::fromFloatRGBA (b, b * 1.02f, b * 1.05f, alpha));
        }
    }

    return cache[diameterPx] = std::move (image);
}

// Angles follow the addCentredArc / getPointOnCircumference convention:
// radians clockwise from 12 o'clock.
void drawMetalKnob (juce::Graphics& g, juce::Rectangle<float> bounds,
                    float pos01, float startAngle, float endAngle,
                    juce::Colour accent, float hover)
{
    const auto centre = bounds.getCentre();
    const float outerR = bounds.getWidth() * 0.5f;
    const float valueAngle = juce::jmap (juce::jlimit (0.0f, 1.0f, pos01), startAngle, endAngle);

    // Segmented value ring: lit up to the current value, unlit beyond.
    const int numSegments = juce::jmax (16, juce::roundToInt (bounds.getWidth() * 0.36f));
    const float tickThickness = juce::jmax (3.0f, bounds.getWidth() * 0.07f);
    const float ringR = outerR - tickThickness * 0.5f - 0.5f;
    const float segSpan = (endAngle - startAngle) / (float) numSegments;
    constexpr float gapFrac = 0.34f;

    for (int i = 0; i < numSegments; ++i)
    {
        const float a0 = startAngle + segSpan * ((float) i + gapFrac * 0.5f);
        const float a1 = a0 + segSpan * (1.0f - gapFrac);

        // Fractional lighting: the segment at the value boundary fades in
        // proportionally instead of snapping, so fine-sensitivity drags
        // (SCROLL) don't flicker the leading tick on and off.
        const float lit = juce::jlimit (0.0f, 1.0f, (valueAngle - a0) / (a1 - a0));

        juce::Path seg;
        seg.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, a0, a1, true);

        if (lit > 0.0f)
        {
            // soft bloom behind the lit tick
            g.setColour (accent.withAlpha ((0.22f + 0.16f * hover) * lit));
            g.strokePath (seg, juce::PathStrokeType (tickThickness + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        }

        g.setColour (juce::Colours::black.withAlpha (0.50f)
                         .interpolatedWith (accent.brighter (0.18f), lit));
        g.strokePath (seg, juce::PathStrokeType (tickThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Body: drop shadow, dark bezel, spun-metal face.
    const float bodyR = outerR - tickThickness - 3.5f;
    auto bodyBounds = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);

    if (hover > 0.01f)
        drawSoftDropShadow (g, bodyBounds, 0.0f, true, 1.2f + 1.3f * hover, 2.0f * (1.0f - hover), 3.0f + 6.0f * hover, accent.withAlpha (hover));
    else
        drawSoftDropShadow (g, bodyBounds, 0.0f, true, 1.2f, 2.0f, 3.0f);

    fillEllipseGradient (g, bodyBounds, juce::Colour (0xff232529), juce::Colour (0xff0c0d0f));

    auto faceBounds = bodyBounds.reduced (bodyR * 0.16f);
    g.drawImage (getSpunMetalFace (juce::roundToInt (faceBounds.getWidth())), faceBounds,
                 juce::RectanglePlacement (juce::RectanglePlacement::stretchToFit));

    g.setColour (juce::Colours::white.withAlpha (0.10f + 0.10f * hover));
    g.drawEllipse (faceBounds.reduced (0.5f), 1.0f);

    // Recessed indicator dot, rotating with the value.
    const float dotR = juce::jmax (1.8f, faceBounds.getWidth() * 0.05f);
    const auto dotCentre = centre.getPointOnCircumference (faceBounds.getWidth() * 0.34f, valueAngle);
    auto dotBounds = juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (dotCentre);

    g.setColour (juce::Colours::white.withAlpha (0.16f));
    g.fillEllipse (dotBounds.translated (0.0f, 1.0f));
    g.setColour (juce::Colour (0xff101113));
    g.fillEllipse (dotBounds);
    g.setColour (accent.withAlpha (0.30f + 0.45f * hover));
    g.drawEllipse (dotBounds, 1.0f);
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
// Mechanical-keycap chassis shared by all push buttons: a dark skirted cap
// floating on a soft contact shadow, with a lighter top face inset like a
// keycap's top surface — the bottom wall reads thicker, like a key seen
// slightly from the front. Pressing sinks the whole cap keycapTravel px and
// collapses the shadow so the key visibly bottoms out; toggles warm the cap
// toward the accent as they switch on (accentAmount 0..1).
constexpr float keycapTravel = 3.0f;

// Cap at rest is the component bounds minus a bottom sliver reserved for the
// contact shadow (painting is clipped to the component); pressing translates
// it down by keycapTravel.
juce::Rectangle<float> getKeycapBounds (juce::Rectangle<float> bounds, bool isDown)
{
    return bounds.withTrimmedBottom (keycapTravel + 1.0f)
                 .translated (0.0f, isDown ? keycapTravel : 0.0f);
}

// The top-face plateau: inset and shifted up inside the cap so the front
// wall reads thicker, like a key seen slightly from the front. Labels and
// icons are laid out against this rect so they sit on (and ride) the face.
juce::Rectangle<float> getKeycapFaceBounds (juce::Rectangle<float> bounds, bool isDown)
{
    const auto cap = getKeycapBounds (bounds, isDown);
    const float inset = juce::jlimit (2.0f, 5.0f, cap.getHeight() * 0.11f);
    return cap.reduced (inset).translated (0.0f, -inset * 0.30f);
}

void drawKeycap (juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize,
                 float hover, bool isDown, float accentAmount = 0.0f,
                 juce::Colour base = juce::Colour (0xff34373d))
{
    const auto cap = getKeycapBounds (bounds, isDown);
    const auto face = getKeycapFaceBounds (bounds, isDown);
    const float faceCorner = juce::jmax (2.0f, cornerSize * 0.72f);
    const auto accent = accentOrange;

    if (accentAmount > 0.001f)
        base = base.interpolatedWith (accent.withAlpha (0.85f), 0.16f * accentAmount);

    // Contact shadow: lifts the cap at rest, collapses as it bottoms out.
    if (isDown)
        drawSoftDropShadow (g, cap, cornerSize, false, 0.9f, 1.0f, 2.0f);
    else
        drawSoftDropShadow (g, cap, cornerSize, false, 1.7f, 4.0f, 6.0f);

    if (hover > 0.01f && ! isDown)
        drawSoftDropShadow (g, cap, cornerSize, false, 1.5f * hover, 2.0f, 9.0f, accent.withAlpha (hover));

    // Skirt: one continuous moulded surface, darkest along the bottom wall.
    fillRoundedGradient (g, cap,
                         base.brighter (0.02f + 0.05f * hover),
                         base.darker (0.58f), cornerSize);

    // Light catching the top rim of the cap.
    juce::ColourGradient rim (juce::Colours::white.withAlpha (0.14f + 0.05f * hover),
                              cap.getCentreX(), cap.getY(),
                              juce::Colours::white.withAlpha (0.0f),
                              cap.getCentreX(), cap.getY() + 5.0f, false);
    g.setGradientFill (rim);
    g.fillRoundedRectangle (cap, cornerSize);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawRoundedRectangle (cap, cornerSize, 1.0f);

    // Soft recess where the face plateau meets the skirt — a blurred halo
    // rather than a stroked ring, so the transition reads moulded.
    drawSoftDropShadow (g, face, faceCorner, false, 1.0f, 1.0f, 2.5f);

    auto faceTop = base.brighter (isDown ? 0.10f : 0.22f).brighter (0.06f * hover);
    auto faceBottom = base.brighter (isDown ? 0.0f : 0.04f);
    if (accentAmount > 0.001f)
    {
        faceTop = faceTop.interpolatedWith (accent, 0.10f * accentAmount);
        faceBottom = faceBottom.interpolatedWith (accent, 0.07f * accentAmount);
    }
    fillRoundedGradient (g, face, faceTop, faceBottom, faceCorner);

    // Soft sheen across the top half of the face, compressed while held down.
    const float sheenAlpha = (isDown ? 0.05f : 0.11f) + 0.06f * hover;
    juce::ColourGradient sheen (juce::Colours::white.withAlpha (sheenAlpha),
                                face.getCentreX(), face.getY(),
                                juce::Colours::white.withAlpha (0.0f),
                                face.getCentreX(), face.getY() + face.getHeight() * 0.60f, false);
    g.setGradientFill (sheen);
    g.fillRoundedRectangle (face, faceCorner);
}

// Shared chassis for the glass toggle switches (HALF TIME, SYNC TO DAW): a
// tactile keycap that warms toward the accent as the switch animates on,
// with a status LED riding the cap. One code path keeps hover/press/on
// states consistent between the two switches.
void drawGlassToggle (juce::Graphics& g, juce::Rectangle<float> bounds,
                      float position, float hover, bool isDown,
                      float ledRadius, float ledOffsetY)
{
    const float pos = juce::jlimit (0.0f, 1.0f, position);

    drawKeycap (g, bounds, 10.0f, hover, isDown, pos);

    // Status LED — positioned from the (possibly pressed) cap so it rides the key.
    const auto cap = getKeycapBounds (bounds, isDown);
    auto ledCentre = juce::Point<float> (cap.getCentreX(), cap.getY() + ledOffsetY);
    auto ledBounds = juce::Rectangle<float> (ledRadius * 2.0f, ledRadius * 2.0f).withCentre (ledCentre);

    if (pos > 0.01f)
    {
        g.setColour (accentOrange.withAlpha (0.45f * pos));
        g.fillEllipse (ledBounds.expanded (3.5f * pos));
    }

    auto ledColour = juce::Colour (0xff1a1a1a).interpolatedWith (accentOrange.brighter (0.2f), pos);
    fillRoundedGradient (g, ledBounds, ledColour.brighter (0.15f), ledColour.darker (0.15f), ledRadius);

    if (pos > 0.1f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f * pos));
        g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre ({ ledCentre.x - 1.0f, ledCentre.y - 1.0f }));
    }

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (ledBounds, 1.0f);
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

        fillRoundedGradient (g, bounds, blackPanel.brighter (0.1f).withAlpha (0.98f),
                             blackPanel.darker (0.25f).withAlpha (0.98f), 7.0f);
        g.setColour (accentOrange.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds, 7.0f, 1.5f);

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

        // Recessed glass groove
        auto track = isScrollbarVertical ? bounds.reduced (bounds.getWidth() * 0.30f, 0.0f)
                                         : bounds.reduced (0.0f, bounds.getHeight() * 0.30f);
        const float trackRadius = juce::jmin (track.getWidth(), track.getHeight()) * 0.5f;
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (track, trackRadius);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (track, trackRadius, 1.0f);

        if (thumbSize <= 0)
            return;

        auto thumb = isScrollbarVertical
            ? bounds.withY ((float) thumbStartPosition).withHeight ((float) thumbSize).reduced (1.0f, 0.0f)
            : bounds.withX ((float) thumbStartPosition).withWidth ((float) thumbSize).reduced (0.0f, 1.0f);
        const float thumbRadius = juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f;

        const float hover = isMouseDown ? 1.0f : (isMouseOver ? 0.6f : 0.0f);
        auto base = juce::Colour (0xff3c3f45).interpolatedWith (accentOrange, 0.30f + 0.35f * hover);
        fillRoundedGradient (g, thumb, base.brighter (0.10f).withAlpha (0.92f),
                             base.darker (0.12f).withAlpha (0.95f), thumbRadius);
        g.setColour (juce::Colours::white.withAlpha (0.10f + 0.08f * hover));
        g.drawRoundedRectangle (thumb, thumbRadius, 1.0f);
    }

    // --- Popup / drop menus --------------------------------------------------
    // Drop menus (CHOP @ TRANS. sensitivity, warp-marker snap, key override...)
    // are drawn to match the smoked-glass aesthetic: a dark rounded panel with
    // an accent rim, accent-tinted hover rows, and accent ticks / chevrons. The
    // mode-aware accent (orange / blue / purple) follows the rest of the UI.

    int getPopupMenuBorderSize() override { return 7; }

    juce::Font getPopupMenuFont() override
    {
        return { juce::FontOptions (brandFontName(), 14.5f, juce::Font::plain) };
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
        const float corner = 9.0f;

        fillRoundedGradient (g, bounds, blackPanel.brighter (0.12f).withAlpha (0.985f),
                             blackPanel.darker (0.25f).withAlpha (0.985f), corner);

        // Soft top sheen so the panel reads as glass.
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (bounds.withHeight (bounds.getHeight() * 0.5f), corner);

        g.setColour (accentOrange.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds, corner, 1.4f);
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

        g.setColour (accentOrange.withAlpha (0.92f));
        g.setFont (heavyFont (11.5f));
        g.drawText (sectionName.toUpperCase(), r.withTrimmedBottom (3.0f).toNearestInt(),
                    juce::Justification::bottomLeft, false);

        // Hairline under the header.
        g.setColour (juce::Colours::white.withAlpha (0.10f));
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
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (line.getX(), line.getCentreY(), line.getWidth(), 1.0f));
            return;
        }

        auto r = area.reduced (4, 1).toFloat();
        const float corner = 5.0f;

        // Hover / highlight row.
        if (isHighlighted && isActive)
        {
            fillRoundedGradient (g, r, accentOrange.withAlpha (0.34f),
                                 accentOrange.withAlpha (0.20f), corner);
            g.setColour (accentOrange.withAlpha (0.55f));
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
            || style == "flatAction" || style == "utilitySync" || style == "effectSwitch")
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
                                 isHalfTime ? 11.0f : 14.0f); // LED y-offset
                return;
            }


            if (style == "effectSwitch")
            {
                auto trackBounds = bounds.reduced (1.0f);

                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.fillRoundedRectangle (trackBounds.translated (0.0f, 1.0f), 4.0f);

                fillRoundedGradient (g, trackBounds, blackPanel.brighter (0.08f).withAlpha (0.85f),
                                     blackPanel.darker (0.25f).withAlpha (0.88f), 4.0f);
                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.drawRoundedRectangle (trackBounds, 4.0f, 1.0f);

                juce::ColourGradient innerShadow (juce::Colour (0xd80a0a0a), trackBounds.getCentreX(), trackBounds.getY(),
                                                  juce::Colour (0xd81a1a1a), trackBounds.getCentreX(), trackBounds.getBottom(), false);
                g.setGradientFill (innerShadow);
                g.fillRoundedRectangle (trackBounds.reduced (1.0f), 3.0f);

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
                    g.fillRoundedRectangle (trackBounds.reduced (1.0f), 3.0f);
                }

                auto thumbWidth = trackBounds.getWidth() * 0.5f;
                auto thumbBounds = trackBounds.withWidth (thumbWidth).reduced (1.0f);
                
                // Allow a tiny bounce beyond track bounds (mechanical impact feel)
                float drawPosForThumb = juce::jlimit (-0.03f, 1.03f, position);
                float startX = trackBounds.getX() + 1.0f;
                float endX = trackBounds.getRight() - thumbWidth + 1.0f;
                float currentX = startX + (endX - startX) * drawPosForThumb;
                thumbBounds.setX (currentX);

                auto offThumbColour = juce::Colour (0xff444444);
                auto onThumbColour = accentOrange.darker (0.1f);
                auto thumbColour = offThumbColour.interpolatedWith (onThumbColour, clampedPos);
                thumbColour = thumbColour.brighter (0.1f * hover);

                // Raised-cap treatment so the thumb matches the keycap language.
                drawSoftDropShadow (g, thumbBounds, 3.0f, false, 1.1f, 1.5f, 3.0f);

                juce::ColourGradient thumbGrad (thumbColour.brighter (0.1f), thumbBounds.getCentreX(), thumbBounds.getY(),
                                                thumbColour.darker (0.2f), thumbBounds.getCentreX(), thumbBounds.getBottom(), false);
                g.setGradientFill (thumbGrad);
                g.fillRoundedRectangle (thumbBounds, 3.0f);

                juce::ColourGradient thumbSheen (juce::Colours::white.withAlpha (0.12f),
                                                 thumbBounds.getCentreX(), thumbBounds.getY(),
                                                 juce::Colours::white.withAlpha (0.0f),
                                                 thumbBounds.getCentreX(), thumbBounds.getY() + thumbBounds.getHeight() * 0.55f, false);
                g.setGradientFill (thumbSheen);
                g.fillRoundedRectangle (thumbBounds.reduced (1.0f), 2.5f);

                auto offBorderColour = borderLight.brighter (0.2f);
                auto onBorderColour = accentOrange.brighter (0.2f);
                g.setColour (offBorderColour.interpolatedWith (onBorderColour, clampedPos));
                g.drawRoundedRectangle (thumbBounds, 3.0f, 1.0f);

                g.setColour (juce::Colours::black.withAlpha (0.6f));
                float centreX = thumbBounds.getCentreX();
                float gripY = thumbBounds.getY() + 3.0f;
                float gripH = thumbBounds.getHeight() - 6.0f;
                g.fillRect (juce::Rectangle<float> (centreX - 2.0f, gripY, 1.0f, gripH));
                g.fillRect (juce::Rectangle<float> (centreX, gripY, 1.0f, gripH));
                g.fillRect (juce::Rectangle<float> (centreX + 2.0f, gripY, 1.0f, gripH));

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

            g.setColour (juce::Colours::white);
            g.fillPath (icon);
            return;
        }

        if (style == "halfTime")
        {
            float position = button.getToggleState() ? 1.0f : 0.0f;
            if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                position = animatedButton->getCurrentAnimationPosition();
            float clampedPos = juce::jlimit (0.0f, 1.0f, position);

            auto textOff = juce::Colour (0xff777777);
            auto textOn = textPrimary;
            g.setColour (textOff.interpolatedWith (textOn, clampedPos));
            g.setFont (heavyFont (8.0f));
            // Below the status LED (radius 3.5 at capY + 11), inside the face.
            g.drawFittedText (button.getButtonText(),
                              bounds.withTop ((int) (cap.getY() + 16.0f)).withTrimmedBottom (1),
                              juce::Justification::centred, 2);
            return;
        }

        if (style == "flatAction")
        {
            float hover = getHoverAlpha (button, false);
            auto textOff = juce::Colour (0xff99a1af);
            auto textOn = textPrimary;
            g.setColour (textOff.interpolatedWith (textOn, hover));
            g.setFont (heavyFont (juce::jlimit (9.0f, 13.0f, face.getHeight() * 0.60f)));
            g.drawFittedText (button.getButtonText(), bounds.reduced (3, 0), juce::Justification::centred, 1);
            return;
        }

        if (style == "helpButton")
        {
            float hover = getHoverAlpha (button, false);
            auto baseColour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                         : juce::TextButton::textColourOffId);
            g.setColour (baseColour.interpolatedWith (accentOrange, hover));
            g.setFont (heavyFont (16.0f));
            g.drawFittedText (button.getButtonText(), bounds, juce::Justification::centred, 1);
            return;
        }

        if (style == "utilitySync")
        {
            float position = button.getToggleState() ? 1.0f : 0.0f;
            if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&button))
                position = animatedButton->getCurrentAnimationPosition();
            float clampedPos = juce::jlimit (0.0f, 1.0f, position);

            auto textOff = juce::Colour (0xff777777);
            auto textOn = textPrimary;
            g.setColour (textOff.interpolatedWith (textOn, clampedPos));
            g.setFont (heavyFont (9.5f));
            // Below the status LED (radius 4 at capY + 14), inside the face.
            g.drawFittedText (button.getButtonText(),
                              bounds.withTop ((int) (cap.getY() + 20.0f)).withTrimmedBottom (1),
                              juce::Justification::centred, 1);
            return;
        }

        if (style == "waveScaleStep")
        {
            float hover = getHoverAlpha (button, false);
            g.setColour (textPrimary.withAlpha (0.9f).interpolatedWith (accentOrange, hover));
            g.setFont (heavyFont (17.0f));
            g.drawFittedText (button.getButtonText(), bounds, juce::Justification::centred, 1);
            return;
        }

        auto fontSize = juce::jlimit (7.0f, 15.0f, face.getHeight() * 0.40f);
        float hover = getHoverAlpha (button, false);
        auto baseColor = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                     : juce::TextButton::textColourOffId);
        g.setColour (baseColor.interpolatedWith (accentOrange, hover));
        g.setFont (heavyFont (fontSize));
        g.drawFittedText (button.getButtonText(), bounds.reduced (2, 0), juce::Justification::centred, 2);
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

        // All knob styles share the spun-metal look; per-knob accents (CUE /
        // GAIN / PITCH minis) come through the cueAccent property.
        drawMetalKnob (g, bounds, sliderPosProportional, rotaryStartAngle, rotaryEndAngle,
                       getCueAccent (slider, accentOrange), hover);
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
        setColour (juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour (0xffdfe1e4));
        setColour (juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour (0xff202226));
        setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colours::black.withAlpha (0.35f));
        setColour (juce::MidiKeyboardComponent::shadowColourId, juce::Colours::black.withAlpha (0.35f));
        setColour (juce::MidiKeyboardComponent::textLabelColourId, juce::Colour (0xff42454c));
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

class StartKnobComponent final : public juce::Component
{
public:
    StartKnobComponent()
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (-1000.0, 1000.0, 1.0);
        slider.setValue (0.0, juce::dontSendNotification);
        slider.getProperties().set ("cueStyle", "startTiltKnob");
        slider.setPopupDisplayEnabled (true, false, nullptr);
        slider.setTooltip ("GRID: shifts the beat-grid anchor in time (-1000 to +1000 ms). Use this when chop boundaries feel off-beat. Alt-click to reset to 0.");
        slider.setTextValueSuffix (" ms");
        slider.captureCurrentValueAsDefault();
        addAndMakeVisible (slider);

        configureTextLabel (label, "GRID", 16.0f, glassTextMuted, juce::Justification::centred);
        addAndMakeVisible (label);
    }

    void resized() override
    {
        label.setBounds (0, 0, getWidth(), 18);
        slider.setBounds ((getWidth() - smallKnobDiameter) / 2, 19, smallKnobDiameter, smallKnobDiameter);
    }

    juce::Slider& getSlider() noexcept { return slider; }

private:
    OptResetSlider slider;
    juce::Label label;
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
            fillRoundedGradient (g, bounds, blackPanel.brighter (0.08f), blackPanel.darker (0.3f), smallCorner);
            g.setColour (juce::Colours::black);
            g.drawRoundedRectangle (bounds.reduced (0.5f), smallCorner, 1.0f);

            juce::ColourGradient darkFade (juce::Colours::black.withAlpha (0.4f), bounds.getCentreX(), bounds.getY(),
                                           juce::Colours::transparentBlack, bounds.getCentreX(), bounds.getBottom(), false);
            g.setGradientFill (darkFade);
            g.fillRoundedRectangle (bounds.withHeight (46.0f), smallCorner);

            juce::ColourGradient topGlow (juce::Colours::white.withAlpha (0.04f), bounds.getCentreX(), bounds.getY(),
                                          juce::Colours::transparentWhite, bounds.getCentreX(), bounds.getY() + 20.688f, false);
            g.setGradientFill (topGlow);
            g.fillRoundedRectangle (bounds.withHeight (20.688f), smallCorner);

            if (style == "timeBox")
            {
                g.setColour (accentOrange);
                g.setFont (cousineFont (18.0f).withExtraKerningFactor (0.05f));
                g.drawText (value, getLocalBounds(), juce::Justification::centred, false);
            }
            else if (isScanning)
            {
                // Caption
                g.setColour (accentOrange.withAlpha (0.8f));
                g.setFont (heavyFont (10.0f).withExtraKerningFactor (0.05f));
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
                    g.setColour (accentOrange.withAlpha (alpha));
                    g.fillEllipse (startX + (float) i * spacing - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);
                }
            }
            else
            {
                g.setColour (accentOrange.withAlpha (0.8f));
                g.setFont (heavyFont (10.0f).withExtraKerningFactor (0.05f));
                g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop(22).translated (0, 3), juce::Justification::centred, false);

                g.setColour (accentOrange);
                g.setFont (cousineFont (15.0f));
                g.drawText (value, getLocalBounds().removeFromBottom(26).translated (0, -2), juce::Justification::centred, false);
            }

            g.setColour (juce::Colours::black.withAlpha (0.9f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), smallCorner, 1.0f);
            return;
        }

        auto bounds = getLocalBounds().toFloat();
        fillRoundedGradient (g, bounds, blackPanel.brighter (0.02f), blackPanel, smallCorner);
        g.setColour (borderDark);
        g.drawRoundedRectangle (bounds.reduced (0.5f), smallCorner, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRoundedRectangle (bounds.removeFromTop (juce::jmax (12.0f, (float) getHeight() * 0.35f)), smallCorner);

        if (caption.isNotEmpty())
        {
            g.setColour (textMuted);
            g.setFont (heavyFont (7.0f));
            g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (12), juce::Justification::centredLeft);
        }

        g.setColour (accentOrange);
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
        fillRoundedGradient (g, bounds, juce::Colour (0xff171717).withAlpha (0.91f),
                             juce::Colours::black.withAlpha (0.9f), mediumCorner);
        g.setColour (borderLight.withAlpha (0.35f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), mediumCorner, 1.5f);

        // Orange accent stripe at top
        g.setColour (accentOrange);
        juce::Path stripe;
        stripe.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), 4.0f,
                                    mediumCorner, mediumCorner, true, true, false, false);
        g.fillPath (stripe);

        // Dismiss hint
        g.setColour (textMuted);
        g.setFont (heavyFont (11.5f).withExtraKerningFactor (0.06f));
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
        g.setColour (borderLight.withAlpha (0.2f));
        g.drawLine (footer.getX(), footer.getY(), footer.getRight(), footer.getY(), 1.0f);
        g.setColour (textMuted);
        g.setFont (heavyFont (11.0f));
        const juce::StringArray defs {
            "CHOP = auto-sliced bar segment   |   MIDI C2 = chop 1,  D2 = chop 2 ...",
            "CUE = loop-start point inside a chop   |   GRID = beat-grid anchor offset (ms)",
            "PITCH (utility strip) = global shift   |   PITCH (transport) = per-chop shift",
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
        g.setColour (accentOrange);
        g.setFont (heavyFont (14.0f).withExtraKerningFactor (0.08f));
        g.drawText (heading, juce::Rectangle<float> (col.getX(), y, col.getWidth(), 22.0f),
                    juce::Justification::centredLeft, false);
        y += 27.0f;
        g.setColour (borderLight.withAlpha (0.3f));
        g.drawLine (col.getX(), y, col.getRight(), y, 1.0f);
        y += 10.0f;
        g.setFont (heavyFont (12.6f));
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

// The white "CUE." brand wordmark (the bundled cue_logo_white.svg, including
// its trailing square period). Rendered as vector so it stays crisp at any UI
// scale and sits on the top line of the stacked title lockup.
static const char* const cueWordmarkSvg =
R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="20 -700 4123 760"><path fill="#FFFFFF" d="M1255.0 -267Q1250.0 -172 1186.0 -108.0Q1122.0 -44 997.5 -12.0Q873.0 20 683.0 20Q538.0 20 423.0 4.5Q308.0 -11 227.0 -48.5Q146.0 -86 103.0 -151.0Q60.0 -216 60.0 -315Q60.0 -414 103.0 -480.5Q146.0 -547 227.0 -586.5Q308.0 -626 423.0 -643.0Q538.0 -660 683.0 -660Q873.0 -660 998.0 -625.5Q1123.0 -591 1187.0 -524.0Q1251.0 -457 1256.0 -361H996.0Q984.0 -393 952.0 -417.5Q920.0 -442 856.0 -456.0Q792.0 -470 683.0 -470Q555.0 -470 475.0 -454.5Q395.0 -439 358.0 -405.0Q321.0 -371 321.0 -315Q321.0 -264 358.0 -232.0Q395.0 -200 475.0 -185.0Q555.0 -170 683.0 -170Q792.0 -170 855.5 -183.0Q919.0 -196 951.0 -218.5Q983.0 -241 995.0 -267Z M2303.0 -365V-640H2553.0V-320Q2553.0 -235 2521.5 -175.5Q2490.0 -116 2434.0 -77.5Q2378.0 -39 2305.0 -18.0Q2232.0 3 2148.5 11.5Q2065.0 20 1979.0 20Q1888.0 20 1803.0 11.5Q1718.0 3 1645.5 -18.0Q1573.0 -39 1519.0 -77.5Q1465.0 -116 1434.5 -175.5Q1404.0 -235 1404.0 -320V-640H1654.0V-365Q1654.0 -285 1693.0 -243.0Q1732.0 -201 1804.5 -185.5Q1877.0 -170 1979.0 -170Q2078.0 -170 2151.0 -185.5Q2224.0 -201 2263.5 -243.0Q2303.0 -285 2303.0 -365Z M2961.0 -270V-190H3721.0V0H2711.0V-640H3719.0V-450H2961.0V-370H3581.0V-270Z M4103.0 -151V0H3849.0V-151Z"/></svg>)SVG";

class HeaderComponent final : public juce::Component,
                              public juce::SettableTooltipClient,
                              private juce::Timer
{
public:
    explicit HeaderComponent (AudioPluginAudioProcessor& p)
        : processor (p)
    {
        setBufferedToImage (true);
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

        // Opt-in data-sharing toggle. Lights up (toggle "on" colour) when the
        // user has consented to share anonymous BPM/key detection data.
        configureButton (dataButton, "DATA", textPrimary);
        dataButton.getProperties().set ("cueStyle", "helpButton");
        dataButton.setToggleState (processor.isTelemetryEnabled(), juce::dontSendNotification);
        dataButton.setTooltip ("Help improve BPM & key detection by sharing anonymous "
                               "correction data. No audio or file names are ever sent.");
        dataButton.onClick = [this]
        {
            juce::PopupMenu menu;
            menu.addItem (1, "Share anonymous data to improve BPM & key detection",
                          true, processor.isTelemetryEnabled());
            menu.setLookAndFeel (&getLookAndFeel());
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (dataButton),
                                [this] (int result)
                                {
                                    if (result != 1)
                                        return;
                                    const bool newState = ! processor.isTelemetryEnabled();
                                    processor.setTelemetryEnabled (newState);
                                    dataButton.setToggleState (newState, juce::dontSendNotification);
                                    dataButton.repaint();
                                });
        };
        addAndMakeVisible (dataButton);
    }

    void resized() override
    {
        constexpr int buttonSize = 28;
        constexpr int rightMargin = 14;
        constexpr int topMargin = 10;
        constexpr int dataWidth = 46;
        constexpr int undoWidth = 54;
        constexpr int gap = 8;
        helpButton.setBounds (getWidth() - rightMargin - buttonSize, topMargin, buttonSize, buttonSize);
        dataButton.setBounds (helpButton.getX() - gap - dataWidth, topMargin, dataWidth, buttonSize);
        undoButton.setBounds (dataButton.getX() - gap - undoWidth, topMargin, undoWidth, buttonSize);
    }

    std::function<void()> onHelpRequested;

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
                                          (int) std::ceil (lockupW) + 60, 28),
                    juce::Justification::centredLeft, false);

        // Draw the version number next to SAMPLER on the top left
        g.setColour (textPrimary.withAlpha (0.45f));
        g.setFont (brandFont (10.0f));
        g.drawText ("v" + juce::String (CUE_VERSION_STRING),
                    juce::Rectangle<int> ((int) std::round (logoX + lockupW + 8.0f), samplerTop + 5,
                                          80, 28),
                    juce::Justification::centredLeft, false);

        g.setColour (textPrimary.withAlpha (0.85f));
        g.setFont (heavyFont (12.0f));
        g.drawFittedText ("CUE SOFTWARE", juce::Rectangle<int> (bounds.getRight() - 125, 38, 125, 16),
                          juce::Justification::centredRight, 1);
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
    SmoothHoverButton helpButton;
    SmoothHoverButton dataButton;
    SmoothHoverButton undoButton;
    std::unique_ptr<juce::Drawable> cueWordmark;
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
        processor.sampleChangeBroadcaster.removeChangeListener (this);
        processor.editChangeBroadcaster.removeChangeListener (this);
        horizontalScrollBar.removeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &processor.sampleChangeBroadcaster)
        {
            isSelectingAnalysisRegion = false;
            targetZoomLevel = zoomLevel = 0.20f;
            targetScrollPosition = scrollPosition = 0.0f;
            targetWaveformVerticalScale = waveformVerticalScale = defaultWaveformVerticalScale;
            updatePeakCache();
            rebuildWaveformPath();
            updateHorizontalScrollBar();
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
        drawPanelHole (g, { panelX + 767.0f, panelY + 13.0f }, 6.0f);
        drawPanelHole (g, { panelX + 13.0f, panelBounds.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { panelX + 767.0f, panelBounds.getBottom() - 13.0f }, 6.0f);

        auto frameBounds = juce::Rectangle<float> (panelX + 16.0f, panelY + 16.0f, 748.0f, panelH - 32.0f);
        fillRoundedGradient (g, frameBounds, panelInnerDark.brighter (0.14f), panelInnerDark.darker (0.2f), 10.0f);
        g.setColour (borderMid);
        g.drawRoundedRectangle (frameBounds.reduced (0.5f), 10.0f, 1.0f);

        auto displayBounds = juce::Rectangle<float> (panelX + 22.0f, panelY + 22.0f, 734.0f, panelH - 46.0f);
        fillRoundedGradient (g, displayBounds, blackPanel.brighter (0.08f), blackPanel.darker (0.3f), 4.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (displayBounds.reduced (0.5f), 4.0f, 1.0f);

        auto topShade = displayBounds.withHeight (361.0f);
        juce::ColourGradient darkFade (juce::Colours::black.withAlpha (0.4f),
                                       topShade.getCentreX(), topShade.getY(),
                                       juce::Colours::black.withAlpha (0.0f),
                                       topShade.getCentreX(), topShade.getBottom(), false);
        g.setGradientFill (darkFade);
        g.fillRoundedRectangle (topShade, 4.0f);

        auto topGlow = displayBounds.withHeight (162.438f);
        juce::ColourGradient glowFade (juce::Colours::white.withAlpha (0.04f),
                                       topGlow.getCentreX(), topGlow.getY(),
                                       juce::Colours::black.withAlpha (0.0f),
                                       topGlow.getCentreX(), topGlow.getBottom(), false);
        g.setGradientFill (glowFade);
        g.fillRoundedRectangle (topGlow, 4.0f);

        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (displayBounds, 4.0f);
            g.reduceClipRegion (clip);

            auto sheenBounds = juce::Rectangle<float> (1098.0f, 542.0f).withCentre ({ panelX + 329.447f, panelY + 109.251f });
            juce::Path sheen;
            sheen.addRectangle (sheenBounds);

            juce::AffineTransform transform = juce::AffineTransform::rotation (juce::degreesToRadians (12.0f),
                                                                               sheenBounds.getCentreX(),
                                                                               sheenBounds.getCentreY());
            sheen.applyTransform (transform);

            juce::ColourGradient sheenFade (juce::Colours::white.withAlpha (0.02f),
                                            sheenBounds.getCentreX(), sheenBounds.getY(),
                                            juce::Colours::white.withAlpha (0.0f),
                                            sheenBounds.getCentreX(), sheenBounds.getBottom(), false);
            g.setGradientFill (sheenFade);
            g.fillPath (sheen);
        }

        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawRoundedRectangle (displayBounds.reduced (3.0f), 2.0f, 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (frameBounds.reduced (1.0f), 9.0f, 2.0f);

        // ---- Waveform or placeholder ----
        if (! waveformPath.isEmpty())
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clipPath;
            clipPath.addRoundedRectangle (displayBounds.reduced (4.0f), 4.0f);
            g.reduceClipRegion (clipPath);

            // Subtle glow behind waveform
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.strokePath (waveformPath, juce::PathStrokeType (4.0f));

            // Filled waveform
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillPath (waveformPath);

            // Bright outline
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (waveformPath, juce::PathStrokeType (1.0f));

            // Centre line
            auto centreY = displayBounds.getCentreY();
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.drawHorizontalLine ((int) centreY, displayBounds.getX() + 4.0f, displayBounds.getRight() - 4.0f);

            auto hintBar = displayBounds.toNearestInt().reduced (18, 12).removeFromTop (34);
            fillRoundedGradient (g, hintBar.toFloat(), juce::Colour (0xff171717).withAlpha (0.58f),
                                 juce::Colours::black.withAlpha (0.48f), 6.0f);
            drawHelperText (g, "Click chop: preview/select   Drag edge: resize tempo   Shift-drag edge: snap to bar",
                            hintBar.reduced (10, 4), juce::Justification::centred, 10.8f,
                            textFaint.brighter (0.14f).withAlpha (0.86f));
        }
        else
        {
            auto messageBounds = displayBounds.toNearestInt().reduced (68, 132).withSizeKeepingCentre (560, 96);
            fillRoundedGradient (g, messageBounds.toFloat(), juce::Colour (0xff171717).withAlpha (0.48f),
                                 juce::Colours::black.withAlpha (0.42f), 8.0f);
            g.setColour (textMuted.brighter (0.22f).withAlpha (0.92f));
            g.setFont (heavyFont (13.4f));
            g.drawFittedText ("Drag an audio file here - or press LOAD SAMPLE in the transport below",
                              messageBounds.reduced (18, 12), juce::Justification::centred, 2);
        }

        // ---- BPM scan animation ----
        if (processor.isTempoAnalysisInProgress())
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
                        g.setFont (heavyFont (16.0f));
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

        g.setFont (heavyFont (10.5f).withExtraKerningFactor (0.06f));
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
        // also repoint the export button at the struck chop (it instantly
        // scrolls into view) -- this is what makes the button appear when you
        // play a key, not just when you click a chop.
        const auto triggerRevision = processor.getChopTriggerRevision();
        if (triggerRevision != lastObservedChopTriggerRevision)
        {
            lastObservedChopTriggerRevision = triggerRevision;
            const int triggeredId = processor.getLastTriggeredChopId();
            if (triggeredId >= 0)
            {
                chopAnimations[triggeredId].currentTriggerPulse = 1.0f; // Strike pulse!
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

        if (zoomChanged || scrollChanged || vertScaleChanged)
        {
            rebuildWaveformPath();
            updateHorizontalScrollBar();
        }

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

        const auto shouldRepaint = processor.isPlaying() || wasPlayingLastTick
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
        return { panelBounds.getX() + 26.0f, panelBounds.getY() + 26.0f, 726.0f, panelBounds.getHeight() - 54.0f };
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

            if ((double) chop.endSample < visibleStart || (double) chop.startSample > visibleEnd)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX = displayXForSamplePosition ((double) chop.endSample, visibleRange, displayBounds);
            auto chopBounds = juce::Rectangle<float> (juce::jmin (startX, endX), displayBounds.getY(),
                                                      std::abs (endX - startX), displayBounds.getHeight());

            auto& anim = chopAnimations[chop.id];
            const float hoverVal = anim.currentHoverAlpha;
            const float selectVal = anim.currentSelectAlpha;
            const float pulseVal = anim.currentTriggerPulse;
            const bool isSelected = (chop.id == chopState->selectedChopId);
            const bool isHovered = (chop.id == hoveredChopId);

            const auto baseFill = isAlternativeChop
                ? juce::Colour (0xffff4a6b).withAlpha (0.18f)   // Premium glowing rose-red (18% opacity)
                : juce::Colour (0xff3da5ff).withAlpha (0.08f);  // Standard cyan-blue (8% opacity)
            const auto hoverFill = juce::Colour (0xff8fd9ff).withAlpha (0.14f);
            const auto selectFill = juce::Colour (0xff00c950).withAlpha (0.22f);
            const auto fillColour = baseFill.interpolatedWith (hoverFill, hoverVal).interpolatedWith (selectFill, selectVal);

            const auto baseLine = isAlternativeChop
                ? juce::Colour (0xffff4a6b).withAlpha (0.75f)   // Crisp rose-red border (75% opacity)
                : juce::Colour (0xff3da5ff).withAlpha (0.55f);  // Standard cyan-blue border (55% opacity)
            const auto hoverLine = juce::Colour (0xff8fd9ff).withAlpha (0.82f);
            const auto selectLine = juce::Colour (0xff00f57a).withAlpha (0.98f);
            const auto lineColour = baseLine.interpolatedWith (hoverLine, hoverVal).interpolatedWith (selectLine, selectVal);

            fillRectGradient (g, chopBounds, fillColour.brighter (0.35f), fillColour.darker (0.25f));

            if (selectVal > 0.01f)
            {
                fillRectGradient (g, chopBounds.expanded (1.5f * selectVal, 0.0f),
                                  juce::Colour (0xff00f57a).withAlpha (0.18f * selectVal),
                                  juce::Colour (0xff00f57a).withAlpha (0.08f * selectVal));
                g.setColour (juce::Colour (0xff00f57a).withAlpha (0.22f * selectVal));
                g.drawRect (chopBounds.expanded (1.0f * selectVal, -3.0f), 1.5f);
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

            g.setColour (lineColour);
            g.drawLine (chopBounds.getX(), chopBounds.getY() + 4.0f, chopBounds.getX(), chopBounds.getBottom() - 4.0f, isSelected ? 2.2f : isHovered ? 1.9f : 1.5f);
            g.drawLine (chopBounds.getRight(), chopBounds.getY() + 4.0f, chopBounds.getRight(), chopBounds.getBottom() - 4.0f, isSelected ? 2.2f : isHovered ? 1.9f : 1.5f);

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

            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.strokePath (combined, juce::PathStrokeType (4.0f));
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillPath (combined);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (combined, juce::PathStrokeType (1.0f));

            g.setColour (juce::Colours::white.withAlpha (0.15f));
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
                                  juce::Colour (0xff8fd9ff).withAlpha (0.52f * alpha), 0.9f);
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
                                  juce::Colour (0xff8fd9ff).withAlpha (0.62f * alpha), 1.0f);
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
                              juce::Colour (0xff8fd9ff).withAlpha (0.42f),
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
                              juce::Colour (0xffff6900).withAlpha (0.82f),
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

        const auto playheadColour = juce::Colour (0xff00c950);
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

    void rebuildWaveformPath()
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

        juce::Path topPath;
        juce::Array<float> bottomYs;
        bottomYs.ensureStorageAllocated ((int) displayWidth);

        bool topStarted = false;

        for (int pixel = 0; pixel < (int) displayWidth; ++pixel)
        {
            int startSample = sampleOffset + pixel * samplesPerPixel;
            int endSample = juce::jmin (startSample + samplesPerPixel, numSamples);

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
            auto xPos = displayBounds.getX() + (float) i;
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
    int   animHz = waveformRefreshHz;

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
    explicit TransportSectionComponent (AudioPluginAudioProcessor& p)
        : processor (p),
          timeDisplay ("", "00:00:00", 16.0f, "timeBox"),
          tempoDisplay ("BPM", "--.-", 14.0f, "tempoBox"),
          keyDisplay ("KEY", "--", 13.0f, "keyBox"),
          startKnob (),
          cueKnob ("CUE", smallKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xff00c950)),
          gainKnob ("GAIN", smallKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xfffb2c36)),
          pitchKnob ("PITCH", smallKnobDiameter, 15.0f, "miniColourKnob", juce::Colour (0xfff6339a))
    {
        setBufferedToImage (true);
        startTimerHz (transportRefreshHz);

        configureButton (playButton, "", textPrimary);
        configureButton (pauseButton, "", textPrimary);
        configureButton (stopButton, "", textPrimary);
        configureButton (halfSpeedButton, "HALF\nTIME", textMuted);
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
            if (auto* parent = getParentComponent())
            {
                parent->repaint();
                for (auto* c : parent->getChildren())
                    c->repaint();
            }
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
        addAndMakeVisible (startKnob);
        addAndMakeVisible (cueKnob);
        addAndMakeVisible (gainKnob);
        addAndMakeVisible (pitchKnob);

        cueKnob.getSlider().setRange (0.0, 100.0, 0.1);
        cueKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        cueKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        cueKnob.getSlider().setTextValueSuffix (" %");
        cueKnob.captureCurrentValueAsDefault();

        gainKnob.getSlider().setRange (-24.0, 12.0, 0.1);
        gainKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        gainKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        gainKnob.getSlider().setTextValueSuffix (" dB");
        gainKnob.captureCurrentValueAsDefault();

        pitchKnob.getSlider().setRange (-12.0, 12.0, 0.1);
        pitchKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        pitchKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        pitchKnob.getSlider().setTextValueSuffix (" st");
        pitchKnob.captureCurrentValueAsDefault();

        cueKnob.getSlider().setTooltip ("CUE: sets where playback starts inside this chop - 0% = chop start, 100% = chop end. Looping always returns to this point. Alt-click to reset.");
        gainKnob.getSlider().setTooltip ("GAIN: volume for this chop only (-24 to +12 dB). Does not affect other chops. Alt-click to reset to 0 dB.");
        pitchKnob.getSlider().setTooltip ("PITCH (per-chop): pitch shift for this chop only, -12 to +12 semitones. Stacks on top of the global PITCH knob in the utility strip. Alt-click to reset.");

        updateDisplays();
    }

    void paint (juce::Graphics& g) override
    {
        auto topPanel = getTopPanelBounds().toFloat();

        fillGlassRounded (g, *this, topPanel, mediumCorner);

        drawPanelHole (g, { topPanel.getX() + 13.0f, topPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getRight() - 13.0f, topPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getX() + 13.0f, topPanel.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getRight() - 13.0f, topPanel.getBottom() - 13.0f }, 6.0f);

        g.setColour (glassTextMuted.withAlpha (0.35f));
        g.fillRect (216, (int) std::round (topPanel.getCentreY() - 24.0f), 1, 48);

        auto bottomPanel = getBottomPanelBounds().toFloat();

        fillGlassRounded (g, *this, bottomPanel, mediumCorner);

        drawPanelHole (g, { bottomPanel.getX() + 13.0f, bottomPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getRight() - 13.0f, bottomPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getX() + 13.0f, bottomPanel.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getRight() - 13.0f, bottomPanel.getBottom() - 13.0f }, 6.0f);

        auto badgeBounds = juce::Rectangle<float> (bottomPanel.getX() + 20.0f,
                                                   (topPanel.getBottom() + bottomPanel.getY()) * 0.5f - 9.0f,
                                                   122.0f,
                                                   18.0f);
        fillGlassRounded (g, *this, badgeBounds, 6.0f);

        g.setColour (glassText);
        g.setFont (heavyFont (10.8f).withExtraKerningFactor (0.08f));
        g.drawText ("CHOP CONTROLS",
                    badgeBounds.toNearestInt().withY ((int) std::round (badgeBounds.getY() - 1.0f)),
                    juce::Justification::centred, false);

        g.setColour (glassTextMuted.withAlpha (0.85f));
        g.setFont (heavyFont (9.6f).withExtraKerningFactor (0.06f));
        g.drawText (getMidiMappingText(),
                    juce::Rectangle<int> (570, (int) bottomPanel.getY() + 94, 198, 13),
                    juce::Justification::centred, false);

        drawHelperText (g, "Load audio - tempo/key are detected automatically",
                        juce::Rectangle<int> (394, (int) topPanel.getBottom() - 23, 374, 16), juce::Justification::centred, 10.0f);

        // Draw an outline around the warp edit section:
        const int warpRowY = (int) bottomPanel.getY() + 8;
        auto warpGroupBounds = juce::Rectangle<float> (468.0f, (float) warpRowY, 256.0f, 22.0f).expanded (6.0f, 4.0f);

        // Recessed slot/well backdrop
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (warpGroupBounds, 5.0f);

        // Outer dark shadow/border
        g.setColour (juce::Colours::black.withAlpha (0.32f));
        g.drawRoundedRectangle (warpGroupBounds.reduced (0.5f), 5.0f, 1.0f);

        // Highlight/accent rim (glows purple when warp mode is active)
        g.setColour (accentOrange.withAlpha (0.25f));
        g.drawRoundedRectangle (warpGroupBounds, 5.0f, 1.0f);
    }

    void resized() override
    {
        const auto topPanel = getTopPanelBounds();
        const int topCenterY = topPanel.getCentreY();
        playButton.setBounds (16, topCenterY - 24, 48, 48);
        pauseButton.setBounds (80, topCenterY - 24, 48, 48);
        stopButton.setBounds (144, topCenterY - 24, 48, 48);
        halfSpeedButton.setBounds (241, topCenterY - 24, 56, 48);
        startKnob.setBounds (322, topCenterY - 34, smallKnobDiameter, smallKnobDiameter + 19);
        timeDisplay.setBounds (413, topCenterY - 24, 160, 48);
        tempoDisplay.setBounds (589, topCenterY - 24, 80, 48);
        keyDisplay.setBounds (677, topCenterY - 24, 80, 48);

        auto bottomPanel = getBottomPanelBounds();

        // CUE/GAIN/PITCH live in the left bay, bounded on the right by the
        // centred CHOP @ TRANS button. Centre the cluster (3 knobs + 2 gaps)
        // both horizontally within that bay and vertically within the panel.
        constexpr int knobGap = 20;
        const int clusterWidth = 3 * smallKnobDiameter + 2 * knobGap;
        const int chopTransientsX = (getWidth() - 148) / 2;
        const int knobRowX = (chopTransientsX - clusterWidth) / 2;
        const int knobRowH = smallKnobDiameter + 20; // knob + label
        const int knobRowY = bottomPanel.getY() + (bottomPanel.getHeight() - knobRowH) / 2;

        auto knobRow = juce::Rectangle<int> (knobRowX, knobRowY, clusterWidth, knobRowH);
        cueKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));
        knobRow.removeFromLeft (knobGap);
        gainKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));
        knobRow.removeFromLeft (knobGap);
        pitchKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));

        chopTransientsButton.setBounds (chopTransientsX, bottomPanel.getY() + 39, 148, 48);

        auto buttonRow = juce::Rectangle<int> (468, bottomPanel.getY() + 33, 292, 60);
        barsButton.setBounds (buttonRow.removeFromLeft (140).withTrimmedTop (6).withHeight (48));
        buttonRow.removeFromLeft (12);
        loadButton.setBounds (buttonRow.removeFromLeft (140).withTrimmedTop (6).withHeight (48));

        // Warp controls live above the load/bars row inside the bottom panel.
        const int warpRowY = bottomPanel.getY() + 8;
        warpButton.setBounds (468, warpRowY, 80, 22);
        clearWarpButton.setBounds (556, warpRowY, 80, 22);
        warpDivisionCombo.setBounds (644, warpRowY, 80, 22);

        // Octave shift buttons sit side by side, with the (dynamic) MIDI
        // mapping hint to their right.
        const int octRowY = bottomPanel.getY() + 91;
        octDownButton.setBounds (468, octRowY, 44, 18);
        octUpButton.setBounds (516, octRowY, 44, 18);
    }

    juce::TextButton& getLoadButton() noexcept { return loadButton; }
    juce::TextButton& getPlayButton() noexcept { return playButton; }
    juce::TextButton& getPauseButton() noexcept { return pauseButton; }
    juce::TextButton& getStopButton() noexcept { return stopButton; }
    juce::TextButton& getBarsButton() noexcept { return barsButton; }
    juce::Slider& getStartSlider() noexcept { return startKnob.getSlider(); }
    juce::Slider& getCueSlider() noexcept { return cueKnob.getSlider(); }
    juce::Slider& getGainSlider() noexcept { return gainKnob.getSlider(); }
    juce::Slider& getPitchSlider() noexcept { return pitchKnob.getSlider(); }
    void refreshDisplays() { updateDisplays(); }
    std::function<void (double)> onTempoEntered;

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

        tooltip << "\nUse the TEMPO trim knob (utility strip) to nudge the grid if chops feel slightly off-beat.";

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

    juce::Rectangle<int> getTopPanelBounds() const
    {
        return getLocalBounds().removeFromTop (102);
    }

    juce::Rectangle<int> getBottomPanelBounds() const
    {
        constexpr int bottomPanelY = 116;
        return { 0, bottomPanelY, getWidth(), juce::jmax (0, getHeight() - bottomPanelY) };
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
    StartKnobComponent startKnob;
    LabelledKnob cueKnob;
    LabelledKnob gainKnob;
    LabelledKnob pitchKnob;
};

class UtilityStripComponent final : public juce::Component
{
public:
    UtilityStripComponent()
        : zoomKnob ("ZOOM", effectKnobDiameter, 15.0f),
          scrollKnob ("SCROLL", effectKnobDiameter, 15.0f),
          tempoKnob ("TEMPO", effectKnobDiameter, 15.0f),
          pitchKnob ("PITCH", effectKnobDiameter, 15.0f)
    {
        setBufferedToImage (true);

        configureButton (syncButton, "SYNC TO DAW", textMuted);
        syncButton.getProperties().set ("cueStyle", "utilitySync");
        syncButton.setClickingTogglesState (true);
        syncButton.setTooltip ("SYNC: locks playback speed to your DAW's host BPM automatically. Disable for free-running playback.");
        addAndMakeVisible (syncButton);

        zoomKnob.getSlider().getProperties().set ("cueStyle", "miniColourKnob");
        zoomKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        zoomKnob.getSlider().setMouseDragSensitivity (preciseMiniKnobDragSensitivity);
        zoomKnob.getSlider().setTooltip ("ZOOM: zoom into the waveform for detail. Use SCROLL to pan when zoomed in. Alt-click to reset.");
        zoomKnob.captureCurrentValueAsDefault();

        scrollKnob.getSlider().getProperties().set ("cueStyle", "miniColourKnob");
        scrollKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        scrollKnob.getSlider().setMouseDragSensitivity (getScrollDragSensitivity (0.0f));
        scrollKnob.getSlider().setTooltip ("SCROLL: pan the waveform left/right when zoomed in. Has no effect at zero zoom. Alt-click to reset.");
        scrollKnob.captureCurrentValueAsDefault();

        tempoKnob.getSlider().getProperties().set ("cueStyle", "miniColourKnob");
        tempoKnob.getSlider().setRange (-10.0, 10.0, 0.1);
        tempoKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        tempoKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        tempoKnob.getSlider().setTextValueSuffix (" BPM");
        tempoKnob.getSlider().setTooltip ("TEMPO trim: adds a fine BPM offset (-10 to +10 BPM) to shift where chop boundaries fall. Use when chops feel slightly early or late. Alt-click to reset to 0.");
        tempoKnob.captureCurrentValueAsDefault();

        pitchKnob.getSlider().getProperties().set ("cueStyle", "miniColourKnob");
        pitchKnob.getSlider().setRange (-12.0, 12.0, 0.1);
        pitchKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        pitchKnob.getSlider().setNumDecimalPlacesToDisplay (1);
        pitchKnob.getSlider().setTextValueSuffix (" st");
        pitchKnob.getSlider().setTooltip ("PITCH (global): shifts pitch of every chop together, -12 to +12 semitones. The per-chop PITCH knob adds on top of this. Alt-click to reset to 0.");
        pitchKnob.captureCurrentValueAsDefault();

        for (auto* knob : { &zoomKnob, &scrollKnob, &tempoKnob, &pitchKnob })
            addAndMakeVisible (*knob);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        fillGlassRounded (g, *this, bounds, largeCorner);

        drawHelperText (g, "Follows DAW BPM",
                        juce::Rectangle<int> (10, 107, getWidth() - 20, 20), juce::Justification::centred, 9.8f);
        drawHelperText (g, "Waveform detail",
                        juce::Rectangle<int> (10, 238, getWidth() - 20, 20), juce::Justification::centred, 9.8f);
        drawHelperText (g, "Pan when zoomed",
                        juce::Rectangle<int> (10, 361, getWidth() - 20, 20), juce::Justification::centred, 9.8f);
        drawHelperText (g, "Nudge chop grid",
                        juce::Rectangle<int> (10, 484, getWidth() - 20, 20), juce::Justification::centred, 9.8f);
        drawHelperText (g, "All chops pitch",
                        juce::Rectangle<int> (10, 607, getWidth() - 20, 20), juce::Justification::centred, 9.8f);
    }

    void resized() override
    {
        syncButton.setBounds (20, 44, 80, 56);
        zoomKnob.setBounds (28, 154, effectKnobDiameter, 82);
        scrollKnob.setBounds (28, 277, effectKnobDiameter, 82);
        tempoKnob.setBounds (28, 400, effectKnobDiameter, 82);
        pitchKnob.setBounds (28, 524, effectKnobDiameter, 82);
    }

    juce::Slider& getZoomSlider() noexcept { return zoomKnob.getSlider(); }
    juce::Slider& getScrollSlider() noexcept { return scrollKnob.getSlider(); }
    juce::Slider& getTempoSlider() noexcept { return tempoKnob.getSlider(); }
    juce::Slider& getPitchSlider() noexcept { return pitchKnob.getSlider(); }
    juce::TextButton& getSyncButton() noexcept { return syncButton; }
    void updateScrollSensitivityForZoom (float zoomControlValue)
    {
        scrollKnob.getSlider().setMouseDragSensitivity (getScrollDragSensitivity (zoomControlValue));
    }

private:
    SmoothAnimatedSwitchButton syncButton;
    LabelledKnob zoomKnob;
    LabelledKnob scrollKnob;
    LabelledKnob tempoKnob;
    LabelledKnob pitchKnob;
};

class EffectModuleComponent final : public juce::Component, private juce::Timer
{
public:
    EffectModuleComponent (juce::String moduleName, juce::String leftLabel, juce::String rightLabel,
                           juce::Rectangle<int> firstKnobArea, juce::Rectangle<int> secondKnobArea)
        : title (std::move (moduleName)),
          firstKnob (std::move (leftLabel), effectKnobDiameter, 14.0f),
          secondKnob (std::move (rightLabel), effectKnobDiameter, 14.0f),
          firstKnobBounds (firstKnobArea),
          secondKnobBounds (secondKnobArea)
    {
        configureButton (switchButton, "III", textPrimary.withAlpha (0.75f));
        switchButton.getProperties().set ("cueStyle", "effectSwitch");
        switchButton.setClickingTogglesState (true);
        switchButton.onClick = [this] { repaint(); };

        firstKnob.getSlider().getProperties().set ("cueStyle", "effectSquareKnob");
        firstKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        firstKnob.captureCurrentValueAsDefault();

        secondKnob.getSlider().getProperties().set ("cueStyle", "effectSquareKnob");
        secondKnob.getSlider().setValue (0.0, juce::dontSendNotification);
        secondKnob.captureCurrentValueAsDefault();

        addAndMakeVisible (switchButton);
        addAndMakeVisible (firstKnob);
        addAndMakeVisible (secondKnob);
    }

    ~EffectModuleComponent() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        auto titleArea = bounds.removeFromTop (28);

        auto titleFont = heavyFont (18.0f);
        juce::GlyphArrangement titleGlyphs;
        titleGlyphs.addLineOfText (titleFont, title, 0.0f, 0.0f);
        auto titleWidth = titleGlyphs.getBoundingBox (0, -1, true).getWidth();
        constexpr float markerSize = 8.0f;
        constexpr float titleGap = 8.0f;
        auto titleRowWidth = markerSize + titleGap + titleWidth;
        auto rowX = ((float) getWidth() - titleRowWidth) * 0.5f;
        auto markerBounds = juce::Rectangle<float> (rowX,
                                                    (float) titleArea.getCentreY() - markerSize * 0.5f,
                                                    markerSize,
                                                    markerSize);

        float position = switchButton.getToggleState() ? 1.0f : 0.0f;
        if (auto* animatedButton = dynamic_cast<SmoothAnimatedSwitchButton*> (&switchButton))
        {
            position = animatedButton->getCurrentAnimationPosition();
        }

        float clampedPos = juce::jlimit (0.0f, 1.0f, position);

        // LED marker underglow / drop shadow
        if (position > 0.0f)
        {
            drawSoftDropShadow (g, markerBounds, 0.0f, true, 2.4f * position, 0.0f, 4.0f, accentOrange.withAlpha (clampedPos));
        }

        auto offFillTop = panelDark.brighter (0.18f);
        auto offFillBottom = panelDark.darker (0.2f);
        auto onFillTop = accentOrange.brighter (0.45f);
        auto onFillBottom = accentOrange.darker (0.18f);

        auto currentFillTop = offFillTop.interpolatedWith (onFillTop, clampedPos);
        auto currentFillBottom = offFillBottom.interpolatedWith (onFillBottom, clampedPos);

        fillEllipseGradient (g, markerBounds, currentFillTop, currentFillBottom);

        auto offBorder = glassTextMuted;
        auto onBorder = accentOrange;
        auto currentBorder = offBorder.interpolatedWith (onBorder, clampedPos);

        g.setColour (currentBorder);
        g.drawEllipse (markerBounds, 1.0f);

        if (position < 1.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.8f * (1.0f - clampedPos)));
            g.drawEllipse (markerBounds.reduced (0.5f), 1.0f);
        }

        g.setColour (glassText);
        g.setFont (titleFont);
        auto titleBounds = juce::Rectangle<int> ((int) std::round (rowX + markerSize + titleGap),
                                                 titleArea.getY(),
                                                 (int) std::ceil (titleWidth),
                                                 titleArea.getHeight());
        g.drawText (title, titleBounds, juce::Justification::centredLeft, false);

        auto switchBounds = switchButton.getBounds();
        auto offLabelBounds = juce::Rectangle<int> (switchBounds.getX() - 24, switchBounds.getY() + 5, 16, 10);
        auto onLabelBounds = juce::Rectangle<int> (switchBounds.getRight() + 8, switchBounds.getY() + 5, 12, 10);

        // OFF label fades out to 0.35 alpha when ON
        auto offLabelColour = glassText.interpolatedWith (glassTextMuted.withAlpha (0.35f), clampedPos);
        g.setColour (offLabelColour);
        g.setFont (heavyFont (7.0f));
        g.drawText ("OFF", offLabelBounds, juce::Justification::centredLeft, false);

        // ON label fades in from 0.35 alpha of textMuted to bright accentOrange when ON
        auto onLabelColour = glassTextMuted.withAlpha (0.35f).interpolatedWith (accentOrange.darker (0.05f), clampedPos);
        g.setColour (onLabelColour);
        g.setFont (heavyFont (7.0f));
        g.drawText ("ON", onLabelBounds, juce::Justification::centredLeft, false);

        if (gainReductionReadoutVisible)
        {
            auto area = getGainReductionReadoutBounds().toFloat();

            // "GR" label at the top of the channel.
            g.setColour (textMuted);
            g.setFont (heavyFont (7.0f));
            g.drawText ("GR", area.removeFromTop (11.0f).toNearestInt(),
                        juce::Justification::centred, false);

            // dB readout at the bottom.
            const bool reducing = gainReductionDb > 0.05f;
            g.setColour (reducing ? accentOrange.brighter (0.2f) : textPrimary.withAlpha (0.4f));
            g.setFont (monoFont (10.0f));
            g.drawText (juce::String (gainReductionDb, 1),
                        area.removeFromBottom (13.0f).toNearestInt(),
                        juce::Justification::centred, false);

            // Segmented gain-reduction meter column between the label and the
            // value. Fills bottom-to-top with the amount of reduction (scaled to
            // a typical 12 dB span) using discrete square LED segments.
            auto track = area.withSizeKeepingCentre (10.0f, area.getHeight() - 4.0f);

            const int numSegments = 12;
            const float totalHeight = track.getHeight();
            const float segSpan = totalHeight / (float) numSegments;
            const float gapFrac = 0.30f;
            const float segHeight = segSpan * (1.0f - gapFrac);
            const float segWidth = track.getWidth();

            constexpr float displayMaxDb = 12.0f;
            const float frac = juce::jlimit (0.0f, 1.0f, gainReductionDb / displayMaxDb);

            for (int i = 0; i < numSegments; ++i)
            {
                // Segment 0 sits at the bottom and lights first.
                const float startFrac = (float) i / (float) numSegments;
                const float endFrac = (float) (i + 1) / (float) numSegments;
                const float lit = juce::jlimit (0.0f, 1.0f, (frac - startFrac) / (endFrac - startFrac));

                const float y0 = track.getBottom() - segSpan * ((float) (i + 1) - gapFrac * 0.5f);
                auto segRect = juce::Rectangle<float> (track.getX(), y0, segWidth, segHeight);

                if (lit > 0.0f)
                {
                    // Soft bloom behind the lit segment
                    g.setColour (accentOrange.withAlpha (0.18f * lit));
                    g.fillRoundedRectangle (segRect.expanded (1.5f, 1.0f), 1.0f);
                }

                // Segment fill: dark/translucent when unlit, bright orange when lit
                auto baseColour = juce::Colours::black.withAlpha (0.45f);
                auto litColour = accentOrange.brighter (0.18f);
                g.setColour (baseColour.interpolatedWith (litColour, lit));
                g.fillRoundedRectangle (segRect, 1.0f);
            }
        }

        // Lo-fi quantized scope between the knobs. Draws the live crushed output
        // when the module is ON and audio is present, otherwise an internal
        // crushed sine that keeps flowing (incl. while the module is OFF) so the
        // BITS / CRUSH knob shape is always previewed.
        if (scopeActive)
            paintScope (g);
    }

    void paintScope (juce::Graphics& g)
    {
        auto bounds = getScopeBounds().toFloat();
        auto screen = bounds.reduced (2.0f);

        // Assemble the sample window to plot. Live waveform only when the module
        // is ON and signal is present; otherwise the crushed idle sine.
        std::array<float, (size_t) kScopeN> pts;
        const bool on   = switchButton.getToggleState();
        const bool live = on && scopePeak > 0.0025f;
        if (live)
        {
            pts = scopeSamples;
        }
        else
        {
            // Mirror the processor's amount->DSP mapping so the idle preview
            // tracks the knobs exactly (see bitsAmountToDspBits / crushAmount...).
            const float a        = juce::jlimit (0.0f, 1.0f, scopeBitsAmount  * 0.01f);
            const float ac       = juce::jlimit (0.0f, 1.0f, scopeCrushAmount * 0.01f);
            const float bits     = 16.0f - 15.0f * std::sqrt (a);
            const float crushPct = 100.0f - 99.0f * std::sqrt (ac);
            const bool  quantize = bits     < 15.99f;
            const bool  resample = crushPct < 99.99f;
            const float steps    = std::pow (2.0f, bits - 1.0f);
            const float invSteps = 1.0f / steps;
            const double phaseStep = juce::jlimit (1.0e-4, 1.0, (double) (crushPct * 0.01f));

            double rphase = 1.0;
            float  held   = 0.0f;
            constexpr float cycles = 2.2f;
            for (int i = 0; i < kScopeN; ++i)
            {
                const float t = (float) i / (float) kScopeN;
                float x = std::sin (t * cycles * juce::MathConstants<float>::twoPi
                                    + (float) scopeIdlePhase) * 0.72f;
                if (resample)
                {
                    rphase += phaseStep;
                    if (rphase >= 1.0) { rphase -= 1.0; held = x; }
                    x = held;
                }
                if (quantize)
                    x = std::round (x * steps) * invSteps;
                pts[(size_t) i] = x;
            }
        }

        // Clip (invisibly) to the channel so the glow never bleeds onto the knobs.
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (bounds.getSmallestIntegerContainer());

        const float midY  = screen.getCentreY();
        const float halfH = screen.getHeight() * 0.5f * 0.84f;
        auto yFor = [&] (float v) { return midY - juce::jlimit (-1.0f, 1.0f, v) * halfH; };
        auto xFor = [&] (int i)   { return screen.getX() + screen.getWidth() * ((float) i / (float) (kScopeN - 1)); };

        // Sample-and-hold stepped trace (horizontal hold, then vertical step).
        juce::Path trace;
        float prevY = yFor (pts[0]);
        trace.startNewSubPath (xFor (0), prevY);
        for (int i = 1; i < kScopeN; ++i)
        {
            const float xi = xFor (i);
            const float yi = yFor (pts[(size_t) i]);
            trace.lineTo (xi, prevY);
            trace.lineTo (xi, yi);
            prevY = yi;
        }

        // Just the wave: a soft glow pass under a crisp core. Stays clearly
        // visible (a touch dimmer) when idle / OFF, brightest on live signal.
        const juce::PathStrokeType glow (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const juce::PathStrokeType core (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour (accentOrange.withAlpha (live ? 0.22f : 0.16f));
        g.strokePath (trace, glow);
        g.setColour (accentOrange.brighter (live ? 0.25f : 0.08f).withAlpha (live ? 0.95f : 0.72f));
        g.strokePath (trace, core);
    }

    void resized() override
    {
        switchButton.setBounds ((getWidth() - 36) / 2, 36, 36, 20);
        firstKnob.setBounds (firstKnobBounds.translated (0, -8));
        secondKnob.setBounds (secondKnobBounds.translated (0, -8));
    }

    juce::TextButton& getSwitchButton() noexcept { return switchButton; }
    LabelledKnob& getFirstKnob() noexcept        { return firstKnob; }
    LabelledKnob& getSecondKnob() noexcept       { return secondKnob; }

    void setGainReductionReadoutVisible (bool shouldShow) noexcept
    {
        if (gainReductionReadoutVisible != shouldShow)
        {
            gainReductionReadoutVisible = shouldShow;
            refreshAnimationTimer();
            repaint();
        }
    }

    void setGainReductionDb (float dB) noexcept
    {
        targetGainReductionDb = juce::jlimit (0.0f, 99.9f, dB);
        refreshAnimationTimer();
    }

    // Enables the lo-fi scope visualizer for this module (BIT CRUSHER).
    void setScopeActive (bool shouldShow) noexcept
    {
        if (scopeActive != shouldShow)
        {
            scopeActive = shouldShow;
            refreshAnimationTimer();
            repaint();
        }
    }

    bool isScopeActive() const noexcept { return scopeActive; }

    // Latest crushed-output window plus the current knob amounts (0..100), fed
    // from the editor timer. Peak decides live waveform vs. idle synth preview.
    void pushScopeFrame (const float* samples, int numSamples, float bitsAmount, float crushAmount) noexcept
    {
        float peak = 0.0f;
        const int n = juce::jmin (numSamples, kScopeN);
        for (int i = 0; i < kScopeN; ++i)
        {
            const float v = i < n ? samples[i] : 0.0f;
            scopeSamples[(size_t) i] = v;
            peak = juce::jmax (peak, std::abs (v));
        }
        scopePeak        = peak;
        scopeBitsAmount  = bitsAmount;
        scopeCrushAmount = crushAmount;
        refreshAnimationTimer();
    }

private:
    // Runs the shared per-frame timer whenever something on this module needs
    // animating: the compressor GR meter, or the bit-crusher scope while ON.
    void refreshAnimationTimer()
    {
        // The scope flows continuously whenever active (ON or OFF).
        const bool want = isShowing() && (gainReductionReadoutVisible || scopeActive);
        if (want && ! isTimerRunning())
            startTimerHz (animationFrameRateHz());
        else if (! want && isTimerRunning())
            stopTimer();
    }

    void timerCallback() override
    {
        if (! isShowing())
        {
            stopTimer();
            return;
        }

        const int hz = animationFrameRateHz();
        bool needMore = false;

        if (gainReductionReadoutVisible)
        {
            const float target = targetGainReductionDb;

            // Fast attack, slightly slower decay, using rate-independent lerp
            const float lerpFactor = (target > gainReductionDb) ? 0.45f : 0.25f;
            const float nextVal = gainReductionDb + (target - gainReductionDb) * frameRateLerp (lerpFactor, hz);

            if (std::abs (nextVal - gainReductionDb) > 0.01f)
            {
                gainReductionDb = nextVal;
                repaint (getGainReductionReadoutBounds());
            }
            else if (gainReductionDb != target)
            {
                gainReductionDb = target;
                repaint (getGainReductionReadoutBounds());
            }
            needMore = true;
        }

        if (scopeActive)
        {
            // Slow horizontal drift for the idle preview; live frames repaint anyway.
            scopeIdlePhase += juce::MathConstants<double>::twoPi * 0.35 / (double) juce::jmax (1, hz);
            repaint (getScopeBounds());
            needMore = true;
        }

        if (! needMore)
            stopTimer();
    }

    juce::Rectangle<int> getGainReductionReadoutBounds() const noexcept
    {
        // Vertical GR meter occupying the channel between the two knobs.
        constexpr int channelWidth = 46;
        return { getWidth() / 2 - channelWidth / 2, 66, channelWidth, 96 };
    }

    juce::Rectangle<int> getScopeBounds() const noexcept
    {
        // Display sits in the channel between the two knobs.
        constexpr int w = 48, h = 62;
        return { getWidth() / 2 - w / 2, 60, w, h };
    }

    static constexpr int kScopeN = 64;

    juce::String title;
    SmoothAnimatedSwitchButton switchButton;
    LabelledKnob firstKnob;
    LabelledKnob secondKnob;
    juce::Rectangle<int> firstKnobBounds;
    juce::Rectangle<int> secondKnobBounds;
    bool gainReductionReadoutVisible = false;
    float gainReductionDb = 0.0f;
    float targetGainReductionDb = 0.0f;

    bool  scopeActive = false;
    std::array<float, (size_t) kScopeN> scopeSamples {};
    float scopePeak = 0.0f;
    float scopeBitsAmount = 0.0f;
    float scopeCrushAmount = 0.0f;
    double scopeIdlePhase = 0.0;
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
    explicit StemRackComponent (AudioPluginAudioProcessor& p)
        : processorRef (p)
    {
        setBufferedToImage (true);

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
        separateButton.getProperties().set ("cueStyle", "flatAction");
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

        drawPanelHole (g, { 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { 13.0f, bounds.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, bounds.getBottom() - 13.0f }, 6.0f);

        g.setColour (glassText);
        g.setFont (heavyFont (14.0f).withExtraKerningFactor (0.10f));
        g.drawText ("STEMS", juce::Rectangle<int> (26, 13, 200, 24), juce::Justification::centredLeft);

        if (separatingNow)
        {
            paintProgress (g);
            return;
        }

        g.setColour (statusColour);
        g.setFont (heavyFont (9.5f).withExtraKerningFactor (0.06f));
        g.drawText (statusText, juce::Rectangle<int> (27, 39, 210, 18), juce::Justification::centredLeft);
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
        g.setFont (heavyFont (9.5f).withExtraKerningFactor (0.06f));
        g.setColour (accentOrange);
        g.drawText (loadingModel ? "PREPARING MODEL" : "SEPARATING",
                    juce::Rectangle<int> (27, 37, 200, 15), juce::Justification::centredLeft);
        if (! loadingModel)
        {
            g.setColour (textPrimary);
            g.drawText (juce::String (pct) + "%", juce::Rectangle<int> (27, 37, 210, 15), juce::Justification::centredRight);
        }

        // Recessed track.
        const juce::Rectangle<float> track (27.0f, 56.0f, 210.0f, 5.0f);
        const float r = track.getHeight() * 0.5f;
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (track, r);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
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

            auto fill = track.withWidth (fillW);
            juce::ColourGradient fillGrad (accentOrange.brighter (0.20f), fill.getX(), fill.getY(),
                                           accentOrange.darker (0.12f),  fill.getX(), fill.getBottom(), false);
            g.setGradientFill (fillGrad);
            g.fillRect (fill);

            // Brighter leading edge.
            g.setColour (accentOrange.brighter (0.5f).withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (fill.getRight() - 2.0f, fill.getY(), 2.0f, fill.getHeight()));

            // Moving highlight sweep along the filled portion.
            const float sweepW = 48.0f;
            const float sx     = -sweepW + (fillW + sweepW) * sweepPhase;
            juce::Rectangle<float> sweepR (track.getX() + sx, track.getY(), sweepW, track.getHeight());
            juce::ColourGradient sweepGrad (juce::Colours::white.withAlpha (0.0f), sweepR.getX(),     sweepR.getCentreY(),
                                            juce::Colours::white.withAlpha (0.0f), sweepR.getRight(), sweepR.getCentreY(), false);
            sweepGrad.addColour (0.5, juce::Colours::white.withAlpha (0.30f));
            g.setGradientFill (sweepGrad);
            g.fillRect (sweepR);
        }
    }

    void resized() override
    {
        constexpr int y = 11, h = 51, gap = 14;
        constexpr int leftEdge = 255, rightEdge = 758;
        constexpr int w = (rightEdge - leftEdge - 2 * gap) / 3; // 158

        bassBtn.setBounds   (leftEdge,                   y, w, h);
        drumsBtn.setBounds  (leftEdge + (w + gap),       y, w, h);
        vocalsBtn.setBounds (leftEdge + 2 * (w + gap),   y, w, h);

        // Info column, just below the "STEMS" title (shares the row with the status
        // line / progress bar, which are hidden whenever the button is shown).
        separateButton.setBounds (27, 37, 158, 25);
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

class EffectsRackComponent final : public juce::Component
{
public:
    EffectsRackComponent()
        : bitCrusher ("BIT CRUSHER", "BITS", "CRUSH",
                      { 14, 72, effectKnobDiameter, 83 }, { 134, 72, effectKnobDiameter, 83 }),
          compressor ("COMPRESSOR", "COMPRESS", "GAIN",
                      { 14, 72, effectKnobDiameter, 83 }, { 134, 72, effectKnobDiameter, 83 })
    {
        setBufferedToImage (true);
        addAndMakeVisible (bitCrusher);
        addAndMakeVisible (compressor);

        bitCrusher.setScopeActive (true);
        bitCrusher.getSwitchButton().setTooltip ("Enable / disable the Bit Crusher effect.");
        bitCrusher.getFirstKnob().getSlider().setTooltip ("BITS: amount of bit-depth reduction (0 % = clean, 100 % = crunchiest). Alt-click to reset.");
        bitCrusher.getSecondKnob().getSlider().setTooltip ("CRUSH: amount of sample-rate reduction (0 % = clean, 100 % = most lo-fi / aliased). Alt-click to reset.");

        compressor.getSwitchButton().setTooltip ("Enable / disable the Compressor effect.");
        compressor.getFirstKnob().getSlider().setTooltip ("COMPRESS: SSL-calibrated threshold (-15 to +15 dB). Lower values catch more of the signal. Ratio locked at 4:1. Alt-click to reset.");
        compressor.getSecondKnob().getSlider().setTooltip ("GAIN: manual make-up gain (0 to +20 dB) after compression. No limiter is applied; reduce gain if the output clips. Alt-click to reset.");
        compressor.setGainReductionReadoutVisible (true);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        fillGlassRounded (g, *this, bounds.reduced (0.5f), mediumCorner);

        drawPanelHole (g, { 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { 13.0f, bounds.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, bounds.getBottom() - 13.0f }, 6.0f);

        // Recessed glass well that holds the two effect modules.
        auto innerBounds = bounds.reduced (16.0f);
        g.setColour (juce::Colours::black.withAlpha (0.16f));
        g.fillRoundedRectangle (innerBounds, smallCorner);
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.drawRoundedRectangle (innerBounds.reduced (0.5f), smallCorner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (innerBounds.expanded (0.5f), smallCorner + 0.5f, 1.0f);

        g.setColour (glassTextMuted.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (33.0f, 315.0f, 212.0f, 1.0f));

        drawHelperText (g, "Switch must be ON before these knobs change audio",
                        juce::Rectangle<int> (42, 252, 194, 24));
        drawHelperText (g, "Use lightly after chops are playing cleanly",
                        juce::Rectangle<int> (42, 580, 194, 24));
    }

    void resized() override
    {
        bitCrusher.setBounds (33, 92, 212, 147);
        compressor.setBounds (33, 392, 212, 178);
    }

    EffectModuleComponent& getBitCrusherModule() noexcept { return bitCrusher; }
    EffectModuleComponent& getCompressorModule() noexcept { return compressor; }

private:
    EffectModuleComponent bitCrusher;
    EffectModuleComponent compressor;
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
        cue::fillRoundedGradient (g, bounds, juce::Colour (0xff2c2c2c),
                                  juce::Colour (0xff1b1b1b), 6.0f);
        g.setColour (accentOrange.withAlpha (0.85f));
        g.drawRoundedRectangle (bounds, 6.0f, 1.2f);

        g.setColour (accentOrange);
        g.fillRoundedRectangle (bounds.removeFromLeft (4.0f), 2.0f);

        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.setFont (cue::heavyFont (12.0f));
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
    processorRef.sampleChangeBroadcaster.addChangeListener (this);
    processorRef.editChangeBroadcaster.addChangeListener (this);

    backgroundImage = juce::ImageCache::getFromMemory (CueSamplerBinaryData::cue_background_png,
                                                       CueSamplerBinaryData::cue_background_pngSize);

    lookAndFeel = std::make_unique<cue::CueSamplerLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());
    addAndMakeVisible (contentComponent);

    headerComponent = std::make_unique<cue::HeaderComponent> (processorRef);
    waveformDisplayComponent = std::make_unique<cue::WaveformDisplayComponent> (processorRef);
    transportSectionComponent = std::make_unique<cue::TransportSectionComponent> (processorRef);
    utilityStripComponent = std::make_unique<cue::UtilityStripComponent>();
    effectsRackComponent = std::make_unique<cue::EffectsRackComponent>();
    stemRackComponent = std::make_unique<cue::StemRackComponent> (processorRef);

    // Measure the real display refresh rate from the vblank so animations run
    // at the display's native frame rate (120 Hz on ProMotion, 60 Hz otherwise).
    vblankRateMeter = juce::VBlankAttachment (this, [this] (double nowSeconds)
    {
        if (lastVBlankSeconds > 0.0)
            cue::observeVBlankInterval (nowSeconds - lastVBlankSeconds);
        lastVBlankSeconds = nowSeconds;
    });

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
        utilityStripComponent->getTempoSlider().setValue ((double) trimBpm, juce::dontSendNotification);
        transportSectionComponent->refreshDisplays();
        waveformDisplayComponent->repaint();
    };
    transportSectionComponent->getCueSlider().onValueChange = [this]
    {
        processorRef.setSelectedChopCueNormalized ((float) transportSectionComponent->getCueSlider().getValue() / 100.0f);
    };
    transportSectionComponent->getGainSlider().onValueChange = [this]
    {
        const auto newValue = transportSectionComponent->getGainSlider().getValue();
        processorRef.setSelectedChopGainDecibels ((float) newValue);
    };
    transportSectionComponent->getPitchSlider().onValueChange = [this]
    {
        const auto newValue = transportSectionComponent->getPitchSlider().getValue();
        processorRef.setSelectedChopPitchSemitones ((float) newValue);
    };

    utilityStripComponent->getZoomSlider().onValueChange = [this]
    {
        const auto zoomValue = (float) utilityStripComponent->getZoomSlider().getValue();
        processorRef.setWaveformZoom (zoomValue);
        utilityStripComponent->updateScrollSensitivityForZoom (zoomValue);
        waveformDisplayComponent->setZoom (zoomValue);
    };

    utilityStripComponent->getScrollSlider().onValueChange = [this]
    {
        const auto scrollValue = (float) utilityStripComponent->getScrollSlider().getValue();
        processorRef.setWaveformScroll (scrollValue);
        waveformDisplayComponent->setScroll (scrollValue);
    };

    waveformDisplayComponent->onZoomChanged = [this] (float z)
    {
        processorRef.setWaveformZoom (z);
        utilityStripComponent->updateScrollSensitivityForZoom (z);
        utilityStripComponent->getZoomSlider().setValue ((double) z, juce::dontSendNotification);
    };

    waveformDisplayComponent->onScrollChanged = [this] (float s)
    {
        processorRef.setWaveformScroll (s);
        utilityStripComponent->getScrollSlider().setValue ((double) s, juce::dontSendNotification);
    };

    utilityStripComponent->getTempoSlider().onValueChange = [this]
    {
        processorRef.setGridBpmTrim ((float) utilityStripComponent->getTempoSlider().getValue());
    };

    utilityStripComponent->getSyncButton().setToggleState (processorRef.getSyncToHost(), juce::dontSendNotification);
    utilityStripComponent->getSyncButton().onClick = [this]
    {
        processorRef.setSyncToHost (utilityStripComponent->getSyncButton().getToggleState());
    };

    utilityStripComponent->getPitchSlider().onValueChange = [this]
    {
        processorRef.setPitchSemitones ((float) utilityStripComponent->getPitchSlider().getValue());
    };

    {
        auto& bcModule    = effectsRackComponent->getBitCrusherModule();
        auto& bitsSlider  = bcModule.getFirstKnob().getSlider();
        auto& crushSlider = bcModule.getSecondKnob().getSlider();
        auto& bcSwitch    = bcModule.getSwitchButton();

        bitsSlider.setRange (0.0, 100.0, 0.0);
        bitsSlider.setValue ((double) processorRef.getBitCrusherBits(), juce::dontSendNotification);
        bitsSlider.setTextValueSuffix (" %");
        bcModule.getFirstKnob().captureCurrentValueAsDefault();

        crushSlider.setRange (0.0, 100.0, 0.0);
        crushSlider.setValue ((double) processorRef.getBitCrusherCrush(), juce::dontSendNotification);
        crushSlider.setTextValueSuffix (" %");
        bcModule.getSecondKnob().captureCurrentValueAsDefault();

        bcSwitch.setToggleState (processorRef.isBitCrusherEnabled(), juce::dontSendNotification);

        bitsSlider.onValueChange = [this, &bitsSlider]
        {
            processorRef.setBitCrusherBits ((float) bitsSlider.getValue());
        };
        crushSlider.onValueChange = [this, &crushSlider]
        {
            processorRef.setBitCrusherCrush ((float) crushSlider.getValue());
        };
        bcSwitch.onClick = [this]
        {
            auto& m = effectsRackComponent->getBitCrusherModule();
            processorRef.setBitCrusherEnabled (m.getSwitchButton().getToggleState());
            m.repaint();
        };
    }

    {
        auto& compModule       = effectsRackComponent->getCompressorModule();
        auto& thresholdSlider  = compModule.getFirstKnob().getSlider();
        auto& makeupSlider     = compModule.getSecondKnob().getSlider();
        auto& compSwitch       = compModule.getSwitchButton();

        thresholdSlider.setRange (-15.0, 15.0, 0.0);
        thresholdSlider.setValue ((double) processorRef.getCompressorThresholdDb(), juce::dontSendNotification);
        thresholdSlider.setTextValueSuffix (" dB");
        compModule.getFirstKnob().captureCurrentValueAsDefault();

        makeupSlider.setRange (0.0, 20.0, 0.0);
        makeupSlider.setValue ((double) processorRef.getCompressorMakeupDb(), juce::dontSendNotification);
        makeupSlider.setTextValueSuffix (" dB");
        compModule.getSecondKnob().captureCurrentValueAsDefault();

        compSwitch.setToggleState (processorRef.isCompressorEnabled(), juce::dontSendNotification);

        thresholdSlider.onValueChange = [this, &thresholdSlider]
        {
            processorRef.setCompressorThresholdDb ((float) thresholdSlider.getValue());
        };
        makeupSlider.onValueChange = [this, &makeupSlider]
        {
            processorRef.setCompressorMakeupDb ((float) makeupSlider.getValue());
        };
        compSwitch.onClick = [this]
        {
            auto& compressorModule = effectsRackComponent->getCompressorModule();
            processorRef.setCompressorEnabled (compressorModule.getSwitchButton().getToggleState());
            compressorModule.repaint();
        };
    }

    transportSectionComponent->getStartSlider().onValueChange = [this]
    {
        processorRef.setGridStartOffset ((float) (transportSectionComponent->getStartSlider().getValue() / 1000.0));
    };
    transportSectionComponent->getStartSlider().setValue ((double) processorRef.getGridStartOffset() * 1000.0,
                                                          juce::dontSendNotification);
    utilityStripComponent->getZoomSlider().setValue ((double) processorRef.getWaveformZoom(),
                                                     juce::dontSendNotification);
    utilityStripComponent->updateScrollSensitivityForZoom (processorRef.getWaveformZoom());
    waveformDisplayComponent->setZoom (processorRef.getWaveformZoom());
    utilityStripComponent->getScrollSlider().setValue ((double) processorRef.getWaveformScroll(),
                                                       juce::dontSendNotification);
    waveformDisplayComponent->setScroll (processorRef.getWaveformScroll());
    utilityStripComponent->getTempoSlider().setValue ((double) processorRef.getGridBpmTrim(),
                                                      juce::dontSendNotification);
    utilityStripComponent->getPitchSlider().setValue ((double) processorRef.getPitchSemitones(),
                                                      juce::dontSendNotification);
    transportSectionComponent->getBarsButton().setButtonText (juce::String (processorRef.getChopBarsCount())
                                                              + (processorRef.getChopBarsCount() == 1 ? " BAR" : " BARS"));

    juce::Component* sections[] = { headerComponent.get(),
                                    waveformDisplayComponent.get(),
                                    transportSectionComponent.get(),
                                    stemRackComponent.get(),
                                    utilityStripComponent.get(),
                                    effectsRackComponent.get() };

    for (auto* component : sections)
        contentComponent.addAndMakeVisible (*component);

    midiKeyboardComponent = std::make_unique<cue::GlassKeyboard> (processorRef.keyboardState);
    contentComponent.addAndMakeVisible (*midiKeyboardComponent);

    panelShadowEffect.setShadowProperties (defaultShadow);
    waveformDisplayComponent->setComponentEffect (&panelShadowEffect);
    transportSectionComponent->setComponentEffect (&panelShadowEffect);
    stemRackComponent->setComponentEffect (&panelShadowEffect);
    utilityStripComponent->setComponentEffect (&panelShadowEffect);
    effectsRackComponent->setComponentEffect (&panelShadowEffect);

    helpOverlayComponent = std::make_unique<cue::HelpOverlayComponent>();
    contentComponent.addChildComponent (*helpOverlayComponent); // invisible by default
    addKeyListener (helpOverlayComponent.get()); // so ? and Escape reach it

    headerComponent->onHelpRequested = [this]
    {
        const bool nowVisible = ! helpOverlayComponent->isVisible();
        helpOverlayComponent->setVisible (nowVisible);
    };

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
    setResizeLimits (juce::roundToInt ((float) cue::editorWidth * cue::minEditorScale),
                     juce::roundToInt ((float) cue::editorHeight * cue::minEditorScale),
                     juce::roundToInt ((float) cue::editorWidth * cue::maxEditorScale),
                     juce::roundToInt ((float) cue::editorHeight * cue::maxEditorScale));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) cue::editorWidth / (double) cue::editorHeight);
    setResizable (true, true);
    setSize (cue::editorWidth, cue::editorHeight);
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

    // Poll separation progress/ready → button enablement + status line.
    if (stemRackComponent != nullptr)
        stemRackComponent->refresh();

    if (effectsRackComponent == nullptr) return;

    const auto grDb = processorRef.getCompressorGainReductionDb();
    effectsRackComponent->getCompressorModule().setGainReductionDb (grDb);

    auto& crusherModule = effectsRackComponent->getBitCrusherModule();
    if (crusherModule.isScopeActive())
    {
        float scope[AudioPluginAudioProcessor::kBitCrusherScopeSize];
        processorRef.readBitCrusherScope (scope, AudioPluginAudioProcessor::kBitCrusherScopeSize);
        crusherModule.pushScopeFrame (scope, AudioPluginAudioProcessor::kBitCrusherScopeSize,
                                      processorRef.getBitCrusherBits(), processorRef.getBitCrusherCrush());
    }
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
    const auto widthScale = (float) getWidth() / (float) cue::editorWidth;
    const auto heightScale = (float) getHeight() / (float) cue::editorHeight;
    return juce::jlimit (cue::minEditorScale, cue::maxEditorScale, juce::jmin (widthScale, heightScale));
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
    juce::MessageManager::callAsync ([this, source]
    {
        if (source == &processorRef.sampleChangeBroadcaster)
        {
            utilityStripComponent->getZoomSlider().setValue ((double) processorRef.getWaveformZoom(),
                                                             juce::sendNotificationSync);
            utilityStripComponent->getScrollSlider().setValue ((double) processorRef.getWaveformScroll(),
                                                               juce::sendNotificationSync);
        }

        // Always re-sync utility strip controls from the processor so the UI
        // stays correct after a host-initiated state restore (e.g. FL Studio
        // undo / auto-save recall).
        utilityStripComponent->getPitchSlider().setValue ((double) processorRef.getPitchSemitones(),
                                                          juce::dontSendNotification);
        utilityStripComponent->getTempoSlider().setValue ((double) processorRef.getGridBpmTrim(),
                                                          juce::dontSendNotification);
        utilityStripComponent->getSyncButton().setToggleState (processorRef.getSyncToHost(),
                                                               juce::dontSendNotification);

        transportSectionComponent->refreshDisplays();

        // Light the on-screen keyboard key that maps to the previewed
        // (selected) chop; -1 clears it when nothing is selected.
        if (midiKeyboardComponent != nullptr)
            midiKeyboardComponent->setHighlightedNote (processorRef.getSelectedChopMidiNote());
    });
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Underlay for any letterboxing: hosts don't always honour the fixed
    // aspect ratio, and getUiScale() fits the smaller axis, so the faceplate
    // can fall short of the window edge. Painting the underlay with the same
    // background keeps that strip orange instead of a dark bar.
    if (const auto& underlay = cue::getModeTintedBackground (backgroundImage); underlay.isValid())
    {
        g.drawImage (underlay, getLocalBounds().toFloat(),
                     juce::RectanglePlacement (juce::RectanglePlacement::fillDestination));
    }
    else
    {
        juce::ColourGradient background (juce::Colour (0xff242424), 0.0f, 0.0f,
                                         juce::Colour (0xff111111), 0.0f, (float) getHeight(), false);
        g.setGradientFill (background);
        g.fillAll();
    }

    juce::Graphics::ScopedSaveState scaleState (g);
    g.addTransform (juce::AffineTransform::scale (getUiScale()));

    auto faceplate = juce::Rectangle<float> (0.0f, 0.0f, (float) cue::editorWidth, (float) cue::editorHeight);

    if (const auto& background = cue::getModeTintedBackground (backgroundImage); background.isValid())
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (faceplate, cue::largeCorner);
        g.reduceClipRegion (clip);
        g.drawImage (background, faceplate,
                     juce::RectanglePlacement (juce::RectanglePlacement::fillDestination));
    }
    else
    {
        cue::fillRoundedGradient (g, faceplate, cue::shellDark.brighter (0.1f),
                                  cue::shellDark.darker (0.22f), cue::largeCorner);
    }

    g.setColour (cue::borderLight.withAlpha (0.75f));
    g.drawRoundedRectangle (faceplate.reduced (0.5f), cue::largeCorner, 1.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (faceplate, cue::largeCorner);
        g.reduceClipRegion (clip);
        cue::fillRectGradient (g, juce::Rectangle<float> (faceplate.getX(), faceplate.getY(), faceplate.getWidth(), 6.0f),
                               accentOrange.brighter (0.26f), accentOrange.darker (0.12f));
    }

    auto leftRail = juce::Rectangle<int> (0, 0, cue::sideRailWidth, cue::editorHeight);
    auto rightRail = juce::Rectangle<int> (cue::editorWidth - cue::sideRailWidth, 0, cue::sideRailWidth, cue::editorHeight);

    paintSideRail (g, leftRail, true);
    paintSideRail (g, rightRail, false);

    // Glass chassis for the on-screen MIDI keyboard along the bottom strip.
    {
        auto keyboardPanel = juce::Rectangle<float> (96.0f, 798.0f, 1246.0f, 74.0f);
        juce::Path panelPath;
        panelPath.addRoundedRectangle (keyboardPanel, cue::mediumCorner);
        cue::fillGlassPath (g, panelPath, keyboardPanel);
    }

    // White like the website headline: the accent orange is invisible on the
    // orange gradient background.
    g.setColour (juce::Colours::white);
    g.setFont (cue::heavyFont (12.0f));
    g.drawText ("CHOP STATION", juce::Rectangle<int> (96, 116, 150, 16), juce::Justification::centredLeft, false);
}

void AudioPluginAudioProcessorEditor::resized()
{
    const auto scale = getUiScale();
    contentComponent.setTransform (juce::AffineTransform::scale (scale));
    contentComponent.setBounds (0, 0, cue::editorWidth, cue::editorHeight);

    headerComponent->setBounds (96, 32, 1246, 77);
    // Waveform condensed (411 → 330) to make room for the stem strip directly
    // beneath it (8 px gaps match the existing rhythm). Transport unchanged.
    waveformDisplayComponent->setBounds (96, 133, 782, 330);
    stemRackComponent->setBounds (96, 471, 782, 73);
    transportSectionComponent->setBounds (96, 552, 782, 236);
    utilityStripComponent->setBounds (910, 133, 120, 655);
    effectsRackComponent->setBounds (1062, 133, 278, 655);
    helpOverlayComponent->setBounds (96, 133, 782, 655);

    if (midiKeyboardComponent != nullptr)
    {
        midiKeyboardComponent->setKeyWidth (1230.0f / 49.0f); // 49 white keys, C1..B7
        midiKeyboardComponent->setBounds (104, 806, 1230, 58);
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
}

void AudioPluginAudioProcessorEditor::paintSideRail (juce::Graphics& g,
                                                     juce::Rectangle<int> bounds,
                                                     bool isLeftRail) const
{
    auto railArea = bounds.toFloat();
    auto railPath = cue::createRailPath (railArea, isLeftRail, cue::largeCorner);

    cue::fillGlassPath (g, railPath, railArea);

    g.setColour (cue::glassTextMuted.withAlpha (0.4f));
    const float x = isLeftRail ? railArea.getRight() - 0.5f : railArea.getX() + 0.5f;
    g.drawLine (x, railArea.getY(), x, railArea.getBottom(), 1.0f);

    auto slotBounds = juce::Rectangle<float> (railArea.getX() + 26.5f,
                                              railArea.getY() + 335.0f,
                                              10.0f,
                                              128.0f);
    cue::drawSlot (g, slotBounds);

    const auto railOriginX = railArea.getX();
    const auto railOriginY = railArea.getY();
    paintScrew (g, { railOriginX + 31.5f, railOriginY + 50.0f });
    paintScrew (g, { railOriginX + 31.5f, railOriginY + (float) cue::editorHeight - 50.0f });
}

void AudioPluginAudioProcessorEditor::paintScrew (juce::Graphics& g, juce::Point<float> centre) const
{
    auto bounds = juce::Rectangle<float> (20.0f, 20.0f).withCentre (centre);
    cue::fillEllipseGradient (g, bounds, cue::metalGrey.brighter (0.38f).withAlpha (0.86f),
                              cue::metalGrey.darker (0.42f).withAlpha (0.86f));

    g.setColour (cue::borderMid);
    g.drawEllipse (bounds, 1.0f);

    g.setColour (cue::railDark);
    g.drawLine (centre.x - 3.2f, centre.y - 3.2f, centre.x + 3.2f, centre.y + 3.2f, 2.0f);
    g.drawLine (centre.x - 3.2f, centre.y + 3.2f, centre.x + 3.2f, centre.y - 3.2f, 2.0f);
}
