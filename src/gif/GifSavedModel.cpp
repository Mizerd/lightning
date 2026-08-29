#include "gif/GifSavedModel.h"

#include "gif/GifResultModel.h"
#include "gif/GifStoredModel.h"

GifSavedModel::GifSavedModel(GifStoredModel *local, GifStoredModel *provider,
                             QObject *parent)
    : QConcatenateTablesProxyModel(parent)
{
    // Added in this order — QConcatenateTablesProxyModel lists `local`'s rows
    // first, then `provider`'s. get() resolves through the proxy's own
    // mapping rather than re-deriving that order, so this call order is the
    // single place the grouping is decided.
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
    // The ONE table both sources answer with — see the header for the Qt
    // version difference this exists to close. Taken from GifResultModel
    // itself rather than from a source model, so it is right even before a
    // source has been added and cannot drift toward one source's idea of it.
    return GifResultModel().roleNames();
}

int GifSavedModel::count() const
{
    return rowCount();
}

QVariantMap GifSavedModel::get(int row) const
{
    // v0.6.7 review (N2): resolved through the proxy's OWN mapping rather
    // than by re-deriving the group boundary from m_local->rowCount(). Both
    // give the same answer today, but the hand-rolled arithmetic was a second,
    // independent copy of the concatenation rule that addSourceModel()/
    // removeSourceModel() — public on the base class — could silently
    // invalidate. This version cannot disagree with count(), which already
    // reads the proxy's own row count.
    //
    // An out-of-range row answers an EMPTY map, never a neighbouring row:
    // GifPicker.qml's choose() drops a result with no provider/gifId, so an
    // empty map is what makes a stale keyboard row send nothing rather than
    // send the wrong GIF.
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
