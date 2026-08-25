// The one place that answers "which picture does this conversation wear?".
//
// A Matrix DM usually has NO room avatar: the room state is bare and every
// client paints the other person's profile picture instead. Deriving that is
// three separate jobs — decide whether the room is an unambiguous 1:1, look
// the peer up, and fetch the peer's profile once if nobody has — and the room
// list has carried all three privately since 0.6.x. The Channels column then
// grew its own room rows straight from RoomInfo::avatarUrl and showed initials
// for every DM, next to a Home strip showing the real faces.
//
// So the derivation lives here, once, and both list models own one. The caches
// are per-owner and that is deliberate: they are pure presentation memory, the
// fetch is idempotent and bounded, and sharing one instance would couple two
// models that were separated on purpose.
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "matrix/MatrixClient.h"

class DirectAvatarResolver : public QObject
{
    Q_OBJECT

public:
    explicit DirectAvatarResolver(QObject *parent = nullptr);

    /// Follows the client for the lifetime of a session. Passing a different
    /// client (or nullptr) drops everything learned about the previous one:
    /// an avatar cache keyed by user id is account-scoped memory.
    void setClient(MatrixClient *client);
    void clear();

    /// Matrix room state always wins. An explicit room NAME must never
    /// disable this — only an explicit room AVATAR does. Otherwise derive a
    /// member avatar for a room authoritatively classified by m.direct, and
    /// only when it is unambiguously a strict 1:1 (never an arbitrary face
    /// for a group DM).
    QString avatarFor(const RoomInfo &room) const;

    /// Starts at most one bounded profile fetch per peer we cannot answer for
    /// yet. Safe to call on every rebuild: a pending or cached peer is skipped.
    void resolveMissing(const QList<RoomInfo> &rooms);

Q_SIGNALS:
    /// One resolved peer. Owners re-emit dataChanged for the rows that use it
    /// rather than rebuilding, so a late profile cannot move anything.
    void avatarResolved(const QString &userId);

private Q_SLOTS:
    void onUserProfileFinished(quint64 opId, bool ok, const QString &userId,
                               const QString &displayName,
                               const QString &avatarUrl,
                               const QString &category);

private:
    /// The peer whose face this room would wear, or empty when the room is
    /// not an unambiguous 1:1 DM (or already carries its own avatar).
    QString directPeer(const RoomInfo &room) const;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_avatars;
    QHash<quint64, QString> m_ops;
    QSet<QString> m_pending;
    /// Peers we have ASKED about and learned no picture for — a profile that
    /// answered with no avatar set, or a lookup that failed.
    ///
    /// Without this the resolver could not tell "never asked" from "asked,
    /// there is nothing", so every rebuild re-dispatched the same fetch. With
    /// an owner that rebuilds when a peer resolves, that is an unbounded loop
    /// of one /profile request per network round trip, per avatarless peer,
    /// for the whole session — the account switch that "takes longer now".
    ///
    /// Session-scoped and cleared with the rest on sign-out, so it can never
    /// describe the next account's peers. A failure is cached too: retrying
    /// it is what re-creates the loop by another route, and `avatarFor()`
    /// consults the room's own member snapshot BEFORE this cache, so a face
    /// arriving on a member event still wins.
    QSet<QString> m_noAvatar;
};
