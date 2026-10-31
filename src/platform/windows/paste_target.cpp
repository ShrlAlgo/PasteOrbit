#include "paste_target.h"

#include <QString>
#include <windows.h>

namespace PasteTarget {

std::optional<Snapshot> capture(quintptr windowId) {
    const auto window = reinterpret_cast<HWND>(windowId);
    DWORD process = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    if (!thread || process == GetCurrentProcessId()) return std::nullopt;
    Snapshot result;
    result.window = windowId;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread, &info)) return result;
    if (info.hwndFocus && (info.hwndFocus == window || IsChild(window, info.hwndFocus)))
        result.focus = reinterpret_cast<quintptr>(info.hwndFocus);
    if (info.hwndCaret) {
        RECT caret = info.rcCaret;
        MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT *>(&caret), 2);
        result.caret = QRect(caret.left, caret.top, caret.right - caret.left, caret.bottom - caret.top);
    }
    return result;
}

bool prepare(quintptr windowId, bool keepFocus) {
    const auto window = reinterpret_cast<HWND>(windowId);
    if (!window || !IsWindow(window)) return false;
    // 只还原最小化窗口，保留目标原有的最大化状态。
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    if (!keepFocus) SetForegroundWindow(window);
    return true;
}

Result send(quintptr windowId, quintptr focusId, bool keepFocus, bool directText, bool copy) {
    const auto window = reinterpret_cast<HWND>(windowId);
    const auto focus = reinterpret_cast<HWND>(focusId);
    if (!IsWindow(window) || GetForegroundWindow() != window) return Result::Failed;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000 || GetAsyncKeyState(VK_MENU) & 0x8000
        || GetAsyncKeyState(VK_SHIFT) & 0x8000 || GetAsyncKeyState(VK_LWIN) & 0x8000
        || GetAsyncKeyState(VK_RWIN) & 0x8000) return Result::WaitingForModifiers;

    // 仅在主动激活后恢复输入控件，非激活浏览保持目标已有焦点。
    if (focus && !keepFocus) {
        if (!IsWindow(focus) || (focus != window && !IsChild(window, focus))) return Result::Failed;
        const DWORD targetThread = GetWindowThreadProcessId(focus, nullptr);
        const DWORD currentThread = GetCurrentThreadId();
        const bool attached = targetThread && targetThread != currentThread
            && AttachThreadInput(currentThread, targetThread, TRUE);
        if (attached || targetThread == currentThread) SetFocus(focus);
        if (attached) AttachThreadInput(currentThread, targetThread, FALSE);
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(targetThread, &info) || info.hwndFocus != focus
            || GetForegroundWindow() != window) return Result::Failed;
    }
    wchar_t controlClass[32]{};
    const bool nativeEdit = !copy && directText && focus && GetClassNameW(focus, controlClass, 32)
        && QString::fromWCharArray(controlClass).compare(QStringLiteral("Edit"), Qt::CaseInsensitive) == 0;
    if (nativeEdit) {
        // 原生 Edit 直接接收粘贴消息，避免模拟按键被目标转发到其他区域。
        DWORD_PTR ignored = 0;
        return SendMessageTimeoutW(focus, WM_PASTE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)
            ? Result::Sent : Result::Failed;
    }
    INPUT inputs[4]{};
    const WORD key = copy ? 'C' : 'V';
    const WORD keys[4]{VK_CONTROL, key, key, VK_CONTROL};
    for (int i = 0; i < 4; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys[i];
        inputs[i].ki.dwFlags = (i >= 2) ? KEYEVENTF_KEYUP : 0;
        // 与键盘钩子共用标记，只忽略本应用注入的按键。
        inputs[i].ki.dwExtraInfo = InputMarker;
    }
    return SendInput(4, inputs, sizeof(INPUT)) == 4 ? Result::Sent : Result::Failed;
}

}
