// MatrixRTC (MSC4143) session observation and transport discovery. Answers
// two questions for the UI: is there a call in this room and who is in it,
// and could this account join one (and if not, why). It does not join
// itself; SfuCallController does, gated on joinBlockReason().
//
// Cross-account isolation rests on three mechanisms:
//   1. setClient() disconnects the previous client, so its late replies are
//      never delivered.
//   2. An account switch reuses the same client (detachSession() ->
//      restoreSession()) and emits `loggedOut`, which drops every observed
//      session and pending read here.
//   3. Op ids come from a monotonic per-client counter.
// An epoch field could never fire given (2), and a reply carries only an op
// id; do not re-add one without carrying the account identity on the reply.
//
// Membership changes arrive as payload-free pokes answered by re-reading, so
// remote and local changes share one parse path. Pokes are coalesced, since
// a filling call rewrites many state events in a burst.
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
#include <QtQml/qqmlregistration.h>

#include "matrix/RtcSession.h"

class MatrixClient;

class RtcController : public QObject
{
    Q_OBJECT
    // Registered for symbolic enum comparison in QML; app.rtc is the only
    // instance.
    QML_ELEMENT
    QML_UNCREATABLE("RtcController is exposed via app.rtc")

    // Whether this backend speaks MatrixRTC at all (mock and HTTP do not).
    Q_PROPERTY(bool supported READ supported NOTIFY availabilityChanged)
    // True only when a transport is actually reachable; an unanswered
    // discovery leaves this false.
    Q_PROPERTY(bool callingAvailable READ callingAvailable
                   NOTIFY availabilityChanged)
    // Closed-set category explaining an unavailable transport; empty when
    // calling is available.
    Q_PROPERTY(QString availabilityCategory READ availabilityCategory
                   NOTIFY availabilityChanged)

public:
    /// Why joining is refused. A closed set, so QML never renders a raw server
    /// string and "not looked yet" stays distinct from "looked, found nothing".
    enum class JoinBlock {
        None,
        /// This backend has no MatrixRTC at all.
        Unsupported,
        /// Discovery has not answered yet.
        Undiscovered,
        /// The homeserver answered and offers no transport.
        NoTransport,
        /// Discovery failed (network, rate limit, forbidden...).
        DiscoveryFailed,
        /// A slot state event says the session is closed.
        SessionClosed,
        /// No SFU media engine in this build: a join would publish a
        /// membership nobody can connect to.
        NoMediaTransport,
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
    /// Account-level: could a call be joined anywhere. Room-specific answers
    /// come from joinBlock().
    bool callingAvailable() const;
    QString availabilityCategory() const { return m_availabilityCategory; }

    /// Re-read one room's session. Safe to call repeatedly; reads for the
    /// same room coalesce.
    Q_INVOKABLE void refresh(const QString &roomId);
    /// Re-read one room's session from the homeserver, bypassing the local
    /// store. For when the store is provably incomplete: the SFU reports a
    /// participant no membership accounts for, so no key can reach them.
    /// Costs a `/state` request, so it is rate-limited per room with a window
    /// that grows while forced reads change nothing (m_serverReadStreak).
    Q_INVOKABLE void refreshFromServer(const QString &roomId);
    /// Run transport discovery. `roomId` may be empty.
    Q_INVOKABLE void discover(const QString &roomId);

    /// Number of participant devices in the room's call; 0 means none (or not
    /// observed yet).
    Q_INVOKABLE int participantCount(const QString &roomId) const;
    /// True when somebody is in the room's call and nothing closed it.
    Q_INVOKABLE bool hasLiveSession(const QString &roomId) const;
    /// True when one of the local user's devices is in the call.
    Q_INVOKABLE bool ownUserInSession(const QString &roomId) const;
    /// True when room state holds a membership naming this device. Not the
    /// same as "this device is in a call": a crashed client leaves a
    /// membership behind until it expires. Use the local call controller
    /// (`app.groupCall.active` with a matching `roomId`) for that question.
    Q_INVOKABLE bool ownDeviceInSession(const QString &roomId) const;
    /// True when any participant declared a video intent. An intent, not a
    /// live camera.
    Q_INVOKABLE bool hasVideoIntent(const QString &roomId) const;

    /// Participants for display, oldest-joined first. Each entry:
    /// {userId, deviceId, intent, ownUser, ownDevice, joinedAtMs}.
    /// Bounded by `max` (<= 0 means all).
    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          int max = -1) const;
    /// Distinct user ids in the call, oldest-joined first, for a facepile
    /// (one entry per person across devices).
    Q_INVOKABLE QStringList participantUserIds(const QString &roomId,
                                               int max = -1) const;
    /// Same de-duplication with the room-resolved profile:
    /// {userId, displayName, avatarMxc}.
    Q_INVOKABLE QVariantList participantFaces(const QString &roomId,
                                              int max = -1) const;

    /// Every other device in the session as media-key targets JSON:
    /// `[{"user_id":...,"device_id":...}, ...]`. Not exposed to QML: device
    /// ids are compared, never rendered. Our own device is excluded.
    QString mediaKeyTargetsJson(const QString &roomId) const;

    /// The focus the room's own session advertises (`select_focus`: the
    /// oldest membership's first `foci_preferred` entry, as in element-call).
    /// Without MSC4143 (most homeservers) this is the only focus, and when a
    /// session exists it must win over our own SFU, or we join an empty call.
    QString sessionFocusFor(const QString &roomId) const;

    /// Whether account-scoped discovery is worth running again: false while
    /// one is in flight and once the server has answered. Only the automatic
    /// room-change trigger consults this.
    bool discoveryWorthRetrying() const;

    /// The SFU service URL for this room: the session's focus, else the
    /// homeserver's answer, else the participant-advertised focus. Empty means
    /// no transport is known.
    Q_INVOKABLE QString focusUrlFor(const QString &roomId) const;

    /// Why joining this room's call is refused right now.
    Q_INVOKABLE JoinBlock joinBlock(const QString &roomId) const;

    /// Whether this account may write the room's call membership, per the
    /// room snapshot. Unknown is treated as permitted: Join must not be
    /// disabled on a guess.
    void setCanPublishMembership(const QString &roomId, bool can);
    /// The same answer as a stable token, translated in QML.
    Q_INVOKABLE QString joinBlockReason(const QString &roomId) const;

    /// Test seam: the poke coalescing window. Production uses the default.
    void setPokeCoalesceMsForTest(int ms);
    /// Test seam: how long an unanswered read holds its room.
    void setReadTimeoutMsForTest(int ms);

    /// Whether call media can be encrypted end to end in this build; distinct
    /// from the room's own encryption. Defaults to false, the safe answer.
    void setMediaEncryptionAvailable(bool available);
    /// Whether an SFU media engine exists. Without it a join would publish a
    /// membership no peer could connect to.
    void setMediaAvailable(bool available);
    bool mediaAvailable() const { return m_mediaAvailable; }
    bool mediaEncryptionAvailable() const { return m_mediaEncryption; }
    /// Records a room's known encryption. Supplied by the owner so this class
    /// keeps no second opinion.
    void setRoomEncrypted(const QString &roomId, bool encrypted);
    /// The SFU identity one device uses in this room's session, or empty. A
    /// wire identifier, not for display. Derived in Rust from the membership
    /// (a sha256 for the sticky format) and never recomputed here, so it
    /// matches what Element computes.
    QString rtcIdentityFor(const QString &roomId, const QString &userId,
                           const QString &deviceId) const;

    /// The Matrix person behind an SFU identity:
    /// {userId, deviceId, displayName, avatarMxc, ownUser, ownDevice}, or an
    /// empty map. The reverse of rtcIdentityFor() and the only correct way to
    /// label a call tile; identities cannot be parsed (sticky format ones are
    /// hashes).
    QVariantMap participantForIdentity(const QString &roomId,
                                      const QString &identity) const;

    /// The SFU identity of whoever declared `membershipEventId`, or empty.
    /// Used to attribute raised hands (an `m.reaction` annotating the raiser's
    /// own `m.call.member` event). `sender` must match the membership's user,
    /// or one user could raise everybody's hand. Empty for a membership not
    /// yet observed.
    QString identityForMembership(const QString &roomId,
                                  const QString &membershipEventId,
                                  const QString &sender) const;
    /// Whether the observed session contains `membershipEventId` at all.
    /// Distinguishes the two empty answers of identityForMembership() (not
    /// read yet, which is worth waiting for, versus not owned by the sender),
    /// so forged annotations cannot occupy the pending store.
    bool knowsMembership(const QString &roomId,
                         const QString &membershipEventId) const;
    /// This device's own membership event id in `roomId`, or empty; what a
    /// raise annotates.
    QString ownMembershipEventId(const QString &roomId) const;

    /// What is known about a room's encryption. A bool cannot say unknown,
    /// and storing the fail-closed assumption as `true` made the downgrade
    /// guard latch it for the session.
    enum class RoomEncryption { Unknown, No, Yes };

    /// Answers a room's encryption from what the owner considers authoritative
    /// (AppController: the room list's encrypted/encryptionKnown pair).
    using EncryptionResolver =
        std::function<RoomEncryption(const QString &roomId)>;
    void setEncryptionResolver(EncryptionResolver resolver);

    /// Whether this room's media must be encrypted. Unknown fails closed to
    /// true, so the failure mode is a refused call, never a cleartext one.
    /// Asks the resolver rather than relying on callers having pushed a value.
    bool roomEncrypted(const QString &roomId) const;

Q_SIGNALS:
    /// One room's observed session changed (or was read for the first time).
    void sessionChanged(const QString &roomId);
    /// Transport availability changed.
    void availabilityChanged();

private Q_SLOTS:
    void onSessionReceived(quint64 opId, const RtcSessionData &session);
    void onSessionPoked(const QString &roomId);
    void onTransportsReceived(quint64 opId, bool serverAnswered,
                              const QString &category,
                              const QStringList &serverServiceUrls,
                              const QString &participantFocusUrl);

private:
    void clearForNewSession();
    void flushPokes();
    void reapStaleReads();
    /// True when some transport is reachable for this room: its session's
    /// focus, the homeserver's answer, or a focus its participants advertise.
    bool transportReachableFor(const QString &roomId) const;

    QPointer<MatrixClient> m_client;
    QHash<QString, RtcSessionData> m_sessions;
    /// Logs an SFU identity that stays unresolvable (see the definition).
    /// Const because the lookup that discovers one is const.
    void noteUnresolvedIdentity(const QString &identity,
                                const QString &reason) const;
public:
    /// Reset the per-call diagnostic state.
    void forgetUnresolvedIdentityDiagnostics();
    /// Test-only: how long an identity must stay unresolvable before it is
    /// reported.
    void setUnresolvedIdentityGraceMsForTest(qint64 ms)
    {
        m_unresolvedIdentityGraceMs = ms;
    }
    /// Test-only: minimum gap between two server-backed reads of one room.
    void setServerReadCooldownMsForTest(int ms)
    {
        m_serverReadCooldownMs = ms;
    }

private:
    mutable QSet<QString> m_unresolvedIdentitiesLogged;
    /// When each unresolved subject was first seen, so a transient miss is not
    /// reported as permanent.
    mutable QHash<QString, qint64> m_unresolvedIdentityFirstSeenMs;
    /// Longer than a join's own settling, far shorter than a user's patience.
    qint64 m_unresolvedIdentityGraceMs = 5000;

    // Reads in flight. `m_roomsBeingRead` stops a poke burst dispatching
    // several reads for one room.
    struct PendingRead {
        QString roomId;
        /// Dispatch time. The Rust event queue drops the oldest event on
        /// overflow, so a reply may never arrive; without a timeout the room
        /// would stay unrefreshable and the poke timer would spin.
        qint64 dispatchedAtMs = 0;
    };
    QHash<quint64, PendingRead> m_pendingReads;
    QSet<QString> m_roomsBeingRead;

    // Server-backed reads. `m_serverReadWanted` survives a read already in
    // flight (a store-backed one cannot answer this question), so the request
    // carries to the next dispatch.
    QSet<QString> m_serverReadWanted;
    QHash<QString, qint64> m_lastServerReadMs;
    /// Consecutive forced reads that changed nothing, per room. The cooldown
    /// doubles with it, so an unnameable participant costs a few requests an
    /// hour rather than one every ten seconds. Reset when a read changes the
    /// answer.
    QHash<QString, int> m_serverReadStreak;
    /// One request per participant burst, yet an unnamed peer is retried
    /// within one refresh cycle.
    int m_serverReadCooldownMs = 10000;
    /// The ceiling the doubling stops at.
    int m_serverReadCooldownMaxMs = 300000;

    // Coalesced pokes.
    QSet<QString> m_pokedRooms;
    QTimer m_pokeTimer;
    int m_pokeCoalesceMs = 250;
    /// How long a dispatched read may stay outstanding before its room is
    /// released.
    int m_readTimeoutMs = 30000;

    // Discovery result. `m_discovered` separates "not looked yet" from
    // "looked and found nothing".
    bool m_discovered = false;
    bool m_serverAnswered = false;
    /// Account-scoped: the homeserver's answer applies to every room.
    QStringList m_serviceUrls;
    /// Room-scoped: a focus advertised by one room's participants says
    /// nothing about another room.
    QHash<QString, QString> m_participantFocus;
    QString m_discoveryRoomId;
    QString m_availabilityCategory;
    quint64 m_discoveryOp = 0;
    /// False until an owner says otherwise.
    bool m_mediaEncryption = false;
    bool m_mediaAvailable = false;
    // Mutable because the const roomEncrypted() remembers a resolver Yes, so
    // the irreversibility guard covers every room seen encrypted.
    mutable QHash<QString, RoomEncryption> m_encryptedRooms;
    EncryptionResolver m_encryptionResolver;
    QHash<QString, bool> m_canPublishMembership;

};
