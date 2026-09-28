#include "history_model.h"

#include <QtConcurrent>

#include <utility>

HistoryModel::HistoryModel(QString databasePath, QObject *parent)
    : QAbstractListModel(parent), databasePath_(std::move(databasePath)) {}

int HistoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : items_.size();
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) return {};
    const auto &item = items_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case PreviewRole: return item.preview;
    case IdRole: return item.id;
    case KindRole: return item.kind;
    case SourceRole: return item.sourceApplication;
    case UpdatedRole: return item.updatedAt;
    case PinnedRole: return item.pinned;
    case SizeRole: return item.contentSize;
    case TextLengthRole: return item.searchTextLength;
    case ThumbnailRole: return thumbnails_.value(item.id);
    case ExpandedRole: return item.id == previewId_;
    case PreviewImageRole: return item.id == previewId_ ? QVariant::fromValue(previewImage_) : QVariant{};
    case PreviewLoadingRole: return item.id == previewId_ && previewLoading_;
    case PreviewZoomRole: return item.id == previewId_ ? previewZoom_ : 1.0;
    case PreviewPanRole: return item.id == previewId_ ? previewPan_ : QPointF{};
    case QuickPasteRole: {
        if (!quickPasteEnabled_ || item.pinned) return {};
        int number = 0;
        for (int row = 0; row <= index.row() && number < 10; ++row) {
            if (!items_.at(row).pinned) ++number;
        }
        return number <= 9 ? QVariant(number) : QVariant{};
    }
    case FormatRole: return formatLabels_.value(item.id);
    case Qt::SizeHintRole: return QSize(1, item.id == previewId_ ? 278 : 94);
    default: return {};
    }
}

void HistoryModel::setFirstPage(const HistoryPage &page) {
    thumbnailGeneration_->fetchAndAddRelaxed(1);
    beginResetModel();
    items_ = page.items;
    previewId_.clear();
    previewImage_ = {};
    previewPan_ = {};
    previewZoom_ = 1.0;
    previewLoading_ = false;
    thumbnails_.clear();
    formatLabels_.clear();
    visibleIds_.clear();
    loadingIds_.clear();
    endResetModel();
}

void HistoryModel::appendPage(const HistoryPage &page) {
    QSet<QString> known;
    known.reserve(items_.size());
    for (const auto &item : items_) known.insert(item.id);
    QVector<HistoryItem> additions;
    additions.reserve(page.items.size());
    for (const auto &item : page.items) if (!known.contains(item.id)) additions.push_back(item);
    if (additions.isEmpty()) return;
    const int first = items_.size();
    beginInsertRows({}, first, first + additions.size() - 1);
    for (auto &item : additions) items_.push_back(std::move(item));
    endInsertRows();
}

void HistoryModel::trimToFirstPage() {
    if (items_.size() <= 30) return;
    if (rowForId(previewId_) >= 30) clearPreview();
    thumbnailGeneration_->fetchAndAddRelaxed(1);
    beginRemoveRows({}, 30, items_.size() - 1);
    for (int row = 30; row < items_.size(); ++row) formatLabels_.remove(items_.at(row).id);
    items_.resize(30);
    thumbnails_.clear();
    visibleIds_.clear();
    loadingIds_.clear();
    endRemoveRows();
    items_.squeeze();
}

const HistoryItem *HistoryModel::itemAt(int row) const {
    return row >= 0 && row < items_.size() ? &items_.at(row) : nullptr;
}

int HistoryModel::rowForId(const QString &id) const {
    for (int i = 0; i < items_.size(); ++i) if (items_.at(i).id == id) return i;
    return -1;
}

int HistoryModel::quickPasteRow(int number) const {
    if (!quickPasteEnabled_ || number < 1 || number > 9) return -1;
    int current = 0;
    for (int row = 0; row < items_.size(); ++row) {
        if (!items_.at(row).pinned && ++current == number) return row;
    }
    return -1;
}

void HistoryModel::setQuickPasteEnabled(bool enabled) {
    if (quickPasteEnabled_ == enabled) return;
    quickPasteEnabled_ = enabled;
    if (!items_.isEmpty()) emit dataChanged(index(0), index(items_.size() - 1), {QuickPasteRole});
}

void HistoryModel::setPinned(const QString &id, bool pinned) {
    const int row = rowForId(id);
    if (row < 0 || items_[row].pinned == pinned) return;
    items_[row].pinned = pinned;
    emit dataChanged(index(row), index(row), {PinnedRole, QuickPasteRole});
    if (row + 1 < items_.size()) emit dataChanged(index(row + 1), index(items_.size() - 1), {QuickPasteRole});
}

void HistoryModel::remove(const QString &id) {
    const int row = rowForId(id);
    if (row < 0) return;
    if (previewId_ == id) clearPreview();
    beginRemoveRows({}, row, row);
    thumbnails_.remove(id);
    formatLabels_.remove(id);
    visibleIds_.remove(id);
    loadingIds_.remove(id);
    items_.removeAt(row);
    endRemoveRows();
}

void HistoryModel::beginPreview(const QString &id) {
    clearPreview();
    previewId_ = id;
    previewLoading_ = true;
    const int row = rowForId(id);
    if (row >= 0) emit dataChanged(index(row), index(row), {Qt::SizeHintRole, ExpandedRole, PreviewLoadingRole});
}

void HistoryModel::finishTextPreview(const QString &id) {
    if (previewId_ != id) return;
    previewLoading_ = false;
    const int row = rowForId(id);
    if (row >= 0) emit dataChanged(index(row), index(row), {PreviewLoadingRole});
}

void HistoryModel::setFormatLabel(const QString &id, const QString &label) {
    const int row = rowForId(id);
    if (row < 0) return;
    if (label.isEmpty()) formatLabels_.remove(id);
    else formatLabels_.insert(id, label);
    emit dataChanged(index(row), index(row), {Qt::SizeHintRole, FormatRole});
}

void HistoryModel::setPreviewImage(const QString &id, const QImage &image) {
    if (previewId_ != id) return;
    previewImage_ = image;
    previewLoading_ = false;
    const int row = rowForId(id);
    if (row >= 0) emit dataChanged(index(row), index(row), {PreviewImageRole, PreviewLoadingRole});
}

void HistoryModel::setPreviewZoom(double zoom, const QPointF &pan) {
    previewZoom_ = zoom;
    previewPan_ = pan;
    const int row = rowForId(previewId_);
    if (row >= 0) emit dataChanged(index(row), index(row), {PreviewZoomRole, PreviewPanRole});
}

void HistoryModel::setPreviewPan(const QPointF &pan) {
    previewPan_ = pan;
    const int row = rowForId(previewId_);
    if (row >= 0) emit dataChanged(index(row), index(row), {PreviewPanRole});
}

void HistoryModel::clearPreview() {
    const QString oldId = std::exchange(previewId_, {});
    previewImage_ = {};
    previewPan_ = {};
    previewZoom_ = 1.0;
    previewLoading_ = false;
    const int row = rowForId(oldId);
    if (row >= 0) emit dataChanged(index(row), index(row), {Qt::SizeHintRole, ExpandedRole,
                                                            PreviewImageRole,
                                                            PreviewLoadingRole, PreviewZoomRole, PreviewPanRole});
}

void HistoryModel::setVisibleRows(int first, int last) {
    QSet<QString> nowVisible;
    if (first >= 0 && last >= first) {
        for (int row = first; row <= last && row < items_.size(); ++row) {
            if (items_.at(row).kind == 1) nowVisible.insert(items_.at(row).id);
        }
    }
    for (const auto &id : std::as_const(visibleIds_)) {
        if (!nowVisible.contains(id)) {
            thumbnails_.remove(id);
            const int row = rowForId(id);
            if (row >= 0) emit dataChanged(index(row), index(row), {ThumbnailRole});
        }
    }
    if (nowVisible != visibleIds_) {
        thumbnailGeneration_->fetchAndAddRelaxed(1);
        loadingIds_.clear();
    }
    visibleIds_ = std::move(nowVisible);
    const int generation = thumbnailGeneration_->loadRelaxed();
    const auto generationState = thumbnailGeneration_;
    for (const auto &id : std::as_const(visibleIds_)) {
        if (thumbnails_.contains(id) || loadingIds_.contains(id)) continue;
        loadingIds_.insert(id);
        auto *watcher = new QFutureWatcher<ThumbnailResult>(this);
        connect(watcher, &QFutureWatcher<ThumbnailResult>::finished, this, [this, watcher, generationState, generation] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (generationState->loadRelaxed() != generation) return;
            loadingIds_.remove(result.id);
            if (!visibleIds_.contains(result.id) || result.bytes.isEmpty()) return;
            QImage image = QImage::fromData(result.bytes);
            if (!image.isNull()) {
                thumbnails_.insert(result.id, image.scaled(300, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                const int row = rowForId(result.id);
                if (row >= 0) emit dataChanged(index(row), index(row), {ThumbnailRole});
            }
        });
        const QString path = databasePath_;
        watcher->setFuture(QtConcurrent::run([path, id, generationState, generation] {
            if (generationState->loadRelaxed() != generation) return ThumbnailResult{id, {}};
            return ThumbnailResult{id, HistoryStore(path).thumbnail(id)};
        }));
    }
}

void HistoryModel::releaseThumbnails() {
    // 面板隐藏或开始恢复备份时，让尚未执行的缩略图任务直接退出。
    thumbnailGeneration_->fetchAndAddRelaxed(1);
    visibleIds_.clear();
    loadingIds_.clear();
    if (thumbnails_.isEmpty()) return;
    thumbnails_.clear();
    if (!items_.isEmpty()) emit dataChanged(index(0), index(items_.size() - 1), {ThumbnailRole});
}
