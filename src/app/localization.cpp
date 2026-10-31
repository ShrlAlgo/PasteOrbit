#include "localization.h"

#include <QFile>
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {
QHash<QString, QString> strings;
QString language = QStringLiteral("zh-CN");

class StandardButtonTranslator : public QTranslator {
public:
    bool isEmpty() const override { return false; }

    QString translate(const char *context, const char *sourceText,
                      const char * = nullptr, int = -1) const override {
        if (qstrcmp(context, "QPlatformTheme") != 0) return {};
        const QString source = QString::fromUtf8(sourceText);
        if (source == QStringLiteral("OK")) return strings.value(QStringLiteral("Ok"));
        if (source == QStringLiteral("&Yes")) return strings.value(QStringLiteral("Yes"));
        if (source == QStringLiteral("&No")) return strings.value(QStringLiteral("No"));
        if (source == QStringLiteral("Cancel")) return strings.value(QStringLiteral("Cancel"));
        return {};
    }
};
}

// 界面语言按系统偏好顺序匹配，不能用地区格式推断；未支持的语言回退到英文。
QString AppLocalization::resolveLanguage(const QString &requestedLanguage) {
    if (requestedLanguage == QStringLiteral("zh-CN") || requestedLanguage == QStringLiteral("en-US"))
        return requestedLanguage;
    for (const auto &preferred : QLocale::system().uiLanguages()) {
        const auto candidate = QLocale(preferred).language();
        if (candidate == QLocale::Chinese) return QStringLiteral("zh-CN");
        if (candidate == QLocale::English) return QStringLiteral("en-US");
    }
    return QStringLiteral("en-US");
}

void AppLocalization::load(const QString &requestedLanguage) {
    language = resolveLanguage(requestedLanguage);
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
    // Qt 标准按钮从 QPlatformTheme 翻译；切换语言时重新安装以通知现有控件刷新。
    static StandardButtonTranslator buttonTranslator;
    QCoreApplication::removeTranslator(&buttonTranslator);
    QCoreApplication::installTranslator(&buttonTranslator);
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
