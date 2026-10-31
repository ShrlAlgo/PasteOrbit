#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>
#include <functional>

class QObject;

namespace MacIntegration {
quintptr foregroundApplication();
bool isExternalApplication(quintptr process);
quint32 clipboardSequence();
// macOS 不公开剪贴板所属进程，只以复制时的前台应用作为尽力识别的来源。
QString clipboardSource();
QHash<QString, QString> runningApplications();
bool revealFile(const QString &path);
bool animationsEnabled();
void preparePanel(quintptr view);
void activateApplication(quintptr view = 0);
bool canUpdateApplication();
bool startUpdate(const QString &imagePath, const QString &version, const QString &failureMessage);
void installDockReopenHandler(QObject *context, std::function<void()> reopen);
void setAlwaysOnTop(quintptr view, bool pinned);
int runPlatformChecks();
}
