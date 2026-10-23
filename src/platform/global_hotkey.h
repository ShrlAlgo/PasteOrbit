#pragma once

#include <QString>
#include <QtGlobal>

namespace GlobalHotkey {
// 替换指定窗口与 ID 的注册；失败时该 ID 不保留旧快捷键。
bool replace(quintptr window, int id, const QString &shortcut);
void remove(quintptr window, int id);
}
