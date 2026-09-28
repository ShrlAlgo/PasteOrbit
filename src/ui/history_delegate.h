#pragma once

#include "history_model.h"

#include <QStyledItemDelegate>

class HistoryDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum Action { Preview = 0, Pin = 1, More = 2 };
    explicit HistoryDelegate(QObject *parent = nullptr);
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
    int actionAt(const QRect &itemRect, const QPoint &position) const;
    QRect previewRect(const QRect &itemRect) const;

signals:
    void actionTriggered(const QString &id, int action);
};
