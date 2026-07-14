#include "FxRackStrip.h"

#include "UI/CueLookAndFeel.h"
#include "UI/RackToolbar.h"
#include "UI/Modules/EQPanel.h"
#include "UI/Modules/CompressorPanel.h"
#include "UI/Modules/LimiterPanel.h"
#include "UI/Modules/DelayPanel.h"
#include "UI/Modules/ReverbPanel.h"
#include "UI/Modules/CrusherPanel.h"
#include "UI/Modules/ImagerPanel.h"
#include "UI/Modules/ModulationPanels.h"
#include "UI/Modules/AmpPanel.h"

namespace cue
{

namespace
{
    constexpr int kGap = 4;
    constexpr int kToolbarH = 46;
    constexpr int kAnimMs = 240;

    // Panels never render below this geometry (the rack itself never goes
    // under ~260px rows; knobs and faders need the room). Width floors are
    // per-panel, proportional to their design weight.
    constexpr int   kMinRowH = 240;
    constexpr float kMinWidthFactor = 0.72f;

    class SmoothRackViewport final : public juce::Viewport, private juce::Timer
    {
    public:
        SmoothRackViewport()
        {
            setScrollOnDragMode (ScrollOnDragMode::all);
        }

        ~SmoothRackViewport() override
        {
            stopTimer();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            const auto* content = getViewedComponent();
            if (content == nullptr)
            {
                juce::Viewport::mouseWheelMove (e, wheel);
                return;
            }

            const int maxX = juce::jmax (0, content->getWidth() - getWidth());
            if (maxX <= 0)
                return;

            if (std::abs ((float) getViewPositionX() - targetX) > 24.0f)
                targetX = (float) getViewPositionX();

            float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY)
                              ? wheel.deltaX
                              : (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);

            if (std::abs (delta) > 0.0001f)
            {
                const float scale = wheel.isSmooth ? 520.0f : 110.0f;
                const float impulse = -delta * scale;

                velocity += impulse * 0.35f;
                targetX = juce::jlimit (0.0f, (float) maxX, targetX + impulse);

                if (! isTimerRunning())
                    startTimerHz (60);
            }
        }

        void syncTargetPosition()
        {
            targetX = (float) getViewPositionX();
            velocity = 0.0f;
        }

    private:
        void timerCallback() override
        {
            const auto* content = getViewedComponent();
            const int maxX = content != nullptr ? juce::jmax (0, content->getWidth() - getWidth()) : 0;

            targetX = juce::jlimit (0.0f, (float) maxX, targetX + velocity);
            velocity *= 0.82f;

            const float currentX = (float) getViewPositionX();
            const float diff = targetX - currentX;

            if (std::abs (diff) < 0.25f && std::abs (velocity) < 0.25f)
            {
                setViewPosition (juce::roundToInt (targetX), getViewPositionY());
                velocity = 0.0f;
                stopTimer();
            }
            else
            {
                const float newX = currentX + diff * 0.32f;
                setViewPosition (juce::roundToInt (newX), getViewPositionY());
            }
        }

        float targetX = 0.0f;
        float velocity = 0.0f;
    };
}

//==============================================================================
struct FxRackStrip::Impl
{
    struct RackItem
    {
        juce::String id;
        ModulePanel* panel;
        float weight;
        const char* powerParam;
        bool defaultHidden;
    };

    Impl (FxRackStrip& ownerIn, juce::AudioProcessorValueTreeState& state,
          const FxRackMeterHooks& hooks)
        : owner (ownerIn), apvts (state),
          eq (state), comp (state), limiter (state), delay (state), reverb (state),
          crusher (state), imager (state), chorusPanel (state), flangerPanel (state),
          flangusPanel (state), ampPanel (state)
    {
        owner.setLookAndFeel (&lookAndFeel);

        // Panels live on a content canvas inside a horizontal viewport: when
        // the window simply cannot fit every visible panel at its minimum
        // usable size (even after re-flowing to more rows), the row scrolls
        // instead of chopping knobs.
        viewport.setViewedComponent (&rackContent, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setScrollBarThickness (8);
        owner.addAndMakeVisible (viewport);

        // id, panel, layout weight, power param, starts-in-toolbar — the
        // CUERACK set (minus CUE / MASTER / HALF, which the sampler covers
        // elsewhere). Default row: EQ | COMP | TAPE DELAY | REVERB.
        items = { { "EQ",      &eq,           380.0f, pid::eqOn,    true  },
                  { "COMP",    &comp,         300.0f, pid::compOn,  true  },
                  { "DLY",     &delay,        300.0f, pid::dlyOn,   true  },
                  { "VERB",    &reverb,       330.0f, pid::revOn,   true  },
                  { "LIM",     &limiter,      165.0f, pid::limOn,   true  },
                  { "CRUSH",   &crusher,      250.0f, pid::crushOn, true  },
                  { "IMG",     &imager,       268.0f, pid::imgOn,   true  },
                  { "CHORUS",  &chorusPanel,  250.0f, pid::chOn,    true  },
                  { "FLANGER", &flangerPanel, 230.0f, pid::flOn,    true  },
                  { "FLANGUS", &flangusPanel, 230.0f, pid::fgOn,    true  },
                  { "AMP",     &ampPanel,     300.0f, pid::ampOn,   true  } };

        for (auto& item : items)
        {
            rackContent.addAndMakeVisible (*item.panel);
            wirePanel (item);
        }

        loadArrangement();

        owner.addAndMakeVisible (toolbar);
        toolbar.onRestorePanel = [this] (const juce::String& id) { restorePanel (id); };
        syncToolbar();

        // Live meters, wired the same way the rack's editor does.
        if (hooks.compGainReductionDb != nullptr)
            comp.setGRSource (hooks.compGainReductionDb);
        if (hooks.limiterBandGRDb != nullptr && hooks.limiterNumBands != nullptr)
            limiter.setBandGRSource (hooks.limiterBandGRDb, hooks.limiterNumBands());
        if (hooks.imagerMidSide != nullptr)
            imager.setLevelSource (hooks.imagerMidSide);

        refreshColours();   // scrollbar + panel colours for the current theme
    }

    ~Impl()
    {
        animator.cancelAllAnimations (false);
        owner.setLookAndFeel (nullptr);
    }

    //==================================================================
    RackItem* itemFor (const juce::String& id)
    {
        for (auto& item : items)
            if (item.id == id)
                return &item;
        return nullptr;
    }

    void wirePanel (RackItem& item)
    {
        auto* panel = item.panel;
        const auto id = item.id;

        panel->onRemoveRequested = [this, id] { removePanel (id); };

        panel->onDragStarted = [this, panel]
        {
            draggedPanel = panel;
            panel->toFront (false);
            panel->setAlpha (0.88f);
            animator.cancelAnimation (panel, false);
        };

        panel->onDragMoved = [this, panel] (juce::Point<int> desiredTopLeft)
        {
            if (draggedPanel == panel)
                updateDragPreview (desiredTopLeft, *panel);
        };

        panel->onDragFinished = [this, panel]
        {
            if (draggedPanel == panel)
            {
                draggedPanel = nullptr;
                panel->setAlpha (1.0f);
                applyLayout (true);
                saveArrangement();
            }
        };
    }

    //==================================================================
    void loadArrangement()
    {
        auto& state = apvts.state;

        juce::StringArray allIds, defaultOrder, defaultHidden;
        for (const auto& item : items)
        {
            allIds.add (item.id);
            (item.defaultHidden ? defaultHidden : defaultOrder).add (item.id);
        }

        order.clear();
        hidden.clear();
        order.addTokens (state.getProperty ("fxOrder", juce::String()).toString(), ",", {});
        hidden.addTokens (state.getProperty ("fxHidden", juce::String()).toString(), ",", {});
        order.removeEmptyStrings();
        hidden.removeEmptyStrings();

        // validate: every known id appears exactly once across order + hidden
        auto valid = order.size() + hidden.size() == allIds.size();
        for (const auto& id : allIds)
            if ((order.contains (id) ? 1 : 0) + (hidden.contains (id) ? 1 : 0) != 1)
                valid = false;

        if (! valid)
        {
            order  = defaultOrder;
            hidden = defaultHidden;
        }

        for (const auto& item : items)
            item.panel->setVisible (order.contains (item.id));
    }

    void saveArrangement()
    {
        auto& state = apvts.state;
        state.setProperty ("fxOrder",  order.joinIntoString (","), nullptr);
        state.setProperty ("fxHidden", hidden.joinIntoString (","), nullptr);
    }

    void syncToolbar()
    {
        std::vector<RackToolbar::ModuleEntry> unused;
        for (const auto& id : hidden)
            if (auto* item = itemFor (id))
                unused.push_back ({ id, item->panel->getAccent() });
        toolbar.setUnusedPanels (unused);
    }

    //==================================================================
    // CUERACK's weighted rack layout, made resize-proof:
    //  - the band's height decides how many rows fit (1..3, each >= kMinRowH)
    //  - panels split across rows by balanced weight (the rack's midpoint
    //    rule, generalised)
    //  - every panel has a width floor proportional to its weight; if even
    //    the deepest row split cannot satisfy the floors, the content canvas
    //    grows and the viewport scrolls horizontally — knobs never chop.
    static int minPanelWidth (const RackItem& item)
    {
        return juce::roundToInt (item.weight * kMinWidthFactor);
    }

    // Balanced partition into `rows` runs; returns each row's start index.
    static std::vector<size_t> computeBreaks (const std::vector<RackItem*>& visible, int rows)
    {
        std::vector<size_t> breaks { 0 };
        if (rows <= 1 || visible.size() < 2)
            return breaks;

        float total = 0.0f;
        for (auto* v : visible)
            total += v->weight;

        float acc = 0.0f;
        int nextRow = 1;
        for (size_t i = 0; i < visible.size() && nextRow < rows; ++i)
        {
            const auto remainingRows = (size_t) (rows - nextRow);
            if (i > 0 && breaks.back() != i
                && visible.size() - i >= remainingRows
                && acc + visible[i]->weight * 0.5f >= total * (float) nextRow / (float) rows)
            {
                breaks.push_back (i);
                ++nextRow;
            }
            acc += visible[i]->weight;
        }
        return breaks;
    }

    // Widest row's floor requirement (margins + gaps included).
    static int requiredWidth (const std::vector<RackItem*>& visible,
                              const std::vector<size_t>& breaks)
    {
        int widest = 0;
        for (size_t r = 0; r < breaks.size(); ++r)
        {
            const auto begin = breaks[r];
            const auto end   = r + 1 < breaks.size() ? breaks[r + 1] : visible.size();
            int wsum = 0;
            for (size_t i = begin; i < end; ++i)
                wsum += minPanelWidth (*visible[i]);
            wsum += kGap * (int) (end - begin - 1);
            widest = juce::jmax (widest, wsum);
        }
        return widest + 20;   // content margins
    }

    void rebuildTargets()
    {
        targets.clear();

        const int viewW = juce::jmax (0, viewport.getMaximumVisibleWidth());
        const int viewH = juce::jmax (0, viewport.getMaximumVisibleHeight());

        std::vector<RackItem*> visible;
        for (const auto& id : order)
            if (auto* item = itemFor (id))
                visible.push_back (item);

        if (visible.empty() || viewW <= 0 || viewH <= 0)
        {
            rackContent.setSize (juce::jmax (1, viewW), juce::jmax (1, viewH));
            return;
        }

        // How many rows does the height allow, and how many do the width
        // floors ask for? Meet in the middle; scroll only when forced.
        const int maxRows = juce::jlimit (1, 3, (viewH + kGap) / (kMinRowH + kGap));

        int rows = 1;
        std::vector<size_t> breaks = computeBreaks (visible, rows);
        while (rows < maxRows && rows < (int) visible.size()
               && requiredWidth (visible, breaks) > viewW)
        {
            ++rows;
            breaks = computeBreaks (visible, rows);
        }

        const int contentW = juce::jmax (viewW, requiredWidth (visible, breaks));
        rackContent.setSize (contentW, viewH);

        auto area = juce::Rectangle<int> (0, 0, contentW, viewH).reduced (10, 0);
        const int rowH = juce::jmax (1, (area.getHeight() - kGap * (rows - 1)) / rows);

        for (size_t r = 0; r < breaks.size(); ++r)
        {
            const auto begin = breaks[r];
            const auto end   = r + 1 < breaks.size() ? breaks[r + 1] : visible.size();
            auto rowArea = juce::Rectangle<int> (area.getX(),
                                                 area.getY() + (int) r * (rowH + kGap),
                                                 area.getWidth(), rowH);

            // Weighted widths with per-panel floors: floored panels lock at
            // their minimum, the rest share the remaining width by weight.
            const auto count = end - begin;
            const int usable = rowArea.getWidth() - kGap * (int) (count - 1);

            std::vector<int> widths ((size_t) count, 0);
            std::vector<bool> locked ((size_t) count, false);

            for (int pass = 0; pass < 4; ++pass)
            {
                float freeWeight = 0.0f;
                int lockedWidth = 0;
                for (size_t i = 0; i < count; ++i)
                    locked[i] ? (void) (lockedWidth += widths[i])
                              : (void) (freeWeight += visible[begin + i]->weight);

                if (freeWeight <= 0.0f)
                    break;

                bool changed = false;
                const int freeWidth = usable - lockedWidth;
                for (size_t i = 0; i < count; ++i)
                {
                    if (locked[i])
                        continue;
                    const auto w = juce::roundToInt ((float) freeWidth * visible[begin + i]->weight / freeWeight);
                    const auto floorW = minPanelWidth (*visible[begin + i]);
                    if (w < floorW)
                    {
                        widths[i] = floorW;
                        locked[i] = true;
                        changed = true;
                    }
                    else
                    {
                        widths[i] = w;
                    }
                }
                if (! changed)
                    break;
            }

            auto x = rowArea.getX();
            for (size_t i = 0; i < count; ++i)
            {
                auto w = widths[i];
                if (i == count - 1)   // absorb rounding, respecting the floor
                    w = juce::jmax (minPanelWidth (*visible[begin + i]), rowArea.getRight() - x);
                targets[visible[begin + i]->id] = { x, rowArea.getY(), w, rowArea.getHeight() };
                x += w + kGap;
            }
        }
    }

    void applyLayout (bool animate)
    {
        for (const auto& id : order)
        {
            auto* item = itemFor (id);
            if (item == nullptr || item->panel == draggedPanel)
                continue;

            const auto it = targets.find (id);
            if (it == targets.end())
                continue;

            item->panel->setVisible (true);
            if (animate)
                animator.animateComponent (item->panel, it->second, 1.0f, kAnimMs, false, 1.0, 0.35);
            else
                item->panel->setBounds (it->second);
        }
    }

    //==================================================================
    void updateDragPreview (juce::Point<int> desiredTopLeft, ModulePanel& panel)
    {
        panel.setTopLeftPosition (desiredTopLeft);

        const auto dragCentre = panel.getBounds().getCentre();
        juce::String nearestId;
        auto nearestDist = std::numeric_limits<int>::max();

        for (const auto& [id, rect] : targets)
        {
            const auto d = rect.getCentre().getDistanceSquaredFrom (dragCentre);
            if (d < nearestDist)
            {
                nearestDist = d;
                nearestId = id;
            }
        }

        juce::String dragId;
        for (const auto& item : items)
            if (item.panel == &panel)
                dragId = item.id;

        if (nearestId.isEmpty() || nearestId == dragId)
            return;

        const auto from = order.indexOf (dragId);
        const auto to   = order.indexOf (nearestId);
        if (from >= 0 && to >= 0 && from != to)
        {
            order.move (from, to);
            rebuildTargets();
            applyLayout (true);
        }
    }

    //==================================================================
    void removePanel (const juce::String& id)
    {
        auto* item = itemFor (id);
        if (item == nullptr || ! order.contains (id))
            return;

        order.removeString (id);
        hidden.addIfNotAlreadyThere (id);

        if (item->powerParam != nullptr)
            if (auto* param = apvts.getParameter (item->powerParam))
                param->setValueNotifyingHost (0.0f);

        auto* panel = item->panel;
        const auto target = panel->getBounds().withSizeKeepingCentre (
                                juce::jmax (40, panel->getWidth() / 2),
                                juce::jmax (40, panel->getHeight() / 2));
        animator.animateComponent (panel, target, 0.0f, 180, true, 1.0, 0.6);

        juce::Component::SafePointer<juce::Component> safe (panel);
        juce::Timer::callAfterDelay (200, [safe]
        {
            if (safe != nullptr)
            {
                safe->setVisible (false);
                safe->setAlpha (1.0f);
            }
        });

        rebuildTargets();
        applyLayout (true);
        syncToolbar();
        saveArrangement();
        owner.repaint();
    }

    void restorePanel (const juce::String& id)
    {
        auto* item = itemFor (id);
        if (item == nullptr || ! hidden.contains (id))
            return;

        hidden.removeString (id);
        order.add (id);

        if (item->powerParam != nullptr)
            if (auto* param = apvts.getParameter (item->powerParam))
                param->setValueNotifyingHost (1.0f);

        rebuildTargets();

        auto* panel = item->panel;
        const auto it = targets.find (id);
        if (it != targets.end())
        {
            panel->setBounds (it->second.withSizeKeepingCentre (juce::jmax (40, it->second.getWidth() / 2),
                                                                juce::jmax (40, it->second.getHeight() / 2)));
            panel->setAlpha (0.0f);
            panel->setVisible (true);
            animator.animateComponent (panel, it->second, 1.0f, kAnimMs, false, 1.0, 0.35);
        }

        applyLayout (true);
        syncToolbar();
        saveArrangement();
        owner.repaint();
    }

    bool isEmpty() const { return order.isEmpty(); }

    void paint (juce::Graphics& g)
    {
        if (isEmpty())
        {
            auto area = viewport.getBounds();
            g.setColour (colours::creamDim.withAlpha (0.4f));
            g.setFont (monoFont (13.0f));
            g.drawFittedText ("FX RACK EMPTY\nCLICK MODULE CHIPS ABOVE TO ADD EFFECTS",
                              area, juce::Justification::centred, 2);
        }
    }

    //==================================================================
    void resized()
    {
        animator.cancelAllAnimations (false);
        auto r = owner.getLocalBounds();
        toolbar.setBounds (r.removeFromTop (kToolbarH));
        r.removeFromTop (kGap);
        viewport.setBounds (r);
        rebuildTargets();
        applyLayout (false);
        viewport.syncTargetPosition();
    }

    void refreshColours()
    {
        lookAndFeel.refreshColours();
        for (auto& item : items)
            item.panel->refreshColours();

        auto& hbar = viewport.getHorizontalScrollBar();
        hbar.setColour (juce::ScrollBar::thumbColourId, colours::creamDim);
        hbar.setColour (juce::ScrollBar::trackColourId, colours::slot);

        owner.repaint();
    }

    //==================================================================
    FxRackStrip& owner;
    juce::AudioProcessorValueTreeState& apvts;

    CueLookAndFeel lookAndFeel;

    EQPanel         eq;
    CompressorPanel comp;
    LimiterPanel    limiter;
    DelayPanel      delay;
    ReverbPanel     reverb;
    CrusherPanel    crusher;
    ImagerPanel     imager;
    ChorusPanel     chorusPanel;
    FlangerPanel    flangerPanel;
    FlangusPanel    flangusPanel;
    AmpPanel        ampPanel;

    RackToolbar toolbar;
    SmoothRackViewport viewport;
    juce::Component rackContent;

    std::vector<RackItem> items;
    juce::StringArray order, hidden;
    std::map<juce::String, juce::Rectangle<int>> targets;

    ModulePanel* draggedPanel = nullptr;
    juce::ComponentAnimator animator;
};

//==============================================================================
FxRackStrip::FxRackStrip (juce::AudioProcessorValueTreeState& state, const FxRackMeterHooks& hooks)
    : impl (std::make_unique<Impl> (*this, state, hooks))
{
}

FxRackStrip::~FxRackStrip() = default;

void FxRackStrip::paint (juce::Graphics& g)   { impl->paint (g); }
void FxRackStrip::resized()                 { impl->resized(); }
void FxRackStrip::refreshColours()          { impl->refreshColours(); }

void FxRackStrip::applyRackTheme (bool light, bool warpActive, bool halftimeActive)
{
    cue::applyTheme (light ? Theme::light : Theme::dark, warpActive, halftimeActive);
}

} // namespace cue
