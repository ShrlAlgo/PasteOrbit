#include "app_settings.h"
#include "localization.h"
#include "main_window.h"
#include "settings_dialog.h"
#include "update_service.h"
#ifdef Q_OS_MACOS
#include "macos/desktop_integration.h"
#endif

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPixmapCache>
#include <QScopeGuard>
#include <QSettings>
#include <QLockFile>
#include <QStyleOption>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <shellapi.h>
#include <windows.h>
#endif

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

#ifdef Q_OS_WIN
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
    const bool started = ShellExecuteExW(&info) != FALSE;
    if (info.hProcess) CloseHandle(info.hProcess);
    return started;
}
#endif
}

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    // Qlementine 统一绘制控件，主题色由主窗口根据用户设置切换。
    QApplication::setStyle(new PasteOrbitStyle(&application));
    application.setWindowIcon(QIcon(QStringLiteral(":/PasteOrbit.ico")));
#ifdef Q_OS_WIN
    QFont uiFont(QStringLiteral("Microsoft YaHei UI"));
#else
    QFont uiFont = application.font();
#endif
    uiFont.setPixelSize(14);
    application.setFont(uiFont);
    QApplication::setApplicationName(QStringLiteral("PasteOrbit"));
    QApplication::setOrganizationName(QStringLiteral("PasteOrbit"));
    QApplication::setApplicationVersion(QStringLiteral(PASTEORBIT_VERSION));
    application.setQuitOnLastWindowClosed(false);
    QPixmapCache::setCacheLimit(2048);
#ifdef Q_OS_MACOS
    if (application.arguments().contains(QStringLiteral("--platform-check")))
        return MacIntegration::runPlatformChecks();
#endif

#ifdef Q_OS_WIN
    QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty())
        localAppData = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + QStringLiteral("/AppData/Local");
    const QString dataDirectory = localAppData + QStringLiteral("/PasteOrbit");
#else
    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
    QDir().mkpath(dataDirectory);
    AppSettings settings = AppSettings::load(dataDirectory + QStringLiteral("/settings.json"));
    AppLocalization::load(settings.language);
    application.setApplicationDisplayName(AppLocalization::get(QStringLiteral("ApplicationDisplayName")));

#ifdef Q_OS_WIN
    const bool elevatedRestart = application.arguments().contains(QStringLiteral("--elevated-restart"), Qt::CaseInsensitive);
    // 互斥锁负责跨权限级别的单实例约束；本地消息通道只用于唤起已有窗口。
    HANDLE singletonMutex = CreateMutexW(nullptr, TRUE, L"Local\\PasteOrbit.Native.SingleInstance");
    const DWORD mutexError = GetLastError();
    bool ownsMutex = singletonMutex && mutexError != ERROR_ALREADY_EXISTS;
    const auto releaseMutex = qScopeGuard([&] {
        if (ownsMutex) ReleaseMutex(singletonMutex);
        if (singletonMutex) CloseHandle(singletonMutex);
    });
    if (!singletonMutex && mutexError != ERROR_ACCESS_DENIED) {
        QMessageBox::critical(nullptr, QApplication::applicationDisplayName(),
                              QStringLiteral("无法检查运行中的实例（%1）。").arg(mutexError));
        return 1;
    }
    // 提权重启时等待原进程退出并交接锁，避免把自己的新进程判成重复实例。
    if (!ownsMutex && elevatedRestart && singletonMutex) {
        const DWORD waitResult = WaitForSingleObject(singletonMutex, 10000);
        ownsMutex = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
    }
#else
    // 锁文件覆盖进程整个生命周期，QLocalServer 仅用于唤起已有实例。
    QLockFile instanceLock(dataDirectory + QStringLiteral("/instance.lock"));
    const bool ownsMutex = instanceLock.tryLock();
    if (!ownsMutex && instanceLock.error() != QLockFile::LockFailedError) {
        QMessageBox::critical(nullptr, QApplication::applicationDisplayName(),
                              QStringLiteral("Cannot lock the PasteOrbit data directory."));
        return 1;
    }
#endif
    if (!ownsMutex) {
        QLocalSocket existingInstance;
        existingInstance.connectToServer(QString::fromLatin1(ServerName));
        if (existingInstance.waitForConnected(250)) {
            existingInstance.write("show");
            existingInstance.waitForBytesWritten(250);
        }
        return 0;
    }

    // 更新卸载可能删除启动项；以保存的设置修复当前程序的注册路径。
    if (settings.startWithWindows && !settings.applyWindowsStartup())
        qWarning("Could not synchronize PasteOrbit login startup registration");

#ifdef Q_OS_WIN
    if (settings.runAsAdministrator && !isAdministrator() && !elevatedRestart && startElevated()) return 0;
#endif

    QLocalServer::removeServer(QString::fromLatin1(ServerName));
    QLocalServer server;
    if (!server.listen(QString::fromLatin1(ServerName))) {
        QMessageBox::critical(nullptr, QApplication::applicationDisplayName(), server.errorString());
        return 1;
    }

    MainWindow window(dataDirectory, settings);
#ifdef Q_OS_MACOS
    MacIntegration::installDockReopenHandler(&window, [&window] { window.showPanel(true); });
#endif
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
            window.showPanel(true);
            return;
        }
        const bool wasAdministrator = window.settings().runAsAdministrator;
        auto *dialog = new SettingsDialog(window.settings(), window.databasePath(), window.settingsPath(), &window);
        settingsDialog = dialog;
        QObject::connect(dialog, &SettingsDialog::hideRequested, &window, &MainWindow::hidePanel);
        QObject::connect(dialog, &SettingsDialog::settingsChanged, &window, &MainWindow::applySettings);
        QObject::connect(dialog, &SettingsDialog::checkUpdatesRequested, dialog, [&updateService, dialog, &window] {
            updateService.check(dialog, window.settingsPath());
        });
        QObject::connect(dialog, &SettingsDialog::storageOperationStarted,
                         &window, &MainWindow::beginStorageOperation);
        QObject::connect(dialog, &SettingsDialog::storageOperationFinished,
                         &window, &MainWindow::finishStorageOperation);
        QObject::connect(dialog, &QDialog::finished, &window,
                         [&window, &server, &application, &settingsDialog, dialog, wasAdministrator] {
            settingsDialog = nullptr;
            window.leaveSettings();
            dialog->deleteLater();
#ifdef Q_OS_WIN
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
                    QMessageBox::critical(&window, QApplication::applicationDisplayName(), server.errorString());
                }
            }
#endif
        });
        window.showSettings(dialog);
    });
    QObject::connect(&updateService, &UpdateService::dialogAboutToShow,
                     &window, &MainWindow::activatePanel);
    QObject::connect(&window, &MainWindow::checkUpdatesRequested, &updateService, [&updateService, &window] {
        updateService.check(&window, window.settingsPath());
    });
    QObject::connect(&window, &MainWindow::firstPanelShown, &updateService, [&updateService, &window] {
        updateService.check(&window, window.settingsPath(), true);
    });
    QObject::connect(&window, &MainWindow::exitRequested, &application, &QApplication::quit);
#if !defined(NDEBUG) || defined(Q_OS_MACOS)
    // 调试版和 macOS 启动时显示面板，方便调试及首次配置权限。
    QTimer::singleShot(0, &window, [&window] { window.showPanel(true); });
#endif
    return application.exec();
}
