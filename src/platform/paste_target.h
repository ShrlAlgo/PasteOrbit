#pragma once

#include <QRect>
#include <QtGlobal>
#include <optional>

namespace PasteTarget {
// 窗口标识仅用于回传平台实现，界面层不解引用。
struct Snapshot {
    quintptr window = 0;
    quintptr focus = 0;
    std::optional<QRect> caret;
};

enum class Result { WaitingForModifiers, Sent, Failed };
inline constexpr quintptr InputMarker = 0x504F5041;

// 忽略本应用窗口；有效目标即使无法读取输入焦点也返回快照。
std::optional<Snapshot> capture(quintptr window);
bool prepare(quintptr window, bool keepFocus);
// 单次尝试，不等待按键释放；重试时限与取消由调用方管理。
Result send(quintptr window, quintptr focus, bool keepFocus, bool directText);
}
