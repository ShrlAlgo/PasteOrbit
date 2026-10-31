#include "main_window.h"

#include "history_delegate.h"
#include "history_model.h"
#include "localization.h"
#include "settings_dialog.h"
#include "clipboard_content.h"
#include "paste_target.h"
#include "global_hotkey.h"
#include "portable_icons.h"


#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QButtonGroup>
#include <QDateTime>
#include <QDataStream>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QFontDatabase>
#include <QSettings>
#include <QStyleHints>
#include <QShortcut>
#include <QSet>
#include <QScrollBar>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStyle>
#include <QStyleFactory>
#include <QStackedLayout>
#include <QTextLayout>
#include <QScreen>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#include <shlobj.h>
#include <windows.h>
#else
#include "macos/desktop_integration.h"
#endif

using namespace ClipboardContent;

namespace {
constexpr int HotkeyId = 0x504F;
constexpr int HistoryPageSize = 30;
constexpr int TrayResumeMilliseconds = 10 * 60 * 1000;
const QString WindowsRtfMime = QStringLiteral("application/x-qt-windows-mime;value=\"Rich Text Format\"");

QByteArray clipboardRtf(const QMimeData *mime) {
    for (const auto &format : {WindowsRtfMime, QStringLiteral("text/rtf"), QStringLiteral("application/rtf")}) {
        const auto bytes = mime->data(format);
        if (!bytes.isEmpty()) return bytes;
    }
    return {};
}

QStyle *popupStyle() {
    static QStyle *style = [] {
        auto *fusion = QStyleFactory::create(QStringLiteral("Fusion"));
        fusion->setParent(qApp);
        return fusion;
    }();
    return style;
}

class PopupMenuFrame final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            auto *menu = static_cast<QMenu *>(watched);
            const QRect anchor = menu->property("rowMenuAnchor").toRect();
            if (anchor.isValid() && menu->isVisible() && menu->screen()) {
                // 异步追加项目改变菜单大小后，仍固定对齐同一个按钮；空间不足时向上展开。
                const auto *screen = QGuiApplication::screenAt(anchor.center());
                const QRect available = (screen ? screen : menu->screen())->availableGeometry();
                const int x = qBound(available.left(), anchor.right() - menu->width() + 1,
                                     qMax(available.left(), available.right() - menu->width() + 1));
                int y = anchor.bottom() + 3;
                if (y + menu->height() > available.bottom() + 1) y = anchor.top() - menu->height() - 2;
                y = qBound(available.top(), y, qMax(available.top(), available.bottom() - menu->height() + 1));
                if (menu->pos() != QPoint(x, y)) menu->move(x, y);
            }
            auto *parentMenu = qobject_cast<QMenu *>(menu->parentWidget());
            if (parentMenu && parentMenu->isVisible() && menu->isVisible() && menu->screen()) {
                // Qt 按菜单项边缘定位会盖住外边框；改按窗口边缘留缝，仍遵守屏幕边界。
                const QRect parentRect = parentMenu->frameGeometry();
                const QRect menuRect = menu->frameGeometry();
                const QRect available = menu->screen()->availableGeometry();
                const int left = parentRect.left() - menuRect.width() - 2;
                const int right = parentRect.right() + 1 + 2;
                const auto fits = [&](int x) {
                    return x >= available.left() && x + menuRect.width() <= available.right() + 1;
                };
                const bool opensLeft = menuRect.center().x() < parentRect.center().x();
                int x = opensLeft ? left : right;
                if (!fits(x)) x = opensLeft ? right : left;
                if (fits(x) && x != menu->x()) menu->move(x, menu->y());
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void stylePopupMenu(QMenu *menu, const QPalette &palette) {
    menu->setStyle(popupStyle());
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlag(Qt::FramelessWindowHint);
    menu->setWindowFlag(Qt::NoDropShadowWindowHint);
    menu->setAutoFillBackground(false);
    if (!menu->property("roundedPopupFrame").toBool()) {
        menu->installEventFilter(new PopupMenuFrame(menu));
        menu->setProperty("roundedPopupFrame", true);
    }
    menu->setPalette(palette);
    menu->setStyleSheet(QStringLiteral(
        "QMenu{background:%1;color:%2;border:1px solid %3;border-radius:8px;padding:4px;}"
        "QMenu::item{padding:6px 12px 6px 24px;}"
        "QMenu::indicator{width:0;height:0;image:none;}"
        "QMenu::item:checked{color:%5;}"
        "QMenu::item:selected{background:%4;border-radius:4px;}"
    ).arg(palette.color(QPalette::Base).name(), palette.color(QPalette::Text).name(),
          palette.color(QPalette::Mid).name(), palette.color(QPalette::AlternateBase).name(),
          palette.color(QPalette::Highlight).name()));
}

QIcon headerGlyphIcon(QChar glyph, const QColor &color, const QWidget *widget) {
    const qreal scale = widget->devicePixelRatioF();
    QPixmap pixmap(qRound(20 * scale), qRound(20 * scale));
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
#ifdef Q_OS_WIN
    QFont font;
    font.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    font.setPixelSize(16);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, QString(glyph));
#else
    drawPortableIcon(&painter, QRectF(0, 0, 20, 20), glyph, color);
#endif
    return QIcon(pixmap);
}

QLabel *createSettingsSnapshot(QWidget *page, QWidget *parent, const QPoint &position) {
    auto *snapshot = new QLabel(parent);
    snapshot->setPixmap(page->grab());
    snapshot->setFixedSize(page->size());
    snapshot->move(position);
    snapshot->show();
    return snapshot;
}

QByteArray encodePng(const QImage &image) {
    QByteArray bytes;
    QBuffer output(&bytes);
    if (output.open(QIODevice::WriteOnly)) {
        QImageWriter writer(&output, "png");
        writer.setCompression(3);
        writer.write(image);
    }
    return bytes;
}

QImage readBoundedImage(const QByteArray &content, const QSize &target) {
    QBuffer buffer;
    buffer.setData(content);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    if (target.isValid()) reader.setScaledSize(target);
    return reader.read();
}

QString imageMimeType(const QByteArray &content) {
    if (content.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) return QStringLiteral("image/png");
    if (content.startsWith(QByteArray::fromHex("ffd8ff"))) return QStringLiteral("image/jpeg");
    if (content.startsWith("BM")) return QStringLiteral("image/bmp");
    if (content.startsWith("GIF8")) return QStringLiteral("image/gif");
    if (content.size() >= 12 && content.mid(0, 4) == "RIFF" && content.mid(8, 4) == "WEBP") return QStringLiteral("image/webp");
    return QStringLiteral("image/png");
}

void encodeMixedImage(ClipboardCapture &capture) {
    if (capture.content.isEmpty() || (capture.text.isEmpty() && capture.html.isEmpty() && capture.rtf.isEmpty())) return;
    QByteArray encoded(MixedImagePayloadPrefix);
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    QDataStream stream(&buffer);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << capture.content << capture.text << capture.html << capture.rtf;
    if (stream.status() == QDataStream::Ok) capture.content = std::move(encoded);
}

void prepareImageCapture(ClipboardCapture &capture, const QImage &fallback) {
    if (capture.content.isEmpty() && !fallback.isNull()) capture.content = encodePng(fallback);
    if (capture.content.isEmpty()) return;
    QImage image;
    QSize originalSize;
    if (!fallback.isNull()) {
        originalSize = fallback.size();
        const QSize previewSize = boundedSize(originalSize, 1600, 0, 4'000'000);
        image = previewSize == originalSize ? fallback : fallback.scaled(previewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        QBuffer buffer;
        buffer.setData(capture.content);
        if (buffer.open(QIODevice::ReadOnly)) {
            QImageReader reader(&buffer);
            reader.setAutoTransform(true);
            originalSize = reader.size();
            const QSize previewSize = boundedSize(originalSize, 1600, 0, 4'000'000);
            if (previewSize.isValid()) reader.setScaledSize(previewSize);
            image = reader.read();
        }
    }
    if (image.isNull()) return;
    if (originalSize.isValid() && image.size() != originalSize) {
        capture.imagePreview = encodePng(image);
        if (capture.imagePreview.size() >= capture.content.size()) capture.imagePreview.clear();
    }
    const QSize thumbnailSize = boundedSize(image.size(), 240, 96, 240 * 96);
    if (thumbnailSize.isValid()) {
        const QImage thumbnail = image.size() == thumbnailSize
            ? image : image.scaled(thumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        capture.thumbnail = encodePng(thumbnail);
    }
}

#ifdef Q_OS_WIN
MainWindow *winVHookTarget = nullptr;
MainWindow *passivePanelTarget = nullptr;

Qt::KeyboardModifiers panelKeyModifiers() {
    Qt::KeyboardModifiers modifiers;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= Qt::ControlModifier;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= Qt::ShiftModifier;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= Qt::AltModifier;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) modifiers |= Qt::MetaModifier;
    return modifiers;
}

int panelQtKey(DWORD key) {
    if (key >= '0' && key <= '9') return Qt::Key_0 + int(key - '0');
    if (key >= 'A' && key <= 'Z') return Qt::Key_A + int(key - 'A');
    if (key >= VK_F1 && key <= VK_F24) return Qt::Key_F1 + int(key - VK_F1);
    switch (key) {
    case VK_LEFT: return Qt::Key_Left;
    case VK_RIGHT: return Qt::Key_Right;
    case VK_UP: return Qt::Key_Up;
    case VK_DOWN: return Qt::Key_Down;
    case VK_HOME: return Qt::Key_Home;
    case VK_END: return Qt::Key_End;
    case VK_PRIOR: return Qt::Key_PageUp;
    case VK_NEXT: return Qt::Key_PageDown;
    case VK_RETURN: return Qt::Key_Return;
    case VK_ESCAPE: return Qt::Key_Escape;
    case VK_SPACE: return Qt::Key_Space;
    case VK_TAB: return Qt::Key_Tab;
    case VK_DELETE: return Qt::Key_Delete;
    case VK_INSERT: return Qt::Key_Insert;
    case VK_BACK: return Qt::Key_Backspace;
    case VK_APPS: return Qt::Key_Menu;
    default: return int(MapVirtualKeyW(key, MAPVK_VK_TO_CHAR) & 0x7fffffff);
    }
}

LRESULT CALLBACK interceptWinV(int code, WPARAM message, LPARAM data) {
    static bool intercepted = false;
    if (code >= 0) {
        const auto *key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
        const bool windowsKeyDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
        if (key && key->vkCode == 'V' && intercepted && (message == WM_KEYUP || message == WM_SYSKEYUP)) {
            intercepted = false;
            return 1;
        }
        if (winVHookTarget && key && key->vkCode == 'V' && windowsKeyDown
            && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)
            && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)
            && !(GetAsyncKeyState(VK_MENU) & 0x8000)
            && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
            if (!intercepted) {
                intercepted = true;
                // Shell 要看到完整的其他按键，否则释放 Win 时会误打开开始菜单。
                INPUT dummy[2]{};
                for (auto &input : dummy) { input.type = INPUT_KEYBOARD; input.ki.wVk = 0xFF; }
                dummy[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, dummy, sizeof(INPUT));
                MainWindow *target = winVHookTarget;
                const HWND foreground = GetForegroundWindow();
                QMetaObject::invokeMethod(target, [target, foreground] {
                    target->showPanel(false, foreground);
                }, Qt::QueuedConnection);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

QString processName(HWND window) {
    if (!window) return {};
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!ok) return {};
    const QString fullPath = QString::fromWCharArray(path, static_cast<int>(length));
    return fullPath.mid(fullPath.lastIndexOf(QLatin1Char('\\')) + 1, fullPath.size()).toCaseFolded();
}

#endif

PanelTarget foregroundTarget() {
#ifdef Q_OS_WIN
    return GetForegroundWindow();
#else
    return MacIntegration::foregroundApplication();
#endif
}

quint32 clipboardSequence() {
#ifdef Q_OS_WIN
    return GetClipboardSequenceNumber();
#else
    return MacIntegration::clipboardSequence();
#endif
}

struct PasteResult {
    QString unavailablePath;
    HistoryItem item;
    QByteArray content;
    ImageContent image;
    QImage decoded;
    QPair<QString, QByteArray> file;
};

#ifdef Q_OS_WIN
bool revealFile(const QString &path) {
    if (!QDir::isAbsolutePath(path)) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
    PIDLIST_ABSOLUTE item = nullptr;
    const QString nativePath = QDir::toNativeSeparators(path);
    // 文件夹直接打开；普通文件只定位，绝不按关联程序执行文件。
    if (QFileInfo(path).isDir())
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
            reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    const HRESULT parsed = SHParseDisplayName(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, &item, 0, nullptr);
    const auto freeItem = qScopeGuard([&] { CoTaskMemFree(item); });
    return SUCCEEDED(parsed) && SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
}
#else
bool revealFile(const QString &path) { return MacIntegration::revealFile(path); }
#endif
}

#ifdef Q_OS_WIN
// 不激活浏览时只接管面板操作键，普通输入仍交给原应用；吞掉的按键成对释放。
LRESULT CALLBACK MainWindow::panelKeyboardProc(int code, WPARAM message, LPARAM data) {
    auto *panel = passivePanelTarget;
    const auto *input = reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
    if (code < 0 || !panel || !input || input->vkCode >= 256 || input->dwExtraInfo == PasteTarget::InputMarker)
        return CallNextHookEx(nullptr, code, message, data);
    const auto vk = input->vkCode;
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        if (!panel->swallowedPanelKeys_.test(vk)) return CallNextHookEx(nullptr, code, message, data);
        panel->swallowedPanelKeys_.reset(vk);
        QMetaObject::invokeMethod(panel, [panel] { panel->updatePanelInputHooks(); }, Qt::QueuedConnection);
        return 1;
    }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        && panel->swallowedPanelKeys_.test(vk) && !panel->passiveBrowsing_) return 1;
    if (panel->passiveBrowsing_ && !panel->targetWindow_) panel->capturePasteTarget(GetForegroundWindow());
    auto *popup = QApplication::activePopupWidget();
    if ((message != WM_KEYDOWN && message != WM_SYSKEYDOWN) || !panel->passiveBrowsing_
        || !panel->isVisible() || panel->settingsPage_ || QApplication::activeModalWidget()
        || (popup && !panel->isPanelWindow((PanelTarget)popup->internalWinId()))
        || GetForegroundWindow() != panel->targetWindow_)
        return CallNextHookEx(nullptr, code, message, data);
    const auto modifiers = panelKeyModifiers();
    if (modifiers.testFlag(Qt::MetaModifier)) return CallNextHookEx(nullptr, code, message, data);
    const int key = panelQtKey(vk);
    const bool search = (key == Qt::Key_F && modifiers == Qt::ControlModifier)
        || (key == Qt::Key_Tab && (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier));
    bool handled = search || (modifiers == Qt::NoModifier
        && ((key >= Qt::Key_1 && key <= Qt::Key_9) || key == Qt::Key_Escape || key == Qt::Key_Menu
            || key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down
            || key == Qt::Key_Home || key == Qt::Key_End || key == Qt::Key_PageUp || key == Qt::Key_PageDown));
    if (popup && modifiers == Qt::NoModifier && (key == Qt::Key_Return || key == Qt::Key_Space)) handled = true;
    if (!handled && key) {
        QString pressed = QKeySequence(modifiers | key).toString(QKeySequence::PortableText);
        pressed.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
        const auto normalized = AppSettings::normalizedShortcut(pressed);
        for (const auto *shortcut : {&panel->settings_.pasteShortcut, &panel->settings_.plainTextPasteShortcut,
                                    &panel->settings_.previewShortcut, &panel->settings_.pinShortcut,
                                    &panel->settings_.pasteAsFileShortcut, &panel->settings_.moreShortcut, &panel->settings_.openShortcut})
            handled = handled || normalized == AppSettings::normalizedShortcut(*shortcut);
    }
    if (!handled) return CallNextHookEx(nullptr, code, message, data);
    const bool repeat = panel->swallowedPanelKeys_.test(vk);
    panel->swallowedPanelKeys_.set(vk);
    const int generation = panel->panelShowGeneration_;
    QMetaObject::invokeMethod(panel, [panel, key, modifiers, repeat, search, generation,
                                    menu = QPointer<QWidget>(popup), hadMenu = popup != nullptr] {
        if (!panel->isVisible() || !panel->passiveBrowsing_ || generation != panel->panelShowGeneration_
            || GetForegroundWindow() != panel->targetWindow_) return;
        if (hadMenu && (!menu || !menu->isVisible())) return;
        if (search) {
            if (menu) menu->close();
            panel->activatePanel(panel->searchBox_);
            return;
        }
        QKeyEvent event(QEvent::KeyPress, key, modifiers, {}, repeat);
        QApplication::sendEvent(menu ? menu.data() : panel->historyStack_->currentWidget(), &event);
    }, Qt::QueuedConnection);
    return 1;
}

// 未激活窗口收不到完整的外部点击和滚轮消息，仅在浏览期间补齐这两类交互。
LRESULT CALLBACK MainWindow::panelMouseProc(int code, WPARAM message, LPARAM data) {
    auto *panel = passivePanelTarget;
    const auto *input = reinterpret_cast<const MSLLHOOKSTRUCT *>(data);
    if (code < 0 || !panel || !input || !panel->passiveBrowsing_ || !panel->isVisible()
        || QApplication::activeModalWidget())
        return CallNextHookEx(nullptr, code, message, data);
    auto *popup = QApplication::activePopupWidget();
    if (popup && !panel->isPanelWindow((PanelTarget)popup->internalWinId()))
        return CallNextHookEx(nullptr, code, message, data);
    const bool inside = panel->isPanelWindow(WindowFromPoint(input->pt));
    const bool pressed = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
        || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
    if (pressed) {
        if (inside) panel->capturePasteTarget(GetForegroundWindow());
        else if (!panel->isPinned_) {
            const int generation = panel->panelShowGeneration_;
            QMetaObject::invokeMethod(panel, [panel, generation] {
                if (panel->passiveBrowsing_ && generation == panel->panelShowGeneration_) panel->hidePanel();
            }, Qt::QueuedConnection);
        }
    }
    if (inside && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        // Qt 使用逻辑坐标，不能直接把钩子的物理像素坐标用于高 DPI 屏幕。
        const QPoint position = QCursor::pos();
        const int delta = static_cast<short>(HIWORD(input->mouseData));
        const QPoint angle = message == WM_MOUSEWHEEL ? QPoint(0, delta) : QPoint(delta, 0);
        const auto modifiers = panelKeyModifiers();
        const int generation = panel->panelShowGeneration_;
        QMetaObject::invokeMethod(panel, [panel, position, angle, modifiers, generation] {
            if (!panel->passiveBrowsing_ || !panel->isVisible() || generation != panel->panelShowGeneration_) return;
            auto *widget = QApplication::widgetAt(position);
            if (!widget || !panel->isPanelWindow(reinterpret_cast<HWND>(widget->window()->internalWinId()))) return;
            QWheelEvent event(QPointF(widget->mapFromGlobal(position)), QPointF(position), {}, angle,
                              QApplication::mouseButtons(), modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(widget, &event);
        }, Qt::QueuedConnection);
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool MainWindow::isPanelWindow(HWND window) const {
    const HWND root = window ? GetAncestor(window, GA_ROOT) : nullptr;
    auto *widget = root ? QWidget::find(reinterpret_cast<WId>(root)) : nullptr;
    return root && (root == reinterpret_cast<HWND>(internalWinId())
        || (hoverPreview_ && root == reinterpret_cast<HWND>(hoverPreview_->internalWinId()))
        || (widget && widget->property("passivePanelMenu").toBool()));
}

// 菜单也属于浏览操作，保持不激活；键盘事件由浏览钩子交给当前弹出菜单。
void MainWindow::preparePanelMenu(QMenu *menu) {
    if (!passiveBrowsing_) return;
    auto menus = menu->findChildren<QMenu *>();
    menus.prepend(menu);
    for (auto *item : menus) {
        item->setProperty("passivePanelMenu", true);
        item->setAttribute(Qt::WA_ShowWithoutActivating);
        item->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
        item->winId();
        // QWidget 保留 Popup 交互；原生 Tool 避免强制激活，并显式置顶以覆盖历史面板。
        auto *window = item->windowHandle();
        window->setFlags((window->flags() & ~Qt::WindowType_Mask) | Qt::Tool | Qt::FramelessWindowHint
                         | Qt::WindowStaysOnTopHint);
        const HWND handle = reinterpret_cast<HWND>(window->winId());
        SetWindowLongPtrW(handle, GWL_EXSTYLE, GetWindowLongPtrW(handle, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
}

void MainWindow::updatePanelInputHooks() {
    if (passiveBrowsing_) {
        passivePanelTarget = this;
        if (!panelKeyboardHook_) panelKeyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, panelKeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!panelMouseHook_) panelMouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, panelMouseProc, GetModuleHandleW(nullptr), 0);
    } else {
        if (panelMouseHook_) { UnhookWindowsHookEx(panelMouseHook_); panelMouseHook_ = nullptr; }
        // 关闭面板或进入搜索后，仍吞掉已接管按键的抬起，避免泄漏给原输入框。
        if (panelKeyboardHook_ && swallowedPanelKeys_.none()) { UnhookWindowsHookEx(panelKeyboardHook_); panelKeyboardHook_ = nullptr; }
        if (!panelKeyboardHook_) passivePanelTarget = nullptr;
    }
}

// 保持原窗口句柄不变，分别控制首次显示和后续鼠标点击是否激活。
bool MainWindow::setPassiveBrowsing(bool passive) {
    passiveBrowsing_ = passive;
    updatePanelInputHooks();
    if (passive && (!panelKeyboardHook_ || !panelMouseHook_)) {
        passiveBrowsing_ = false;
        updatePanelInputHooks();
        return false;
    }
    for (auto *window : {static_cast<QWidget *>(this), hoverPreview_}) {
        if (!window) continue;
        window->setAttribute(Qt::WA_ShowWithoutActivating, passive);
        const HWND handle = reinterpret_cast<HWND>(window->winId());
        const LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
        SetWindowLongPtrW(handle, GWL_EXSTYLE, passive ? style | WS_EX_NOACTIVATE : style & ~WS_EX_NOACTIVATE);
        SetWindowPos(handle, passive || isPinned_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return true;
}

// 只有用户明确进入搜索、设置或编辑等交互时，才把输入焦点交给本应用。
void MainWindow::activatePanel(QWidget *focus) {
    while (auto *popup = QApplication::activePopupWidget()) {
        if (!popup->property("passivePanelMenu").toBool()) break;
        popup->close();
    }
    setPassiveBrowsing(false);
    QWidget *window = focus ? focus->window() : this;
    window->raise();
    window->activateWindow();
    SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
    if (focus) focus->setFocus(Qt::ShortcutFocusReason);
}

#else
bool MainWindow::isPanelWindow(PanelTarget window) const {
    auto *widget = QWidget::find(static_cast<WId>(window));
    return widget && (widget == this || widget == hoverPreview_ || isAncestorOf(widget)
                      || widget->property("pasteOrbitMenu").toBool());
}

void MainWindow::preparePanelMenu(QMenu *menu) {
    menu->setProperty("pasteOrbitMenu", true);
    for (auto *child : menu->findChildren<QMenu *>()) child->setProperty("pasteOrbitMenu", true);
}
void MainWindow::updatePanelInputHooks() {}

bool MainWindow::setPassiveBrowsing(bool) {
    passiveBrowsing_ = false;
    return true;
}

void MainWindow::activatePanel(QWidget *focus) {
    auto *window = focus ? focus->window() : this;
    MacIntegration::activateApplication(static_cast<quintptr>(window->winId()));
    window->raise();
    window->activateWindow();
    if (focus) focus->setFocus(Qt::ShortcutFocusReason);
}

#endif

MainWindow::MainWindow(QString dataDirectory, AppSettings settings)
    : dataDirectory_(std::move(dataDirectory)),
      databasePath_(dataDirectory_ + QStringLiteral("/history.db")),
      settingsPath_(dataDirectory_ + QStringLiteral("/settings.json")),
      settings_(std::move(settings)), store_(databasePath_) {
    // 短查询独立排队；文件访问和内容解码串行，避免慢文件阻塞搜索。
    readPool_.setMaxThreadCount(1);
    contentPool_.setMaxThreadCount(1);
    writePool_.setMaxThreadCount(1);
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    buildUi();
    buildTray();
    applyTheme();
    qApp->installNativeEventFilter(this);
    createWinId();
    registerHotkey();
    updateWinVHook();
#ifdef Q_OS_WIN
    clipboardNotificationsAvailable_ = AddClipboardFormatListener(reinterpret_cast<HWND>(winId())) != FALSE;
#endif
    lastClipboardSequence_ = clipboardSequence();
#ifdef Q_OS_MACOS
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (settings_.themeMode == QStringLiteral("System")) applyTheme();
    });
#endif
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::scheduleCapture);
    // 系统通知负责即时捕获，隐藏时只保留低频漏通知补偿。
    clipboardPollTimer_.setInterval(clipboardNotificationsAvailable_ ? 2000 : 250);
    connect(&clipboardPollTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && dateIndicatorDay_ != QDate::currentDate()) updateDateIndicator();
#ifdef Q_OS_WIN
        if (passiveBrowsing_ && isVisible()) {
            const HWND foreground = GetForegroundWindow();
            if (foreground && !isPanelWindow(foreground) && foreground != targetWindow_) {
                if (isPinned_ || !targetWindow_) capturePasteTarget(foreground);
                else hidePanel();
            }
        }
#endif
        const quint32 sequence = clipboardSequence();
        if (sequence && sequence != lastClipboardSequence_) scheduleCapture();
    });
    clipboardPollTimer_.start();
    captureTimer_.setSingleShot(true);
    captureTimer_.setInterval(70);
    connect(&captureTimer_, &QTimer::timeout, this, &MainWindow::captureClipboard);
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(180);
    connect(&searchTimer_, &QTimer::timeout, this, &MainWindow::refreshHistory);
    pauseTimer_.setSingleShot(true);
    connect(&pauseTimer_, &QTimer::timeout, this, [this] { setTrayPaused(false); });
    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
#ifdef Q_OS_MACOS
        // macOS 在按下图标时已打开原生菜单，不能再同时唤出历史面板。
        if (trayIcon_->contextMenu()) return;
#endif
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) showPanel(true);
    });
    const QString error = store_.initialize();
    if (!error.isEmpty()) showStatus(error);
    else {
        // 启动后异步预载第一页，首次唤出面板即可显示已有记录。
        refreshHistory();
        const QString path = databasePath_;
        const int retention = settings_.retentionDays;
        const int maximum = settings_.maxHistoryEntries;
        writePool_.start([path, retention, maximum] {
            HistoryStore(path).cleanup(retention, maximum);
        });
    }
    cleanupPasteFiles();
}

MainWindow::~MainWindow() {
#ifdef Q_OS_WIN
    if (panelKeyboardHook_) UnhookWindowsHookEx(panelKeyboardHook_);
    if (panelMouseHook_) UnhookWindowsHookEx(panelMouseHook_);
    if (passivePanelTarget == this) passivePanelTarget = nullptr;
#endif
    closeHoverPreview();
    closingForExit_ = true;
    qApp->removeNativeEventFilter(this);
#ifdef Q_OS_WIN
    if (winVHook_) {
        UnhookWindowsHookEx(winVHook_);
        winVHook_ = nullptr;
    }
    if (winVHookTarget == this) winVHookTarget = nullptr;
#endif
    GlobalHotkey::remove(winId(), HotkeyId);
    GlobalHotkey::remove(winId(), HotkeyId + 1);
    GlobalHotkey::remove(winId(), HotkeyId + 2);
#ifdef Q_OS_WIN
    RemoveClipboardFormatListener(reinterpret_cast<HWND>(winId()));
#endif
    trayIcon_->hide();
    trayIcon_->setContextMenu(nullptr);
    delete trayMenu_;
    readPool_.waitForDone(1000);
    contentPool_.waitForDone(1000);
    writePool_.waitForDone(1000);
}

void MainWindow::beginStorageOperation() {
    sequenceQueue_.clear();
    sequenceCopyPending_ = false;
    ++sequenceGeneration_;
    suppressClipboardCapture_ = true;
    // 嵌入设置页执行备份时保留宿主窗口，避免连同进度和错误提示一起隐藏。
    if (!settingsPage_) hidePanel();
    restoreWasPaused_ = paused_;
    paused_ = true;
    captureTimer_.stop();
    // 先取消排队的读取，再等待已运行任务释放数据库，避免备份被过期预览拖住。
    historyGeneration_->fetchAndAddRelaxed(1);
    pasteGeneration_->fetchAndAddRelaxed(1);
    closeHoverPreview();
    model_->releaseThumbnails();
    readPool_.waitForDone();
    contentPool_.waitForDone();
    writePool_.waitForDone();
    QThreadPool::globalInstance()->waitForDone();
}

void MainWindow::finishStorageOperation(bool restored) {
    suppressClipboardCapture_ = false;
    paused_ = restoreWasPaused_;
    lastClipboardSequence_ = clipboardSequence();
    if (restored) {
        const AppSettings restoredSettings = AppSettings::load(settingsPath_);
        QString language = restoredSettings.language;
        if (language.isEmpty()) language = QLocale().name().startsWith(QStringLiteral("en"))
            ? QStringLiteral("en-US") : QStringLiteral("zh-CN");
        AppLocalization::load(language);
        applySettings(restoredSettings);
    }
    refreshHistory();
}

void MainWindow::buildUi() {
    setObjectName(QStringLiteral("PasteOrbitPanel"));
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(AppLocalization::get(QStringLiteral("ApplicationDisplayName")));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
#ifdef Q_OS_MACOS
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    setFixedSize(360, 500);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    historyPage_ = new QWidget(this);
    historyPage_->setObjectName(QStringLiteral("HistoryPage"));
    outer->addWidget(historyPage_);
    auto *root = new QVBoxLayout(historyPage_);
    root->setContentsMargins(12, 0, 12, 6);
    root->setSpacing(0);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("Header"));
    header->installEventFilter(this);
    header->setFixedHeight(48);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    auto *appIcon = new QLabel(header);
    appIcon->setFixedSize(20, 20);
    appIcon->setPixmap(QIcon(QStringLiteral(":/PasteOrbit.ico")).pixmap(20, 20));
    auto *title = new QLabel(AppLocalization::get(QStringLiteral("ApplicationDisplayName")), header);
    title->setObjectName(QStringLiteral("AppTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(17);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    QFont iconFont;
    iconFont.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    iconFont.setPixelSize(14);
    QFont filterIconFont = iconFont;
    filterIconFont.setPixelSize(18);
    headerLayout->addWidget(appIcon);
    headerLayout->addWidget(title);
    headerLayout->addSpacing(12);
    settingsButton_ = new QToolButton(header);
    settingsButton_->setText(QChar(0xE713));
    settingsButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    settingsButton_->setIconSize(QSize(20, 20));
    settingsButton_->setObjectName(QStringLiteral("HeaderButton"));
    settingsButton_->setToolTip(AppLocalization::get(QStringLiteral("MainSettingsButtonTooltip")));
    settingsButton_->setFont(iconFont);
    connect(settingsButton_, &QToolButton::clicked, this, &MainWindow::settingsRequested);
    pinButton_ = new QToolButton(header);
    pinButton_->setText(QChar(0xE718));
    pinButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    pinButton_->setIconSize(QSize(20, 20));
    pinButton_->setCheckable(true);
    pinButton_->setObjectName(QStringLiteral("HeaderButton"));
    pinButton_->setToolTip(AppLocalization::get(QStringLiteral("MainWindowPinToggleTooltip")));
    pinButton_->setFont(iconFont);
    connect(pinButton_, &QToolButton::toggled, this, [this](bool pinned) {
        isPinned_ = pinned;
        pinButton_->setText(pinned ? QChar(0xE77A) : QChar(0xE718));
        pinButton_->setIcon(headerGlyphIcon(pinned ? QChar(0xE77A) : QChar(0xE718),
                                            qApp->palette().color(QPalette::Text), pinButton_));
#ifdef Q_OS_WIN
        if (isVisible()) SetWindowPos(reinterpret_cast<HWND>(winId()), pinned || passiveBrowsing_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                                      0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
        MacIntegration::setAlwaysOnTop(winId(), pinned);
#endif
    });
    headerLayout->addWidget(settingsButton_);
    headerLayout->addWidget(pinButton_);
    headerLayout->addStretch(1);
    root->addWidget(header);

    auto *searchFrame = new QWidget(this);
    auto *searchLayout = new QVBoxLayout(searchFrame);
    searchLayout->setContentsMargins(0, 0, 0, 10);
    searchBox_ = new QLineEdit(searchFrame);
    searchBox_->setPlaceholderText(AppLocalization::get(QStringLiteral("MainSearchBoxPlaceholder")));
    searchBox_->setClearButtonEnabled(true);
    searchBox_->installEventFilter(this);
    connect(searchBox_, &QLineEdit::textChanged, this, [this] {
        // 防抖只延迟发起查询，旧结果与旧游标立即失效，避免混用新关键词。
        historyGeneration_->fetchAndAddRelaxed(1);
        historyDirty_ = true;
        loadingPage_ = true;
        updateDateIndicator();
        loadingMore_ = false;
        nextCursor_.reset();
        model_->setQuickPasteEnabled(false);
        closeHoverPreview();
        searchTimer_.start();
    });
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->addWidget(searchBox_, 1);
    sourceButton_ = new QToolButton(searchFrame);
    sourceButton_->setIconSize(QSize(18, 18));
    sourceButton_->setObjectName(QStringLiteral("HeaderButton"));
    sourceButton_->setToolTip(AppLocalization::get(QStringLiteral("FilterSource")));
    sourceButton_->setAccessibleName(sourceButton_->toolTip());
    sourceButton_->setCheckable(true);
    sourceButton_->setFocusPolicy(Qt::NoFocus);
    connect(sourceButton_, &QToolButton::clicked, this, &MainWindow::openSourceMenu);
    searchRow->addWidget(sourceButton_);
    searchLayout->addLayout(searchRow);
    sourceFilterChip_ = new QToolButton(searchFrame);
    sourceFilterChip_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    sourceFilterChip_->hide();
    connect(sourceFilterChip_, &QToolButton::clicked, this, [this] { setSourceFilter(std::nullopt); });
    searchLayout->addWidget(sourceFilterChip_);
    root->addWidget(searchFrame);

    auto *filterRow = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 10);
    filterLayout->setSpacing(0);
    filterButtons_ = new QButtonGroup(this);
    filterButtons_->setExclusive(true);
    const QStringList labels{QString(QChar(0xE8A9)), QString(QChar(0xE8D2)), QString(QChar(0xE8B9)), QString(QChar(0xE8B7)), QString(QChar(0xE734))};
    const QStringList tooltips{QStringLiteral("FilterAllButtonTooltip"), QStringLiteral("FilterTextButtonTooltip"),
                               QStringLiteral("FilterImageButtonTooltip"), QStringLiteral("FilterFilesButtonTooltip"),
                               QStringLiteral("FilterFavoritesButtonTooltip")};
    // QButtonGroup 将 -1 保留给“无选择”，按钮序号通过偏移映射到记录类型。
    for (int i = 0; i < labels.size(); ++i) {
        auto *button = new QToolButton(filterRow);
        button->setText(labels.at(i));
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("FilterButton"));
        button->setFont(filterIconFont);
        button->setToolTip(AppLocalization::get(tooltips.at(i)));
        // 分类切换由列表和空状态处理，筛选按钮不占用键盘焦点。
        button->setFocusPolicy(Qt::NoFocus);
        filterButtons_->addButton(button, i);
        filterLayout->addWidget(button);
    }
    filterButtons_->button(0)->setChecked(true);
    connect(filterButtons_, &QButtonGroup::idClicked, this, [this](int id) { setFilter(id - 1); });
    filterLayout->addStretch(1);
    clearButton_ = new QToolButton(filterRow);
    clearButton_->setText(QChar(0xE75C));
    clearButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    clearButton_->setIconSize(QSize(20, 20));
    clearButton_->setObjectName(QStringLiteral("HeaderButton"));
    clearButton_->setToolTip(AppLocalization::get(QStringLiteral("ClearHistoryButtonTooltip")));
    clearButton_->setFont(iconFont);
    connect(clearButton_, &QToolButton::clicked, this, &MainWindow::clearCurrentList);
    filterLayout->addWidget(clearButton_);
    root->addWidget(filterRow);

    auto *listHost = new QWidget(this);
    auto *listLayout = new QVBoxLayout(listHost);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    dateLabel_ = new QLabel(listHost);
    dateLabel_->setObjectName(QStringLiteral("HistoryDate"));
    dateLabel_->setFixedHeight(22);
    dateLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    dateLabel_->setMargin(2);
    QFont dateFont = font();
    dateFont.setPixelSize(12);
    dateLabel_->setFont(dateFont);
    listLayout->addWidget(dateLabel_);
    historyList_ = new QListView(listHost);
    historyList_->setObjectName(QStringLiteral("HistoryList"));
    historyList_->setSelectionMode(QAbstractItemView::SingleSelection);
    historyList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    historyList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    historyList_->setUniformItemSizes(false);
    historyList_->setSpacing(0);
    historyList_->setMouseTracking(true);
    historyList_->viewport()->setMouseTracking(true);
    historyList_->installEventFilter(this);
    historyList_->viewport()->installEventFilter(this);
    model_ = new HistoryModel(databasePath_, this);
    delegate_ = new HistoryDelegate(historyList_);
    historyList_->setModel(model_);
    historyList_->setItemDelegate(delegate_);
    // Windows 的 Shift+F10 必须正式注册，否则 Qt 会交回系统，不发送 KeyPress。
    moreShortcut_ = new QShortcut(QKeySequence::fromString(QString(settings_.moreShortcut).remove(QLatin1Char(' ')), QKeySequence::PortableText), historyList_);
    moreShortcut_->setContext(Qt::WidgetShortcut);
    moreShortcut_->setAutoRepeat(false);
    connect(moreShortcut_, &QShortcut::activated, this, [this] {
        const auto current = historyList_->currentIndex();
        if (!current.isValid()) return;
        openRowMenu(current.data(HistoryModel::IdRole).toString());
    });
    connect(model_, &QAbstractItemModel::modelAboutToBeReset, this, [this] { closeHoverPreview(); });
    connect(delegate_, &HistoryDelegate::actionTriggered, this, [this](const QString &id, int action) {
        if (action == HistoryDelegate::Preview) toggleHoverPreview(id);
        else if (action == HistoryDelegate::Pin) setPinned(id);
        else openRowMenu(id);
    });
    connect(historyList_, &QListView::clicked, this, [this](const QModelIndex &index) {
        const QPoint point = historyList_->viewport()->mapFromGlobal(QCursor::pos());
        if (delegate_->actionAt(historyList_->visualRect(index), point) >= 0) return;
        pasteRecord(index.data(HistoryModel::IdRole).toString());
    });
    connect(historyList_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        updateVisibleThumbnails();
        auto *bar = historyList_->verticalScrollBar();
        if (bar->maximum() > 0 && bar->value() >= bar->maximum() - qMax(40, bar->pageStep() / 3)) loadMoreHistory();
    });
    auto *stack = new QWidget(listHost);
    historyStack_ = new QStackedLayout(stack);
    historyStack_->setContentsMargins(0, 0, 0, 0);
    historyStack_->addWidget(historyList_);
    emptyLabel_ = new QLabel(AppLocalization::get(QStringLiteral("MainEmptyHistoryText")), stack);
    emptyLabel_->setObjectName(QStringLiteral("EmptyHistory"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setFocusPolicy(Qt::StrongFocus);
    emptyLabel_->installEventFilter(this);
    historyStack_->addWidget(emptyLabel_);
    listLayout->addWidget(stack);
    statusLabel_ = new QLabel(listHost);
    statusLabel_->setObjectName(QStringLiteral("StatusText"));
    statusLabel_->setMaximumHeight(0);
    root->addWidget(listHost, 1);
}

void MainWindow::buildTray() {
    if (!trayIcon_) trayIcon_ = new QSystemTrayIcon(QIcon(QStringLiteral(":/PasteOrbit.ico")), this);
    if (trayMenu_) {
        trayIcon_->setContextMenu(nullptr);
        delete trayMenu_;
        trayMenu_ = nullptr;
        pauseAction_ = nullptr;
    }
    updateTrayTooltip();
    // 避免弹出菜单继承透明面板样式及 Qlementine 的延迟点击/缩放处理。
    trayMenu_ = new QMenu();
    stylePopupMenu(trayMenu_, qApp->palette());
    auto *openAction = trayMenu_->addAction(AppLocalization::get(QStringLiteral("TrayOpenHistory")));
    connect(openAction, &QAction::triggered, this, [this] { showPanel(true); });
    pauseAction_ = trayMenu_->addAction(AppLocalization::get(QStringLiteral("TrayPauseMonitoring")));
    pauseAction_->setCheckable(true);
    pauseAction_->setChecked(paused_);
    connect(pauseAction_, &QAction::toggled, this, &MainWindow::setTrayPaused);
    trayMenu_->addSeparator();
    auto *settingsAction = trayMenu_->addAction(AppLocalization::get(QStringLiteral("TraySettings")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::settingsRequested);
    auto *updatesAction = trayMenu_->addAction(AppLocalization::get(QStringLiteral("TrayCheckForUpdates")));
    connect(updatesAction, &QAction::triggered, this, &MainWindow::checkUpdatesRequested);
    trayMenu_->addSeparator();
    auto *exitAction = trayMenu_->addAction(AppLocalization::get(QStringLiteral("TrayExit")));
    connect(exitAction, &QAction::triggered, this, &MainWindow::exitRequested);
    trayIcon_->setContextMenu(trayMenu_);
    if (!trayIcon_->isVisible()) trayIcon_->show();
}

void MainWindow::applyTheme() {
    if (hoverPreview_) closeHoverPreview();
    QToolTip::hideText();
    bool dark = settings_.themeMode == QStringLiteral("Dark");
    if (settings_.themeMode == QStringLiteral("System")) {
#ifdef Q_OS_WIN
        QSettings system(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                         QSettings::NativeFormat);
        dark = system.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#else
        dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#endif
    }
    const QColor page(dark ? QStringLiteral("#202020") : QStringLiteral("#F3F3F3"));
    const QColor surface(dark ? QStringLiteral("#2B2B2B") : QStringLiteral("#FFFFFF"));
    const QColor stroke(dark ? QStringLiteral("#505050") : QStringLiteral("#D8D8D8"));
    const QColor text(dark ? QStringLiteral("#F5F5F5") : QStringLiteral("#1A1A1A"));
    const QColor muted(dark ? QStringLiteral("#B8B8B8") : QStringLiteral("#6D6D6D"));
    const QColor hover(dark ? QStringLiteral("#3B3B3B") : QStringLiteral("#EAEAEA"));
    // 应用级主题会同步所有顶层页面与弹窗的调色板。
    if (auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style())) {
        auto theme = dark ? oclero::qlementine::Theme::makeDark()
                           : oclero::qlementine::Theme::makeLight();
        theme.primaryColor = QColor("#0078D4");
        // 关闭控件外圈泛光及其动画，保留普通边框和键盘焦点。
        theme.focusBorderWidth = 0;
        theme.focusAnimationDuration = 0;
        theme.backgroundColorMain1 = surface;
        theme.backgroundColorMain2 = page;
        theme.backgroundColorMain3 = hover;
        // 普通按钮和下拉框共用中性色，让默认底色融入卡片并保留交互层次。
        theme.neutralColor = dark ? QColor("#333333") : QColor("#F8F8F8");
        theme.neutralColorHovered = dark ? QColor("#3D3D3D") : QColor("#EFEFEF");
        theme.neutralColorPressed = dark ? QColor("#262626") : QColor("#E5E5E5");
        theme.neutralColorDisabled = surface;
        theme.secondaryColor = text;
        theme.secondaryColorDisabled = muted;
        theme.secondaryAlternativeColor = muted;
        theme.scrollBarThicknessFull = 8;
        theme.scrollBarThicknessSmall = 8;
        auto palette = theme.palette;
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::All, role, text);
        palette.setColor(QPalette::All, QPalette::Window, page);
        palette.setColor(QPalette::All, QPalette::Base, surface);
        palette.setColor(QPalette::All, QPalette::AlternateBase, hover);
        palette.setColor(QPalette::All, QPalette::Button, surface);
        palette.setColor(QPalette::All, QPalette::Mid, stroke);
        palette.setColor(QPalette::All, QPalette::PlaceholderText, muted);
        palette.setColor(QPalette::All, QPalette::Highlight, theme.primaryColor);
        palette.setColor(QPalette::All, QPalette::HighlightedText, Qt::white);
        // 自定义 tooltip 绘制读取这组配对色，避免深色主题出现同色文字与背景。
        palette.setColor(QPalette::All, QPalette::ToolTipBase, surface);
        palette.setColor(QPalette::All, QPalette::ToolTipText, text);
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, muted);
        theme.palette = palette;
        theme.fontSize = 14;
        theme.fontRegular.setPixelSize(14);
        theme.fontBold.setPixelSize(14);
        theme.fontCaption.setPixelSize(12);
        // 中英文统一使用微软雅黑 UI，避免语言切换时字体更新不完整而回退到宋体。
        for (QFont *font : {&theme.fontRegular, &theme.fontBold, &theme.fontH1, &theme.fontH2,
                            &theme.fontH3, &theme.fontH4, &theme.fontH5, &theme.fontCaption}) {
            font->setFamily(QStringLiteral("Microsoft YaHei UI"));
            font->setStyleStrategy(QFont::PreferAntialias);
        }
        style->setTheme(theme);
        qApp->setPalette(palette);
        QToolTip::setPalette(palette);
        if (trayMenu_) stylePopupMenu(trayMenu_, palette);
    }
#ifdef Q_OS_MACOS
    const ushort glyphs[]{0xE8A9, 0xE8D2, 0xE8B9, 0xE8B7, 0xE734};
    for (int i = 0; i < 5; ++i) {
        auto *button = filterButtons_->button(i);
        button->setIcon(headerGlyphIcon(QChar(glyphs[i]), text, button));
        button->setIconSize(QSize(18, 18));
        button->setText(QString{});
    }
#endif
    settingsButton_->setIcon(headerGlyphIcon(QChar(0xE713), text, settingsButton_));
    pinButton_->setIcon(headerGlyphIcon(isPinned_ ? QChar(0xE77A) : QChar(0xE718), text, pinButton_));
    clearButton_->setIcon(headerGlyphIcon(QChar(0xE75C), text, clearButton_));
    sourceButton_->setIcon(headerGlyphIcon(QChar(0xE71C), text, sourceButton_));
    dateLabel_->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(muted.name()));
    pageColor_ = page;
    borderColor_ = stroke;
    // 历史页样式不能成为设置页祖先样式，否则会覆盖 Qlementine Switch 的原生绘制。
    historyPage_->setStyleSheet(QStringLiteral(
        "QWidget#HistoryPage{background:transparent;color:%1;border:0;}"
        "QWidget#Header{background:transparent;border:0;} QLabel#AppTitle{color:%1;border:0;}"
        "QToolButton#HeaderButton{color:%1;background:%2;border:0;border-radius:6px;min-width:32px;max-width:32px;min-height:32px;max-height:32px;}"
        "QToolButton#HeaderButton:hover{background:%3;} QToolButton#FilterButton{color:%4;background:%2;border:0;border-radius:6px;min-width:36px;max-width:36px;min-height:32px;max-height:32px;margin-right:6px;}"
        "QToolButton#FilterButton:hover{background:%3;} QToolButton#FilterButton:checked{color:white;background:#0078D4;}"
        "QListView#HistoryList{background:transparent;color:%1;border:0;outline:0;}"
        "QLabel#EmptyHistory{color:%4;background:transparent;border:0;}"
    ).arg(text.name(), surface.name(), hover.name(), muted.name()));
    historyList_->viewport()->update();
    update();
}

void MainWindow::applySettings(const AppSettings &settings) {
    const bool languageChanged = settings_.language != settings.language;
    const bool themeChanged = settings_.themeMode != settings.themeMode;
    const bool hotkeyChanged = settings_.globalHotKey != settings.globalHotKey
        || settings_.sequenceCopyShortcut != settings.sequenceCopyShortcut
        || settings_.sequencePasteShortcut != settings.sequencePasteShortcut;
    const bool winVChanged = settings_.interceptWindowsClipboardShortcut != settings.interceptWindowsClipboardShortcut;
    const bool cleanupChanged = settings_.retentionDays != settings.retentionDays
        || settings_.maxHistoryEntries != settings.maxHistoryEntries;
    settings_ = settings;
    moreShortcut_->setKey(QKeySequence::fromString(QString(settings_.moreShortcut).remove(QLatin1Char(' ')), QKeySequence::PortableText));
    if (themeChanged) applyTheme();
    if (languageChanged) {
        // 字体不再随语言切换；仅关闭旧语言浮层并更新文案，不重复应用全局主题。
        if (!themeChanged) {
            closeHoverPreview();
            QToolTip::hideText();
        }
        searchBox_->setPlaceholderText(AppLocalization::get(QStringLiteral("MainSearchBoxPlaceholder")));
        emptyLabel_->setText(AppLocalization::get(QStringLiteral("MainEmptyHistoryText")));
        clearButton_->setToolTip(AppLocalization::get(QStringLiteral("ClearHistoryButtonTooltip")));
        const QStringList tooltips{QStringLiteral("FilterAllButtonTooltip"), QStringLiteral("FilterTextButtonTooltip"),
                                   QStringLiteral("FilterImageButtonTooltip"), QStringLiteral("FilterFilesButtonTooltip"),
                                   QStringLiteral("FilterFavoritesButtonTooltip")};
        for (int i = 0; i < tooltips.size(); ++i) {
            if (auto *button = filterButtons_->button(i)) button->setToolTip(AppLocalization::get(tooltips.at(i)));
        }
        buildTray();
    }
    if (hotkeyChanged) registerHotkey();
    if (winVChanged) updateWinVHook();
    updateTrayTooltip();
    if (cleanupChanged) {
        const QString path = databasePath_;
        const int retention = settings_.retentionDays;
        const int maximum = settings_.maxHistoryEntries;
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            const QString error = watcher->result();
            watcher->deleteLater();
            if (!error.isEmpty()) showStatus(error);
            else refreshHistory();
        });
        watcher->setFuture(QtConcurrent::run(&writePool_, [path, retention, maximum] {
            return HistoryStore(path).cleanup(retention, maximum);
        }));
    }
}

void MainWindow::registerHotkey() {
    const auto bind = [this](int id, const QString &key, std::function<void()> action) {
        if (key.isEmpty()) { GlobalHotkey::remove(winId(), id); return; }
        if (!GlobalHotkey::replace(winId(), id, key, std::move(action)))
            showStatus(AppLocalization::format(QStringLiteral("GlobalHotKeyRegistrationFailed"), {AppSettings::shortcutDisplayText(key)}));
    };
    bind(HotkeyId + 1, settings_.sequenceCopyShortcut, [this] { sequenceCopy(); });
    bind(HotkeyId + 2, settings_.sequencePasteShortcut, [this] { sequencePaste(); });
    if (!GlobalHotkey::replace(winId(), HotkeyId, settings_.globalHotKey, [this] { showPanel(); })) {
        showStatus(AppLocalization::format(QStringLiteral("GlobalHotKeyRegistrationFailed"),
                                           {AppSettings::shortcutDisplayText(settings_.globalHotKey)}));
    }
}

// 每次快捷键只发起一次复制；等待修饰键释放及新剪贴板内容，不拿旧内容充数。
void MainWindow::sequenceCopy() {
    if (sequenceCopyPending_ || sequencePasting_ || captureSaving_ || paused_ || suppressClipboardCapture_) return;
    if (sequenceQueue_.size() >= 256) { showStatus(AppLocalization::get(QStringLiteral("SequenceLimit"))); return; }
    const auto target = PasteTarget::capture((quintptr)foregroundTarget());
    if (!target || !PasteTarget::prepare(target->window, true)) return;
    const int generation = ++sequenceGeneration_;
    sequenceCopyPending_ = true;
    sequenceCopySent_ = false;
    sequenceCopyBaseline_ = clipboardSequence();
    auto *timer = new QTimer(this);
    timer->setInterval(25);
    const auto attempts = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this, [this, timer, attempts, target, generation] {
        if (generation != sequenceGeneration_ || !sequenceCopyPending_) { timer->stop(); timer->deleteLater(); return; }
        if (++*attempts > 80 || paused_ || suppressClipboardCapture_ || foregroundTarget() != (PanelTarget)target->window) {
            sequenceCopyPending_ = false;
            timer->stop(); timer->deleteLater();
            showStatus(AppLocalization::get(QStringLiteral("SequenceCopyFailed")));
            return;
        }
        if (!sequenceCopySent_) {
            const auto result = PasteTarget::send(target->window, target->focus, true, false, true);
            if (result == PasteTarget::Result::WaitingForModifiers) return;
            if (result == PasteTarget::Result::Failed) { *attempts = 80; return; }
            sequenceCopySent_ = true;
        }
        if (clipboardSequence() != sequenceCopyBaseline_ && !captureTimer_.isActive()) scheduleCapture();
    });
    timer->start();
}

void MainWindow::sequencePaste() {
    if (sequenceCopyPending_ || captureSaving_ || sequencePasting_ || suppressClipboardCapture_) return;
    if (sequenceQueue_.isEmpty()) { showStatus(AppLocalization::get(QStringLiteral("SequenceEmpty"))); return; }
    const auto target = foregroundTarget();
    if (!PasteTarget::capture((quintptr)target)) return;
    capturePasteTarget(target);
    sequencePasting_ = true;
    pasteRecord(sequenceQueue_.first().id, false, false, std::nullopt, true);
}

void MainWindow::updateWinVHook() {
#ifdef Q_OS_WIN
    if (settings_.interceptWindowsClipboardShortcut) {
        if (winVHook_) return;
        winVHookTarget = this;
        winVHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, interceptWinV, GetModuleHandleW(nullptr), 0);
        if (!winVHook_) showStatus(AppLocalization::get(QStringLiteral("WinVHookUnavailable")));
    } else if (winVHook_) {
        UnhookWindowsHookEx(winVHook_);
        winVHook_ = nullptr;
        if (winVHookTarget == this) winVHookTarget = nullptr;
    }
#endif
}

void MainWindow::updateTrayTooltip() {
    if (!trayIcon_) return;
    trayIcon_->setToolTip(AppLocalization::get(QStringLiteral("TrayToolTip")));
}

void MainWindow::setTrayPaused(bool paused) {
    paused_ = paused;
    if (pauseAction_ && pauseAction_->isChecked() != paused_) {
        const QSignalBlocker blocker(pauseAction_);
        pauseAction_->setChecked(paused_);
    }
    if (paused_) {
        pauseTimer_.start(TrayResumeMilliseconds);
    } else {
        pauseTimer_.stop();
        lastClipboardSequence_ = clipboardSequence();
    }
    updateTrayTooltip();
}

void MainWindow::hidePanel() {
    if (settingsPage_) {
        settingsPage_->reject();
        if (settingsPage_) return; // 备份进行中，设置页会拒绝关闭。
    }
    animatePage(nullptr);
    ++panelShowGeneration_;
    pasteGeneration_->fetchAndAddRelaxed(1);
    while (auto *popup = QApplication::activePopupWidget()) {
        if (!isPanelWindow((PanelTarget)popup->internalWinId())) break;
        popup->close();
    }
    setPassiveBrowsing(false);
    if (!isVisible()) return;
    hide();
    // 保留分页、筛选和选择，仅释放可重新加载的图片资源。
    closeHoverPreview();
    model_->releaseThumbnails();
    targetWindow_ = {};
    targetFocusWindow_ = {};
}

void MainWindow::showSettings(SettingsDialog *page) {
    if (settingsPage_) return;
    animatePage(nullptr);
    ++panelShowGeneration_;
    pasteGeneration_->fetchAndAddRelaxed(1);
    closeHoverPreview();
    if (!isVisible()) positionPanel(true);
    historyFocus_ = historyPage_->isAncestorOf(QApplication::focusWidget()) ? QApplication::focusWidget() : nullptr;
    settingsPage_ = page;
    // 抽屉独立于历史布局，移动时既不改变宿主尺寸，也不反复排版下面的历史卡片。
    drawerHost_ = new QWidget(this);
    drawerHost_->setGeometry(rect().adjusted(1, 1, -1, -1));
    QPainterPath clip;
    clip.addRoundedRect(QRectF(drawerHost_->rect()), 9, 9);
    drawerHost_->setMask(QRegion(clip.toFillPolygon().toPolygon()));
    page->setParent(drawerHost_, Qt::Widget);
    page->setFixedSize(drawerHost_->size());
    page->move(-drawerHost_->width(), 0);
    page->show();
    drawerHost_->show();
    drawerHost_->raise();
    if (auto *header = page->findChild<QWidget *>(QStringLiteral("Header"))) header->installEventFilter(this);
    show();
    raise();
    activatePanel();
    page->setFocus(Qt::OtherFocusReason);
    animatePage(page);
}

void MainWindow::leaveSettings() {
    if (!settingsPage_) return;
    ++panelShowGeneration_;
    // 设置页返回后即销毁；只保留一张短暂快照滑出，不延迟备份、提权等原有收尾流程。
    QPoint position = settingsPage_->pos();
    if (pageAnimation_) {
        if (auto *moving = qobject_cast<QWidget *>(pageAnimation_->targetObject())) position = moving->pos();
    }
    auto *outgoing = createSettingsSnapshot(settingsPage_, drawerHost_, position);
    settingsPage_ = nullptr;
    if (historyFocus_) historyFocus_->setFocus(Qt::OtherFocusReason);
    else historyStack_->currentWidget()->setFocus(Qt::OtherFocusReason);
    historyFocus_.clear();
    updateVisibleThumbnails();
    animatePage(outgoing, false);
    scheduleAutoHide();
}

void MainWindow::animatePage(QWidget *page, bool opening) {
    // 快速往返先停止旧动画；关闭宿主时立即释放抽屉及返回快照。
    if (pageAnimation_) {
        auto *previous = qobject_cast<QWidget *>(pageAnimation_->targetObject());
        pageAnimation_->stop();
        pageAnimation_->deleteLater();
        pageAnimation_.clear();
        if (previous && previous != page && previous != settingsPage_) {
            previous->hide();
            previous->deleteLater();
        }
    }
    if (!page) {
        delete drawerHost_;
        drawerHost_ = nullptr;
        return;
    }
#ifdef Q_OS_WIN
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
#else
    const bool enabled = MacIntegration::animationsEnabled();
#endif
    const QPoint end = opening ? QPoint(0, 0) : QPoint(-drawerHost_->width(), 0);
    if (!enabled || !isVisible() || page->pos() == end) {
        if (opening) page->move(0, 0);
        else {
            // 保持当前 finished 回调中的对话框有效，离开调用栈后再释放宿主。
            drawerHost_->hide();
            drawerHost_->deleteLater();
            drawerHost_ = nullptr;
        }
        return;
    }
    // 打开前完成排版绘制，逐帧只平移快照，避免首帧准备和整页控件重绘造成跳帧。
    QWidget *animated = opening ? createSettingsSnapshot(page, drawerHost_, page->pos()) : page;
    auto *animation = new QPropertyAnimation(animated, "pos", this);
    pageAnimation_ = animation;
    animation->setDuration(opening ? 280 : 240);
    animation->setStartValue(animated->pos());
    animation->setEndValue(end);
    animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(animation, &QPropertyAnimation::valueChanged, this, [this] { update(); });
    connect(animation, &QPropertyAnimation::finished, this, [this, opening, animation, page, animated] {
        if (pageAnimation_ != animation) return;
        pageAnimation_.clear();
        animation->deleteLater();
        if (opening) {
            if (settingsPage_ == page) page->move(0, 0);
            animated->hide();
            animated->deleteLater();
        } else {
            delete drawerHost_;
            drawerHost_ = nullptr;
        }
    });
    animation->start();
}

void MainWindow::refreshHistory() {
    searchTimer_.stop();
    const int generation = historyGeneration_->fetchAndAddRelaxed(1) + 1;
    historyDirty_ = false;
    closeHoverPreview();
    model_->setQuickPasteEnabled(false);
    loadingPage_ = true;
    updateDateIndicator();
    loadingMore_ = false;
    nextCursor_.reset();
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    const auto source = selectedSource_;
    auto *watcher = new QFutureWatcher<HistoryPage>(this);
    connect(watcher, &QFutureWatcher<HistoryPage>::finished, this, [this, watcher, generation] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (generation != historyGeneration_->loadRelaxed()) return;
        loadingPage_ = false;
        if (!page.error.isEmpty()) { historyDirty_ = true; showStatus(page.error); }
        // 在模型重置前保存焦点归属，结果返回后继续由列表或空状态接收按键。
        const bool restoreFocus = historyList_->hasFocus() || emptyLabel_->hasFocus();
        model_->setFirstPage(page);
        model_->setQuickPasteEnabled(searchBox_->text().isEmpty());
        nextCursor_ = page.next;
        unpinnedCount_ = page.unpinnedCount;
        // 查询失败持续显示具体错误，避免被当成正常的空筛选结果。
        emptyLabel_->setText(page.error.isEmpty()
            ? AppLocalization::get(QStringLiteral("MainEmptyHistoryText")) : page.error);
        updateEmptyState();
        if (restoreFocus && !passiveBrowsing_) historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
        updateVisibleThumbnails();
    });
    const QString path = databasePath_;
    const auto generationState = historyGeneration_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, query, kind, source, generationState, generation] {
        if (generationState->loadRelaxed() != generation) return HistoryPage{};
        return HistoryStore(path).search(query, kind, std::nullopt, HistoryPageSize, source);
    }));
}

void MainWindow::loadMoreHistory() {
    if (!isVisible() || settingsPage_ || loadingPage_ || loadingMore_ || !nextCursor_) return;
    loadingMore_ = true;
    const int generation = historyGeneration_->loadRelaxed();
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    const auto source = selectedSource_;
    const auto cursor = nextCursor_;
    auto *watcher = new QFutureWatcher<HistoryPage>(this);
    connect(watcher, &QFutureWatcher<HistoryPage>::finished, this, [this, watcher, generation] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (generation != historyGeneration_->loadRelaxed()) return;
        loadingMore_ = false;
        if (!page.error.isEmpty()) { showStatus(page.error); nextCursor_.reset(); return; }
        model_->appendPage(page);
        nextCursor_ = page.next;
        updateVisibleThumbnails();
    });
    const QString path = databasePath_;
    const auto generationState = historyGeneration_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, query, kind, source, cursor, generationState, generation] {
        if (generationState->loadRelaxed() != generation) return HistoryPage{};
        return HistoryStore(path).search(query, kind, cursor, HistoryPageSize, source);
    }));
}

void MainWindow::updateDateIndicator() {
    dateIndicatorDay_ = QDate::currentDate();
    if (!dateLabel_) return;
    if (loadingPage_ || !model_ || model_->rowCount() == 0) {
        dateLabel_->clear();
        dateLabel_->setToolTip({});
        return;
    }
    // 使用视口最上方可见卡片；卡片间隙处再向下探测，不跟随键盘选中项跳动。
    QModelIndex top;
    for (int y = 0; y < historyList_->viewport()->height() && !top.isValid(); ++y)
        top = historyList_->indexAt(QPoint(2, y));
    if (!top.isValid()) top = model_->index(0);
    const QDate date = QDateTime::fromMSecsSinceEpoch(top.data(HistoryModel::UpdatedRole).toLongLong()).date();
    const QString label = date == dateIndicatorDay_ ? AppLocalization::get(QStringLiteral("HistoryToday"))
        : date == dateIndicatorDay_.addDays(-1) ? AppLocalization::get(QStringLiteral("HistoryYesterday"))
        : date.toString(QStringLiteral("yyyy-MM-dd"));
    if (dateLabel_->text() != label) dateLabel_->setText(label);
    dateLabel_->setToolTip(date.toString(QStringLiteral("yyyy-MM-dd")));
}

void MainWindow::updateVisibleThumbnails() {
    updateDateIndicator();
    if (!isVisible() || settingsPage_ || loadingPage_ || !historyList_ || !model_) return;
    if (hoverPreview_) {
        const int row = model_->rowForId(hoverPreviewId_);
        if (row < 0) closeHoverPreview();
    }
    int first = model_->rowCount();
    int last = -1;
    const int step = qMax(24, delegate_->sizeHint({}, model_->index(0, 0)).height() / 2);
    for (int y = 0; y < historyList_->viewport()->height(); y += step) {
        const auto index = historyList_->indexAt(QPoint(2, y));
        if (!index.isValid()) continue;
        first = qMin(first, index.row());
        last = qMax(last, index.row());
    }
    model_->setVisibleRows(first, last);
}

void MainWindow::setSourceFilter(std::optional<QString> source) {
    selectedSource_ = std::move(source);
    sourceButton_->setChecked(selectedSource_.has_value());
    sourceFilterChip_->setVisible(selectedSource_.has_value());
    if (selectedSource_) {
        const QString name = selectedSource_->isEmpty()
            ? AppLocalization::get(QStringLiteral("UnknownApplication")) : *selectedSource_;
        const QString label = AppLocalization::format(QStringLiteral("ActiveSourceFilter"), {name});
        sourceFilterChip_->setText(AppLocalization::format(QStringLiteral("ActiveSourceFilter"),
            {sourceFilterChip_->fontMetrics().elidedText(name, Qt::ElideMiddle, 200)}));
        sourceFilterChip_->setToolTip(label);
        sourceFilterChip_->setAccessibleName(label);
    }
    searchTimer_.stop();
    historyList_->verticalScrollBar()->setValue(0);
    refreshHistory();
    if (!passiveBrowsing_) historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::openSourceMenu() {
    sourceButton_->setChecked(selectedSource_.has_value());
    if (QApplication::activePopupWidget()) return;
    auto *menu = new QMenu(this);
    stylePopupMenu(menu, qApp->palette());
    preparePanelMenu(menu);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto *all = menu->addAction(AppLocalization::get(QStringLiteral("AllSources")));
    all->setCheckable(true);
    all->setChecked(!selectedSource_);
    connect(all, &QAction::triggered, this, [this] { setSourceFilter(std::nullopt); });
    menu->addSeparator();
    auto *loading = menu->addAction(AppLocalization::get(QStringLiteral("LoadingSources")));
    loading->setEnabled(false);
    const auto cancelled = QSharedPointer<QAtomicInteger<bool>>::create(false);
    connect(menu, &QMenu::aboutToHide, this, [cancelled] { cancelled->storeRelaxed(true); });
    connect(menu, &QObject::destroyed, this, [cancelled] { cancelled->storeRelaxed(true); });
    auto *watcher = new QFutureWatcher<QPair<QStringList, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QStringList, QString>>::finished, this,
            [this, watcher, guard = QPointer<QMenu>(menu), loading] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!guard || !guard->isVisible()) return;
        if (!result.second.isEmpty()) { loading->setText(result.second); return; }
        if (result.first.isEmpty()) { loading->setText(AppLocalization::get(QStringLiteral("NoSources"))); return; }
        guard->removeAction(loading);
        delete loading;
        for (const auto &source : result.first) {
            const QString label = source.isEmpty() ? AppLocalization::get(QStringLiteral("UnknownApplication")) : source;
            auto *action = guard->addAction(guard->fontMetrics().elidedText(label, Qt::ElideMiddle, 240)
                                               .replace(QLatin1Char('&'), QStringLiteral("&&")));
            action->setToolTip(label);
            action->setCheckable(true);
            action->setChecked(selectedSource_ && selectedSource_->compare(source, Qt::CaseInsensitive) == 0);
            connect(action, &QAction::triggered, this, [this, source] { setSourceFilter(source); });
        }
        guard->setToolTipsVisible(true);
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, cancelled] {
        if (cancelled->loadRelaxed()) return QPair<QStringList, QString>{};
        return HistoryStore(path).sourceApplications();
    }));
    menu->popup(sourceButton_->mapToGlobal(QPoint(0, sourceButton_->height())));
}

void MainWindow::setFilter(int kind) {
    selectedKind_ = kind;
    // 收藏受保留规则保护，不提供整批清空；单条删除仍需确认。
    clearButton_->setEnabled(kind != 3);
    refreshHistory();
    if (!passiveBrowsing_) historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::switchFilter(int direction) {
    const int count = filterButtons_->buttons().size();
    const int next = (selectedKind_ + 1 + direction + count) % count;
    filterButtons_->button(next)->setChecked(true);
    setFilter(next - 1);
}

void MainWindow::updateEmptyState() {
    const bool empty = model_->rowCount() == 0 && !loadingPage_;
    historyStack_->setCurrentWidget(empty ? static_cast<QWidget *>(emptyLabel_) : static_cast<QWidget *>(historyList_));
    // 模型刷新会清除当前项，恢复首条记录以便立即使用卡片快捷键。
    if (!empty && !historyList_->currentIndex().isValid())
        historyList_->setCurrentIndex(model_->index(0));
}

void MainWindow::showStatus(const QString &message) {
    if (message.isEmpty()) return;
    // Qt 的纯文本提示不会主动限制长路径宽度，按屏幕宽度布局后保留显式换行。
    const int maxWidth = qMax(1, qMin(360, screen()->availableGeometry().width() - 24));
    QStringList lines;
    for (const auto &paragraph : message.split(QLatin1Char('\n'))) {
        if (paragraph.isEmpty()) { lines.append(QString{}); continue; }
        QTextLayout layout(paragraph, QToolTip::font());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(maxWidth);
            lines.append(paragraph.mid(line.textStart(), line.textLength()));
        }
        layout.endLayout();
    }
    // 转义外部错误详情，避免路径或错误文本被当成 HTML 解析。
    const QString text = lines.join(QLatin1Char('\n')).toHtmlEscaped()
        .replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    QToolTip::showText(QCursor::pos(), QStringLiteral("<qt>%1</qt>").arg(text), this, rect(), 2800);
}

bool MainWindow::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    const auto *native = static_cast<MSG *>(message);
    // 消息过滤期间不能用 winId() 创建窗口，否则窗口创建消息会递归进入过滤器。
    const HWND panel = reinterpret_cast<HWND>(internalWinId());
    if (passiveBrowsing_ && native->message == WM_MOUSEACTIVATE && isPanelWindow(native->hwnd)) {
        auto *clicked = QApplication::widgetAt(QCursor::pos());
        if (clicked && (clicked == searchBox_ || searchBox_->isAncestorOf(clicked))) {
            activatePanel(searchBox_);
            if (result) *result = MA_ACTIVATE;
        } else {
            if (result) *result = MA_NOACTIVATE;
        }
        return true;
    }
    if (panel && native->hwnd == panel && native->message == WM_SETTINGCHANGE
        && settings_.themeMode == QStringLiteral("System"))
        QTimer::singleShot(0, this, &MainWindow::applyTheme);
    // 鼠标激活置顶面板之前保存最新目标，此时外部输入控件尚未失去焦点。
    if (isPinned_ && !settingsPage_ && panel && native->hwnd == panel
        && native->message == WM_MOUSEACTIVATE) capturePasteTarget(GetForegroundWindow());
    if (isPinned_ && !settingsPage_ && panel && native->hwnd == panel
        && native->message == WM_ACTIVATE && LOWORD(native->wParam) != WA_INACTIVE) {
        const HWND previous = reinterpret_cast<HWND>(native->lParam);
        if (previous != targetWindow_) capturePasteTarget(previous);
    }
    if (native->message == WM_HOTKEY && (native->wParam == HotkeyId + 1 || native->wParam == HotkeyId + 2)) {
        if (native->wParam == HotkeyId + 1) sequenceCopy(); else sequencePaste();
        if (result) *result = 0;
        return true;
    }
    if (native->message == WM_HOTKEY && native->wParam == HotkeyId) {
        const HWND foreground = GetForegroundWindow();
        showPanel(false, foreground);
        if (result) *result = 0;
        return true;
    }
    if (native->message == WM_CLIPBOARDUPDATE) {
        scheduleCapture();
        if (result) *result = 0;
        return true;
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == searchBox_ && passiveBrowsing_ && event->type() == QEvent::MouseButtonPress)
        activatePanel(searchBox_);
    if (watched == hoverPreview_ && event->type() == QEvent::WindowDeactivate) scheduleAutoHide();
    if (historyList_ && watched == historyList_->viewport()) {
        // 新分类加载完成前，拦截旧卡片的点击和预览提示。
        if (loadingPage_ && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::ToolTip)) return true;
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::Leave) {
            const QRect row = event->type() != QEvent::Leave
                ? historyList_->visualRect(historyList_->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint()))
                : QRect{};
            if (hoveredHistoryRow_ != row) historyList_->viewport()->update(hoveredHistoryRow_);
            hoveredHistoryRow_ = row;
            if (row.isValid()) historyList_->viewport()->update(row);
        }
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const QModelIndex hovered = historyList_->indexAt(help->pos());
            if (hovered.isValid()) {
                const int action = delegate_->actionAt(historyList_->visualRect(hovered), help->pos());
                QString label;
                if (action == HistoryDelegate::Preview)
                    label = AppLocalization::get(hoverPreview_
                        && hovered.data(HistoryModel::IdRole).toString() == hoverPreviewId_
                            ? QStringLiteral("CollapsePreview") : QStringLiteral("PreviewContent"));
                else if (action == HistoryDelegate::Pin)
                    label = AppLocalization::get(hovered.data(HistoryModel::PinnedRole).toBool()
                        ? QStringLiteral("UnpinItem") : QStringLiteral("PinItem"));
                else if (action == HistoryDelegate::More)
                    label = AppLocalization::format(QStringLiteral("RecordMoreShortcutTooltip"), {AppSettings::shortcutDisplayText(settings_.moreShortcut)});
                if (!label.isEmpty()) { QToolTip::showText(help->globalPos(), label, historyList_->viewport()); return true; }
            }
            QToolTip::hideText();
            return true;
        }
    }
    if (hoverImageScroll_ && (watched == hoverImageLabel_ || watched == hoverImageScroll_->viewport())) {
        // 预览切换及 DPI 变化会重新布局，使用最终视口尺寸更新图片缩放。
        if (watched == hoverImageScroll_->viewport() && event->type() == QEvent::Resize)
            updateHoverPreviewImage();
        if (event->type() == QEvent::Wheel && !hoverImageSize_.isEmpty()) {
            const auto *wheel = static_cast<QWheelEvent *>(event);
            const double factor = wheel->angleDelta().y() > 0 ? 1.25 : 0.8;
            const double zoom = qBound(1.0, hoverImageZoom_ * factor, 4.0);
            if (!qFuzzyCompare(zoom, hoverImageZoom_)) {
                hoverImageZoom_ = zoom;
                updateHoverPreviewImage();
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::MiddleButton) {
                hoverImageZoom_ = 1.0;
                updateHoverPreviewImage();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                hoverImageDragging_ = true;
                hoverDragStart_ = mouse->globalPosition().toPoint();
                hoverScrollStart_ = {hoverImageScroll_->horizontalScrollBar()->value(),
                                     hoverImageScroll_->verticalScrollBar()->value()};
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && hoverImageDragging_) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint delta = mouse->globalPosition().toPoint() - hoverDragStart_;
            hoverImageScroll_->horizontalScrollBar()->setValue(hoverScrollStart_.x() - delta.x());
            hoverImageScroll_->verticalScrollBar()->setValue(hoverScrollStart_.y() - delta.y());
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && hoverImageDragging_) {
            hoverImageDragging_ = false;
            return true;
        }
    }
    if (watched->objectName() == QStringLiteral("Header")) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                dragging_ = true;
                dragStart_ = mouse->globalPosition().toPoint();
                dragWindowStart_ = pos();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && dragging_) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            move(dragWindowStart_ + mouse->globalPosition().toPoint() - dragStart_);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease && dragging_) {
            dragging_ = false;
            return true;
        }
    }
    if ((watched == historyList_ || watched == searchBox_ || watched == emptyLabel_)
        && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            // 按交互层级退出，预览是否持有焦点不影响 Esc 的行为。
            if (hoverPreview_) closeHoverPreview(true);
            else if (!searchBox_->text().isEmpty()) searchBox_->clear();
            else hidePanel();
            return true;
        }
        if (key->key() == Qt::Key_F && key->modifiers() == Qt::ControlModifier) {
            activatePanel(searchBox_);
            return true;
        }
        if (watched != searchBox_ && key->modifiers() == Qt::NoModifier
            && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            switchFilter(key->key() == Qt::Key_Right ? 1 : -1);
            return true;
        }
        // 加载期间保留关闭和分类切换，避免导航或快捷键操作旧列表。
        if (loadingPage_ && watched != searchBox_) return true;
        if (watched == searchBox_ && !loadingPage_
            && key->key() == Qt::Key_Down && model_->rowCount() > 0) {
            historyList_->setCurrentIndex(model_->index(0));
            historyList_->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
        if ((watched == historyList_ || watched == emptyLabel_)
            && key->key() == Qt::Key_Up && historyList_->currentIndex().row() <= 0) {
            activatePanel(searchBox_);
            return true;
        }
        if (watched == historyList_ && key->modifiers() == Qt::NoModifier
            && searchBox_->text().isEmpty() && key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9) {
            const int row = model_->quickPasteRow(key->key() - Qt::Key_1 + 1);
            if (const auto *item = model_->itemAt(row)) { pasteRecord(item->id); return true; }
        }
        if (watched == historyList_ && historyList_->currentIndex().isValid()) {
            const auto id = historyList_->currentIndex().data(HistoryModel::IdRole).toString();
            QString pressed = QKeySequence(key->modifiers() | key->key()).toString(QKeySequence::PortableText);
            pressed.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
            const auto normalized = AppSettings::normalizedShortcut(pressed);
            if (key->key() == Qt::Key_Menu || normalized == AppSettings::normalizedShortcut(settings_.moreShortcut)) {
                openRowMenu(id);
                return true;
            }
            if (normalized == AppSettings::normalizedShortcut(settings_.plainTextPasteShortcut)) {
                pasteRecord(id, true); return true;
            }
            if (normalized == AppSettings::normalizedShortcut(settings_.pasteShortcut)) {
                pasteRecord(id); return true;
            }
            if (!settings_.openShortcut.isEmpty() && normalized == AppSettings::normalizedShortcut(settings_.openShortcut)) {
                if (!key->isAutoRepeat()) openRecord(id);
                return true;
            }
            if (normalized == AppSettings::normalizedShortcut(settings_.previewShortcut)) {
                // 再次按预览快捷键关闭当前预览，忽略长按产生的重复事件。
                if (key->isAutoRepeat()) return true;
                toggleHoverPreview(id);
                return true;
            }
            if (normalized == AppSettings::normalizedShortcut(settings_.pinShortcut)) {
                setPinned(id); return true;
            }
            if (normalized == AppSettings::normalizedShortcut(settings_.pasteAsFileShortcut)) {
                pasteRecord(id, false, true); return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool MainWindow::event(QEvent *event) {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
        clipboardPollTimer_.setInterval(isVisible() || !clipboardNotificationsAvailable_ ? 250 : 2000);
    // 托盘唤出的顶层窗口失去激活时不会触发 QWidget::focusOutEvent。
    if (event->type() == QEvent::WindowDeactivate && !passiveBrowsing_) {
        if (isPinned_ && !settingsPage_) {
#ifdef Q_OS_WIN
            const HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground) GetWindowThreadProcessId(foreground, &processId);
            if (processId && processId != GetCurrentProcessId()) {
                targetFocusWindow_ = {};
                targetWindow_ = foreground;
            }
#else
            const auto foreground = foregroundTarget();
            if (MacIntegration::isExternalApplication(foreground)) {
                targetWindow_ = foreground;
                targetFocusWindow_ = {};
            }
#endif
        }
        scheduleAutoHide();
    }
    return handled;
}

void MainWindow::paintEvent(QPaintEvent *) {
    // 透明原生窗口内只绘制圆角面板，四角保持透明而不是仅让样式表裁剪边框。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(borderColor_, 1));
    painter.setBrush(pageColor_);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
}

void MainWindow::scheduleAutoHide() {
    // 历史面板只由置顶状态决定是否失焦隐藏；设置、查看和弹出菜单期间仍保留交互。
    const int generation = panelShowGeneration_;
    QTimer::singleShot(120, this, [this, generation] {
        if (generation != panelShowGeneration_) return;
        if (isVisible() && !passiveBrowsing_ && !isActiveWindow() && !isPinned_ && !settingsPage_
            && (!hoverPreview_ || !hoverPreview_->isActiveWindow())
            && !qApp->activePopupWidget() && !qApp->activeModalWidget()) {
            hidePanel();
        }
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (closingForExit_) event->accept();
    else { event->ignore(); hidePanel(); }
}

void MainWindow::setPinned(const QString &id) {
    if (loadingPage_) return;
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
    const bool newState = !item->pinned;
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, id, newState] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) { showStatus(error); return; }
        // 切换分类期间可能读到了写入前的数据，重新查询当前分类即可。
        if (loadingPage_) { refreshHistory(); return; }
        const int row = model_->rowForId(id);
        const auto *current = model_->itemAt(row);
        if (!current) { historyDirty_ = true; return; }
        if (current->pinned == newState) return;
        // 收藏不再影响排序，只更新角色，保留选中项、滚动位置和已加载的分页。
        if (selectedKind_ != 3) unpinnedCount_ = qMax(0, unpinnedCount_ + (newState ? -1 : 1));
        model_->setPinned(id, newState);
        if (selectedKind_ == 3 && !newState) {
            const bool restoreFocus = historyList_->hasFocus();
            const bool removingCurrent = historyList_->currentIndex().data(HistoryModel::IdRole).toString() == id;
            model_->remove(id);
            if (model_->rowCount() == 0 && nextCursor_) { refreshHistory(); return; }
            if (removingCurrent && model_->rowCount() > 0)
                historyList_->setCurrentIndex(model_->index(qMin(row, model_->rowCount() - 1)));
            updateEmptyState();
            if (restoreFocus && !passiveBrowsing_) historyStack_->currentWidget()->setFocus(Qt::ShortcutFocusReason);
            updateVisibleThumbnails();
        }
    });
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, id, newState] {
        return HistoryStore(path).setPinned(id, newState);
    }));
}

void MainWindow::deleteRecord(const QString &id) {
    if (loadingPage_) return;
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
    if (item->pinned) activatePanel();
    if (item->pinned && QMessageBox::question(this,
            AppLocalization::get(QStringLiteral("DeletePinnedItemTitle")),
            AppLocalization::get(QStringLiteral("DeletePinnedItemMessage")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) showStatus(error);
        refreshHistory();
    });
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, id] { return HistoryStore(path).remove(id); }));
}

void MainWindow::clearCurrentList() {
    if (loadingPage_ || unpinnedCount_ <= 0) return;
    activatePanel();
    const QString message = AppLocalization::format(QStringLiteral("ClearCurrentListMessage"),
                                                     {QString::number(unpinnedCount_)});
    if (QMessageBox::question(this, AppLocalization::get(QStringLiteral("ClearCurrentListTitle")), message,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const QString path = databasePath_;
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    const auto source = selectedSource_;
    auto *watcher = new QFutureWatcher<QPair<int, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished, this, [this, watcher, path] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.second.isEmpty()) showStatus(result.second);
        // 删除提交后立即刷新，后台仅限量回收空闲页，避免全库重写阻塞后续保存。
        refreshHistory();
        if (result.first <= 0 || !result.second.isEmpty()) return;
        showStatus(AppLocalization::format(QStringLiteral("CurrentListCleared"),
                                           {QString::number(result.first)}));
        auto *compactWatcher = new QFutureWatcher<QString>(this);
        connect(compactWatcher, &QFutureWatcher<QString>::finished, this, [this, compactWatcher] {
            const QString error = compactWatcher->result();
            compactWatcher->deleteLater();
            if (!error.isEmpty()) showStatus(error);
        });
        compactWatcher->setFuture(QtConcurrent::run(&writePool_, [path] {
            return HistoryStore(path).compact();
        }));
    });
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, query, kind, source] {
        QString error;
        const int deleted = HistoryStore(path).removeMatching(query, kind, &error, source);
        return qMakePair(deleted, error);
    }));
}

void MainWindow::openTarget(const QString &target) {
    // 打开和粘贴互斥，避免较早的异步粘贴在浏览器或资源管理器出现后才执行。
    pasteGeneration_->fetchAndAddRelaxed(1);
    const QUrl url(target, QUrl::StrictMode);
    if (!isPinned_) hidePanel();
    if (!url.isLocalFile()) {
        if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            || !QDesktopServices::openUrl(url))
            showStatus(AppLocalization::get(QStringLiteral("OpenLinkFailed")));
        return;
    }
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        const bool opened = watcher->result();
        watcher->deleteLater();
        if (!opened) showStatus(AppLocalization::get(QStringLiteral("RevealFileFailed")));
    });
    const QString location = url.toLocalFile();
    // 网络路径、Shell 扩展只在用户确认打开后由后台访问。
    watcher->setFuture(QtConcurrent::run(&contentPool_, [location] { return revealFile(location); }));
}

void MainWindow::addOpenTargets(QMenu *menu, const QStringList &targets) {
    menu->setToolTipsVisible(true);
    for (const auto &target : targets) {
        const QUrl url(target, QUrl::StrictMode);
        const QString label = url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : url.toDisplayString();
        auto *action = menu->addAction(targets.size() == 1
            ? AppLocalization::get(QStringLiteral("OpenTargetMenuItemText"))
            : menu->fontMetrics().elidedText(label, Qt::ElideMiddle, 360).replace(QLatin1Char('&'), QStringLiteral("&&")));
        action->setToolTip(label);
        connect(action, &QAction::triggered, this, [this, target] { openTarget(target); });
    }
}

void MainWindow::openRecord(const QString &id) {
    if (loadingPage_ || settingsPage_ || QApplication::activePopupWidget()) return;
    pasteGeneration_->fetchAndAddRelaxed(1);
    const int generation = ++openGeneration_;
    const int panelGeneration = panelShowGeneration_;
    const int historyGeneration = historyGeneration_->loadRelaxed();
    const auto foreground = foregroundTarget();
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, id, generation, panelGeneration, historyGeneration, foreground] {
        const auto targets = watcher->result();
        watcher->deleteLater();
        // 读取期间切换记录、筛选、窗口或其他操作后，不再打开旧请求的目标。
        if (generation != openGeneration_ || panelGeneration != panelShowGeneration_
            || historyGeneration != historyGeneration_->loadRelaxed() || !isVisible() || settingsPage_
            || foregroundTarget() != foreground
            || historyList_->currentIndex().data(HistoryModel::IdRole).toString() != id) return;
        if (targets.isEmpty()) {
            showStatus(AppLocalization::get(QStringLiteral("NoOpenTarget")));
            return;
        }
        if (targets.size() == 1) { openTarget(targets.first()); return; }
        auto *menu = new QMenu(this);
        stylePopupMenu(menu, qApp->palette());
        addOpenTargets(menu, targets);
        connect(menu, &QMenu::aboutToHide, this, &MainWindow::scheduleAutoHide);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        preparePanelMenu(menu);
        const QRect card = historyList_->visualRect(historyList_->currentIndex());
        menu->popup(historyList_->viewport()->mapToGlobal(card.topRight()));
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&contentPool_, [path, id] {
        return HistoryStore(path).contentMetadata(id).openTargets;
    }));
}

void MainWindow::openRowMenu(const QString &id) {
    if (loadingPage_ || QApplication::activePopupWidget()) return;
    ++openGeneration_;
    const int row = model_->rowForId(id);
    const auto *item = model_->itemAt(row);
    if (!item) return;
    const int kind = item->kind;
    const int generation = panelShowGeneration_;
    const QRect button = delegate_->actionGeometry(historyList_->visualRect(model_->index(row)), HistoryDelegate::More);
    const QRect anchor(historyList_->viewport()->mapToGlobal(button.topLeft()), button.size());
    // 常用操作立即显示，不等待数据库读取和富文本链接解析。
    auto *menu = new QMenu();
    stylePopupMenu(menu, qApp->palette());
    menu->setProperty("rowMenuAnchor", anchor);
    connect(this, &QObject::destroyed, menu, &QObject::deleteLater);
    QAction *plain = nullptr;
    QAction *asFile = nullptr;
    if (kind == 0) plain = menu->addAction(AppLocalization::get(QStringLiteral("PastePlainTextMenuItem.Text")));
    if (kind == 0 || kind == 1) asFile = menu->addAction(AppLocalization::get(QStringLiteral("PasteAsFileMenuItem.Text")));
    if (plain || asFile) menu->addSeparator();
    auto *remove = menu->addAction(AppLocalization::get(QStringLiteral("DeleteRecordMenuItem.Text")));
    if (plain) connect(plain, &QAction::triggered, this, [this, id] { pasteRecord(id, true); });
    if (asFile) connect(asFile, &QAction::triggered, this, [this, id] { pasteRecord(id, false, true); });
    connect(remove, &QAction::triggered, this, [this, id] {
        // 删除留在历史面板，菜单关闭后重新激活以免失焦回调误隐藏。
        if (!passiveBrowsing_) activatePanel(historyList_);
        deleteRecord(id);
    });
    connect(menu, &QMenu::aboutToHide, this, &MainWindow::scheduleAutoHide);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    preparePanelMenu(menu);
    menu->popup(anchor.bottomLeft() + QPoint(0, 3));
    if (kind == 1) return;
    const QString path = databasePath_;
    const auto cancelled = QSharedPointer<QAtomicInteger<bool>>::create(false);
    connect(menu, &QMenu::aboutToHide, this, [cancelled] { cancelled->storeRelaxed(true); });
    connect(menu, &QObject::destroyed, this, [cancelled] { cancelled->storeRelaxed(true); });
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, menu = QPointer<QMenu>(menu), generation] {
        const auto targets = watcher->result();
        watcher->deleteLater();
        if (!menu || !menu->isVisible() || generation != panelShowGeneration_ || targets.isEmpty()) return;
        // 可选操作追加到底部，不移动用户正在选择的常用操作。
        menu->addSeparator();
        const QString title = AppLocalization::get(QStringLiteral("OpenTargetMenuItemText"));
        QMenu *choices = menu;
        if (targets.size() > 1) {
            choices = menu->addMenu(title);
            stylePopupMenu(choices, qApp->palette());
            preparePanelMenu(choices);
        }
        addOpenTargets(choices, targets);
    });
    // 新记录只读取缓存；旧记录首次打开菜单时补算，关闭菜单后跳过排队任务。
    watcher->setFuture(QtConcurrent::run(&contentPool_, [path, id, cancelled] {
        if (cancelled->loadRelaxed()) return QStringList{};
        return HistoryStore(path).contentMetadata(id).openTargets;
    }));
}

void MainWindow::scheduleCapture() {
    if (paused_ || suppressClipboardCapture_) return;
    const quint32 sequence = clipboardSequence();
    if (sequence && sequence == ownClipboardSequence_) {
        lastClipboardSequence_ = sequence;
        return;
    }
    if (sequence && sequence != lastClipboardSequence_) captureTimer_.start();
}

bool MainWindow::sourceIsExcluded(const QString &source) const {
    if (source.isEmpty() || settings_.excludedApplications.isEmpty()) return false;
    QString configured = settings_.excludedApplications;
    configured.replace(QLatin1Char(','), QLatin1Char(';'));
    configured.replace(QLatin1Char('\r'), QLatin1Char(';'));
    configured.replace(QLatin1Char('\n'), QLatin1Char(';'));
    const auto excluded = configured.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (auto name : excluded) {
        name = name.trimmed().toCaseFolded();
#ifdef Q_OS_WIN
        if (!name.isEmpty() && !name.endsWith(QStringLiteral(".exe"))) name.append(QStringLiteral(".exe"));
#else
        if (name.endsWith(QStringLiteral(".app"))) name.chop(4);
#endif
        if (source == name) return true;
    }
    return false;
}

void MainWindow::captureClipboard() {
    if (paused_ || suppressClipboardCapture_ || captureSaving_) return;
    const quint32 sequence = clipboardSequence();
    if (!sequence || sequence == lastClipboardSequence_ || sequence == ownClipboardSequence_) return;
    // 忽略规则与历史来源共用一次进程查询，避免重复打开源进程句柄。
#ifdef Q_OS_WIN
    const QString source = processName(GetClipboardOwner());
#else
    const QString source = MacIntegration::clipboardSource();
#endif
    if (sourceIsExcluded(source)) { lastClipboardSequence_ = sequence; return; }
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    ClipboardCapture capture;
    capture.sourceApplication = source;
    QImage fallback;
    const auto urls = mime->urls();

    if (mime->hasImage()) {
        if (!settings_.monitorImages) { lastClipboardSequence_ = sequence; return; }
        capture.kind = 1;
        capture.searchText = AppLocalization::get(QStringLiteral("ImageContent"));
        const QStringList formats{QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                  QStringLiteral("image/webp"), QStringLiteral("image/bmp"), QStringLiteral("image/gif")};
        for (const auto &format : formats) {
            if (mime->hasFormat(format)) {
                capture.content = mime->data(format);
                if (!capture.content.isEmpty()) break;
            }
        }
        if (capture.content.isEmpty()) fallback = qvariant_cast<QImage>(mime->imageData());
        if (capture.content.isEmpty() && fallback.isNull()) return;
        capture.text = mime->hasText() ? mime->text() : QString{};
        capture.html = mime->hasHtml() ? mime->html() : QString{};
        capture.rtf = clipboardRtf(mime);
        if (!capture.text.isEmpty()) capture.searchText = capture.text;
    } else if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); })) {
        if (!settings_.monitorFiles) { lastClipboardSequence_ = sequence; return; }
        QStringList paths;
        QJsonArray json;
        for (const auto &url : urls) {
            if (!url.isLocalFile()) continue;
            const QString path = QDir::toNativeSeparators(url.toLocalFile());
            paths.push_back(path);
            json.append(path);
        }
        if (paths.isEmpty()) { lastClipboardSequence_ = sequence; return; }
        capture.kind = 2;
        capture.searchText = paths.join(QLatin1Char('\n'));
        capture.content = QJsonDocument(json).toJson(QJsonDocument::Compact);
    } else if (mime->hasText()) {
        if (!settings_.monitorText) { lastClipboardSequence_ = sequence; return; }
        const QString text = mime->text();
        if (text.isEmpty()) { lastClipboardSequence_ = sequence; return; }
        capture.kind = 0;
        capture.searchText = text;
        QJsonObject payload;
        payload.insert(QStringLiteral("Text"), text);
        payload.insert(QStringLiteral("Html"), mime->hasHtml() ? QJsonValue(mime->html()) : QJsonValue(QJsonValue::Null));
        payload.insert(QStringLiteral("Markdown"), mime->hasFormat(QStringLiteral("text/markdown")));
        const QByteArray rtf = clipboardRtf(mime);
        payload.insert(QStringLiteral("RtfBase64"), QString::fromLatin1(rtf.toBase64()));
        capture.content = QByteArray(TextPayloadPrefix) + QJsonDocument(payload).toJson(QJsonDocument::Compact);
    } else {
        lastClipboardSequence_ = sequence;
        return;
    }
    saveCapture(std::move(capture), std::move(fallback), sequence);
}

void MainWindow::saveCapture(ClipboardCapture capture, QImage fallbackImage, quint32 sequence) {
    const bool queued = sequenceCopyPending_ && sequenceCopySent_ && sequence != sequenceCopyBaseline_;
    if (queued) sequenceCopyPending_ = false;
    const bool preserveQueue = queued || !sequenceQueue_.isEmpty();
    const auto saved = QSharedPointer<HistoryItem>::create();
    const int sequenceGeneration = sequenceGeneration_;
    captureSaving_ = true;
    const bool normalizeUnicode = settings_.normalizeUnicodeText;
    const int retention = settings_.retentionDays;
    const int maximum = settings_.maxHistoryEntries;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, sequence, queued, saved, sequenceGeneration] {
        const QString error = watcher->result();
        watcher->deleteLater();
        captureSaving_ = false;
        lastClipboardSequence_ = sequence;
        if (!error.isEmpty()) showStatus(error);
        else {
            if (queued && sequenceGeneration == sequenceGeneration_) {
                // 队列只保留标识和类型，不依赖面板当前加载的分页，也不复制正文。
                HistoryItem entry;
                entry.id = saved->id;
                entry.kind = saved->kind;
                sequenceQueue_.append(entry);
                showStatus(AppLocalization::format(QStringLiteral("SequenceRemaining"), {QString::number(sequenceQueue_.size())}));
            }
            // 隐藏期间只标记待刷新，下一次打开时再查询第一页。
            historyDirty_ = true;
            newHistoryAvailable_ = !isVisible();
            if (isVisible()) refreshHistory();
        }
        if (clipboardSequence() != lastClipboardSequence_) scheduleCapture();
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, retention, maximum, normalizeUnicode, saved, preserveQueue, capture = std::move(capture),
                                                        fallback = std::move(fallbackImage)]() mutable {
        if (capture.kind == 0) {
            auto payload = QJsonDocument::fromJson(capture.content.mid(qstrlen(TextPayloadPrefix))).object();
            // 图文和富文本必须保留原始格式；仅规范化纯文本和 Markdown，不丢弃嵌入图片。
            if (normalizeUnicode && payload.value(QStringLiteral("Html")).toString().isEmpty()
                && payload.value(QStringLiteral("RtfBase64")).toString().isEmpty()) {
                const QString normalized = capture.searchText.normalized(QString::NormalizationForm_C);
                if (normalized != capture.searchText) {
                    payload.insert(QStringLiteral("Text"), normalized);
                    capture.searchText = normalized;
                }
            }
            const auto images = captureHtmlImages(payload.value(QStringLiteral("Html")).toString());
            if (!images.isEmpty()) {
                QJsonObject encoded;
                for (auto it = images.begin(); it != images.end(); ++it)
                    encoded.insert(it.key(), QString::fromLatin1(it.value().toBase64()));
                payload.insert(QStringLiteral("HtmlImages"), encoded);
            }
            capture.content = QByteArray(TextPayloadPrefix) + QJsonDocument(payload).toJson(QJsonDocument::Compact);
        }
        if (capture.kind == 1) {
            prepareImageCapture(capture, fallback);
            encodeMixedImage(capture);
        }
        HistoryStore store(path);
        QString error = store.save(capture, saved.data());
        if (error.isEmpty() && !preserveQueue) error = store.cleanup(retention, maximum);
        return error;
    }));
}

void MainWindow::cleanupPasteFiles() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 day = 24LL * 60 * 60 * 1000;
    // 每日最多清理一次，避免连续粘贴文件时重复扫描目录。
    if (lastPasteFileCleanup_ && now - lastPasteFileCleanup_ < day) return;
    lastPasteFileCleanup_ = now;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/PasteOrbit/PasteFiles");
    QSet<QString> protectedPaths;
    if (const auto *mime = QApplication::clipboard()->mimeData()) {
        for (const auto &url : mime->urls()) {
            if (url.isLocalFile()) protectedPaths.insert(QDir::cleanPath(url.toLocalFile()).toCaseFolded());
        }
    }
    const QString prefix = QDir::cleanPath(QDir(directory).absolutePath()).toCaseFolded() + QLatin1Char('/');
    writePool_.start([directory, prefix, protectedPaths, cutoff = now - 7 * day] {
        const auto files = QDir(directory).entryInfoList({QStringLiteral("PasteOrbit_*")}, QDir::Files | QDir::NoSymLinks);
        for (const auto &file : files) {
            const QString path = QDir::cleanPath(file.absoluteFilePath()).toCaseFolded();
            // 只删除指定目录中的生成文件；保留当前剪贴板引用和最近七天的文件。
            if (path.startsWith(prefix) && !protectedPaths.contains(path)
                && file.lastModified().toMSecsSinceEpoch() < cutoff)
                QFile::remove(file.absoluteFilePath());
        }
    });
}

void MainWindow::pasteRecord(const QString &id, bool plainText, bool asFile, std::optional<QByteArray> editedContent, bool queued) {
    // 操作完成、失败或被取消时都释放顺序粘贴锁，避免异步返回后卡住队列。
    const auto completion = QSharedPointer<int>(new int(0), [guard = QPointer<MainWindow>(this), queued](int *value) {
        if (guard && queued) guard->sequencePasting_ = false;
        delete value;
    });
    if (loadingPage_ && !queued) return;
    ++openGeneration_;
    const int row = model_->rowForId(id);
    const auto *itemPointer = queued && !sequenceQueue_.isEmpty() ? &sequenceQueue_.first() : model_->itemAt(row);
    if (!itemPointer) return;
    const HistoryItem item = *itemPointer;
    if (editedContent && item.kind != 0) return;
    if (asFile && item.kind == 2) return;
    const int generation = pasteGeneration_->fetchAndAddRelaxed(1) + 1;
    auto target = targetWindow_;
    const auto targetFocus = targetFocusWindow_;
    if (!target) {
        const auto foreground = foregroundTarget();
#ifdef Q_OS_WIN
        if (foreground != reinterpret_cast<HWND>(winId())) target = foreground;
#else
        if (MacIntegration::isExternalApplication(foreground)) target = foreground;
#endif
    }
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<PasteResult>(this);
    connect(watcher, &QFutureWatcher<PasteResult>::finished, this,
            [this, watcher, target, targetFocus, plainText, asFile, id, generation, queued, completion] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != pasteGeneration_->loadRelaxed()) return;
        if (!result.unavailablePath.isEmpty()) {
            showStatus(AppLocalization::format(QStringLiteral("FilePathUnavailable"), {result.unavailablePath}));
            return;
        }
        const HistoryItem &item = result.item;
        const QByteArray &content = result.content;
        if (content.isEmpty()) { showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed"))); return; }
        const ImageContent &image = result.image;
        if (item.kind == 1 && image.bytes.isEmpty()) {
            showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed")));
            return;
        }

        auto *mime = new QMimeData();
        if (asFile) {
            cleanupPasteFiles();
            const QString &suffix = result.file.first;
            const QByteArray &fileBytes = result.file.second;
            const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                + QStringLiteral("/PasteOrbit/PasteFiles");
            if (!QDir().mkpath(directory)) { delete mime; showStatus(AppLocalization::get(QStringLiteral("SaveAsFileFailed"))); return; }
            const QString filePath = directory + QStringLiteral("/PasteOrbit_%1_%2.%3")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
                     QUuid::createUuid().toString(QUuid::WithoutBraces).left(8), suffix);
            QFile file(filePath);
            if (!file.open(QIODevice::WriteOnly) || file.write(fileBytes) != fileBytes.size()) {
                delete mime; showStatus(AppLocalization::get(QStringLiteral("SaveAsFileFailed"))); return;
            }
            mime->setUrls({QUrl::fromLocalFile(filePath)});
        } else if (item.kind == 0) {
            const auto text = decodeTextContent(content);
            mime->setText(text.text);
            if (!plainText) {
                if (!text.html.isEmpty()) mime->setHtml(text.html);
                if (!text.rtf.isEmpty()) {
                    mime->setData(QStringLiteral("text/rtf"), text.rtf);
                    mime->setData(WindowsRtfMime, text.rtf);
                }
                if (text.markdown) mime->setData(QStringLiteral("text/markdown"), text.text.toUtf8());
            }
        } else if (item.kind == 1) {
            if (plainText && !image.text.isEmpty()) {
                mime->setText(image.text);
            } else {
                if (result.decoded.isNull()) {
                    delete mime;
                    showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed")));
                    return;
                }
                // Windows 粘贴目标通常读取位图格式，原始 image/png 等 MIME 单独提供并不可靠。
                mime->setImageData(result.decoded);
                mime->setData(imageMimeType(image.bytes), image.bytes);
                if (!image.text.isEmpty()) mime->setText(image.text);
                if (!image.html.isEmpty()) mime->setHtml(image.html);
                if (!image.rtf.isEmpty()) {
                    mime->setData(QStringLiteral("text/rtf"), image.rtf);
                    mime->setData(WindowsRtfMime, image.rtf);
                }
            }
        } else if (item.kind == 2) {
            const auto paths = QJsonDocument::fromJson(content).array();
            QList<QUrl> urls;
            for (const auto &path : paths) if (path.isString()) urls.push_back(QUrl::fromLocalFile(path.toString()));
            mime->setUrls(urls);
        }
        QApplication::clipboard()->setMimeData(mime);
        ownClipboardSequence_ = clipboardSequence();
        lastClipboardSequence_ = ownClipboardSequence_;
        const quint32 pasteSequence = ownClipboardSequence_;
        const bool keepPanel = isPinned_;
        // 浏览期间目标一直持有焦点，粘贴时不再发送额外的激活或恢复焦点消息。
        const bool targetKeptFocus = passiveBrowsing_ && foregroundTarget() == target;
        if (!keepPanel) hidePanel();
        if (!PasteTarget::prepare((quintptr)target, targetKeptFocus)) {
#ifdef Q_OS_MACOS
            showStatus(AppLocalization::get(QStringLiteral("MacManualPasteHint")));
#else
            showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
#endif
            return;
        }
        auto *timer = new QTimer(this);
        timer->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [this, timer, attempts, target, targetFocus, targetKeptFocus, id, keepPanel, queued, completion,
                                              directTextPaste = item.kind == 0 && !asFile,
                                              generation = pasteGeneration_->loadRelaxed(), pasteSequence] {
            if (generation != pasteGeneration_->loadRelaxed() || clipboardSequence() != pasteSequence) {
                timer->stop(); timer->deleteLater();
                return;
            }
            const auto result = PasteTarget::send((quintptr)target,
                (quintptr)targetFocus, targetKeptFocus, directTextPaste);
            if (result == PasteTarget::Result::WaitingForModifiers && ++*attempts < 100) return;
            timer->stop(); timer->deleteLater();
            if (result != PasteTarget::Result::Sent) {
                showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                return;
            }
            if (queued && !sequenceQueue_.isEmpty() && sequenceQueue_.first().id == id) {
                sequenceQueue_.removeFirst();
                showStatus(AppLocalization::format(QStringLiteral("SequenceRemaining"), {QString::number(sequenceQueue_.size())}));
            }
            const QString path = databasePath_;
            auto *touchWatcher = new QFutureWatcher<QString>(this);
            connect(touchWatcher, &QFutureWatcher<QString>::finished, this, [this, touchWatcher, keepPanel] {
                const auto error = touchWatcher->result();
                touchWatcher->deleteLater();
                if (!error.isEmpty()) showStatus(error);
                if (keepPanel) refreshHistory();
            });
            touchWatcher->setFuture(QtConcurrent::run(&writePool_, [path, id] { return HistoryStore(path).touch(id); }));
        });
        QTimer::singleShot(100, this, [timer] { timer->start(); });
    });
    const auto generationState = pasteGeneration_;
    watcher->setFuture(QtConcurrent::run(&contentPool_, [path, item, plainText, asFile, generationState, generation, editedContent] {
        PasteResult result;
        result.item = item;
        if (generationState->loadRelaxed() != generation) return result;
        result.content = editedContent ? *editedContent : HistoryStore(path).content(item.id);
        if (generationState->loadRelaxed() != generation) return result;
        if (item.kind == 2) {
            QStringList paths;
            for (const auto &entry : QJsonDocument::fromJson(result.content).array()) paths.append(entry.toString());
            result.unavailablePath = firstUnavailableFilePath(paths);
            if (!result.unavailablePath.isEmpty()) return result;
        }
        // 格式识别在读取线程完成，避免大段 JSON/XML 阻塞面板响应。
        if (asFile && item.kind == 0) result.file = textFileContent(decodeTextContent(result.content));
        if (item.kind == 1) {
            result.image = decodeImageContent(result.content);
            if (asFile) {
                const QString type = imageMimeType(result.image.bytes);
                result.file = {type == QStringLiteral("image/jpeg") ? QStringLiteral("jpg") : type.mid(6),
                               result.image.bytes};
            }
            // 恢复位图需要原图；存为文件或已有纯文本时无需解码。
            if (!asFile && !(plainText && !result.image.text.isEmpty())
                && generationState->loadRelaxed() == generation)
                result.decoded = readBoundedImage(result.image.bytes, {});
        }
        return result;
    }));
}
