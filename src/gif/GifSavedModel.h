#pragma once

#include <QConcatenateTablesProxyModel>
#include <QVariantMap>

class GifStoredModel;

// The single "Saved" collection behind the picker's Saved tab: provider
// favorites and locally saved chat images in one list, one star, one verb.
//
// A presentation merge only. The stores stay separate because their security
// properties differ:
//   - provider favorites hold a provider id and public CDN URLs, nothing else;
//   - locally saved images are bytes on disk, the bounded, account-scoped,
//     disclosed exception to CLAUDE.md §6 (see GifStarredStore), deleted on
//     sign-out and account removal.
// GifPicker.qml badges the on-disk rows so the distinction stays visible.
//
// Local rows come first, then provider favorites, each newest-first. Neither
// store records a timestamp, so there is no chronological interleave.
//
// The same GIF may appear twice (a favorite and a local copy). Local rows
// record no provenance, so this cannot be detected, and the rows really are
// different things (a link and a copy).
//
// QConcatenateTablesProxyModel forwards numeric roles unmapped, which is
// correct only because both sources (GifStoredModel subclasses) answer
// GifResultModel's identical roleNames() table.
//
// Adds count/get so the picker can snapshot a merged row like any other
// model's. The star path routes on the snapshot's `provider` ("local" or a
// provider id), never on a row index.
class GifSavedModel : public QConcatenateTablesProxyModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Neither source is owned; both are controller-owned and outlive this
    // model. Neither is kept as a member: everything goes through the proxy's
    // mapping.
    GifSavedModel(GifStoredModel *local, GifStoredModel *provider,
                  QObject *parent = nullptr);

    // The shared role table, stated explicitly. QConcatenateTablesProxyModel
    // does not forward its sources' role names on Qt 6.8.2 (the packaged
    // builds' Qt), though it does on 6.11. The picker's delegate resolves
    // required properties by name, and QQmlDelegateModel refuses to build a
    // delegate it cannot satisfy, so the tab rendered blank while count() was
    // correct. Answering GifResultModel's table is correct on both versions and
    // is the invariant the role forwarding already depends on;
    // GifCollectionsTest and GifSavedTabQmlTest pin it.
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    // Row -> the same QVariantMap the single models return; empty when out of
    // range.
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();
};
