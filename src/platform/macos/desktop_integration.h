#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

namespace MacIntegration {
quintptr foregroundApplication();
bool isExternalApplication(quintptr process);
quint32 clipboardSequence();
// macOS 不公开剪贴板所属进程，只以复制时的前台应用作为尽力识别的来源。
QString clipboardSource();
QHash<QString, QString> runningApplications();
bool revealFile(const QString &path);
bool animationsEnabled();
void activateApplication();
void setAlwaysOnTop(quintptr view, bool pinned);
int runPlatformChecks();
}
