#include "app_settings.h"
#include "localization.h"
#include "main_window.h"
#include "settings_dialog.h"
#include "update_service.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPixmapCache>
#include <QSettings>
#include <QStyleOption>
#include <QStandardPaths>
#include <QTimer>

#include <shellapi.h>
#include <windows.h>

#include <oclero/qlementine/style/QlementineStyle.hpp>

namespace {
constexpr auto ServerName = "PasteOrbit.Native.SingleInstance";

class PasteOrbitStyle final : public oclero::qlementine::QlementineStyle {
public:
    using QlementineStyle::QlementineStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override {
        if (element != PE_PanelTipLabel || !option) {
            QlementineStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(option->palette.color(QPalette::ToolTipBase));
        painter->setPen(QPen(option->palette.color(QPalette::Mid), 1));
        painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        painter->restore();
    }

    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                  QStyleHintReturn *result = nullptr) const override {
        if (hint == SH_ToolTip_Mask && option && result && result->type == QStyleHintReturn::SH_Mask) {
            QPainterPath path;
            path.addRoundedRect(option->rect, 6, 6);
            static_cast<QStyleHintReturnMask *>(result)->region = QRegion(path.toFillPolygon().toPolygon());
            return 1;
        }
        return QlementineStyle::styleHint(hint, option, widget, result);
    }
};

bool isAdministrator() {
    BOOL member = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) return false;
    CheckTokenMembership(nullptr, group, &member);
    FreeSid(group);
    return member != FALSE;
}

bool startElevated() {
    const QString executable = QCoreApplication::applicationFilePath();
    const QString workingDirectory = QFileInfo(executable).absolutePath();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
    info.lpParameters = L"--elevated-restart";
    info.lpDirectory = reinterpret_cast<LPCWSTR>(workingDirectory.utf16());
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}
}

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    // Qlementine 统一绘制控件，主题色由主窗口根据用户设置切换。
    QApplication::setStyle(new PasteOrbitStyle(&application));
    application.setWindowIcon(QIcon(QStringLiteral(":/PasteOrbit.ico")));
    QFont uiFont(QStringLiteral("Segoe UI"));
    uiFont.setPixelSize(14);
    application.setFont(uiFont);
    QApplication::setApplicationName(QStringLiteral("PasteOrbit"));
    QApplication::setOrganizationName(QStringLiteral("PasteOrbit"));
    QApplication::setApplicationVersion(QStringLiteral(PASTEORBIT_VERSION));
    application.setQuitOnLastWindowClosed(false);
    QPixmapCache::setCacheLimit(2048);

    QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty())
        localAppData = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + QStringLiteral("/AppData/Local");
    const QString dataDirectory = localAppData + QStringLiteral("/PasteOrbit");
    QDir().mkpath(dataDirectory);
    AppSettings settings = AppSettings::load(dataDirectory + QStringLiteral("/settings.json"));
    QString language = settings.language;
    if (language.isEmpty()) language = QLocale().name().startsWith(QStringLiteral("en"))
        ? QStringLiteral("en-US") : QStringLiteral("zh-CN");
    AppLocalization::load(language);

    const bool elevatedRestart = application.arguments().contains(QStringLiteral("--elevated-restart"), Qt::CaseInsensitive);
    if (!elevatedRestart) {
        QLocalSocket existingInstance;
        existingInstance.connectToServer(QString::fromLatin1(ServerName));
        if (existingInstance.waitForConnected(250)) {
            existingInstance.write("show");
            existingInstance.waitForBytesWritten(250);
            return 0;
        }
    }

    if (settings.runAsAdministrator && !isAdministrator() && !elevatedRestart && startElevated()) return 0;

    QLocalServer::removeServer(QString::fromLatin1(ServerName));
    QLocalServer server;
    if (!server.listen(QString::fromLatin1(ServerName))) {
        QMessageBox::critical(nullptr, QStringLiteral("PasteOrbit"), server.errorString());
        return 1;
    }

    MainWindow window(dataDirectory, settings);
    UpdateService updateService(&application);
    QPointer<SettingsDialog> settingsDialog;
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, &window] {
                if (socket->readAll().contains("show")) window.showPanel(true);
                socket->disconnectFromServer();
                socket->deleteLater();
            });
            if (socket->bytesAvailable()) {
                if (socket->readAll().contains("show")) window.showPanel(true);
                socket->disconnectFromServer();
                socket->deleteLater();
            }
        }
    });
    QObject::connect(&window, &MainWindow::settingsRequested, &window,
                     [&window, &server, &application, &updateService, &settingsDialog] {
        if (settingsDialog) {
            settingsDialog->raise();
            settingsDialog->activateWindow();
            return;
        }
        const bool wasAdministrator = window.settings().runAsAdministrator;
        // 独立顶层弹窗不继承历史面板的透明窗口样式表。
        auto *dialog = new SettingsDialog(window.settings(), window.databasePath(), window.settingsPath());
        settingsDialog = dialog;
        dialog->setWindowModality(Qt::ApplicationModal);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        window.setSettingsWindowOpen(true);
        QObject::connect(dialog, &SettingsDialog::settingsChanged, &window, &MainWindow::applySettings);
        QObject::connect(dialog, &SettingsDialog::checkUpdatesRequested, dialog, [&updateService, dialog, &window] {
            updateService.check(dialog, window.settingsPath());
        });
        QObject::connect(dialog, &SettingsDialog::storageOperationStarted,
                         &window, &MainWindow::beginStorageOperation);
        QObject::connect(dialog, &SettingsDialog::storageOperationFinished,
                         &window, &MainWindow::finishStorageOperation);
        QObject::connect(dialog, &QDialog::finished, &window,
                         [&window, &server, &application, &settingsDialog, wasAdministrator] {
            settingsDialog = nullptr;
            window.setSettingsWindowOpen(false);
            if (!wasAdministrator && window.settings().runAsAdministrator) {
                server.close();
                QLocalServer::removeServer(QString::fromLatin1(ServerName));
                if (startElevated()) {
                    application.quit();
                    return;
                }
                AppSettings reverted = window.settings();
                reverted.runAsAdministrator = false;
                reverted.save(window.settingsPath());
                window.applySettings(reverted);
                if (!server.listen(QString::fromLatin1(ServerName))) {
                    QMessageBox::critical(&window, QStringLiteral("PasteOrbit"), server.errorString());
                }
            }
        });
        dialog->open();
    });
    QObject::connect(&window, &MainWindow::checkUpdatesRequested, &updateService, [&updateService, &window] {
        updateService.check(&window, window.settingsPath());
    });
    QObject::connect(&window, &MainWindow::firstPanelShown, &updateService, [&updateService, &window] {
        updateService.check(&window, window.settingsPath(), true);
    });
    QObject::connect(&window, &MainWindow::exitRequested, &application, &QApplication::quit);
#ifndef NDEBUG
    // 调试版启动时直接显示面板，F5 不再看起来像进程立即退出。
    QTimer::singleShot(0, &window, [&window] { window.showPanel(true); });
#endif
    return application.exec();
}
