#include "gif/GifSavedModel.h"

#include "gif/GifResultModel.h"
#include "gif/GifStoredModel.h"

GifSavedModel::GifSavedModel(GifStoredModel *local, GifStoredModel *provider,
                             QObject *parent)
    : QConcatenateTablesProxyModel(parent)
{
    addSourceModel(local);
    addSourceModel(provider);
    connect(this, &QAbstractItemModel::rowsInserted,
            this, &GifSavedModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,
            this, &GifSavedModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset,
            this, &GifSavedModel::countChanged);
}

QHash<int, QByteArray> GifSavedModel::roleNames() const
{
    // The one table both sources answer with (see the header for the Qt version
    // difference). Taken from GifResultModel itself so it is right before any
    // source is added.
    return GifResultModel().roleNames();
}

int GifSavedModel::count() const
{
    return rowCount();
}

QVariantMap GifSavedModel::get(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    const QModelIndex sourceIndex = mapToSource(index(row, 0));
    if (!sourceIndex.isValid())
        return {};
    const auto *source =
        qobject_cast<const GifStoredModel *>(sourceIndex.model());
    if (!source)
        return {};
    return source->get(sourceIndex.row());
}
