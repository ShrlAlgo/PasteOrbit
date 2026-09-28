#include "localization.h"

#include <QFile>
#include <QLocale>
#include <QXmlStreamReader>

namespace {
QHash<QString, QString> strings;
QString language = QStringLiteral("zh-CN");
}

void AppLocalization::load(const QString &requestedLanguage) {
    language = requestedLanguage == QStringLiteral("en-US") ? requestedLanguage : QStringLiteral("zh-CN");
    strings.clear();
    QFile file(QStringLiteral(":/Strings/%1/Resources.resw").arg(language));
    if (!file.open(QIODevice::ReadOnly)) return;
    QXmlStreamReader reader(&file);
    QString key;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QStringLiteral("data")) {
            key = reader.attributes().value(QStringLiteral("name")).toString();
        } else if (reader.isStartElement() && reader.name() == QStringLiteral("value") && !key.isEmpty()) {
            strings.insert(key, reader.readElementText());
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("data")) {
            key.clear();
        }
    }
}

QString AppLocalization::get(const QString &key) {
    const auto value = strings.value(key);
    return value.isEmpty() ? key : value;
}

QString AppLocalization::format(const QString &key, const QStringList &values) {
    QString value = get(key);
    for (qsizetype i = 0; i < values.size(); ++i) {
        value.replace(QStringLiteral("{%1}").arg(i), values.at(i));
    }
    return value;
}

QString AppLocalization::activeLanguage() {
    return language;
}
