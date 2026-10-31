#pragma once

#include <QString>

struct AppSettings {
    QString language;
    QString globalHotKey = QStringLiteral("Ctrl + Shift + V");
    QString sequenceCopyShortcut = QStringLiteral("Alt + C");
    QString sequencePasteShortcut = QStringLiteral("Alt + V");
    bool interceptWindowsClipboardShortcut = false;
    QString pasteShortcut = QStringLiteral("Enter");
    QString plainTextPasteShortcut = QStringLiteral("Shift + Enter");
    QString previewShortcut = QStringLiteral("Space");
    QString pinShortcut = QStringLiteral("Ctrl + D");
    QString pasteAsFileShortcut = QStringLiteral("Ctrl + Shift + S");
    QString moreShortcut = QStringLiteral("Shift + F10");
    QString openShortcut = QStringLiteral("Ctrl + O");
    bool startWithWindows = false;
    bool runAsAdministrator = false;
    bool monitorText = true;
    bool normalizeUnicodeText = false;
    bool monitorImages = true;
    bool monitorFiles = true;
    QString excludedApplications = QStringLiteral("1Password; Bitwarden; KeePass; KeePassXC; mstsc; msrdc; Windows365");
    QString themeMode = QStringLiteral("System");
    int retentionDays = 30;
    int maxHistoryEntries = 5000;
    QString skippedUpdateVersion;

    static AppSettings load(const QString &path);
    static QString normalizedShortcut(QString value);
    bool shortcutConflicts(const QString &value, const QString &bindingKey) const;
    bool save(const QString &path) const;
    // 沿用已有接口名称：Windows 注册启动项，macOS 写入当前用户的 LaunchAgent。
    bool applyWindowsStartup() const;
};
