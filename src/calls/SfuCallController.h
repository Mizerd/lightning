// The MatrixRTC group-call lifecycle (phase 2).
//
// One object binds the three halves that must agree for a call to work:
//
//   Matrix   — publish our membership so other clients see us, refresh it,
//              and retract it on leave.
//   SFU      — authorize, connect, and negotiate two peer connections.
//   Media    — SfuMediaEngine, which owns the actual RTP.
//
// The ORDER matters and is the main thing this class exists to get right:
//
//   1. Discover a focus (phase 1's RtcController).
//   2. Publish membership FIRST, carrying that focus. Other clients pick
//      their SFU from the oldest membership, so ours has to be on the wire
//      before we start expecting anyone to meet us there.
//   3. Connect to the SFU and negotiate.
//   4. Only then publish tracks.
//
// Leaving runs in reverse, and every step is idempotent, because the leave
// path is also the failure path: anything that goes wrong mid-join has to be
// able to unwind from wherever it got to.
//
// SAFETY, and the reason this class refuses more than it accepts:
//
//   * An ENCRYPTED room whose call media cannot be encrypted is refused.
//     Joining would carry audio the SFU can read, in a room the user was
//     told is end-to-end encrypted. §6 requires failing safely and saying
//     so, never silently downgrading.
//   * One call at a time, globally. A second join tears the first down
//     explicitly rather than leaving two engines holding the microphone.
//   * Every asynchronous reply is checked against a join generation, so a
//     late answer from a call the user already left cannot resurrect it.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class MatrixClient;
class RtcController;
class ScreenCastPortal;
class SfuVideoRouter;
class SfuMediaEngine;

class SfuCallController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("SfuCallController is exposed via app.groupCall")

    Q_PROPERTY(int state READ stateInt NOTIFY stateChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(bool microphoneMuted READ microphoneMuted
                   NOTIFY mediaStateChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY mediaStateChanged)
    Q_PROPERTY(bool cameraOn READ cameraOn NOTIFY mediaStateChanged)
    Q_PROPERTY(bool screenSharing READ screenSharing NOTIFY mediaStateChanged)
    Q_PROPERTY(bool handRaised READ handRaised NOTIFY mediaStateChanged)
    Q_PROPERTY(bool mediaEncrypted READ mediaEncrypted NOTIFY mediaStateChanged)
    Q_PROPERTY(int participantCount READ participantCount
                   NOTIFY participantsChanged)

public:
    /// The call lifecycle, as the UI needs to distinguish it.
    enum class State {
        Idle,
        /// Publishing membership; nothing is connected yet.
        Preparing,
        /// Membership is out; obtaining SFU authorization.
        Authorizing,
        /// Signalling is up; peer connections negotiating.
        Connecting,
        /// Media is flowing.
        Connected,
        /// Lost the SFU and retrying.
        Reconnecting,
        /// Left cleanly.
        Ended,
        /// Terminal failure; `lastError` says why.
        Failed,
    };
    Q_ENUM(State)

    explicit SfuCallController(QObject *parent = nullptr);
    ~SfuCallController() override;

    void setClient(MatrixClient *client);
    void setRtcController(RtcController *rtc);
    /// Not owned. Absent means calling refuses honestly rather than
    /// pretending — the same discipline as the 1:1 lane's media seam.
    void setMediaEngine(SfuMediaEngine *engine);
    /// Not owned. Absent means screen sharing refuses honestly.
    void setScreenCastPortal(ScreenCastPortal *portal);

    /// Attach a QML `VideoOutput`'s sink to one participant's video.
    ///
    /// Keyed on the SFU participant `identity` the stage already has, NOT on
    /// (userId, deviceId). The participant rows derive those two by
    /// splitting the identity on ':', which is right for the legacy
    /// `@user:server:DEVICE` form and GARBAGE for the sticky form, whose
    /// identity is a sha256 — so a modern Element participant would resolve
    /// to nothing and simply never show video. The identity is authoritative
    /// in both formats.
    ///
    /// An identity we cannot resolve to a stream routes nothing at all: a
    /// guess would put one participant's frames in another's tile.
    Q_INVOKABLE void attachVideoSink(const QString &identity,
                                     QObject *videoSink);
    /// Release a tile's sink. Must be called when a tile is destroyed, or
    /// the router keeps a dangling destination for one frame.
    Q_INVOKABLE void detachVideoSink(const QString &identity);
    /// Attach a sink to one participant's SCREEN SHARE, which is a second,
    /// separate video track from the same person.
    ///
    /// This needs its own entry point precisely because it is a second track:
    /// routing keyed on the participant alone can only ever feed ONE surface,
    /// so a camera and a share from the same person landed on the same key and
    /// only one of them could render. Resolution goes through the track's
    /// media-section id (`mid`), which LiveKit states per track.
    Q_INVOKABLE void attachScreenSink(const QString &identity,
                                      QObject *videoSink);
    Q_INVOKABLE void detachScreenSink(const QString &identity);
    /// Attach a sink to OUR OWN screen share, straight off the capture
    /// pipeline. Nothing is sent for this and nothing is decrypted: it is the
    /// only way the sharer can see that their share is carrying pixels.
    Q_INVOKABLE void attachLocalScreenSink(QObject *videoSink);
    Q_INVOKABLE void detachLocalScreenSink();

    State state() const { return m_state; }
    int stateInt() const { return static_cast<int>(m_state); }
    QString roomId() const { return m_roomId; }
    bool active() const;
    QString lastError() const { return m_lastError; }
    bool microphoneMuted() const { return m_micMuted; }
    bool deafened() const { return m_deafened; }
    bool cameraOn() const { return m_cameraOn; }
    bool screenSharing() const { return m_screenSharing; }
    bool handRaised() const { return m_handRaised; }
    /// True only when every frame we publish is encrypted. Never optimistic.
    bool mediaEncrypted() const { return m_mediaEncrypted; }
    int participantCount() const { return m_participants.size(); }

    /// Join the room's call. Refuses (and says why through `lastError`) when
    /// the room is encrypted and media E2EE is unavailable, when no focus is
    /// known, or when there is no media engine.
    Q_INVOKABLE bool join(const QString &roomId, bool withVideo = false);
    /// Leave. Safe to call in any state, including mid-join.
    Q_INVOKABLE void leave();

    Q_INVOKABLE void setMicrophoneMuted(bool muted);
    Q_INVOKABLE void toggleMicrophoneMuted();
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void toggleDeafened();
    Q_INVOKABLE void setCameraOn(bool on);
    Q_INVOKABLE void toggleCamera();
    /// Ask the desktop portal for a source and start sharing what the user
    /// picks. This is the entry point the UI uses: the portal owns the
    /// picker, so Lightning never enumerates windows itself.
    Q_INVOKABLE void requestScreenShare();
    /// Start sharing a PipeWire node the portal already granted. A negative
    /// id is REFUSED rather than defaulted — "whatever PipeWire feels like"
    /// is how you publish the wrong monitor.
    /// Publish a screen share the desktop portal has already granted.
    ///
    /// `pipewireFd` is the descriptor from OpenPipeWireRemote and OWNERSHIP
    /// PASSES to the media engine on success; on any refusal the caller still
    /// owns it and must close it. -1 means "no remote", which only the test
    /// source path accepts.
    Q_INVOKABLE bool startScreenShare(int pipewireNodeId,
                                      int pipewireFd = -1);
    Q_INVOKABLE void stopScreenShare();
    Q_INVOKABLE void setHandRaised(bool raised);
    Q_INVOKABLE void toggleHandRaised();
    /// Local-only playback volume for one participant, 0..100.
    Q_INVOKABLE void setParticipantVolume(const QString &identity,
                                          int percent);

    /// Participants for the call stage: {identity, userId, deviceId,
    /// speaking, muted, cameraOn, screenSharing, local}.
    Q_INVOKABLE QVariantList participants() const;

Q_SIGNALS:
    void stateChanged();
    void mediaStateChanged();
    void participantsChanged();
    /// A user-facing failure, already reduced to plain wording.
    void callFailed(const QString &message);

private Q_SLOTS:
    void onMembershipPublished(quint64 opId, bool ok, const QString &category,
                               const QString &eventId,
                               const QString &delayId);
    void onSfuState(const QString &state, const QString &category);
    void onSfuJoined(const QString &identity,
                     const QVariantList &participants,
                     const QVariantList &iceServers);
    void onSfuParticipants(const QVariantList &participants);
    void onSfuSpeakers(const QVariantList &speakers);
    void onSfuRemoteDescription(const QString &kind, const QString &target,
                                const QString &sdp);
    void onSfuRemoteCandidate(const QString &target,
                              const QString &candidateInit);
    void onEngineLocalDescription(int target, const QString &kind,
                                  const QString &sdp);
    void onEngineLocalCandidate(int target, const QString &candidateInit);
    void onEngineFailed(const QString &category);
    void onMediaKeyReceived(const QString &roomId, const QString &sender,
                            const QString &claimedDeviceId, int keyIndex,
                            const QString &keyBase64);
    void refreshMembership();

private:
    void setState(State state, const QString &error = QString());
    void teardown(State finalState, const QString &error = QString());
    void publishTracks();
    void applyAudioState();
    /// The LiveKit stream id (participant sid) one Matrix device is sending
    /// under, or empty if we cannot attribute it.
    ///
    /// Two hops, and both are needed: the MatrixRTC membership gives the
    /// device's SFU IDENTITY, and the SFU's own participant list gives the
    /// SID that appears in the SDP's `msid`. Empty means "do not guess" —
    /// installing a key under the wrong stream id would decrypt one
    /// participant's frames with another's key, which is silent corruption
    /// rather than an honest drop.
    QString streamIdForSender(const QString &userId,
                              const QString &deviceId) const;
    /// The LiveKit stream id (participant sid) for one SFU identity.
    QString streamIdForIdentity(const QString &identity) const;
    /// The routing key for one participant's track of `source`
    /// ("camera" / "screen_share"): the track's `mid` when the SFU stated
    /// one, else empty. Never the participant sid — that is the caller's
    /// fallback to apply deliberately, not a silent substitution that could
    /// point a screen-share surface at a camera.
    QString trackKeyForSource(const QString &identity,
                              const QString &source) const;

    /// Rotate and redistribute the media key. Called on join and whenever
    /// the participant set changes, because a leaver must not keep being
    /// able to decrypt.
    void rotateAndDistributeKey();
    /// Unpublish `cid` and clear it. Takes the member by reference so the
    /// slot cannot be left naming a track that no longer exists.
    void unpublishTrack(QString &cid);
    /// Our own row in the SFU participant list, or an empty map.
    QVariantMap ownParticipantRow() const;
    /// Tell the SFU whether our microphone track is muted, so every other
    /// client's mic indicator matches ours. Idempotent: it compares against
    /// the state the server currently reports and sends only on a difference.
    void syncMicMuteToSfu();
    QString userFacingError(const QString &category) const;

    QPointer<MatrixClient> m_client;
    QPointer<RtcController> m_rtc;
    /// Owned. Created eagerly because a tile can attach before any media
    /// exists, and dropping those attachments would mean the first frames of
    /// every call go nowhere.
    SfuVideoRouter *m_videoRouter = nullptr;
    QPointer<SfuMediaEngine> m_engine;
    QPointer<ScreenCastPortal> m_portal;

    State m_state = State::Idle;
    QString m_roomId;
    QString m_lastError;
    QString m_focusUrl;
    QString m_membershipEventId;
    QString m_delayId;
    QString m_ownIdentity;
    bool m_withVideo = false;

    bool m_micMuted = false;
    bool m_deafened = false;
    bool m_micMutedBeforeDeafen = false;
    bool m_cameraOn = false;
    bool m_screenSharing = false;
    bool m_handRaised = false;
    bool m_mediaEncrypted = false;
    /// Whether the ROOM is encrypted, so call media must be too. Captured at
    /// join from the tri-state the client reports, and UNKNOWN fails closed
    /// to true — a call in a room we cannot prove is unencrypted encrypts.
    bool m_roomEncrypted = true;

    /// Bumped on every join/leave. Every async reply carries the generation
    /// it was dispatched under; a mismatch is dropped.
    quint64 m_generation = 0;
    quint64 m_publishOp = 0;

    QVariantList m_participants;
    QHash<QString, bool> m_speaking;

    /// Membership must be refreshed before it expires, and the delayed
    /// retraction restarted, or the server cleans us out mid-call.
    QTimer m_refreshTimer;
    /// Track ids we published, so leave can unpublish them.
    QStringList m_publishedTrackIds;
    /// The published track id PER KIND. One list plus "unpublish the last
    /// one" cannot express this: with a camera and a screen share live at
    /// once, stopping either one took whichever was published second.
    QString m_audioCid;
    QString m_cameraCid;
    QString m_screenCid;
    int m_keyIndex = 0;
    /// Local ICE candidates produced this session. Diagnostic only — zero on
    /// the publisher means the peer connection never started, which is what
    /// LiveKit's 60 s JOIN_FAILURE timeout is reporting.
    int m_candidatesSent = 0;
};
