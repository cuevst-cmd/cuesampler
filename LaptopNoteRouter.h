#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <functional>

namespace cuesampler
{
// One authoritative destination per held note. A newly established or lost
// host connection must never redirect its note-off into a different path.
class LaptopNoteRouter
{
public:
    using Sink = std::function<void (const juce::MidiMessage&)>;
    LaptopNoteRouter (Sink preview, Sink host) : local (std::move (preview)), remote (std::move (host)) {}
    void send (const juce::MidiMessage& message, bool connected)
    {
        if (! message.isNoteOn() && ! message.isNoteOff()) return;
        auto& route = routes[(size_t) (message.getChannel() - 1)][(size_t) message.getNoteNumber()];
        if (message.isNoteOn())
        {
            if (route != Route::none) return;
            route = connected ? Route::host : Route::preview;
            (connected ? remote : local) (message);
        }
        else
        {
            if (route == Route::host) remote (message);
            if (route == Route::preview) local (message);
            route = Route::none;
        }
    }
private:
    enum class Route : unsigned char { none, preview, host };
    std::array<std::array<Route, 128>, 16> routes {};
    Sink local, remote;
};
}
