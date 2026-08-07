#include "gif/GifFavoritesMergedModel.h"

#include "gif/GifStoredModel.h"

GifFavoritesMergedModel::GifFavoritesMergedModel(GifStoredModel *local,
                                                 GifStoredModel *provider,
                                                 QObject *parent)
    : QConcatenateTablesProxyModel(parent)
    , m_local(local)
    , m_provider(provider)
{
    // Added in this order — QConcatenateTablesProxyModel lists `local`'s
    // rows first, then `provider`'s — matching get()/isLocalRow() below.
    addSourceModel(local);
    addSourceModel(provider);
    connect(this, &QAbstractItemModel::rowsInserted,
            this, &GifFavoritesMergedModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,
            this, &GifFavoritesMergedModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset,
            this, &GifFavoritesMergedModel::countChanged);
}

int GifFavoritesMergedModel::count() const
{
    return rowCount();
}

bool GifFavoritesMergedModel::isLocalRow(int row) const
{
    return m_local && row >= 0 && row < m_local->rowCount();
}

QVariantMap GifFavoritesMergedModel::get(int row) const
{
    if (row < 0)
        return {};
    const int localCount = m_local ? m_local->rowCount() : 0;
    if (row < localCount)
        return m_local->get(row);
    return m_provider ? m_provider->get(row - localCount) : QVariantMap{};
}
