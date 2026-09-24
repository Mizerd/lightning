#pragma once

#include "gif/GifResponseParser.h"
#include "gif/GifResultModel.h"

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>

class QSettings;

// Base for the locally persisted GIF collections (Favorites, Recents). Exposes
// the GifResultModel roles so the same delegate renders them. Persists only
// provider, item id, safe title, preview/still/gif URLs, dimensions, rating
// and a timestamp: never a room, thread, event or user id, search query,
// message body, temp path or credential. Local state; not synced.
class GifStoredModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // `settings` is owned by the caller (SettingsManager's store) and must
    // outlive this model. `key` is the QSettings key holding the JSON array.
    GifStoredModel(QSettings *settings, QString key, int maxRows,
                   QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_rows.size(); }

    Q_INVOKABLE bool contains(const QString &provider, const QString &id) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void clearAll();

    // Repoints persistence at `settings` (nullptr = none yet) and reloads,
    // replacing every row. Used by GifStarredStore to switch accounts without
    // recreating the model, whose identity GifSavedModel depends on. A null
    // `settings` clears all rows without persisting.
    void reopen(QSettings *settings);

    gif::GifResult resultAt(int row) const;

    // Build a safe GifResult from a QML result map (GifResultModel role names).
    static gif::GifResult fromVariantMap(const QVariantMap &map);

Q_SIGNALS:
    void countChanged();

protected:
    // Insert `r` at the front (newest first), de-duplicating by (provider,id):
    // an existing entry is moved to the front. Enforces the row cap. Persists.
    void insertFront(const gif::GifResult &r);
    // Remove a (provider,id) entry if present. Persists. Returns true if
    // something was removed.
    bool removeEntry(const QString &provider, const QString &id);
    int indexOf(const QString &provider, const QString &id) const;

    void load();
    void save();

    QList<gif::GifResult> m_rows;

private:
    QSettings *m_settings;
    QString m_key;
    int m_maxRows;
};
