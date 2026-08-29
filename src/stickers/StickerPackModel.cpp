#include "stickers/StickerPackModel.h"

StickerPackModel::StickerPackModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int StickerPackModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_packs.size();
}

QVariant StickerPackModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_packs.size())
        return {};
    const stickers::Pack &pack = m_packs.at(index.row());
    switch (role) {
    case IdRole:            return pack.id;
    case DisplayNameRole:   return pack.displayName;
    case AvatarUrlRole:     return pack.avatarUrl;
    case AttributionRole:   return pack.attribution;
    case SourceRole:        return pack.source;
    case RoomIdRole:        return pack.roomId;
    case StateKeyRole:      return pack.stateKey;
    case EnabledGloballyRole: return pack.enabledGlobally;
    case CanManageRole:     return pack.canManage;
    case ImageCountRole:    return pack.images.size();
    case StickerCountRole:  return pack.stickerCount();
    case EmoticonCountRole: return pack.emoticonCount();
    default:                return {};
    }
}

QHash<int, QByteArray> StickerPackModel::roleNames() const
{
    return {
        { IdRole,            "packId" },
        { DisplayNameRole,   "displayName" },
        { AvatarUrlRole,     "avatarUrl" },
        { AttributionRole,   "attribution" },
        { SourceRole,        "source" },
        { RoomIdRole,        "roomId" },
        { StateKeyRole,      "stateKey" },
        { EnabledGloballyRole, "enabledGlobally" },
        { CanManageRole,     "canManage" },
        { ImageCountRole,    "imageCount" },
        { StickerCountRole,  "stickerCount" },
        { EmoticonCountRole, "emoticonCount" },
    };
}

void StickerPackModel::reset(const QList<stickers::Pack> &packs)
{
    beginResetModel();
    m_packs = packs;
    endResetModel();
    Q_EMIT countChanged();
}

void StickerPackModel::clear()
{
    if (m_packs.isEmpty())
        return;
    beginResetModel();
    m_packs.clear();
    endResetModel();
    Q_EMIT countChanged();
}

QVariantMap StickerPackModel::get(int row) const
{
    if (row < 0 || row >= m_packs.size())
        return {};
    const stickers::Pack &pack = m_packs.at(row);
    QVariantMap map;
    map.insert(QStringLiteral("packId"), pack.id);
    map.insert(QStringLiteral("displayName"), pack.displayName);
    map.insert(QStringLiteral("avatarUrl"), pack.avatarUrl);
    map.insert(QStringLiteral("attribution"), pack.attribution);
    map.insert(QStringLiteral("source"), pack.source);
    map.insert(QStringLiteral("roomId"), pack.roomId);
    map.insert(QStringLiteral("stateKey"), pack.stateKey);
    map.insert(QStringLiteral("enabledGlobally"), pack.enabledGlobally);
    map.insert(QStringLiteral("canManage"), pack.canManage);
    map.insert(QStringLiteral("imageCount"), pack.images.size());
    map.insert(QStringLiteral("stickerCount"), pack.stickerCount());
    map.insert(QStringLiteral("emoticonCount"), pack.emoticonCount());
    return map;
}

int StickerPackModel::indexOfPack(const QString &id) const
{
    for (int row = 0; row < m_packs.size(); ++row) {
        if (m_packs.at(row).id == id)
            return row;
    }
    return -1;
}
