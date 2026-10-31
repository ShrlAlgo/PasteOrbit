#include "app_settings.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QStringList>
#include <QSaveFile>
#include <QSettings>
#ifndef Q_OS_WIN
#include <QXmlStreamWriter>
#endif

bool AppSettings::applyWindowsStartup() const {
#ifdef Q_OS_WIN
    QSettings startup(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                      QSettings::NativeFormat);
    const QString command = startWithWindows
        ? QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
        : QString{};
    // 启动项一致时不再强制同步注册表，避免每次调整普通设置都触发落盘。
    if (startup.value(QStringLiteral("PasteOrbit")).toString() == command)
        return startup.status() == QSettings::NoError;
    if (command.isEmpty()) startup.remove(QStringLiteral("PasteOrbit"));
    else startup.setValue(QStringLiteral("PasteOrbit"), command);
    // 立即落盘并核实结果，避免界面已开启但 Windows 启动项没有生效。
    startup.sync();
    return startup.status() == QSettings::NoError
        && startup.value(QStringLiteral("PasteOrbit")).toString() == command;
#else
    // 保留既有配置字段；macOS 登录项使用当前用户的 LaunchAgent，不需要管理员权限。
    const QString directory = QDir::homePath() + QStringLiteral("/Library/LaunchAgents");
    const QString path = directory + QStringLiteral("/com.pasteorbit.PasteOrbit.plist");
    if (!startWithWindows) return !QFile::exists(path) || QFile::remove(path);
    if (!QDir().mkpath(directory)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QXmlStreamWriter xml(&file);
    xml.writeStartDocument();
    xml.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
    xml.writeStartElement(QStringLiteral("plist"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    xml.writeStartElement(QStringLiteral("dict"));
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("Label"));
    xml.writeTextElement(QStringLiteral("string"), QStringLiteral("com.pasteorbit.PasteOrbit"));
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("ProgramArguments"));
    xml.writeStartElement(QStringLiteral("array"));
    xml.writeTextElement(QStringLiteral("string"), QCoreApplication::applicationFilePath());
    xml.writeEndElement();
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("RunAtLoad"));
    xml.writeEmptyElement(QStringLiteral("true"));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return !xml.hasError() && file.commit();
#endif
}

QString AppSettings::normalizedShortcut(QString value) {
    value.remove(QLatin1Char(' '));
    value.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
    return QKeySequence::fromString(value, QKeySequence::PortableText).toString(QKeySequence::PortableText).toCaseFolded();
}

// 显示使用平台原生符号，保存和匹配仍使用 PortableText，避免改变已有绑定。
QString AppSettings::shortcutDisplayText(QString value) {
#ifdef Q_OS_MACOS
    value.remove(QLatin1Char(' '));
    // 已有绑定把 Return 与 Enter 合并，界面显示为常用的回车键。
    value.replace(QStringLiteral("Enter"), QStringLiteral("Return"), Qt::CaseInsensitive);
    return QKeySequence::fromString(value, QKeySequence::PortableText).toString(QKeySequence::NativeText);
#else
    return value;
#endif
}

bool AppSettings::shortcutConflicts(const QString &value, const QString &bindingKey) const {
    const QString normalized = normalizedShortcut(value);
    if (normalized.isEmpty()) return true;
    const struct { const char *key; const QString *value; } bindings[]{
        {"GlobalHotKey", &globalHotKey}, {"SequenceCopyShortcut", &sequenceCopyShortcut},
        {"SequencePasteShortcut", &sequencePasteShortcut}, {"PasteShortcut", &pasteShortcut},
        {"PlainTextPasteShortcut", &plainTextPasteShortcut}, {"PreviewShortcut", &previewShortcut},
        {"PinShortcut", &pinShortcut}, {"PasteAsFileShortcut", &pasteAsFileShortcut},
        {"MoreShortcut", &moreShortcut}, {"OpenShortcut", &openShortcut}};
    for (const auto &binding : bindings)
        if (bindingKey != QLatin1String(binding.key) && normalizedShortcut(*binding.value) == normalized) return true;
    // 固定导航和搜索先于记录快捷键处理，不能录成无法触发的绑定。
    const QStringList reserved{QStringLiteral("Esc"), QStringLiteral("Tab"), QStringLiteral("Shift+Tab"),
        QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Up"), QStringLiteral("Down"),
        QStringLiteral("Home"), QStringLiteral("End"), QStringLiteral("PgUp"), QStringLiteral("PgDown"),
        QStringLiteral("Delete"), QStringLiteral("Menu"), QStringLiteral("Ctrl+F")};
    for (const auto &shortcut : reserved)
        if (normalizedShortcut(shortcut) == normalized) return true;
    const auto combination = QKeySequence::fromString(value, QKeySequence::PortableText)[0];
    const auto modifiers = combination.keyboardModifiers();
#ifdef Q_OS_MACOS
    // 固定的应用操作和系统切换不能被记录快捷键覆盖；全局操作还应避开常见编辑组合。
    const QStringList systemReserved{QStringLiteral("Ctrl+Q"), QStringLiteral("Ctrl+W"),
        QStringLiteral("Ctrl+H"), QStringLiteral("Ctrl+M"), QStringLiteral("Ctrl+,"),
        QStringLiteral("Ctrl+Tab"), QStringLiteral("Ctrl+Shift+Tab"), QStringLiteral("Ctrl+Space"),
        QStringLiteral("Ctrl+Alt+Space"), QStringLiteral("Meta+Space"), QStringLiteral("Meta+Alt+Space"),
        QStringLiteral("Meta+Ctrl+Q"), QStringLiteral("Ctrl+Alt+Esc")};
    for (const auto &shortcut : systemReserved)
        if (normalizedShortcut(shortcut) == normalized) return true;
    if (bindingKey == QStringLiteral("GlobalHotKey") || bindingKey.startsWith(QStringLiteral("Sequence"))) {
        const QStringList editingReserved{QStringLiteral("Ctrl+C"), QStringLiteral("Ctrl+V"),
            QStringLiteral("Ctrl+X"), QStringLiteral("Ctrl+A"), QStringLiteral("Ctrl+Z"),
            QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Ctrl+Shift+C"), QStringLiteral("Ctrl+Shift+V"),
            QStringLiteral("Ctrl+Alt+C"), QStringLiteral("Ctrl+Alt+V"), QStringLiteral("Ctrl+Alt+Shift+V")};
        for (const auto &shortcut : editingReserved)
            if (normalizedShortcut(shortcut) == normalized) return true;
    }
#endif
    if (bindingKey == QStringLiteral("GlobalHotKey") || bindingKey.startsWith(QStringLiteral("Sequence"))) return modifiers == Qt::NoModifier;
    return
#ifdef Q_OS_WIN
        modifiers.testFlag(Qt::MetaModifier) ||
#endif
        combination.key() == Qt::Key_Escape
        || combination.key() == Qt::Key_Menu || combination.key() == Qt::Key_Up
        || (modifiers == Qt::NoModifier
        && combination.key() >= Qt::Key_1 && combination.key() <= Qt::Key_9);
}

AppSettings AppSettings::load(const QString &path) {
    AppSettings settings;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return settings;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return settings;
    const auto json = document.object();
    settings.language = json.value(QStringLiteral("Language")).toString();
    if (settings.language != QStringLiteral("zh-CN") && settings.language != QStringLiteral("en-US")) settings.language.clear();
    settings.globalHotKey = json.value(QStringLiteral("GlobalHotKey")).toString(settings.globalHotKey);
    settings.sequenceCopyShortcut = json.value(QStringLiteral("SequenceCopyShortcut")).toString(settings.sequenceCopyShortcut);
    settings.sequencePasteShortcut = json.value(QStringLiteral("SequencePasteShortcut")).toString(settings.sequencePasteShortcut);
    settings.interceptWindowsClipboardShortcut = json.value(QStringLiteral("InterceptWindowsClipboardShortcut")).toBool();
    settings.pasteShortcut = json.value(QStringLiteral("PasteShortcut")).toString(settings.pasteShortcut);
    settings.plainTextPasteShortcut = json.value(QStringLiteral("PlainTextPasteShortcut")).toString(settings.plainTextPasteShortcut);
    settings.previewShortcut = json.value(QStringLiteral("PreviewShortcut")).toString(settings.previewShortcut);
    settings.pinShortcut = json.value(QStringLiteral("PinShortcut")).toString(settings.pinShortcut);
    settings.pasteAsFileShortcut = json.value(QStringLiteral("PasteAsFileShortcut")).toString(settings.pasteAsFileShortcut);
    settings.moreShortcut = json.value(QStringLiteral("MoreShortcut")).toString(settings.moreShortcut);
    settings.openShortcut = json.value(QStringLiteral("OpenShortcut")).toString(settings.openShortcut);
#ifdef Q_OS_MACOS
    // 只迁移旧版默认值一次；自定义组合以及迁移后用户重新选择的组合保持不变。
    if (json.value(QStringLiteral("MacShortcutDefaultsVersion")).toInt() < 1) {
        const AppSettings defaults;
        const auto migrate = [&settings](QString &value, const QString &oldValue,
                                         const QString &replacement, const QString &key) {
            if (normalizedShortcut(value) == normalizedShortcut(oldValue)
                && !settings.shortcutConflicts(replacement, key)) value = replacement;
        };
        migrate(settings.globalHotKey, QStringLiteral("Ctrl+Shift+V"), defaults.globalHotKey, QStringLiteral("GlobalHotKey"));
        migrate(settings.sequenceCopyShortcut, QStringLiteral("Alt+C"), defaults.sequenceCopyShortcut, QStringLiteral("SequenceCopyShortcut"));
        migrate(settings.sequencePasteShortcut, QStringLiteral("Alt+V"), defaults.sequencePasteShortcut, QStringLiteral("SequencePasteShortcut"));
        migrate(settings.moreShortcut, QStringLiteral("Shift+F10"), defaults.moreShortcut, QStringLiteral("MoreShortcut"));
    }
#endif
    // 旧配置可能已把 Ctrl+O 用于其他操作，升级时保留原绑定，不抢占按键。
    if (!json.contains(QStringLiteral("OpenShortcut"))
        && settings.shortcutConflicts(settings.openShortcut, QStringLiteral("OpenShortcut"))) settings.openShortcut.clear();
    settings.startWithWindows = json.value(QStringLiteral("StartWithWindows")).toBool();
    settings.runAsAdministrator = json.value(QStringLiteral("RunAsAdministrator")).toBool();
    settings.monitorText = json.value(QStringLiteral("MonitorText")).toBool(true);
    settings.normalizeUnicodeText = json.value(QStringLiteral("NormalizeUnicodeText")).toBool(false);
    settings.monitorImages = json.value(QStringLiteral("MonitorImages")).toBool(true);
    settings.monitorFiles = json.value(QStringLiteral("MonitorFiles")).toBool(true);
    settings.excludedApplications = json.value(QStringLiteral("ExcludedApplications")).toString(settings.excludedApplications);
    settings.themeMode = json.value(QStringLiteral("ThemeMode")).toString(settings.themeMode);
    if (settings.themeMode == QStringLiteral("浅色")) settings.themeMode = QStringLiteral("Light");
    if (settings.themeMode == QStringLiteral("深色")) settings.themeMode = QStringLiteral("Dark");
    if (settings.themeMode == QStringLiteral("跟随系统")) settings.themeMode = QStringLiteral("System");
    if (settings.themeMode != QStringLiteral("Light") && settings.themeMode != QStringLiteral("Dark")
        && settings.themeMode != QStringLiteral("System")) settings.themeMode = QStringLiteral("System");
    settings.retentionDays = json.value(QStringLiteral("RetentionDays")).toInt(settings.retentionDays);
    settings.maxHistoryEntries = json.value(QStringLiteral("MaxHistoryEntries")).toInt(settings.maxHistoryEntries);
    // 无效清理参数回退到默认值，避免损坏的配置导致全部未置顶记录被删除。
    if (settings.retentionDays <= 0) settings.retentionDays = AppSettings{}.retentionDays;
    if (settings.maxHistoryEntries <= 0) settings.maxHistoryEntries = AppSettings{}.maxHistoryEntries;
    settings.skippedUpdateVersion = json.value(QStringLiteral("SkippedUpdateVersion")).toString();
    if (!json.contains(QStringLiteral("SequenceCopyShortcut")) && settings.shortcutConflicts(settings.sequenceCopyShortcut, QStringLiteral("SequenceCopyShortcut"))) settings.sequenceCopyShortcut.clear();
    if (!json.contains(QStringLiteral("SequencePasteShortcut")) && settings.shortcutConflicts(settings.sequencePasteShortcut, QStringLiteral("SequencePasteShortcut"))) settings.sequencePasteShortcut.clear();
    return settings;
}

bool AppSettings::save(const QString &path) const {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QJsonObject json;
#ifdef Q_OS_MACOS
    json.insert(QStringLiteral("MacShortcutDefaultsVersion"), 1);
#endif
    json.insert(QStringLiteral("Language"), language);
    json.insert(QStringLiteral("GlobalHotKey"), globalHotKey);
    json.insert(QStringLiteral("SequencePasteShortcut"), sequencePasteShortcut);
    json.insert(QStringLiteral("SequenceCopyShortcut"), sequenceCopyShortcut);
    json.insert(QStringLiteral("InterceptWindowsClipboardShortcut"), interceptWindowsClipboardShortcut);
    json.insert(QStringLiteral("PasteShortcut"), pasteShortcut);
    json.insert(QStringLiteral("PlainTextPasteShortcut"), plainTextPasteShortcut);
    json.insert(QStringLiteral("PreviewShortcut"), previewShortcut);
    json.insert(QStringLiteral("PinShortcut"), pinShortcut);
    json.insert(QStringLiteral("PasteAsFileShortcut"), pasteAsFileShortcut);
    json.insert(QStringLiteral("MoreShortcut"), moreShortcut);
    json.insert(QStringLiteral("OpenShortcut"), openShortcut);
    json.insert(QStringLiteral("StartWithWindows"), startWithWindows);
    json.insert(QStringLiteral("RunAsAdministrator"), runAsAdministrator);
    json.insert(QStringLiteral("MonitorText"), monitorText);
    json.insert(QStringLiteral("NormalizeUnicodeText"), normalizeUnicodeText);
    json.insert(QStringLiteral("MonitorImages"), monitorImages);
    json.insert(QStringLiteral("MonitorFiles"), monitorFiles);
    json.insert(QStringLiteral("ExcludedApplications"), excludedApplications);
    json.insert(QStringLiteral("ThemeMode"), themeMode);
    json.insert(QStringLiteral("RetentionDays"), retentionDays);
    json.insert(QStringLiteral("MaxHistoryEntries"), maxHistoryEntries);
    json.insert(QStringLiteral("SkippedUpdateVersion"), skippedUpdateVersion);
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size()
        && file.commit();
}
