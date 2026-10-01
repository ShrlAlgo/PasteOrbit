#pragma once

#include <QString>

struct AppSettings {
    QString language;
    QString globalHotKey = QStringLiteral("Alt + V");
    bool interceptWindowsClipboardShortcut = false;
    QString pasteShortcut = QStringLiteral("Enter");
    QString plainTextPasteShortcut = QStringLiteral("Shift + Enter");
    QString previewShortcut = QStringLiteral("Space");
    QString pinShortcut = QStringLiteral("Ctrl + P");
    QString pasteAsFileShortcut = QStringLiteral("Ctrl + Shift + S");
    bool startWithWindows = false;
    bool runAsAdministrator = false;
    bool autoHideOnDeactivate = true;
    bool monitorText = true;
    bool monitorImages = true;
    bool monitorFiles = true;
    QString excludedApplications = QStringLiteral("1Password; Bitwarden; KeePass; KeePassXC; mstsc; msrdc; Windows365");
    QString themeMode = QStringLiteral("System");
    int retentionDays = 30;
    int maxHistoryEntries = 5000;
    QString skippedUpdateVersion;

    static AppSettings load(const QString &path);
    bool save(const QString &path) const;
    bool applyWindowsStartup() const;
};
