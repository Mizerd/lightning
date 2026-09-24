#pragma once

#include "gif/GifStoredModel.h"

// Rows of the locally saved chat images. GifStarredStore owns hashing,
// validation, disk I/O, caps and account scoping; this only holds the rows.
// Each has provider "local" and the file's sha256 as id, never any Matrix
// identifier.
class GifStarredModel : public GifStoredModel
{
    Q_OBJECT

public:
    explicit GifStarredModel(QSettings *settings, QObject *parent = nullptr);

    // Inserts or refreshes a validated entry. Called only by GifStarredStore
    // after the bytes are on disk.
    void insertLocal(const gif::GifResult &r) { insertFront(r); }
    // Removes by content hash. Returns true only when a row was removed.
    Q_INVOKABLE bool unstar(const QString &hash)
    { return removeEntry(QStringLiteral("local"), hash); }
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return contains(QStringLiteral("local"), hash); }

    // Total stored bytes, for the size cap (the base cap counts items only).
    qint64 totalBytes() const;

    // Item cap, enforced as a refusal by GifStarredStore, alongside its size
    // cap.
    static constexpr int kMaxStarred = 200;
};
