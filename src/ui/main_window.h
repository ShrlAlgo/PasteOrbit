#pragma once

#include "app_settings.h"
#include "history_store.h"

#include <QAbstractNativeEventFilter>
#include <QAtomicInteger>
#include <QSharedPointer>
#include <QFutureWatcher>
#include <QColor>
#include <QDate>
#include <QImage>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>
using PanelTarget = HWND;
using PanelBounds = RECT;
#else
using PanelTarget = quintptr;
using PanelBounds = QRect;
#endif

#include <optional>
#include <bitset>

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
class QShortcut;
class QStackedLayout;
class QPropertyAnimation;
class SettingsDialog;

class MainWindow final : public QWidget, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    MainWindow(QString dataDirectory, AppSettings settings);
    ~MainWindow() override;

    void showPanel(bool fromTray = false, PanelTarget requestedTarget = {});
    void hidePanel();
    void applySettings(const AppSettings &settings);
    void showSettings(SettingsDialog *page);
    void leaveSettings();
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
    void animatePage(QWidget *page, bool opening = true);
    void buildTray();
    void applyTheme();
    void registerHotkey();
    void updateWinVHook();
    bool setPassiveBrowsing(bool passive);
    void updatePanelInputHooks();
    void activatePanel(QWidget *focus = nullptr);
    void preparePanelMenu(QMenu *menu);
    bool isPanelWindow(PanelTarget window) const;
#ifdef Q_OS_WIN
    static LRESULT CALLBACK panelKeyboardProc(int code, WPARAM message, LPARAM data);
    static LRESULT CALLBACK panelMouseProc(int code, WPARAM message, LPARAM data);
#endif
    std::optional<PanelBounds> capturePasteTarget(PanelTarget window);
    void positionPanel(bool preferCursor, std::optional<PanelBounds> resolvedBounds = std::nullopt,
                       std::optional<PanelBounds> controlBounds = std::nullopt);
    void refreshHistory();
    void loadMoreHistory();
    void updateVisibleThumbnails();
    void updateDateIndicator();
    void scheduleCapture();
    void captureClipboard();
    void saveCapture(ClipboardCapture capture, QImage fallbackImage, quint32 sequence);
    void cleanupPasteFiles();
    void pasteRecord(const QString &id, bool plainText = false, bool asFile = false,
                     std::optional<QByteArray> editedContent = std::nullopt);
    void openRowMenu(const QString &id);
    void openRecord(const QString &id);
    void addOpenTargets(QMenu *menu, const QStringList &targets);
    void openTarget(const QString &target);
    void toggleHoverPreview(QString id);
    void closeHoverPreview(bool restoreFocus = false);
    void updateHoverPreviewImage();
    void setPinned(const QString &id);
    void deleteRecord(const QString &id);
    void clearCurrentList();
    void switchFilter(int direction);
    void setFilter(int kind);
    void openSourceMenu();
    void setSourceFilter(std::optional<QString> source);
    void scheduleAutoHide();
    void updateEmptyState();
    void showStatus(const QString &message);
    void setTrayPaused(bool paused);
    bool sourceIsExcluded(const QString &source) const;
    void updateTrayTooltip();

    QString dataDirectory_;
    QString databasePath_;
    QString settingsPath_;
    AppSettings settings_;
    HistoryStore store_;
    HistoryModel *model_ = nullptr;
    HistoryDelegate *delegate_ = nullptr;
    QListView *historyList_ = nullptr;
    QShortcut *moreShortcut_ = nullptr;
    QLineEdit *searchBox_ = nullptr;
    QToolButton *sourceButton_ = nullptr;
    QToolButton *sourceFilterChip_ = nullptr;
    std::optional<QString> selectedSource_;
    QLabel *emptyLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *dateLabel_ = nullptr;
    QDate dateIndicatorDay_;
    QStackedLayout *historyStack_ = nullptr;
    QWidget *historyPage_ = nullptr;
    SettingsDialog *settingsPage_ = nullptr;
    QWidget *drawerHost_ = nullptr;
    QPointer<QWidget> historyFocus_;
    QPointer<QPropertyAnimation> pageAnimation_;
    QWidget *hoverPreview_ = nullptr;
    QLabel *hoverImageLabel_ = nullptr;
    QScrollArea *hoverImageScroll_ = nullptr;
    QSize hoverImageSize_;
    QString hoverPreviewId_;
    QSharedPointer<QAtomicInteger<int>> hoverPreviewGeneration_ = QSharedPointer<QAtomicInteger<int>>::create(0);
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
    bool clipboardNotificationsAvailable_ = false;
    QTimer pauseTimer_;
    std::optional<HistoryCursor> nextCursor_;
    int selectedKind_ = -1;
    QSharedPointer<QAtomicInteger<int>> historyGeneration_ = QSharedPointer<QAtomicInteger<int>>::create(0);
    bool historyDirty_ = false;
    bool newHistoryAvailable_ = false;
    bool loadingPage_ = false;
    bool loadingMore_ = false;
    int unpinnedCount_ = 0;
    bool paused_ = false;
    bool isPinned_ = false;
    bool closingForExit_ = false;
    bool suppressClipboardCapture_ = false;
    bool captureSaving_ = false;
    bool startupUpdateCheckRequested_ = false;
    qint64 lastPasteFileCleanup_ = 0;
    quint32 lastClipboardSequence_ = 0;
    quint32 ownClipboardSequence_ = 0;
    PanelTarget targetWindow_ = {};
    PanelTarget targetFocusWindow_ = {};
    QPoint dragStart_;
    QPoint dragWindowStart_;
    bool dragging_ = false;
#ifdef Q_OS_WIN
    HHOOK winVHook_ = nullptr;
    HHOOK panelKeyboardHook_ = nullptr;
    HHOOK panelMouseHook_ = nullptr;
    std::bitset<256> swallowedPanelKeys_;
#endif
    bool passiveBrowsing_ = false;
    QRect hoveredHistoryRow_;
    bool restoreWasPaused_ = false;
    int panelShowGeneration_ = 0;
    int openGeneration_ = 0;
    QSharedPointer<QAtomicInteger<int>> pasteGeneration_ = QSharedPointer<QAtomicInteger<int>>::create(0);
    QThreadPool readPool_;
    QThreadPool contentPool_;
    QThreadPool writePool_;
    QColor pageColor_;
    QColor borderColor_;
};
