#include "desktop_integration.h"
#include "data_protection.h"
#include "app_settings.h"
#include "history_store.h"
#include "backup_service.h"
#include "localization.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace MacIntegration {
int runPlatformChecks() {
    // 仅在显式自检时运行；所有历史、备份和设置写入当前构建目录下的临时目录。
    AppLocalization::load(QStringLiteral("en-US"));
    const auto check = [](bool passed, const char *name) {
        if (!passed) qCritical() << "FAIL:" << name;
        return passed;
    };
    QTemporaryDir temporary(QDir::current().filePath(QStringLiteral("platform-check-XXXXXX")));
    if (!check(temporary.isValid(), "temporary directory")) return 1;
    const QByteArray content = QByteArray("PasteOrbit platform roundtrip\0", 29) + QByteArray(200000, 'x');
    const auto first = DataProtection::protect(content);
    const auto second = DataProtection::protect(content);
    if (!check(first && second && *first != *second && DataProtection::unprotect(*first) == content,
               "keychain encryption and random IV")) return 1;
    auto damaged = *first;
    damaged[20] = static_cast<char>(damaged[20] ^ 1);
    if (!check(!DataProtection::unprotect(damaged) && !DataProtection::unprotect(first->first(10))
               && !DataProtection::unprotect(QByteArray("foreign-format")), "tamper and format rejection")) return 1;
    const auto empty = DataProtection::protect({});
    if (!check(empty && DataProtection::unprotect(*empty) == QByteArray{}, "empty plaintext")) return 1;

    const QString database = temporary.filePath(QStringLiteral("history.db"));
    const QString settingsPath = temporary.filePath(QStringLiteral("settings.json"));
    const QString backupPath = temporary.filePath(QStringLiteral("history.pasteorbit"));
    const HistoryStore store(database);
    ClipboardCapture capture;
    capture.searchText = QStringLiteral("macOS platform check");
    capture.content = content;
    HistoryItem item;
    if (!check(store.initialize().isEmpty() && store.save(capture, &item).isEmpty()
               && store.content(item.id) == content, "encrypted history storage")) return 1;
    AppSettings settings;
    if (!check(AppSettings::shortcutDisplayText(QStringLiteral("Ctrl + O")) == QStringLiteral("⌘O")
               && AppSettings::shortcutDisplayText(QStringLiteral("Alt + V")) == QStringLiteral("⌥V")
               && AppSettings::shortcutDisplayText(QStringLiteral("Meta + O")) == QStringLiteral("⌃O")
               && AppSettings::shortcutDisplayText(QStringLiteral("Enter")) == QStringLiteral("\u21B5"),
               "native shortcut symbols")) return 1;
    if (!check(AppLocalization::resolveLanguage(QStringLiteral("en-US")) == QStringLiteral("en-US")
               && AppLocalization::resolveLanguage(QStringLiteral("zh-CN")) == QStringLiteral("zh-CN")
               && QStringList{QStringLiteral("en-US"), QStringLiteral("zh-CN")}
                   .contains(AppLocalization::resolveLanguage()), "interface language selection")) return 1;
    const struct { const char *key; const QString *value; } bindings[]{
        {"GlobalHotKey", &settings.globalHotKey}, {"SequenceCopyShortcut", &settings.sequenceCopyShortcut},
        {"SequencePasteShortcut", &settings.sequencePasteShortcut}, {"PasteShortcut", &settings.pasteShortcut},
        {"PlainTextPasteShortcut", &settings.plainTextPasteShortcut}, {"PreviewShortcut", &settings.previewShortcut},
        {"PinShortcut", &settings.pinShortcut}, {"PasteAsFileShortcut", &settings.pasteAsFileShortcut},
        {"MoreShortcut", &settings.moreShortcut}, {"OpenShortcut", &settings.openShortcut}};
    for (const auto &binding : bindings)
        if (!check(!settings.shortcutConflicts(*binding.value, QString::fromLatin1(binding.key)),
                   "default shortcuts do not conflict")) return 1;
    if (!check(settings.shortcutConflicts(QStringLiteral("Ctrl+Shift+V"), QStringLiteral("GlobalHotKey"))
               && settings.shortcutConflicts(QStringLiteral("Ctrl+V"), QStringLiteral("SequencePasteShortcut"))
               && settings.shortcutConflicts(QStringLiteral("Meta+Space"), QStringLiteral("GlobalHotKey"))
               && settings.shortcutConflicts(QStringLiteral("Ctrl+Q"), QStringLiteral("PinShortcut")),
               "system and editing shortcuts reserved")) return 1;
    const auto writeSettings = [&settingsPath](const QJsonObject &json) {
        QFile file(settingsPath);
        const auto bytes = QJsonDocument(json).toJson();
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    QJsonObject legacy{{"GlobalHotKey", "Ctrl + Shift + V"}, {"SequenceCopyShortcut", "Alt + C"},
        {"SequencePasteShortcut", "Alt + V"}, {"MoreShortcut", "Shift + F10"}};
    if (!check(writeSettings(legacy), "legacy shortcut fixture")) return 1;
    const auto migrated = AppSettings::load(settingsPath);
    if (!check(migrated.globalHotKey == settings.globalHotKey
               && migrated.sequenceCopyShortcut == settings.sequenceCopyShortcut
               && migrated.sequencePasteShortcut == settings.sequencePasteShortcut
               && migrated.moreShortcut == settings.moreShortcut, "legacy defaults migrated")) return 1;
    legacy.insert(QStringLiteral("GlobalHotKey"), QStringLiteral("Meta+Alt+P"));
    if (!check(writeSettings(legacy)
               && AppSettings::load(settingsPath).globalHotKey == QStringLiteral("Meta+Alt+P"),
               "custom shortcut preserved")) return 1;
    legacy.insert(QStringLiteral("GlobalHotKey"), QStringLiteral("Ctrl+Shift+V"));
    legacy.insert(QStringLiteral("PinShortcut"), settings.globalHotKey);
    if (!check(writeSettings(legacy)
               && AppSettings::normalizedShortcut(AppSettings::load(settingsPath).globalHotKey)
                   == AppSettings::normalizedShortcut(QStringLiteral("Ctrl+Shift+V")),
               "migration does not overwrite another binding")) return 1;
    legacy.remove(QStringLiteral("PinShortcut"));
    legacy.insert(QStringLiteral("GlobalHotKey"), QStringLiteral("Meta+Shift+Space"));
    legacy.insert(QStringLiteral("MacShortcutDefaultsVersion"), 1);
    if (!check(writeSettings(legacy)
               && AppSettings::load(settingsPath).globalHotKey == settings.globalHotKey,
               "handwriting shortcut migrated")) return 1;
    legacy.insert(QStringLiteral("GlobalHotKey"), QStringLiteral("Ctrl+Shift+V"));
    legacy.insert(QStringLiteral("MacShortcutDefaultsVersion"), 2);
    if (!check(writeSettings(legacy)
               && AppSettings::load(settingsPath).globalHotKey == QStringLiteral("Ctrl+Shift+V"),
               "migration runs only once")) return 1;
    if (!check(settings.save(settingsPath)
               && AppSettings::load(settingsPath).globalHotKey == settings.globalHotKey
               && AppSettings::load(settingsPath).sequencePasteShortcut == settings.sequencePasteShortcut,
               "portable shortcut persistence")) return 1;
    settings.maxHistoryEntries = 1234;
    if (!check(settings.save(settingsPath)
               && BackupService::exportToFile(database, settingsPath, backupPath).isEmpty(), "backup export")) return 1;
    if (!check(store.remove(item.id).isEmpty()
               && BackupService::restoreFromFile(backupPath, database, settingsPath).isEmpty()
               && store.content(item.id) == content && AppSettings::load(settingsPath).maxHistoryEntries == 1234,
               "backup restore")) return 1;
    QFile backup(backupPath);
    if (!check(backup.open(QIODevice::ReadWrite) && backup.seek(backup.size() - 1), "backup tamper setup")) return 1;
    char last = 0;
    if (!check(backup.read(&last, 1) == 1 && backup.seek(backup.size() - 1), "backup tag read")) return 1;
    last ^= 1;
    if (!check(backup.write(&last, 1) == 1, "backup tag write")) return 1;
    backup.close();
    if (!check(!BackupService::restoreFromFile(backupPath, database, settingsPath).isEmpty()
               && store.content(item.id) == content, "corrupt backup preserves current history")) return 1;
    qInfo() << "PASS: encryption, storage, backup and tamper checks";
    return 0;
}
}
