#include "update_service.h"

#include "app_settings.h"
#include "localization.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVersionNumber>
#include <QUuid>

#include <windows.h>

namespace {
constexpr auto LatestReleaseEndpoint = "https://api.github.com/repos/ShrlAlgo/PasteOrbit/releases/latest";
constexpr qint64 DownloadBufferLimit = 128 * 1024;

QString localized(const QString &key) {
    return AppLocalization::get(key);
}

bool isHttpsUrl(const QUrl &url) {
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
}
}

UpdateService::UpdateService(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void UpdateService::check(QWidget *parent, const QString &settingsPath, bool automatic) {
    if (checking_ || downloading_) return;
    parent_ = parent;
    settingsPath_ = settingsPath;
    automatic_ = automatic;
    checking_ = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(LatestReleaseEndpoint))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("PasteOrbit/%1").arg(QApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(120000);
    auto *reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleCheckFinished(reply); });
}

void UpdateService::handleCheckFinished(QNetworkReply *reply) {
    checking_ = false;
    const QByteArray body = reply->readAll();
    const auto networkError = reply->error();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError) {
        if (!automatic_ && parent_)
            QMessageBox::warning(parent_, localized(QStringLiteral("UpdateCheckFailedTitle")),
                                 localized(QStringLiteral("UpdateCheckFailedMessage")));
        return;
    }

    const auto document = QJsonDocument::fromJson(body);
    const auto release = document.object();
    releaseTag_ = release.value(QStringLiteral("tag_name")).toString();
    releaseNotes_ = release.value(QStringLiteral("body")).toString().trimmed();
    releaseUrl_ = QUrl(release.value(QStringLiteral("html_url")).toString());
    static const QRegularExpression versionPattern(QStringLiteral("^[vV][0-9]+\\.[0-9]+\\.[0-9]+(?:\\.[0-9]+)?$"));
    const QString versionText = releaseTag_.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)
        ? releaseTag_.mid(1) : QString{};
    const QVersionNumber latestVersion = QVersionNumber::fromString(versionText);
    const QVersionNumber currentVersion = QVersionNumber::fromString(QApplication::applicationVersion());
    if (!document.isObject() || !versionPattern.match(releaseTag_).hasMatch()
        || latestVersion.isNull() || currentVersion.isNull() || !isHttpsUrl(releaseUrl_)) {
        if (!automatic_ && parent_)
            QMessageBox::warning(parent_, localized(QStringLiteral("UpdateCheckFailedTitle")),
                                 localized(QStringLiteral("UpdateCheckFailedMessage")));
        return;
    }

    installerUrl_ = QUrl{};
    installerName_.clear();
    installerSize_ = 0;
    const auto assets = release.value(QStringLiteral("assets")).toArray();
    for (const auto &assetValue : assets) {
        const auto asset = assetValue.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
        if (name.endsWith(QStringLiteral("-Setup.exe"), Qt::CaseInsensitive) && isHttpsUrl(url)) {
            installerName_ = name;
            installerUrl_ = url;
            installerSize_ = asset.value(QStringLiteral("size")).toVariant().toLongLong();
            break;
        }
    }

    if (latestVersion <= currentVersion) {
        if (!automatic_ && parent_)
            QMessageBox::information(parent_, localized(QStringLiteral("UpdateNoUpdateTitle")),
                AppLocalization::format(QStringLiteral("UpdateNoUpdateMessage"), {currentVersion.toString()}));
        return;
    }

    if (automatic_) {
        const auto settings = AppSettings::load(settingsPath_);
        if (settings.skippedUpdateVersion.compare(releaseTag_, Qt::CaseInsensitive) == 0) return;
    }
    showResult();
}

void UpdateService::showResult() {
    if (!parent_) return;
    const QString currentVersion = QApplication::applicationVersion();
    QMessageBox dialog(parent_);
    dialog.setIcon(QMessageBox::Information);
    dialog.setWindowTitle(localized(QStringLiteral("UpdateAvailableTitle")));
    dialog.setText(AppLocalization::format(QStringLiteral("UpdateAvailableMessage"),
                                           {releaseTag_.mid(1), currentVersion}));
    if (!releaseNotes_.isEmpty()) {
        dialog.setDetailedText(releaseNotes_);
        // 此时尚未添加操作按钮，唯一按钮是 Qt 内建的详情开关。
        for (auto *details : dialog.findChildren<QPushButton *>()) {
            details->setText(localized(QStringLiteral("ShowUpdateDetails")));
            connect(details, &QPushButton::clicked, &dialog, [details, expanded = false]() mutable {
                expanded = !expanded;
                details->setText(localized(expanded ? QStringLiteral("HideUpdateDetails")
                                                    : QStringLiteral("ShowUpdateDetails")));
            });
        }
    }

    QAbstractButton *primary = nullptr;
    QAbstractButton *skip = nullptr;
    if (!installerUrl_.isEmpty()) {
        primary = dialog.addButton(localized(QStringLiteral("DownloadUpdateButton")), QMessageBox::AcceptRole);
        skip = dialog.addButton(localized(QStringLiteral("DoNotRemindUpdateButton")), QMessageBox::DestructiveRole);
    } else {
        skip = dialog.addButton(localized(QStringLiteral("DoNotRemindUpdateButton")), QMessageBox::AcceptRole);
    }
    auto *later = dialog.addButton(localized(QStringLiteral("Later")), QMessageBox::RejectRole);
    // 详情按钮由 Qt 创建，统一使用操作按钮的尺寸，避免它在按钮栏中单独变大。
    QSize buttonSize = later->sizeHint().expandedTo(skip->sizeHint());
    if (primary) buttonSize = buttonSize.expandedTo(primary->sizeHint());
    for (auto *button : dialog.findChildren<QPushButton *>()) button->setFixedSize(buttonSize);
    dialog.exec();
    if (dialog.clickedButton() == primary && primary) startDownload();
    else if (dialog.clickedButton() == skip) ignoreCurrentVersion();
}

void UpdateService::ignoreCurrentVersion() {
    AppSettings settings = AppSettings::load(settingsPath_);
    settings.skippedUpdateVersion = releaseTag_;
    settings.save(settingsPath_);
}

void UpdateService::startDownload() {
    if (downloading_ || installerUrl_.isEmpty()) return;
    if (QFileInfo(installerName_).fileName() != installerName_
        || !installerName_.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        showDownloadFailure();
        return;
    }

    const QString version = releaseTag_.mid(1);
    const QString directory = QDir(QDir::tempPath()).filePath(QStringLiteral("PasteOrbit/Updates/") + version);
    if (!QDir().mkpath(directory)) {
        showDownloadFailure();
        return;
    }
    installerPath_ = QDir(directory).filePath(installerName_);
    downloadPath_ = installerPath_ + QStringLiteral(".download");
    if (QFileInfo::exists(downloadPath_) && !QFile::remove(downloadPath_)) {
        showDownloadFailure();
        return;
    }
    downloadFile_ = new QFile(downloadPath_, this);
    if (!downloadFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        downloadFile_->deleteLater();
        downloadFile_ = nullptr;
        showDownloadFailure();
        return;
    }

    downloading_ = true;
    downloadError_.clear();
    progressDialog_ = new QProgressDialog(localized(QStringLiteral("UpdateDownloadingMessage")),
                                           QString{}, 0, 100, parent_);
    progressDialog_->setWindowTitle(localized(QStringLiteral("UpdateDownloadingTitle")));
    progressDialog_->setCancelButton(nullptr);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);
    progressDialog_->setRange(0, installerSize_ > 0 ? 100 : 0);
    progressDialog_->show();

    QNetworkRequest request(installerUrl_);
    request.setRawHeader("Accept", "application/octet-stream");
    request.setTransferTimeout(120000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = network_->get(request);
    reply->setReadBufferSize(DownloadBufferLimit);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { writeDownloadData(reply); });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (!progressDialog_) return;
        const qint64 size = installerSize_ > 0 ? installerSize_ : total;
        if (size <= 0) progressDialog_->setRange(0, 0);
        else {
            progressDialog_->setRange(0, 100);
            progressDialog_->setValue(static_cast<int>(qMin<qint64>(100, received * 100 / size)));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        writeDownloadData(reply);
        downloading_ = false;
        const auto networkError = reply->error();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError && downloadError_.isEmpty())
            downloadError_ = QStringLiteral("network");
        handleDownloadFinished();
    });
}

void UpdateService::writeDownloadData(QNetworkReply *reply) {
    if (!downloadFile_) return;
    if (!reply) return;
    // 下载内容直接落盘，网络响应体不会整体驻留内存。
    const QByteArray chunk = reply->readAll();
    if (!chunk.isEmpty() && downloadFile_->write(chunk) != chunk.size()) {
        downloadError_ = downloadFile_->errorString();
        reply->abort();
    }
}

void UpdateService::handleDownloadFinished() {
    qint64 downloadedSize = 0;
    if (downloadFile_) {
        if (!downloadFile_->flush() && downloadError_.isEmpty()) downloadError_ = downloadFile_->errorString();
        downloadedSize = downloadFile_->size();
        downloadFile_->close();
        downloadFile_->deleteLater();
        downloadFile_ = nullptr;
    }
    if (progressDialog_) {
        progressDialog_->close();
        progressDialog_->deleteLater();
        progressDialog_ = nullptr;
    }

    if (!downloadError_.isEmpty() || downloadedSize <= 0
        || (installerSize_ > 0 && downloadedSize != installerSize_)) {
        QFile::remove(downloadPath_);
        showDownloadFailure();
        return;
    }
    if (QFileInfo::exists(installerPath_) && !QFile::remove(installerPath_)) {
        QFile::remove(downloadPath_);
        showDownloadFailure();
        return;
    }
    if (!QFile::rename(downloadPath_, installerPath_) || !startInstaller()) {
        showDownloadFailure();
        return;
    }
    QApplication::quit();
}

void UpdateService::showDownloadFailure() {
    if (parent_)
        QMessageBox::warning(parent_, localized(QStringLiteral("UpdateDownloadFailedTitle")),
                             localized(QStringLiteral("UpdateDownloadFailedMessage")));
}

bool UpdateService::startInstaller() {
    const QString applicationPath = QCoreApplication::applicationFilePath();
    const QString applicationDirectory = QFileInfo(applicationPath).absolutePath();
    const QString scriptPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("PasteOrbit-update-%1.ps1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QString script = QString::fromUtf8(R"PS(
param([int]$ProcessId,[string]$InstallerPath,[string]$ApplicationPath,[string]$ApplicationDirectory,[string]$ExpectedVersion)
$ErrorActionPreference = 'Stop'
$installed = $false
try {
    try { Wait-Process -Id $ProcessId -Timeout 10 -ErrorAction Stop } catch {}
    # 旧版可能多开；只结束与当前安装目录完全一致的残留进程，避免占用安装文件。
    $applicationFullPath = [IO.Path]::GetFullPath($ApplicationPath)
    $instances = Get-CimInstance Win32_Process -Filter "Name = 'PasteOrbit.exe'" |
        Where-Object { $_.ExecutablePath -and [string]::Equals(
            [IO.Path]::GetFullPath($_.ExecutablePath), $applicationFullPath,
            [StringComparison]::OrdinalIgnoreCase) }
    foreach ($instance in $instances) {
        Stop-Process -Id $instance.ProcessId -Force -ErrorAction Stop
        try { Wait-Process -Id $instance.ProcessId -Timeout 10 -ErrorAction Stop } catch {}
        if (Get-Process -Id $instance.ProcessId -ErrorAction SilentlyContinue) {
            throw 'PasteOrbit is still running'
        }
    }
    if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) { throw 'Downloaded installer is missing' }

    # 只卸载与当前程序目录匹配的 Inno 安装，绝不执行其他位置的卸载程序。
    $expectedDirectory = [IO.Path]::GetFullPath($ApplicationDirectory).TrimEnd([char]'\')
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
    try {
        $key = $registry.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Uninstall\{D6C9A5F7-5C7E-4C1D-9F2A-7C9D8B3E4A11}_is1')
        if ($key) {
            try {
                $installedDirectory = [string]$key.GetValue('InstallLocation')
                $uninstallCommand = [string]$key.GetValue('UninstallString')
            } finally { $key.Close() }
            if ([string]::IsNullOrWhiteSpace($installedDirectory) -or
                -not [string]::Equals([IO.Path]::GetFullPath($installedDirectory).TrimEnd([char]'\'),
                                      $expectedDirectory, [StringComparison]::OrdinalIgnoreCase) -or
                $uninstallCommand -cnotmatch '^"([^"]+\\unins[0-9]+\.exe)"$') {
                throw 'Existing installation does not match the running application'
            }
            $uninstallerPath = $Matches[1]
            if (-not [string]::Equals([IO.Path]::GetDirectoryName($uninstallerPath).TrimEnd([char]'\'),
                                      $expectedDirectory, [StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-Path -LiteralPath $uninstallerPath -PathType Leaf)) {
                throw 'Existing uninstaller is unavailable or outside the application directory'
            }
            if (((Get-Item -LiteralPath $expectedDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                ((Get-Item -LiteralPath $uninstallerPath).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Refusing to uninstall through a symbolic link'
            }
            $installed = $true
        }
    } finally { $registry.Close() }

    $tasks = @()
    if ($installed) {
        $runValue = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name PasteOrbit -ErrorAction SilentlyContinue).PasteOrbit
        if ($runValue -eq ('"' + $ApplicationPath + '"')) { $tasks += 'autostart' }
        $desktop = [Environment]::GetFolderPath('DesktopDirectory')
        if ($desktop -and (Test-Path -LiteralPath (Join-Path $desktop 'PasteOrbit.lnk') -PathType Leaf)) {
            $tasks += 'desktopicon'
        }
        $uninstaller = Start-Process -FilePath $uninstallerPath -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
        if ($uninstaller.ExitCode -ne 0) { throw 'Uninstaller failed' }
        # Inno 的卸载程序会派生临时副本，等待原卸载文件消失后再安装新版。
        for ($i = 0; $i -lt 300 -and (Test-Path -LiteralPath $uninstallerPath); $i++) {
            Start-Sleep -Milliseconds 100
        }
        if (Test-Path -LiteralPath $uninstallerPath) { throw 'Uninstaller did not finish' }
    }
    $args = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CLOSEAPPLICATIONS',('/DIR="' + $ApplicationDirectory + '"'))
    if ($installed) { $args += ('/TASKS="' + ($tasks -join ',') + '"') }
    $installer = Start-Process -FilePath $InstallerPath -ArgumentList $args -Wait -PassThru
    $installedVersion = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{D6C9A5F7-5C7E-4C1D-9F2A-7C9D8B3E4A11}_is1' -ErrorAction SilentlyContinue).DisplayVersion
    if ($installer.ExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $ApplicationPath -PathType Leaf) -or
        $installedVersion -ne $ExpectedVersion) {
        throw 'Installer failed'
    }
    $success = $true
    Start-Process -FilePath $ApplicationPath -WorkingDirectory $ApplicationDirectory
} catch {
    if (Test-Path -LiteralPath $ApplicationPath -PathType Leaf) {
        Start-Process -FilePath $ApplicationPath -WorkingDirectory $ApplicationDirectory
    }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(
        "PasteOrbit 更新未完成。安装包已保留：$InstallerPath`n$($_.Exception.Message)",
        'PasteOrbit', 'OK', 'Error') | Out-Null
} finally {
    if ($success) { Remove-Item -LiteralPath $InstallerPath -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
}
)PS");
    QSaveFile scriptFile(scriptPath);
    const QByteArray scriptBytes = script.toUtf8();
    if (!scriptFile.open(QIODevice::WriteOnly) || scriptFile.write(scriptBytes) != scriptBytes.size()
        || !scriptFile.commit()) return false;

    QProcess launcher;
    launcher.setProgram(QStringLiteral("powershell.exe"));
    launcher.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath,
        QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("-InstallerPath"), installerPath_, QStringLiteral("-ApplicationPath"), applicationPath,
        QStringLiteral("-ApplicationDirectory"), applicationDirectory,
        QStringLiteral("-ExpectedVersion"), releaseTag_.mid(1)});
    launcher.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    if (launcher.startDetached()) return true;
    QFile::remove(scriptPath);
    return false;
}
