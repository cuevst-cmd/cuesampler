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
static const char* const cueWordmarkSvg =
R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="20 -700 4123 760"><path fill="#F2E7DA" d="M1255.0 -267Q1250.0 -172 1186.0 -108.0Q1122.0 -44 997.5 -12.0Q873.0 20 683.0 20Q538.0 20 423.0 4.5Q308.0 -11 227.0 -48.5Q146.0 -86 103.0 -151.0Q60.0 -216 60.0 -315Q60.0 -414 103.0 -480.5Q146.0 -547 227.0 -586.5Q308.0 -626 423.0 -643.0Q538.0 -660 683.0 -660Q873.0 -660 998.0 -625.5Q1123.0 -591 1187.0 -524.0Q1251.0 -457 1256.0 -361H996.0Q984.0 -393 952.0 -417.5Q920.0 -442 856.0 -456.0Q792.0 -470 683.0 -470Q555.0 -470 475.0 -454.5Q395.0 -439 358.0 -405.0Q321.0 -371 321.0 -315Q321.0 -264 358.0 -232.0Q395.0 -200 475.0 -185.0Q555.0 -170 683.0 -170Q792.0 -170 855.5 -183.0Q919.0 -196 951.0 -218.5Q983.0 -241 995.0 -267Z M2303.0 -365V-640H2553.0V-320Q2553.0 -235 2521.5 -175.5Q2490.0 -116 2434.0 -77.5Q2378.0 -39 2305.0 -18.0Q2232.0 3 2148.5 11.5Q2065.0 20 1979.0 20Q1888.0 20 1803.0 11.5Q1718.0 3 1645.5 -18.0Q1573.0 -39 1519.0 -77.5Q1465.0 -116 1434.5 -175.5Q1404.0 -235 1404.0 -320V-640H1654.0V-365Q1654.0 -285 1693.0 -243.0Q1732.0 -201 1804.5 -185.5Q1877.0 -170 1979.0 -170Q2078.0 -170 2151.0 -185.5Q2224.0 -201 2263.5 -243.0Q2303.0 -285 2303.0 -365Z M2961.0 -270V-190H3721.0V0H2711.0V-640H3719.0V-450H2961.0V-370H3581.0V-270Z M4103.0 -151V0H3849.0V-151Z"/></svg>)SVG";

// Toolbar
//==============================================================================
RackToolbar::RackToolbar()
{
    startTimerHz (24);
    addAndMakeVisible (minimizeButton);
    minimizeButton.setButtonText ("MINIMIZE");
    minimizeButton.setTooltip ("Hide the FX rack panels.");
    minimizeButton.onClick = [this]
    {
        if (onMinimizeRequested != nullptr)
            onMinimizeRequested();
    };

    const juce::String svg (cueWordmarkSvg);
    cueWordmark = juce::Drawable::createFromImageData (svg.toRawUTF8(),
                                                       (size_t) svg.getNumBytesAsUTF8());
}

RackToolbar::~RackToolbar()
{
    stopTimer();
}

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

void RackToolbar::setMinimized (bool shouldBeMinimized)
{
    minimizeButton.setButtonText (shouldBeMinimized ? "SHOW FX" : "MINIMIZE");
    minimizeButton.setTooltip (shouldBeMinimized ? "Show the FX rack panels."
                                                 : "Hide the FX rack panels.");
}

void RackToolbar::resized()
{
    auto r = getLocalBounds().reduced (8, 5);

    float lockupW = 108.0f;
    if (cueWordmark != nullptr)
    {
        const auto db = cueWordmark->getDrawableBounds();
        if (db.getHeight() > 0.0f)
            lockupW = 20.0f * db.getWidth() / db.getHeight();
    }
    const int rightReserved = juce::roundToInt (lockupW) + 20;
    r.removeFromRight (rightReserved);
    minimizeButton.setBounds (r.removeFromRight (88));
    r.removeFromRight (8);

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
    g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(), (float) getHeight() - 0.5f, 1.0f);

    // --- Stacked brand lockup: "CUE." over wide-tracked "RACK" right-anchored ---
    const float logoH = 20.0f;
    float lockupW = 108.0f;

    if (cueWordmark != nullptr)
    {
        const auto db = cueWordmark->getDrawableBounds();
        if (db.getHeight() > 0.0f)
            lockupW = logoH * db.getWidth() / db.getHeight();
    }

    const float rightMargin = 14.0f;
    const float logoX = juce::jmax (8.0f, (float) getWidth() - lockupW - rightMargin);
    const float logoY = 4.0f;
    const auto targetInk = colours::cream;

    if (cueWordmark != nullptr)
    {
        if (wordmarkColour != targetInk)
        {
            cueWordmark->replaceColour (wordmarkColour, targetInk);
            wordmarkColour = targetInk;
        }
        cueWordmark->drawWithin (g, { logoX, logoY, lockupW, logoH },
                                 juce::RectanglePlacement::stretchToFit, 1.0f);
    }

    const float rackSize = 10.5f;
    auto rackFont = juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), rackSize, juce::Font::bold));
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (rackFont, "RACK", 0.0f, 0.0f);
        const float naturalW = ga.getBoundingBox (0, -1, true).getWidth();
        const float kern = juce::jmax (0.0f, (lockupW - naturalW) / (rackSize * 4.0f));
        rackFont = rackFont.withExtraKerningFactor (kern);
    }

    const int rackTop = (int) std::round (logoY + logoH - 1.0f);
    g.setColour (targetInk);
    g.setFont (rackFont);
    g.drawText ("RACK",
                juce::Rectangle<int> ((int) std::round (logoX), rackTop,
                                      (int) std::ceil (lockupW) + 20, 18),
                juce::Justification::centredLeft, false);

    if (moduleChips.isEmpty())
    {
        g.setColour (colours::creamDim.withAlpha (0.4f));
        g.setFont (monoFont (9.5f));
        g.drawText ("ALL MODULES RACKED", 12, 0, 180, getHeight(),
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
