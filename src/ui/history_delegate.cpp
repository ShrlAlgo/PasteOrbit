#include "portable_icons.h"
#include "history_delegate.h"

#include "localization.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QFontInfo>
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>

namespace {
// 卡片只渲染文字，不访问富文本引用的文件、网络或内嵌图片。
class CardTextDocument final : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

QRect cardRect(const QRect &row) {
    // 滚动条已在视口外单独占位，卡片右侧仅保留小间距。
    constexpr int gutter = 4;
    return row.adjusted(0, 0, -gutter, -6);
}

QRect actionRect(const QRect &card, int action) {
    return QRect(card.right() - 96 + action * 29, card.top() + 10, 28, 28);
}

// 仅布局前三行；最后一行省略剩余内容，避免长文本挤占卡片元数据。
void drawPreviewText(QPainter *painter, const QRect &rect, const QString &text) {
    if (rect.width() <= 0 || rect.height() <= 0) return;
    QTextLayout layout(text, painter->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    const QFontMetrics metrics(painter->font());
    const int lines = qMin(3, qMax(1, rect.height() / metrics.lineSpacing()));
    layout.beginLayout();
    for (int i = 0; i < lines; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(rect.width());
        const QPointF position(rect.left(), rect.top() + i * metrics.lineSpacing());
        if (i == lines - 1 && line.textStart() + line.textLength() < text.size()) {
            painter->drawText(QRect(position.toPoint(), QSize(rect.width(), metrics.lineSpacing())),
                Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(text.mid(line.textStart()), Qt::ElideRight, rect.width()));
        } else {
            line.draw(painter, position);
        }
    }
    layout.endLayout();
}
}

HistoryDelegate::HistoryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void HistoryDelegate::setActivePreviewId(const QString &id) {
    if (activePreviewId_ == id) return;
    activePreviewId_ = id;
    // 只刷新预览按钮状态，保持卡片高度和布局不变。
    if (auto *view = qobject_cast<QAbstractItemView *>(parent())) view->viewport()->update();
}

QSize HistoryDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const {
    return QSize(option.rect.width(), 76);
}

void HistoryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect card = cardRect(option.rect);
    const QPalette palette = option.palette;
    // 列表窗口背景透明时，Window 色不代表卡片主题；以实际文字色判断明暗。
    const bool dark = palette.color(QPalette::Text).lightness() > 128;
    QColor fill = dark ? QColor("#303030") : QColor("#FFFFFF");
    if (option.state.testFlag(QStyle::State_MouseOver)) fill = dark ? QColor("#393939") : QColor("#F5F5F5");
    QColor border = palette.color(QPalette::Mid);
    if (option.state.testFlag(QStyle::State_Selected)) border = palette.color(QPalette::Highlight);
    painter->setBrush(fill);
    painter->setPen(QPen(border, option.state.testFlag(QStyle::State_Selected) ? 1.4 : 0.7));
    // 半像素对齐细边框，圆弧与直边平滑相切。
    painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 11, 11);
    const bool pinned = index.data(HistoryModel::PinnedRole).toBool();
    const int quickPasteNumber = index.data(HistoryModel::QuickPasteRole).toInt();
    if (pinned || quickPasteNumber > 0) {
        // 编号和收藏共用贴边角标，按卡片圆角裁剪，不额外绘制编号边框。
        QPainterPath outline;
        outline.addRoundedRect(QRectF(card).adjusted(1, 1, -1, -1), 10, 10);
        QPainterPath corner;
        corner.moveTo(card.left(), card.top());
        corner.lineTo(card.left() + 32, card.top());
        corner.lineTo(card.left(), card.top() + 32);
        corner.closeSubpath();
        painter->fillPath(corner.intersected(outline), pinned
            ? (dark ? QColor("#70BE96") : QColor("#339468"))
            : (dark ? QColor("#50545E") : QColor("#DCE1EA")));
        if (quickPasteNumber > 0) {
            QFont numberFont = option.font;
            numberFont.setPixelSize(10);
            painter->setFont(numberFont);
            painter->setPen(pinned ? (dark ? QColor("#143D2B") : QColor(Qt::white))
                                   : palette.color(QPalette::PlaceholderText));
            painter->drawText(QRect(card.left() + 3, card.top() + 1, 16, 18),
                Qt::AlignCenter, QString::number(quickPasteNumber));
        }
    }

    // 元数据与操作按钮共用右侧一列，把来源应用占用的宽度还给内容。
    const int kind = index.data(HistoryModel::KindRole).toInt();
    const QString formatKey = kind == 1 ? QStringLiteral("ImageFormat") : index.data(HistoryModel::FormatRole).toString();
    const QString format = (formatKey.endsWith(QStringLiteral("Format")))
        ? AppLocalization::get(formatKey) : formatKey;
    QFont formatFont = option.font;
    formatFont.setPixelSize(10);
    const int formatWidth = format.isEmpty() ? 0 : qMin(96, QFontMetrics(formatFont).horizontalAdvance(format) + 16);
    QFont detailFont = option.font;
    detailFont.setPixelSize(12);
    const QString time = QDateTime::fromMSecsSinceEpoch(index.data(HistoryModel::UpdatedRole).toLongLong())
                             .toLocalTime().toString(QStringLiteral("HH:mm"));
    const int timeWidth = QFontMetrics(detailFont).horizontalAdvance(time);
    // 整组元数据靠右，仅按实际文字与编号宽度给摘要让位。
    const int rightX = qMin(actionRect(card, Preview).left(),
                           card.right() - 9 - formatWidth - timeWidth + 1);
    const QString note = index.data(HistoryModel::NoteRole).toString();
    // 仅带角标的卡片避让左上角，其余卡片沿用原来的左边距。
    const bool hasCorner = pinned || quickPasteNumber > 0;
    const int contentInset = hasCorner ? 24 : 10;
    const int noteInset = hasCorner ? 32 : 10;
    if (!note.isEmpty()) {
        QFont noteFont = option.font;
        noteFont.setPixelSize(12);
        const QFontMetrics metrics(noteFont);
        const int availableWidth = qMax(0, rightX - card.left() - noteInset - 8);
        const QRect badge(card.left() + noteInset, card.top() + 1,
            qMin(availableWidth, metrics.horizontalAdvance(note) + 12), 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(dark ? QColor("#42354E") : QColor("#F1EAF8"));
        painter->drawRoundedRect(badge, 4, 4);
        painter->setFont(noteFont);
        painter->setPen(dark ? QColor("#D7BFED") : QColor("#694491"));
        painter->drawText(badge.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(note, Qt::ElideRight, qMax(0, badge.width() - 12)));
    }
    // 编号不再占据底部，正文恢复使用备注之外的可用空间。
    const int lines = note.isEmpty() ? 3 : 2;
    const int bodyTop = card.top() + (note.isEmpty() ? 8 : 22);
    const int bodyBottom = card.bottom() - 5;
    const int bodyHeight = qMax(0, qMin(lines * QFontMetrics(option.font).lineSpacing(), bodyBottom - bodyTop + 1));
    const QRect left(card.left() + contentInset, bodyTop, qMax(48, rightX - card.left() - contentInset - 8), bodyHeight);
    const QImage image = qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole));
    if (kind == 1 && !image.isNull()) {
        const QSize fitted = image.size().scaled(QSize(qMin(300, left.width()), left.height()), Qt::KeepAspectRatio);
        const QRect target(QPoint(left.left(), left.top() + (left.height() - fitted.height()) / 2), fitted);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(target, image, image.rect());
    } else if (kind == 2) {
        const int iconSize = qMin(24, left.height());
        const QRect iconRect(left.left() + 2, left.top() + (left.height() - iconSize) / 2, iconSize, iconSize);
        if (!image.isNull()) {
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawImage(iconRect, image);
        } else {
            QFont fileFont;
            fileFont.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
            fileFont.setPixelSize(qMax(1, iconSize));
            painter->setFont(fileFont);
            painter->setPen(palette.color(QPalette::PlaceholderText));
#ifdef Q_OS_WIN
            painter->drawText(iconRect, Qt::AlignLeft | Qt::AlignVCenter, QChar(0xE8A5));
#else
            drawPortableIcon(painter, QRectF(iconRect), QChar(0xE8A5),
                             palette.color(QPalette::PlaceholderText));
#endif
        }
        const QRect textRect(left.left() + 34, left.top(), left.width() - 34, left.height());
        const bool unavailable = index.data(HistoryModel::UnavailableRole).toBool();
        QFont fileTextFont = option.font;
        if (unavailable) fileTextFont.setStrikeOut(true);
        painter->setFont(fileTextFont);
        painter->setPen(unavailable ? QColor("#D13438") : palette.color(QPalette::Text));
        drawPreviewText(painter, textRect, index.data(HistoryModel::PreviewRole).toString());
    } else if (kind == 0) {
        painter->setFont(option.font);
        painter->setPen(palette.color(QPalette::Text));
        const QString source = index.data(HistoryModel::CardTextRole).toString();
        if (source.isEmpty()) {
            drawPreviewText(painter, left, index.data(HistoryModel::PreviewRole).toString());
        } else {
            const bool markdown = index.data(HistoryModel::CardMarkdownRole).toBool();
            const QColor textColor = palette.color(QPalette::Text);
            // 包含内容、宽度、字体和主题的有界缓存，编辑及缩放不会复用旧布局。
            const QString key = index.data(HistoryModel::IdRole).toString() + QLatin1Char('|')
                + QString::number(left.width()) + QLatin1Char('|') + option.font.toString()
                + textColor.name(QColor::HexArgb) + (markdown ? QLatin1Char('M') : QLatin1Char('H')) + source;
            auto *document = textDocuments_.object(key);
            if (!document) {
                document = new CardTextDocument;
                document->setUndoRedoEnabled(false);
                document->setDocumentMargin(0);
                document->setDefaultFont(option.font);
                if (markdown) document->setMarkdown(source);
                else document->setHtml(source);
                // 保留强调、列表和代码字体，统一字号及颜色以适配卡片和深浅主题。
                QTextCursor cursor(document);
                cursor.select(QTextCursor::Document);
                QTextCharFormat textFormat;
                textFormat.setForeground(textColor);
                textFormat.setBackground(Qt::transparent);
                textFormat.setFontPointSize(QFontInfo(option.font).pointSizeF());
                cursor.mergeCharFormat(textFormat);
                for (auto block = document->begin(); block.isValid(); block = block.next()) {
                    QTextCursor blockCursor(block);
                    auto format = block.blockFormat();
                    format.setTopMargin(0);
                    format.setBottomMargin(0);
                    format.clearBackground();
                    blockCursor.setBlockFormat(format);
                }
                // HTML 的背景也可能来自外层框架或表格，统一透出卡片底色。
                QList<QTextFrame *> frames{document->rootFrame()};
                while (!frames.isEmpty()) {
                    auto *frame = frames.takeLast();
                    frames.append(frame->childFrames());
                    auto format = frame->frameFormat();
                    format.clearBackground();
                    frame->setFrameFormat(format);
                    if (auto *table = qobject_cast<QTextTable *>(frame)) {
                        for (int row = 0; row < table->rows(); ++row) {
                            for (int column = 0; column < table->columns(); ++column) {
                                auto cell = table->cellAt(row, column);
                                auto cellFormat = cell.format();
                                cellFormat.clearBackground();
                                cell.setFormat(cellFormat);
                            }
                        }
                    }
                }
                cursor = QTextCursor(document);
                while (!(cursor = document->find(QString(QChar::ObjectReplacementCharacter), cursor)).isNull())
                    cursor.removeSelectedText();
                document->setTextWidth(left.width());
                textDocuments_.insert(key, document);
            }
            painter->save();
            painter->setClipRect(left, Qt::IntersectClip);
            painter->translate(left.topLeft());
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette = palette;
            context.clip = QRectF(0, 0, left.width(), left.height());
            document->documentLayout()->draw(painter, context);
            painter->restore();
        }
    }

    const int metaY = card.top() + 42;
    const QRect timeRect(card.right() - 9 - timeWidth + 1, metaY, timeWidth, 14);
    const QRect formatRect(timeRect.left() - formatWidth, metaY, qMax(0, formatWidth - 6), 14);
    const auto drawBadge = [&](const QRect &rect, const QString &text) {
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->setPen(QPen(palette.color(QPalette::Mid), 0.7));
        painter->drawRoundedRect(rect, 3, 3);
        painter->setFont(formatFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(rect, Qt::AlignCenter, QFontMetrics(formatFont).elidedText(text, Qt::ElideRight, qMax(0, rect.width() - 4)));
    };
    if (!format.isEmpty()) drawBadge(formatRect, format);
    painter->setFont(detailFont);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, time);
    QFont iconFont;
    iconFont.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    iconFont.setPixelSize(14);
    painter->setFont(iconFont);
    const auto *view = qobject_cast<const QAbstractItemView *>(parent());
    const QPoint cursor = view ? view->viewport()->mapFromGlobal(QCursor::pos()) : QPoint(-1, -1);
    for (int action = 0; action < 3; ++action) {
        const auto rect = actionRect(card, action);
        const bool hovered = view && view->viewport()->rect().contains(cursor) && rect.contains(cursor);
        const bool previewActive = action == Preview
            && index.data(HistoryModel::IdRole).toString() == activePreviewId_;
        if (hovered || previewActive) {
            const bool pressed = hovered && QApplication::mouseButtons().testFlag(Qt::LeftButton);
            QColor feedback = palette.color(previewActive ? QPalette::Highlight : QPalette::Text);
            feedback.setAlpha(pressed ? 55 : 25);
            painter->setPen(Qt::NoPen);
            painter->setBrush(feedback);
            painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }
        QColor iconColor = palette.color(QPalette::PlaceholderText);
        if (previewActive || (action == Pin && index.data(HistoryModel::PinnedRole).toBool()))
            iconColor = palette.color(QPalette::Highlight);
        else if (hovered) iconColor = palette.color(QPalette::Text);
        painter->setPen(iconColor);
        // 预览开启时显示隐藏图标，关闭时恢复查看图标。
        const QChar glyph = action == Preview ? QChar(previewActive ? 0xED1A : 0xE890)
            : action == Pin ? QChar(index.data(HistoryModel::PinnedRole).toBool() ? 0xE735 : 0xE734)
                            : QChar(0xE712);
#ifdef Q_OS_WIN
        painter->drawText(rect, Qt::AlignCenter, glyph);
#else
        drawPortableIcon(painter, QRectF(rect).adjusted(4, 4, -4, -4), glyph, iconColor);
#endif
    }

    painter->restore();
}

QRect HistoryDelegate::actionGeometry(const QRect &itemRect, Action action) const {
    // 菜单锚点复用绘制和命中测试的按钮边界，不另外维护一套坐标。
    return actionRect(cardRect(itemRect), action);
}

int HistoryDelegate::actionAt(const QRect &itemRect, const QPoint &position) const {
    const auto card = cardRect(itemRect);
    for (int action = 0; action < 3; ++action) if (actionRect(card, action).contains(position)) return action;
    return -1;
}

bool HistoryDelegate::editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) {
    if (event->type() != QEvent::MouseButtonRelease) return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    const int action = actionAt(option.rect, mouse->position().toPoint());
    if (action < 0) return false;
    emit actionTriggered(index.data(HistoryModel::IdRole).toString(), action);
    return true;
}
