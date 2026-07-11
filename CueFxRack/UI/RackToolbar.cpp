#include "RackToolbar.h"

namespace cue
{

//==============================================================================
// Chips
//==============================================================================
class RackToolbar::ModuleChip final : public juce::Component
{
public:
    ModuleChip (RackToolbar& t, juce::String moduleId, juce::Colour c)
        : toolbar (t), id (std::move (moduleId)), colour (c)
    {
        setRepaintsOnMouseActivity (true);
    }

    void paint (juce::Graphics& g) override
    {
        if (getWidth() <= 2 || getHeight() <= 2)
            return;

        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (isMouseOver() ? colours::panelHi : colours::panel);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (isMouseOver() ? colour : colours::line);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        drawModuleIcon (g, id, { r.getX(), r.getY() + 2.0f, r.getWidth(), 22.0f },
                        toolbar.phase, colour);

        g.setColour (isMouseOver() ? colour : colours::creamDim);
        g.setFont (monoFont (8.5f));
        g.drawText (id, getLocalBounds().removeFromBottom (13),
                    juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && toolbar.onRestorePanel != nullptr)
            toolbar.onRestorePanel (id);
    }

    RackToolbar& toolbar;
    juce::String id;
    juce::Colour colour;
};

//==============================================================================
// Toolbar
//==============================================================================
RackToolbar::RackToolbar()
{
    startTimerHz (24);
}

RackToolbar::~RackToolbar() = default;

void RackToolbar::timerCallback()
{
    phase += 1.0f / 24.0f;

    for (auto* chip : moduleChips)
        chip->repaint();
}

void RackToolbar::setUnusedPanels (const std::vector<ModuleEntry>& entries)
{
    moduleChips.clear();
    for (const auto& e : entries)
        addAndMakeVisible (moduleChips.add (new ModuleChip (*this, e.id, e.colour)));
    resized();
    repaint();
}

void RackToolbar::resized()
{
    auto r = getLocalBounds().reduced (8, 5);

    // unused modules from the left (after the section label)
    r.removeFromLeft (58);
    for (auto* chip : moduleChips)
    {
        chip->setBounds (r.removeFromLeft (74));
        r.removeFromLeft (4);
    }
}

void RackToolbar::paint (juce::Graphics& g)
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    g.fillAll (colours::bg.darker (0.25f));
    g.setColour (colours::line);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f, 1.0f);

    g.setColour (colours::creamDim.withAlpha (0.8f));
    g.setFont (monoFont (8.5f));
    g.drawFittedText ("MOD-\nULES", 10, 8, 44, juce::jmax (0, getHeight() - 16),
                      juce::Justification::centredLeft, 2);

    if (moduleChips.isEmpty())
    {
        g.setColour (colours::creamDim.withAlpha (0.4f));
        g.setFont (monoFont (9.5f));
        g.drawText ("ALL MODULES RACKED", 58, 0, 220, getHeight(),
                    juce::Justification::centredLeft, false);
    }
}

//==============================================================================
// Animated chip icons: a tiny living glyph per module
//==============================================================================
void RackToolbar::drawModuleIcon (juce::Graphics& g, const juce::String& id,
                                  juce::Rectangle<float> a, float t, juce::Colour c)
{
    const auto cx = a.getCentreX(), cy = a.getCentreY();
    g.setColour (c);

    if (id == "EQ")
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto h = 5.0f + 5.5f * (0.5f + 0.5f * std::sin (t * 2.1f + (float) i * 1.4f));
            const auto x = cx - 12.0f + (float) i * 8.0f;
            g.fillRect (x, cy + 7.0f - h, 3.0f, h);
        }
    }
    else if (id == "COMP")
    {
        const auto gap = 5.0f + 3.5f * std::sin (t * 2.4f);
        g.drawLine (cx - 10.0f, cy - gap, cx + 10.0f, cy - gap, 1.6f);
        g.drawLine (cx - 10.0f, cy + gap, cx + 10.0f, cy + gap, 1.6f);
        g.drawLine (cx, cy - gap + 2.0f, cx, cy + gap - 2.0f, 1.0f);
    }
    else if (id == "LIM")
    {
        juce::Path p;
        for (int i = 0; i <= 20; ++i)
        {
            const auto x = cx - 11.0f + (float) i * 1.1f;
            const auto v = juce::jmin (4.5f, 8.0f * std::abs (std::sin ((float) i * 0.55f + t * 3.0f)));
            i == 0 ? p.startNewSubPath (x, cy + 3.0f - v) : p.lineTo (x, cy + 3.0f - v);
        }
        g.strokePath (p, juce::PathStrokeType (1.3f));
        g.drawLine (cx - 12.0f, cy - 2.0f, cx + 12.0f, cy - 2.0f, 1.0f);
    }
    else if (id == "CUE")
    {
        const auto r = 7.0f + 1.5f * std::sin (t * 1.8f);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.4f);
        const auto p = juce::Point<float> (cx, cy).getPointOnCircumference (r, t * 1.3f);
        g.fillEllipse (p.x - 1.6f, p.y - 1.6f, 3.2f, 3.2f);
    }
    else if (id == "MASTER")
    {
        for (int i = 0; i < 3; ++i)
        {
            const auto h = 5.0f + 6.0f * (0.5f + 0.5f * std::sin (t * 3.2f + (float) i * 2.1f));
            g.fillRect (cx - 8.0f + (float) i * 6.0f, cy + 7.0f - h, 4.0f, h);
        }
    }
    else if (id == "DLY")
    {
        for (auto sx : { cx - 7.0f, cx + 7.0f })
        {
            g.drawEllipse (sx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 1.2f);
            for (int i = 0; i < 3; ++i)
            {
                const auto ang = t * 2.6f * (sx < cx ? 1.0f : -1.0f)
                               + (float) i * (juce::MathConstants<float>::twoPi / 3.0f);
                const auto p = juce::Point<float> (sx, cy).getPointOnCircumference (4.0f, ang);
                g.drawLine (sx, cy, p.x, p.y, 1.2f);
            }
        }
    }
    else if (id == "VERB")
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto ring = std::fmod (t * 0.45f + (float) k / 3.0f, 1.0f);
            const auto r = 2.0f + ring * 9.0f;
            g.setColour (c.withAlpha ((1.0f - ring) * 0.9f));
            g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
        }
    }
    else if (id == "CRUSH")
    {
        for (int i = 0; i < 9; ++i)
        {
            const auto on = std::sin (t * 3.0f + (float) (i * 37 % 9)) > 0.1f;
            const auto x = cx - 9.0f + (float) (i % 3) * 7.0f;
            const auto y = cy - 9.0f + (float) (i / 3) * 7.0f;
            if (on) g.fillRect (x, y, 5.0f, 5.0f);
            else    g.drawRect (juce::Rectangle<float> (x, y, 5.0f, 5.0f), 0.8f);
        }
    }
    else if (id == "IMG")
    {
        const auto w = 7.0f + 5.0f * (0.5f + 0.5f * std::sin (t * 1.7f));
        g.drawEllipse (cx - w, cy - 6.0f, w * 2.0f, 12.0f, 1.2f);
        g.fillEllipse (cx - w - 1.5f, cy - 1.5f, 3.0f, 3.0f);
        g.fillEllipse (cx + w - 1.5f, cy - 1.5f, 3.0f, 3.0f);
    }
    else if (id == "HALF")
    {
        // short wave on top, stretched wave at half scroll speed below
        for (int wv = 0; wv < 2; ++wv)
        {
            juce::Path p;
            const auto stretch = wv == 0 ? 1.0f : 2.0f;
            for (int i = 0; i <= 22; ++i)
            {
                const auto x = cx - 11.0f + (float) i;
                const auto y = cy + ((float) wv - 0.5f) * 8.0f
                             + 3.0f * std::sin ((float) i * 0.85f / stretch + t * 3.0f / stretch);
                i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
            }
            g.setColour (c.withAlpha (wv == 0 ? 0.65f : 1.0f));
            g.strokePath (p, juce::PathStrokeType (1.2f));
        }
    }
    else if (id == "AMP")
    {
        // glowing tube with flicker
        const auto glow = 0.55f + 0.35f * std::sin (t * 5.2f) * std::sin (t * 1.7f);
        juce::Rectangle<float> tube (cx - 5.0f, cy - 9.0f, 10.0f, 18.0f);
        g.setColour (c.withAlpha (glow * 0.35f));
        g.fillEllipse (tube.expanded (5.0f, 3.0f));
        g.setColour (c.withAlpha (0.9f));
        g.drawRoundedRectangle (tube, 5.0f, 1.2f);
        g.setColour (c.withAlpha (0.4f + 0.6f * glow));
        g.drawLine (cx, tube.getY() + 4.0f, cx, tube.getBottom() - 4.0f, 1.8f);
        g.fillRect (cx - 3.5f, tube.getBottom(), 7.0f, 2.0f);
    }
    else if (id == "CHORUS" || id == "FLANGER" || id == "FLANGUS")
    {
        const auto waves = id == "CHORUS" ? 2 : (id == "FLANGER" ? 1 : 3);
        for (int wv = 0; wv < waves; ++wv)
        {
            juce::Path p;
            for (int i = 0; i <= 22; ++i)
            {
                const auto x = cx - 11.0f + (float) i;
                const auto ph = t * (1.4f + 0.5f * (float) wv) + (float) wv * 1.9f;
                const auto y = cy + ((float) wv - (float) (waves - 1) * 0.5f) * 4.5f
                             + 3.5f * std::sin ((float) i * 0.5f + ph);
                i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
            }
            g.setColour (c.withAlpha (1.0f - 0.25f * (float) wv));
            g.strokePath (p, juce::PathStrokeType (1.2f));
        }
        if (id == "FLANGER")   // sweeping notch dot on the wave
        {
            const auto sweep = cx - 11.0f + 22.0f * (0.5f + 0.5f * std::sin (t * 1.1f));
            g.fillEllipse (sweep - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        }
    }
}

} // namespace cue
