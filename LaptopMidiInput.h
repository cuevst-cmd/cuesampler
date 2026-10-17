#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <functional>

namespace cuesampler
{
// Message-thread input only. The host records channel + note, rather than a
// viewport-dependent transpose. Each key retains its original note-off pair.
class LaptopMidiInput
{
public:
    using Sink = std::function<void (const juce::MidiMessage&)>;
    explicit LaptopMidiInput (Sink output) : send (std::move (output)) {}
    ~LaptopMidiInput() { releaseAll(); }

    static int offsetForKey (int key)
    {
        if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
        constexpr char keys[] = "ZSXDCVGBHNJM,L.;/Q2W3ER5T6Y7UI9O0P[]";
        constexpr int offsets[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                                   12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,31 };
        static_assert (sizeof (keys) - 1 == sizeof (offsets) / sizeof (offsets[0]));
        for (size_t i = 0; i < sizeof (keys) - 1; ++i)
            if (key == keys[i]) return offsets[i];
        return -1;
    }

    bool press (int key, int baseNote, int channel)
    {
        if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
        const int offset = offsetForKey (key);
        if (offset < 0 || key < 0 || key >= (int) held.size()) return false;
        if (blocked[(size_t) key]) return true;
        const int note = baseNote + offset;
        if (note < 0 || note > 127 || channel < 1 || channel > 16) return true;
        auto& entry = held[(size_t) key];
        if (entry.channel != 0) return true; // Ignore OS key-repeat.
        // Overlapping piano rows can address the same note. Only the first
        // physical key starts it and the last release ends it.
        bool alreadyDown = false;
        for (const auto& other : held)
            alreadyDown |= other.channel == channel && other.note == note;
        entry = { channel, note };
        if (! alreadyDown) send (juce::MidiMessage::noteOn (channel, note, (juce::uint8) 100));
        return true;
    }

    template <typename IsDown> void releaseMissing (IsDown&& isDown)
    {
        for (size_t key = 0; key < held.size(); ++key)
            if (! isDown ((int) key))
            {
                blocked[key] = false;
                if (held[key].channel != 0) release (key);
            }
    }
    void releaseKey (int key)
    {
        if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
        if (key < 0 || key >= (int) held.size()) return;
        blocked[(size_t) key] = false;
        if (held[(size_t) key].channel != 0) release ((size_t) key);
    }
    void releaseAll()
    {
        for (size_t key = 0; key < held.size(); ++key)
            if (held[key].channel != 0)
            {
                blocked[key] = true; // No repeat into a new bank before key-up.
                release (key);
            }
    }

private:
    struct Held { int channel = 0, note = 0; };
    void release (size_t key)
    {
        const auto entry = held[key];
        held[key] = {};
        for (const auto& other : held)
            if (other.channel == entry.channel && other.note == entry.note) return;
        send (juce::MidiMessage::noteOff (entry.channel, entry.note));
    }
    Sink send;
    std::array<Held, 128> held {};
    std::array<bool, 128> blocked {};
};
}
