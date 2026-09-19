#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

/// ── EVERY SIGNED-IN ACCOUNT'S LAST KNOWN PICTURE, ON DISK ───────────────────
///
/// The media cache is a `QHash` in RAM (`MediaBridge::m_cache`) and nothing
/// persists it, so every avatar in the application is re-fetched from scratch
/// on each launch. For the account switcher that is not a cosmetic delay, it
/// is a picture that may never arrive at all:
///
///   * `Avatar` resolves an `mxc://` through `MediaBridge`, which fetches
///     through whichever client is ACTIVE. An inactive account's avatar lives
///     on ITS homeserver, and the active client has no business — and on a
///     different server, no ability — to fetch it. So the other accounts in
///     the switcher fall back to initials indefinitely.
///   * Even the active account shows initials for the first moments of every
///     launch, before its own avatar has been fetched again.
///
/// This store is the last-known picture per account, written when we DO have
/// the bytes (the account is active and its avatar resolved) and readable
/// whenever any account needs drawing, including while it is signed out of
/// focus. A cache, never a source of truth: the mxc in the account record
/// still decides what the picture IS, and a stale file is replaced the moment
/// the real one resolves.
///
/// SCOPE, deliberately app-level and not account-scoped. Every other
/// per-account artifact lives under the account's own store directory and is
/// deleted with it — correct for a crypto store, wrong here, because the whole
/// point is reading account B's picture while account A is the live session.
/// Removal is therefore explicit: `forget()` on sign-out, called from the same
/// place that removes the record.
///
/// PRIVACY. A Matrix avatar is public content — it is served unauthenticated
/// to anyone who knows the mxc, and it is already on this disk inside the
/// media cache's process memory and in the SDK's own event-cache store. What
/// is new is only that it OUTLIVES the process. The file name is
/// `safeUserSlug()`, the existing convention for a path that names an account
/// (CLAUDE.md §6: local account-scoped paths are deliberate practice and stay
/// on the user's own machine), and nothing here is ever exported: the
/// support-diagnostics bundle carries hashed ids and no paths.
class AccountAvatarStore : public QObject
{
    Q_OBJECT
public:
    explicit AccountAvatarStore(QObject *parent = nullptr);

    /// A `file://` URL for this account's last known avatar, or empty when
    /// there is none. `Q_INVOKABLE` because the account switcher's delegate
    /// asks per row.
    ///
    /// Carries the file's mtime as a query so a QML `Image` reloads when the
    /// bytes are replaced — the same reason `MediaBridge::cachedSource`
    /// appends a revision, and for the same failure: Qt caches by URL string
    /// and a path that never changes shows the old picture for ever.
    Q_INVOKABLE QString avatarUrlFor(const QString &userId) const;

    /// Write (or replace) this account's picture. Refuses anything that is
    /// not a raster image this client already accepts, and anything above
    /// `kMaxBytes` — a cap, not a guess: an avatar is fetched at
    /// `kAvatarCanonicalEdge` and one that does not fit is not an avatar.
    ///
    /// Returns true only when bytes were actually written, so a caller can
    /// tell "stored" from "declined" — this project has paid for conflating
    /// those more than once.
    bool store(const QString &userId, const QByteArray &bytes);

    /// Remove one account's picture. Called when the account is removed, so
    /// signing out really does take the picture with it.
    bool forget(const QString &userId);

    /// The directory, created on demand. Empty when no writable location can
    /// be resolved, which is the one case every caller treats as "no store".
    static QString storeRoot();

    /// 256 KiB. The canonical avatar edge is 96px; a PNG of that is a few
    /// tens of KiB and this leaves room for a detailed one without letting a
    /// hostile or broken server park a megabyte per account on disk.
    static constexpr qint64 kMaxBytes = 256 * 1024;

Q_SIGNALS:
    /// One account's stored picture changed. The switcher rebinds on it
    /// rather than polling, and it carries the user id so a delegate can
    /// ignore rows that did not move.
    void avatarStored(const QString &userId);
};
