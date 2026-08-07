#pragma once

#include <QConcatenateTablesProxyModel>
#include <QVariantMap>

class GifStoredModel;

// v0.6.6: read-only presentation merge of the local-starred store and the
// provider-favorites store for the picker's single "Favorites" tab (SPEC:
// local starred entries "appear in the GIF picker's Favorites alongside
// provider favorites"). Local-starred rows are listed FIRST, provider
// favorites follow — each group internally newest-first (both sources
// already prepend on insert). This is a grouped-by-kind merge, not a true
// chronological interleave across kinds: neither GifStoredModel persists a
// cross-kind-comparable timestamp (both only ever track recency via list
// position), and adding one purely to interleave two lists that are each
// already newest-first was not worth a new persisted field for this round.
//
// QConcatenateTablesProxyModel (Qt/QtCore) already does the hard part
// correctly — row-offset bookkeeping across inserts/removes/moves/resets
// from either source. Its data(index, role) forwards the numeric `role` AS
// GIVEN to whichever source row the proxy index maps to, with NO per-source
// remapping by name — that only produces the right value here because BOTH
// sources return the IDENTICAL GifResultModel::roleNames() table (same
// GifResultModel::Roles enum, same int -> name pairing), so role number N
// means the same field (e.g. GifIdRole) in either source. Two sources with
// merely similarly-named-but-differently-numbered roles would silently
// cross-wire fields; that risk does not exist here because GifStoredModel
// (the base both GifStarredModel and GifFavoritesModel derive from) always
// answers roleNames() with GifResultModel's own table, never its own.
//
// This subclass adds only the small slice of GifStoredModel's QML-facing
// contract GifPicker.qml actually needs (count/get/row-kind) so a merged
// row can be captured as a plain snapshot exactly like a single model's row
// — GifPicker.qml's send path (choose(), which calls
// activeModel.get(resultOrRow)) needed no changes, and the star-button path
// resolves a tile's OWN captured snapshot (see tile.snapshot() in
// GifPicker.qml) rather than re-deriving anything from this model by index.
class GifFavoritesMergedModel : public QConcatenateTablesProxyModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Neither pointer is owned; both must outlive this model. In practice
    // both are long-lived controller-owned singletons for the app's whole
    // life (GifSearchController's own m_starred/m_favorites), exactly like
    // GifSearchController already assumes for m_favorites/m_recent.
    GifFavoritesMergedModel(GifStoredModel *local, GifStoredModel *provider,
                            QObject *parent = nullptr);

    int count() const;
    Q_INVOKABLE QVariantMap get(int row) const;
    // True when `row` belongs to the local-starred group — GifPicker.qml
    // uses this to route the tile star-button click to unstar() instead of
    // the provider toggleFavorite() path.
    Q_INVOKABLE bool isLocalRow(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    GifStoredModel *m_local;
    GifStoredModel *m_provider;
};
