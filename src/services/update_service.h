#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QWidget;

class UpdateService final : public QObject {
    Q_OBJECT
public:
    explicit UpdateService(QObject *parent = nullptr);
    void check(QWidget *parent, const QString &settingsPath, bool automatic = false);

private:
    void handleCheckFinished(QNetworkReply *reply);
    void showResult();
    void startDownload();
    void writeDownloadData(QNetworkReply *reply);
    void handleDownloadFinished();
    void showDownloadFailure();
    bool startInstaller();
    void ignoreCurrentVersion();

    QNetworkAccessManager *network_ = nullptr;
    QPointer<QWidget> parent_;
    QString settingsPath_;
    QString releaseTag_;
    QString releaseNotes_;
    QUrl releaseUrl_;
    QUrl installerUrl_;
    QString installerName_;
    qint64 installerSize_ = 0;
    QString downloadPath_;
    QString installerPath_;
    QString downloadError_;
    QFile *downloadFile_ = nullptr;
    QProgressDialog *progressDialog_ = nullptr;
    bool checking_ = false;
    bool downloading_ = false;
    bool automatic_ = false;
};
