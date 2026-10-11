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
    if (index.data(HistoryModel::PinnedRole).toBool()) fill = dark ? QColor("#284539") : QColor("#E5F3E8");
    else if (option.state.testFlag(QStyle::State_MouseOver)) fill = dark ? QColor("#393939") : QColor("#F5F5F5");
    QColor border = palette.color(QPalette::Mid);
    if (option.state.testFlag(QStyle::State_Selected)) border = palette.color(QPalette::Highlight);
    painter->setBrush(fill);
    painter->setPen(QPen(border, option.state.testFlag(QStyle::State_Selected) ? 1.4 : 0.7));
    // 半像素对齐细边框，圆弧与直边平滑相切。
    painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 11, 11);

    // 元数据与操作按钮共用右侧一列，把来源应用占用的宽度还给内容。
    const int rightX = actionRect(card, Preview).left();
    const QRect left(card.left() + 10, card.top() + 8, qMax(48, rightX - card.left() - 18), 56);
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

    QFont detailFont = option.font;
    detailFont.setPixelSize(12);
    QRect timeRect(rightX, card.top() + 42, 86, 14);
    const int quickPasteNumber = index.data(HistoryModel::QuickPasteRole).toInt();
    if (quickPasteNumber > 0) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        const QString number = QString::number(quickPasteNumber);
        const int badgeWidth = QFontMetrics(badgeFont).horizontalAdvance(number) + 10;
        // 时间与数字快捷键并排放在按钮下方，保持卡片高度不变。
        const QRect badge(timeRect.right() - badgeWidth + 1, timeRect.top(), badgeWidth, timeRect.height());
        timeRect.setWidth(timeRect.width() - badgeWidth - 4);
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->setPen(QPen(palette.color(QPalette::Mid), 0.7));
        painter->drawRoundedRect(badge, 3, 3);
        painter->setFont(badgeFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(badge, Qt::AlignCenter, number);
    }
    painter->setFont(detailFont);
    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter,
                      QDateTime::fromMSecsSinceEpoch(index.data(HistoryModel::UpdatedRole).toLongLong())
                          .toLocalTime().toString(QStringLiteral("HH:mm")));
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
            : action == Pin ? QChar(index.data(HistoryModel::PinnedRole).toBool() ? 0xE77A : 0xE718)
                            : QChar(0xE712);
        painter->drawText(rect, Qt::AlignCenter, glyph);
    }

    painter->restore();
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
