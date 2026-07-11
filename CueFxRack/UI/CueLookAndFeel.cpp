#include "CueLookAndFeel.h"

namespace cue
{

juce::Font monoFont (float height, bool bold)
{
    const auto style = bold ? juce::Font::bold : juce::Font::plain;
   #if JUCE_MAJOR_VERSION >= 8
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, style));
   #else
    return juce::Font (juce::Font::getDefaultMonospacedFontName(), height, style);
   #endif
}

CueLookAndFeel::CueLookAndFeel()
{
    refreshColours();
}

void CueLookAndFeel::refreshColours()
{
    setColour (juce::Slider::thumbColourId, colours::cream);   // knob/cap body; panels tint per-module
    setColour (juce::Slider::textBoxTextColourId, colours::cream);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, colours::creamDim);
    setColour (juce::BubbleComponent::backgroundColourId, colours::slot);
    setColour (juce::BubbleComponent::outlineColourId, colours::line);
    setColour (juce::TooltipWindow::textColourId, colours::cream);
}

//==============================================================================
void CueLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float pos, float startAngle, float endAngle, juce::Slider& s)
{
    if (width <= 0 || height <= 0)
        return;

    const auto bounds  = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre  = bounds.getCentre();
    const auto radius  = juce::jmax (0.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f);
    const auto enabled = s.isEnabled();

    const auto body     = s.findColour (juce::Slider::thumbColourId);
    const auto cream    = enabled ? body : body.withAlpha (0.35f);
    const auto creamDim = enabled ? colours::creamDim : colours::creamDim.withAlpha (0.35f);
    const auto angle    = startAngle + pos * (endAngle - startAngle);

    // Tick ring
    g.setColour (creamDim);
    const int numTicks = 11;
    for (int i = 0; i < numTicks; ++i)
    {
        const auto a  = startAngle + (float) i / (float) (numTicks - 1) * (endAngle - startAngle);
        const auto p1 = centre.getPointOnCircumference (radius - 3.0f, a);
        const auto p2 = centre.getPointOnCircumference (radius, a);
        g.drawLine ({ p1, p2 }, 1.0f);
    }

    // Knob body
    const auto knobR = juce::jmax (0.0f, radius - 6.5f);
    g.setColour (cream);
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    // Subtle rim shading
    g.setColour (colours::dark.withAlpha (0.25f));
    g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.2f);

    // Pointer
    const auto pIn  = centre.getPointOnCircumference (knobR * 0.25f, angle);
    const auto pOut = centre.getPointOnCircumference (knobR * 0.85f, angle);
    g.setColour (colours::dark.withAlpha (enabled ? 0.9f : 0.5f));
    g.drawLine ({ pIn, pOut }, juce::jmax (2.0f, knobR * 0.09f));
}

//==============================================================================
void CueLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float, float,
                                       juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (width <= 0 || height <= 0)
        return;
    const auto enabled  = s.isEnabled();
    const auto body     = s.findColour (juce::Slider::thumbColourId);
    const auto cream    = enabled ? body : body.withAlpha (0.35f);
    const auto creamDim = enabled ? colours::creamDim : colours::creamDim.withAlpha (0.35f);

    if (style == juce::Slider::LinearVertical)
    {
        const auto cx = (float) x + (float) width * 0.5f;

        // Side tick marks
        g.setColour (creamDim.withAlpha (0.6f));
        const int nTicks = 7;
        for (int i = 0; i < nTicks; ++i)
        {
            const auto ty = (float) y + (float) i / (float) (nTicks - 1) * (float) (height - 1);
            g.drawLine (cx - 13.0f, ty, cx - 9.0f, ty, 1.0f);
            g.drawLine (cx + 9.0f,  ty, cx + 13.0f, ty, 1.0f);
        }

        // Slot
        g.setColour (colours::slot);
        g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) height, 2.0f);
        g.setColour (colours::line);
        g.drawRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) height, 2.0f, 1.0f);

        // Cap
        const auto capW = 20.0f, capH = 13.0f;
        juce::Rectangle<float> cap (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        g.setColour (colours::dark.withAlpha (0.6f));
        g.fillRoundedRectangle (cap.translated (0.0f, 1.5f), 2.0f);
        g.setColour (cream);
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (colours::dark);
        g.drawLine (cap.getX() + 2.0f, cap.getCentreY(), cap.getRight() - 2.0f, cap.getCentreY(), 1.4f);
    }
    else // horizontal
    {
        const auto cy = (float) y + (float) height * 0.5f;

        g.setColour (colours::slot);
        g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) width, 4.0f, 2.0f);
        g.setColour (colours::line);
        g.drawRoundedRectangle ((float) x, cy - 2.0f, (float) width, 4.0f, 2.0f, 1.0f);

        const auto capW = 13.0f, capH = 20.0f;
        juce::Rectangle<float> cap (sliderPos - capW * 0.5f, cy - capH * 0.5f, capW, capH);
        g.setColour (cream);
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (colours::dark);
        g.drawLine (cap.getCentreX(), cap.getY() + 2.0f, cap.getCentreX(), cap.getBottom() - 2.0f, 1.4f);
    }
}

//==============================================================================
void CueLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                       bool highlighted, bool)
{
    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        return;
    const auto enabled = b.isEnabled();
    const auto on      = b.getToggleState();
    auto box = juce::Rectangle<float> (0.0f, ((float) b.getHeight() - 13.0f) * 0.5f, 13.0f, 13.0f);

    auto cream    = enabled ? colours::cream    : colours::cream.withAlpha (0.35f);
    auto creamDim = enabled ? colours::creamDim : colours::creamDim.withAlpha (0.35f);

    if (on)
    {
        g.setColour (enabled ? colours::chip : colours::chip.withAlpha (0.35f));
        g.fillRoundedRectangle (box, 2.5f);
        g.setColour (colours::dark);
        g.fillEllipse (box.reduced (4.2f));
    }
    else
    {
        g.setColour (highlighted ? cream : creamDim);
        g.drawRoundedRectangle (box.reduced (0.6f), 2.5f, 1.2f);
    }

    if (b.getButtonText().isNotEmpty())
    {
        g.setColour (on ? cream : creamDim);
        g.setFont (monoFont (10.5f));
        g.drawText (b.getButtonText().toUpperCase(),
                    b.getLocalBounds().withTrimmedLeft (19),
                    juce::Justification::centredLeft, false);
    }
}

//==============================================================================
juce::Font CueLookAndFeel::getSliderPopupFont (juce::Slider&)         { return monoFont (12.5f); }
int CueLookAndFeel::getSliderPopupPlacement (juce::Slider&)           { return juce::BubbleComponent::above; }

void CueLookAndFeel::drawBubble (juce::Graphics& g, juce::BubbleComponent& bubble,
                                 const juce::Point<float>&, const juce::Rectangle<float>& body)
{
    if (body.getWidth() <= 0.0f || body.getHeight() <= 0.0f)
        return;
    g.setColour (bubble.findColour (juce::BubbleComponent::backgroundColourId));
    g.fillRoundedRectangle (body, 3.0f);
    g.setColour (bubble.findColour (juce::BubbleComponent::outlineColourId));
    g.drawRoundedRectangle (body, 3.0f, 1.0f);
}

//==============================================================================
void CueLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                           const juce::Colour&, bool highlighted, bool down)
{
    if (b.getWidth() <= 1 || b.getHeight() <= 1)
        return;
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? colours::panelHi : colours::panel);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (highlighted ? colours::cream : colours::creamDim);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
}

juce::Font CueLookAndFeel::getTextButtonFont (juce::TextButton&, int)   { return monoFont (10.5f); }

void CueLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b,
                                     bool highlighted, bool)
{
    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        return;
    g.setColour (highlighted ? colours::cream : colours::creamDim);
    g.setFont (monoFont (10.5f));
    g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(),
                juce::Justification::centred, false);
}

} // namespace cue
