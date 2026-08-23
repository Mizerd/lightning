// MatrixRTC (MSC4143) session observation and transport discovery.
//
// This controller answers two questions for the UI:
//
//   1. "Is there a call in this room, and who is in it?" — today that is
//      the room's call banner and its facepile. `participants()` exposes
//      the fuller per-device list for the phase-2 call stage; nothing
//      renders it yet.
//   2. "Could this account join one at all?"              — whether the
//      homeserver offers a MatrixRTC transport, and if not, WHY.
//
// It deliberately cannot join. Publishing membership without a media
// transport tells every other client in the room to open an SFU connection
// that can never complete — a lie on the wire, not a stub, and the same
// reason the legacy lane refuses to invite without an engine. So
// `joinBlockReason()` always has something to say today, and the banner
// renders it as an INLINE label rather than offering a dead button — a
// disabled control receives no hover in Qt Quick, so a tooltip could never
// have explained itself.
//
// Threading/lifecycle rules that matter here:
//
// * Cross-account isolation (§9) rests on three real mechanisms, none of
//   them a version counter:
//     1. `setClient()` disconnects the previous client, so a REPLACED
//        client's late reply cannot be delivered at all.
//     2. An account switch reuses the SAME client object
//        (`detachSession()` → `restoreSession()`), and detach emits
//        `loggedOut`, which drops every observed session and every pending
//        read here.
//     3. Op ids come from a monotonic per-client counter, so within one
//        client instance an id is never reused across accounts.
//   An epoch field was tried and removed: with (2) clearing the pending map
//   on every switch it could never fire, and a reply carries only an op id,
//   so it could not have discriminated a genuine collision anyway. Do not
//   re-add one without also carrying the account identity on the reply.
// * A membership change arrives as a payload-free poke and is answered by
//   RE-READING, so a remote change and our own take the same parse path.
//   Pokes are COALESCED: one Element joining a call rewrites one state
//   event, but a group filling up rewrites many in a burst, and a read per
//   event would be pure waste.
#pragma once

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
    // Registered so QML can compare enums symbolically instead of by magic
    // int (the PaginationController/CallController precedent). Never
    // creatable: app.rtc is the one instance.
    QML_ELEMENT
    QML_UNCREATABLE("RtcController is exposed via app.rtc")

    // Whether this backend speaks MatrixRTC at all (the mock and HTTP
    // backends do not).
    Q_PROPERTY(bool supported READ supported NOTIFY availabilityChanged)
    // True only when a transport is actually reachable. NOT "the server
    // might have one" — an unanswered discovery leaves this false.
    Q_PROPERTY(bool callingAvailable READ callingAvailable
                   NOTIFY availabilityChanged)
    // Closed-set category explaining an unavailable transport, for the
    // user-facing message. Empty when calling is available.
    Q_PROPERTY(QString availabilityCategory READ availabilityCategory
                   NOTIFY availabilityChanged)

public:
    /// Why joining is refused. A closed set so QML never renders a raw
    /// server string, and so "we have not looked yet" stays distinct from
    /// "we looked and there is nothing".
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
        /// Everything on the Matrix side is fine, but this build has no SFU
        /// media transport, so joining would publish a membership nobody
        /// can connect to.
        NoMediaTransport,
        /// The room is ENCRYPTED but call media E2EE is not active, so
        /// joining would carry audio and video the SFU could read. Refused
        /// rather than downgraded: §6 requires failing safely and saying so,
        /// never silently weakening encryption.
        MediaEncryptionUnavailable,
    };
    Q_ENUM(JoinBlock)

    explicit RtcController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    /// Account-level: could a call be joined ANYWHERE. Room-specific
    /// answers come from `joinBlock(roomId)`.
    bool callingAvailable() const;
    QString availabilityCategory() const { return m_availabilityCategory; }

    /// Re-read one room's session. Safe to call repeatedly; reads for the
    /// same room coalesce.
    Q_INVOKABLE void refresh(const QString &roomId);
    /// Run transport discovery. `roomId` may be empty.
    Q_INVOKABLE void discover(const QString &roomId);

    /// Number of participant DEVICES currently in the room's call. 0 means
    /// no call (or none observed yet).
    Q_INVOKABLE int participantCount(const QString &roomId) const;
    /// True when somebody is in the room's call and nothing closed it.
    Q_INVOKABLE bool hasLiveSession(const QString &roomId) const;
    /// True when one of the local user's own devices is in the call — which
    /// is how "you are in this call, from somewhere" is answered.
    Q_INVOKABLE bool ownUserInSession(const QString &roomId) const;
    /// True when THIS device is in the call.
    Q_INVOKABLE bool ownDeviceInSession(const QString &roomId) const;
    /// True when any participant declared a video intent. An intent, not a
    /// live camera: never render this as "their camera is on".
    Q_INVOKABLE bool hasVideoIntent(const QString &roomId) const;

    /// Participants for display, oldest-joined first. Each entry:
    /// {userId, deviceId, intent, ownUser, ownDevice, joinedAtMs}.
    /// Bounded by `max` (<= 0 means all).
    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          int max = -1) const;
    /// Distinct user ids in the call, oldest-joined first — for a facepile,
    /// where the same person on two devices must appear ONCE.
    Q_INVOKABLE QStringList participantUserIds(const QString &roomId,
                                               int max = -1) const;
    /// Same de-duplication, but carrying the room-resolved profile so a
    /// facepile can draw real avatars instead of initials:
    /// {userId, displayName, avatarMxc}.
    Q_INVOKABLE QVariantList participantFaces(const QString &roomId,
                                              int max = -1) const;

    /// Every OTHER device in the session, as the targets JSON the media-key
    /// send takes: `[{"user_id":…,"device_id":…}, …]`.
    ///
    /// NOT Q_INVOKABLE and deliberately not a property: device ids are
    /// compared, never rendered, and this is the one consumer that genuinely
    /// needs them (an Olm-encrypted to-device message is addressed per
    /// device). Our own device is excluded — we already hold our own key,
    /// and sending it to ourselves would be one more copy on the wire.
    QString mediaKeyTargetsJson(const QString &roomId) const;

    /// The focus the room's OWN session advertises: `select_focus` over its
    /// memberships, which is the oldest membership's first `foci_preferred`
    /// entry — the same rule the reference implementation applies.
    ///
    /// This is the only focus that exists on a homeserver without MSC4143,
    /// which is nearly all of them, so it is the PRIMARY source rather than
    /// a fallback. It also has to win over our own homeserver's advertised
    /// SFU when a session exists: joining a different SFU than everyone else
    /// is a call with nobody in it.
    QString sessionFocusFor(const QString &roomId) const;

    /// Whether the ACCOUNT-scoped discovery is worth running again. False
    /// while one is in flight and once the server has answered either way.
    /// The automatic room-change trigger consults this; an explicit
    /// `discover()` is always honoured.
    bool discoveryWorthRetrying() const;

    /// The SFU service URL to use for this room: the homeserver's own
    /// answer if it gave one, otherwise the focus this room's participants
    /// advertise. Empty means no transport is known, which is a real answer
    /// and not an error.
    Q_INVOKABLE QString focusUrlFor(const QString &roomId) const;

    /// Why joining this room's call is refused right now.
    Q_INVOKABLE JoinBlock joinBlock(const QString &roomId) const;
    /// The same answer as a stable, translatable-at-the-QML-layer token.
    Q_INVOKABLE QString joinBlockReason(const QString &roomId) const;

    /// Test seam: the poke coalescing window. Production uses the default.
    void setPokeCoalesceMsForTest(int ms);
    /// Test seam: how long an unanswered read holds its room.
    void setReadTimeoutMsForTest(int ms);

    /// Whether call MEDIA can be encrypted end to end on this build.
    ///
    /// Distinct from the room's own encryption: a Matrix-encrypted room
    /// hides message bodies, and says nothing about whether the SFU can
    /// read the audio. Defaults FALSE — a boolean that cannot say "unknown"
    /// must default to the safe answer.
    void setMediaEncryptionAvailable(bool available);
    /// Whether an SFU media engine exists in this build/run. Until it does,
    /// joining would publish a membership no peer could connect to, so the
    /// join block says exactly that.
    void setMediaAvailable(bool available);
    bool mediaAvailable() const { return m_mediaAvailable; }
    bool mediaEncryptionAvailable() const { return m_mediaEncryption; }
    /// Tell the controller which rooms are encrypted. Supplied by the owner
    /// rather than read here, so this class keeps no second opinion about
    /// room encryption state.
    void setRoomEncrypted(const QString &roomId, bool encrypted);
    /// The SFU identity one device uses in this room's session, or empty if
    /// that device is not a participant.
    ///
    /// NOT Q_INVOKABLE: this is a wire identifier, not something to render.
    /// It is DERIVED in Rust from the membership (a sha256 for the sticky
    /// format), never recomputed here — the identity must match what Element
    /// computes or the two clients disagree about which SFU participant is
    /// which Matrix device.
    QString rtcIdentityFor(const QString &roomId, const QString &userId,
                           const QString &deviceId) const;

    /// Whether this room's media must be encrypted. UNKNOWN fails CLOSED to
    /// true: a room we cannot prove is unencrypted is treated as encrypted,
    /// so the honest failure is a refused call, never a cleartext one.
    bool roomEncrypted(const QString &roomId) const
    { return m_encryptedRooms.value(roomId, true); }

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
    /// True when SOME transport is reachable for this room: the
    /// homeserver's own answer, or a focus this room's participants
    /// advertise.
    bool transportReachableFor(const QString &roomId) const;

    QPointer<MatrixClient> m_client;
    QHash<QString, RtcSessionData> m_sessions;

    // Reads in flight. Each carries the epoch it was dispatched under, so a
    // reply that outlives an account switch is dropped instead of writing
    // one account's participants into another's room (§9 generation
    // isolation). `m_roomsBeingRead` stops a poke burst dispatching N reads
    // for the same room.
    struct PendingRead {
        QString roomId;
        /// Dispatch time. The Rust event queue DROPS the oldest event on
        /// overflow, so a reply can legitimately never arrive; without a
        /// bound the room would stay in `m_roomsBeingRead` forever, making
        /// it permanently un-refreshable AND spinning the poke timer.
        /// (`requestTurnServersIfStale` and ThreadManager learned this
        /// same lesson.)
        qint64 dispatchedAtMs = 0;
    };
    QHash<quint64, PendingRead> m_pendingReads;
    QSet<QString> m_roomsBeingRead;

    // Coalesced pokes.
    QSet<QString> m_pokedRooms;
    QTimer m_pokeTimer;
    int m_pokeCoalesceMs = 250;
    /// How long a dispatched read may stay outstanding before the room is
    /// released and becomes retryable again.
    int m_readTimeoutMs = 30000;

    // Discovery result. `m_discovered` distinguishes "not looked yet" from
    // "looked and found nothing", which the UI must not conflate.
    bool m_discovered = false;
    bool m_serverAnswered = false;
    /// Account-scoped: the homeserver's own answer applies to every room.
    QStringList m_serviceUrls;
    /// Room-scoped, keyed by room id. A focus advertised by ROOM A's
    /// participants says nothing about room B, so it must never decide
    /// room B's availability or join-block wording.
    QHash<QString, QString> m_participantFocus;
    QString m_discoveryRoomId;
    QString m_availabilityCategory;
    quint64 m_discoveryOp = 0;
    /// False until an owner says otherwise: see setMediaEncryptionAvailable.
    bool m_mediaEncryption = false;
    bool m_mediaAvailable = false;
    QHash<QString, bool> m_encryptedRooms;

};
