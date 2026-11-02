#pragma once

#include <QHash>
#include <functional>
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

    void setClient(MatrixClient *client);
    void clear();

    /// Room state wins: only an explicit room avatar disables this, never a
    /// room name. Otherwise use the peer's avatar for a room m.direct
    /// classifies as a strict 1:1 (never an arbitrary face for a group DM).
    QString avatarFor(const RoomInfo &room) const;

    void resolveMissing(const QList<RoomInfo> &rooms);

    static constexpr int kMaxRefreshesPerPass = 6;
    void setRefreshIntervalMs(qint64 ms) { m_refreshMs = ms; }
    void setClockForTest(std::function<qint64()> nowMs) { m_clock = std::move(nowMs); }

    void applyPeerAvatar(const QString &roomId, const QString &userId,
                         const QString &avatarUrl);

Q_SIGNALS:
    void avatarResolved(const QString &userId);

private Q_SLOTS:
    void onUserProfileFinished(quint64 opId, bool ok, const QString &userId,
                               const QString &displayName,
                               const QString &avatarUrl,
                               const QString &category);

private:
    QString directPeer(const RoomInfo &room) const;

    MatrixClient *m_client = nullptr;
    QHash<QString, QString> m_avatars;
    QHash<quint64, QString> m_ops;
    QSet<QString> m_pending;
    QHash<QString, qint64> m_refreshDueAt;
    qint64 m_refreshMs = 15 * 60 * 1000;
    std::function<qint64()> m_clock;
    qint64 nowMs() const;
    QSet<QString> m_noAvatar;
    struct PeerFace
    {
        QString userId;
        QString avatarUrl;
    };
    QHash<QString, PeerFace> m_peerFaces;
    bool hasPeerFace(const QString &roomId, const QString &peer) const;
};
