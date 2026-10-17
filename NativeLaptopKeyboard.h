#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

namespace juce { class Component; }

namespace cuesampler
{
// Message-thread state, independent of Cocoa so physical-key pairing can be
// tested without posting events into a host. Logical keys are resolved once,
// on key-down; changing layouts or modifiers cannot change their key-up pair.
class LaptopPhysicalKeys
{
public:
    template <typename OnDown>
    bool down (int physicalKey, int logicalKey, bool repeat, OnDown&& onDown)
    {
        if (! valid (physicalKey)) return false;
        auto& key = keys[(size_t) physicalKey];
        if (key.owned) return true;
        if (repeat || logicalKey < 0 || logicalKey > 127) return false;

        bool duplicate = false;
        for (const auto& other : keys)
            duplicate |= other.owned && ! other.released && other.logical == logicalKey;
        key = { logicalKey, true, false };
        // Store ownership before invoking user code: a synchronous close or
        // release from onDown must still find the newly started pair.
        if (! duplicate && ! onDown (logicalKey)) { key = {}; return false; }
        return true;
    }

    template <typename OnUp>
    bool up (int physicalKey, OnUp&& onUp)
    {
        if (! owns (physicalKey)) return false;
        const auto key = keys[(size_t) physicalKey];
        keys[(size_t) physicalKey] = {};
        // A blocked key still needs its logical release to clear the input
        // model's repeat suppression. Duplicate physical keys share one pair.
        bool duplicate = false;
        for (const auto& other : keys)
            duplicate |= other.owned && other.logical == key.logical;
        if (! duplicate) onUp (key.logical);
        return true;
    }

    template <typename OnReleaseAll>
    void releaseAll (OnReleaseAll&& onReleaseAll)
    {
        bool playing = false;
        for (auto& key : keys)
            if (key.owned && ! key.released)
            {
                playing = true;
                key.released = true;
            }
        if (playing) onReleaseAll();
    }

    template <typename IsDown, typename OnUp>
    void reconcile (IsDown&& isDown, OnUp&& onUp)
    {
        for (size_t physical = 0; physical < keys.size(); ++physical)
            if (keys[physical].owned && ! isDown ((int) physical))
                up ((int) physical, onUp);
    }

    bool owns (int physicalKey) const noexcept
    {
        return valid (physicalKey) && keys[(size_t) physicalKey].owned;
    }

private:
    static bool valid (int physicalKey) noexcept { return physicalKey >= 0 && physicalKey < 128; }
    struct Key { int logical = -1; bool owned = false, released = false; };
    std::array<Key, 128> keys {};
};

// Local AppKit event capture only: no global tap, helper, Accessibility access,
// host preferences, or MIDI device. All callbacks run on the message thread.
// The caller supplies its own note delivery/recording path.
class NativeLaptopKeyboard
{
public:
    NativeLaptopKeyboard (juce::Component& owner,
                          std::function<bool()> mayPlay,
                          std::function<bool (int logicalKey)> onDown,
                          std::function<void (int logicalKey)> onUp,
                          std::function<void()> onReleaseAll);
    ~NativeLaptopKeyboard();

    bool isValid() const;
    // Poll from the editor's message-thread timer. Physical state repairs ups
    // missed during AppKit's nested menu/control tracking loops.
    void reconcile();
    // End playing notes, but swallow/block their repeats until physical up.
    void releaseAll();

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    NativeLaptopKeyboard (const NativeLaptopKeyboard&) = delete;
    NativeLaptopKeyboard& operator= (const NativeLaptopKeyboard&) = delete;
};
}
