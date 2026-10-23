#include "main_window.h"
#include "history_model.h"
#include "localization.h"
#include <QApplication>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QScopeGuard>
#include <QWindow>
#include <QtMath>
#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <thread>
#include <system_error>
#include <UIAutomation.h>
#include <oleacc.h>
#include <wrl/client.h>

namespace {
std::optional<RECT> firstAutomationRect(SAFEARRAY *rectangles) {
    if (!rectangles || SafeArrayGetDim(rectangles) != 1) return std::nullopt;
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(rectangles, 1, &lower))
        || FAILED(SafeArrayGetUBound(rectangles, 1, &upper))) return std::nullopt;
    for (LONG index = lower; index <= upper - 3; index += 4) {
        double values[4]{};
        bool valid = true;
        for (LONG offset = 0; offset < 4; ++offset) {
            LONG element = index + offset;
            if (FAILED(SafeArrayGetElement(rectangles, &element, &values[offset]))
                || !std::isfinite(values[offset])) { valid = false; break; }
        }
        if (!valid || values[2] <= 0 || values[3] <= 0) continue;
        const double rightValue = values[0] + values[2];
        const double bottomValue = values[1] + values[3];
        if (values[0] < LONG_MIN || values[0] > LONG_MAX
            || values[1] < LONG_MIN || values[1] > LONG_MAX
            || rightValue < LONG_MIN || rightValue > LONG_MAX
            || bottomValue < LONG_MIN || bottomValue > LONG_MAX) continue;
        return RECT{static_cast<LONG>(std::lround(values[0])), static_cast<LONG>(std::lround(values[1])),
                    static_cast<LONG>(std::lround(rightValue)), static_cast<LONG>(std::lround(bottomValue))};
    }
    return std::nullopt;
}

struct InputBounds {
    std::optional<RECT> caret;
    std::optional<RECT> control;
};

struct InputBoundsQuery {
    InputBounds bounds;
    std::atomic_bool finished{false};
};

InputBounds automationInputBounds(HWND foreground) {
    InputBounds bounds;
    DWORD targetProcess = 0;
    if (!foreground || !GetWindowThreadProcessId(foreground, &targetProcess)
        || targetProcess == GetCurrentProcessId()) return bounds;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return bounds;
    const auto cleanup = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized)) CoUninitialize();
    });

    using Microsoft::WRL::ComPtr;
    // Chromium 等自绘输入框没有 Win32 caret，也未必支持 TextPattern2；查询其真实 MSAA 插入光标。
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    if (thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus
        && (info.hwndFocus == foreground || IsChild(foreground, info.hwndFocus))) {
        ComPtr<IAccessible> caret;
        if (SUCCEEDED(AccessibleObjectFromWindow(info.hwndFocus, OBJID_CARET,
                                               IID_PPV_ARGS(caret.GetAddressOf()))) && caret) {
            VARIANT self{};
            self.vt = VT_I4;
            self.lVal = CHILDID_SELF;
            VARIANT state{};
            LONG x = 0, y = 0, width = 0, height = 0;
            if (SUCCEEDED(caret->get_accState(self, &state)) && state.vt == VT_I4
                && !(state.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))
                && SUCCEEDED(caret->accLocation(&x, &y, &width, &height, self))
                && width >= 0 && height > 0 && x <= LONG_MAX - qMax(1L, width)
                && y <= LONG_MAX - height)
                bounds.caret = RECT{x, y, x + qMax(1L, width), y + height};
            VariantClear(&state);
        }
    }
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> focused;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.GetAddressOf())))) return bounds;
    if (FAILED(automation->GetFocusedElement(focused.GetAddressOf())) || !focused) return bounds;
    BOOL hasFocus = FALSE;
    int process = 0;
    if (FAILED(focused->get_CurrentHasKeyboardFocus(&hasFocus)) || !hasFocus
        || FAILED(focused->get_CurrentProcessId(&process))
        || static_cast<DWORD>(process) != targetProcess) return bounds;

    ComPtr<IUIAutomationTextPattern2> pattern;
    if (!bounds.caret && SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPattern2Id,
            __uuidof(IUIAutomationTextPattern2), reinterpret_cast<void **>(pattern.GetAddressOf()))) && pattern) {
        BOOL active = FALSE;
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(pattern->GetCaretRange(&active, range.GetAddressOf())) && active && range) {
            SAFEARRAY *rectangles = nullptr;
            if (SUCCEEDED(range->GetBoundingRectangles(&rectangles)))
                bounds.caret = firstAutomationRect(rectangles);
            if (rectangles) SafeArrayDestroy(rectangles);
        }
    }

    CONTROLTYPEID controlType = 0;
    RECT controlRect{};
    if (SUCCEEDED(focused->get_CurrentControlType(&controlType))
        && (controlType == UIA_ComboBoxControlTypeId || controlType == UIA_EditControlTypeId
            || controlType == UIA_TextControlTypeId || controlType == UIA_CustomControlTypeId
            || controlType == UIA_PaneControlTypeId || controlType == UIA_DocumentControlTypeId
            || (controlType == UIA_GroupControlTypeId && bounds.caret))
        && SUCCEEDED(focused->get_CurrentBoundingRectangle(&controlRect))
        && !IsRectEmpty(&controlRect)) bounds.control = controlRect;
    return bounds;
}

std::shared_ptr<InputBoundsQuery> startInputBoundsQuery(HWND target) {
    // 外部提供程序可能长期无响应；只允许一个查询，后台线程不持有窗口或 Qt 对象。
    static std::weak_ptr<InputBoundsQuery> pending;
    if (!pending.expired()) return {};
    auto query = std::make_shared<InputBoundsQuery>();
    pending = query;
    try {
        std::thread([query, target] {
            query->bounds = automationInputBounds(target);
            query->finished.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error &) {
        return {};
    }
    return query;
}

}

void MainWindow::positionPanel(bool preferCursor, std::optional<RECT> resolvedBounds,
                               std::optional<RECT> controlBounds) {
    POINT point{};
    bool hasPoint = false;
    std::optional<RECT> inputBounds = resolvedBounds;
    if (preferCursor) {
        inputBounds.reset();
        hasPoint = GetCursorPos(&point) != FALSE;
    }
    if (!hasPoint && !inputBounds && targetWindow_) {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        const DWORD thread = GetWindowThreadProcessId(targetWindow_, nullptr);
        if (!inputBounds && thread && GetGUIThreadInfo(thread, &info) && info.hwndCaret) {
            RECT caret = info.rcCaret;
            MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT *>(&caret), 2);
            inputBounds = caret;
        }
    }
    if (inputBounds) {
        point = {inputBounds->left, inputBounds->bottom};
        hasPoint = true;
    }
    if (!hasPoint) hasPoint = GetCursorPos(&point) != FALSE;
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;
    RECT area = info.rcWork;
    // 用原生显示器句柄匹配屏幕，避免型号名称不唯一及混合 DPI 坐标歧义。
    QScreen *screen = nullptr;
    for (auto *candidate : QGuiApplication::screens()) {
        const auto *native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor) {
            screen = candidate;
            break;
        }
    }
    const qreal scale = screen ? screen->devicePixelRatio() : devicePixelRatioF();
    // 每次按目标工作区重算，回到大屏时恢复默认尺寸，不继承小屏的收缩结果。
    const int margin = qRound(8 * scale);
    const int availableWidth = qMax(1L, area.right - area.left - 2 * margin);
    const int availableHeight = qMax(1L, area.bottom - area.top - 2 * margin);
    setFixedSize(qMin(360, qMax(1, qFloor(availableWidth / scale))),
                 qMin(500, qMax(1, qFloor(availableHeight / scale))));
    if (area.right - area.left > 2 * margin && area.bottom - area.top > 2 * margin) {
        InflateRect(&area, -margin, -margin);
    }
    const int panelWidth = qRound(width() * scale);
    const int panelHeight = qRound(height() * scale);
    const int gap = qRound(14 * scale);
    int x = preferCursor ? point.x - panelWidth / 2 : point.x;
    int y = preferCursor ? point.y - panelHeight - gap : point.y + gap;
    if (inputBounds) {
        if (x + panelWidth > area.right) x = inputBounds->right - panelWidth;
        if (y + panelHeight > area.bottom) y = inputBounds->top - panelHeight - gap;
    }
    x = qBound(area.left, x, qMax(area.left, area.right - panelWidth));
    y = qBound(area.top, y, qMax(area.top, area.bottom - panelHeight));
    if (!preferCursor && inputBounds) {
        // 先避开整个输入框；区域过大时退回避开插入光标，不能因边缘裁剪再次覆盖它。
        bool positioned = false;
        const RECT regions[]{controlBounds.value_or(*inputBounds), *inputBounds};
        const int regionCount = EqualRect(&regions[0], &regions[1]) ? 1 : 2;
        for (int region = 0; region < regionCount; ++region) {
            const RECT &bounds = regions[region];
            const QRect excluded(bounds.left - margin, bounds.top - margin,
                                 qMax(1L, bounds.right - bounds.left) + 2 * margin,
                                 qMax(1L, bounds.bottom - bounds.top) + 2 * margin);
            const QPoint candidates[]{
                {point.x, excluded.bottom() + qRound(6 * scale) + 1},
                {point.x, excluded.top() - panelHeight - qRound(6 * scale)},
                {excluded.right() + qRound(6 * scale) + 1, point.y},
                {excluded.left() - panelWidth - qRound(6 * scale), point.y}};
            for (const QPoint &candidate : candidates) {
                const int candidateX = qBound(area.left, candidate.x(), qMax(area.left, area.right - panelWidth));
                const int candidateY = qBound(area.top, candidate.y(), qMax(area.top, area.bottom - panelHeight));
                if (QRect(candidateX, candidateY, panelWidth, panelHeight).intersects(excluded)) continue;
                x = candidateX;
                y = candidateY;
                positioned = true;
                break;
            }
            if (positioned) break;
        }
    }
    // 按目标屏幕转换物理坐标，并通过 Qt 更新缓存，避免 show() 恢复隐藏前的位置。
    const QPoint origin = screen ? screen->geometry().topLeft()
                                : QPoint(info.rcMonitor.left, info.rcMonitor.top);
    move(origin + QPoint(qRound((x - info.rcMonitor.left) / scale),
                         qRound((y - info.rcMonitor.top) / scale)));
}

std::optional<RECT> MainWindow::capturePasteTarget(HWND window) {
    DWORD process = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    if (!thread || process == GetCurrentProcessId()) return {};
    targetWindow_ = window;
    targetFocusWindow_ = nullptr;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread, &info)) return {};
    if (info.hwndFocus && (info.hwndFocus == window || IsChild(window, info.hwndFocus)))
        targetFocusWindow_ = info.hwndFocus;
    if (!info.hwndCaret) return {};
    RECT caret = info.rcCaret;
    MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT *>(&caret), 2);
    return caret;
}

void MainWindow::showPanel(bool fromTray, HWND requestedTarget) {
    if (auto *modal = QApplication::activeModalWidget()) { activatePanel(modal); return; }
    if (auto *popup = QApplication::activePopupWidget()) {
        if (!passiveBrowsing_ || !isPanelWindow(reinterpret_cast<HWND>(popup->internalWinId()))) activatePanel(popup);
        return;
    }
    if (settingsPage_) {
        // 托盘和全局快捷键只激活当前设置页，不重新定位或抢走设置控件的焦点。
        show();
        raise();
        activatePanel();
        return;
    }
    if (suppressClipboardCapture_) return;
    pasteGeneration_->fetchAndAddRelaxed(1);
    const int generation = ++panelShowGeneration_;
    const HWND foreground = requestedTarget && IsWindow(requestedTarget) ? requestedTarget : GetForegroundWindow();
    const bool hasTarget = foreground && foreground != reinterpret_cast<HWND>(winId());
    const auto inputBounds = hasTarget ? capturePasteTarget(foreground) : std::nullopt;
    std::optional<RECT> controlBounds;
    RECT nativeControl{};
    if (!fromTray && hasTarget && targetFocusWindow_ && targetFocusWindow_ != foreground) {
        wchar_t className[128]{};
        GetClassNameW(targetFocusWindow_, className, 128);
        const QString controlClass = QString::fromWCharArray(className);
        // 仅把原生输入控件当成输入框，浏览器视口等包装窗口应继续查询 UIA。
        const bool nativeInput = controlClass.compare(QStringLiteral("Edit"), Qt::CaseInsensitive) == 0
            || controlClass.compare(QStringLiteral("ComboBox"), Qt::CaseInsensitive) == 0
            || controlClass.startsWith(QStringLiteral("RichEdit"), Qt::CaseInsensitive);
        if (nativeInput && GetWindowRect(targetFocusWindow_, &nativeControl) && !IsRectEmpty(&nativeControl))
            controlBounds = nativeControl;
    }
    const auto shown = std::make_shared<bool>(false);
    const auto present = [this, fromTray, foreground, generation, shown](std::optional<RECT> bounds,
                                                                       std::optional<RECT> control) {
        if (*shown || generation != panelShowGeneration_) return;
        *shown = true;
        // 等待定位期间用户已切换窗口时，丢弃这次唤出，避免抢回焦点。
        if (!fromTray && GetForegroundWindow() != foreground) return;
        // 默认只展示历史；明确进入搜索等输入操作后，才激活窗口。
        if (!setPassiveBrowsing(!isVisible() || passiveBrowsing_ || !isPanelWindow(foreground))) {
            showStatus(AppLocalization::get(QStringLiteral("PassiveBrowsingUnavailable")));
            return;
        }
        positionPanel(fromTray, bounds, control);
        if (!isVisible()) show();
        if (passiveBrowsing_)
            SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else activatePanel();
        if (!startupUpdateCheckRequested_) {
            startupUpdateCheckRequested_ = true;
            emit firstPanelShown();
        }
        if (isPinned_) SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE);
        // 无新记录时继续浏览原位置；隐藏期间有新记录才重新展示最新内容。
        if (newHistoryAvailable_) {
            searchTimer_.stop();
            const QSignalBlocker blocker(searchBox_);
            searchBox_->clear();
            historyList_->verticalScrollBar()->setValue(0);
            if (model_->rowCount() > 0) historyList_->setCurrentIndex(model_->index(0));
            newHistoryAvailable_ = false;
            historyDirty_ = true;
        }
        if (historyDirty_) refreshHistory();
        if (!passiveBrowsing_) historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
        updateVisibleThumbnails();
    };
    if (fromTray || !hasTarget || (inputBounds && controlBounds)) {
        present(inputBounds, controlBounds);
        return;
    }
    // 无障碍接口查询限时等待，且只在显示前定位一次。
    const auto query = startInputBoundsQuery(foreground);
    if (!query) { present(inputBounds, controlBounds); return; }
    auto *poll = new QTimer(this);
    poll->setInterval(10);
    connect(poll, &QTimer::timeout, this, [this, query, poll, present, inputBounds, controlBounds, generation, foreground] {
        if (!query->finished.load(std::memory_order_acquire)) return;
        poll->stop();
        if (generation != panelShowGeneration_ || foreground != targetWindow_) return;
        // 输入框范围只参与避让，不能替代已确认活跃的插入光标作为定位锚点。
        const auto resolved = query->bounds.caret ? query->bounds.caret : inputBounds;
        present(resolved, query->bounds.control ? query->bounds.control : controlBounds);
    });
    poll->start();
    QTimer::singleShot(100, poll, [poll, present, inputBounds, controlBounds] {
        poll->stop();
        poll->deleteLater();
        present(inputBounds, controlBounds);
    });
}

