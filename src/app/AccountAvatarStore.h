#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class AccountAvatarStore : public QObject
{
    Q_OBJECT
public:
    explicit AccountAvatarStore(QObject *parent = nullptr);

    Q_INVOKABLE QString avatarUrlFor(const QString &userId) const;

    /// Write or replace the account's picture. Refuses non-raster data and
    /// anything above `kMaxBytes`. Returns true only when bytes were written.
    bool store(const QString &userId, const QByteArray &bytes);

    bool forget(const QString &userId);

    bool hasStored(const QString &userId) const;

    static QString storeRoot();

    static constexpr qint64 kMaxBytes = 256 * 1024;

Q_SIGNALS:
    void avatarStored(const QString &userId);
};
