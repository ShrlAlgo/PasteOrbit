#include "history_delegate.h"

#include "localization.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTextLayout>

namespace {
QRect cardRect(const QRect &row) {
    // Qlementine 的滚动条覆盖视口，卡片仅让出实际滚动条宽度。
    constexpr int gutter = 10;
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

QString metadata(const QModelIndex &index) {
    const int kind = index.data(HistoryModel::KindRole).toInt();
    if (kind == 0) return AppLocalization::format(QStringLiteral("CharacterCount"),
                                                   {QString::number(index.data(HistoryModel::TextLengthRole).toInt())});
    if (kind == 1) {
        const auto size = index.data(HistoryModel::SizeRole).toLongLong();
        return QStringLiteral("%1 KB").arg(qMax(1.0, size / 1024.0), 0, 'f', 1);
    }
    return AppLocalization::get(QStringLiteral("ContentTypeFiles"));
}
}

HistoryDelegate::HistoryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize HistoryDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    return QSize(option.rect.width(), index.data(HistoryModel::ExpandedRole).toBool() ? 260 : 76);
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
    if (index.data(HistoryModel::PinnedRole).toBool()) fill = dark ? QColor("#284539") : QColor("#E5F3E8");
    else if (option.state.testFlag(QStyle::State_MouseOver)) fill = dark ? QColor("#393939") : QColor("#F5F5F5");
    QColor border = palette.color(QPalette::Mid);
    if (option.state.testFlag(QStyle::State_Selected)) border = palette.color(QPalette::Highlight);
    painter->setBrush(fill);
    painter->setPen(QPen(border, option.state.testFlag(QStyle::State_Selected) ? 1.4 : 0.7));
    // 半像素对齐细边框，圆弧与直边平滑相切。
    painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 11, 11);

    const int rightX = card.right() - 96 - 8 - 120;
    const QRect left(card.left() + 10, card.top() + 8, qMax(48, rightX - card.left() - 14), 56);
    const int kind = index.data(HistoryModel::KindRole).toInt();
    const QImage image = qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole));
    if (kind == 1 && !image.isNull()) {
        const QSize fitted = image.size().scaled(QSize(qMin(300, left.width()), 56), Qt::KeepAspectRatio);
        const QRect target(QPoint(left.left(), left.top() + (56 - fitted.height()) / 2), fitted);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(target, image, image.rect());
    } else if (kind == 2) {
        QFont fileFont;
        fileFont.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
        fileFont.setPixelSize(24);
        painter->setFont(fileFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(QRect(left.left(), left.top(), 32, 50), Qt::AlignLeft | Qt::AlignVCenter, QChar(0xE8A5));
        const QRect textRect(left.left() + 34, left.top(), left.width() - 34, 56);
        painter->setFont(option.font);
        painter->setPen(palette.color(QPalette::Text));
        drawPreviewText(painter, textRect, index.data(HistoryModel::PreviewRole).toString());
    } else if (kind == 0) {
        painter->setFont(option.font);
        painter->setPen(palette.color(QPalette::Text));
        drawPreviewText(painter, left, index.data(HistoryModel::PreviewRole).toString());
    }

    const QRect details(rightX, card.top() + 8, 120, 66);
    painter->setFont(option.font);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(QRect(details.left(), details.top(), details.width(), 18),
                      Qt::AlignRight | Qt::AlignVCenter,
                      QFontMetrics(option.font).elidedText(index.data(HistoryModel::SourceRole).toString().isEmpty()
                          ? AppLocalization::get(QStringLiteral("UnknownApplication"))
                          : index.data(HistoryModel::SourceRole).toString(), Qt::ElideRight, details.width()));
    QFont detailFont = option.font;
    detailFont.setPixelSize(12);
    painter->setFont(detailFont);
    const QRect timeRect(details.left(), details.top() + 20, details.width(), 16);
    painter->drawText(timeRect,
                      Qt::AlignRight | Qt::AlignVCenter,
                      QDateTime::fromMSecsSinceEpoch(index.data(HistoryModel::UpdatedRole).toLongLong())
                          .toLocalTime().toString(QStringLiteral("HH:mm")));
    const QString formatLabel = index.data(HistoryModel::FormatRole).toString();
    if (!formatLabel.isEmpty()) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        const QFontMetrics badgeMetrics(badgeFont);
        const int width = qMin(badgeMetrics.horizontalAdvance(formatLabel) + 10, details.width() - 46);
        const QRect badge(timeRect.left(), timeRect.top(), width, timeRect.height());
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->setPen(QPen(palette.color(QPalette::Mid), 0.7));
        painter->drawRoundedRect(badge, 3, 3);
        painter->setFont(badgeFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(badge, Qt::AlignCenter, badgeMetrics.elidedText(formatLabel, Qt::ElideRight, width - 6));
    }
    const int quickPasteNumber = index.data(HistoryModel::QuickPasteRole).toInt();
    QRect metadataRect(details.left(), details.top() + 38, details.width(), 16);
    if (quickPasteNumber > 0) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        const QString number = QString::number(quickPasteNumber);
        const int badgeWidth = QFontMetrics(badgeFont).horizontalAdvance(number) + 10;
        // 数字快捷键与容量统计共用一行，不再占用卡片底部。
        const QRect badge(details.right() - badgeWidth + 1, metadataRect.top(), badgeWidth, metadataRect.height());
        metadataRect.setWidth(metadataRect.width() - badgeWidth - 4);
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->setPen(QPen(palette.color(QPalette::Mid), 0.7));
        painter->drawRoundedRect(badge, 3, 3);
        painter->setFont(badgeFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(badge, Qt::AlignCenter, number);
    }
    painter->setFont(detailFont);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(metadataRect, Qt::AlignRight | Qt::AlignVCenter,
                      QFontMetrics(detailFont).elidedText(metadata(index), Qt::ElideRight, metadataRect.width()));

    QFont iconFont;
    iconFont.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    iconFont.setPixelSize(14);
    painter->setFont(iconFont);
    const auto *view = qobject_cast<const QAbstractItemView *>(parent());
    const QPoint cursor = view ? view->viewport()->mapFromGlobal(QCursor::pos()) : QPoint(-1, -1);
    for (int action = 0; action < 3; ++action) {
        const auto rect = actionRect(card, action);
        const bool hovered = view && view->viewport()->rect().contains(cursor) && rect.contains(cursor);
        if (hovered) {
            const bool pressed = QApplication::mouseButtons().testFlag(Qt::LeftButton);
            QColor feedback = palette.color(QPalette::Text);
            feedback.setAlpha(pressed ? 55 : 25);
            painter->setPen(Qt::NoPen);
            painter->setBrush(feedback);
            painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }
        QColor iconColor = palette.color(QPalette::PlaceholderText);
        if (action == Pin && index.data(HistoryModel::PinnedRole).toBool()) iconColor = palette.color(QPalette::Highlight);
        else if (hovered) iconColor = palette.color(QPalette::Text);
        painter->setPen(iconColor);
        const QChar glyph = action == Preview ? QChar(0xE890)
            : action == Pin ? QChar(index.data(HistoryModel::PinnedRole).toBool() ? 0xE77A : 0xE718)
                            : QChar(0xE712);
        painter->drawText(rect, Qt::AlignCenter, glyph);
    }

    if (index.data(HistoryModel::ExpandedRole).toBool()) {
        const QRect area = previewRect(option.rect);
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->setPen(QPen(palette.color(QPalette::Mid), 0.7));
        painter->drawRoundedRect(area, 6, 6);
        const QRect content = area.adjusted(8, 6, -8, -6);
        if (index.data(HistoryModel::PreviewLoadingRole).toBool()) {
            painter->setPen(palette.color(QPalette::PlaceholderText));
            painter->drawText(content, Qt::AlignCenter, QStringLiteral("…"));
        } else if (kind == 1) {
            const QImage expandedImage = qvariant_cast<QImage>(index.data(HistoryModel::PreviewImageRole));
            if (expandedImage.isNull()) {
                painter->setPen(palette.color(QPalette::PlaceholderText));
                painter->drawText(content, Qt::AlignCenter, AppLocalization::get(QStringLiteral("ImagePreviewUnavailable")));
            } else {
                const double scale = content.width() / static_cast<double>(expandedImage.width())
                    * index.data(HistoryModel::PreviewZoomRole).toDouble();
                const QPointF pan = index.data(HistoryModel::PreviewPanRole).toPointF();
                const QSizeF size(expandedImage.width() * scale, expandedImage.height() * scale);
                const QRectF target(QPointF(content.topLeft()) + pan, size);
                painter->save();
                painter->setClipRect(content);
                painter->drawImage(target, expandedImage, expandedImage.rect());
                painter->restore();
            }
        }
    }
    painter->restore();
}

int HistoryDelegate::actionAt(const QRect &itemRect, const QPoint &position) const {
    const auto card = cardRect(itemRect);
    for (int action = 0; action < 3; ++action) if (actionRect(card, action).contains(position)) return action;
    return -1;
}

QRect HistoryDelegate::previewRect(const QRect &itemRect) const {
    return cardRect(itemRect).adjusted(10, 70, -10, -10);
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
