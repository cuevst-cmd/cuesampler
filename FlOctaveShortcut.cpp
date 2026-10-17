#include "FlOctaveShortcut.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#endif

namespace cuesampler
{
bool isFlOctaveShortcutAvailable()
{
   #if JUCE_MAC
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
   #else
    juce::ignoreUnused (startNote, layout);
    return FlOctaveShortcutResult::unsupportedHost;
   #endif
}
}
