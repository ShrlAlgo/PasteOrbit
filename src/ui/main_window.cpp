#include "main_window.h"

#include "history_delegate.h"
#include "history_model.h"
#include "localization.h"

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
#include <QFile>
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
#include <QPixmap>
#include <QSettings>
#include <QScrollBar>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QStyleFactory>
#include <QStackedLayout>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <climits>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <system_error>
#include <UIAutomation.h>
#include <oleacc.h>
#include <wrl/client.h>
#include <windows.h>

namespace {
constexpr int HotkeyId = 0x504F;
constexpr int HistoryPageSize = 30;
constexpr UINT TrayResumeMilliseconds = 10 * 60 * 1000;
constexpr auto TextPayloadPrefix = "PasteOrbit.Text/1\n";
constexpr char MixedImagePayloadPrefix[] = "PasteOrbit.Image/2\n";
const QString WindowsRtfMime = QStringLiteral("application/x-qt-windows-mime;value=\"Rich Text Format\"");

void adaptPreviewContrast(QTextDocument *document, const QColor &background, const QColor &defaultText) {
    const auto luminance = [](const QColor &color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
    };
    struct Span { int position; int length; QColor color; };
    QList<Span> changes;
    QHash<quint64, QColor> colors;
    // 仅调整预览文档的低对比文字，同色系适配明暗底色；原始 HTML 与剪贴板内容不变。
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            const QColor ink = format.foreground().style() == Qt::NoBrush ? defaultText : format.foreground().color();
            const QColor paper = format.background().style() != Qt::NoBrush ? format.background().color()
                : block.blockFormat().background().style() != Qt::NoBrush ? block.blockFormat().background().color() : background;
            const quint64 key = (quint64(ink.rgba()) << 32) | paper.rgba();
            if (!colors.contains(key)) {
                const double paperLuminance = luminance(paper);
                const auto contrast = [&](const QColor &color) {
                    const double alpha = color.alphaF();
                    const QColor visible = QColor::fromRgbF(color.redF() * alpha + paper.redF() * (1 - alpha),
                        color.greenF() * alpha + paper.greenF() * (1 - alpha), color.blueF() * alpha + paper.blueF() * (1 - alpha));
                    const double inkLuminance = luminance(visible);
                    return (qMax(inkLuminance, paperLuminance) + 0.05) / (qMin(inkLuminance, paperLuminance) + 0.05);
                };
                QColor adjusted = ink;
                if (contrast(ink) < 4.5) {
                    const double target = paperLuminance < 0.179 ? 1.0 : 0.0;
                    double low = 0, high = 1;
                    for (int step = 0; step < 12; ++step) {
                        const double amount = (low + high) / 2;
                        const QColor candidate = QColor::fromHslF(ink.hslHueF(), ink.hslSaturationF(),
                            ink.lightnessF() + (target - ink.lightnessF()) * amount);
                        if (contrast(candidate) >= 4.5) high = amount;
                        else low = amount;
                    }
                    adjusted = QColor::fromHslF(ink.hslHueF(), ink.hslSaturationF(),
                        ink.lightnessF() + (target - ink.lightnessF()) * high);
                    // 低对比的黑白灰直接采用正文色，避免适配后显得发虚；彩色仍保留色相。
                    if (ink.hslSaturationF() < 0.05 && contrast(defaultText) >= 4.5) adjusted = defaultText;
                }
                colors.insert(key, adjusted);
            }
            if (colors[key] != ink) changes.append({fragment.position(), fragment.length(), colors[key]});
        }
    }
    // 收集后再应用，避免修改格式时使文档片段迭代器失效；不创建文档副本。
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (const auto &span : changes) {
        cursor.setPosition(span.position);
        cursor.setPosition(span.position + span.length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setForeground(span.color);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

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

void stylePopupMenu(QMenu *menu, const QPalette &palette) {
    menu->setStyle(popupStyle());
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlag(Qt::FramelessWindowHint);
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
    QFont font;
    font.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    font.setPixelSize(16);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, QString(glyph));
    return QIcon(pixmap);
}

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

QSize boundedSize(QSize size, int maxWidth, int maxHeight, qint64 maxPixels) {
    if (!size.isValid()) return {};
    double scale = 1.0;
    if (maxWidth > 0) scale = qMin(scale, maxWidth / static_cast<double>(size.width()));
    if (maxHeight > 0) scale = qMin(scale, maxHeight / static_cast<double>(size.height()));
    const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
    if (pixels > maxPixels) scale = qMin(scale, qSqrt(maxPixels / static_cast<double>(pixels)));
    return QSize(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale)));
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

struct ImageContent {
    QByteArray bytes;
    QString text;
    QString html;
    QByteArray rtf;
};

// 旧图片记录直接存原图；新图文记录在原图之外保留剪贴板的其他格式。
ImageContent decodeImageContent(const QByteArray &content) {
    if (!content.startsWith(MixedImagePayloadPrefix)) return {content, {}, {}, {}};
    QBuffer buffer;
    buffer.setData(content);
    if (!buffer.open(QIODevice::ReadOnly) || !buffer.seek(sizeof(MixedImagePayloadPrefix) - 1)) return {};
    QDataStream stream(&buffer);
    stream.setVersion(QDataStream::Qt_6_0);
    ImageContent result;
    stream >> result.bytes >> result.text >> result.html >> result.rtf;
    return stream.status() == QDataStream::Ok && buffer.atEnd() ? result : ImageContent{};
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

bool parseHotkey(const QString &shortcut, UINT &modifiers, UINT &key) {
    modifiers = MOD_NOREPEAT;
    key = 0;
    const auto parts = shortcut.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (auto part : parts) {
        part = part.trimmed().toLower();
        if (part == QStringLiteral("ctrl") || part == QStringLiteral("control")) modifiers |= MOD_CONTROL;
        else if (part == QStringLiteral("alt")) modifiers |= MOD_ALT;
        else if (part == QStringLiteral("shift")) modifiers |= MOD_SHIFT;
        else if (part == QStringLiteral("win") || part == QStringLiteral("windows")
                 || part == QStringLiteral("meta")) modifiers |= MOD_WIN;
        else if (part.size() == 1) key = static_cast<UINT>(VkKeyScanW(part.at(0).toUpper().unicode()) & 0xFF);
        else if (part.size() >= 2 && part.at(0) == QLatin1Char('f')) {
            bool ok = false;
            const int number = part.mid(1).toInt(&ok);
            if (ok && number >= 1 && number <= 24) key = VK_F1 + number - 1;
        } else if (part == QStringLiteral("space")) key = VK_SPACE;
        else if (part == QStringLiteral("enter") || part == QStringLiteral("return")) key = VK_RETURN;
        else if (part == QStringLiteral("tab")) key = VK_TAB;
        else if (part == QStringLiteral("esc") || part == QStringLiteral("escape")) key = VK_ESCAPE;
        else if (part == QStringLiteral("delete")) key = VK_DELETE;
    }
    return key != 0 && (modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) != 0;
}

QString normalizedShortcut(const QString &value) {
    QString result = value;
    result.remove(QLatin1Char(' '));
    return result.toLower();
}

MainWindow *winVHookTarget = nullptr;

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

struct TextContent {
    QString text;
    QString html;
    QByteArray rtf;
    bool markdown = false;
};

struct PreviewResult {
    QString id;
    QString text;
    QString html;
    bool markdown = false;
    QString formatLabel;
    QImage image;
    int kind = 0;
};

class PreviewTextEdit final : public QTextEdit {
public:
    using QTextEdit::QTextEdit;

protected:
    QVariant loadResource(int, const QUrl &) override {
        // 历史中的 HTML 不自动读取本地或远程资源，避免预览触发额外 I/O。
        return {};
    }
};

class HoverPreviewFrame final : public QFrame {
public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent *) override {
        // 透明顶层窗口需自行绘制实体背景，否则浮层内容会透出桌面。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.setBrush(palette().color(QPalette::ToolTipBase));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
    }
};

TextContent decodeTextContent(const QByteArray &content) {
    TextContent result;
    const QByteArray prefix(TextPayloadPrefix);
    if (!content.startsWith(prefix)) {
        result.text = QString::fromUtf8(content);
        return result;
    }
    const auto json = QJsonDocument::fromJson(content.mid(prefix.size())).object();
    result.text = json.value(QStringLiteral("Text")).toString();
    result.html = json.value(QStringLiteral("Html")).toString();
    // RTF 可包含非 UTF-8 字节；新记录使用 Base64，旧记录仍按原格式读取。
    result.rtf = json.contains(QStringLiteral("RtfBase64"))
        ? QByteArray::fromBase64(json.value(QStringLiteral("RtfBase64")).toString().toLatin1())
        : json.value(QStringLiteral("Rtf")).toString().toUtf8();
    result.markdown = json.value(QStringLiteral("Markdown")).toBool();
    return result;
}
}

MainWindow::MainWindow(QString dataDirectory, AppSettings settings)
    : dataDirectory_(std::move(dataDirectory)),
      databasePath_(dataDirectory_ + QStringLiteral("/history.db")),
      settingsPath_(dataDirectory_ + QStringLiteral("/settings.json")),
      settings_(std::move(settings)), store_(databasePath_) {
    // 限制并行解码，避免滚动预览与图片捕获同时放大内存峰值。
    readPool_.setMaxThreadCount(1);
    writePool_.setMaxThreadCount(1);
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    buildUi();
    buildTray();
    applyTheme();
    qApp->installNativeEventFilter(this);
    createWinId();
    registerHotkey();
    updateWinVHook();
    AddClipboardFormatListener(reinterpret_cast<HWND>(winId()));
    lastClipboardSequence_ = GetClipboardSequenceNumber();
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::scheduleCapture);
    clipboardPollTimer_.setInterval(250);
    connect(&clipboardPollTimer_, &QTimer::timeout, this, [this] {
        const DWORD sequence = GetClipboardSequenceNumber();
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
}

MainWindow::~MainWindow() {
    closeHoverPreview();
    closingForExit_ = true;
    qApp->removeNativeEventFilter(this);
    if (winVHook_) {
        UnhookWindowsHookEx(winVHook_);
        winVHook_ = nullptr;
    }
    if (winVHookTarget == this) winVHookTarget = nullptr;
    UnregisterHotKey(reinterpret_cast<HWND>(winId()), HotkeyId);
    RemoveClipboardFormatListener(reinterpret_cast<HWND>(winId()));
    trayIcon_->hide();
    trayIcon_->setContextMenu(nullptr);
    delete trayMenu_;
    readPool_.waitForDone(1000);
    writePool_.waitForDone(1000);
}

void MainWindow::beginStorageOperation() {
    suppressClipboardCapture_ = true;
    hidePanel();
    restoreWasPaused_ = paused_;
    paused_ = true;
    captureTimer_.stop();
    readPool_.waitForDone();
    writePool_.waitForDone();
    QThreadPool::globalInstance()->waitForDone();
    ++historyGeneration_;
    ++previewGeneration_;
    closeTextPreview();
    model_->clearPreview();
    model_->releaseThumbnails();
}

void MainWindow::finishStorageOperation(bool restored) {
    suppressClipboardCapture_ = false;
    paused_ = restoreWasPaused_;
    lastClipboardSequence_ = GetClipboardSequenceNumber();
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
    setWindowTitle(QStringLiteral("PasteOrbit"));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setFixedSize(360, 500);
    auto *root = new QVBoxLayout(this);
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
    auto *title = new QLabel(QStringLiteral("PasteOrbit"), header);
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
        if (isVisible()) SetWindowPos(reinterpret_cast<HWND>(winId()), pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                                      0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        model_->setQuickPasteEnabled(false);
        searchTimer_.start();
    });
    searchLayout->addWidget(searchBox_);
    root->addWidget(searchFrame);

    auto *filterRow = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 10);
    filterLayout->setSpacing(0);
    filterButtons_ = new QButtonGroup(this);
    filterButtons_->setExclusive(true);
    const QStringList labels{QString(QChar(0xE8A9)), QString(QChar(0xE8D2)), QString(QChar(0xE8B9)), QString(QChar(0xE8B7))};
    const QStringList tooltips{QStringLiteral("FilterAllButtonTooltip"), QStringLiteral("FilterTextButtonTooltip"),
                               QStringLiteral("FilterImageButtonTooltip"), QStringLiteral("FilterFilesButtonTooltip")};
    // QButtonGroup 将 -1 保留给“无选择”，按钮序号通过偏移映射到记录类型。
    for (int i = 0; i < labels.size(); ++i) {
        auto *button = new QToolButton(filterRow);
        button->setText(labels.at(i));
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("FilterButton"));
        button->setFont(filterIconFont);
        button->setToolTip(AppLocalization::get(tooltips.at(i)));
        button->installEventFilter(this);
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
    historyList_ = new QListView(listHost);
    historyList_->setObjectName(QStringLiteral("HistoryList"));
    historyList_->setSelectionMode(QAbstractItemView::SingleSelection);
    historyList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    historyList_->verticalScrollBar()->setFixedWidth(10);
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
    hoverPreviewTimer_.setSingleShot(true);
    hoverCloseTimer_.setSingleShot(true);
    connect(&hoverPreviewTimer_, &QTimer::timeout, this, [this] {
        if (!pendingHoverPreviewId_.isEmpty()) showHoverPreview(pendingHoverPreviewId_);
    });
    connect(&hoverCloseTimer_, &QTimer::timeout, this, [this] {
        if (!hoverPreview_) return;
        const QPoint cursor = QCursor::pos();
        const QPoint position = historyList_->viewport()->mapFromGlobal(cursor);
        const QModelIndex index = historyList_->indexAt(position);
        if ((index.isValid() && delegate_->actionAt(historyList_->visualRect(index), position) == HistoryDelegate::Preview)
            || hoverPreview_->geometry().contains(cursor)) return;
        closeHoverPreview();
    });
    connect(model_, &QAbstractItemModel::modelAboutToBeReset, this, &MainWindow::closeTextPreview);
    connect(delegate_, &HistoryDelegate::actionTriggered, this, [this](const QString &id, int action) {
        if (action == HistoryDelegate::Preview) showHoverPreview(id);
        else if (action == HistoryDelegate::Pin) setPinned(id);
        else openRowMenu(id);
    });
    connect(historyList_, &QListView::clicked, this, [this](const QModelIndex &index) {
        const QPoint point = historyList_->viewport()->mapFromGlobal(QCursor::pos());
        if (delegate_->actionAt(historyList_->visualRect(index), point) >= 0) return;
        pasteRecord(index.data(HistoryModel::IdRole).toString());
    });
    connect(historyList_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        closeHoverPreview();
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
    historyStack_->addWidget(emptyLabel_);
    listLayout->addWidget(stack);
    statusLabel_ = new QLabel(listHost);
    statusLabel_->setObjectName(QStringLiteral("StatusText"));
    statusLabel_->setMaximumHeight(0);
    root->addWidget(listHost, 1);
    applyTheme();
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
        QSettings system(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                         QSettings::NativeFormat);
        dark = system.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
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
        theme.backgroundColorMain1 = surface;
        theme.backgroundColorMain2 = page;
        theme.backgroundColorMain3 = hover;
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
        // 统一使用 Windows 中文界面字体，避免 Inter 与汉字回退字体混排造成字重不均。
        const QString family = AppLocalization::activeLanguage() == QStringLiteral("zh-CN")
            ? QStringLiteral("Microsoft YaHei UI") : QStringLiteral("Segoe UI");
        for (QFont *font : {&theme.fontRegular, &theme.fontBold, &theme.fontH1, &theme.fontH2,
                            &theme.fontH3, &theme.fontH4, &theme.fontH5, &theme.fontCaption}) {
            font->setFamily(family);
            font->setStyleStrategy(QFont::PreferAntialias);
        }
        style->setTheme(theme);
        qApp->setPalette(palette);
        QToolTip::setPalette(palette);
        if (trayMenu_) stylePopupMenu(trayMenu_, palette);
    }
    settingsButton_->setIcon(headerGlyphIcon(QChar(0xE713), text, settingsButton_));
    pinButton_->setIcon(headerGlyphIcon(isPinned_ ? QChar(0xE77A) : QChar(0xE718), text, pinButton_));
    clearButton_->setIcon(headerGlyphIcon(QChar(0xE75C), text, clearButton_));
    pageColor_ = page;
    borderColor_ = stroke;
    setStyleSheet(QStringLiteral(
        "QWidget#PasteOrbitPanel{background:transparent;color:%1;border:0;}"
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
    const bool hotkeyChanged = settings_.globalHotKey != settings.globalHotKey;
    const bool winVChanged = settings_.interceptWindowsClipboardShortcut != settings.interceptWindowsClipboardShortcut;
    const bool cleanupChanged = settings_.retentionDays != settings.retentionDays
        || settings_.maxHistoryEntries != settings.maxHistoryEntries;
    settings_ = settings;
    if (themeChanged || languageChanged) applyTheme();
    if (languageChanged) {
        searchBox_->setPlaceholderText(AppLocalization::get(QStringLiteral("MainSearchBoxPlaceholder")));
        emptyLabel_->setText(AppLocalization::get(QStringLiteral("MainEmptyHistoryText")));
        clearButton_->setToolTip(AppLocalization::get(QStringLiteral("ClearHistoryButtonTooltip")));
        const QStringList tooltips{QStringLiteral("FilterAllButtonTooltip"), QStringLiteral("FilterTextButtonTooltip"),
                                   QStringLiteral("FilterImageButtonTooltip"), QStringLiteral("FilterFilesButtonTooltip")};
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
    UnregisterHotKey(reinterpret_cast<HWND>(winId()), HotkeyId);
    UINT modifiers = 0;
    UINT key = 0;
    if (!parseHotkey(settings_.globalHotKey, modifiers, key)
        || !RegisterHotKey(reinterpret_cast<HWND>(winId()), HotkeyId, modifiers, key)) {
        showStatus(AppLocalization::format(QStringLiteral("GlobalHotKeyRegistrationFailed"),
                                           {settings_.globalHotKey}));
    }
}

void MainWindow::updateWinVHook() {
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
        lastClipboardSequence_ = GetClipboardSequenceNumber();
    }
    updateTrayTooltip();
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
    const RECT area = info.rcWork;
    // Windows 下 Qt 保留各屏幕的原点坐标，只缩放屏幕内部的距离。
    QScreen *screen = QGuiApplication::screenAt(QPoint(info.rcMonitor.left, info.rcMonitor.top));
    const qreal scale = screen ? screen->devicePixelRatio() : devicePixelRatioF();
    const int panelWidth = qRound(width() * scale);
    const int panelHeight = qRound(height() * scale);
    int x = preferCursor ? point.x - panelWidth / 2 : point.x;
    int y = preferCursor ? point.y - panelHeight - 14 : point.y + 14;
    if (inputBounds) {
        if (x + panelWidth > area.right) x = inputBounds->right - panelWidth;
        if (y + panelHeight > area.bottom) y = inputBounds->top - panelHeight - 14;
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
            const QRect excluded(bounds.left - 8, bounds.top - 8,
                                 qMax(1L, bounds.right - bounds.left) + 16,
                                 qMax(1L, bounds.bottom - bounds.top) + 16);
            const QPoint candidates[]{
                {point.x, excluded.bottom() + 7},
                {point.x, excluded.top() - panelHeight - 6},
                {excluded.right() + 7, point.y},
                {excluded.left() - panelWidth - 6, point.y}};
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
    if (suppressClipboardCapture_) return;
    ++pasteGeneration_;
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
        positionPanel(fromTray, bounds, control);
        if (!isVisible()) show();
        raise();
        activateWindow();
        SetForegroundWindow(reinterpret_cast<HWND>(winId()));
        if (!startupUpdateCheckRequested_) {
            startupUpdateCheckRequested_ = true;
            emit firstPanelShown();
        }
        if (isPinned_) SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE);
        if (historyDirty_ || (searchBox_->text().isEmpty() && model_->rowCount() == 0 && !loadingPage_))
            refreshHistory();
        historyList_->verticalScrollBar()->setValue(0);
        if (model_->rowCount() > 0) historyList_->setCurrentIndex(model_->index(0));
        historyList_->setFocus(Qt::ShortcutFocusReason);
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

void MainWindow::hidePanel() {
    ++panelShowGeneration_;
    ++pasteGeneration_;
    if (!isVisible()) return;
    hide();
    if (!searchBox_->text().isEmpty()) {
        const QSignalBlocker blocker(searchBox_);
        searchBox_->clear();
        refreshHistory();
    } else {
        // 隐藏后丢弃未完成的后续页，避免裁剪后的列表又被异步回调填满。
        if (loadingMore_) {
            ++historyGeneration_;
            loadingMore_ = false;
        }
        model_->trimToFirstPage();
        nextCursor_ = firstPageCursor_;
    }
    historyList_->clearSelection();
    ++previewGeneration_;
    closeTextPreview();
    model_->clearPreview();
    historyList_->doItemsLayout();
    highResolutionPreviewId_.clear();
    highResolutionLoadingId_.clear();
    model_->releaseThumbnails();
    targetWindow_ = nullptr;
    targetFocusWindow_ = nullptr;
}

void MainWindow::refreshHistory() {
    const int generation = ++historyGeneration_;
    historyDirty_ = false;
    closeTextPreview();
    model_->setQuickPasteEnabled(false);
    loadingPage_ = true;
    loadingMore_ = false;
    nextCursor_.reset();
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    auto *watcher = new QFutureWatcher<HistoryPage>(this);
    connect(watcher, &QFutureWatcher<HistoryPage>::finished, this, [this, watcher, generation] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (generation != historyGeneration_) return;
        loadingPage_ = false;
        if (!page.error.isEmpty()) { historyDirty_ = true; showStatus(page.error); }
        model_->setFirstPage(page);
        model_->setQuickPasteEnabled(searchBox_->text().isEmpty());
        firstPageCursor_ = page.next;
        nextCursor_ = page.next;
        unpinnedCount_ = page.unpinnedCount;
        updateEmptyState();
        updateVisibleThumbnails();
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, query, kind] {
        return HistoryStore(path).search(query, kind, std::nullopt, HistoryPageSize);
    }));
}

void MainWindow::loadMoreHistory() {
    if (!isVisible() || loadingPage_ || loadingMore_ || !nextCursor_) return;
    loadingMore_ = true;
    const int generation = historyGeneration_;
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    const auto cursor = nextCursor_;
    auto *watcher = new QFutureWatcher<HistoryPage>(this);
    connect(watcher, &QFutureWatcher<HistoryPage>::finished, this, [this, watcher, generation] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (generation != historyGeneration_) return;
        loadingMore_ = false;
        if (!page.error.isEmpty()) { showStatus(page.error); nextCursor_.reset(); return; }
        model_->appendPage(page);
        nextCursor_ = page.next;
        updateVisibleThumbnails();
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, query, kind, cursor] {
        return HistoryStore(path).search(query, kind, cursor, HistoryPageSize);
    }));
}

void MainWindow::updateVisibleThumbnails() {
    if (!historyList_ || !model_) return;
    if (hoverPreview_) {
        const int row = model_->rowForId(hoverPreviewId_);
        if (row < 0 || !historyList_->visualRect(model_->index(row)).intersects(historyList_->viewport()->rect()))
            closeHoverPreview();
    }
    const int previewRow = model_->rowForId(model_->previewId());
    if (previewRow >= 0 && !historyList_->visualRect(model_->index(previewRow)).intersects(historyList_->viewport()->rect())) {
        ++previewGeneration_;
        closeTextPreview();
        model_->clearPreview();
        historyList_->doItemsLayout();
        highResolutionPreviewId_.clear();
        highResolutionLoadingId_.clear();
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
    updateTextPreviewGeometry();
}

QString textFormatLabel(const TextContent &content) {
    const char *richMarkers[]{"<a ", "<b>", "<b ", "<strong", "<i>", "<i ", "<em", "<u>",
                              "<u ", "<s>", "<s ", "<table", "<img", "<ul", "<ol", "<li",
                              "<h1", "<h2", "<h3", " style=", " class="};
    bool formattedHtml = false;
    for (const char *marker : richMarkers) {
        if (content.html.contains(QLatin1String(marker), Qt::CaseInsensitive)) {
            formattedHtml = true;
            break;
        }
    }
    if (formattedHtml && !content.rtf.isEmpty()) return QStringLiteral("HTML · RTF");
    if (formattedHtml) return QStringLiteral("HTML");
    if (content.markdown) return QStringLiteral("Markdown");
    return content.rtf.isEmpty() ? QString{} : QStringLiteral("RTF");
}

void MainWindow::setFilter(int kind) {
    selectedKind_ = kind;
    refreshHistory();
    if (historyList_->isVisible()) historyList_->setFocus(Qt::ShortcutFocusReason);
    else filterButtons_->button(kind + 1)->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::switchFilter(int direction) {
    const int next = (selectedKind_ + 1 + direction + 4) % 4;
    filterButtons_->button(next)->setChecked(true);
    setFilter(next - 1);
}

void MainWindow::updateEmptyState() {
    const bool empty = model_->rowCount() == 0 && !loadingPage_;
    // 空结果会隐藏列表，先将键盘焦点交回当前筛选按钮。
    if (empty && historyList_->hasFocus())
        filterButtons_->button(selectedKind_ + 1)->setFocus(Qt::ShortcutFocusReason);
    historyStack_->setCurrentWidget(empty ? static_cast<QWidget *>(emptyLabel_) : static_cast<QWidget *>(historyList_));
}

void MainWindow::showStatus(const QString &message) {
    if (message.isEmpty()) return;
    QToolTip::showText(QCursor::pos(), message, this, rect(), 2800);
}

bool MainWindow::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    const auto *native = static_cast<MSG *>(message);
    // 消息过滤期间不能用 winId() 创建窗口，否则窗口创建消息会递归进入过滤器。
    const HWND panel = reinterpret_cast<HWND>(internalWinId());
    if (panel && native->hwnd == panel && native->message == WM_SETTINGCHANGE
        && settings_.themeMode == QStringLiteral("System"))
        QTimer::singleShot(0, this, &MainWindow::applyTheme);
    // 鼠标激活置顶面板之前保存最新目标，此时外部输入控件尚未失去焦点。
    if (isPinned_ && panel && native->hwnd == panel
        && native->message == WM_MOUSEACTIVATE) capturePasteTarget(GetForegroundWindow());
    if (isPinned_ && panel && native->hwnd == panel
        && native->message == WM_ACTIVATE && LOWORD(native->wParam) != WA_INACTIVE) {
        const HWND previous = reinterpret_cast<HWND>(native->lParam);
        if (previous != targetWindow_) capturePasteTarget(previous);
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
    return false;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (historyList_ && watched == historyList_->viewport()) {
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::Leave) {
            const QRect row = event->type() != QEvent::Leave
                ? historyList_->visualRect(historyList_->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint()))
                : QRect{};
            if (hoveredHistoryRow_ != row) historyList_->viewport()->update(hoveredHistoryRow_);
            hoveredHistoryRow_ = row;
            if (row.isValid()) historyList_->viewport()->update(row);
        }
        if (event->type() == QEvent::Resize && textPreview_)
            QTimer::singleShot(0, this, &MainWindow::updateTextPreviewGeometry);
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const QModelIndex hovered = historyList_->indexAt(help->pos());
            if (hovered.isValid()) {
                const int action = delegate_->actionAt(historyList_->visualRect(hovered), help->pos());
                QString label;
                if (action == HistoryDelegate::Preview) {
                    QToolTip::hideText();
                    // 静止悬停也能打开预览，不依赖后续 MouseMove 事件。
                    showHoverPreview(hovered.data(HistoryModel::IdRole).toString());
                    return true;
                }
                if (action == HistoryDelegate::Pin)
                    label = AppLocalization::get(hovered.data(HistoryModel::PinnedRole).toBool()
                        ? QStringLiteral("UnpinItem") : QStringLiteral("PinItem"));
                else if (action == HistoryDelegate::More)
                    label = AppLocalization::get(QStringLiteral("RecordMoreButtonTooltip"));
                if (!label.isEmpty()) { QToolTip::showText(help->globalPos(), label, historyList_->viewport()); return true; }
            }
            QToolTip::hideText();
            return true;
        }
        QPoint position(-1, -1);
        if (event->type() == QEvent::Wheel) position = static_cast<QWheelEvent *>(event)->position().toPoint();
        else if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
                 || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
            position = static_cast<QMouseEvent *>(event)->position().toPoint();
        }
        if (previewDragging_ && event->type() == QEvent::MouseMove) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->buttons().testFlag(Qt::LeftButton)) {
                model_->setPreviewPan(previewPanStart_ + mouse->position() - previewDragStart_);
                return true;
            }
            previewDragging_ = false;
        }
        const QModelIndex index = position.x() >= 0 ? historyList_->indexAt(position) : QModelIndex{};
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::Leave) {
            const QString id = event->type() == QEvent::MouseMove && index.isValid()
                && delegate_->actionAt(historyList_->visualRect(index), position) == HistoryDelegate::Preview
                    ? index.data(HistoryModel::IdRole).toString() : QString{};
            if (!id.isEmpty()) {
                hoverCloseTimer_.stop();
                if (hoverPreviewId_ != id && pendingHoverPreviewId_ != id) {
                    pendingHoverPreviewId_ = id;
                    hoverPreviewTimer_.start(300);
                }
            } else {
                pendingHoverPreviewId_.clear();
                hoverPreviewTimer_.stop();
                if (hoverPreview_ && !hoverCloseTimer_.isActive()) hoverCloseTimer_.start(180);
            }
        }
        if (index.isValid() && index.data(HistoryModel::IdRole).toString() == model_->previewId()
            && delegate_->previewRect(historyList_->visualRect(index)).contains(position)) {
            const int kind = index.data(HistoryModel::KindRole).toInt();
            if (event->type() == QEvent::MouseButtonPress) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    previewDragging_ = kind == 1;
                    previewDragStart_ = mouse->position().toPoint();
                    previewPanStart_ = index.data(HistoryModel::PreviewPanRole).toPointF();
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease && previewDragging_) {
                previewDragging_ = false;
                return true;
            } else if (event->type() == QEvent::MouseButtonDblClick) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::MiddleButton && kind == 1) {
                    model_->setPreviewZoom(1.0, {});
                    return true;
                }
            } else if (event->type() == QEvent::Wheel) {
                const auto *wheel = static_cast<QWheelEvent *>(event);
                const QRect area = delegate_->previewRect(historyList_->visualRect(index)).adjusted(8, 6, -8, -6);
                const QPointF anchor = wheel->position() - area.topLeft();
                const QPointF oldPan = index.data(HistoryModel::PreviewPanRole).toPointF();
                if (kind == 1 && !qvariant_cast<QImage>(index.data(HistoryModel::PreviewImageRole)).isNull()) {
                    const double oldZoom = index.data(HistoryModel::PreviewZoomRole).toDouble();
                    const double factor = wheel->angleDelta().y() > 0 ? 1.25 : 0.8;
                    const double zoom = qBound(1.0, oldZoom * factor, 4.0);
                    if (!qFuzzyCompare(zoom, oldZoom)) {
                        const QPointF pan = anchor - (anchor - oldPan) * (zoom / oldZoom);
                        model_->setPreviewZoom(zoom, pan);
                        if (zoom >= 1.25) upgradePreviewImage(model_->previewId());
                    }
                    return true;
                }
            }
        }
    }
    if (watched == hoverPreview_) {
        if (event->type() == QEvent::Enter) hoverCloseTimer_.stop();
        else if (event->type() == QEvent::Leave) hoverCloseTimer_.start(180);
    }
    if (hoverImageScroll_ && (watched == hoverImageLabel_ || watched == hoverImageScroll_->viewport())) {
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
    if ((watched == historyList_ || watched == searchBox_
         || watched->objectName() == QStringLiteral("FilterButton")) && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) { hidePanel(); return true; }
        if (watched == searchBox_ && key->key() == Qt::Key_Down && model_->rowCount() > 0) {
            historyList_->setCurrentIndex(model_->index(0));
            historyList_->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
        if (watched == historyList_ && key->key() == Qt::Key_Up && historyList_->currentIndex().row() <= 0) {
            searchBox_->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
        if (watched != searchBox_ && key->modifiers() == Qt::NoModifier
            && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            switchFilter(key->key() == Qt::Key_Right ? 1 : -1);
            return true;
        }
        if (watched == historyList_ && key->modifiers() == Qt::NoModifier
            && searchBox_->text().isEmpty() && key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9) {
            const int row = model_->quickPasteRow(key->key() - Qt::Key_1 + 1);
            if (const auto *item = model_->itemAt(row)) { pasteRecord(item->id); return true; }
        }
        if (watched == historyList_ && historyList_->currentIndex().isValid()) {
            const auto id = historyList_->currentIndex().data(HistoryModel::IdRole).toString();
            if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier)) {
                const QRect card = historyList_->visualRect(historyList_->currentIndex());
                openRowMenu(id, historyList_->viewport()->mapToGlobal(card.topRight()));
                return true;
            }
            QString pressed = QKeySequence(key->modifiers() | key->key()).toString(QKeySequence::PortableText);
            pressed.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
            const auto normalized = normalizedShortcut(pressed);
            if (normalized == normalizedShortcut(settings_.plainTextPasteShortcut)) {
                pasteRecord(id, true); return true;
            }
            if (normalized == normalizedShortcut(settings_.pasteShortcut)) {
                pasteRecord(id); return true;
            }
            if (normalized == normalizedShortcut(settings_.previewShortcut)) {
                showHoverPreview(id); return true;
            }
            if (normalized == normalizedShortcut(settings_.pinShortcut)) {
                setPinned(id); return true;
            }
            if (normalized == normalizedShortcut(settings_.pasteAsFileShortcut)) {
                pasteRecord(id, false, true); return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool MainWindow::event(QEvent *event) {
    const bool handled = QWidget::event(event);
    // 托盘唤出的顶层窗口失去激活时不会触发 QWidget::focusOutEvent。
    if (event->type() == QEvent::WindowDeactivate) {
        if (isPinned_ && !settingsWindowOpen_) {
            targetFocusWindow_ = nullptr;
            const HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground) GetWindowThreadProcessId(foreground, &processId);
            if (processId && processId != GetCurrentProcessId()) targetWindow_ = foreground;
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
    const int generation = panelShowGeneration_;
    QTimer::singleShot(120, this, [this, generation] {
        if (generation != panelShowGeneration_) return;
        if (isVisible() && !isActiveWindow() && !isPinned_ && !settingsWindowOpen_
            && settings_.autoHideOnDeactivate && !qApp->activePopupWidget() && !qApp->activeModalWidget()) {
            hidePanel();
        }
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (closingForExit_) event->accept();
    else { event->ignore(); hidePanel(); }
}

void MainWindow::closeHoverPreview() {
    ++hoverPreviewGeneration_;
    hoverPreviewTimer_.stop();
    hoverCloseTimer_.stop();
    pendingHoverPreviewId_.clear();
    hoverPreviewId_.clear();
    hoverImageScroll_ = nullptr;
    hoverImageLabel_ = nullptr;
    hoverImageSize_ = {};
    hoverImageZoom_ = 1.0;
    hoverImageDragging_ = false;
    delete hoverPreview_;
    hoverPreview_ = nullptr;
}

void MainWindow::showHoverPreview(QString id) {
    // 按值保存 ID；关闭旧浮层会清空待预览成员，引用传参会导致首次加载空记录。
    if (id.isEmpty() || (hoverPreview_ && hoverPreviewId_ == id)) return;
    const int row = model_->rowForId(id);
    const auto *item = model_->itemAt(row);
    if (!item) return;
    const int kind = item->kind;
    closeHoverPreview();
    QToolTip::hideText();
    hoverPreviewId_ = id;
    const int generation = hoverPreviewGeneration_;

    // 浮层独立于列表项，悬停预览不会改变卡片高度或触发列表重排。
    auto *popup = new HoverPreviewFrame(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    popup->setObjectName(QStringLiteral("HoverPreview"));
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->setFocusPolicy(Qt::NoFocus);
    popup->setPalette(qApp->palette());
    const bool dark = popup->palette().color(QPalette::Window).lightness() < 128;
    const QColor previewBackground(dark ? QStringLiteral("#272B30") : QStringLiteral("#EBEEF2"));
    const QColor previewText(dark ? QStringLiteral("#DCE0E5") : QStringLiteral("#252A30"));
    const QColor previewBorder(dark ? QStringLiteral("#454B52") : QStringLiteral("#CCD2DA"));
    popup->setStyleSheet(QStringLiteral(
        "QScrollArea{background:transparent;border:0;}"
        "QLabel{background:transparent;color:%1;border:0;}"
        "QLabel[previewBadge=\"true\"]{background:%2;border:1px solid %3;border-radius:5px;padding:3px 7px;}"
        "QFrame#HoverPreviewContent{background:%4;border:1px solid %5;border-radius:7px;}"
    ).arg(popup->palette().color(QPalette::ToolTipText).name(),
          popup->palette().color(QPalette::AlternateBase).name(),
          popup->palette().color(QPalette::Mid).name(),
          previewBackground.name(), previewBorder.name()));
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(10, 10, 10, 10);
    // 来源应用、格式与容量统计放在浮层页头，不再占用卡片内容宽度。
    const QString source = item->sourceApplication.isEmpty()
        ? AppLocalization::get(QStringLiteral("UnknownApplication")) : item->sourceApplication;
    const QString format = model_->index(row).data(HistoryModel::FormatRole).toString();
    const QString statistics = kind == 0
        ? AppLocalization::format(QStringLiteral("CharacterCount"), {QString::number(item->searchTextLength)})
        : kind == 1 ? QStringLiteral("%1 KB").arg(qMax(1.0, item->contentSize / 1024.0), 0, 'f', 1)
                    : AppLocalization::get(QStringLiteral("ContentTypeFiles"));
    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(6);
    const auto makeBadge = [popup](const QString &text) {
        auto *label = new QLabel(text, popup);
        label->setTextFormat(Qt::PlainText);
        label->setProperty("previewBadge", true);
        label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        QFont font = label->font();
        font.setPixelSize(12);
        label->setFont(font);
        return label;
    };
    auto *sourceBadge = makeBadge(source);
    auto *statisticsBadge = makeBadge(statistics);
    heading->addWidget(sourceBadge);
    int detailsWidth = statisticsBadge->fontMetrics().horizontalAdvance(statistics) + 16;
    if (!format.isEmpty()) {
        auto *formatBadge = makeBadge(format);
        heading->addWidget(formatBadge);
        detailsWidth += formatBadge->fontMetrics().horizontalAdvance(format) + 16 + heading->spacing();
    }
    heading->addStretch();
    heading->addWidget(statisticsBadge);
    layout->addLayout(heading);
    // 内容背景独立于页头标签，保持文字与图片预览的内边距一致。
    auto *content = new QFrame(popup);
    content->setObjectName(QStringLiteral("HoverPreviewContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(content, 1);
    popup->installEventFilter(this);
    hoverPreview_ = popup;

    const QPoint anchor = historyList_->viewport()->mapToGlobal(QPoint(0, 0));
    QScreen *screen = QGuiApplication::screenAt(anchor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(anchor, QSize(800, 600));
    const int width = qMin(kind == 1 ? 520 : 460, available.width() - 20);
    const int height = qMin(kind == 1 ? 400 : 320, available.height() - 20);
    popup->setFixedSize(width, height);
    const int sourceWidth = qMax(0, width - 20 - detailsWidth - heading->spacing() - 16);
    sourceBadge->setText(sourceBadge->fontMetrics().elidedText(source, Qt::ElideRight, sourceWidth));
    int x = frameGeometry().right() + 8;
    if (x + width > available.right()) x = frameGeometry().left() - width - 8;
    x = qBound(available.left(), x, available.right() - width + 1);
    const int y = qBound(available.top(), anchor.y(), available.bottom() - height + 1);
    popup->move(x, y);
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this, [this, watcher, generation, id, content, previewBackground, previewText] {
        const PreviewResult result = watcher->result();
        watcher->deleteLater();
        if (generation != hoverPreviewGeneration_ || id != hoverPreviewId_ || !hoverPreview_) return;
        auto *layout = content->layout();
        if (result.kind == 1) {
            if (result.image.isNull()) {
                auto *label = new QLabel(AppLocalization::get(QStringLiteral("ImagePreviewUnavailable")), content);
                label->setAlignment(Qt::AlignCenter);
                layout->addWidget(label);
            } else {
                hoverImageSize_ = result.image.size();
                auto *scroll = new QScrollArea(content);
                scroll->setFrameShape(QFrame::NoFrame);
                scroll->verticalScrollBar()->setFixedWidth(historyList_->verticalScrollBar()->width());
                scroll->horizontalScrollBar()->setFixedHeight(historyList_->verticalScrollBar()->width());
                scroll->setAlignment(Qt::AlignCenter);
                scroll->setWidgetResizable(false);
                scroll->viewport()->setAutoFillBackground(false);
                scroll->viewport()->setStyleSheet(QStringLiteral("background:transparent;"));
                auto *label = new QLabel(scroll);
                label->setAlignment(Qt::AlignCenter);
                label->setScaledContents(true);
                label->installEventFilter(this);
                scroll->setWidget(label);
                scroll->viewport()->installEventFilter(this);
                layout->addWidget(scroll);
                hoverImageScroll_ = scroll;
                hoverImageLabel_ = label;
                // 标签持有显示用 QPixmap，不再额外常驻一份解码后的 QImage。
                hoverImageLabel_->setPixmap(QPixmap::fromImage(result.image));
            }
        } else {
            auto *preview = new PreviewTextEdit(content);
            preview->setReadOnly(true);
            preview->setUndoRedoEnabled(false);
            preview->setAcceptDrops(false);
            preview->setFocusPolicy(Qt::NoFocus);
            preview->setFrameShape(QFrame::NoFrame);
            preview->setLineWrapMode(QTextEdit::WidgetWidth);
            preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            preview->verticalScrollBar()->setFixedWidth(historyList_->verticalScrollBar()->width());
            const bool richText = !result.html.isEmpty() || result.markdown;
            preview->setStyleSheet(QStringLiteral("QTextEdit{background:transparent;border:0;color:%1;}")
                .arg(previewText.name()));
            preview->viewport()->setAutoFillBackground(false);
            // 优先使用剪贴板提供的富文本格式；普通文本保持原样。
            if (!result.html.isEmpty()) preview->setHtml(result.html);
            else if (result.markdown) preview->setMarkdown(result.text);
            else preview->setPlainText(result.text);
            if (richText) adaptPreviewContrast(preview->document(), previewBackground, previewText);
            layout->addWidget(preview);
        }
        // 内容与嵌套布局都准备好后再首次显示，避免加载占位符及首帧尺寸变化闪烁。
        hoverPreview_->layout()->activate();
        layout->activate();
        if (hoverImageScroll_) updateHoverPreviewImage();
        hoverPreview_->show();
    });
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, id, kind] {
        PreviewResult result;
        result.id = id;
        result.kind = kind;
        const QByteArray content = HistoryStore(path).content(id, false);
        if (kind == 1) {
            // 优先从原图按上限解码，预览图仅作为原图损坏时的回退。
            for (const bool preferPreview : {false, true}) {
                const QByteArray bytes = decodeImageContent(preferPreview
                    ? HistoryStore(path).content(id, true) : content).bytes;
                if (bytes.isEmpty()) continue;
                QBuffer buffer;
                buffer.setData(bytes);
                if (!buffer.open(QIODevice::ReadOnly)) continue;
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                const QSize target = boundedSize(reader.size(), 1600, 0, 4'000'000);
                if (target.isValid()) reader.setScaledSize(target);
                result.image = reader.read();
                if (!result.image.isNull()) break;
            }
        } else if (kind == 0) {
            const auto text = decodeTextContent(content);
            result.text = text.text;
            result.html = text.html;
            result.markdown = text.markdown;
        } else {
            const auto paths = QJsonDocument::fromJson(content).array();
            QStringList values;
            values.reserve(paths.size());
            for (const auto &pathValue : paths) values.push_back(pathValue.toString());
            result.text = values.join(QLatin1Char('\n'));
        }
        return result;
    }));
}

void MainWindow::updateHoverPreviewImage() {
    if (!hoverPreview_ || !hoverImageLabel_ || !hoverImageScroll_ || hoverImageSize_.isEmpty()) return;
    // 首次显示前 viewport 尚未稳定；用已激活布局的滚动容器宽度，包含内容背景的内边距。
    const int availableWidth = qMax(1, hoverImageScroll_->width() - hoverImageScroll_->verticalScrollBar()->width());
    const double fit = availableWidth / static_cast<double>(hoverImageSize_.width());
    hoverImageLabel_->resize(hoverImageSize_ * (fit * hoverImageZoom_));
}

void MainWindow::togglePreview(const QString &id) {
    if (model_->previewId() == id) {
        ++previewGeneration_;
        closeTextPreview();
        model_->clearPreview();
        historyList_->doItemsLayout();
        highResolutionPreviewId_.clear();
        highResolutionLoadingId_.clear();
        updateVisibleThumbnails();
        return;
    }
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
    const int kind = item->kind;
    const int generation = ++previewGeneration_;
    closeTextPreview();
    highResolutionPreviewId_.clear();
    highResolutionLoadingId_.clear();
    model_->beginPreview(id);
    historyList_->doItemsLayout();
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != previewGeneration_ || result.id != model_->previewId()) return;
        if (result.kind == 1) model_->setPreviewImage(result.id, result.image);
        else model_->finishTextPreview(result.id);
        if (result.kind == 0) model_->setFormatLabel(result.id, result.formatLabel);
        historyList_->doItemsLayout();
        if (result.kind != 1) showTextPreview(result.text, result.html, result.markdown);
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, id, kind] {
        PreviewResult result;
        result.id = id;
        result.kind = kind;
        const QByteArray content = HistoryStore(path).content(id, kind == 1);
        if (kind == 1) {
            QBuffer buffer;
            buffer.setData(decodeImageContent(content).bytes);
            if (buffer.open(QIODevice::ReadOnly)) {
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                const QSize target = boundedSize(reader.size(), 800, 0, 1'000'000);
                if (target.isValid()) reader.setScaledSize(target);
                result.image = reader.read();
            }
        } else if (kind == 0) {
            const auto text = decodeTextContent(content);
            result.text = text.text;
            result.html = text.html;
            result.markdown = text.markdown;
            result.formatLabel = textFormatLabel(text);
        } else {
            const auto paths = QJsonDocument::fromJson(content).array();
            QStringList values;
            values.reserve(paths.size());
            for (const auto &pathValue : paths) values.push_back(pathValue.toString());
            result.text = values.join(QLatin1Char('\n'));
        }
        return result;
    }));
}

void MainWindow::showTextPreview(const QString &text, const QString &html, bool markdown) {
    closeTextPreview();
    auto *preview = new PreviewTextEdit(historyList_->viewport());
    preview->setReadOnly(true);
    preview->setAcceptDrops(false);
    preview->setFocusPolicy(Qt::NoFocus);
    preview->setFrameShape(QFrame::NoFrame);
    preview->setLineWrapMode(QTextEdit::WidgetWidth);
    preview->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    preview->setStyleSheet(QStringLiteral("QTextEdit{background:transparent;border:0;padding:0;}"));
    if (!html.isEmpty()) preview->setHtml(html);
    else if (markdown) preview->setMarkdown(text);
    else preview->setPlainText(text);
    textPreview_ = preview;
    updateTextPreviewGeometry();
}

void MainWindow::closeTextPreview() {
    closeHoverPreview();
    delete textPreview_;
    textPreview_ = nullptr;
}

void MainWindow::updateTextPreviewGeometry() {
    if (!textPreview_) return;
    const int row = model_->rowForId(model_->previewId());
    if (row < 0) { closeTextPreview(); return; }
    const QRect card = historyList_->visualRect(model_->index(row));
    const QRect content = delegate_->previewRect(card).adjusted(8, 6, -8, -6);
    textPreview_->setGeometry(content);
    textPreview_->setVisible(content.intersects(historyList_->viewport()->rect()));
}

void MainWindow::upgradePreviewImage(const QString &id) {
    if (id.isEmpty() || id == highResolutionPreviewId_ || id == highResolutionLoadingId_) return;
    highResolutionLoadingId_ = id;
    const int generation = previewGeneration_;
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, id, generation] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (highResolutionLoadingId_ == id) highResolutionLoadingId_.clear();
        if (generation != previewGeneration_ || id != model_->previewId() || image.isNull()) return;
        highResolutionPreviewId_ = id;
        model_->setPreviewImage(id, image);
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, id] {
        const QByteArray content = decodeImageContent(HistoryStore(path).content(id, true)).bytes;
        QBuffer buffer;
        buffer.setData(content);
        if (!buffer.open(QIODevice::ReadOnly)) return QImage{};
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize target = boundedSize(reader.size(), 1600, 0, 4'000'000);
        if (target.isValid()) reader.setScaledSize(target);
        return reader.read();
    }));
}

void MainWindow::setPinned(const QString &id) {
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
    const bool newState = !item->pinned;
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) showStatus(error);
        refreshHistory();
    });
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, id, newState] {
        return HistoryStore(path).setPinned(id, newState);
    }));
}

void MainWindow::deleteRecord(const QString &id) {
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
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
    if (unpinnedCount_ <= 0) return;
    const QString message = AppLocalization::format(QStringLiteral("ClearCurrentListMessage"),
                                                     {QString::number(unpinnedCount_)});
    if (QMessageBox::question(this, AppLocalization::get(QStringLiteral("ClearCurrentListTitle")), message,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const QString path = databasePath_;
    const QString query = searchBox_->text();
    const int kind = selectedKind_;
    auto *watcher = new QFutureWatcher<QPair<int, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.second.isEmpty()) showStatus(result.second);
        refreshHistory();
        if (result.first > 0) showStatus(AppLocalization::format(QStringLiteral("CurrentListCleared"),
                                                                 {QString::number(result.first)}));
    });
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, query, kind] {
        QString error;
        const int deleted = HistoryStore(path).removeMatching(query, kind, &error);
        if (error.isEmpty()) error = HistoryStore(path).compact(true);
        return qMakePair(deleted, error);
    }));
}

void MainWindow::openRowMenu(const QString &id, std::optional<QPoint> position) {
    const auto *item = model_->itemAt(model_->rowForId(id));
    if (!item) return;
    // 菜单作为独立弹窗，避免继承透明历史面板的样式表。
    auto *menu = new QMenu();
    stylePopupMenu(menu, qApp->palette());
    QAction *plain = nullptr;
    QAction *asFile = nullptr;
    if (item->kind == 0) plain = menu->addAction(AppLocalization::get(QStringLiteral("PastePlainTextMenuItem.Text")));
    if (item->kind == 0 || item->kind == 1) asFile = menu->addAction(AppLocalization::get(QStringLiteral("PasteAsFileMenuItem.Text")));
    if (plain || asFile) menu->addSeparator();
    auto *remove = menu->addAction(AppLocalization::get(QStringLiteral("DeleteRecordMenuItem.Text")));
    if (plain) connect(plain, &QAction::triggered, this, [this, id] { pasteRecord(id, true); });
    if (asFile) connect(asFile, &QAction::triggered, this, [this, id] { pasteRecord(id, false, true); });
    connect(remove, &QAction::triggered, this, [this, id] {
        // 删除留在历史面板，菜单关闭后重新激活以免失焦回调误隐藏。
        activateWindow();
        SetForegroundWindow(reinterpret_cast<HWND>(winId()));
        deleteRecord(id);
    });
    connect(menu, &QMenu::aboutToHide, this, &MainWindow::scheduleAutoHide);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position.value_or(QCursor::pos()));
}

void MainWindow::scheduleCapture() {
    if (paused_ || suppressClipboardCapture_) return;
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence && sequence == ownClipboardSequence_) {
        lastClipboardSequence_ = sequence;
        return;
    }
    if (sequence && sequence != lastClipboardSequence_) captureTimer_.start();
}

bool MainWindow::sourceIsExcluded() const {
    const QString source = processName(GetClipboardOwner());
    if (source.isEmpty()) return false;
    QString configured = settings_.excludedApplications;
    configured.replace(QLatin1Char(','), QLatin1Char(';'));
    configured.replace(QLatin1Char('\r'), QLatin1Char(';'));
    configured.replace(QLatin1Char('\n'), QLatin1Char(';'));
    const auto excluded = configured.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (auto name : excluded) {
        name = name.trimmed().toCaseFolded();
        if (!name.isEmpty() && !name.endsWith(QStringLiteral(".exe"))) name.append(QStringLiteral(".exe"));
        if (source == name) return true;
    }
    return false;
}

void MainWindow::captureClipboard() {
    if (paused_ || suppressClipboardCapture_ || captureSaving_) return;
    const DWORD sequence = GetClipboardSequenceNumber();
    if (!sequence || sequence == lastClipboardSequence_ || sequence == ownClipboardSequence_) return;
    if (sourceIsExcluded()) { lastClipboardSequence_ = sequence; return; }
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    const QString source = processName(GetClipboardOwner());
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

void MainWindow::saveCapture(ClipboardCapture capture, QImage fallbackImage, DWORD sequence) {
    captureSaving_ = true;
    const int retention = settings_.retentionDays;
    const int maximum = settings_.maxHistoryEntries;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, sequence] {
        const QString error = watcher->result();
        watcher->deleteLater();
        captureSaving_ = false;
        lastClipboardSequence_ = sequence;
        if (!error.isEmpty()) showStatus(error);
        else {
            // 隐藏期间只标记待刷新，下一次打开时再查询第一页。
            historyDirty_ = true;
            if (isVisible()) refreshHistory();
        }
        if (GetClipboardSequenceNumber() != lastClipboardSequence_) scheduleCapture();
    });
    const QString path = databasePath_;
    watcher->setFuture(QtConcurrent::run(&writePool_, [path, retention, maximum, capture = std::move(capture),
                                                        fallback = std::move(fallbackImage)]() mutable {
        if (capture.kind == 1) {
            prepareImageCapture(capture, fallback);
            encodeMixedImage(capture);
        }
        HistoryStore store(path);
        QString error = store.save(capture);
        if (error.isEmpty()) error = store.cleanup(retention, maximum);
        return error;
    }));
}

void MainWindow::pasteRecord(const QString &id, bool plainText, bool asFile) {
    const int row = model_->rowForId(id);
    const auto *itemPointer = model_->itemAt(row);
    if (!itemPointer) return;
    const HistoryItem item = *itemPointer;
    if (asFile && item.kind == 2) return;
    const int generation = ++pasteGeneration_;
    HWND target = targetWindow_;
    const HWND targetFocus = targetFocusWindow_;
    if (!target) {
        const HWND foreground = GetForegroundWindow();
        if (foreground != reinterpret_cast<HWND>(winId())) target = foreground;
    }
    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<QPair<HistoryItem, QByteArray>>(this);
    connect(watcher, &QFutureWatcher<QPair<HistoryItem, QByteArray>>::finished, this,
            [this, watcher, target, targetFocus, plainText, asFile, id, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != pasteGeneration_) return;
        const HistoryItem item = result.first;
        const QByteArray content = result.second;
        if (content.isEmpty()) { showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed"))); return; }
        const ImageContent image = item.kind == 1 ? decodeImageContent(content) : ImageContent{};
        if (item.kind == 1 && image.bytes.isEmpty()) {
            showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed")));
            return;
        }

        auto *mime = new QMimeData();
        if (asFile) {
            QString suffix = QStringLiteral("txt");
            QByteArray fileBytes;
            if (item.kind == 0) fileBytes = decodeTextContent(content).text.toUtf8();
            else {
                const QString type = imageMimeType(image.bytes);
                suffix = type == QStringLiteral("image/jpeg") ? QStringLiteral("jpg") : type.mid(6);
                fileBytes = image.bytes;
            }
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
                const QImage decoded = readBoundedImage(image.bytes, {});
                if (decoded.isNull()) {
                    delete mime;
                    showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed")));
                    return;
                }
                // Windows 粘贴目标通常读取位图格式，原始 image/png 等 MIME 单独提供并不可靠。
                mime->setImageData(decoded);
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
        ownClipboardSequence_ = GetClipboardSequenceNumber();
        lastClipboardSequence_ = ownClipboardSequence_;
        const DWORD pasteSequence = ownClipboardSequence_;
        const bool keepPanel = isPinned_;
        if (!keepPanel) hidePanel();
        if (!target || !IsWindow(target)) {
            showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
            return;
        }
        // 已最大化的窗口不能用 SW_RESTORE，否则粘贴时会意外退出最大化。
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        SetForegroundWindow(target);
        auto *timer = new QTimer(this);
        timer->setInterval(10);
        auto attempts = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [this, timer, attempts, target, targetFocus, id, keepPanel,
                                              directTextPaste = item.kind == 0 && !asFile,
                                              generation = pasteGeneration_, pasteSequence] {
            if (generation != pasteGeneration_ || GetClipboardSequenceNumber() != pasteSequence) {
                timer->stop(); timer->deleteLater();
                return;
            }
            if (!IsWindow(target) || GetForegroundWindow() != target) {
                timer->stop(); timer->deleteLater();
                showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                return;
            }
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000 || GetAsyncKeyState(VK_MENU) & 0x8000
                || GetAsyncKeyState(VK_SHIFT) & 0x8000 || GetAsyncKeyState(VK_LWIN) & 0x8000
                || GetAsyncKeyState(VK_RWIN) & 0x8000) {
                if (++*attempts < 100) return;
                timer->stop(); timer->deleteLater();
                showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                return;
            }
            // 激活目标窗口后恢复保存的原生输入窗口。
            if (targetFocus) {
                if (!IsWindow(targetFocus) || (targetFocus != target && !IsChild(target, targetFocus))) {
                    timer->stop(); timer->deleteLater();
                    showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                    return;
                }
                const DWORD targetThread = GetWindowThreadProcessId(targetFocus, nullptr);
                const DWORD currentThread = GetCurrentThreadId();
                const bool attached = targetThread && targetThread != currentThread
                    && AttachThreadInput(currentThread, targetThread, TRUE);
                if (attached || targetThread == currentThread) SetFocus(targetFocus);
                if (attached) AttachThreadInput(currentThread, targetThread, FALSE);
                GUITHREADINFO info{};
                info.cbSize = sizeof(info);
                if (!GetGUIThreadInfo(targetThread, &info) || info.hwndFocus != targetFocus
                    || GetForegroundWindow() != target) {
                    timer->stop(); timer->deleteLater();
                    showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                    return;
                }
            }
            bool pasted = false;
            wchar_t controlClass[32]{};
            const bool pasteToNativeEdit = directTextPaste && targetFocus
                && GetClassNameW(targetFocus, controlClass, 32)
                && QString::fromWCharArray(controlClass).compare(QStringLiteral("Edit"), Qt::CaseInsensitive) == 0;
            if (pasteToNativeEdit) {
                // 原生 Edit 直接接收粘贴消息，避免目标程序将模拟按键转给其他区域。
                DWORD_PTR ignored = 0;
                pasted = SendMessageTimeoutW(targetFocus, WM_PASTE, 0, 0,
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored) != 0;
            } else {
                INPUT inputs[4]{};
                const WORD keys[4]{VK_CONTROL, 'V', 'V', VK_CONTROL};
                for (int i = 0; i < 4; ++i) {
                    inputs[i].type = INPUT_KEYBOARD;
                    inputs[i].ki.wVk = keys[i];
                    inputs[i].ki.dwFlags = (i >= 2) ? KEYEVENTF_KEYUP : 0;
                }
                pasted = SendInput(4, inputs, sizeof(INPUT)) == 4;
            }
            timer->stop(); timer->deleteLater();
            if (!pasted) {
                showStatus(AppLocalization::get(QStringLiteral("ContentRestoredManualPaste")));
                return;
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
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, item] {
        return qMakePair(item, HistoryStore(path).content(item.id));
    }));
}
