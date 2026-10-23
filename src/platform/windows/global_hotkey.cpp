#include "global_hotkey.h"

#include <QStringList>
#include <windows.h>

namespace {
bool parseHotkey(const QString &shortcut, UINT &modifiers, UINT &key) {
    modifiers = MOD_NOREPEAT;
    key = 0;
    const auto parts = shortcut.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (auto part : parts) {
        part = part.trimmed().toLower();
        if (part == QStringLiteral("ctrl") || part == QStringLiteral("control")) modifiers |= MOD_CONTROL;
        else if (part == QStringLiteral("alt")) modifiers |= MOD_ALT;
        else if (part == QStringLiteral("shift")) modifiers |= MOD_SHIFT;
        else if (part == QStringLiteral("win") || part == QStringLiteral("windows")
                 || part == QStringLiteral("meta")) modifiers |= MOD_WIN;
        else if (part.size() == 1) key = static_cast<UINT>(VkKeyScanW(part.at(0).toUpper().unicode()) & 0xFF);
        else if (part.size() >= 2 && part.at(0) == QLatin1Char('f')) {
            bool ok = false;
            const int number = part.mid(1).toInt(&ok);
            if (ok && number >= 1 && number <= 24) key = VK_F1 + number - 1;
        } else if (part == QStringLiteral("space")) key = VK_SPACE;
        else if (part == QStringLiteral("enter") || part == QStringLiteral("return")) key = VK_RETURN;
        else if (part == QStringLiteral("tab")) key = VK_TAB;
        else if (part == QStringLiteral("esc") || part == QStringLiteral("escape")) key = VK_ESCAPE;
        else if (part == QStringLiteral("delete")) key = VK_DELETE;
    }
    return key != 0 && (modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) != 0;
}
}

namespace GlobalHotkey {

bool replace(quintptr window, int id, const QString &shortcut) {
    // 保持设置更新的原有语义：先注销旧组合，再尝试注册新组合。
    remove(window, id);
    UINT modifiers = 0;
    UINT key = 0;
    return parseHotkey(shortcut, modifiers, key)
        && RegisterHotKey(reinterpret_cast<HWND>(window), id, modifiers, key) != FALSE;
}

void remove(quintptr window, int id) {
    UnregisterHotKey(reinterpret_cast<HWND>(window), id);
}

}
