#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CorrectionLog.h"

#include <BinaryData.h>

#include <cmath>
#include <functional>

namespace cue
{
namespace
{
constexpr int editorWidth = 1438;
constexpr int editorHeight = 798;
constexpr float minEditorScale = 0.75f;
constexpr float maxEditorScale = 1.5f;
constexpr float defaultWaveformVerticalScale = 0.75f;
constexpr int sideRailWidth = 64;
constexpr int headerRefreshHz = 20;
constexpr int waveformRefreshHz = 30;
constexpr int transportRefreshHz = 20;
constexpr int smallKnobDiameter = 50;
constexpr int effectKnobDiameter = 64;
constexpr int preciseMiniKnobDragSensitivity = 3200;
constexpr int maxZoomedScrollDragSensitivity = 13200;
constexpr float zoomResponseMidpoint = 0.5f;
constexpr float zoomMappedMidpoint = 0.75f;

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

juce::Typeface::Ptr getSyneTypeface()
{
    static auto typeface = juce::Typeface::createSystemTypefaceFor (CueSamplerBinaryData::Synewght_ttf,
                                                                    CueSamplerBinaryData::Synewght_ttfSize);
    return typeface;
}


juce::Font heavyFont (float height)
{
    static const auto registeredTypeface = getSyneTypeface();
    juce::ignoreUnused (registeredTypeface);

    return { juce::FontOptions ("Syne", "ExtraBold", height) };
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
    button.setColour (juce::TextButton::buttonColourId, blackPanel);
    button.setColour (juce::TextButton::buttonOnColourId, panelDark.brighter (0.08f));
    button.setColour (juce::TextButton::textColourOffId, textColour);
    button.setColour (juce::TextButton::textColourOnId, textColour);
    button.setWantsKeyboardFocus (false);
    button.setMouseClickGrabsKeyboardFocus (false);
}

void drawHelperText (juce::Graphics& g, const juce::String& text,
                     juce::Rectangle<int> bounds,
                     juce::Justification justification = juce::Justification::centred,
                     float fontHeight = 9.6f)
{
    g.setColour (textFaint.brighter (0.14f).withAlpha (0.86f));
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

void drawSlot (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    fillRoundedGradient (g, bounds, blackPanel.brighter (0.08f), blackPanel.darker (0.25f), bounds.getWidth() * 0.5f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (bounds, bounds.getWidth() * 0.5f, 1.0f);
}

void drawPanelHole (juce::Graphics& g, juce::Point<float> centre, float diameter)
{
    auto bounds = juce::Rectangle<float> (diameter, diameter).withCentre (centre);
    g.setColour (juce::Colours::white.withAlpha (0.1f));
    g.fillEllipse (bounds.translated (0.0f, -1.0f));
    fillEllipseGradient (g, bounds, blackPanel.brighter (0.08f), blackPanel.darker (0.3f));
    g.setColour (juce::Colours::black);
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
} // namespace

class CueSamplerLookAndFeel final : public juce::LookAndFeel_V4
{
public:
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

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override
    {
        const auto style = getCueStyle (button);

        if (style == "transportSquare" || style == "halfTime"
            || style == "flatAction" || style == "utilitySync" || style == "effectSwitch")
        {
            auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
            auto pressedOffset = isButtonDown ? 2.0f : 0.0f;
            bounds = bounds.translated (0.0f, pressedOffset);

            if (style == "transportSquare")
            {
                if (isMouseOverButton)
                    drawSoftDropShadow (g, bounds, 10.0f, false, 3.0f, 0.0f, 12.0f, accentOrange);
                else
                    drawSoftDropShadow (g, bounds, 10.0f, false, 1.5f, 3.0f, 4.0f);

                juce::ColourGradient gradient (juce::Colour (0xff202020), bounds.getCentreX(), bounds.getY(),
                                               juce::Colour (0xff1a1a1a), bounds.getCentreX(), bounds.getBottom(), false);
                g.setGradientFill (gradient);
                g.fillRoundedRectangle (bounds, 10.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.35f) : borderMid);
                g.drawRoundedRectangle (bounds, 10.0f, 2.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.06f));
                g.drawRoundedRectangle (bounds.reduced (1.0f), 9.0f, 1.0f);
                return;
            }

            if (style == "halfTime")
            {
                if (isMouseOverButton)
                    drawSoftDropShadow (g, bounds, 8.0f, false, 3.0f, 0.0f, 12.0f, accentOrange);
                else
                    drawSoftDropShadow (g, bounds, 8.0f, false, 1.2f, 2.0f, 3.0f);

                auto base = juce::Colour (0xff252525);
                if (button.getToggleState())
                    base = accentOrange.withAlpha (0.32f).interpolatedWith (juce::Colour (0xff252525), 0.45f);
                if (isMouseOverButton)
                    base = base.brighter (0.05f);

                fillRoundedGradient (g, bounds, base.brighter (0.03f), base.darker (0.08f), 8.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.35f) : borderDark);
                g.drawRoundedRectangle (bounds, 8.0f, 1.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.05f));
                g.drawRoundedRectangle (bounds.reduced (1.0f), 7.0f, 1.0f);

                auto slot = juce::Rectangle<float> (12.0f, 4.0f).withCentre ({ bounds.getCentreX(), bounds.getY() + 7.5f });
                const auto slotTop = button.getToggleState() ? accentOrange.brighter (0.22f) : blackPanel.brighter (0.08f);
                const auto slotBottom = button.getToggleState() ? accentOrange.darker (0.18f) : blackPanel.darker (0.25f);
                fillRoundedGradient (g, slot, slotTop, slotBottom, 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.drawRoundedRectangle (slot, 2.0f, 1.0f);
                return;
            }

            if (style == "utilitySync")
            {
                if (isMouseOverButton)
                    drawSoftDropShadow (g, bounds, 8.0f, false, 3.0f, 0.0f, 12.0f, accentOrange);
                else
                    drawSoftDropShadow (g, bounds, 8.0f, false, 1.2f, 2.0f, 3.0f);

                auto base = juce::Colour (0xff252525);
                if (button.getToggleState())
                    base = accentOrange.withAlpha (0.32f).interpolatedWith (juce::Colour (0xff252525), 0.45f);
                if (isMouseOverButton)
                    base = base.brighter (0.05f);

                fillRoundedGradient (g, bounds, base.brighter (0.02f), base.darker (0.06f), 8.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.35f) : borderDark);
                g.drawRoundedRectangle (bounds, 8.0f, 1.0f);
                g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.05f));
                g.drawRoundedRectangle (bounds.reduced (1.0f), 7.0f, 1.0f);

                auto slot = juce::Rectangle<float> (16.0f, 4.0f).withCentre ({ bounds.getCentreX(), bounds.getY() + 10.0f });
                const auto slotTop = button.getToggleState() ? accentOrange.brighter (0.22f) : blackPanel.brighter (0.08f);
                const auto slotBottom = button.getToggleState() ? accentOrange.darker (0.18f) : blackPanel.darker (0.25f);
                fillRoundedGradient (g, slot, slotTop, slotBottom, 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.65f));
                g.drawRoundedRectangle (slot, 2.0f, 1.0f);
                return;
            }

            if (style == "effectSwitch")
            {
                auto trackBounds = bounds.reduced (1.0f);

                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.fillRoundedRectangle (trackBounds.translated (0.0f, 1.0f), 4.0f);

                fillRoundedGradient (g, trackBounds, blackPanel.brighter (0.08f), blackPanel.darker (0.25f), 4.0f);
                g.setColour (juce::Colours::black);
                g.drawRoundedRectangle (trackBounds, 4.0f, 1.0f);

                juce::ColourGradient innerShadow (juce::Colour (0xff0a0a0a), trackBounds.getCentreX(), trackBounds.getY(),
                                                  juce::Colour (0xff1a1a1a), trackBounds.getCentreX(), trackBounds.getBottom(), false);
                g.setGradientFill (innerShadow);
                g.fillRoundedRectangle (trackBounds.reduced (1.0f), 3.0f);

                if (button.getToggleState())
                {
                    g.setColour (accentOrange.withAlpha (0.15f));
                    g.fillRoundedRectangle (trackBounds.reduced (1.0f), 3.0f);
                }

                auto thumbWidth = trackBounds.getWidth() * 0.5f;
                auto thumbBounds = trackBounds.withWidth (thumbWidth).reduced (1.0f);
                
                if (button.getToggleState())
                    thumbBounds.setX (trackBounds.getRight() - thumbWidth + 1.0f);
                else
                    thumbBounds.setX (trackBounds.getX() + 1.0f);

                auto thumbColour = button.getToggleState() ? accentOrange.darker(0.1f) : juce::Colour(0xff444444);
                if (isMouseOverButton)
                    thumbColour = thumbColour.brighter (0.1f);

                juce::ColourGradient thumbGrad (thumbColour.brighter (0.1f), thumbBounds.getCentreX(), thumbBounds.getY(),
                                                thumbColour.darker (0.2f), thumbBounds.getCentreX(), thumbBounds.getBottom(), false);
                g.setGradientFill (thumbGrad);
                g.fillRoundedRectangle (thumbBounds, 3.0f);

                g.setColour (button.getToggleState() ? accentOrange.brighter(0.2f) : borderLight.brighter(0.2f));
                g.drawRoundedRectangle (thumbBounds, 3.0f, 1.0f);

                g.setColour (juce::Colours::black.withAlpha (0.6f));
                float centreX = thumbBounds.getCentreX();
                float gripY = thumbBounds.getY() + 3.0f;
                float gripH = thumbBounds.getHeight() - 6.0f;
                g.fillRect (centreX - 2.0f, gripY, 1.0f, gripH);
                g.fillRect (centreX, gripY, 1.0f, gripH);
                g.fillRect (centreX + 2.0f, gripY, 1.0f, gripH);

                return;
            }

            if (isMouseOverButton)
                drawSoftDropShadow (g, bounds, 4.0f, false, 3.0f, 0.0f, 12.0f, accentOrange);
            else
                drawSoftDropShadow (g, bounds, 4.0f, false, 1.3f, 2.0f, 3.0f);

            juce::ColourGradient gradient (juce::Colour (0xff2a2a2a), bounds.getCentreX(), bounds.getY(),
                                           juce::Colour (0xff1c1c1c), bounds.getCentreX(), bounds.getBottom(), false);
            g.setGradientFill (gradient);
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.35f) : borderDark);
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
            g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.05f));
            g.drawRoundedRectangle (bounds.reduced (1.0f), 3.0f, 1.0f);
            return;
        }

        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto corner = juce::jlimit (4.0f, 10.0f, bounds.getHeight() * 0.18f);

        auto base = backgroundColour;

        if (isMouseOverButton)
            base = base.brighter (0.08f);

        if (isButtonDown)
            base = base.darker (0.15f);

        if (isMouseOverButton)
            drawSoftDropShadow (g, bounds, corner, false, 3.0f, 0.0f, 12.0f, accentOrange);
        else
            drawSoftDropShadow (g, bounds, corner, false, 1.2f, 2.0f, 3.0f);

        fillRoundedGradient (g, bounds, base.brighter (0.1f), base.darker (0.28f), corner);

        g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.35f) : borderDark);
        g.drawRoundedRectangle (bounds, corner, 1.0f);

        g.setColour (isMouseOverButton ? accentOrange.withAlpha (0.12f) : juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), juce::jmax (1.0f, corner - 1.0f), 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool, bool) override
    {
        const auto style = getCueStyle (button);
        auto bounds = button.getLocalBounds().reduced (4, 4);

        if (style == "transportSquare")
        {
            juce::Path icon;
            const auto iconName = getCueIcon (button);
            auto iconBounds = bounds.toFloat().withSizeKeepingCentre (20.0f, 20.0f);

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
            g.setColour (button.getToggleState() ? textPrimary : juce::Colour (0xff777777));
            g.setFont (heavyFont (8.0f));
            g.drawFittedText (button.getButtonText(), bounds.withTrimmedTop (10), juce::Justification::centred, 2);
            return;
        }

        if (style == "flatAction")
        {
            g.setColour (juce::Colour (0xff99a1af));
            g.setFont (heavyFont (14.0f));
            g.drawFittedText (button.getButtonText(), bounds, juce::Justification::centred, 1);
            return;
        }

        if (style == "helpButton")
        {
            g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                     : juce::TextButton::textColourOffId));
            g.setFont (heavyFont (18.0f));
            g.drawFittedText (button.getButtonText(), bounds.translated (0, -1), juce::Justification::centred, 1);
            return;
        }

        if (style == "utilitySync")
        {
            g.setColour (button.getToggleState() ? textPrimary : juce::Colour (0xff777777));
            g.setFont (heavyFont (10.0f));
            g.drawFittedText (button.getButtonText(), bounds.withTrimmedTop (18), juce::Justification::centred, 2);
            return;
        }

        if (style == "effectSwitch")
        {
            return; // Handled entirely in drawButtonBackground
        }

        if (style == "waveScaleStep")
        {
            g.setColour (textPrimary.withAlpha (0.9f));
            g.setFont (heavyFont (20.0f));
            g.drawFittedText (button.getButtonText(), bounds.translated (0, -1),
                              juce::Justification::centred, 1);
            return;
        }

        auto fontSize = juce::jlimit (7.0f, 15.0f, (float) bounds.getHeight() * 0.32f);

        g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                 : juce::TextButton::textColourOffId));
        g.setFont (heavyFont (fontSize));
        g.drawFittedText (button.getButtonText(), bounds, juce::Justification::centred, 2);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        if (getCueStyle (slider) == "effectSquareKnob")
        {
            juce::ignoreUnused (rotaryStartAngle, rotaryEndAngle);

            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);

            bool isHovered = slider.isMouseOverOrDragging();
            if (isHovered)
                drawSoftDropShadow (g, bounds, 0.0f, true, 2.5f, 0.0f, 9.0f, accentOrange);
            else
                drawSoftDropShadow (g, bounds, 0.0f, true, 1.2f, 2.0f, 3.0f);

            fillEllipseGradient (g, bounds, borderMid.darker (0.18f), borderDark.darker (0.35f));
            g.setColour (isHovered ? accentOrange.withAlpha (0.22f) : juce::Colours::white.withAlpha (0.06f));
            g.drawEllipse (bounds.reduced (0.6f), 1.0f);

            auto innerCircle = bounds.reduced (4.0f);
            fillEllipseGradient (g, innerCircle, juce::Colour (0xff383838), juce::Colour (0xff171717));
            g.setColour (borderMid);
            g.drawEllipse (innerCircle, 1.0f);

            auto pointerCentre = innerCircle.getCentre();
            auto localAngle = juce::jmap (sliderPosProportional, 0.0f, 1.0f,
                                          -juce::MathConstants<float>::pi * 0.8f,
                                          juce::MathConstants<float>::pi * 0.8f);

            float radius = innerCircle.getWidth() * 0.5f;
            float indicatorLength = radius * 0.55f;
            float indicatorOffsetY = radius - indicatorLength * 0.5f - 2.0f;
            auto indicator = juce::Rectangle<float> (2.0f, indicatorLength)
                                 .withCentre ({ pointerCentre.x, pointerCentre.y - indicatorOffsetY });
                                 
            juce::Path indicatorPath;
            indicatorPath.addRoundedRectangle (indicator, 1.0f);
            indicatorPath.applyTransform (juce::AffineTransform::rotation (localAngle, pointerCentre.x, pointerCentre.y));

            g.fillPath (indicatorPath, juce::AffineTransform::translation (0.0f, 0.5f));
            g.setColour (accentOrange);
            g.fillPath (indicatorPath);
            
            juce::Path trackArc, meterArc;
            float arcRadius = bounds.getWidth() * 0.5f + 2.0f;
            float meterStart = -juce::MathConstants<float>::pi * 0.8f;
            float meterEnd = juce::MathConstants<float>::pi * 0.8f;
            
            trackArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, meterEnd, true);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (sliderPosProportional > 0.01f)
            {
                meterArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, localAngle, true);
                g.setColour (accentOrange);
                g.strokePath (meterArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            return;
        }

        if (getCueStyle (slider) == "startTiltKnob")
        {
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);

            bool isHovered = slider.isMouseOverOrDragging();
            if (isHovered)
                drawSoftDropShadow (g, bounds, 0.0f, true, 2.5f, 0.0f, 9.0f, accentOrange);
            else
                drawSoftDropShadow (g, bounds, 0.0f, true, 1.2f, 2.0f, 3.0f);

            fillEllipseGradient (g, bounds, borderMid.darker (0.18f), borderDark.darker (0.35f));
            g.setColour (isHovered ? accentOrange.withAlpha (0.22f) : juce::Colours::white.withAlpha (0.05f));
            g.drawEllipse (bounds.reduced (0.6f), 1.0f);

            auto innerCircle = bounds.reduced (4.0f);
            fillEllipseGradient (g, innerCircle, juce::Colour (0xff383838), juce::Colour (0xff171717));
            g.setColour (borderMid);
            g.drawEllipse (innerCircle, 1.0f);

            auto pointerCentre = innerCircle.getCentre();
            float radius = innerCircle.getWidth() * 0.5f;
            float indicatorLength = radius * 0.55f;
            float indicatorOffsetY = radius - indicatorLength * 0.5f - 2.0f;
            auto pointer = juce::Rectangle<float> (2.0f, indicatorLength)
                               .withCentre ({ pointerCentre.x, pointerCentre.y - indicatorOffsetY });
                               
            auto localAngle = juce::jmap (sliderPosProportional, 0.0f, 1.0f,
                                          -juce::MathConstants<float>::pi * 0.8f,
                                          juce::MathConstants<float>::pi * 0.8f);
            juce::Path indicator;
            indicator.addRoundedRectangle (pointer, 1.0f);
            indicator.applyTransform (juce::AffineTransform::rotation (localAngle, pointerCentre.x, pointerCentre.y));

            g.fillPath (indicator, juce::AffineTransform::translation (0.0f, 0.5f));
            g.setColour (accentOrange);
            g.fillPath (indicator);
            
            juce::Path trackArc, meterArc;
            float arcRadius = bounds.getWidth() * 0.5f + 2.0f;
            float meterStart = -juce::MathConstants<float>::pi * 0.8f;
            float meterEnd = juce::MathConstants<float>::pi * 0.8f;
            
            trackArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, meterEnd, true);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (sliderPosProportional > 0.01f)
            {
                meterArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, localAngle, true);
                g.setColour (accentOrange);
                g.strokePath (meterArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            return;
        }

        if (getCueStyle (slider) == "miniColourKnob")
        {
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);
            auto accent = getCueAccent (slider, accentOrange);

            bool isHovered = slider.isMouseOverOrDragging();
            if (isHovered)
                drawSoftDropShadow (g, bounds, 0.0f, true, 2.5f, 0.0f, 9.0f, accent);
            else
                drawSoftDropShadow (g, bounds, 0.0f, true, 1.2f, 2.0f, 3.0f);

            fillEllipseGradient (g, bounds, borderMid.darker (0.18f), borderDark.darker (0.35f));
            g.setColour (isHovered ? accent.withAlpha (0.22f) : juce::Colours::white.withAlpha (0.06f));
            g.drawEllipse (bounds.reduced (0.6f), 1.0f);

            auto innerCircle = bounds.reduced (4.0f);
            fillEllipseGradient (g, innerCircle, juce::Colour (0xff383838), juce::Colour (0xff171717));
            g.setColour (borderMid);
            g.drawEllipse (innerCircle, 1.0f);

            auto centre = innerCircle.getCentre();
            float radius = innerCircle.getWidth() * 0.5f;
            float indicatorLength = radius * 0.55f;
            float indicatorOffsetY = radius - indicatorLength * 0.5f - 2.0f;
            auto indicator = juce::Rectangle<float> (2.0f, indicatorLength)
                                 .withCentre ({ centre.x, centre.y - indicatorOffsetY });
                                 
            auto localAngle = juce::jmap (sliderPosProportional, 0.0f, 1.0f,
                                          -juce::MathConstants<float>::pi * 0.8f,
                                          juce::MathConstants<float>::pi * 0.8f);
            juce::Path indicatorPath;
            indicatorPath.addRoundedRectangle (indicator, 1.0f);
            indicatorPath.applyTransform (juce::AffineTransform::rotation (localAngle, centre.x, centre.y));

            g.fillPath (indicatorPath, juce::AffineTransform::translation (0.0f, 0.5f));
            g.setColour (accent);
            g.fillPath (indicatorPath);
            
            juce::Path trackArc, meterArc;
            float arcRadius = bounds.getWidth() * 0.5f + 2.0f;
            float meterStart = -juce::MathConstants<float>::pi * 0.8f;
            float meterEnd = juce::MathConstants<float>::pi * 0.8f;
            
            trackArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, meterEnd, true);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (sliderPosProportional > 0.01f)
            {
                meterArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, meterStart, localAngle, true);
                g.setColour (accent);
                g.strokePath (meterArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            return;
        }

        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);

        bool isHovered = slider.isMouseOverOrDragging();
        if (isHovered)
            drawSoftDropShadow (g, bounds, 0.0f, true, 2.5f, 0.0f, 9.0f, accentOrange);
        else
            drawSoftDropShadow (g, bounds, 0.0f, true, 1.2f, 2.0f, 3.0f);

        fillEllipseGradient (g, bounds, borderMid.darker (0.18f), borderDark.darker (0.35f));

        g.setColour (isHovered ? accentOrange.withAlpha (0.22f) : juce::Colours::white.withAlpha (0.06f));
        g.drawEllipse (bounds.reduced (0.6f), 1.0f);

        auto inner = bounds.reduced (4.0f);
        fillRoundedGradient (g, inner, panelDark.brighter (0.05f), panelDark.darker (0.2f), inner.getWidth() * 0.5f);
        g.setColour (borderMid);
        g.drawEllipse (inner, 1.0f);

        auto centre = inner.getCentre();
        auto radius = inner.getWidth() * 0.36f;
        auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        auto pointerEnd = centre.getPointOnCircumference (radius, angle - juce::MathConstants<float>::halfPi);

        juce::Path pointer;
        pointer.startNewSubPath (centre.x, centre.y);
        pointer.lineTo (pointerEnd.x, pointerEnd.y);

        g.setColour (accentOrange.withAlpha (0.35f));
        g.strokePath (pointer, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (accentOrange);
        g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (pointerEnd));
        
        juce::Path trackArc, meterArc;
        float arcRadius = bounds.getWidth() * 0.5f + 1.5f;
        trackArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (sliderPosProportional > 0.01f)
        {
            meterArc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
            g.setColour (accentOrange);
            g.strokePath (meterArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
};

class OptResetSlider final : public juce::Slider
{
public:
    void captureCurrentValueAsDefault() noexcept
    {
        defaultValue = getValue();
        hasDefaultValue = true;
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
    double defaultValue = 0.0;
    bool hasDefaultValue = false;
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

        configureTextLabel (label, text, labelHeight, textMuted, juce::Justification::centred);
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

        configureTextLabel (label, "GRID", 16.0f, textMuted, juce::Justification::centred);
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

        setTooltip ("Output level meter - Green = safe, Yellow = loud, Red = clipping. Turn down the GAIN knob if it clips.");

        configureButton (helpButton, "?", textPrimary);
        helpButton.getProperties().set ("cueStyle", "helpButton");
        helpButton.setTooltip ("Open the quick-start reference guide.");
        helpButton.onClick = [this] { if (onHelpRequested) onHelpRequested(); };
        addAndMakeVisible (helpButton);
    }

    void resized() override
    {
        constexpr int buttonSize = 28;
        constexpr int rightMargin = 14;
        constexpr int topMargin = 10;
        helpButton.setBounds (getWidth() - rightMargin - buttonSize, topMargin, buttonSize, buttonSize);
    }

    std::function<void()> onHelpRequested;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour (borderLight);
        g.drawLine ((float) bounds.getX(), (float) bounds.getBottom() - 1.0f,
                    (float) bounds.getRight(), (float) bounds.getBottom() - 1.0f, 1.0f);

        auto logoCircle = juce::Rectangle<float> (50.0f, 50.0f).withCentre ({ 40.0f, 38.0f });
        auto meterBounds = logoCircle.expanded (5.0f);
        const auto meterLevel = juce::jlimit (0.0f, 1.0f, processor.getOutputMeterLevel());
        const auto meterVisualLevel = std::sqrt (meterLevel);
        constexpr float meterStartAngle = juce::degreesToRadians (135.0f);
        constexpr float meterEndAngle = juce::degreesToRadians (405.0f);
        constexpr float meterTrackThickness = 3.5f;
        constexpr float meterGlowThickness = 6.5f;
        const auto meterColour = getMeterColourForLevel (meterLevel);

        juce::Path meterTrack;
        meterTrack.addCentredArc (meterBounds.getCentreX(), meterBounds.getCentreY(),
                                  meterBounds.getWidth() * 0.5f, meterBounds.getHeight() * 0.5f,
                                  0.0f, meterStartAngle, meterEndAngle, true);
        g.setColour (juce::Colour (0xff1f1f1f));
        g.strokePath (meterTrack, juce::PathStrokeType (meterTrackThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (meterVisualLevel > 0.001f)
        {
            juce::Path meterArc;
            meterArc.addCentredArc (meterBounds.getCentreX(), meterBounds.getCentreY(),
                                    meterBounds.getWidth() * 0.5f, meterBounds.getHeight() * 0.5f,
                                    0.0f, meterStartAngle,
                                    juce::jmap (meterVisualLevel, 0.0f, 1.0f, meterStartAngle, meterEndAngle), true);

            g.setColour (meterColour.withAlpha (0.16f + 0.12f * meterVisualLevel));
            g.strokePath (meterArc, juce::PathStrokeType (meterGlowThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (meterColour);
            g.strokePath (meterArc, juce::PathStrokeType (meterTrackThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        fillEllipseGradient (g, logoCircle, accentOrange.brighter (0.35f), accentOrange.darker (0.28f));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawEllipse (logoCircle.expanded (0.5f), 1.0f);

        g.setColour (textPrimary);
        g.setFont (heavyFont (16.0f));
        g.drawFittedText ("CUE", logoCircle.toNearestInt(), juce::Justification::centred, 1);

        const auto titleFont = heavyFont (60.0f);
        juce::GlyphArrangement titleGlyphs;
        titleGlyphs.addLineOfText (titleFont, "SAMPLER", 0.0f, 0.0f);
        const int samplerTextWidth = (int) std::ceil (titleGlyphs.getBoundingBox (0, -1, true).getWidth());

        const int titleX = 96;
        g.setColour (textPrimary);
        g.setFont (titleFont);
        g.drawText ("SAMPLER", juce::Rectangle<int> (titleX, 0, samplerTextWidth + 4, 60),
                    juce::Justification::centredLeft, false);

        const int dotX = titleX + samplerTextWidth;
        g.setColour (accentOrange);
        g.drawText (".", juce::Rectangle<int> (dotX, 0, 20, 60), juce::Justification::centredLeft, false);

        const int betaX = dotX + 18;
        g.setColour (textMuted);
        g.setFont (heavyFont (18.0f));
        g.drawText ("(beta)", juce::Rectangle<int> (betaX, 36, 90, 20), juce::Justification::centredLeft, false);

        g.setColour (textMuted.brighter (0.25f));
        g.setFont (heavyFont (12.0f));
        g.drawFittedText ("CUE SOFTWARE", juce::Rectangle<int> (bounds.getRight() - 125, 38, 125, 16),
                          juce::Justification::centredRight, 1);
    }

private:
    void timerCallback() override
    {
        if (! shouldRunRealtimeUi (*this))
            return;

        repaint (4, 4, 72, 68);
    }

    AudioPluginAudioProcessor& processor;
    juce::TextButton helpButton;
};

class WaveformDisplayComponent final : public juce::Component,
                                        public juce::ChangeListener,
                                        public juce::FileDragAndDropTarget,
                                        public juce::SettableTooltipClient,
                                        private juce::Timer
{
public:
    WaveformDisplayComponent (AudioPluginAudioProcessor& p)
        : processor (p)
    {
        setBufferedToImage (true);
        setTooltip ("Click a chop to select and preview it.  Double-click to toggle favourite (pink highlight).  "
                    "Drag an audio file here to load it.");
        processor.sampleChangeBroadcaster.addChangeListener (this);
        processor.editChangeBroadcaster.addChangeListener (this);
        lastObservedChopTriggerRevision = processor.getChopTriggerRevision();
        updatePeakCache();
        rebuildWaveformPath();
        startTimerHz (waveformRefreshHz);

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
    }

    ~WaveformDisplayComponent() override
    {
        processor.sampleChangeBroadcaster.removeChangeListener (this);
        processor.editChangeBroadcaster.removeChangeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &processor.sampleChangeBroadcaster)
        {
            isSelectingAnalysisRegion = false;
            waveformVerticalScale = defaultWaveformVerticalScale;
            updatePeakCache();
            rebuildWaveformPath();
        }
        setBufferedToImage (false);  // force repaint
        repaint();
        setBufferedToImage (true);
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
        hoveredDisplayX = 0.0f;
        hoveredChopId = -1;
        if (edgeDragChopId < 0)
            edgeHoverKind = 0;
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! isPositionInsideDisplay (event.position))
            return;

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
                edgeDragLiveSample = sampleForDisplayPosition (event.position.x);
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                setBufferedToImage (false);
                repaint();
                setBufferedToImage (true);
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
            return;
        }

        const int    totalSamples = sampleData->buffer.getNumSamples();
        const double sr           = sampleData->sampleRate > 0.0 ? sampleData->sampleRate : 48000.0;
        const int    minLen       = juce::jmax (1, (int) std::round (sr * 0.05)); // 50 ms minimum chop

        double newSample = sampleForDisplayPosition (mousePos.x);

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
        setBufferedToImage (false);
        repaint();
        setBufferedToImage (true);
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
                processor.resizeChopBoundary (edgeDragChopId, newStart, newEnd);
        }

        edgeDragChopId     = -1;
        edgeDragKind       = 0;
        edgeDragLiveSample = 0.0;
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
            setBufferedToImage (false);
            repaint();
            setBufferedToImage (true);
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

                zoomLevel      = newZoom;
                scrollPosition = newScroll;
                rebuildWaveformPath();
                setBufferedToImage (false);
                repaint();
                setBufferedToImage (true);

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
        auto panelBounds = juce::Rectangle<float> (780.0f, 409.0f).withCentre (bounds.getCentre());

        fillRoundedGradient (g, panelBounds, panelDark.brighter (0.1f), panelDark.darker (0.18f), mediumCorner);
        g.setColour (borderDark);
        g.drawRoundedRectangle (panelBounds.reduced (0.5f), mediumCorner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.1f));
        g.drawRoundedRectangle (panelBounds.reduced (1.0f), mediumCorner - 1.0f, 1.0f);

        const auto panelX = panelBounds.getX();
        const auto panelY = panelBounds.getY();
        drawPanelHole (g, { panelX + 13.0f, panelY + 13.0f }, 6.0f);
        drawPanelHole (g, { panelX + 767.0f, panelY + 13.0f }, 6.0f);
        drawPanelHole (g, { panelX + 13.0f, panelY + 396.0f }, 6.0f);
        drawPanelHole (g, { panelX + 767.0f, panelY + 396.0f }, 6.0f);

        auto frameBounds = juce::Rectangle<float> (panelX + 16.0f, panelY + 16.0f, 748.0f, 377.0f);
        fillRoundedGradient (g, frameBounds, panelInnerDark.brighter (0.14f), panelInnerDark.darker (0.2f), 10.0f);
        g.setColour (borderMid);
        g.drawRoundedRectangle (frameBounds.reduced (0.5f), 10.0f, 1.0f);

        auto displayBounds = juce::Rectangle<float> (panelX + 22.0f, panelY + 22.0f, 734.0f, 363.0f);
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
            drawHelperText (g, "Click chop: preview/select   Double-click: favorite   Hold 2s + drag: export",
                            hintBar.reduced (10, 4), juce::Justification::centred, 10.8f);
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

                    const float progress = (float) holdTickCount / (float) kHoldTicksRequired;
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
                        g.setFont (juce::Font (juce::FontOptions().withHeight (16.0f)));
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
    }

    void setZoom (float newZoom)
    {
        zoomLevel = juce::jlimit (0.0f, 1.0f, newZoom);
        rebuildWaveformPath();
        setBufferedToImage (false);
        repaint();
        setBufferedToImage (true);
    }

    void setScroll (float newScroll)
    {
        scrollPosition = juce::jlimit (0.0f, 1.0f, newScroll);
        rebuildWaveformPath();
        setBufferedToImage (false);
        repaint();
        setBufferedToImage (true);
    }

    std::function<void(float)> onZoomChanged;
    std::function<void(float)> onScrollChanged;

private:
    void stepWaveformVerticalScale (float delta)
    {
        const float next = juce::jlimit (0.25f, 4.0f, waveformVerticalScale + delta);
        if (std::abs (next - waveformVerticalScale) < 1.0e-4f)
            return;
        waveformVerticalScale = next;
        rebuildWaveformPath();
        setBufferedToImage (false);
        repaint();
        setBufferedToImage (true);
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
        setBufferedToImage (false);
        repaint();
        setBufferedToImage (true);
    }

    void timerCallback() override
    {
        if (! shouldRunRealtimeUi (*this))
            return;

        if (isHoldingToPlay && ! processor.isPlaying())
            processor.startPlayback();

        // Accumulate hold ticks for drag-export detection
        if (holdChopId >= 0 && ! exportDragFired)
        {
            if (++holdTickCount >= kHoldTicksRequired && ! exportDragReady)
            {
                exportDragReady = true;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                setBufferedToImage (false);
                repaint();
                setBufferedToImage (true);
            }
        }

        followTriggeredChopIfNeeded();

        const auto currentPlayheadPosition = processor.getPlaybackSamplePosition();
        const auto targetHoverAlpha = isHoveringDisplay ? 1.0f : 0.0f;
        hoverAnimationAlpha += (targetHoverAlpha - hoverAnimationAlpha) * 0.22f;
        if (std::abs (targetHoverAlpha - hoverAnimationAlpha) < 0.01f)
            hoverAnimationAlpha = targetHoverAlpha;

        const bool isScanning = processor.isTempoAnalysisInProgress();
        if (isScanning)
            ++scanAnimFrame;

        const auto shouldRepaint = processor.isPlaying() || wasPlayingLastTick
                                || std::abs (currentPlayheadPosition - lastPaintedPlayheadSample) > 0.5
                                || std::abs (hoverAnimationAlpha - lastPaintedHoverAlpha) > 0.01f
                                || (isHoveringDisplay && std::abs (hoveredDisplayX - lastPaintedHoverX) > 0.5f)
                                || lastTempoUiRevision != processor.getTempoUiRevision()
                                || isScanning
                                || exportDragReady
                                || (holdChopId >= 0 && ! exportDragReady && holdTickCount > 0);

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
        auto panelBounds = juce::Rectangle<float> (780.0f, 409.0f).withCentre (bounds.getCentre());
        return { panelBounds.getX() + 26.0f, panelBounds.getY() + 26.0f, 726.0f, 355.0f };
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

    void followTriggeredChopIfNeeded()
    {
        const auto triggerRevision = processor.getChopTriggerRevision();
        if (triggerRevision == lastObservedChopTriggerRevision)
            return;

        lastObservedChopTriggerRevision = triggerRevision;
        scrollToChop (processor.getLastTriggeredChopId());
    }

    void scrollToChop (int chopId)
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
        const double targetOffset = chopLength >= visibleSamples * 0.8
                                  ? chopStart - visibleSamples * 0.1
                                  : ((chopStart + chopEnd) * 0.5) - visibleSamples * 0.5;
        const float targetScroll = juce::jlimit (0.0f, 1.0f, (float) (targetOffset / (double) maxOffset));

        if (std::abs (targetScroll - scrollPosition) < 0.0005f)
            return;

        setScroll (targetScroll);
        if (onScrollChanged)
            onScrollChanged (targetScroll);
    }

    void updateHoverState (juce::Point<float> position)
    {
        const auto displayBounds = getDisplayBounds();
        isHoveringDisplay = displayBounds.contains (position);
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
        const float anchorX = displayXForSamplePosition (
            (double) (edgeDragKind == 1 ? chop->endSample : chop->startSample),
            visibleRange, displayBounds);
        const float liveX   = displayXForSamplePosition (edgeDragLiveSample, visibleRange, displayBounds);

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        const auto ghostBounds = juce::Rectangle<float> (juce::jmin (anchorX, liveX),
                                                          displayBounds.getY(),
                                                          std::abs (liveX - anchorX),
                                                          displayBounds.getHeight());

        fillRectGradient (g, ghostBounds, juce::Colour (0xff00f57a).withAlpha (0.24f),
                          juce::Colour (0xff00f57a).withAlpha (0.10f));

        g.setColour (juce::Colour (0xff00f57a).withAlpha (0.95f));
        g.drawLine (liveX, displayBounds.getY() + 2.0f, liveX, displayBounds.getBottom() - 2.0f, 2.0f);
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

        for (const auto& chop : chopState->chops)
        {
            if ((double) chop.endSample < visibleStart || (double) chop.startSample > visibleEnd)
                continue;

            const auto startX = displayXForSamplePosition ((double) chop.startSample, visibleRange, displayBounds);
            const auto endX = displayXForSamplePosition ((double) chop.endSample, visibleRange, displayBounds);
            auto chopBounds = juce::Rectangle<float> (juce::jmin (startX, endX), displayBounds.getY(),
                                                      std::abs (endX - startX), displayBounds.getHeight());

            const bool isSelected = chop.id == chopState->selectedChopId;
            const bool isHovered = chop.id == hoveredChopId;
            const auto fillColour = isSelected ? juce::Colour (0xff00c950).withAlpha (0.22f)
                                               : isHovered ? juce::Colour (0xff8fd9ff).withAlpha (0.14f)
                                                           : juce::Colour (0xff3da5ff).withAlpha (0.08f);
            const auto lineColour = isSelected ? juce::Colour (0xff00f57a).withAlpha (0.98f)
                                               : isHovered ? juce::Colour (0xff8fd9ff).withAlpha (0.82f)
                                                           : juce::Colour (0xff3da5ff).withAlpha (0.55f);

            fillRectGradient (g, chopBounds, fillColour.brighter (0.35f), fillColour.darker (0.25f));

            if (isSelected)
            {
                fillRectGradient (g, chopBounds.expanded (1.5f, 0.0f),
                                  juce::Colour (0xff00f57a).withAlpha (0.18f),
                                  juce::Colour (0xff00f57a).withAlpha (0.08f));
                g.setColour (juce::Colour (0xff00f57a).withAlpha (0.22f));
                g.drawRect (chopBounds.expanded (1.0f, -3.0f), 1.5f);
            }
            else if (isHovered)
            {
                fillRectGradient (g, chopBounds.expanded (0.5f, 0.0f),
                                  juce::Colour (0xff8fd9ff).withAlpha (0.15f),
                                  juce::Colour (0xff8fd9ff).withAlpha (0.06f));
            }

            if (isHovered)
            {
                const float cx = chopBounds.getCentreX();
                const float cy = chopBounds.getCentreY();
                const float maxR = juce::jmin (18.0f, chopBounds.getWidth() * 0.38f);
                if (maxR >= 6.0f)
                {
                    const float triH = maxR * 1.4f;
                    const float triW = maxR * 1.2f;
                    g.setColour (juce::Colours::black.withAlpha (0.52f));
                    g.fillEllipse (cx - maxR, cy - maxR, maxR * 2.0f, maxR * 2.0f);
                    juce::Path tri;
                    tri.addTriangle (cx - triW * 0.33f, cy - triH * 0.5f,
                                     cx - triW * 0.33f, cy + triH * 0.5f,
                                     cx + triW * 0.67f, cy);
                    g.setColour (juce::Colours::white.withAlpha (0.92f));
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
                const auto fillAlpha = isSelected ? 0.40f : isHovered ? 0.26f : 0.18f;
                const auto borderAlpha = isSelected ? 0.8f : isHovered ? 0.55f : 0.38f;

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
        const auto guideColour = accentOrange;
        lastPaintedHoverAlpha = hoverAnimationAlpha;
        lastPaintedHoverX = hoverX;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle (displayBounds, 4.0f);
        g.reduceClipRegion (clipPath);

        g.setColour (guideColour.withAlpha (0.08f * hoverAnimationAlpha));
        g.fillRect (juce::Rectangle<float> (hoverX - 5.0f, displayBounds.getY() + 4.0f, 10.0f, displayBounds.getHeight() - 8.0f));
        g.setColour (guideColour.withAlpha (0.3f * hoverAnimationAlpha));
        g.drawLine (hoverX, displayBounds.getY() + 6.0f, hoverX, displayBounds.getBottom() - 6.0f, 1.0f);

        auto handleBounds = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ hoverX, displayBounds.getY() + 14.0f });
        g.setColour (guideColour.withAlpha (0.22f * hoverAnimationAlpha));
        g.fillEllipse (handleBounds.expanded (4.0f));
        g.setColour (guideColour.withAlpha (0.92f * hoverAnimationAlpha));
        g.fillEllipse (handleBounds);
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
    float scrollPosition = 0.0f;
    float waveformVerticalScale = defaultWaveformVerticalScale;
    juce::TextButton verticalMinusButton;
    juce::TextButton verticalPlusButton;
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
    int scanAnimFrame = 0;

    // Hold-to-export drag state
    int  holdChopId      = -1;
    int  holdTickCount   = 0;
    bool exportDragReady = false;
    bool exportDragFired = false;
    static constexpr int kHoldTicksRequired = 2 * waveformRefreshHz; // 120 ticks @ 60 Hz = 2 s

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
            juce::PopupMenu menu;
            menu.addSectionHeader ("Chop at transients");
            menu.addItem (1, "Light  (heavy hits only)");
            menu.addItem (2, "Medium (kicks + snares)");

            auto& proc = processor;
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetComponent (&chopTransientsButton),
                                [&proc] (int chosen)
                                {
                                    if (chosen == 1)
                                        proc.chopAtTransients (AudioPluginAudioProcessor::TransientSensitivity::Light);
                                    else if (chosen == 2)
                                        proc.chopAtTransients (AudioPluginAudioProcessor::TransientSensitivity::Medium);
                                });
        };
        barsButton.getProperties().set ("cueStyle", "flatAction");
        loadButton.getProperties().set ("cueStyle", "flatAction");

        playButton.setTooltip ("Play the selected chop from its cue point. Click a chop on the waveform first to pick which one plays.");
        pauseButton.setTooltip ("Pause playback - press Play to resume from the same spot.");
        stopButton.setTooltip ("Stop playback and return to the beginning of the current chop.");
        halfSpeedButton.setTooltip ("Half-Time: plays at half speed while preserving pitch. Active when lit.");
        chopTransientsButton.setTooltip ("Chop at transients: scans the sample for heavy hits (kicks/snares) and places chop markers at each onset. Click to choose sensitivity - Light (heavy hits only) or Medium (more onsets).");
        barsButton.setTooltip ("Sets how many bars each chop covers - cycles 1 / 2 / 4 / 8. Larger = fewer, longer chops.");
        loadButton.setTooltip ("Open a file browser to load a new audio sample (WAV, AIFF, MP3, FLAC, OGG). You can also drag a file onto the waveform.");

        for (auto* button : { &playButton, &pauseButton, &stopButton, &halfSpeedButton,
                              &chopTransientsButton, &barsButton, &loadButton })
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
        keyDisplay.setTooltip ("Shows detected musical key and Camelot code.");
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

        fillRoundedGradient (g, topPanel, panelDark.brighter (0.1f), panelDark.darker (0.18f), mediumCorner);
        g.setColour (borderDark);
        g.drawRoundedRectangle (topPanel.reduced (0.5f), mediumCorner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.1f));
        g.drawRoundedRectangle (topPanel.reduced (1.0f), mediumCorner - 1.0f, 1.0f);

        drawPanelHole (g, { topPanel.getX() + 13.0f, topPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getRight() - 13.0f, topPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getX() + 13.0f, topPanel.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { topPanel.getRight() - 13.0f, topPanel.getBottom() - 13.0f }, 6.0f);

        g.setColour (borderDark);
        g.fillRect (216, (int) std::round (topPanel.getCentreY() - 24.0f), 1, 48);

        auto bottomPanel = getBottomPanelBounds().toFloat();

        fillRoundedGradient (g, bottomPanel, panelDark.brighter (0.1f), panelDark.darker (0.18f), mediumCorner);
        g.setColour (borderDark);
        g.drawRoundedRectangle (bottomPanel.reduced (0.5f), mediumCorner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.1f));
        g.drawRoundedRectangle (bottomPanel.reduced (1.0f), mediumCorner - 1.0f, 1.0f);

        drawPanelHole (g, { bottomPanel.getX() + 13.0f, bottomPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getRight() - 13.0f, bottomPanel.getY() + 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getX() + 13.0f, bottomPanel.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { bottomPanel.getRight() - 13.0f, bottomPanel.getBottom() - 13.0f }, 6.0f);

        auto badgeBounds = juce::Rectangle<float> (bottomPanel.getX() + 20.0f,
                                                   (topPanel.getBottom() + bottomPanel.getY()) * 0.5f - 9.0f,
                                                   122.0f,
                                                   18.0f);
        fillRoundedGradient (g, badgeBounds, shellDark.brighter (0.12f), shellDark.darker (0.18f), 6.0f);
        g.setColour (borderLight);
        g.drawRoundedRectangle (badgeBounds.reduced (0.5f), 6.0f, 1.0f);

        g.setColour (juce::Colour (0xff99a1af));
        g.setFont (heavyFont (10.8f).withExtraKerningFactor (0.08f));
        g.drawText ("CHOP CONTROLS",
                    badgeBounds.toNearestInt().withY ((int) std::round (badgeBounds.getY() - 1.0f)),
                    juce::Justification::centred, false);

        g.setColour (textMuted.brighter (0.18f).withAlpha (0.62f));
        g.setFont (heavyFont (9.6f).withExtraKerningFactor (0.06f));
        g.drawText ("MIDI: C2 = chop 1,  D2 = chop 2 ...",
                    juce::Rectangle<int> (452, (int) bottomPanel.getY() + 94, 328, 13),
                    juce::Justification::centred, false);

        drawHelperText (g, "Load audio - tempo/key are detected automatically",
                        juce::Rectangle<int> (394, (int) topPanel.getBottom() - 23, 374, 16), juce::Justification::centred, 10.0f);
        drawHelperText (g, "START nudges grid timing",
                        juce::Rectangle<int> (282, (int) topPanel.getBottom() - 14, 146, 12), juce::Justification::centred, 9.0f);
        drawHelperText (g, "CUE loops from inside the selected chop",
                        juce::Rectangle<int> (18, (int) bottomPanel.getY() + 96, 156, 15), juce::Justification::centred, 9.5f);
        drawHelperText (g, "GAIN and PITCH affect only the selected chop",
                        juce::Rectangle<int> (168, (int) bottomPanel.getY() + 96, 198, 15), juce::Justification::centred, 9.5f);
        drawHelperText (g, "BARS sets chop length before rebuild",
                        juce::Rectangle<int> (452, (int) bottomPanel.getY() + 105, 328, 15), juce::Justification::centred, 9.5f);
    }

    void resized() override
    {
        const auto topPanel = getTopPanelBounds();
        const int topCenterY = topPanel.getCentreY();
        playButton.setBounds (16, topCenterY - 24, 48, 48);
        pauseButton.setBounds (80, topCenterY - 24, 48, 48);
        stopButton.setBounds (144, topCenterY - 24, 48, 48);
        halfSpeedButton.setBounds (241, topCenterY - 24, 56, 48);
        startKnob.setBounds (328, topCenterY - 34, smallKnobDiameter, 69);
        timeDisplay.setBounds (413, topCenterY - 24, 160, 48);
        tempoDisplay.setBounds (589, topCenterY - 24, 80, 48);
        keyDisplay.setBounds (677, topCenterY - 24, 80, 48);

        auto bottomPanel = getBottomPanelBounds();
        
        auto knobRow = juce::Rectangle<int> (52, bottomPanel.getY() + 18, 254, 72);
        cueKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));
        knobRow.removeFromLeft (52);
        gainKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));
        knobRow.removeFromLeft (52);
        pitchKnob.setBounds (knobRow.removeFromLeft (smallKnobDiameter));

        chopTransientsButton.setBounds ((getWidth() - 148) / 2, bottomPanel.getY() + 39, 148, 48);

        auto buttonRow = juce::Rectangle<int> (468, bottomPanel.getY() + 33, 292, 60);
        barsButton.setBounds (buttonRow.removeFromLeft (140).withTrimmedTop (6).withHeight (48));
        buttonRow.removeFromLeft (12);
        loadButton.setBounds (buttonRow.removeFromLeft (140).withTrimmedTop (6).withHeight (48));

        // Warp controls live above the load/bars row inside the bottom panel.
        const int warpRowY = bottomPanel.getY() + 8;
        warpButton.setBounds (468, warpRowY, 80, 22);
        clearWarpButton.setBounds (556, warpRowY, 80, 22);
        warpDivisionCombo.setBounds (644, warpRowY, 80, 22);
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
    juce::TextButton playButton;
    juce::TextButton pauseButton;
    juce::TextButton stopButton;
    juce::TextButton halfSpeedButton;
    juce::TextButton chopTransientsButton;
    juce::TextButton barsButton;
    juce::TextButton loadButton;
    juce::TextButton warpButton;
    juce::TextButton clearWarpButton;
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

        fillRoundedGradient (g, bounds, panelDark.brighter (0.1f), panelDark.darker (0.2f), largeCorner);
        g.setColour (borderLight);
        g.drawRoundedRectangle (bounds, largeCorner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), largeCorner - 1.0f, 1.0f);

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
    juce::TextButton syncButton;
    LabelledKnob zoomKnob;
    LabelledKnob scrollKnob;
    LabelledKnob tempoKnob;
    LabelledKnob pitchKnob;
};

class EffectModuleComponent final : public juce::Component
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

        if (switchButton.getToggleState())
        {
            drawSoftDropShadow (g, markerBounds, 0.0f, true, 2.4f, 0.0f, 4.0f);
            fillEllipseGradient (g, markerBounds.reduced (1.0f),
                                 accentOrange.brighter (0.45f), accentOrange.darker (0.18f));
            g.setColour (accentOrange);
            g.drawEllipse (markerBounds, 1.0f);
        }
        else
        {
            fillEllipseGradient (g, markerBounds, panelDark.brighter (0.18f), panelDark.darker (0.2f));
            g.setColour (textPrimary);
            g.drawEllipse (markerBounds, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.drawEllipse (markerBounds.reduced (0.5f), 1.0f);
        }

        g.setColour (textPrimary);
        g.setFont (titleFont);
        auto titleBounds = juce::Rectangle<int> ((int) std::round (rowX + markerSize + titleGap),
                                                 titleArea.getY(),
                                                 (int) std::ceil (titleWidth),
                                                 titleArea.getHeight());
        g.drawText (title, titleBounds, juce::Justification::centredLeft, false);

        auto switchBounds = switchButton.getBounds();
        auto offLabelBounds = juce::Rectangle<int> (switchBounds.getX() - 24, switchBounds.getY() + 5, 16, 10);
        auto onLabelBounds = juce::Rectangle<int> (switchBounds.getRight() + 8, switchBounds.getY() + 5, 12, 10);

        g.setColour (textMuted);
        g.setFont (heavyFont (7.0f));
        g.drawText ("OFF", offLabelBounds, juce::Justification::centredLeft, false);
        g.setColour (borderLight.darker (0.4f));
        g.drawText ("ON", onLabelBounds, juce::Justification::centredLeft, false);

        if (gainReductionReadoutVisible)
        {
            auto readoutBounds = getGainReductionReadoutBounds().toFloat();
            fillRoundedGradient (g, readoutBounds, blackPanel.brighter (0.06f), blackPanel, 4.0f);
            g.setColour (borderDark);
            g.drawRoundedRectangle (readoutBounds.reduced (0.5f), 4.0f, 1.0f);

            g.setColour (textMuted);
            g.setFont (heavyFont (7.0f));
            g.drawText ("GR", readoutBounds.removeFromLeft (34.0f).toNearestInt(),
                        juce::Justification::centred, false);

            g.setColour (gainReductionDb > 0.05f ? accentOrange.brighter (0.2f) : textPrimary.withAlpha (0.55f));
            g.setFont (monoFont (10.5f));
            g.drawText (juce::String (gainReductionDb, 1) + " dB",
                        readoutBounds.toNearestInt().reduced (4, 0),
                        juce::Justification::centredRight, false);
        }
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
            repaint();
        }
    }

    void setGainReductionDb (float dB) noexcept
    {
        const auto clamped = juce::jlimit (0.0f, 99.9f, dB);
        if (std::abs (clamped - gainReductionDb) > 0.05f)
        {
            gainReductionDb = clamped;
            repaint (getGainReductionReadoutBounds());
        }
    }

private:
    juce::Rectangle<int> getGainReductionReadoutBounds() const noexcept
    {
        return { 30, getHeight() - 24, getWidth() - 60, 18 };
    }

    juce::String title;
    juce::TextButton switchButton;
    LabelledKnob firstKnob;
    LabelledKnob secondKnob;
    juce::Rectangle<int> firstKnobBounds;
    juce::Rectangle<int> secondKnobBounds;
    bool gainReductionReadoutVisible = false;
    float gainReductionDb = 0.0f;
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

        fillRoundedGradient (g, bounds, panelDark.brighter (0.1f), panelDark.darker (0.2f), mediumCorner);
        g.setColour (borderDark);
        g.drawRoundedRectangle (bounds.reduced (0.5f), mediumCorner, 1.0f);

        drawPanelHole (g, { 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, 13.0f }, 6.0f);
        drawPanelHole (g, { 13.0f, bounds.getBottom() - 13.0f }, 6.0f);
        drawPanelHole (g, { bounds.getRight() - 13.0f, bounds.getBottom() - 13.0f }, 6.0f);

        auto innerBounds = bounds.reduced (16.0f);
        fillRoundedGradient (g, innerBounds, juce::Colour (0xff242424), juce::Colour (0xff101010), smallCorner);
        g.setColour (borderMid);
        g.drawRoundedRectangle (innerBounds.reduced (0.5f), smallCorner, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (innerBounds.reduced (1.0f), smallCorner - 1.0f, 2.0f);

        fillRectGradient (g, juce::Rectangle<float> (33.0f, 314.0f, 212.0f, 3.0f),
                          juce::Colour (0xff3a3a3a), juce::Colour (0xff171717));

        g.setColour (juce::Colours::white.withAlpha (0.1f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), mediumCorner - 1.0f, 1.0f);

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
} // namespace cue

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    processorRef.sampleChangeBroadcaster.addChangeListener (this);
    processorRef.editChangeBroadcaster.addChangeListener (this);

    lookAndFeel = std::make_unique<cue::CueSamplerLookAndFeel>();
    setLookAndFeel (lookAndFeel.get());
    addAndMakeVisible (contentComponent);

    headerComponent = std::make_unique<cue::HeaderComponent> (processorRef);
    waveformDisplayComponent = std::make_unique<cue::WaveformDisplayComponent> (processorRef);
    transportSectionComponent = std::make_unique<cue::TransportSectionComponent> (processorRef);
    utilityStripComponent = std::make_unique<cue::UtilityStripComponent>();
    effectsRackComponent = std::make_unique<cue::EffectsRackComponent>();

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
                                    utilityStripComponent.get(),
                                    effectsRackComponent.get() };

    for (auto* component : sections)
        contentComponent.addAndMakeVisible (*component);

    cue::configureButton (saveCorrectionButton, "Save Correction", cue::textPrimary);
    saveCorrectionButton.getProperties().set ("cueStyle", "helpButton");
    saveCorrectionButton.setTooltip ("Save a corrected BPM for the current sample.");
    saveCorrectionButton.onClick = [this] { showCorrectionDialog(); };
    contentComponent.addAndMakeVisible (saveCorrectionButton);

    panelShadowEffect.setShadowProperties (defaultShadow);
    waveformDisplayComponent->setComponentEffect (&panelShadowEffect);
    transportSectionComponent->setComponentEffect (&panelShadowEffect);
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
    if (! cue::shouldRunRealtimeUi (*this))
        return;

    if (effectsRackComponent == nullptr) return;

    const auto grDb = processorRef.getCompressorGainReductionDb();
    effectsRackComponent->getCompressorModule().setGainReductionDb (grDb);
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

void AudioPluginAudioProcessorEditor::showCorrectionDialog()
{
    const auto sampleData = processorRef.getLoadedSample();
    const auto analysis = processorRef.getTempoAnalysis();

    if (sampleData == nullptr || analysis == nullptr || analysis->estimatedBpm <= 0.0
        || sampleData->buffer.getNumSamples() <= 0 || sampleData->sampleRate <= 0.0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                "No Sample",
                                                "Load a sample first.",
                                                "OK",
                                                this);
        return;
    }

    const auto detectedBpm = analysis->estimatedBpm;
    auto* alert = new juce::AlertWindow ("Save Tempo Correction",
                                         "Detected BPM: " + juce::String (detectedBpm, 2)
                                             + "\n\nEnter the correct BPM:",
                                         juce::AlertWindow::NoIcon,
                                         this);

    alert->addTextEditor ("correctedBpm", juce::String (detectedBpm, 2), {});
    alert->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeThis (this);
    juce::Component::SafePointer<juce::AlertWindow> safeAlert (alert);

    alert->enterModalState (true,
                            juce::ModalCallbackFunction::create (
                                [safeThis, safeAlert, sampleData, detectedBpm] (int result)
                                {
                                    if (result != 1 || safeThis == nullptr || safeAlert == nullptr)
                                        return;

                                    const auto correctedBpm = safeAlert->getTextEditorContents ("correctedBpm").getDoubleValue();
                                    if (correctedBpm <= 0.0)
                                    {
                                        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                                                "Invalid BPM",
                                                                                "Enter a positive BPM value.",
                                                                                "OK",
                                                                                safeThis);
                                        return;
                                    }

                                    auto sourceFile = sampleData->sourceFile;
                                    if (sourceFile.getFullPathName().isEmpty() && sampleData->filePath.isNotEmpty())
                                        sourceFile = juce::File (sampleData->filePath);

                                    saveTempoCorrection (sourceFile,
                                                         sampleData->buffer,
                                                         detectedBpm,
                                                         correctedBpm,
                                                         sampleData->sampleRate);

                                    juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                                            "Saved",
                                                                            "Correction saved. Total saved: "
                                                                                + juce::String (countSavedCorrections()),
                                                                            "OK",
                                                                            safeThis);
                                }),
                            true);
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
    });
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ColourGradient background (juce::Colour (0xff242424), 0.0f, 0.0f,
                                     juce::Colour (0xff111111), 0.0f, (float) getHeight(), false);
    g.setGradientFill (background);
    g.fillAll();

    juce::Graphics::ScopedSaveState scaleState (g);
    g.addTransform (juce::AffineTransform::scale (getUiScale()));

    auto faceplate = juce::Rectangle<float> (0.0f, 0.0f, (float) cue::editorWidth, (float) cue::editorHeight);
    cue::fillRoundedGradient (g, faceplate, cue::shellDark.brighter (0.1f),
                              cue::shellDark.darker (0.22f), cue::largeCorner);

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

    g.setColour (accentOrange);
    g.setFont (cue::heavyFont (12.0f));
    g.drawText ("CHOP STATION", juce::Rectangle<int> (96, 116, 150, 16), juce::Justification::centredLeft, false);
}

void AudioPluginAudioProcessorEditor::resized()
{
    const auto scale = getUiScale();
    contentComponent.setTransform (juce::AffineTransform::scale (scale));
    contentComponent.setBounds (0, 0, cue::editorWidth, cue::editorHeight);

    headerComponent->setBounds (96, 32, 1246, 77);
    saveCorrectionButton.setBounds (1140, 42, 150, 28);
    waveformDisplayComponent->setBounds (96, 133, 782, 411);
    transportSectionComponent->setBounds (96, 552, 782, 236);
    utilityStripComponent->setBounds (910, 133, 120, 655);
    effectsRackComponent->setBounds (1062, 133, 278, 655);
    helpOverlayComponent->setBounds (96, 133, 782, 655);
}

void AudioPluginAudioProcessorEditor::paintSideRail (juce::Graphics& g,
                                                     juce::Rectangle<int> bounds,
                                                     bool isLeftRail) const
{
    auto railArea = bounds.toFloat();
    auto railPath = cue::createRailPath (railArea, isLeftRail, cue::largeCorner);

    cue::fillPathGradient (g, railPath, railArea, cue::railDark.brighter (0.12f),
                           cue::railDark.darker (0.25f));

    g.setColour (cue::borderDark);
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
    paintScrew (g, { railOriginX + 31.5f, railOriginY + 748.0f });
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
