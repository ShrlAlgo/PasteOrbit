#include "paste_target.h"
#include "desktop_integration.h"

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

namespace PasteTarget {
std::optional<Snapshot> capture(quintptr process) {
    if (!MacIntegration::isExternalApplication(process)) return std::nullopt;
    @autoreleasepool {
        if (![NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(process)])
            return std::nullopt;
    }
    Snapshot result{process, 0, std::nullopt};
    // 未授权时直接回退到鼠标定位；捕获目标本身不触发权限提示。
    if (!AXIsProcessTrusted()) return result;
    auto app = AXUIElementCreateApplication(static_cast<pid_t>(process));
    AXUIElementSetMessagingTimeout(app, 0.05f);
    CFTypeRef focused = nullptr;
    if (AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused) == kAXErrorSuccess
        && focused && CFGetTypeID(focused) == AXUIElementGetTypeID()) {
        AXUIElementSetMessagingTimeout(static_cast<AXUIElementRef>(focused), 0.05f);
        CFTypeRef range = nullptr;
        if (AXUIElementCopyAttributeValue(static_cast<AXUIElementRef>(focused),
                                         kAXSelectedTextRangeAttribute, &range) == kAXErrorSuccess && range) {
            CFTypeRef bounds = nullptr;
            if (AXUIElementCopyParameterizedAttributeValue(static_cast<AXUIElementRef>(focused),
                    kAXBoundsForRangeParameterizedAttribute, range, &bounds) == kAXErrorSuccess && bounds) {
                CGRect rect{};
                if (CFGetTypeID(bounds) == AXValueGetTypeID()
                    && AXValueGetValue(static_cast<AXValueRef>(bounds), kAXValueTypeCGRect, &rect)
                    && rect.size.height > 0)
                    result.caret = QRect(qRound(rect.origin.x), qRound(rect.origin.y),
                                         qMax(1, qRound(rect.size.width)), qRound(rect.size.height));
                CFRelease(bounds);
            }
            CFRelease(range);
        }
    }
    if (focused) CFRelease(focused);
    CFRelease(app);
    return result;
}

bool prepare(quintptr process, bool) {
    if (!MacIntegration::isExternalApplication(process)) return false;
    @autoreleasepool {
        // 只在用户选择粘贴后请求辅助功能权限；拒绝时保留剪贴板供手动粘贴。
        NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) return false;
        auto *app = [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(process)];
        return app && [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    }
}

Result send(quintptr process, quintptr, bool, bool, bool copy) {
    if (!AXIsProcessTrusted() || MacIntegration::foregroundApplication() != process) return Result::Failed;
    const auto flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    if (flags & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate | kCGEventFlagMaskShift))
        return Result::WaitingForModifiers;
    auto down = CGEventCreateKeyboardEvent(nullptr, copy ? kVK_ANSI_C : kVK_ANSI_V, true);
    auto up = CGEventCreateKeyboardEvent(nullptr, copy ? kVK_ANSI_C : kVK_ANSI_V, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return Result::Failed;
    }
    CGEventSetFlags(down, kCGEventFlagMaskCommand);
    CGEventSetFlags(up, kCGEventFlagMaskCommand);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return Result::Sent;
}
}
