#include "gif/GifResultModel.h"

GifResultModel::GifResultModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GifResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant GifResultModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const gif::GifResult &r = m_rows.at(index.row());
    switch (role) {
    case ProviderRole:      return r.provider;
    case GifIdRole:         return r.id;
    case TitleRole:         return r.title;
    case PreviewUrlRole:    return r.previewUrl;
    case StillUrlRole:      return r.stillUrl;
    case GifUrlRole:        return r.gifUrl;
    case Mp4UrlRole:        return r.mp4Url;
    case WidthRole:         return r.gifWidth;
    case HeightRole:        return r.gifHeight;
    case PreviewWidthRole:  return r.previewWidth;
    case PreviewHeightRole: return r.previewHeight;
    case AspectRole: {
        const int w = r.previewWidth > 0 ? r.previewWidth : r.gifWidth;
        const int h = r.previewHeight > 0 ? r.previewHeight : r.gifHeight;
        return (w > 0 && h > 0) ? static_cast<double>(w) / h : 1.0;
    }
    case RatingRole:        return r.rating;
    case FavoriteRole:
        return m_isFavorite ? m_isFavorite(r.provider, r.id) : false;
    case BytesRole:         return r.gifBytes;
    default:                return {};
    }
}

QHash<int, QByteArray> GifResultModel::roleNames() const
{
    return {
        { ProviderRole,      "provider" },
        { GifIdRole,         "gifId" },
        { TitleRole,         "title" },
        { PreviewUrlRole,    "previewUrl" },
        { StillUrlRole,      "stillUrl" },
        { GifUrlRole,        "gifUrl" },
        { Mp4UrlRole,        "mp4Url" },
        { WidthRole,         "gifWidth" },
        { HeightRole,        "gifHeight" },
        { PreviewWidthRole,  "previewWidth" },
        { PreviewHeightRole, "previewHeight" },
        { AspectRole,        "aspect" },
        { RatingRole,        "rating" },
        { FavoriteRole,      "favorite" },
        { BytesRole,         "gifBytes" },
    };
}

void GifResultModel::reset(const QList<gif::GifResult> &rows)
{
    beginResetModel();
    m_rows.clear();
    m_seen.clear();
    for (const auto &r : rows) {
        if (m_rows.size() >= kMaxRows)
            break;
        const QString key = dedupKey(r.provider, r.id);
        if (m_seen.contains(key))
            continue;
        m_seen.insert(key);
        m_rows.append(r);
    }
    endResetModel();
    Q_EMIT countChanged();
}

int GifResultModel::append(const QList<gif::GifResult> &rows)
{
    QList<gif::GifResult> fresh;
    fresh.reserve(rows.size());
    for (const auto &r : rows) {
        if (m_rows.size() + fresh.size() >= kMaxRows)
            break;
        const QString key = dedupKey(r.provider, r.id);
        if (m_seen.contains(key))
            continue;
        m_seen.insert(key);
        fresh.append(r);
    }
    if (fresh.isEmpty())
        return 0;
    beginInsertRows({}, m_rows.size(), m_rows.size() + fresh.size() - 1);
    m_rows.append(fresh);
    endInsertRows();
    Q_EMIT countChanged();
    return fresh.size();
}

void GifResultModel::clear()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    m_seen.clear();
    endResetModel();
    Q_EMIT countChanged();
}

gif::GifResult GifResultModel::resultAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

QVariantMap GifResultModel::get(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const QModelIndex idx = index(row, 0);
    QVariantMap map;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}

void GifResultModel::refreshFavorite(const QString &provider, const QString &id)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).provider == provider && m_rows.at(row).id == id) {
            const QModelIndex idx = index(row, 0);
            Q_EMIT dataChanged(idx, idx, { FavoriteRole });
        }
    }
}
