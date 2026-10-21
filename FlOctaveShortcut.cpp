#include "FlOctaveShortcut.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#elif JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cuesampler
{
#if JUCE_WINDOWS
namespace
{
bool belongsToCurrentProcess (HWND window)
{
    DWORD process = 0;
    return window != nullptr && GetWindowThreadProcessId (window, &process) != 0
           && process == GetCurrentProcessId();
}

bool isNativeTextInput (HWND window)
{
    wchar_t className[256] {};
    if (window == nullptr || GetClassNameW (window, className, 256) == 0) return false;
    const auto name = juce::String (className).toLowerCase();
    // Match native/VCL edit controls without treating plugin "Editor"
    // wrapper windows as text fields. RichEdit class names include versions.
    return name.endsWith ("edit") || name.startsWith ("richedit")
           || name.endsWith ("memo") || name.endsWith ("combobox");
}

HWND usableFlWindow()
{
    const auto foreground = GetForegroundWindow();
    if (! belongsToCurrentProcess (foreground)
        || GetWindowThreadProcessId (foreground, nullptr) != GetCurrentThreadId()) return nullptr;
    auto target = GetAncestor (foreground, GA_ROOT);
    // VCL applications may have a hidden application HWND above their main
    // form. Choose the outermost visible owner, keeping disabled owners so a
    // native modal dialog causes the enabled check below to reject the request.
    for (auto owner = GetWindow (target, GW_OWNER); owner != nullptr; owner = GetWindow (owner, GW_OWNER))
        if (belongsToCurrentProcess (owner) && IsWindowVisible (owner)) target = owner;
    // SetKeyboardState is local to this thread. Never attach another input
    // queue or dispatch into another process (including a bridged plugin).
    if (! belongsToCurrentProcess (target)
        || GetWindowThreadProcessId (target, nullptr) != GetCurrentThreadId()
        || ! IsWindowVisible (target) || ! IsWindowEnabled (target)
        || ! IsWindowEnabled (foreground)
        || juce::Component::getCurrentlyModalComponent() != nullptr)
        return nullptr;

    GUITHREADINFO info {};
    info.cbSize = sizeof (info);
    if (! GetGUIThreadInfo (GetCurrentThreadId(), &info)
        || info.hwndFocus == nullptr || ! belongsToCurrentProcess (info.hwndFocus)
        // JUCE creates this caret only for an active TextInputTarget, and
        // destroys it when text input is dismissed or its peer loses focus.
        || info.hwndCaret != nullptr
        || (info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0)
        return nullptr;
    for (auto window = info.hwndFocus; window != nullptr; window = GetParent (window))
        if (isNativeTextInput (window)) return nullptr;
    for (auto* focus = juce::Component::getCurrentlyFocusedComponent(); focus != nullptr; focus = focus->getParentComponent())
        if (dynamic_cast<juce::TextEditor*> (focus) != nullptr) return nullptr;
    return target;
}

bool hasHeldKeyboardKeys()
{
    // Ignore mouse buttons so a button's mouse-up callback can request OCT.
    // Include modifiers and every typing key: FL owns their note-off pairs.
    for (int key = VK_BACK; key < 256; ++key)
        if ((GetAsyncKeyState (key) & 0x8000) != 0) return true;
    return false;
}

bool sendWindowsOctaveShortcut (HWND target, FlOctaveShortcut shortcut)
{
    struct ScopedKeyboardState
    {
        std::array<BYTE, 256> original {};
        bool valid = GetKeyboardState (original.data()) != FALSE;
        ~ScopedKeyboardState() { if (valid) SetKeyboardState (original.data()); }
    } keyboard;
    if (! keyboard.valid || target != usableFlWindow() || hasHeldKeyboardKeys()) return false;
    auto state = keyboard.original;
    for (const auto event : makeFlWindowsOctaveKeyEvents (shortcut))
    {
        state[event.virtualKey] = (BYTE) ((state[event.virtualKey] & 0x7f) | (event.down ? 0x80 : 0));
        if (event.virtualKey == VK_MENU) state[VK_LMENU] = state[VK_MENU];
        if (! SetKeyboardState (state.data())) return false;
        const auto scan = MapVirtualKeyW (event.virtualKey, MAPVK_VK_TO_VSC) & 0xffu;
        const auto flags = 1u | (scan << 16) | (event.altContext ? 1u << 29 : 0u)
                           | (event.down ? 0u : (1u << 30) | (1u << 31));
        // Synchronous, same-thread delivery keeps temporary modifier state
        // scoped to the host handler. No global keyboard input is injected.
        SendMessageW (target, event.down ? WM_SYSKEYDOWN : WM_SYSKEYUP,
                      (WPARAM) event.virtualKey, (LPARAM) flags);
        if (! IsWindow (target)) return false;
    }
    return true;
}
}
#endif

bool isFlOctaveShortcutAvailable()
{
   #if JUCE_MAC || JUCE_WINDOWS
    return juce::PluginHostType().isFruityLoops();
   #else
    return false;
   #endif
}

FlOctaveShortcutResult postFlOctaveShortcut (int startNote, FlTypingLayout layout)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
   #if JUCE_MAC
    @autoreleasepool
    {
        // Post directly to this application's queue, never the global keyboard
        // stream. No helper process or Accessibility permission is required.
        // FL receives the same absolute shortcut as Opt+numeric-keypad 2..6.
        if (! isFlOctaveShortcutAvailable())
            return FlOctaveShortcutResult::unsupportedHost;
        if (! [NSApp isActive])
            return FlOctaveShortcutResult::inactiveHost;

        auto* window = [NSApp keyWindow];
        bool heldKeys = ([NSEvent modifierFlags] & (NSEventModifierFlagShift | NSEventModifierFlagControl
                                                   | NSEventModifierFlagOption | NSEventModifierFlagCommand)) != 0;
        // Query physical state: FL consumes typing notes before JUCE sees them,
        // so JUCE's per-view key cache cannot detect those held notes. FL must
        // receive the old note-off before changing its typing octave.
        for (CGKeyCode key = 0; key < 128 && ! heldKeys; ++key)
            heldKeys = CGEventSourceKeyState (kCGEventSourceStateHIDSystemState, key);

        bool editingText = [[window firstResponder] isKindOfClass:[NSTextView class]] != NO;
        for (auto* focused = juce::Component::getCurrentlyFocusedComponent(); focused != nullptr; focused = focused->getParentComponent())
            editingText = editingText || dynamic_cast<juce::TextEditor*> (focused) != nullptr;

        return dispatchFlOctaveShortcut (startNote,
            { true, [NSApp isActive] != NO, heldKeys,
              window != nil && ! editingText && [NSApp modalWindow] == nil
                            && [window attachedSheet] == nil && [window sheetParent] == nil
                            && juce::Component::getCurrentlyModalComponent() == nullptr },
            [window] (FlOctaveShortcut shortcut)
            {
                NSString* digit = [NSString stringWithFormat:@"%d", shortcut.octave];
                const auto flags = NSEventModifierFlagOption | NSEventModifierFlagNumericPad;
                const double timestamp = [[NSProcessInfo processInfo] systemUptime];
                NSEvent* optionDown = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint
                    modifierFlags:NSEventModifierFlagOption timestamp:timestamp windowNumber:[window windowNumber]
                    context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:58];
                NSEvent* down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                    modifierFlags:flags timestamp:timestamp windowNumber:[window windowNumber]
                    context:nil characters:digit charactersIgnoringModifiers:digit
                    isARepeat:NO keyCode:shortcut.macKeyCode];
                NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint
                    modifierFlags:flags timestamp:timestamp windowNumber:[window windowNumber]
                    context:nil characters:digit charactersIgnoringModifiers:digit
                    isARepeat:NO keyCode:shortcut.macKeyCode];
                NSEvent* optionUp = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint
                    modifierFlags:0 timestamp:timestamp windowNumber:[window windowNumber]
                    context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:58];
                if (optionDown == nil || down == nil || up == nil || optionUp == nil) return false;
                // Queue the complete local shortcut, including paired Option
                // transitions for hosts that track modifier events separately.
                // Nothing changes the OS's physical/global keyboard state.
                [NSApp postEvent:optionDown atStart:NO];
                [NSApp postEvent:down atStart:NO];
                [NSApp postEvent:up atStart:NO];
                [NSApp postEvent:optionUp atStart:NO];
                return true;
            }, layout);
    }
   #elif JUCE_WINDOWS
    if (! isFlOctaveShortcutAvailable()) return FlOctaveShortcutResult::unsupportedHost;
    if (! belongsToCurrentProcess (GetForegroundWindow())) return FlOctaveShortcutResult::inactiveHost;
    const auto target = usableFlWindow();
    return dispatchFlOctaveShortcut (startNote,
        { true, true, hasHeldKeyboardKeys(), target != nullptr },
        [target] (FlOctaveShortcut shortcut) { return sendWindowsOctaveShortcut (target, shortcut); }, layout);
   #else
    juce::ignoreUnused (startNote, layout);
    return FlOctaveShortcutResult::unsupportedHost;
   #endif
}
}
