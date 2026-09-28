#pragma once

#include <QString>
#include <QStringList>

class AppLocalization {
public:
    static void load(const QString &language);
    static QString get(const QString &key);
    static QString format(const QString &key, const QStringList &values);
    static QString activeLanguage();
};
