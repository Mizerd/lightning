#pragma once

#include "gif/GifResponseParser.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>

#include <functional>

class GifResultModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        ProviderRole = Qt::UserRole + 1,
        GifIdRole,
        TitleRole,
        PreviewUrlRole,
        StillUrlRole,
        GifUrlRole,
        Mp4UrlRole,
        WidthRole,
        HeightRole,
        PreviewWidthRole,
        PreviewHeightRole,
        AspectRole,
        RatingRole,
        FavoriteRole,
        BytesRole,
    };

    explicit GifResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_rows.size(); }

    void reset(const QList<gif::GifResult> &rows);
    int append(const QList<gif::GifResult> &rows);
    void clear();

    gif::GifResult resultAt(int row) const;
    Q_INVOKABLE QVariantMap get(int row) const;

    void refreshFavorite(const QString &provider, const QString &id);

    void setFavoriteResolver(std::function<bool(const QString &, const QString &)> fn)
    { m_isFavorite = std::move(fn); }

    static QString dedupKey(const QString &provider, const QString &id)
    { return provider + QChar(0x1f) + id; }

    static constexpr int kMaxRows = 300;

Q_SIGNALS:
    void countChanged();

private:
    QList<gif::GifResult> m_rows;
    QSet<QString> m_seen;
    std::function<bool(const QString &, const QString &)> m_isFavorite;
};
