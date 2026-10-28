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

bool AppSettings::applyWindowsStartup() const {
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
}

QString AppSettings::normalizedShortcut(QString value) {
    value.remove(QLatin1Char(' '));
    value.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
    return QKeySequence(value).toString(QKeySequence::PortableText).toCaseFolded();
}

bool AppSettings::shortcutConflicts(const QString &value, const QString &bindingKey) const {
    const QString normalized = normalizedShortcut(value);
    if (normalized.isEmpty()) return true;
    const struct { const char *key; const QString *value; } bindings[]{
        {"GlobalHotKey", &globalHotKey}, {"PasteShortcut", &pasteShortcut},
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
    const auto combination = QKeySequence(value)[0];
    const auto modifiers = combination.keyboardModifiers();
    if (bindingKey == QStringLiteral("GlobalHotKey")) return modifiers == Qt::NoModifier;
    return modifiers.testFlag(Qt::MetaModifier) || combination.key() == Qt::Key_Escape
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
    settings.interceptWindowsClipboardShortcut = json.value(QStringLiteral("InterceptWindowsClipboardShortcut")).toBool();
    settings.pasteShortcut = json.value(QStringLiteral("PasteShortcut")).toString(settings.pasteShortcut);
    settings.plainTextPasteShortcut = json.value(QStringLiteral("PlainTextPasteShortcut")).toString(settings.plainTextPasteShortcut);
    settings.previewShortcut = json.value(QStringLiteral("PreviewShortcut")).toString(settings.previewShortcut);
    settings.pinShortcut = json.value(QStringLiteral("PinShortcut")).toString(settings.pinShortcut);
    settings.pasteAsFileShortcut = json.value(QStringLiteral("PasteAsFileShortcut")).toString(settings.pasteAsFileShortcut);
    settings.moreShortcut = json.value(QStringLiteral("MoreShortcut")).toString(settings.moreShortcut);
    settings.openShortcut = json.value(QStringLiteral("OpenShortcut")).toString(settings.openShortcut);
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
    return settings;
}

bool AppSettings::save(const QString &path) const {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QJsonObject json;
    json.insert(QStringLiteral("Language"), language);
    json.insert(QStringLiteral("GlobalHotKey"), globalHotKey);
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
