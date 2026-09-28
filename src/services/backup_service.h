#pragma once

#include <QString>

class BackupService final {
public:
    // 成功返回空字符串；失败返回已本地化的错误信息。
    static QString exportToFile(const QString &databasePath, const QString &settingsPath,
                                const QString &destinationPath);
    static QString restoreFromFile(const QString &sourcePath, const QString &databasePath,
                                   const QString &settingsPath);
};
