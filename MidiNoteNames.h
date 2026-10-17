#pragma once

#include <juce_core/juce_core.h>

namespace cuesampler
{
// FL Studio labels MIDI note 0 C0, note 48 C4, and middle C (60) C5.
inline constexpr int midiMiddleCOctave = 5;
inline constexpr int defaultKeyboardStartNote = 24; // First chop: FL C2.

inline juce::String midiNoteName (int note)
{
    if (note < 0 || note > 127)
        return "NO MIDI";
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[note % 12]) + juce::String (note / 12);
}
}
