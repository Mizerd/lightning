// Answers "which picture does this conversation wear?".
//
// A Matrix DM usually has no room avatar, so clients show the peer's profile
// picture: decide whether the room is an unambiguous 1:1, find the peer, and
// fetch their profile once. Both room-list models own one of these so every
// surface derives it the same way. Caches are per-owner on purpose: they are
// presentation memory and the fetch is idempotent and bounded.
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

    /// Follows the client for a session. A different client (or nullptr) drops
    /// everything learned: the cache is account-scoped.
    void setClient(MatrixClient *client);
    void clear();

    /// Room state wins: only an explicit room avatar disables this, never a
    /// room name. Otherwise use the peer's avatar for a room m.direct
    /// classifies as a strict 1:1 (never an arbitrary face for a group DM).
    QString avatarFor(const RoomInfo &room) const;

    /// Starts at most one bounded profile fetch per unanswered peer. Safe on
    /// every rebuild: pending and cached peers are skipped.
    void resolveMissing(const QList<RoomInfo> &rooms);

Q_SIGNALS:
    /// One peer resolved. Owners emit dataChanged for the affected rows instead
    /// of rebuilding, so a late profile cannot move anything.
    void avatarResolved(const QString &userId);

private Q_SLOTS:
    void onUserProfileFinished(quint64 opId, bool ok, const QString &userId,
                               const QString &displayName,
                               const QString &avatarUrl,
                               const QString &category);

private:
    /// The peer whose face this room would wear, or empty when the room is not
    /// an unambiguous 1:1 DM or has its own avatar.
    QString directPeer(const RoomInfo &room) const;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_avatars;
    QHash<quint64, QString> m_ops;
    QSet<QString> m_pending;
    /// Peers we asked about and learned no picture for (no avatar set, or our
    /// own lookup failed). Distinguishes "asked, nothing there" from "never
    /// asked", so rebuilds do not re-fetch in a loop. Session-scoped and
    /// cleared on sign-out. Only failures this resolver incurred are recorded;
    /// the shared signal also carries other consumers' failures, which must not
    /// pin a DM to initials. avatarFor() checks the member snapshot first, so a
    /// later face still wins.
    QSet<QString> m_noAvatar;
};
