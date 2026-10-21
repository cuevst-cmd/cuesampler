#pragma once

#include <optional>
#include <algorithm>
#include <bitset>

namespace cuesampler
{
// FL's documented Opt+numeric-keypad shortcuts select absolute octaves 2..6.
// This bridge never changes the plugin's incoming MIDI or saved chop mapping.
enum class FlTypingLayout { standard, extended };

// The dedicated extended layout is the normal piano layout shifted +24 notes.
// FL octave 2 therefore plays C4, and FL octave 6 plays C8 (upper keys reach G10).
inline int flTypingLayoutOffset (FlTypingLayout layout) { return layout == FlTypingLayout::extended ? 24 : 0; }

struct KeyboardOctaveRange
{
    int firstNote = 48, lastNote = 48;
    bool hasNotes = false;
};

inline KeyboardOctaveRange keyboardOctaveRange (const std::bitset<128>& mapped, int firstAllowed = 0,
                                               int lastAllowed = 120, int visibleNotes = 24)
{
    KeyboardOctaveRange result { std::clamp (48, firstAllowed, lastAllowed),
                                 std::clamp (48, firstAllowed, lastAllowed), false };
    for (int note = firstAllowed; note <= std::min (127, lastAllowed + visibleNotes - 1); ++note)
        if (mapped.test (static_cast<std::size_t> (note)))
        {
            const int octaveStart = std::clamp (note / 12 * 12, firstAllowed, lastAllowed);
            if (! result.hasNotes) result.firstNote = octaveStart;
            result.lastNote = octaveStart;
            result.hasNotes = true;
        }
    return result;
}

struct FlOctaveShortcut
{
    int octave;
    unsigned short macKeyCode;
};

inline std::optional<FlOctaveShortcut> makeFlOctaveShortcut (int startNote, FlTypingLayout layout = FlTypingLayout::standard)
{
    const int shortcutNote = startNote - flTypingLayoutOffset (layout);
    if (shortcutNote < 24 || shortcutNote > 72 || shortcutNote % 12 != 0)
        return std::nullopt;
    const int octave = shortcutNote / 12;
    return FlOctaveShortcut { octave, static_cast<unsigned short> (84 + octave - 2) };
}

enum class FlOctaveShortcutResult
{
    queued, unsupportedHost, unsupportedOctave, inactiveHost, keysHeld, unavailableWindow, failed, viewOnly
};

// C2-C6 sends FL's absolute shortcut; higher octaves browse CUE's screen keys
// without changing FL. Returning to C6 sends its real shortcut again. Older
// C0-root projects can also browse their lower screen keys. Blocked native
// requests leave the old view intact; posting is not host acknowledgement.
template <typename PostShortcut, typename ChangeView>
std::optional<FlOctaveShortcutResult> stepFlTypingOctave (int currentNote, int direction,
                                                        PostShortcut&& postShortcut,
                                                        ChangeView&& changeView, int minimumNote = 24)
{
    if (direction == 0) return std::nullopt;
    minimumNote = std::clamp (minimumNote, 0, 24);
    const int nextNote = std::clamp (currentNote + (direction < 0 ? -12 : 12), minimumNote, 120);
    if (nextNote == currentNote) return std::nullopt;
    if (nextNote < 24 || nextNote > 72)
    {
        changeView (nextNote);
        return FlOctaveShortcutResult::viewOnly;
    }
    const auto result = postShortcut (nextNote);
    if (result == FlOctaveShortcutResult::queued) changeView (nextNote);
    return result;
}

// Incoming MIDI is authoritative for key lights. Reveal a note outside the
// current view without jumping between the overlapping Z and Q piano rows.
// No MIDI is translated, and notes outside the standard typing range are
// left to the normal MIDI/controller view.
inline int flTypingViewForNote (int currentNote, int playedNote, int visibleNotes = 32)
{
    // Do not let a clip or FL typing input pull the user out of the higher
    // screen-keyboard view they explicitly chose with OCT.
    if (currentNote > 72) return currentNote;
    if (playedNote < 24 || playedNote > 72 + visibleNotes - 1)
        return currentNote;
    if (currentNote >= 24 && currentNote <= 72
        && playedNote >= currentNote && playedNote < currentNote + visibleNotes)
        return currentNote;
    return std::clamp (playedNote / 12 * 12, 24, 72);
}

// The pad performance UI starts at FL's default C4. Keep its OCT controls
// within C4..C6 without changing older screen-keyboard helper policies or MIDI.
inline int flPadTypingOctaveStep (int currentNote, int direction) noexcept
{
    return std::clamp (currentNote + (direction < 0 ? -12 : direction > 0 ? 12 : 0), 48, 72);
}

struct FlOctaveShortcutContext
{
    bool isFlStudio = false;
    bool isActive = false;
    bool hasHeldKeys = false;
    bool hasUsableWindow = false;
};

// Keep the dispatch policy testable without sending shortcuts into a real DAW.
template <typename PostEvents>
FlOctaveShortcutResult dispatchFlOctaveShortcut (int startNote,
                                                FlOctaveShortcutContext context,
                                                PostEvents&& postEvents,
                                                FlTypingLayout layout = FlTypingLayout::standard)
{
    if (! context.isFlStudio) return FlOctaveShortcutResult::unsupportedHost;
    const auto shortcut = makeFlOctaveShortcut (startNote, layout);
    if (! shortcut) return FlOctaveShortcutResult::unsupportedOctave;
    if (! context.isActive) return FlOctaveShortcutResult::inactiveHost;
    if (context.hasHeldKeys) return FlOctaveShortcutResult::keysHeld;
    if (! context.hasUsableWindow) return FlOctaveShortcutResult::unavailableWindow;
    return postEvents (*shortcut) ? FlOctaveShortcutResult::queued : FlOctaveShortcutResult::failed;
}

bool isFlOctaveShortcutAvailable();
// Message thread only. Queued means posted, not acknowledged by FL Studio.
FlOctaveShortcutResult postFlOctaveShortcut (int startNote, FlTypingLayout layout = FlTypingLayout::standard);
}
