#include "ModulePanel.h"

namespace cue
{

//==============================================================================
void PowerButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    if (getWidth() <= 4 || getHeight() <= 4)
        return;

    const auto b  = getLocalBounds().toFloat();
    const auto c  = b.getCentre();
    const auto r  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 2.0f;
    const auto on = getToggleState();

    if (on)
    {
        g.setColour (onColour);
        g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
    }

    const auto fg = on ? colours::dark
                       : (highlighted ? colours::cream : colours::creamDim);

    // Power glyph: open arc + line through the gap
    juce::Path arc;
    const auto ar = r * 0.52f;
    arc.addCentredArc (c.x, c.y + 0.5f, ar, ar, 0.0f, 0.7f,
                       juce::MathConstants<float>::twoPi - 0.7f, true);
    g.setColour (fg);
    g.strokePath (arc, juce::PathStrokeType (1.6f));
    g.drawLine (c.x, c.y - r * 0.75f, c.x, c.y - r * 0.05f, 1.6f);

    if (! on)
    {
        g.setColour (fg.withAlpha (0.7f));
        g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);
    }
}

//==============================================================================
void RemoveButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    if (getWidth() <= 4 || getHeight() <= 4)
        return;

    const auto b = getLocalBounds().toFloat();
    const auto c = b.getCentre();
    const auto r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 2.0f;

    const auto fg = highlighted ? colours::cueEmber : colours::creamDim.withAlpha (0.6f);
    g.setColour (fg);
    g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);

    const auto k = r * 0.42f;
    g.drawLine (c.x - k, c.y - k, c.x + k, c.y + k, 1.4f);
    g.drawLine (c.x - k, c.y + k, c.x + k, c.y - k, 1.4f);
}

//==============================================================================
ModulePanel::ModulePanel (juce::AudioProcessorValueTreeState& state,
                          juce::String titleText, const char* powerParamID,
                          juce::Colour accentColour)
    : apvts (state), title (std::move (titleText)), accent (accentColour)
{
    power.onColour = accent;

    if (powerParamID != nullptr)
    {
        addAndMakeVisible (power);
        power.onStateChange = [this] { repaint(); };     // dim/undim with the switch
        powerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                              apvts, powerParamID, power);
    }

    addAndMakeVisible (removeButton);
    removeButton.onClick = [this]
    {
        if (onRemoveRequested != nullptr)
            onRemoveRequested();
    };
}

//==============================================================================
void ModulePanel::refreshColours()
{
    // Foundation colours moved with the theme; re-apply everything this panel's
    // widgets cached at construction time. Accents are theme-invariant.
    for (auto& [label, bright] : themedLabels)
        if (label != nullptr)
            label->setColour (juce::Label::textColourId,
                              bright ? colours::cream : colours::creamDim);

    power.onColour = accent;
    for (auto& s : sliders)
        s->setColour (juce::Slider::thumbColourId, accent);

    repaint();
}

//==============================================================================
// Header strip = drag handle for reordering the rack
void ModulePanel::mouseDown (const juce::MouseEvent& e)
{
    if (onDragStarted != nullptr)
    {
        draggingHeader = true;
        dragGrabOffset = e.getPosition();
        onDragStarted();
    }
}

void ModulePanel::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingHeader && onDragMoved != nullptr)
        onDragMoved (getPosition() + e.getPosition() - dragGrabOffset);
}

void ModulePanel::mouseUp (const juce::MouseEvent&)
{
    if (draggingHeader)
    {
        draggingHeader = false;
        if (onDragFinished != nullptr)
            onDragFinished();
    }
}

juce::Rectangle<int> ModulePanel::contentArea() const
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (32);
    if (tagline.isNotEmpty())
        r.removeFromBottom (16);
    return r;
}

void ModulePanel::resized()
{
    removeButton.setBounds (getWidth() - 32, 9, 20, 20);
    power.setBounds (getWidth() - 56, 9, 20, 20);
    layoutContent (contentArea());
}

void ModulePanel::parentHierarchyChanged()
{
    // Value popups must live INSIDE the editor. With a null parent JUCE puts
    // the bubble in its own native OS window, which is created on every drag
    // start — a visible hitch per knob grab on macOS.
    if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
        for (auto& s : sliders)
            s->setPopupDisplayEnabled (true, false, top);
}

void ModulePanel::paint (juce::Graphics& g)
{
    if (getWidth() < 4 || getHeight() < 4)
        return;

    auto b = getLocalBounds().toFloat();

    // Plate takes a barely-there cast of the module's hue
    g.setColour (colours::panel.interpolatedWith (accent, 0.07f));
    g.fillRoundedRectangle (b.reduced (0.5f), 2.0f);
    g.setColour (colours::line.interpolatedWith (accent, 0.12f));
    g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);

    drawScrew (g, 9.0f, 9.0f);
    drawScrew (g, b.getWidth() - 9.0f, 9.0f);
    drawScrew (g, 9.0f, b.getHeight() - 9.0f);
    drawScrew (g, b.getWidth() - 9.0f, b.getHeight() - 9.0f);

    g.setColour (themedTitleColour (accent));
    g.setFont (monoFont (20.0f, true));
    g.drawText (title.toUpperCase(), 22, 6, juce::jmax (0, getWidth() - 88), 26,
                juce::Justification::centredLeft, false);

    if (tagline.isNotEmpty())
    {
        g.setColour (colours::creamDim.withAlpha (0.75f));
        g.setFont (monoFont (8.5f));
        g.drawFittedText (tagline.toUpperCase(), 12, getHeight() - 22,
                          juce::jmax (0, getWidth() - 24), 14, juce::Justification::centredLeft, 2);
    }

    paintContent (g, contentArea());
}

void ModulePanel::paintOverChildren (juce::Graphics& g)
{
    if (getWidth() < 4 || getHeight() < 4)
        return;

    // Module off: dim everything below the header. The header (title, power,
    // remove) stays bright so re-enabling is obvious.
    if (! isPowered() && getHeight() > 40)
    {
        g.setColour (colours::panel.withAlpha (0.62f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f).withTrimmedTop (34.0f), 2.0f);
    }
}

//==============================================================================
juce::Slider& ModulePanel::makeSlider (const char* paramID, const juce::String& labelText,
                                       juce::Slider::SliderStyle style)
{
    auto slider = std::make_unique<juce::Slider> (style, juce::Slider::NoTextBox);
    slider->setPopupDisplayEnabled (true, false, nullptr);
    slider->setMouseDragSensitivity (140);
    slider->setColour (juce::Slider::thumbColourId, accent);
    addAndMakeVisible (*slider);

    auto label = std::make_unique<juce::Label> (juce::String(), labelText.toUpperCase());
    label->setFont (monoFont (11.0f));
    label->setColour (juce::Label::textColourId, colours::creamDim);
    label->setJustificationType (juce::Justification::centred);
    label->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*label);

    sliderAttachments.push_back (
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            apvts, paramID, *slider));

    if (auto* p = apvts.getParameter (paramID))
        slider->setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));

    labelFor[slider.get()] = label.get();
    themedLabels.push_back ({ label.get(), false });   // dim, follows theme
    ownedLabels.push_back (std::move (label));
    sliders.push_back (std::move (slider));
    return *sliders.back();
}

juce::Slider& ModulePanel::addKnob (const char* paramID, const juce::String& labelText,
                                    const juce::String& suffix, int decimals)
{
    auto& s = makeSlider (paramID, labelText, juce::Slider::RotaryHorizontalVerticalDrag);
    s.textFromValueFunction = [suffix, decimals] (double v)
    {
        return juce::String (v, decimals) + suffix;
    };
    return s;
}

juce::Slider& ModulePanel::addChoiceKnob (const char* paramID, const juce::String& labelText,
                                          const juce::StringArray& names, const juce::String& suffix)
{
    auto& s = makeSlider (paramID, labelText, juce::Slider::RotaryHorizontalVerticalDrag);
    s.textFromValueFunction = [names, suffix] (double v)
    {
        const auto i = juce::jlimit (0, names.size() - 1, (int) std::lround (v));
        return names[i] + suffix;
    };
    return s;
}

juce::Slider& ModulePanel::addFader (const char* paramID, const juce::String& labelText,
                                     const juce::String& suffix)
{
    auto& s = makeSlider (paramID, labelText, juce::Slider::LinearVertical);
    s.textFromValueFunction = [suffix] (double v)
    {
        return juce::String (v, 1) + suffix;
    };
    return s;
}

juce::Slider& ModulePanel::addChoiceFader (const char* paramID, const juce::String& labelText,
                                           const juce::StringArray& names, const juce::String& suffix)
{
    auto& s = makeSlider (paramID, labelText, juce::Slider::LinearVertical);
    s.textFromValueFunction = [names, suffix] (double v)
    {
        const auto i = juce::jlimit (0, names.size() - 1, (int) std::lround (v));
        return names[i] + suffix;
    };
    return s;
}

juce::ToggleButton& ModulePanel::addToggle (const char* paramID, const juce::String& labelText)
{
    auto toggle = std::make_unique<juce::ToggleButton> (labelText);
    addAndMakeVisible (*toggle);
    buttonAttachments.push_back (
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            apvts, paramID, *toggle));
    toggles.push_back (std::move (toggle));
    return *toggles.back();
}

juce::Label& ModulePanel::addStatic (const juce::String& text, float fontHeight, bool bright)
{
    auto label = std::make_unique<juce::Label> (juce::String(), text.toUpperCase());
    label->setFont (monoFont (fontHeight));
    label->setColour (juce::Label::textColourId, bright ? colours::cream : colours::creamDim);
    label->setJustificationType (juce::Justification::centred);
    label->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*label);
    themedLabels.push_back ({ label.get(), bright });
    ownedLabels.push_back (std::move (label));
    return *ownedLabels.back();
}

//==============================================================================
void ModulePanel::placeKnob (juce::Slider& s, juce::Rectangle<int> cell, int knobSize)
{
    // Knob + label move as one unit, label hugging the knob's bottom edge.
    constexpr int labelH = 15, gap = 1;
    const int k = juce::jmin (knobSize, juce::jmax (24, cell.getHeight() - labelH - gap));

    auto unit = cell.withSizeKeepingCentre (cell.getWidth(), k + gap + labelH);
    s.setBounds (unit.removeFromTop (k).withSizeKeepingCentre (k, k));
    unit.removeFromTop (gap);
    if (auto* l = labelFor[&s])
        l->setBounds (unit);
}

void ModulePanel::placeFader (juce::Slider& s, juce::Rectangle<int> cell)
{
    auto labelArea = cell.removeFromBottom (15);
    s.setBounds (cell.reduced (0, 1));
    if (auto* l = labelFor[&s])
        l->setBounds (labelArea);
}

void ModulePanel::placeFaderH (juce::Slider& s, juce::Rectangle<int> cell, int labelWidth)
{
    auto labelArea = cell.removeFromLeft (labelWidth);
    s.setBounds (cell.reduced (2, 0));
    if (auto* l = labelFor[&s])
    {
        l->setBounds (labelArea);
        l->setJustificationType (juce::Justification::centredLeft);
    }
}

//==============================================================================
void ModulePanel::drawDotGrid (juce::Graphics& g, juce::Rectangle<float> area,
                               int cols, int rows, float dotRadius, juce::Colour colour)
{
    g.setColour (colour);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
        {
            const auto x = area.getX() + (cols <= 1 ? 0.5f : (float) c / (float) (cols - 1)) * area.getWidth();
            const auto y = area.getY() + (rows <= 1 ? 0.5f : (float) r / (float) (rows - 1)) * area.getHeight();
            g.fillEllipse (x - dotRadius, y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
        }
}

void ModulePanel::drawJackRow (juce::Graphics& g, juce::Rectangle<float> area,
                               int count, const juce::String& caption)
{
    if (caption.isNotEmpty())
    {
        g.setColour (colours::creamDim);
        g.setFont (monoFont (8.5f));
        g.drawText ("[ " + caption.toUpperCase() + " ]", area.removeFromTop (10.0f),
                    juce::Justification::centred, false);
        area.removeFromTop (2.0f);
    }

    const float r = 5.0f;
    const auto cy = area.getCentreY();
    for (int i = 0; i < count; ++i)
    {
        const auto cx = area.getX() + ((float) i + 0.5f) / (float) count * area.getWidth();
        g.setColour (colours::slot);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.setColour (colours::cream);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.3f);
    }
}

void ModulePanel::drawScrew (juce::Graphics& g, float cx, float cy)
{
    const float r = 3.2f;
    g.setColour (colours::creamDim.withAlpha (0.65f));
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);
    g.drawLine (cx - r * 0.6f, cy - r * 0.6f, cx + r * 0.6f, cy + r * 0.6f, 1.0f);
}

} // namespace cue
