#include "global_hotkey.h"

#include <QCoreApplication>
#include <QHash>
#include <QKeySequence>
#include <QTimer>
#include <Carbon/Carbon.h>
#include <utility>
#include <optional>

namespace {
constexpr OSType HotkeySignature = 0x504f4854;
struct Binding {
    quintptr window;
    EventHotKeyRef reference;
    std::function<void()> activated;
    bool pressed = false;
};
QHash<int, Binding> bindings;
EventHandlerRef handler = nullptr;

OSStatus hotkeyPressed(EventHandlerCallRef, EventRef event, void *) {
    EventHotKeyID key{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                          sizeof(key), nullptr, &key) != noErr || key.signature != HotkeySignature)
        return eventNotHandledErr;
    const auto found = bindings.find(static_cast<int>(key.id));
    if (found == bindings.end()) return eventNotHandledErr;
    if (GetEventKind(event) == kEventHotKeyReleased) { found->pressed = false; return noErr; }
    if (found->pressed) return noErr;
    found->pressed = true;
    QTimer::singleShot(0, QCoreApplication::instance(), found->activated);
    return noErr;
}

std::optional<UInt32> keyCode(Qt::Key key) {
    switch (key) {
    case Qt::Key_Space: return kVK_Space;
    case Qt::Key_Return: case Qt::Key_Enter: return kVK_Return;
    case Qt::Key_Tab: return kVK_Tab;
    case Qt::Key_Escape: return kVK_Escape;
    case Qt::Key_Delete: return kVK_ForwardDelete;
    default: break;
    }
    constexpr UInt32 functionKeys[]{kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6,
        kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
        kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20) return functionKeys[key - Qt::Key_F1];
    // 依据当前键盘布局查硬件键码，避免把所有用户都当成美式键盘。
    auto source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source) return std::nullopt;
    auto data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    std::optional<UInt32> result;
    if (data) {
        auto layout = reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data));
        for (UInt16 code = 0; code < 128; ++code) {
            UInt32 dead = 0;
            UniChar chars[4]{};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, code, kUCKeyActionDown, 0, LMGetKbdType(),
                               kUCKeyTranslateNoDeadKeysMask, &dead, 4, &length, chars) == noErr
                && length == 1 && QChar(chars[0]).toUpper().unicode() == static_cast<uint>(key)) {
                result = code;
                break;
            }
        }
    }
    CFRelease(source);
    return result;
}
}

namespace GlobalHotkey {
bool replace(quintptr window, int id, const QString &shortcut, std::function<void()> activated) {
    remove(window, id);
    QString value = shortcut;
    value.remove(QLatin1Char(' '));
    const QKeySequence sequence = QKeySequence::fromString(value, QKeySequence::PortableText);
    if (sequence.count() != 1 || !activated) return false;
    const auto combination = sequence[0];
    const auto code = keyCode(combination.key());
    if (!code) return false;
    UInt32 modifiers = 0;
    const auto flags = combination.keyboardModifiers();
    // Qt 在 macOS 上将 ControlModifier 映射为 Command，MetaModifier 映射为 Control。
    if (flags.testFlag(Qt::ControlModifier)) modifiers |= cmdKey;
    if (flags.testFlag(Qt::MetaModifier)) modifiers |= controlKey;
    if (flags.testFlag(Qt::AltModifier)) modifiers |= optionKey;
    if (flags.testFlag(Qt::ShiftModifier)) modifiers |= shiftKey;
    if (!modifiers) return false;
    if (!handler) {
        const EventTypeSpec types[]{{kEventClassKeyboard, kEventHotKeyPressed},
                                    {kEventClassKeyboard, kEventHotKeyReleased}};
        if (InstallApplicationEventHandler(hotkeyPressed, 2, types, nullptr, &handler) != noErr) return false;
    }
    EventHotKeyRef reference = nullptr;
    const EventHotKeyID key{HotkeySignature, static_cast<UInt32>(id)};
    if (RegisterEventHotKey(*code, modifiers, key, GetApplicationEventTarget(), 0, &reference) != noErr) {
        if (bindings.isEmpty()) { RemoveEventHandler(handler); handler = nullptr; }
        return false;
    }
    bindings.insert(id, Binding{window, reference, std::move(activated)});
    return true;
}

void remove(quintptr window, int id) {
    const auto found = bindings.find(id);
    if (found == bindings.end() || found->window != window) return;
    UnregisterEventHotKey(found->reference);
    bindings.erase(found);
    if (bindings.isEmpty() && handler) { RemoveEventHandler(handler); handler = nullptr; }
}
}
