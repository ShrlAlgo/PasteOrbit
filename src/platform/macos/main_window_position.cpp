#include "main_window.h"
#include "history_model.h"
#include "paste_target.h"
#include "desktop_integration.h"
#include "settings_dialog.h"

#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedLayout>

std::optional<PanelBounds> MainWindow::capturePasteTarget(PanelTarget process) {
    const auto snapshot = PasteTarget::capture(process);
    if (!snapshot) return {};
    targetWindow_ = snapshot->window;
    targetFocusWindow_ = snapshot->focus;
    return snapshot->caret;
}

void MainWindow::positionPanel(bool preferCursor, std::optional<PanelBounds> bounds,
                                std::optional<PanelBounds>) {
    const QPoint point = !preferCursor && bounds ? bounds->bottomLeft() : QCursor::pos();
    auto *screen = QGuiApplication::screenAt(point);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry().adjusted(8, 8, -8, -8);
    setFixedSize(qMin(360, qMax(1, area.width())), qMin(500, qMax(1, area.height())));
    int x = preferCursor ? point.x() - width() / 2 : point.x();
    int y = point.y() + 14;
    if (y + height() > area.bottom() + 1)
        y = (bounds ? bounds->top() : point.y()) - height() - 14;
    move(qBound(area.left(), x, qMax(area.left(), area.right() - width() + 1)),
         qBound(area.top(), y, qMax(area.top(), area.bottom() - height() + 1)));
}

void MainWindow::showPanel(bool fromTray, PanelTarget requestedTarget) {
    if (auto *modal = QApplication::activeModalWidget()) { activatePanel(modal); return; }
    if (QApplication::activePopupWidget()) return;
    if (settingsPage_) { show(); activatePanel(); return; }
    if (suppressClipboardCapture_) return;
    pasteGeneration_->fetchAndAddRelaxed(1);
    ++panelShowGeneration_;
    const auto foreground = requestedTarget ? requestedTarget : MacIntegration::foregroundApplication();
    if (isVisible() && !fromTray && !MacIntegration::isExternalApplication(foreground)) {
        hidePanel();
        return;
    }
    // 显示前记录外部应用；激活面板之后不能再把本应用误记为粘贴目标。
    const auto bounds = capturePasteTarget(foreground);
    positionPanel(fromTray, bounds);
    show();
    activatePanel();
    if (!startupUpdateCheckRequested_) {
        startupUpdateCheckRequested_ = true;
        emit firstPanelShown();
    }
    if (newHistoryAvailable_) {
        searchTimer_.stop();
        const QSignalBlocker blocker(searchBox_);
        searchBox_->clear();
        historyList_->verticalScrollBar()->setValue(0);
        if (model_->rowCount()) historyList_->setCurrentIndex(model_->index(0));
        newHistoryAvailable_ = false;
        historyDirty_ = true;
    }
    if (historyDirty_) refreshHistory();
    historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
    updateVisibleThumbnails();
}
