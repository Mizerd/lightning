#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "matrix/RtcSession.h"

class MatrixClient;

class RtcController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RtcController is exposed via app.rtc")

    // Whether this backend speaks MatrixRTC at all (mock and HTTP do not).
    Q_PROPERTY(bool supported READ supported NOTIFY availabilityChanged)
    Q_PROPERTY(bool callingAvailable READ callingAvailable
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QString availabilityCategory READ availabilityCategory
                   NOTIFY availabilityChanged)

public:
    enum class JoinBlock {
        None,
        Unsupported,
        Undiscovered,
        NoTransport,
        DiscoveryFailed,
        SessionClosed,
        NoMediaTransport,
        /// This account's traffic follows a network route (a proxy), which
        /// cannot carry a call; joining would connect directly.
        NetworkRoute,
        /// This account may not write `org.matrix.msc3401.call.member` here
        /// (default power levels put state events at 50, so this is the
        /// ordinary member's case), so the server would refuse the join.
        NoPermission,
        /// The room is encrypted but call media E2EE is not active; refused
        /// rather than joined in the clear.
        MediaEncryptionUnavailable,
    };
    Q_ENUM(JoinBlock)

    explicit RtcController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool callingAvailable() const;
    QString availabilityCategory() const { return m_availabilityCategory; }

    Q_INVOKABLE void refresh(const QString &roomId);
    Q_INVOKABLE void refreshFromServer(const QString &roomId);
    Q_INVOKABLE void discover(const QString &roomId);

    Q_INVOKABLE int participantCount(const QString &roomId) const;
    int authoritativeParticipantCount(const QString &roomId) const;
    Q_INVOKABLE bool hasLiveSession(const QString &roomId) const;
    Q_INVOKABLE bool ownUserInSession(const QString &roomId) const;
    Q_INVOKABLE bool ownDeviceInSession(const QString &roomId) const;
    Q_INVOKABLE bool hasVideoIntent(const QString &roomId) const;

    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          int max = -1) const;
    Q_INVOKABLE QStringList participantUserIds(const QString &roomId,
                                               int max = -1) const;
    Q_INVOKABLE QVariantList participantFaces(const QString &roomId,
                                              int max = -1) const;

    QString mediaKeyTargetsJson(const QString &roomId) const;

    QString sessionFocusFor(const QString &roomId) const;

    bool discoveryWorthRetrying() const;

    Q_INVOKABLE QString focusUrlFor(const QString &roomId) const;
    QString ownFocusUrlFor() const;
    QStringList subscriberFocusUrlsFor(const QString &roomId) const;
    /// Whether two focus URLs name the same SFU. Members may spell one focus
    /// with or without the bare origin's trailing slash (GitHub #57).
    /// Match Rust's default-port and dot-segment normalization; prefix
    /// trailing slashes remain distinct. Never rewrite the advertised URL.
    static bool sameFocusUrl(const QString &left, const QString &right);
    bool usesLegacyFocusFor(const QString &roomId) const;

    Q_INVOKABLE JoinBlock joinBlock(const QString &roomId) const;

    void setCanPublishMembership(const QString &roomId, bool can);
    void setCurrentRoom(const QString &roomId, bool snapshotRequested = false);
    void noteMembershipRefused(const QString &roomId, const QString &category);
    void setPermissionRereadMsForTest(int ms);
    Q_INVOKABLE QString joinBlockReason(const QString &roomId) const;

    void setPokeCoalesceMsForTest(int ms);
    void setReadTimeoutMsForTest(int ms);

    void setMediaEncryptionAvailable(bool available);
    void setMediaAvailable(bool available);
    bool mediaAvailable() const { return m_mediaAvailable; }
    bool mediaEncryptionAvailable() const { return m_mediaEncryption; }
    void setRoomEncrypted(const QString &roomId, bool encrypted);
    QString rtcIdentityFor(const QString &roomId, const QString &userId,
                           const QString &deviceId) const;

    QVariantMap participantForIdentity(const QString &roomId,
                                      const QString &identity) const;

    QString identityForMembership(const QString &roomId,
                                  const QString &membershipEventId,
                                  const QString &sender) const;
    bool knowsMembership(const QString &roomId,
                         const QString &membershipEventId) const;
    QString ownMembershipEventId(const QString &roomId) const;

    enum class RoomEncryption { Unknown, No, Yes };

    using EncryptionResolver =
        std::function<RoomEncryption(const QString &roomId)>;
    void setEncryptionResolver(EncryptionResolver resolver);

    /// Whether this room's media must be encrypted. Unknown fails closed to
    /// true, so the failure mode is a refused call, never a cleartext one.
    /// Asks the resolver rather than relying on callers having pushed a value.
    bool roomEncrypted(const QString &roomId) const;

Q_SIGNALS:
    void sessionChanged(const QString &roomId);
    void availabilityChanged();
    void membershipRefused(const QString &roomId);

private Q_SLOTS:
    void onSessionReceived(quint64 opId, const RtcSessionData &session);
    void onSessionPoked(const QString &roomId);
    void onTransportsReceived(quint64 opId, bool serverAnswered,
                              const QString &category,
                              const QStringList &serverServiceUrls,
                              const QString &participantFocusUrl);
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);
    void onRoomMemberEventSeen(const QString &roomId);
    void rereadCurrentRoomPermission();
    void rereadExpiredSessions();

private:
    void clearForNewSession();
    bool publishRefused(const QString &roomId) const;
    void requestPermissionRead(const QString &roomId);
    void reapStalePermissionReads();
    void flushPokes();
    void reapStaleReads();
    void scheduleExpiryReread();
    bool expiryReadDue(const QString &roomId, qint64 expiresAtMs,
                       qint64 nowMs) const;
    bool transportReachableFor(const QString &roomId) const;

    QPointer<MatrixClient> m_client;
    QHash<QString, RtcSessionData> m_sessions;
    void noteUnresolvedIdentity(const QString &identity,
                                const QString &reason) const;
public:
    void forgetUnresolvedIdentityDiagnostics();
    void setUnresolvedIdentityGraceMsForTest(qint64 ms)
    {
        m_unresolvedIdentityGraceMs = ms;
    }
    void setServerReadCooldownMsForTest(int ms)
    {
        m_serverReadCooldownMs = ms;
    }

private:
    mutable QSet<QString> m_unresolvedIdentitiesLogged;
    mutable QHash<QString, qint64> m_unresolvedIdentityFirstSeenMs;
    qint64 m_unresolvedIdentityGraceMs = 5000;

    struct PendingRead {
        QString roomId;
        /// Dispatch time. The Rust event queue drops the oldest event on
        /// overflow, so a reply may never arrive; without a timeout the room
        /// would stay unrefreshable and the poke timer would spin.
        qint64 dispatchedAtMs = 0;
    };
    QHash<quint64, PendingRead> m_pendingReads;
    QSet<QString> m_roomsBeingRead;

    QSet<QString> m_serverReadWanted;
    QHash<QString, qint64> m_lastServerReadMs;
    QHash<QString, int> m_serverReadStreak;
    int m_serverReadCooldownMs = 10000;
    int m_serverReadCooldownMaxMs = 300000;

    QSet<QString> m_pokedRooms;
    QTimer m_pokeTimer;
    /// Fires just after the earliest observed membership expiry. Expiry
    /// writes no state, so nothing else would make a vanished client's
    /// membership leave the banner and the facepile.
    QTimer m_expiryTimer;
    QHash<QString, qint64> m_sessionReadAtMs;
    int m_pokeCoalesceMs = 250;
    int m_readTimeoutMs = 30000;

    bool m_discovered = false;
    bool m_serverAnswered = false;
    QStringList m_serviceUrls;
    QHash<QString, QString> m_participantFocus;
    QString m_discoveryRoomId;
    QString m_availabilityCategory;
    quint64 m_discoveryOp = 0;
    bool m_mediaEncryption = false;
    bool m_mediaAvailable = false;
    // Mutable because the const roomEncrypted() remembers a resolver Yes, so
    // the irreversibility guard covers every room seen encrypted.
    mutable QHash<QString, RoomEncryption> m_encryptedRooms;
    EncryptionResolver m_encryptionResolver;
    QHash<QString, bool> m_canPublishMembership;
    QSet<QString> m_permissionStale;
    QHash<QString, qint64> m_membershipRefusedAtMs;
    QHash<quint64, PendingRead> m_permissionReads;
    QSet<QString> m_permissionRoomsBeingRead;
    QString m_currentRoom;
    QTimer m_permissionRereadTimer;
    int m_permissionRereadMs = 10000;

};
