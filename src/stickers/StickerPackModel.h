#pragma once

#include "stickers/StickerPack.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>

// The picker's tab strip: one row per MSC2545 image pack this account can
// use. Presentation-safe roles only — the pack's raw account-data/state JSON
// never reaches QML.
//
// Rows are REPLACED wholesale by the manager on each snapshot. A pack refresh
// is a whole answer, not a diff: the three source events have no per-image
// change feed, so anything finer would be invented.
class StickerPackModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        AttributionRole,
        SourceRole,      // "user" | "room"
        RoomIdRole,
        StateKeyRole,
        EnabledGloballyRole,
        CanManageRole,
        ImageCountRole,
        StickerCountRole,
        EmoticonCountRole,
    };

    explicit StickerPackModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_packs.size(); }

    void reset(const QList<stickers::Pack> &packs);
    void clear();

    // Row lookup for the manager and for tests.
    const QList<stickers::Pack> &packs() const { return m_packs; }
    Q_INVOKABLE QVariantMap get(int row) const;
    // -1 when absent.
    int indexOfPack(const QString &id) const;

Q_SIGNALS:
    void countChanged();

private:
    QList<stickers::Pack> m_packs;
};
