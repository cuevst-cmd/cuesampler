#include "NativeLaptopKeyboard.h"
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <vector>

namespace cuesampler
{
struct NativeLaptopKeyboard::Impl : juce::ComponentListener,
                                   std::enable_shared_from_this<Impl>
{
    struct Registry;
    static Registry& registry();

    Impl (juce::Component& component, std::function<bool()> allowed,
          std::function<bool (int)> down, std::function<void (int)> up,
          std::function<void()> release)
        : owner (&component), mayPlay (std::move (allowed)), onDown (std::move (down)),
          onUp (std::move (up)), onReleaseAll (std::move (release)) {}

    void attach();
    void shutdown();
    bool valid() const;
    void reconcile();
    void releaseAll()
    {
        physicalKeys.releaseAll ([this] { if (onReleaseAll) onReleaseAll(); });
    }

    NSView* view() const
    {
        if (auto* component = owner.getComponent())
            if (auto* peer = component->getPeer())
                return (NSView*) peer->getNativeHandle();
        return nil;
    }

    bool nativeFocus() const
    {
        auto* nativeView = view();
        auto* responder = [[nativeView window] firstResponder];
        return nativeView != nil && [responder isKindOfClass:[NSView class]]
               && (responder == nativeView || [(NSView*) responder isDescendantOf:nativeView]);
    }

    bool usable (NSEvent* event = nil) const
    {
        auto* component = owner.getComponent();
        auto* nativeView = view();
        auto* window = [nativeView window];
        if (! attached || component == nullptr || ! component->isShowing()
            || ! [NSApp isActive] || window == nil || window != [NSApp keyWindow]
            || (event != nil && [event window] != window)
            || [nativeView isHiddenOrHasHiddenAncestor]
            || [NSApp modalWindow] != nil || [window attachedSheet] != nil || [window sheetParent] != nil
            || juce::Component::getCurrentlyModalComponent() != nullptr
            || component->isCurrentlyBlockedByAnotherModalComponent()) return false;

        auto* responder = [window firstResponder];
        if ([responder isKindOfClass:[NSTextView class]] || [responder isKindOfClass:[NSTextField class]])
            return false;
        // FL may keep a host parent as first responder even after CUE is
        // clicked. That ancestor is safe with our explicit click ownership;
        // an unrelated sibling editor/view is not.
        bool responderInScope = nativeFocus();
        for (NSResponder* ancestor = nativeView; ancestor != nil && ! responderInScope; ancestor = [ancestor nextResponder])
            responderInScope = ancestor == responder;
        if (! responderInScope) return false;
        if ([responder conformsToProtocol:@protocol(NSTextInputClient)]
            && [(id<NSTextInputClient>) responder hasMarkedText]) return false;
        for (auto* focus = juce::Component::getCurrentlyFocusedComponent(); focus != nullptr; focus = focus->getParentComponent())
            if (dynamic_cast<juce::TextEditor*> (focus) != nullptr) return false;
        return mayPlay && mayPlay();
    }

    bool hit (NSEvent* event) const
    {
        auto* component = owner.getComponent();
        auto* nativeView = view();
        auto* window = [nativeView window];
        if (! attached || component == nullptr || ! component->isShowing() || nativeView == nil
            || [nativeView isHiddenOrHasHiddenAncestor] || [event window] != window) return false;
        // A peer may sit beneath host views. Only a hit inside CUE's actual
        // native subtree claims ownership; a host toolbar/titlebar never does.
        auto* content = [window contentView];
        auto* parent = [content superview];
        const auto point = parent != nil ? [parent convertPoint:[event locationInWindow] fromView:nil] : [event locationInWindow];
        auto* hitView = [content hitTest:point]; // AppKit hitTest takes superview coordinates.
        return hitView == nativeView || [hitView isDescendantOf:nativeView];
    }

    void componentVisibilityChanged (juce::Component&) override;
    void componentParentHierarchyChanged (juce::Component&) override;
    void componentBeingDeleted (juce::Component&) override { shutdown(); owner = nullptr; }

    juce::Component::SafePointer<juce::Component> owner;
    std::function<bool()> mayPlay;
    std::function<bool (int)> onDown;
    std::function<void (int)> onUp;
    std::function<void()> onReleaseAll;
    LaptopPhysicalKeys physicalKeys;
    bool attached = false;
};

struct NativeLaptopKeyboard::Impl::Registry
{
    std::vector<std::weak_ptr<Impl>> clients;
    std::weak_ptr<Impl> active;
    id monitor = nil, appResigned = nil, windowResigned = nil;

    std::vector<std::shared_ptr<Impl>> snapshot()
    {
        std::vector<std::shared_ptr<Impl>> result;
        result.reserve (clients.size());
        for (auto& client : clients)
            if (auto strong = client.lock()) result.push_back (std::move (strong));
        return result;
    }

    void releaseActive()
    {
        auto previous = active.lock();
        active.reset();
        if (previous != nullptr) previous->releaseAll();
    }

    void add (const std::shared_ptr<Impl>& client)
    {
        clients.push_back (client);
        if (monitor != nil || NSApp == nil) return;
        auto* self = this;
        monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:
            NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged
            | NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
            handler:^NSEvent* (NSEvent* event) { return self->dispatch (event); }] retain];
        appResigned = [[[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification
            object:nil queue:nil usingBlock:^(NSNotification*) { self->releaseActive(); }] retain];
        windowResigned = [[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
            object:nil queue:nil usingBlock:^(NSNotification* notification)
            {
                if (auto current = self->active.lock(); current != nullptr && [current->view() window] == [notification object])
                    self->releaseActive();
            }] retain];
    }

    void remove (Impl* client)
    {
        if (auto current = active.lock(); current.get() == client) active.reset();
        clients.erase (std::remove_if (clients.begin(), clients.end(), [client] (const auto& weak)
            { auto strong = weak.lock(); return strong == nullptr || strong.get() == client; }), clients.end());
        if (! clients.empty()) return;
        if (monitor != nil) { [NSEvent removeMonitor:monitor]; [monitor release]; monitor = nil; }
        if (appResigned != nil) { [[NSNotificationCenter defaultCenter] removeObserver:appResigned]; [appResigned release]; appResigned = nil; }
        if (windowResigned != nil) { [[NSNotificationCenter defaultCenter] removeObserver:windowResigned]; [windowResigned release]; windowResigned = nil; }
    }

    NSEvent* dispatch (NSEvent* event)
    {
        jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
        const auto type = [event type];
        auto currentClients = snapshot(); // Keep callbacks safe if an editor closes.
        if (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown || type == NSEventTypeOtherMouseDown)
        {
            active.reset();
            // Release before a control starts its nested AppKit tracking loop.
            for (auto& client : currentClients) client->releaseAll();
            for (auto& client : currentClients)
                if (client->hit (event)) { active = client; break; }
            return event;
        }

        if (type == NSEventTypeFlagsChanged)
        {
            if (([event modifierFlags] & (NSEventModifierFlagShift | NSEventModifierFlagControl
                                         | NSEventModifierFlagOption | NSEventModifierFlagCommand)) != 0)
                for (auto& client : currentClients) client->releaseAll();
            return event;
        }

        const auto physical = (int) [event keyCode];
        // An owned pair stays ours even if native focus, modifiers, or the
        // logical keyboard layout changes between its down and up.
        for (auto& client : currentClients)
            if (client->physicalKeys.owns (physical))
            {
                if (type == NSEventTypeKeyUp)
                    client->physicalKeys.up (physical, [&] (int logical) { if (client->onUp) client->onUp (logical); });
                return nil;
            }
        if (type != NSEventTypeKeyDown || [event isARepeat]) return event;
        if (([event modifierFlags] & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption
                                     | NSEventModifierFlagCommand | NSEventModifierFlagNumericPad | NSEventModifierFlagFunction)) != 0)
            return event;

        auto current = active.lock();
        if (current == nullptr || ! current->usable (event))
        {
            releaseActive();
            current.reset();
            for (auto& candidate : currentClients)
                if (candidate->nativeFocus() && candidate->usable (event)) { current = candidate; active = candidate; break; }
        }
        if (current == nullptr) return event;

        NSString* characters = [event charactersIgnoringModifiers];
        int logical = [characters length] == 1 ? (int) [characters characterAtIndex:0] : -1;
        if (logical >= 'a' && logical <= 'z') logical -= 'a' - 'A';
        const bool used = current->physicalKeys.down (physical, logical, false,
            [&] (int key) { return current->onDown && current->onDown (key); });
        // Tab/Escape can transfer focus without a mouse click. Never retain a
        // fallback ownership claim across that transfer.
        if (! used && (logical == '\t' || logical == 27)) releaseActive();
        return used ? nil : event;
    }
};

NativeLaptopKeyboard::Impl::Registry& NativeLaptopKeyboard::Impl::registry()
{
    static Registry instance;
    return instance;
}

void NativeLaptopKeyboard::Impl::attach()
{
    attached = true;
    if (auto* component = owner.getComponent()) component->addComponentListener (this);
    registry().add (shared_from_this());
}

void NativeLaptopKeyboard::Impl::shutdown()
{
    if (! attached) return;
    attached = false;
    if (auto* component = owner.getComponent()) component->removeComponentListener (this);
    registry().remove (this);
    releaseAll();
}

bool NativeLaptopKeyboard::Impl::valid() const { return attached && registry().monitor != nil; }

void NativeLaptopKeyboard::Impl::reconcile()
{
    if (! usable())
    {
        if (auto current = registry().active.lock(); current.get() == this) registry().active.reset();
        releaseAll();
    }
    physicalKeys.reconcile ([] (int physical)
        { return CGEventSourceKeyState (kCGEventSourceStateHIDSystemState, (CGKeyCode) physical); },
        [this] (int logical) { if (onUp) onUp (logical); });
}

void NativeLaptopKeyboard::Impl::componentVisibilityChanged (juce::Component&)
{
    if (auto* component = owner.getComponent(); component == nullptr || ! component->isShowing())
    {
        if (auto current = registry().active.lock(); current.get() == this) registry().active.reset();
        releaseAll();
    }
}

void NativeLaptopKeyboard::Impl::componentParentHierarchyChanged (juce::Component&) { reconcile(); }

NativeLaptopKeyboard::NativeLaptopKeyboard (juce::Component& owner, std::function<bool()> mayPlay,
                                          std::function<bool (int)> onDown, std::function<void (int)> onUp,
                                          std::function<void()> onReleaseAll)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    impl = std::make_shared<Impl> (owner, std::move (mayPlay), std::move (onDown), std::move (onUp), std::move (onReleaseAll));
    impl->attach();
}

NativeLaptopKeyboard::~NativeLaptopKeyboard()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (impl != nullptr) impl->shutdown();
}

bool NativeLaptopKeyboard::isValid() const { return impl != nullptr && impl->valid(); }
void NativeLaptopKeyboard::reconcile()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (impl != nullptr) impl->reconcile();
}
void NativeLaptopKeyboard::releaseAll()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (impl != nullptr) impl->releaseAll();
}
}

#else

namespace cuesampler
{
struct NativeLaptopKeyboard::Impl {};
NativeLaptopKeyboard::NativeLaptopKeyboard (juce::Component&, std::function<bool()>, std::function<bool (int)>,
                                          std::function<void (int)>, std::function<void()>) {}
NativeLaptopKeyboard::~NativeLaptopKeyboard() = default;
bool NativeLaptopKeyboard::isValid() const { return false; }
void NativeLaptopKeyboard::reconcile() {}
void NativeLaptopKeyboard::releaseAll() {}
}

#endif
