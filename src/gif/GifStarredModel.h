#pragma once

#include "gif/GifStoredModel.h"

// v0.6.6: locally-persisted "starred chat GIFs" — see GifStarredStore for
// the full feature (that class owns hashing, validation, disk I/O, caps and
// account scoping; this is just the thin GifStoredModel sibling that holds
// the resulting rows, exactly like GifFavoritesModel/GifRecentModel already
// do for their own collections). Every row carries provider == "local" and
// id == the sha256 content hash of the stored file — never a room, event,
// sender, or any other Matrix identifier (see GifStarredStore's header for
// why provenance is deliberately never persisted).
class GifStarredModel : public GifStoredModel
{
    Q_OBJECT

public:
    explicit GifStarredModel(QSettings *settings, QObject *parent = nullptr);

    // Insert or refresh a validated local entry — called only by
    // GifStarredStore, which has already hashed, validated and (for a new
    // hash) written the bytes to disk. Never called from QML directly.
    void insertLocal(const gif::GifResult &r) { insertFront(r); }
    // Remove by content hash. Safe to call for an absent hash (no-op).
    // Returns true only if a row was actually removed, so callers (e.g.
    // GifStarredStore's unstarFinished feedback) can distinguish a real
    // removal from a no-op.
    Q_INVOKABLE bool unstar(const QString &hash)
    { return removeEntry(QStringLiteral("local"), hash); }
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return contains(QStringLiteral("local"), hash); }

    // Sum of every stored entry's byte size — used to enforce the total-size
    // cap (GifStoredModel's own cap is item-count only).
    qint64 totalBytes() const;

    // Bounded: an explicit item cap (refused, never silently evicted — see
    // GifStarredStore::star()) and a total-size cap enforced by the store.
    static constexpr int kMaxStarred = 200;
};
