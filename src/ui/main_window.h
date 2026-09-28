#pragma once

#include "app_settings.h"
#include "history_store.h"

#include <QAbstractNativeEventFilter>
#include <QFutureWatcher>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <windows.h>

#include <optional>

class HistoryDelegate;
class HistoryModel;
class QButtonGroup;
class QLineEdit;
class QListView;
class QLabel;
class QScrollArea;
class QPushButton;
class QToolButton;
class QMenu;
class QStackedLayout;
class QTextEdit;

class MainWindow final : public QWidget, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    MainWindow(QString dataDirectory, AppSettings settings);
    ~MainWindow() override;

    void showPanel(bool fromTray = false, HWND requestedTarget = nullptr);
    void hidePanel();
    void applySettings(const AppSettings &settings);
    void setSettingsWindowOpen(bool open) { settingsWindowOpen_ = open; }
    void beginStorageOperation();
    void finishStorageOperation(bool restored);
    const AppSettings &settings() const { return settings_; }
    QString databasePath() const { return databasePath_; }
    QString settingsPath() const { return settingsPath_; }

signals:
    void settingsRequested();
    void checkUpdatesRequested();
    void firstPanelShown();
    void exitRequested();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void buildTray();
    void applyTheme();
    void registerHotkey();
    void updateWinVHook();
    void positionPanel(bool preferCursor, std::optional<RECT> resolvedBounds = std::nullopt);
    void refreshHistory();
    void loadMoreHistory();
    void updateVisibleThumbnails();
    void scheduleCapture();
    void captureClipboard();
    void saveCapture(ClipboardCapture capture, QImage fallbackImage, DWORD sequence);
    void pasteRecord(const QString &id, bool plainText = false, bool asFile = false);
    void openRowMenu(const QString &id, std::optional<QPoint> position = std::nullopt);
    void togglePreview(const QString &id);
    void showHoverPreview(const QString &id);
    void closeHoverPreview();
    void updateHoverPreviewImage();
    void showTextPreview(const QString &text, const QString &html, bool markdown);
    void closeTextPreview();
    void updateTextPreviewGeometry();
    void upgradePreviewImage(const QString &id);
    void setPinned(const QString &id);
    void deleteRecord(const QString &id);
    void clearCurrentList();
    void switchFilter(int direction);
    void setFilter(int kind);
    void scheduleAutoHide();
    void updateEmptyState();
    void showStatus(const QString &message);
    void setTrayPaused(bool paused);
    bool sourceIsExcluded() const;
    void updateTrayTooltip();

    QString dataDirectory_;
    QString databasePath_;
    QString settingsPath_;
    AppSettings settings_;
    HistoryStore store_;
    HistoryModel *model_ = nullptr;
    HistoryDelegate *delegate_ = nullptr;
    QListView *historyList_ = nullptr;
    QLineEdit *searchBox_ = nullptr;
    QLabel *emptyLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QStackedLayout *historyStack_ = nullptr;
    QTextEdit *textPreview_ = nullptr;
    QWidget *hoverPreview_ = nullptr;
    QLabel *hoverImageLabel_ = nullptr;
    QScrollArea *hoverImageScroll_ = nullptr;
    QSize hoverImageSize_;
    QString hoverPreviewId_;
    QString pendingHoverPreviewId_;
    QTimer hoverPreviewTimer_;
    QTimer hoverCloseTimer_;
    int hoverPreviewGeneration_ = 0;
    double hoverImageZoom_ = 1.0;
    bool hoverImageDragging_ = false;
    QPoint hoverDragStart_;
    QPoint hoverScrollStart_;
    QToolButton *settingsButton_ = nullptr;
    QToolButton *pinButton_ = nullptr;
    QToolButton *clearButton_ = nullptr;
    QButtonGroup *filterButtons_ = nullptr;
    QSystemTrayIcon *trayIcon_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QAction *pauseAction_ = nullptr;
    QTimer searchTimer_;
    QTimer captureTimer_;
    QTimer clipboardPollTimer_;
    QTimer pauseTimer_;
    std::optional<HistoryCursor> firstPageCursor_;
    std::optional<HistoryCursor> nextCursor_;
    int selectedKind_ = -1;
    int historyGeneration_ = 0;
    bool historyDirty_ = false;
    bool loadingPage_ = false;
    bool loadingMore_ = false;
    int unpinnedCount_ = 0;
    bool paused_ = false;
    bool settingsWindowOpen_ = false;
    bool isPinned_ = false;
    bool closingForExit_ = false;
    bool suppressClipboardCapture_ = false;
    bool captureSaving_ = false;
    bool startupUpdateCheckRequested_ = false;
    DWORD lastClipboardSequence_ = 0;
    DWORD ownClipboardSequence_ = 0;
    HWND targetWindow_ = nullptr;
    HWND targetFocusWindow_ = nullptr;
    QPoint dragStart_;
    QPoint dragWindowStart_;
    bool dragging_ = false;
    HHOOK winVHook_ = nullptr;
    bool previewDragging_ = false;
    QRect hoveredHistoryRow_;
    bool restoreWasPaused_ = false;
    QPoint previewDragStart_;
    QPointF previewPanStart_;
    QString highResolutionPreviewId_;
    QString highResolutionLoadingId_;
    int previewGeneration_ = 0;
    int panelShowGeneration_ = 0;
    QThreadPool readPool_;
    QThreadPool writePool_;
    QColor pageColor_;
    QColor borderColor_;
};
