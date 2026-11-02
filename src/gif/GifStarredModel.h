#pragma once

#include "gif/GifStoredModel.h"

class GifStarredModel : public GifStoredModel
{
    Q_OBJECT

public:
    explicit GifStarredModel(QSettings *settings, QObject *parent = nullptr);

    // Inserts or refreshes a validated entry. Called only by GifStarredStore
    // after the bytes are on disk.
    void insertLocal(const gif::GifResult &r) { insertFront(r); }
    Q_INVOKABLE bool unstar(const QString &hash)
    { return removeEntry(QStringLiteral("local"), hash); }
    Q_INVOKABLE bool hasHash(const QString &hash) const
    { return contains(QStringLiteral("local"), hash); }

    qint64 totalBytes() const;

    static constexpr int kMaxStarred = 200;
};
