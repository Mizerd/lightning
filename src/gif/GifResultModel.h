#pragma once

#include "gif/GifResponseParser.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>

#include <functional>

// v0.6.1: the animated result grid model. Holds safe gif::GifResult rows and
// exposes only presentation-safe roles to QML — never raw provider JSON. Dedup
// is by (provider, id) so pagination never repeats a tile. Bounded so a runaway
// provider cannot grow the model without limit.
class GifResultModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        ProviderRole = Qt::UserRole + 1,
        GifIdRole,
        TitleRole,
        PreviewUrlRole,   // small animated preview
        StillUrlRole,     // static thumbnail
        GifUrlRole,       // sendable original (download source)
        Mp4UrlRole,       // optional preview video — never sent
        WidthRole,        // sendable width
        HeightRole,       // sendable height
        PreviewWidthRole,
        PreviewHeightRole,
        AspectRole,       // width/height, for stable grid sizing
        RatingRole,
        FavoriteRole,     // set by the controller from favorites state
        // v0.6.5: the sendable-variant byte size the provider parser already
        // extracts (gif::GifResult::gifBytes) but which never reached QML
        // before now. 0 = unknown; the picker only shows a size overlay when
        // this is > 0.
        BytesRole,
    };

    explicit GifResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_rows.size(); }

    // Replace all rows (a fresh trending/search). Resets dedup.
    void reset(const QList<gif::GifResult> &rows);
    // Append a page, skipping (provider,id) duplicates. Returns how many were
    // actually added. Enforces the bounded cap.
    int append(const QList<gif::GifResult> &rows);
    void clear();

    // Row lookup for send/favorite actions.
    gif::GifResult resultAt(int row) const;
    Q_INVOKABLE QVariantMap get(int row) const;

    // Re-emit FavoriteRole for a (provider,id) when favorites change, without
    // rebuilding the grid.
    void refreshFavorite(const QString &provider, const QString &id);

    // Favorite-state hook the controller installs (provider, id) -> bool.
    void setFavoriteResolver(std::function<bool(const QString &, const QString &)> fn)
    { m_isFavorite = std::move(fn); }

    static QString dedupKey(const QString &provider, const QString &id)
    { return provider + QChar(0x1f) + id; } // unit separator

    // Hard cap on loaded results (bounded model / memory).
    static constexpr int kMaxRows = 300;

Q_SIGNALS:
    void countChanged();

private:
    QList<gif::GifResult> m_rows;
    QSet<QString> m_seen;
    std::function<bool(const QString &, const QString &)> m_isFavorite;
};
