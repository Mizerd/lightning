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
// LEAVING IS THE HARD HALF, and it is the half this class got wrong for a
// long time. Three separate mechanisms have to agree, and each covers a
// failure the others cannot:
//
//   * The RETRACTION we send on a clean leave. It is a network request like
//     any other and it can fail — being offline at the moment of hang-up is
//     the ordinary case, not an exotic one. Its answer is now observed and a
//     transient failure is retried, bounded; before that it was
//     fire-and-forget with literally nothing connected to the result signal.
//   * The MSC4140 DELAYED RETRACTION the server holds on our behalf. This is
//     the only cleanup that survives a kill, a crash or a lost network,
//     because those run no code of ours. It is restarted on a heartbeat, and
//     a restart that FAILS is repaired by re-publishing (which arms a fresh
//     one) rather than by restarting an id the server may already have
//     consumed.
//   * The membership's own `expires`. On a homeserver without MSC4140 — which
//     is the default Synapse configuration — this is the ONLY cleanup that
//     exists at all. Rust therefore publishes a short one in that case, and
//     this class re-publishes the state event on a cadence so a LIVE
//     participant never ages out of it. Neither half is optional: a short
//     expiry without the re-publish would start removing people mid-sentence.
//
// A membership that never retracts does not merely look untidy. It poisons
// the room for every other client: element-call addresses its media keys per
// device, so a ghost device is sent keys nobody can use, and a member with a
// membership and no media is what "waiting for media" means.
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

// INCLUDED, not forward-declared. These three are Q_PROPERTY types, and
// moc compiles moc_SfuCallController.cpp as its own translation unit that
// sees this header and nothing else — a pointer property whose type is
// incomplete there needs a complete QMetaType and fails in ways that depend
// on the Qt version (§16's QPointer-of-incomplete-type trap is the same
// family, and it cost a release pipeline).
#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"

class MatrixClient;
class RtcController;
class ScreenCastPortal;
class SettingsManager;
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
    /// NOTIFY is the MODEL's own countChanged, forwarded, and not
    /// `participantsChanged`. This reader now answers out of
    /// `CallParticipantModel::rowCount()`, and the model is rebuilt from
    /// paths that do not all emit `participantsChanged` (`onSfuJoined`, and
    /// every `mediaStateChanged`) — so a surface binding this saw a stale
    /// count. Forwarding the model's own signal cannot go stale and cannot
    /// storm: the model emits it only when the row count really changed.
    Q_PROPERTY(int participantCount READ participantCount
                   NOTIFY participantCountChanged)
    /// THE call's people. CONSTANT because the object lives as long as the
    /// controller does — it is emptied on leave, never replaced, so a view
    /// bound to it is never re-bound and never reset.
    Q_PROPERTY(CallParticipantModel *participantModel READ participantModel
                   CONSTANT)
    /// ONE ROW PER ACTIVE SCREEN SHARE. N simultaneous sharers are N rows;
    /// nothing about the wire changes to support that.
    Q_PROPERTY(CallShareModel *shareModel READ shareModel CONSTANT)
    /// Call-scoped VIEW state (pin, dismissed shares, layout preference).
    /// Here rather than in the QML component because a room switch destroys
    /// the component and this belongs to the call.
    Q_PROPERTY(CallStageState *stageState READ stageState CONSTANT)

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
    /// Not owned. WHERE THE VOLUMES LIVE.
    ///
    /// Absent means volumes still WORK for the duration of a call — the
    /// engine and the model are told, the slider moves — but nothing is
    /// remembered, which is an honest degradation rather than a dead control.
    /// The maintainer asked for the opposite of that ("make the value saved so
    /// if a user A sets user B volume to 70% it stays the same in next call or
    /// other room"), so a build without this seam wired is a wiring bug, not a
    /// supported mode.
    void setSettings(SettingsManager *settings);

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
    /// Attach a sink to OUR OWN CAMERA, straight off the capture pipeline.
    /// Our camera is published, never received, so this self-view is the only
    /// local camera video there is — without it a local tile can only show an
    /// avatar while the capture light is on.
    Q_INVOKABLE void attachLocalCameraSink(QObject *videoSink);
    /// Attach a sink to OUR OWN screen share, straight off the capture
    /// pipeline. Nothing is sent for this and nothing is decrypted: it is the
    /// only way the sharer can see that their share is carrying pixels.
    Q_INVOKABLE void attachLocalScreenSink(QObject *videoSink);

    /// ONE detach, and it names the SINK rather than a key. This is the whole
    /// correction of 2026-08-27 and it replaced four per-key detaches
    /// (`detachVideoSink`/`detachScreenSink`/`detachLocalCameraSink`/
    /// `detachLocalScreenSink`), which are gone rather than deprecated.
    ///
    /// Two things were wrong with naming a key here, and both bit:
    ///
    ///  * The key was DERIVED at call time — `trackKeyForSource()` reads the
    ///    live participant list, and `local`/`mediaKind` are tile properties
    ///    that can change — so a tile could compute a different key at
    ///    destruction than it did at creation and release the wrong one.
    ///  * Far worse, a key-named release removed whatever was there. Qt
    ///    destroys a replaced surface AFTER building its replacement
    ///    (deleteLater vs. synchronous create), so on every layout swap and
    ///    every Repeater regenerate the dying tile unhooked the living one
    ///    and the video never came back.
    ///
    /// A sink cannot be named wrongly: a surface releases exactly what it
    /// holds. A null or non-QVideoSink argument is a NO-OP, deliberately —
    /// the old code treated "no sink" as "remove the key", which is the same
    /// defect wearing a different hat.
    Q_INVOKABLE void detachSink(QObject *videoSink);

    /// Diagnostic: is anything currently watching this routing key?
    ///
    /// Exists so a test can assert the ROUTER's state across a layout change
    /// rather than a QML property, which is the whole failure — a tile can
    /// report "attached" while the router disagrees. §16 records twice what a
    /// test that never reaches production is worth.
    Q_INVOKABLE bool isRoutingVideoTo(const QString &streamId) const;

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
    /// Read from the MODEL, not from the raw SFU list, so the count and the
    /// tiles can never disagree — the model is the one derivation.
    int participantCount() const;
    CallParticipantModel *participantModel() const
    {
        return m_participantModel;
    }
    CallShareModel *shareModel() const { return m_shareModel; }
    CallStageState *stageState() const { return m_stageState; }

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
    /// Local-only playback volume for one participant, 0..200.
    ///
    /// 200 rather than 100 because amplification was the request. Nothing is
    /// sent to anyone: it changes one `volume` element in OUR receive chain.
    ///
    /// The value is PERSISTED against the person's MATRIX USER ID, which is
    /// resolved from the identity through the membership — never by splitting
    /// the identity on ':'. An identity is per-device in the legacy format and
    /// an unpadded-base64 sha256 in the sticky one, so keying storage on it
    /// would forget the setting the moment the same person rejoined.
    Q_INVOKABLE void setParticipantVolume(const QString &identity,
                                          int percent);
    /// The stored volume for one participant, 0..200, or 100 when nothing is
    /// stored (or nothing can be resolved). Exists so a slider can be BORN at
    /// the right value rather than jumping there after the first write.
    Q_INVOKABLE int participantVolume(const QString &identity) const;

    /// Participants for the call stage, in the shape callers already expect.
    ///
    /// KEPT because other surfaces (the speaker bubbles, the banner facepile)
    /// still read it — but it is now READ OUT OF THE MODEL rather than
    /// rebuilt from the SFU list, so there is exactly one derivation and the
    /// two can never drift. New surfaces should bind `participantModel`.
    Q_INVOKABLE QVariantList participants() const;

    // --- TEST SEAMS -------------------------------------------------------
    //
    // The stage could not be instantiated in a test at all: every existing
    // CallStage assertion is a source scan, because driving the real surface
    // needs participants and participants needed a live SFU. These inject
    // exactly the payloads the SFU slots receive, through exactly the same
    // private merge helpers, WITHOUT the `active()` gate the slots keep — so
    // production behaviour is unchanged and a test does not have to fake a
    // call lifecycle to get a populated model.

    /// Inject a LiveKit ParticipantUpdate payload (a DELTA, merged by
    /// identity, `state: "disconnected"` removes).
    void ingestParticipantsForTest(const QVariantList &updates);
    /// Inject a SpeakersChanged payload: [{sid, active, level}].
    void ingestSpeakersForTest(const QVariantList &speakers);
    /// Inject a ConnectionQuality payload: [{sid, quality}].
    void ingestConnectionQualityForTest(const QVariantList &updates);
    /// Name the local device's SFU identity, as onSfuJoined would.
    void setOwnIdentityForTest(const QString &identity);
    /// Put the controller in a call STATE, so the `active()` gates that guard
    /// every wire-touching path behave as they do in a real call.
    void setCallStateForTest(State state);
    /// Name the room this call is in and the MSC4140 delay id the server gave
    /// us, as a successful `rtcPublishMembership` answer would.
    ///
    /// An EMPTY delay id is the important case, not a degenerate one: it is
    /// what a homeserver without MSC4140 produces — Synapse's default — and
    /// it is the whole of the maintainer's report. Without this seam neither
    /// the leave path nor the refresh heartbeat could be reached at all,
    /// which is exactly why neither had ever been tested.
    void setMembershipForTest(const QString &roomId, const QString &delayId);
    /// Drive the local device's camera / screen-share INTENT the way the
    /// buttons do, minus the media engine.
    ///
    /// A CONSCIOUS extension of the seam, not a convenience: `setCameraOn`
    /// and `startScreenShare`/`stopScreenShare` publish through
    /// SfuMediaEngine, which needs a real GStreamer pipeline — so the one
    /// behaviour that matters here (a local STOP must clear the local row and
    /// must reach the SFU, even while the server still reports the track
    /// live) had no way to be exercised at any layer. This routes through the
    /// same private applyVideoState() production does.
    void setLocalMediaStateForTest(bool cameraOn, bool screenSharing);

Q_SIGNALS:
    void stateChanged();
    void mediaStateChanged();
    void participantsChanged();
    /// Forwarded from CallParticipantModel::countChanged. See the property.
    void participantCountChanged();
    /// A user-facing failure, already reduced to plain wording.
    void callFailed(const QString &message);

private Q_SLOTS:
    /// Our own raise/lower completed. A refusal puts the control back —
    /// nothing but our own row is ever optimistic.
    void onHandResult(quint64 opId, bool ok, bool raised,
                      const QString &category, const QString &eventId);
    /// Somebody's hand went up or down, from the sync loop.
    void onHandChanged(const QString &roomId, const QString &sender,
                       const QString &membershipEventId,
                       const QString &reactionEventId, bool raised);
    /// The join-time sweep over hands raised before we arrived.
    void onHandsReceived(quint64 opId, const QString &roomId,
                         const QVariantList &hands);
    void onMembershipPublished(quint64 opId, bool ok, const QString &category,
                               const QString &eventId,
                               const QString &delayId);
    void onSfuState(const QString &state, const QString &category);
    void onSfuJoined(const QString &identity,
                     const QVariantList &participants,
                     const QVariantList &iceServers);
    void onSfuParticipants(const QVariantList &updates);
    void onSfuSpeakers(const QVariantList &speakers);
    void onSfuConnectionQuality(const QVariantList &updates);
    void onSfuRemoteDescription(const QString &kind, const QString &target,
                                const QString &sdp);
    void onSfuRemoteCandidate(const QString &target,
                              const QString &candidateInit);
    void onEngineLocalDescription(int target, const QString &kind,
                                  const QString &sdp);
    void onEngineLocalCandidate(int target, const QString &candidateInit);
    void onEngineFailed(const QString &category);
    /// ONE track could not carry media. Turns that control back off and says
    /// so; the call is NOT ended. See SfuMediaEngine::publishFailed.
    void onEnginePublishFailed(const QString &cid, const QString &category);
    void onMediaKeyReceived(const QString &roomId, const QString &sender,
                            const QString &claimedDeviceId, int keyIndex,
                            const QString &keyBase64);
    /// The one answer for BOTH `rtc_membership_retracted` and
    /// `rtc_delayed_updated` — the bridge routes them onto one signal, and
    /// they are told apart by OP ID, which is the only thing that can tell
    /// them apart. Until this existed nothing in the whole application was
    /// connected to it: a retraction that failed failed SILENTLY, and a
    /// delayed-leave restart that 404'd was equally invisible.
    void onMembershipRetracted(quint64 opId, bool ok, const QString &category);
    void refreshMembership();
    /// Re-issue a retraction that failed transiently. Separate from the
    /// timer's slot so the retry is a real, bounded, observable path.
    void retryRetraction();

private:
    void setState(State state, const QString &error = QString());
    void teardown(State finalState, const QString &error = QString());
    /// Ask the server to remove our membership, and REMEMBER the attempt so
    /// its answer can be judged and retried. `teardown()` clears m_roomId, so
    /// the target is captured here rather than re-read later.
    void dispatchRetraction(const QString &roomId, const QString &delayId);
    /// Re-send the membership state event, which also arms a fresh delayed
    /// retraction. This is the ONLY refresh mechanism on a homeserver without
    /// MSC4140, and it is the repair for a delayed-leave restart that failed.
    void republishMembership();
    void publishTracks();
    void applyAudioState();
    /// The LiveKit stream id (participant sid) for one SFU identity.
    QString streamIdForIdentity(const QString &identity) const;
    /// The devices a media key should go to: the SFU's live participants,
    /// resolved to Matrix devices through the membership. The INTERSECTION,
    /// because a membership alone includes ghosts — devices that died without
    /// retracting and cannot receive anything.
    QString mediaKeyTargets() const;
    /// The name one sending DEVICE's media-key ring is stored under. Derived
    /// from the to-device sender, so it is knowable the moment a key arrives
    /// and never depends on the SFU or the membership having caught up.
    static QString mediaKeyRingName(const QString &userId,
                                    const QString &deviceId);
    /// Bind every resolvable (sid, sending device) pair in the engine, so a
    /// media key addressed to a Matrix device reaches the ring the arriving
    /// FRAMES consult. Re-run on every participant update and every key: a
    /// sid does not exist until the SFU announces the participant, which can
    /// be long after that participant's key arrived.
    void noteParticipantIdentities();
    /// The routing key for one participant's track of `source`
    /// ("camera" / "screen_share"): the TRACK's sid when the SFU stated one,
    /// else empty. Never the PARTICIPANT sid — that is where the camera
    /// lands, so substituting it would point a screen-share surface at a
    /// camera. (This comment used to say `mid`; it has been the track sid
    /// since the interop round, because a `mid` belongs to the publisher's
    /// connection and our subscriber transceiver is numbered independently.)
    QString trackKeyForSource(const QString &identity,
                              const QString &source) const;

    /// Merge one LiveKit ParticipantUpdate delta into `m_participants` and
    /// rebuild the models. Returns true when the identity SET changed, which
    /// is what must rotate the media key. Carries NO `active()` gate: the
    /// slot owns that, and the test seam deliberately bypasses it.
    bool mergeParticipants(const QVariantList &updates);
    /// Push the current SFU list through to `m_participantModel` and
    /// `m_shareModel` as a DIFF. The single derivation of both.
    void rebuildModels();
    /// The share rows implied by the current participants plus our own live
    /// share. Separated out so it is readable and so the local-share rule is
    /// in one place.
    void rebuildShareModel();

    /// Redistribute the media key IF the set of devices we can address has
    /// grown since the last distribution.
    ///
    /// The membership and the SFU participant list are independent feeds and
    /// nothing orders them, so a peer is routinely on the SFU before their
    /// membership has been read — and a key can only be addressed to a
    /// membership. Idempotent by comparison against `m_lastKeyTargets`.
    void distributeKeyIfNeeded();
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
    /// client's mic indicator matches ours. Goes BOTH ways, which only the
    /// microphone can: it is published once per call and never republished,
    /// so our row carries at most one of them. It compares against the state
    /// the server currently reports and sends only on a difference, so it
    /// converges and cannot loop.
    void syncMicMuteToSfu();
    /// The Matrix user id behind one SFU identity, or empty when the
    /// membership has not been read yet. Empty means UNKNOWN: nothing is
    /// stored under a guess, and nothing is read back under one either.
    QString userIdForIdentity(const QString &identity) const;
    /// Push every stored per-person volume into the engine and the model.
    /// Called whenever the participant set changes, because a row that has
    /// just appeared is born at unity and a person the user turned down two
    /// calls ago must not come back loud.
    void applyStoredVolumes();
    /// Mute every track of `source` ("camera" / "screen_share") that the SFU
    /// still reports as LIVE. MUTE ONLY — it never unmutes, deliberately.
    ///
    /// A stop is a mute on this wire rather than a removal, so the corpse of a
    /// stopped track stays listed; its sid is server-assigned and nothing maps
    /// it back to the cid we published, so an unmute could name the dead one
    /// and put a track producing no RTP back on the wire. Nothing is lost:
    /// video is published fresh every time and a fresh track is reported
    /// unmuted. See the definition.
    void muteOwnTrackIfLive(const QString &source);
    /// The video mirror of applyAudioState(): push our camera / screen-share
    /// intent to the SFU.
    ///
    /// This exists because NOTHING ELSE tells the server a video track ended.
    /// The client seam has only `sfuAddTrack` and `sfuMuteTrack`, and the
    /// Rust bridge sends no unpublish message of any kind, so stopping a
    /// share used to tear down the local pipeline and leave the SFU still
    /// listing an unmuted screen_share track for us — forwarded to everyone
    /// else, and read back by rebuildModels() as "still sharing".
    void applyVideoState();
    /// Hand one LOCAL self-view surface an empty frame, so it stops painting
    /// the last picture the capture gave it. See the definition.
    void clearLocalVideoSurface(const QString &streamId);
    QString userFacingError(const QString &category) const;

    QPointer<MatrixClient> m_client;
    QPointer<RtcController> m_rtc;
    /// Owned. Created eagerly because a tile can attach before any media
    /// exists, and dropping those attachments would mean the first frames of
    /// every call go nowhere.
    SfuVideoRouter *m_videoRouter = nullptr;
    QPointer<SfuMediaEngine> m_engine;
    QPointer<ScreenCastPortal> m_portal;
    QPointer<SettingsManager> m_settings;

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
    /// The `m.reaction` OUR raise produced. A hand can only be lowered by
    /// redacting the specific event that raised it, and this device is the
    /// only thing that knows which one that was.
    QString m_handReactionId;
    quint64 m_handOp = 0;
    /// reaction event id -> the SFU identity whose hand it is.
    ///
    /// A redaction names only what it removed — the reaction itself is gone —
    /// so this is the only way to answer "whose hand just went down". It is
    /// also what makes forwarding every redaction in every room cheap: one
    /// hash lookup rejects the ones that are not ours.
    QHash<QString, QString> m_handReactions;
    bool m_mediaEncrypted = false;
    /// Whether the ROOM is encrypted, so call media must be too. Captured at
    /// join from the tri-state the client reports, and UNKNOWN fails closed
    /// to true — a call in a room we cannot prove is unencrypted encrypts.
    bool m_roomEncrypted = true;

    /// Bumped on every join/leave. Every async reply carries the generation
    /// it was dispatched under; a mismatch is dropped.
    quint64 m_generation = 0;
    quint64 m_publishOp = 0;
    /// A membership RE-publish issued by the refresh heartbeat. Distinct from
    /// m_publishOp because its answer must not re-run the join sequence.
    quint64 m_refreshOp = 0;
    /// The in-flight delayed-leave restart, so its failure is actionable.
    quint64 m_delayedRestartOp = 0;
    /// The in-flight retraction, and everything needed to re-issue it. These
    /// deliberately OUTLIVE the call: teardown() clears m_roomId, and a
    /// retraction that has not been acknowledged is still owed to the room.
    quint64 m_retractOp = 0;
    QString m_retractRoomId;
    QString m_retractDelayId;
    int m_retractAttempts = 0;
    /// When the membership state event was last (re-)published, so the
    /// re-publish cadence is independent of the 5 s delayed-restart tick.
    qint64 m_lastPublishMs = 0;

    QVariantList m_participants;
    /// The LAST SpeakersChanged round, kept so a participant update can
    /// re-apply it to a row that has only just appeared.
    ///
    /// Both halves are kept. `level` (LiveKit's SpeakerInfo carries it,
    /// 0..1, loudest first) is what makes a volume-reactive ring possible at
    /// all — Discord's own protocol has no such field, its speaking payload
    /// being a bitmask. `active` is kept alongside it so an SFU that reports
    /// only the flag still lights a binary ring instead of a dead one. What
    /// is NOT done is inventing a level from the flag.
    QHash<QString, bool> m_speaking;
    QHash<QString, qreal> m_speakingLevel;
    QHash<QString, QString> m_connectionQuality;

    /// Owned. Created once and emptied on leave — never replaced, so a bound
    /// view is never re-bound.
    CallParticipantModel *m_participantModel = nullptr;
    CallShareModel *m_shareModel = nullptr;
    CallStageState *m_stageState = nullptr;

    /// Bumped every time WE start sharing, so each local share gets a
    /// distinct id (`local:<n>`). Our own share exists the moment the portal
    /// grants it, before the SFU has stated a track sid for it, so it cannot
    /// be keyed on the sid the way a remote share is — and reusing one id
    /// across a stop/start would let a dismissal from the first share
    /// silently suppress the second.
    quint64 m_localShareEpoch = 0;

    /// Membership must be refreshed before it expires, and the delayed
    /// retraction restarted, or the server cleans us out mid-call.
    QTimer m_refreshTimer;
    /// Bounded retry for a retraction the server did not accept. Leaving a
    /// call over a flaky connection is the ordinary case, and until this
    /// existed the only attempt was fire-and-forget.
    QTimer m_retractRetryTimer;
    /// Track ids we published, so leave can unpublish them.
    QStringList m_publishedTrackIds;
    /// The published track id PER KIND. One list plus "unpublish the last
    /// one" cannot express this: with a camera and a screen share live at
    /// once, stopping either one took whichever was published second.
    QString m_audioCid;
    QString m_cameraCid;
    QString m_screenCid;
    int m_keyIndex = 0;
    /// The addressable-device set the last media key actually reached. See
    /// distributeKeyIfNeeded(); an empty set is never recorded.
    QString m_lastKeyTargets;
    /// Local ICE candidates produced this session. Diagnostic only — zero on
    /// the publisher means the peer connection never started, which is what
    /// LiveKit's 60 s JOIN_FAILURE timeout is reporting.
    int m_candidatesSent = 0;
};
