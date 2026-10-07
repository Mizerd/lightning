#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

/// Each account's last known avatar, on disk.
///
/// The media cache lives in RAM and fetches through the active client, so an
/// inactive account's avatar (on its own homeserver) could never be fetched
/// for the switcher. This store keeps the last picture seen while each account
/// was active. It is a cache: the account record's mxc stays authoritative.
///
/// App-level rather than account-scoped, because account B's picture must be
/// readable while A is active; `forget()` removes it on sign-out. Avatars are
/// public content; files are named by `safeUserSlug()` and never exported.
class AccountAvatarStore : public QObject
{
    Q_OBJECT
public:
    explicit AccountAvatarStore(QObject *parent = nullptr);

    /// A `file://` URL for the account's last known avatar, or empty. The
    /// file's mtime is appended as a query so a QML `Image`, which caches by
    /// URL, reloads when the bytes are replaced.
    Q_INVOKABLE QString avatarUrlFor(const QString &userId) const;

    /// Write or replace the account's picture. Refuses non-raster data and
    /// anything above `kMaxBytes`. Returns true only when bytes were written.
    bool store(const QString &userId, const QByteArray &bytes);

    /// Remove the account's picture; called when the account is removed or
    /// signs out. False when nothing was there to remove, or the delete
    /// failed: use hasStored() first to tell those apart.
    bool forget(const QString &userId);

    /// Whether a picture file exists for the account (any size).
    bool hasStored(const QString &userId) const;

    /// The directory, created on demand; empty when nothing is writable.
    static QString storeRoot();

    /// A 96px avatar is a few tens of KiB; this bounds what a hostile server
    /// can put on disk.
    static constexpr qint64 kMaxBytes = 256 * 1024;

Q_SIGNALS:
    /// One account's stored picture changed.
    void avatarStored(const QString &userId);
};
