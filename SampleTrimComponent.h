#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace cue
{
// A local edit transaction over the retained original. Only Done publishes
// audio; waveform drawing, candidates and text edits stay on the UI thread.
class SampleTrimComponent final : public juce::Component, private juce::Timer
{
public:
    struct Palette
    {
        juce::Colour panel, well, border, text, muted, accent;
    };

    SampleTrimComponent (AudioPluginAudioProcessor& p, std::function<Palette()> colours)
        : processor (p), palette (std::move (colours))
    {
        setComponentID ("sample-trim-editor");
        setWantsKeyboardFocus (true);
        configureButton (done, "DONE", "sample-trim-done");
        configureButton (cancel, "CANCEL", "sample-trim-cancel");
        configureButton (reset, "FULL SAMPLE", "sample-trim-reset");
        configureButton (zoomOut, "-", "sample-trim-zoom-out");
        configureButton (zoomIn, "+", "sample-trim-zoom-in");
        configureButton (fit, "FIT", "sample-trim-fit");
        done.setTooltip ("Apply trim, clear Favorites, reanalyze BPM and rebuild chops. The original audio stays recoverable.");
        reset.setTooltip ("Restore the full original range. Press Done to apply it.");
        zoomOut.setTooltip ("Zoom out around the middle of the trim view. The selected trim range stays unchanged.");
        zoomIn.setTooltip ("Zoom in for precise trim edges. Scroll vertically or pinch over the waveform to zoom at the pointer.");
        fit.setTooltip ("Show the full original waveform without changing the selected trim range. Drag the overview below to pan when zoomed.");
        zoomOut.onClick = [this] { zoomAt (0.5, 0.5); };
        zoomIn.onClick = [this] { zoomAt (2.0, 0.5); };
        fit.onClick = [this] { fitView(); };
        done.onClick = [this]
        {
            if (! isCurrentSource() || processor.getSampleImportStatus() != nullptr)
            {
                if (onClose) onClose();
                return;
            }
            commitPendingFields();
            if (source != nullptr && ! processor.isMidiMappingLocked()
                && processor.applySampleTrim (startSample, endSample, openingSample) && onClose)
                onClose();
        };
        cancel.onClick = [this] { if (onClose) onClose(); };
        reset.onClick = [this]
        {
            if (source == nullptr) return;
            startSample = 0; endSample = source->buffer.getNumSamples();
            fitView();
            updateFields(); repaint();
        };
        configureField (startField, "sample-trim-start-seconds", true);
        configureField (endField, "sample-trim-end-seconds", false);
        refreshColours();
    }

    ~SampleTrimComponent() override
    {
        stopTimer();
        processor.endSampleTrimPreview();
        startField.onFocusLost = {}; endField.onFocusLost = {};
        startField.onReturnKey = {}; endField.onReturnKey = {};
        startField.onEscapeKey = {}; endField.onEscapeKey = {};
    }
    std::function<void()> onClose;

    void playPreview()
    {
        if (! isCurrentSource() || processor.getSampleImportStatus() != nullptr) return;
        commitPendingFields();
        if (! processor.isSampleTrimPreviewActive() && ! processor.beginSampleTrimPreview (openingSample)) return;
        processor.updateSampleTrimPreviewRange (startSample, endSample);
        processor.startPlayback();
        startTimerHz (30);
    }

    void stopAudition()
    {
        processor.endSampleTrimPreview();
        playheadSample = -1.0;
        getProperties().set ("trimPreviewPlayheadSample", playheadSample);
        repaint();
    }

    bool beginTrim()
    {
        openingSample = processor.getLoadedSample();
        source = processor.getOriginalSampleForTrim();
        if (source == nullptr || source->sampleRate <= 0.0 || source->buffer.getNumSamples() < 2) return false;
        if (! processor.beginSampleTrimPreview (openingSample)) return false;
        initialStart = processor.getSampleTrimStartSample();
        initialEnd = processor.getSampleTrimEndSample();
        startSample = juce::jlimit (0, source->buffer.getNumSamples() - 2, initialStart);
        endSample = juce::jlimit (startSample + 2, source->buffer.getNumSamples(), initialEnd);
        viewStart = 0;
        viewSamples = source->buffer.getNumSamples();
        draggingStart.reset();
        draggingOverview = false;
        playheadSample = -1.0;
        wasShowing = false;
        rebuildPeaks();
        rebuildViewPeaks();
        updateViewDiagnostics();
        updateFields();
        done.setEnabled (! processor.isMidiMappingLocked());
        startTimerHz (10);
        repaint();
        return true;
    }

    void refreshColours()
    {
        const auto c = palette();
        for (auto* field : { &startField, &endField })
        {
            field->setColour (juce::TextEditor::backgroundColourId, c.well);
            field->setColour (juce::TextEditor::textColourId, c.text);
            field->applyColourToAllText (c.text);
            field->setColour (juce::TextEditor::outlineColourId, c.border);
            field->setColour (juce::TextEditor::focusedOutlineColourId, c.accent);
        }
        repaint();
    }

    void visibilityChanged() override
    {
        if (! isVisible())
        {
            stopTimer(); draggingStart.reset();
            stopAudition();
            draggingOverview = false;
            source.reset(); openingSample.reset();
            peakLow.clear(); peakHigh.clear();
            viewPeakLow.clear(); viewPeakHigh.clear();
        }
        else if (source != nullptr) startTimerHz (10);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onClose) onClose();
            return true;
        }
        return false;
    }

    void resized() override
    {
        auto header = getLocalBounds().reduced (12, 6).removeFromTop (26);
        done.setBounds (header.removeFromRight (72));
        header.removeFromRight (8);
        cancel.setBounds (header.removeFromRight (80));
        reset.setBounds (142, 6, 112, 26);
        fit.setBounds (270, 6, 48, 26);
        zoomOut.setBounds (324, 6, 28, 26);
        zoomIn.setBounds (358, 6, 28, 26);
        auto footer = getLocalBounds().reduced (12, 6).removeFromBottom (26);
        footer.removeFromLeft (64);
        startField.setBounds (footer.removeFromLeft (116));
        footer.removeFromLeft (74);
        endField.setBounds (footer.removeFromLeft (116));
        updateViewDiagnostics();
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = palette();
        g.fillAll (c.panel);
        g.setColour (c.border);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 2.0f, 1.0f);
        g.setColour (c.text);
        g.setFont (font (13.0f));
        g.drawText ("TRIM SAMPLE", 12, 6, 126, 26, juce::Justification::centredLeft);
        g.setFont (font (11.0f));
        g.setColour (c.muted);
        g.drawText ("Scroll / pinch: zoom. Overview: pan. Done: apply trim.",
                    juce::Rectangle<int> (480, 6, juce::jmax (0, getWidth() - 658), 26),
                    juce::Justification::centredLeft, true);
        if (source != nullptr)
            g.drawText (juce::String ((double) source->buffer.getNumSamples() / juce::jmax (1, viewSamples), 1) + "x",
                        394, 6, 78, 26, juce::Justification::centredLeft);
        g.drawText ("START s", 12, getHeight() - 32, 60, 26, juce::Justification::centredLeft);
        g.drawText ("END s", 200, getHeight() - 32, 62, 26, juce::Justification::centredRight);
        if (source == nullptr) return;

        const auto area = waveBounds();
        g.setColour (c.well); g.fillRect (area);
        const float middle = area.getCentreY();
        {
            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (area.toNearestInt());
            drawPeaks (g, area, viewPeakLow, viewPeakHigh, c.muted.withAlpha (0.65f));
            const float left = xForSample (startSample), right = xForSample (endSample);
            g.setColour (c.accent.withAlpha (0.11f));
            g.fillRect (juce::Rectangle<float> (left, area.getY(), right - left, area.getHeight()));
            g.setColour (c.panel.withAlpha (0.72f));
            if (left > area.getX()) g.fillRect (area.withRight (juce::jmin (left, area.getRight())));
            if (right < area.getRight()) g.fillRect (area.withLeft (juce::jmax (right, area.getX())));
            g.setColour (c.accent);
            for (const float x : { left, right })
            {
                if (x < area.getX() || x > area.getRight()) continue;
                g.fillRect (x - 1.0f, area.getY(), 2.0f, area.getHeight());
                g.fillRoundedRectangle (x - 5.0f, middle - 13.0f, 10.0f, 26.0f, 2.0f);
            }
            g.setColour (c.text); g.setFont (font (11.0f));
            if (left >= area.getX() && left < area.getRight())
                g.drawText ("START", juce::Rectangle<float> (left + 7.0f, area.getY() + 4.0f, 50.0f, 18.0f), juce::Justification::centredLeft);
            if (right > area.getX() && right <= area.getRight())
                g.drawText ("END", juce::Rectangle<float> (right - 47.0f, area.getY() + 4.0f, 40.0f, 18.0f), juce::Justification::centredRight);
            if (playheadSample >= viewStart && playheadSample < viewStart + viewSamples)
            {
                const float x = xForSample (playheadSample);
                g.setColour (playheadColour (c));
                g.fillRect (x - 0.75f, area.getY(), 1.5f, area.getHeight());
                g.fillEllipse (x - 3.0f, area.getY() + 2.0f, 6.0f, 6.0f);
            }
        }
        paintOverview (g, c);
        g.setColour (c.muted);
        g.drawText ("Original audio retained / Favorites cleared on Done",
                    juce::Rectangle<int> (400, getHeight() - 32, juce::jmax (0, getWidth() - 412), 26),
                    juce::Justification::centredRight, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (overviewBounds().contains (e.position) && viewSamples < originalFrames()
                            ? juce::MouseCursor::DraggingHandCursor
                            : waveBounds().contains (e.position) ? juce::MouseCursor::LeftRightResizeCursor
                                                               : juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingStart.reset();
        draggingOverview = false;
        if (source == nullptr || ! e.mods.isLeftButtonDown()) return;
        if (overviewBounds().contains (e.position))
        {
            const double sample = overviewSampleAt (e.position.x);
            overviewGrabOffset = overviewThumbBounds().contains (e.position)
                                     ? sample - viewStart : viewSamples * 0.5;
            draggingOverview = true;
            panOverviewTo (e.position.x);
            return;
        }
        if (! waveBounds().contains (e.position)) return;
        draggingStart = std::abs (e.position.x - xForSample (startSample)) <= std::abs (e.position.x - xForSample (endSample));
        dragTo (e.position.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingOverview) panOverviewTo (e.position.x);
        else if (draggingStart.has_value()) dragTo (e.position.x);
    }
    void mouseUp (const juce::MouseEvent&) override { draggingStart.reset(); draggingOverview = false; }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (source == nullptr || draggingStart.has_value() || draggingOverview
            || ! std::isfinite (wheel.deltaX) || ! std::isfinite (wheel.deltaY)) return;
        if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) || e.mods.isShiftDown())
        {
            const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
            const double next = viewStart - (double) delta * viewSamples * 0.5;
            setView ((int) std::llround (juce::jlimit (0.0, (double) (originalFrames() - viewSamples), next)), viewSamples);
        }
        else if (wheel.deltaY != 0.0f)
            zoomAt (std::pow (2.0, (double) wheel.deltaY * 3.0), proportionAtX (e.position.x));
    }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        if (! draggingStart.has_value() && ! draggingOverview)
            zoomAt (scaleFactor, proportionAtX (e.position.x));
    }

private:
    static juce::Font font (float height) { return juce::Font (juce::FontOptions ("Menlo", height, juce::Font::plain)); }
    static juce::Colour playheadColour (const Palette& c)
    {
        return c.panel.getBrightness() > 0.5f ? juce::Colour (0xff00a33c) : juce::Colour (0xff00c950);
    }

    void commitPendingFields()
    {
        const auto pendingEnd = endField.getText();
        const bool endWasEdited = source != nullptr
            && pendingEnd.trim() != juce::String ((double) endSample / source->sampleRate, 6);
        commitField (startField, true);
        if (endWasEdited)
        {
            endField.setText (pendingEnd, false);
            commitField (endField, false);
        }
    }

    void configureButton (juce::TextButton& button, const char* label, const char* id)
    {
        button.setButtonText (label); button.setComponentID (id);
        button.setWantsKeyboardFocus (false); button.setMouseClickGrabsKeyboardFocus (false);
        button.getProperties().set ("cueStyle", "padMidiLock");
        addAndMakeVisible (button);
    }

    void configureField (juce::TextEditor& field, const char* id, bool isStart)
    {
        field.setComponentID (id); field.setMultiLine (false); field.setFont (font (12.0f));
        field.setInputRestrictions (20, "0123456789.");
        field.setSelectAllWhenFocused (true);
        field.onReturnKey = [this, &field, isStart] { commitField (field, isStart); };
        field.onFocusLost = [this, &field, isStart] { commitField (field, isStart); };
        field.onEscapeKey = [this] { if (onClose) onClose(); };
        addAndMakeVisible (field);
    }

    void commitField (juce::TextEditor& field, bool isStart)
    {
        if (source == nullptr) return;
        const int previousSample = isStart ? startSample : endSample;
        const auto text = field.getText().trim();
        char* end = nullptr;
        const double seconds = std::strtod (text.toRawUTF8(), &end);
        if (text.isNotEmpty() && end != nullptr && *end == '\0' && std::isfinite (seconds))
        {
            const double limited = juce::jlimit (0.0, (double) source->buffer.getNumSamples() / source->sampleRate, seconds);
            const int sample = (int) std::llround (limited * source->sampleRate);
            if (isStart)
            {
                startSample = juce::jlimit (0, source->buffer.getNumSamples() - 2, sample);
                endSample = juce::jmax (endSample, startSample + 2);
            }
            else
            {
                endSample = juce::jlimit (2, source->buffer.getNumSamples(), sample);
                startSample = juce::jmin (startSample, endSample - 2);
            }
        }
        updateFields();
        const int updatedSample = isStart ? startSample : endSample;
        if (updatedSample != previousSample) revealSample (updatedSample);
        repaint();
    }

    void updateFields()
    {
        startField.setText (juce::String ((double) startSample / source->sampleRate, 6), false);
        endField.setText (juce::String ((double) endSample / source->sampleRate, 6), false);
        getProperties().set ("trimStartSample", startSample);
        getProperties().set ("trimEndSample", endSample);
        processor.updateSampleTrimPreviewRange (startSample, endSample);
    }

    juce::Rectangle<float> waveBounds() const { return getLocalBounds().toFloat().reduced (18.0f, 0.0f).withTrimmedTop (40.0f).withTrimmedBottom (66.0f); }
    juce::Rectangle<float> overviewBounds() const { return { 18.0f, (float) getHeight() - 60.0f, (float) getWidth() - 36.0f, 18.0f }; }
    int originalFrames() const noexcept { return source != nullptr ? source->buffer.getNumSamples() : 0; }
    double proportionAtX (float x) const noexcept
    {
        const auto area = waveBounds();
        return juce::jlimit (0.0, 1.0, (double) (x - area.getX()) / juce::jmax (1.0f, area.getWidth()));
    }
    float xForSample (double sample) const
    {
        const auto area = waveBounds();
        return area.getX() + area.getWidth() * (float) ((double) (sample - viewStart) / juce::jmax (1, viewSamples));
    }
    void dragTo (float x)
    {
        const auto sample = viewStart + (int) std::llround (proportionAtX (x) * viewSamples);
        if (*draggingStart) startSample = juce::jlimit (0, endSample - 2, sample);
        else endSample = juce::jlimit (startSample + 2, source->buffer.getNumSamples(), sample);
        updateFields(); repaint();
    }

    void setView (int start, int length)
    {
        const int total = originalFrames();
        if (total < 2) return;
        length = juce::jlimit (juce::jmin (32, total), total, length);
        start = juce::jlimit (0, total - length, start);
        if (start == viewStart && length == viewSamples) return;
        viewStart = start;
        viewSamples = length;
        rebuildViewPeaks();
        updateViewDiagnostics();
        repaint();
    }

    void fitView() { setView (0, originalFrames()); }

    void zoomAt (double factor, double proportion)
    {
        if (source == nullptr || ! std::isfinite (factor) || factor <= 0.0) return;
        proportion = juce::jlimit (0.0, 1.0, proportion);
        const int total = originalFrames();
        const int length = juce::jlimit (juce::jmin (32, total), total,
                                       (int) std::llround (juce::jlimit (1.0, (double) total, viewSamples / factor)));
        const double anchor = viewStart + proportion * viewSamples;
        setView ((int) std::llround (anchor - proportion * length), length);
    }

    void revealSample (int sample)
    {
        if (source == nullptr || viewSamples >= originalFrames()) return;
        if (sample < viewStart || sample > viewStart + viewSamples)
            setView (sample - viewSamples / 2, viewSamples);
    }

    double overviewSampleAt (float x) const
    {
        const auto area = overviewBounds();
        return juce::jlimit (0.0, 1.0, (double) (x - area.getX()) / juce::jmax (1.0f, area.getWidth())) * originalFrames();
    }

    juce::Rectangle<float> overviewThumbBounds() const
    {
        const auto area = overviewBounds();
        if (area.isEmpty() || originalFrames() < 2) return {};
        const float scale = area.getWidth() / (float) originalFrames();
        const float width = juce::jlimit (juce::jmin (4.0f, area.getWidth()), area.getWidth(), (float) viewSamples * scale);
        const float left = juce::jmin (area.getRight() - width, area.getX() + (float) viewStart * scale);
        return { left, area.getY(), width, area.getHeight() };
    }

    void panOverviewTo (float x)
    {
        setView ((int) std::llround (overviewSampleAt (x) - overviewGrabOffset), viewSamples);
    }

    void updateViewDiagnostics()
    {
        getProperties().set ("trimViewStartSample", viewStart);
        getProperties().set ("trimViewEndSample", viewStart + viewSamples);
        getProperties().set ("trimViewSamples", viewSamples);
        const auto wave = waveBounds(), overview = overviewBounds();
        getProperties().set ("trimWaveX", wave.getX()); getProperties().set ("trimWaveY", wave.getY());
        getProperties().set ("trimWaveWidth", wave.getWidth()); getProperties().set ("trimWaveHeight", wave.getHeight());
        getProperties().set ("trimOverviewX", overview.getX()); getProperties().set ("trimOverviewY", overview.getY());
        getProperties().set ("trimOverviewWidth", overview.getWidth()); getProperties().set ("trimOverviewHeight", overview.getHeight());
        const auto thumb = overviewThumbBounds();
        getProperties().set ("trimOverviewThumbX", thumb.getX()); getProperties().set ("trimOverviewThumbY", thumb.getY());
        getProperties().set ("trimOverviewThumbWidth", thumb.getWidth()); getProperties().set ("trimOverviewThumbHeight", thumb.getHeight());
        zoomIn.setEnabled (source != nullptr && viewSamples > juce::jmin (32, originalFrames()));
        zoomOut.setEnabled (source != nullptr && viewSamples < originalFrames());
        fit.setEnabled (source != nullptr && (viewStart != 0 || viewSamples != originalFrames()));
    }

    void rebuildPeaks()
    {
        buildPeaks (peakLow, peakHigh, 0, originalFrames());
    }

    void rebuildViewPeaks()
    {
        if (viewStart == 0 && viewSamples == originalFrames())
        {
            viewPeakLow = peakLow;
            viewPeakHigh = peakHigh;
        }
        else
            buildPeaks (viewPeakLow, viewPeakHigh, viewStart, viewStart + viewSamples);
    }

    void buildPeaks (std::vector<float>& low, std::vector<float>& high, int begin, int finish)
    {
        constexpr int bins = 1024;
        low.assign (bins, 0.0f); high.assign (bins, 0.0f);
        const int frames = finish - begin, channels = source->buffer.getNumChannels();
        for (int bin = 0; bin < bins; ++bin)
        {
            const int start = begin + (int) ((int64_t) bin * frames / bins);
            const int end = juce::jmin (finish, juce::jmax (start + 1, begin + (int) ((int64_t) (bin + 1) * frames / bins)));
            const int stride = juce::jmax (1, (end - start) / 64);
            for (int sample = start; sample < end; sample += stride)
                for (int channel = 0; channel < channels; ++channel)
                {
                    const float value = juce::jlimit (-1.0f, 1.0f, source->buffer.getSample (channel, sample));
                    low[(size_t) bin] = juce::jmin (low[(size_t) bin], value);
                    high[(size_t) bin] = juce::jmax (high[(size_t) bin], value);
                }
        }
    }

    static void drawPeaks (juce::Graphics& g, juce::Rectangle<float> area,
                           const std::vector<float>& low, const std::vector<float>& high, juce::Colour colour)
    {
        if (low.empty()) return;
        const float middle = area.getCentreY(), amplitude = area.getHeight() * 0.43f;
        g.setColour (colour);
        for (size_t i = 0; i < low.size(); ++i)
        {
            const float x = area.getX() + ((float) i + 0.5f) * area.getWidth() / (float) low.size();
            g.drawVerticalLine (juce::roundToInt (x), middle - high[i] * amplitude, middle - low[i] * amplitude);
        }
    }

    void paintOverview (juce::Graphics& g, const Palette& colours)
    {
        const auto area = overviewBounds();
        if (area.isEmpty()) return;
        g.setColour (colours.well);
        g.fillRect (area);
        drawPeaks (g, area.reduced (0.0f, 1.0f), peakLow, peakHigh, colours.muted.withAlpha (0.45f));
        const float scale = area.getWidth() / (float) originalFrames();
        const float selectedLeft = area.getX() + (float) startSample * scale;
        const float selectedRight = area.getX() + (float) endSample * scale;
        g.setColour (colours.accent.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (selectedLeft, area.getY(), selectedRight - selectedLeft, area.getHeight()));
        g.setColour (colours.accent.withAlpha (0.75f));
        g.drawVerticalLine (juce::roundToInt (selectedLeft), area.getY(), area.getBottom());
        g.drawVerticalLine (juce::roundToInt (selectedRight), area.getY(), area.getBottom());
        const auto thumb = overviewThumbBounds().reduced (0.5f);
        g.setColour (colours.text.withAlpha (0.08f));
        g.fillRect (thumb);
        g.setColour (colours.text.withAlpha (0.85f));
        g.drawRect (thumb, 1.0f);
        if (playheadSample >= 0.0)
        {
            g.setColour (playheadColour (colours));
            g.fillRect (area.getX() + (float) playheadSample * scale - 0.75f, area.getY(), 1.5f, area.getHeight());
        }
    }

    void timerCallback() override
    {
        if (! isCurrentSource())
        {
            stopTimer();
            if (onClose) onClose();
            return;
        }
        const bool showing = isShowing();
        if (wasShowing && ! showing) stopAudition();
        wasShowing = showing;
        done.setEnabled (! processor.isMidiMappingLocked() && processor.getSampleImportStatus() == nullptr);
        const bool playing = processor.isSampleTrimPreviewPlaying();
        const double nextPlayhead = processor.getSampleTrimPreviewPosition();
        if (nextPlayhead != playheadSample)
        {
            playheadSample = nextPlayhead;
            getProperties().set ("trimPreviewPlayheadSample", playheadSample);
            repaint (waveBounds().getUnion (overviewBounds()).toNearestInt());
        }
        const int wantedHz = playing ? 30 : 10;
        if (getTimerInterval() != 1000 / wantedHz) startTimerHz (wantedHz);
    }

    bool isCurrentSource() const
    {
        return openingSample == processor.getLoadedSample() && source == processor.getOriginalSampleForTrim()
            && initialStart == processor.getSampleTrimStartSample() && initialEnd == processor.getSampleTrimEndSample();
    }

    AudioPluginAudioProcessor& processor;
    std::function<Palette()> palette;
    std::shared_ptr<const AudioPluginAudioProcessor::LoadedSampleData> source;
    std::shared_ptr<const AudioPluginAudioProcessor::LoadedSampleData> openingSample;
    std::vector<float> peakLow, peakHigh, viewPeakLow, viewPeakHigh;
    juce::TextButton done, cancel, reset, zoomOut, zoomIn, fit;
    juce::TextEditor startField, endField;
    int startSample = 0, endSample = 2, initialStart = 0, initialEnd = 0;
    int viewStart = 0, viewSamples = 0;
    double overviewGrabOffset = 0.0;
    double playheadSample = -1.0;
    bool draggingOverview = false;
    bool wasShowing = false;
    std::optional<bool> draggingStart;
};
}
