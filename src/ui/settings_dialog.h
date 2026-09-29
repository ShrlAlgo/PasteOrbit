#pragma once

#include "app_settings.h"

#include <QDialog>
#include <QHash>

class QAbstractButton;
class QComboBox;
class QLineEdit;
class QPushButton;
class QListWidget;
class QStackedWidget;

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(AppSettings settings, QString databasePath, QString settingsPath, QWidget *parent = nullptr);

signals:
    void settingsChanged(const AppSettings &settings);
    void checkUpdatesRequested();
    void storageOperationStarted();
    void storageOperationFinished(bool restored);

protected:
    void done(int result) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyTheme();
    QWidget *createPage(const QString &heading);
    void addCard(QWidget *page, const QString &titleKey, const QString &descriptionKey, QWidget *control);
    void loadControls(const AppSettings &settings);
    void saveControls();
    void persist(bool preserveSkippedVersion = true);
    void chooseProcesses();
    void restoreDefaults();
    void exportBackup();
    void restoreBackup();
    void refreshLocalization();

    AppSettings settings_;
    QString databasePath_;
    QString settingsPath_;
    QAbstractButton *startup_ = nullptr;
    QAbstractButton *administrator_ = nullptr;
    QAbstractButton *autoHide_ = nullptr;
    QAbstractButton *monitorText_ = nullptr;
    QAbstractButton *monitorImages_ = nullptr;
    QAbstractButton *monitorFiles_ = nullptr;
    QAbstractButton *interceptWinV_ = nullptr;
    QComboBox *theme_ = nullptr;
    QComboBox *language_ = nullptr;
    QComboBox *retention_ = nullptr;
    QComboBox *maximumEntries_ = nullptr;
    QLineEdit *excludedApplications_ = nullptr;
    QListWidget *navigation_ = nullptr;
    QStackedWidget *tabs_ = nullptr;
    QHash<QString, QPushButton *> shortcuts_;
    bool loading_ = false;
    bool storageBusy_ = false;
};
