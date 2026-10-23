#include "main_window.h"
#include "history_delegate.h"
#include "history_model.h"
#include "localization.h"
#include "clipboard_content.h"
#include "text_formats.h"
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QSaveFile>
#include <QMimeDatabase>
#include <QStringDecoder>
#include <QFrame>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QShortcut>
#include <QStackedLayout>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace ClipboardContent;

namespace {
constexpr int OriginalForegroundProperty = QTextFormat::UserProperty + 1;

void adaptPreviewContrast(QTextDocument *document, const QColor &background, const QColor &defaultText) {
    const auto luminance = [](const QColor &color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
    };
    struct Span { int position; int length; QColor color; QBrush original; };
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
            if (colors[key] != ink) changes.append({fragment.position(), fragment.length(), colors[key], format.foreground()});
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
        format.setProperty(OriginalForegroundProperty, span.original);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

QString editedHtml(const QTextDocument *document) {
    // 对比度适配仅用于显示，输出时在临时文档中恢复原文字色，不污染粘贴样式。
    std::unique_ptr<QTextDocument> output(document->clone());
    struct Span { int position; int length; QTextCharFormat format; };
    QList<Span> changes;
    for (auto block = output->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            auto format = fragment.charFormat();
            if (!format.hasProperty(OriginalForegroundProperty)) continue;
            const auto original = qvariant_cast<QBrush>(format.property(OriginalForegroundProperty));
            if (original.style() == Qt::NoBrush) format.clearProperty(QTextFormat::ForegroundBrush);
            else format.setForeground(original);
            format.clearProperty(OriginalForegroundProperty);
            changes.append({fragment.position(), fragment.length(), format});
        }
    }
    QTextCursor cursor(output.get());
    for (const auto &span : changes) {
        cursor.setPosition(span.position);
        cursor.setPosition(span.position + span.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(span.format);
    }
    return output->toHtml();
}

struct PreviewResult {
    QString text;
    QString html;
    bool markdown = false;
    bool sourceMarkdown = false;
    TextTable table;
    QString error;
    QString unavailablePath;
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


}

void MainWindow::closeHoverPreview() {
    hoverPreviewGeneration_->fetchAndAddRelaxed(1);
    hoverPreviewId_.clear();
    hoverImageScroll_ = nullptr;
    hoverImageLabel_ = nullptr;
    hoverImageSize_ = {};
    hoverImageZoom_ = 1.0;
    hoverImageDragging_ = false;
    delete hoverPreview_;
    hoverPreview_ = nullptr;
    if (delegate_) delegate_->setActivePreviewId({});
}

void MainWindow::toggleHoverPreview(QString id) {
    // 按值保存 ID；关闭旧浮层会清空待预览成员，引用传参会导致首次加载空记录。
    if (loadingPage_ || id.isEmpty()) return;
    // 点击和快捷键共用状态：同一卡片关闭预览，另一卡片切换预览。
    if (hoverPreview_ && hoverPreviewId_ == id) {
        closeHoverPreview();
        return;
    }
    const int row = model_->rowForId(id);
    const auto *item = model_->itemAt(row);
    if (!item) return;
    const int kind = item->kind;
    const int generation = hoverPreviewGeneration_->fetchAndAddRelaxed(1) + 1;
    QToolTip::hideText();
    hoverPreviewId_ = id;

    // 复用顶层窗口，新页面准备好之前保留旧内容，避免先关闭再打开导致闪烁。
    auto *popup = hoverPreview_;
    if (!popup) {
        popup = new HoverPreviewFrame(this, Qt::Tool | Qt::FramelessWindowHint);
        popup->installEventFilter(this);
        auto *close = new QShortcut(QKeySequence(Qt::Key_Escape), popup);
        connect(close, &QShortcut::activated, this, [this] {
            closeHoverPreview();
            if (isVisible() && !passiveBrowsing_) activatePanel(historyList_);
        });
        auto *pages = new QStackedLayout(popup);
        pages->setContentsMargins(0, 0, 0, 0);
    }
    popup->setObjectName(QStringLiteral("HoverPreview"));
    popup->setWindowTitle(AppLocalization::get(QStringLiteral("PreviewContent")));
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->setFocusPolicy(Qt::NoFocus);
    popup->setPalette(qApp->palette());
    const bool dark = popup->palette().color(QPalette::Window).lightness() < 128;
    const QColor previewBackground(dark ? QStringLiteral("#272B30") : QStringLiteral("#EBEEF2"));
    const QColor previewText(dark ? QStringLiteral("#DCE0E5") : QStringLiteral("#252A30"));
    const QColor previewBorder(dark ? QStringLiteral("#454B52") : QStringLiteral("#CCD2DA"));
    const QString previewStyle = QStringLiteral(
        "QScrollArea{background:transparent;border:0;}"
        "QLabel{background:transparent;color:%1;border:0;}"
        "QLabel[previewBadge=\"true\"]{background:%2;border:1px solid %3;border-radius:5px;padding:3px 7px;}"
        "QFrame#HoverPreviewContent{background:%4;border:1px solid %5;border-radius:7px;}"
    ).arg(popup->palette().color(QPalette::ToolTipText).name(),
          popup->palette().color(QPalette::AlternateBase).name(),
          popup->palette().color(QPalette::Mid).name(),
          previewBackground.name(), previewBorder.name());
    if (popup->styleSheet() != previewStyle) popup->setStyleSheet(previewStyle);
    auto *page = new QWidget(popup);
    page->hide();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    // 来源应用、格式与容量统计放在浮层页头，不再占用卡片内容宽度。
    const QString source = item->sourceApplication.isEmpty()
        ? AppLocalization::get(QStringLiteral("UnknownApplication")) : item->sourceApplication;
    const QString format = kind == 0 ? AppLocalization::get(QStringLiteral("FilterTextButtonTooltip")) : QString{};
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
    auto *formatBadge = makeBadge(format);
    heading->addWidget(formatBadge);
    formatBadge->setVisible(!format.isEmpty());
    heading->addStretch();
    heading->addWidget(statisticsBadge);
    layout->addLayout(heading);
    // 内容背景独立于页头标签，保持文字与图片预览的内边距一致。
    auto *content = new QFrame(popup);
    content->setObjectName(QStringLiteral("HoverPreviewContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(content, 1);
    hoverPreview_ = popup;
    if (passiveBrowsing_) setPassiveBrowsing(true);
    delegate_->setActivePreviewId(id);

    const QString path = databasePath_;
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this, [this, watcher, generation, id, page = QPointer<QWidget>(page),
                                                                 content, previewBackground, previewText,
                                                                 sourceBadge, statisticsBadge, formatBadge, heading, source] {
        const PreviewResult result = watcher->result();
        watcher->deleteLater();
        if (generation != hoverPreviewGeneration_->loadRelaxed() || id != hoverPreviewId_ || !hoverPreview_) {
            if (page) page->deleteLater();
            return;
        }
        hoverPreview_->setUpdatesEnabled(false);
        hoverImageScroll_ = nullptr;
        hoverImageLabel_ = nullptr;
        hoverImageSize_ = {};
        hoverImageZoom_ = 1.0;
        hoverImageDragging_ = false;
        auto *pages = static_cast<QStackedLayout *>(hoverPreview_->layout());
        while (auto *old = pages->takeAt(0)) {
            delete old->widget();
            delete old;
        }
        pages->addWidget(page);
        // 文件读取完成后按当前面板位置和实际内容定位，图片文件同样使用图片预览尺寸。
        const QPoint anchor = historyList_->viewport()->mapToGlobal(QPoint(0, 0));
        QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QRect available = screen->availableGeometry();
        if (available.width() > 20 && available.height() > 20) available.adjust(10, 10, -10, -10);
        const bool image = result.kind == 1 || !result.image.isNull();
        const QSize size = QSize(image ? 520 : 460, image ? 400 : 320).boundedTo(available.size());
        const QRect panel = frameGeometry();
        const QPoint candidates[]{QPoint(panel.right() + 9, anchor.y()),
            QPoint(panel.left() - size.width() - 8, anchor.y()),
            QPoint(panel.left(), panel.bottom() + 9), QPoint(panel.left(), panel.top() - size.height() - 8)};
        QPoint position;
        qint64 leastOverlap = std::numeric_limits<qint64>::max();
        for (const auto &candidate : candidates) {
            const QPoint clamped(qBound(available.left(), candidate.x(), available.right() - size.width() + 1),
                                 qBound(available.top(), candidate.y(), available.bottom() - size.height() + 1));
            const QRect overlap = QRect(clamped, size).intersected(panel);
            const qint64 area = qint64(overlap.width()) * overlap.height();
            if (area < leastOverlap) { leastOverlap = area; position = clamped; }
            if (area == 0) break;
        }
        hoverPreview_->setFixedSize(size);
        hoverPreview_->move(position);
        if (!result.formatLabel.isEmpty()) {
            formatBadge->setText(result.formatLabel);
            formatBadge->show();
        }
        {
            const int sourceWidth = qMax(0, hoverPreview_->width() - 20 - statisticsBadge->sizeHint().width()
                - formatBadge->sizeHint().width() - heading->spacing() * 3);
            sourceBadge->setText(sourceBadge->fontMetrics().elidedText(source, Qt::ElideRight, sourceWidth));
        }
        auto *layout = content->layout();
        if (!result.error.isEmpty()) {
            auto *label = new QLabel(result.unavailablePath.isEmpty() ? AppLocalization::get(result.error)
                : AppLocalization::format(result.error, {result.unavailablePath}), content);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            label->setAlignment(Qt::AlignCenter);
            layout->addWidget(label);
        } else if (result.kind == 1 || !result.image.isNull()) {
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
            preview->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(preview, &QTextEdit::customContextMenuRequested, this, [this, preview](const QPoint &position) {
                auto *menu = preview->createStandardContextMenu();
                preparePanelMenu(menu);
                connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
                menu->popup(preview->viewport()->mapToGlobal(position));
            });
            preview->setLineWrapMode(QTextEdit::WidgetWidth);
            preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            preview->verticalScrollBar()->setFixedWidth(historyList_->verticalScrollBar()->width());
            preview->horizontalScrollBar()->setFixedHeight(historyList_->verticalScrollBar()->width());
            const bool richText = !result.html.isEmpty() || result.markdown;
            preview->setStyleSheet(QStringLiteral("QTextEdit{background:transparent;border:0;color:%1;}")
                .arg(previewText.name()));
            preview->viewport()->setAutoFillBackground(false);
            // Markdown 正文优先，避免编辑器附带的 HTML 覆盖源码和渲染方式。
            if (result.markdown) preview->setMarkdown(result.text);
            else if (!result.html.isEmpty()) preview->setHtml(result.html);
            else if (!result.table.isEmpty()) preview->setHtml(tablePreviewHtml(result.table));
            else preview->setPlainText(result.text);
            if (richText || !result.table.isEmpty()) adaptPreviewContrast(preview->document(), previewBackground, previewText);
            layout->addWidget(preview);
            QPushButton *exportTable = nullptr;
            if (!result.table.isEmpty()) {
                exportTable = new QPushButton(AppLocalization::get(QStringLiteral("ExportTable")), content);
                layout->addWidget(exportTable);
                connect(exportTable, &QPushButton::clicked, this, [this, table = result.table] {
                    // 模态对话框仍会处理刷新事件，预览销毁后不能再读取连接闭包中的数据。
                    const TextTable exportData = table;
                    const QPointer<MainWindow> window(this);
                    activatePanel();
                    QFileDialog picker(this, AppLocalization::get(QStringLiteral("ExportTable")));
                    picker.setAcceptMode(QFileDialog::AcceptSave);
                    picker.setNameFilters({QStringLiteral("CSV (*.csv)"), QStringLiteral("TSV (*.tsv)")});
                    picker.setDefaultSuffix(QStringLiteral("csv"));
                    connect(&picker, &QFileDialog::filterSelected, &picker, [&picker](const QString &filter) {
                        picker.setDefaultSuffix(filter.startsWith(QLatin1String("TSV")) ? QStringLiteral("tsv") : QStringLiteral("csv"));
                    });
                    if (picker.exec() != QDialog::Accepted || !window || picker.selectedFiles().isEmpty()) return;
                    const QString destination = picker.selectedFiles().first();
                    const auto suffix = QFileInfo(destination).suffix();
                    const bool tsv = suffix.compare(QLatin1String("tsv"), Qt::CaseInsensitive) == 0;
                    if (!tsv && suffix.compare(QLatin1String("csv"), Qt::CaseInsensitive) != 0) {
                        window->showStatus(AppLocalization::get(QStringLiteral("TableExportExtension")));
                        return;
                    }
                    QSaveFile file(destination);
                    const auto bytes = delimitedTable(exportData, tsv ? QLatin1Char('\t') : QLatin1Char(','));
                    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                        window->showStatus(AppLocalization::get(QStringLiteral("TableExportFailed")) + QLatin1Char('\n') + file.errorString());
                    else window->showStatus(AppLocalization::get(QStringLiteral("TableExported")));
                });
            }
            if (result.kind == 0) {
                // 推测格式只影响展示，编辑和保存沿用原始格式。
                const bool markdown = result.sourceMarkdown;
                const bool html = !markdown && !result.html.isEmpty();
                const bool plainRtf = !markdown && !html && result.formatLabel == QStringLiteral("RTF");
                auto *actions = new QHBoxLayout;
                actions->addStretch();
                auto *edit = new QPushButton(AppLocalization::get(plainRtf
                    ? QStringLiteral("EditViewedPlainText") : QStringLiteral("EditViewedText")), content);
                edit->setObjectName(QStringLiteral("EditViewedText"));
                auto *paste = new QPushButton(AppLocalization::get(QStringLiteral("PasteEditedText")), content);
                paste->setObjectName(QStringLiteral("PasteEditedText"));
                auto *replace = new QPushButton(AppLocalization::get(QStringLiteral("ReplaceOriginalRecord")), content);
                replace->setObjectName(QStringLiteral("ReplaceOriginalRecord"));
                replace->setToolTip(AppLocalization::get(QStringLiteral("ReplaceOriginalRecordHint")));
                replace->hide();
                paste->hide();
                edit->setToolTip(AppLocalization::get(plainRtf ? QStringLiteral("RtfEditNotice") : QStringLiteral("EditViewedTextHint")));
                paste->setToolTip(edit->toolTip());
                auto *formatText = new QPushButton(AppLocalization::get(QStringLiteral("FormatStructuredText")), content);
                formatText->hide();
                actions->addWidget(formatText);
                connect(formatText, &QPushButton::clicked, this, [this, preview] {
                    if (preview->isReadOnly()) return;
                    const QString formatted = formattedStructuredText(preview->toPlainText());
                    if (formatted.isEmpty()) {
                        showStatus(AppLocalization::get(QStringLiteral("StructuredFormatUnavailable")));
                        return;
                    }
                    // 作为一次可撤销的编辑，只有用户选择替换时才写入历史。
                    auto cursor = preview->textCursor();
                    cursor.beginEditBlock();
                    cursor.select(QTextCursor::Document);
                    cursor.insertText(formatted);
                    cursor.endEditBlock();
                    preview->setTextCursor(cursor);
                });
                actions->addWidget(edit);
                actions->addWidget(replace);
                actions->addWidget(paste);
                static_cast<QVBoxLayout *>(layout)->addLayout(actions);
                const auto beginEdit = [this, generation, preview, edit, paste, replace, html, exportTable, formatText] {
                    if (generation != hoverPreviewGeneration_->loadRelaxed()) return;
                    activatePanel(preview);
                    preview->setAcceptRichText(false);
                    preview->setUndoRedoEnabled(true);
                    preview->document()->setModified(false);
                    preview->setReadOnly(false);
                    preview->setFocusPolicy(Qt::StrongFocus);
                    if (!html) {
                        preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                        preview->setLineWrapMode(QTextEdit::NoWrap);
                    }
                    edit->hide();
                    if (exportTable) exportTable->hide();
                    formatText->show();
                    paste->show();
                    replace->show();
                    replace->setEnabled(false);
                    preview->setFocus(Qt::MouseFocusReason);
                };
                connect(edit, &QPushButton::clicked, this,
                        [this, id, generation, renderedMarkdown = result.markdown || (!result.table.isEmpty() && result.html.isEmpty()), html,
                         sourceHtml = result.html, preview, edit, beginEdit] {
                    if (generation != hoverPreviewGeneration_->loadRelaxed()) return;
                    if (!renderedMarkdown) { beginEdit(); return; }
                    if (html) {
                        preview->setHtml(sourceHtml);
                        beginEdit();
                        return;
                    }
                    // Markdown 编辑源文本，按需重读，不让查看窗口额外常驻一份源码。
                    edit->setEnabled(false);
                    auto *watcher = new QFutureWatcher<QString>(this);
                    connect(watcher, &QFutureWatcher<QString>::finished, this,
                            [this, watcher, generation, guard = QPointer<QTextEdit>(preview), edit, beginEdit] {
                        const QString source = watcher->result();
                        watcher->deleteLater();
                        if (!guard || generation != hoverPreviewGeneration_->loadRelaxed()) return;
                        if (source.isEmpty()) {
                            edit->setEnabled(true);
                            showStatus(AppLocalization::get(QStringLiteral("ContentRestoreFailed")));
                            return;
                        }
                        guard->setPlainText(source);
                        beginEdit();
                    });
                    const QString path = databasePath_;
                    watcher->setFuture(QtConcurrent::run(&readPool_, [path, id] {
                        return decodeTextContent(HistoryStore(path).content(id)).text;
                    }));
                });
                connect(preview, &QTextEdit::textChanged, paste, [preview, paste, statisticsBadge] {
                    paste->setEnabled(!preview->document()->isEmpty());
                    statisticsBadge->setText(AppLocalization::format(QStringLiteral("CharacterCount"),
                        {QString::number(qMax(0, preview->document()->characterCount() - 1))}));
                });
                const auto updateReplace = [preview, replace] {
                    replace->setEnabled(!preview->document()->isEmpty() && preview->document()->isModified()
                        && !replace->property("saving").toBool());
                };
                connect(preview, &QTextEdit::textChanged, replace, updateReplace);
                connect(preview->document(), &QTextDocument::modificationChanged, replace, updateReplace);
                const auto editedContent = [preview, markdown, html] {
                    QJsonObject payload{{QStringLiteral("Text"), preview->toPlainText()}, {QStringLiteral("Markdown"), markdown}};
                    if (html) payload.insert(QStringLiteral("Html"), editedHtml(preview->document()));
                    return QByteArray(TextPayloadPrefix) + QJsonDocument(payload).toJson(QJsonDocument::Compact);
                };
                connect(paste, &QPushButton::clicked, this, [this, id, generation, editedContent] {
                    if (generation != hoverPreviewGeneration_->loadRelaxed()) return;
                    pasteRecord(id, false, false, editedContent());
                });
                connect(replace, &QPushButton::clicked, this, [this, id, generation, preview, replace, paste,
                                                             editedContent, formatBadge, markdown, html] {
                    if (generation != hoverPreviewGeneration_->loadRelaxed()) return;
                    const QString text = preview->toPlainText();
                    const QByteArray content = editedContent();
                    replace->setProperty("saving", true);
                    replace->setEnabled(false);
                    paste->setEnabled(false);
                    preview->setReadOnly(true);
                    const QString path = databasePath_;
                    auto *watcher = new QFutureWatcher<QString>(this);
                    connect(watcher, &QFutureWatcher<QString>::finished, this,
                            [this, watcher, id, generation, text, content, guard = QPointer<QTextEdit>(preview),
                             replace, paste, formatBadge, markdown, html] {
                        const QString error = watcher->result();
                        watcher->deleteLater();
                        // 存储成功后才更新卡片；切换或关闭查看窗口不影响已确认的保存。
                        if (error.isEmpty()) {
                            model_->setText(id, text, content.size(), markdown ? QStringLiteral("Markdown")
                                : html ? QStringLiteral("HTML") : QStringLiteral("PlainTextFormat"));
                            if (loadingPage_ || !searchBox_->text().isEmpty()) historyDirty_ = true;
                        }
                        if (guard && generation == hoverPreviewGeneration_->loadRelaxed()) {
                            guard->setReadOnly(false);
                            replace->setProperty("saving", false);
                            if (error.isEmpty()) {
                                guard->document()->setModified(false);
                                formatBadge->setText(markdown ? QStringLiteral("Markdown") : html ? QStringLiteral("HTML")
                                    : AppLocalization::get(QStringLiteral("FilterTextButtonTooltip")));
                            } else replace->setEnabled(true);
                            paste->setEnabled(!guard->document()->isEmpty());
                        }
                        // 保存成功且仍在当前查看窗口时才粘贴，防止异步保存后误操作已切换的目标。
                        if (!error.isEmpty()) showStatus(AppLocalization::get(error));
                        else if (guard && generation == hoverPreviewGeneration_->loadRelaxed() && isVisible())
                            pasteRecord(id, false, false, content);
                        else showStatus(AppLocalization::get(QStringLiteral("OriginalRecordReplaced")));
                    });
                    watcher->setFuture(QtConcurrent::run(&writePool_, [path, id, content, text] {
                        return HistoryStore(path).replaceText(id, content, text);
                    }));
                });
            }
        }
        // 内容与嵌套布局都准备好后再首次显示，避免加载占位符及首帧尺寸变化闪烁。
        hoverPreview_->layout()->activate();
        layout->activate();
        if (hoverImageScroll_) updateHoverPreviewImage();
        hoverPreview_->setUpdatesEnabled(true);
        if (!hoverPreview_->isVisible()) hoverPreview_->show();
    });
    const auto generationState = hoverPreviewGeneration_;
    watcher->setFuture(QtConcurrent::run(&readPool_, [path, id, kind, generationState, generation] {
        PreviewResult result;
        result.kind = kind;
        if (generationState->loadRelaxed() != generation) return result;
        const QByteArray content = HistoryStore(path).content(id, false);
        if (generationState->loadRelaxed() != generation) return result;
        if (kind == 1) {
            // 优先从原图按上限解码，预览图仅作为原图损坏时的回退。
            for (const bool preferPreview : {false, true}) {
                if (generationState->loadRelaxed() != generation) return result;
                const QByteArray bytes = decodeImageContent(preferPreview
                    ? HistoryStore(path).content(id, true) : content).bytes;
                if (generationState->loadRelaxed() != generation) return result;
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
            result.sourceMarkdown = text.markdown;
            if (!text.markdown) result.table = clipboardTable(text.text, text.html);
            result.markdown = result.table.isEmpty() && isMarkdownContent(text);
            result.formatLabel = textFormatLabel(text, result.markdown);
        } else {
            const auto paths = QJsonDocument::fromJson(content).array();
            QStringList values;
            values.reserve(paths.size());
            for (const auto &pathValue : paths) values.push_back(pathValue.toString());
            result.text = values.join(QLatin1Char('\n'));
            if (values.size() != 1) {
                result.unavailablePath = firstUnavailableFilePath(values);
                if (!result.unavailablePath.isEmpty()) result.error = QStringLiteral("FilePathUnavailable");
                return result;
            }
            QFile file(values.first());
            if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
                result.error = QStringLiteral("FilePathUnavailable");
                result.unavailablePath = values.first();
                return result;
            }
            if (generationState->loadRelaxed() != generation) return result;
            QImageReader reader(&file);
            if (reader.canRead()) {
                reader.setAutoTransform(true);
                const QSize target = boundedSize(reader.size(), 1600, 0, 4'000'000);
                if (target.isValid()) reader.setScaledSize(target);
                result.image = reader.read();
                if (result.image.isNull()) result.error = QStringLiteral("ImagePreviewUnavailable");
                return result;
            }
            const QString suffix = QFileInfo(file.fileName()).suffix().toLower();
            const auto mime = QMimeDatabase().mimeTypeForFile(file.fileName(), QMimeDatabase::MatchExtension);
            const QStringList textSuffixes{QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("svg"),
                QStringLiteral("md"), QStringLiteral("csv"), QStringLiteral("tsv"), QStringLiteral("log")};
            if (mime.inherits(QStringLiteral("text/plain")) || textSuffixes.contains(suffix)) {
                constexpr qint64 limit = 2 * 1024 * 1024;
                if (file.size() > limit) { result.error = QStringLiteral("TextFilePreviewTooLarge"); return result; }
                if (!file.seek(0)) { result.error = QStringLiteral("FilePreviewUnavailable"); return result; }
                const auto bytes = file.read(limit + 1);
                if (file.error() != QFileDevice::NoError) { result.error = QStringLiteral("FilePreviewUnavailable"); return result; }
                if (bytes.size() > limit) { result.error = QStringLiteral("TextFilePreviewTooLarge"); return result; }
                QStringDecoder decoder(QStringDecoder::encodingForData(bytes).value_or(QStringConverter::Utf8));
                result.text = decoder(bytes);
                if (decoder.hasError() || result.text.contains(QChar::Null)) {
                    result.error = QStringLiteral("TextFileEncodingUnsupported"); return result;
                }
                result.markdown = suffix == QLatin1String("md");
                if (!result.markdown) result.table = clipboardTable(result.text, {});
                result.formatLabel = suffix.toUpper();
            } else result.error = QStringLiteral("FilePreviewUnsupported");
        }
        return result;
    }));
}

void MainWindow::updateHoverPreviewImage() {
    if (!hoverPreview_ || !hoverImageLabel_ || !hoverImageScroll_ || hoverImageSize_.isEmpty()) return;
    // 视口已扣除边框与可见滚动条；后续 Resize 事件会纠正首次布局前的临时尺寸。
    const int availableWidth = qMax(1, hoverImageScroll_->viewport()->width());
    const double fit = availableWidth / static_cast<double>(hoverImageSize_.width());
    hoverImageLabel_->resize(hoverImageSize_ * (fit * hoverImageZoom_));
}

