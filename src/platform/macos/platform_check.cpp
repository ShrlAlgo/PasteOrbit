#include "desktop_integration.h"
#include "data_protection.h"
#include "app_settings.h"
#include "history_store.h"
#include "backup_service.h"
#include "localization.h"

#include <QDebug>
#include <QDir>
#include <QFile>
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
