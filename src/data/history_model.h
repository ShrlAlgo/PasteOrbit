#pragma once

#include "history_store.h"

#include <QAbstractListModel>
#include <QAtomicInteger>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QSharedPointer>
#include <QSet>

struct ThumbnailResult {
    QString id;
    QByteArray bytes;
};

class HistoryModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { IdRole = Qt::UserRole + 1, KindRole, PreviewRole, SourceRole, UpdatedRole,
                PinnedRole, SizeRole, TextLengthRole, ThumbnailRole, ExpandedRole,
                PreviewImageRole, PreviewLoadingRole, PreviewZoomRole, PreviewPanRole,
                QuickPasteRole, FormatRole };

    explicit HistoryModel(QString databasePath, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setFirstPage(const HistoryPage &page);
    void appendPage(const HistoryPage &page);
    void trimToFirstPage();
    const HistoryItem *itemAt(int row) const;
    int rowForId(const QString &id) const;
    int quickPasteRow(int number) const;
    void setQuickPasteEnabled(bool enabled);
    void setPinned(const QString &id, bool pinned);
    void remove(const QString &id);
    void setVisibleRows(int first, int last);
    void releaseThumbnails();
    QString previewId() const { return previewId_; }
    void beginPreview(const QString &id);
    void finishTextPreview(const QString &id);
    void setFormatLabel(const QString &id, const QString &label);
    void setPreviewImage(const QString &id, const QImage &image);
    void setPreviewZoom(double zoom, const QPointF &pan);
    void setPreviewPan(const QPointF &pan);
    void clearPreview();

private:
    QString databasePath_;
    QVector<HistoryItem> items_;
    QHash<QString, QImage> thumbnails_;
    QHash<QString, QString> formatLabels_;
    QSet<QString> visibleIds_;
    QSet<QString> loadingIds_;
    QSharedPointer<QAtomicInteger<int>> thumbnailGeneration_ = QSharedPointer<QAtomicInteger<int>>::create(0);
    QString previewId_;
    QImage previewImage_;
    QPointF previewPan_;
    double previewZoom_ = 1.0;
    bool previewLoading_ = false;
    bool quickPasteEnabled_ = true;
};
