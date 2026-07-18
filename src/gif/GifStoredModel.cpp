#include "gif/GifStoredModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
// Only https provider-CDN URLs are ever stored/reloaded, so a corrupted or
// hostile persisted entry can never smuggle a non-provider or non-https URL
// back into the picker.
bool safeHttps(const QString &url)
{
    return url.isEmpty() || url.startsWith(QLatin1String("https://"));
}

QJsonObject toJson(const gif::GifResult &r)
{
    QJsonObject o;
    o.insert(QStringLiteral("provider"), r.provider);
    o.insert(QStringLiteral("id"), r.id);
    o.insert(QStringLiteral("title"), r.title);
    o.insert(QStringLiteral("rating"), r.rating);
    o.insert(QStringLiteral("previewUrl"), r.previewUrl);
    o.insert(QStringLiteral("stillUrl"), r.stillUrl);
    o.insert(QStringLiteral("gifUrl"), r.gifUrl);
    o.insert(QStringLiteral("w"), r.gifWidth);
    o.insert(QStringLiteral("h"), r.gifHeight);
    o.insert(QStringLiteral("pw"), r.previewWidth);
    o.insert(QStringLiteral("ph"), r.previewHeight);
    return o;
}

gif::GifResult fromJson(const QJsonObject &o)
{
    gif::GifResult r;
    r.provider = o.value(QStringLiteral("provider")).toString();
    r.id = o.value(QStringLiteral("id")).toString();
    r.title = o.value(QStringLiteral("title")).toString();
    r.rating = o.value(QStringLiteral("rating")).toString();
    r.previewUrl = o.value(QStringLiteral("previewUrl")).toString();
    r.stillUrl = o.value(QStringLiteral("stillUrl")).toString();
    r.gifUrl = o.value(QStringLiteral("gifUrl")).toString();
    r.gifWidth = o.value(QStringLiteral("w")).toInt();
    r.gifHeight = o.value(QStringLiteral("h")).toInt();
    r.previewWidth = o.value(QStringLiteral("pw")).toInt();
    r.previewHeight = o.value(QStringLiteral("ph")).toInt();
    return r;
}

bool valid(const gif::GifResult &r)
{
    return !r.provider.isEmpty() && !r.id.isEmpty()
        && safeHttps(r.previewUrl) && safeHttps(r.stillUrl)
        && safeHttps(r.gifUrl);
}
} // namespace

GifStoredModel::GifStoredModel(QSettings *settings, QString key, int maxRows,
                               QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
    , m_key(std::move(key))
    , m_maxRows(maxRows)
{
    load();
}

int GifStoredModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant GifStoredModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const gif::GifResult &r = m_rows.at(index.row());
    switch (role) {
    case GifResultModel::ProviderRole:      return r.provider;
    case GifResultModel::GifIdRole:         return r.id;
    case GifResultModel::TitleRole:         return r.title;
    case GifResultModel::PreviewUrlRole:    return r.previewUrl;
    case GifResultModel::StillUrlRole:      return r.stillUrl;
    case GifResultModel::GifUrlRole:        return r.gifUrl;
    case GifResultModel::Mp4UrlRole:        return r.mp4Url;
    case GifResultModel::WidthRole:         return r.gifWidth;
    case GifResultModel::HeightRole:        return r.gifHeight;
    case GifResultModel::PreviewWidthRole:  return r.previewWidth;
    case GifResultModel::PreviewHeightRole: return r.previewHeight;
    case GifResultModel::AspectRole: {
        const int w = r.previewWidth > 0 ? r.previewWidth : r.gifWidth;
        const int h = r.previewHeight > 0 ? r.previewHeight : r.gifHeight;
        return (w > 0 && h > 0) ? static_cast<double>(w) / h : 1.0;
    }
    case GifResultModel::RatingRole:        return r.rating;
    case GifResultModel::FavoriteRole:      return true; // stored == favorited
    default:                                return {};
    }
}

QHash<int, QByteArray> GifStoredModel::roleNames() const
{
    return GifResultModel().roleNames();
}

int GifStoredModel::indexOf(const QString &provider, const QString &id) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).provider == provider && m_rows.at(i).id == id)
            return i;
    return -1;
}

bool GifStoredModel::contains(const QString &provider, const QString &id) const
{
    return indexOf(provider, id) >= 0;
}

QVariantMap GifStoredModel::get(int row) const
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

gif::GifResult GifStoredModel::resultAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

gif::GifResult GifStoredModel::fromVariantMap(const QVariantMap &map)
{
    gif::GifResult r;
    r.provider = map.value(QStringLiteral("provider")).toString();
    r.id = map.value(QStringLiteral("gifId")).toString();
    r.title = map.value(QStringLiteral("title")).toString();
    r.rating = map.value(QStringLiteral("rating")).toString();
    r.previewUrl = map.value(QStringLiteral("previewUrl")).toString();
    r.stillUrl = map.value(QStringLiteral("stillUrl")).toString();
    r.gifUrl = map.value(QStringLiteral("gifUrl")).toString();
    r.gifWidth = map.value(QStringLiteral("gifWidth")).toInt();
    r.gifHeight = map.value(QStringLiteral("gifHeight")).toInt();
    r.previewWidth = map.value(QStringLiteral("previewWidth")).toInt();
    r.previewHeight = map.value(QStringLiteral("previewHeight")).toInt();
    return r;
}

void GifStoredModel::insertFront(const gif::GifResult &r)
{
    if (!valid(r))
        return;
    const int existing = indexOf(r.provider, r.id);
    if (existing == 0) {
        // Already newest — refresh metadata in place and persist.
        m_rows[0] = r;
        Q_EMIT dataChanged(index(0), index(0));
        save();
        return;
    }
    if (existing > 0) {
        beginRemoveRows({}, existing, existing);
        m_rows.removeAt(existing);
        endRemoveRows();
    }
    beginInsertRows({}, 0, 0);
    m_rows.prepend(r);
    endInsertRows();
    // Enforce the cap from the tail (oldest).
    while (m_rows.size() > m_maxRows) {
        const int last = m_rows.size() - 1;
        beginRemoveRows({}, last, last);
        m_rows.removeAt(last);
        endRemoveRows();
    }
    Q_EMIT countChanged();
    save();
}

bool GifStoredModel::removeEntry(const QString &provider, const QString &id)
{
    const int row = indexOf(provider, id);
    if (row < 0)
        return false;
    beginRemoveRows({}, row, row);
    m_rows.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
    save();
    return true;
}

void GifStoredModel::clearAll()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    Q_EMIT countChanged();
    save();
}

void GifStoredModel::load()
{
    if (!m_settings)
        return;
    const QByteArray raw =
        m_settings->value(m_key).toString().toUtf8();
    if (raw.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray())
        return; // corrupted store → start empty, never crash
    beginResetModel();
    m_rows.clear();
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        if (m_rows.size() >= m_maxRows)
            break;
        const gif::GifResult r = fromJson(v.toObject());
        if (valid(r) && indexOf(r.provider, r.id) < 0)
            m_rows.append(r);
    }
    endResetModel();
    Q_EMIT countChanged();
}

void GifStoredModel::save()
{
    if (!m_settings)
        return;
    QJsonArray arr;
    for (const auto &r : m_rows)
        arr.append(toJson(r));
    m_settings->setValue(
        m_key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}
