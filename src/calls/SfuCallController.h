// The MatrixRTC group-call lifecycle. Binds the three halves that must agree:
//
//   Matrix: publish our membership, refresh it, retract it on leave.
//   SFU:    authorize, connect, negotiate two peer connections.
//   Media:  SfuMediaEngine, which owns the RTP.
//
// Join order: discover a focus (RtcController); publish the membership first,
// carrying it (others pick their SFU from the oldest membership); connect to
// the SFU and negotiate; only then publish tracks. Leaving runs in reverse and
// every step is idempotent, since leave is also the failure path.
//
// Leaving relies on three mechanisms, each covering what the others cannot:
//
//   * The retraction sent on a clean leave; its answer is observed and
//     transient failures are retried, bounded.
//   * The MSC4140 delayed retraction held by the server, the only cleanup
//     that survives a crash. Restarted on a heartbeat; a failed restart is
//     repaired by re-publishing.
//   * The membership's own `expires`. Without MSC4140 (Synapse's default)
//     Rust publishes a short one and this class re-publishes on a cadence so
//     live participants never age out.
//
// A membership that is never retracted poisons the room: media keys go to a
// ghost device, and others see a member "waiting for media".
//
// Safety:
//
//   * An encrypted room whose call media cannot be encrypted is refused,
//     never silently downgraded.
//   * One call at a time; a second join tears the first down explicitly.
//   * Every asynchronous reply is checked against a join generation.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// Included, not forward-declared: these are Q_PROPERTY types, and
// moc_SfuCallController.cpp sees only this header, so they must be complete.
#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/WindowCaptureSrc.h"
#endif

class MatrixClient;
class QScreen;
class RtcController;
class CameraPortal;
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
    /// Whether a new share will carry the computer's audio, and whether this
    /// machine can capture it. Separate so the preference survives a platform
    /// that cannot honour it.
    Q_PROPERTY(bool shareAudioEnabled READ shareAudioEnabled
                   WRITE setShareAudioEnabled NOTIFY mediaStateChanged)
    Q_PROPERTY(bool shareAudioSupported READ shareAudioSupported CONSTANT)
    /// Whether share audio excludes this call's own playback: true with
    /// per-application capture (Linux with PipeWire), false where only the
    /// output mix is available. The picker tells the user which they get.
    Q_PROPERTY(bool shareAudioExcludesOwnPlayback
                   READ shareAudioExcludesOwnPlayback CONSTANT)
    Q_PROPERTY(bool handRaised READ handRaised NOTIFY mediaStateChanged)
    Q_PROPERTY(bool mediaEncrypted READ mediaEncrypted NOTIFY mediaStateChanged)
    /// Whether any remote participant's frames are arriving undecryptable.
    Q_PROPERTY(bool remoteMediaBlocked READ remoteMediaBlocked
                   NOTIFY remoteMediaBlockedChanged)
    /// Our own capture is publishing silence. See
    /// SfuMediaEngine::localAudioSilent.
    Q_PROPERTY(bool microphoneSilent READ microphoneSilent
                   NOTIFY microphoneSilentChanged)
    /// NOTIFY is the model's own countChanged, forwarded: the model is rebuilt
    /// from paths that do not all emit participantsChanged.
    Q_PROPERTY(int participantCount READ participantCount
                   NOTIFY participantCountChanged)
    /// The call's participants. CONSTANT: emptied on leave, never replaced,
    /// so bound views are never reset.
    Q_PROPERTY(CallParticipantModel *participantModel READ participantModel
                   CONSTANT)
    /// One row per active screen share.
    Q_PROPERTY(CallShareModel *shareModel READ shareModel CONSTANT)
    /// Call-scoped view state (pin, dismissed shares, layout). Lives here
    /// because a room switch destroys the QML component.
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
    /// Not owned. Absent means calling refuses honestly.
    void setMediaEngine(SfuMediaEngine *engine);
    /// Not owned. Absent means screen sharing refuses honestly.
    void setScreenCastPortal(ScreenCastPortal *portal);
    /// Not owned. Absent means the camera takes the direct route, which is
    /// no camera at all inside a sandbox. Set by AppController only when a
    /// Camera portal answers on the session bus.
    void setCameraPortal(CameraPortal *portal);
    /// Not owned; persists per-person volumes. Without it volumes still work
    /// for the call but are not remembered, which is a wiring bug rather than
    /// a supported mode.
    void setSettings(SettingsManager *settings);

    /// Attach a QML `VideoOutput`'s sink to one participant's camera video.
    /// Keyed on the SFU participant `identity`, which is authoritative in both
    /// the legacy and the sticky (hashed) identity formats. An identity that
    /// cannot be resolved routes nothing rather than guessing.
    Q_INVOKABLE void attachVideoSink(const QString &identity,
                                     QObject *videoSink);
    /// Attach a sink to one participant's screen share, a separate track from
    /// their camera, resolved by its own track sid.
    Q_INVOKABLE void attachScreenSink(const QString &identity,
                                      QObject *videoSink);
    /// Attach a sink to our own camera's self-view (our camera is published,
    /// never received).
    Q_INVOKABLE void attachLocalCameraSink(QObject *videoSink);
    /// Attach a sink to our own screen share's self-view, so the sharer can
    /// see that pixels are being carried.
    Q_INVOKABLE void attachLocalScreenSink(QObject *videoSink);

    /// Release whatever route `videoSink` holds. Names the sink, not a key:
    /// keys are derived from live state and can differ at destruction, and Qt
    /// destroys a replaced surface after building its replacement, so a
    /// key-named release would unhook the new one. A null or non-QVideoSink
    /// argument is a no-op.
    Q_INVOKABLE void detachSink(QObject *videoSink);

    /// Diagnostic: is anything watching this routing key? Lets tests assert
    /// the router's state rather than a QML property.
    Q_INVOKABLE bool isRoutingVideoTo(const QString &streamId) const;

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    /// Development only: stage a call that exists only in this process, for
    /// screenshots on the mock backend. Publishes no membership, contacts no
    /// SFU and opens no device; fills the model with fictional people.
    /// Compiled out of release builds. endDemoCall() returns to Idle.
    void startDemoCall(const QString &roomId, bool withScreenShare);
    void endDemoCall();
#endif

    State state() const { return m_state; }
    int stateInt() const { return static_cast<int>(m_state); }
    QString roomId() const { return m_roomId; }
    bool active() const;
    QString lastError() const { return m_lastError; }
    bool microphoneMuted() const { return m_micMuted; }
    bool deafened() const { return m_deafened; }
    bool cameraOn() const { return m_cameraOn; }
    bool screenSharing() const { return m_screenSharing; }
    bool shareAudioEnabled() const { return m_shareAudioEnabled; }
    void setShareAudioEnabled(bool on);
    bool shareAudioSupported() const;
    bool shareAudioExcludesOwnPlayback() const;

    // Whether an engine failure concerns only the share's sound. Such a
    // failure must never end the session (see onEngineFailed).
    static bool categoryIsShareAudioOnly(const QString &category);
    bool handRaised() const { return m_handRaised; }
    /// True only when every frame we publish is encrypted. Never optimistic.
    bool mediaEncrypted() const { return m_mediaEncrypted; }
    bool remoteMediaBlocked() const { return !m_blockedStreams.isEmpty(); }
    bool microphoneSilent() const { return m_microphoneSilent; }
    /// Whether this participant's media is blocked, for a per-tile mark.
    Q_INVOKABLE bool mediaBlockedFor(const QString &identity) const;
    /// Read from the model, so the count and the tiles always agree.
    int participantCount() const;
    CallParticipantModel *participantModel() const
    {
        return m_participantModel;
    }
    CallShareModel *shareModel() const { return m_shareModel; }
    CallStageState *stageState() const { return m_stageState; }

    /// Join the room's call. Refuses (with `lastError`) when the room is
    /// encrypted and media E2EE is unavailable, when no focus is known, or
    /// without a media engine.
    Q_INVOKABLE bool join(const QString &roomId, bool withVideo = false);
    /// Leave. Safe to call in any state, including mid-join.
    Q_INVOKABLE void leave();

    Q_INVOKABLE void setMicrophoneMuted(bool muted);
    Q_INVOKABLE void toggleMicrophoneMuted();
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void toggleDeafened();
    Q_INVOKABLE void setCameraOn(bool on);
    Q_INVOKABLE void toggleCamera();
    /// Offer a source and share what the user picks; the one entry point on
    /// every platform. The desktop portal owns the picker where reachable (the
    /// only option on Wayland); on Windows, macOS and portal-less X11,
    /// Lightning draws its own over `screenShareSources`. See
    /// `LinuxShareRoute`.
    Q_INVOKABLE void requestScreenShare();
    /// Sources a share could capture where there is no portal. Empty on Linux
    /// when the portal is reachable. On portal-less X11 it lists displays only.
    ///
    /// Each entry: {index, name, geometry, primary, current}; `current` is
    /// the display the app is on.
    Q_PROPERTY(QVariantList screenShareSources READ screenShareSources
                   NOTIFY screenShareSourcesChanged)
    QVariantList screenShareSources() const { return m_screenShareSources; }
    /// Whether this build can list windows at all (Windows only), so the
    /// picker can tell "no windows open" from "not supported here".
    Q_PROPERTY(bool windowCaptureSupported READ windowCaptureSupported
                   CONSTANT)
    bool windowCaptureSupported() const
    {
#ifdef HAVE_LIGHTNING_WEBRTC
        return lightning::wincap::available();
#else
        // No media engine: WindowCaptureSrc.cpp is not built, so an inline
        // call would not link.
        return false;
#endif
    }
    /// Start the share on one of `screenShareSources`. Ignored when the list
    /// is empty (the portal already chose).
    Q_INVOKABLE void chooseScreenShareSource(int index);

    /// Where a Linux screen share gets its source. The order is the contract:
    /// the portal wins whenever present, and Wayland refuses before any X11
    /// clause.
    enum class LinuxShareRoute {
        /// xdg-desktop-portal: preferred everywhere and the only safe path on
        /// Wayland. The portal draws the picker.
        Portal,
        /// No portal, X11, capture element present: Lightning's own picker,
        /// whole displays only.
        FallbackDisplays,
        /// No portal on Wayland: nothing can capture; the fix is to install or
        /// start the portal.
        RefuseWaylandNeedsPortal,
        /// X11 without the capture element in the running registry; refused
        /// up front rather than at PLAYING.
        RefuseNoCaptureElement,
        /// Neither display server is reachable at all.
        RefuseNoDisplayServer,
    };

    /// Classify the session. Pure, with every input passed in (including the
    /// element probe), so it has no GStreamer dependency.
    ///
    /// Wayland is decided before X11 and catches XWayland: an `xcb` app in a
    /// Wayland session has a working DISPLAY whose root window is black. Any
    /// of the three signals is enough.
    static LinuxShareRoute linuxShareRoute(bool portalAvailable,
                                           const QString &platformName,
                                           const QString &sessionType,
                                           const QString &waylandDisplay,
                                           const QString &x11Display,
                                           bool captureElementPresent);

    /// The user-facing refusal for a route, or empty. Pure so the wording is
    /// tested. `sandboxed` changes only the missing-element advice: a Flatpak
    /// or Snap cannot use host GStreamer plugins, and the KDE runtime ships no
    /// `ximagesrc`.
    static QString linuxShareRefusal(LinuxShareRoute route,
                                     bool sandboxed = false);

    /// Where a Linux camera gets its pixels. No refusal state: `v4l2src` can
    /// always be attempted and reports its own failure.
    enum class LinuxCameraRoute {
        /// `v4l2src` on the device node; the default desktop route.
        Direct,
        /// A PipeWire remote from `org.freedesktop.portal.Camera`; required in
        /// a sandbox, which has no `/dev/video*`.
        Portal,
    };

    /// Choose the camera route. Pure, every input passed in. In order:
    ///
    ///  1. Sandboxed always takes the portal, even if it looks unusable:
    ///     Flathub allows no device access, and a portal error is actionable.
    ///  2. A visible device node keeps the direct route.
    ///  3. No device node and a usable portal: the portal.
    ///  4. Otherwise direct, including its honest failure.
    ///
    /// `portalUsable` folds `CameraPortal::available()` and
    /// `CameraPortal::cameraPresent()`: a permission dialog for a camera that
    /// does not exist is worse than the failure it replaces. Both are logged
    /// at the call site.
    static LinuxCameraRoute linuxCameraRoute(bool sandboxed, bool portalUsable,
                                             bool directDeviceVisible);

    /// Accepts a native, root-relative screen rectangle as an X11 capture
    /// region, or returns an invalid rect. No arithmetic (see
    /// nativeScreenRect()). ximagesrc's coordinates are unsigned, so a
    /// negative origin would wrap rather than fail.
    static QRect validX11CaptureRect(const QRect &nativeGeometry);

    /// One screen's native, root-relative rectangle (what `ximagesrc`
    /// addresses), or an invalid rect.
    ///
    /// Taken from the platform, never derived from QScreen::geometry(): Qt
    /// keeps the top-left in native pixels and scales only the size, and
    /// devicePixelRatio() is a rounded presentation value, so arithmetic
    /// lands inside a neighbouring monitor on mixed-DPI setups.
    /// `QScreen::handle()->geometry()` is exact (on XCB it comes from the
    /// XRandR CRTC).
    static QRect nativeScreenRect(const QScreen *screen);
    /// Abandon the picker without sharing.
    Q_INVOKABLE void cancelScreenShareSelection();
    /// Publish a screen share. Exactly one source kind applies:
    /// `pipewireNodeId` (a portal node; a display index off Linux),
    /// `windowHandle` (a Windows HWND for a single window), or `captureRect`
    /// (an X11 root-window rectangle, Linux no-portal fallback). Without any
    /// of them the share is refused rather than defaulted.
    ///
    /// `pipewireFd` is the OpenPipeWireRemote descriptor; ownership passes to
    /// the media engine on success, and on refusal the caller must close it.
    Q_INVOKABLE bool startScreenShare(int pipewireNodeId,
                                      int pipewireFd = -1,
                                      quint64 windowHandle = 0,
                                      const QRect &captureRect = {});
    Q_INVOKABLE void stopScreenShare();
    Q_INVOKABLE void setHandRaised(bool raised);
    Q_INVOKABLE void toggleHandRaised();
    /// Send one transient call reaction in element-call's format:
    /// `io.element.call.reaction` referencing this device's own
    /// `m.call.member` event by `m.reference`. `name` is element-call's key
    /// for the emoji; rust/src/rtc.rs checks the pair against its
    /// `ReactionSet`.
    ///
    /// Not optimistic: our reaction is drawn only when the event comes back,
    /// like a remote one, so a failed send never shows.
    Q_INVOKABLE void sendCallReaction(const QString &emoji,
                                      const QString &name);

    /// How long a reaction stays on a tile. element-call's
    /// `src/reactions/ReactionsReader.ts` sets
    /// `REACTION_ACTIVE_TIME_MS = 3000`; matching it keeps both clients in
    /// step. Also the window in which a second reaction from the same sender
    /// is dropped, and in which this device refuses to send another.
    static constexpr int kReactionActiveMs = 3000;
    /// Local-only playback volume for one participant, 0..200. Nothing is
    /// sent. Persisted against the person's Matrix user id, resolved through
    /// the membership, never by parsing the per-device identity.
    Q_INVOKABLE void setParticipantVolume(const QString &identity,
                                          int percent);
    /// The stored volume for one participant, or 100 when none can be
    /// resolved, so a slider starts at the right value.
    Q_INVOKABLE int participantVolume(const QString &identity) const;

    /// A screen share's own level, separate from its owner's microphone (a
    /// different track), addressed by share id.
    Q_INVOKABLE void setShareVolume(const QString &shareId, int percent);
    Q_INVOKABLE int shareVolume(const QString &shareId) const;
    /// Whether this share carries sound; the tile offers a volume only if so.
    Q_INVOKABLE bool shareHasAudio(const QString &shareId) const;

    /// Participants for the call stage as a list, read out of the model so
    /// there is one derivation. New surfaces should bind `participantModel`.
    Q_INVOKABLE QVariantList participants() const;

    // --- Test seams ---
    //
    // Inject exactly the SFU payloads the slots receive, through the same
    // private merge helpers but without the slots' `active()` gate, so a test
    // gets a populated model without faking a call lifecycle.

    /// Inject a LiveKit ParticipantUpdate payload (a DELTA, merged by
    /// identity, `state: "disconnected"` removes).
    void ingestParticipantsForTest(const QVariantList &updates);
    /// Inject a SpeakersChanged payload: [{sid, active, level}].
    void ingestSpeakersForTest(const QVariantList &speakers);
    /// Inject a ConnectionQuality payload: [{sid, quality}].
    void ingestConnectionQualityForTest(const QVariantList &updates);
    /// Name the local device's SFU identity, as onSfuJoined would.
    void setOwnIdentityForTest(const QString &identity);
    /// Whether joining with these room participants (RtcController rows)
    /// starts the call and must be announced: true when only this device's
    /// own stale membership is present.
    static bool startsCallForAnnouncement(const QVariantList &participants);
    /// How often the refresh tick reconciled the key lane. The reconciliation
    /// is compiled out without WebRTC; this counter is not, so tests can see
    /// the timer reach it.
    int keyLaneReconcilesForTest() const { return m_keyLaneReconciles; }
    /// What the media engine was last told for a participant (by identity)
    /// or a share (by share id), or -1 if never. Kept outside the media guard
    /// for tests built without an engine. participantVolume() and
    /// shareVolume() cannot substitute: they fall back to the store.
    int engineParticipantVolumeForTest(const QString &identity) const
    {
        return m_engineParticipantVolume.value(identity, -1);
    }
    int engineShareVolumeForTest(const QString &shareId) const
    {
        return m_engineShareVolume.value(shareId, -1);
    }
    /// Starts the real refresh timer at `ms`.
    void startRefreshTickForTest(int ms)
    {
        m_refreshTimer.setInterval(ms);
        m_refreshTimer.start();
    }
    /// Put the controller in a call state, so `active()` gates behave as in a
    /// real call.
    void setCallStateForTest(State state);
    /// Set the room and the MSC4140 delay id as a successful publish would.
    /// An empty delay id (a homeserver without MSC4140, Synapse's default) is
    /// the important case for the leave path and the refresh heartbeat.
    void setMembershipForTest(const QString &roomId, const QString &delayId);
    /// Diagnostic: why no delayed retraction is armed, or empty when one is.
    /// A closed category vocabulary, never server text.
    QString delayedRefusalReason() const { return m_delayedCategory; }
    /// Arm the share-audio cid a running share would hold, so the cleanup in
    /// onEngineFailed can be tested without a portal, engine or SFU.
    void setShareAudioCidForTest(const QString &cid)
    {
        m_shareAudioCid = cid;
        if (!cid.isEmpty() && !m_publishedTrackIds.contains(cid))
            m_publishedTrackIds.append(cid);
    }
    QString shareAudioCidForTest() const { return m_shareAudioCid; }
    /// Test-only: parked media keys waiting for the call to start.
    int parkedKeyCountForTest() const { return m_parkedKeys.size(); }
    bool hasParkedKeyForTest(const QString &sender) const
    {
        for (const ParkedKey &k : m_parkedKeys) {
            if (k.sender == sender)
                return true;
        }
        return false;
    }
    /// Put the controller where join() leaves it while the homeserver decides:
    /// Preparing, room and focus recorded, and a real rtcPublishMembership in
    /// flight (its op id is returned). Runs join()'s last steps, which are
    /// the only ones reachable without a media engine.
    quint64 beginMembershipPublishForTest(const QString &roomId,
                                          const QString &focusUrl);
    /// Shorten the reaction window so its expiry, duplicate rule and parked
    /// staleness run quickly; the real timer and clauses still run.
    void setReactionWindowMsForTest(int ms)
    {
        m_reactionWindowMs = ms > 0 ? ms : 1;
    }
    /// Drive the local camera/share intent as the buttons do, minus the media
    /// engine, through the same private applyVideoState().
    void setLocalMediaStateForTest(bool cameraOn, bool screenSharing);

Q_SIGNALS:
    void stateChanged();
    void mediaStateChanged();
    void remoteMediaBlockedChanged();
    void microphoneSilentChanged();
    void participantsChanged();
    /// Forwarded from CallParticipantModel::countChanged. See the property.
    void participantCountChanged();
    void shareVolumeChanged(const QString &shareId, int percent);
    /// The picker has something to show. Only where there is no portal.
    void screenShareSourcesChanged();
    void screenShareSourcesAvailable();
    /// A user-facing failure in plain wording. An empty message withdraws the
    /// previous one (a later attempt got past the gate that refused), the same
    /// idiom as AppController's `errorReported(QString{})`; receivers must
    /// clear on it.
    void callFailed(const QString &message);

private Q_SLOTS:
    /// Our own raise/lower completed; a refusal reverts the control.
    void onHandResult(quint64 opId, bool ok, bool raised,
                      const QString &category, const QString &eventId);
    /// Somebody's hand went up or down, from the sync loop.
    void onHandChanged(const QString &roomId, const QString &sender,
                       const QString &membershipEventId,
                       const QString &reactionEventId, bool raised);
    /// The join-time sweep over hands raised before we arrived.
    void onHandsReceived(quint64 opId, const QString &roomId,
                         const QVariantList &hands);
    /// A transient reaction from sync, attributed like a raise: through the
    /// membership it references, whose owner must be the sender.
    void onCallReactionReceived(const QString &roomId, const QString &sender,
                                const QString &membershipEventId,
                                const QString &emoji);
    /// The generic RTC send answer; only our own reaction op is matched.
    void onRtcSendFinished(quint64 opId, bool ok, const QString &category,
                           const QString &eventId);
    void onMembershipPublished(quint64 opId, bool ok, const QString &category,
                               const QString &eventId, const QString &delayId,
                               const QString &delayedCategory);
    void onSfuState(const QString &state, const QString &category);
    /// `sifTrailer` is LiveKit's `JoinResponse.sif_trailer`, already bounded
    /// by the bridge (empty when absent or unusable).
    void onSfuJoined(const QString &identity,
                     const QVariantList &participants,
                     const QVariantList &iceServers,
                     const QByteArray &sifTrailer = QByteArray());
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
    /// One track could not carry media: turn that control off and say so. The
    /// call is not ended. See SfuMediaEngine::publishFailed.
    void onEnginePublishFailed(const QString &cid, const QString &category);
    void onMediaKeyReceived(const QString &roomId, const QString &sender,
                            const QString &claimedDeviceId, int keyIndex,
                            const QString &keyBase64);
    /// Answers for both `rtc_membership_retracted` and `rtc_delayed_updated`,
    /// which the bridge routes onto one signal; the op id tells them apart.
    void onMembershipRetracted(quint64 opId, bool ok, const QString &category);
    void refreshMembership();
    /// Re-run key-lane reconciliation on the refresh tick. The only other
    /// trigger, RtcController::sessionChanged, is suppressed when a membership
    /// read is unchanged, so a failed distribution's retry would never run.
    /// matrix-js-sdk likewise reconciles on every recalculation. Idempotent.
    void reconcileKeyLane();
    /// Re-issue a transiently failed retraction.
    void retryRetraction();

private:
    /// Fill `m_screenShareSources` with displays for the Linux no-portal
    /// fallback. False when there is none to offer (a refusal).
    bool populateLinuxDisplaySources();
    /// The native root rectangle of the screen named `name`, or invalid if it
    /// is not connected now. Resolved by name at choice time, since an
    /// unplugged monitor renumbers the others.
    static QRect physicalRectForScreenNamed(const QString &name);

    void setState(State state, const QString &error = QString());
    void teardown(State finalState, const QString &error = QString());
    /// Ask the server to remove our membership and remember the attempt for
    /// retries. The room is captured here because teardown() clears m_roomId.
    void dispatchRetraction(const QString &roomId, const QString &delayId);
    /// Re-send the membership state event, which also arms a fresh delayed
    /// retraction. The only refresh without MSC4140, and the repair for a
    /// failed delayed-leave restart.
    void republishMembership();
    void publishTracks();
    void applyAudioState();
    /// The LiveKit stream id (participant sid) for one SFU identity.
    QString streamIdForIdentity(const QString &identity) const;
    /// Devices a media key should go to: the SFU's live participants resolved
    /// to Matrix devices through the membership. The intersection, because
    /// memberships alone include ghost devices.
    QString mediaKeyTargets() const;
    /// The name a sending device's key ring is stored under, derived from the
    /// to-device sender so it is known as soon as a key arrives.
    static QString mediaKeyRingName(const QString &userId,
                                    const QString &deviceId);
    /// Bind every resolvable (sid, sending device) pair in the engine, so a
    /// device-addressed key reaches the ring arriving frames consult. Re-run
    /// on every participant update and key.
    void noteParticipantIdentities();
    /// Mark a participant's hand up. Shared by the live handler and the
    /// pending retry so the own-hand handling cannot drift.
    void applyRaisedHand(const QString &reactionEventId,
                         const QString &identity);
    /// Show one transient reaction. Shared by the live handler and the pending
    /// retry.
    void applyCallReaction(const QString &identity, const QString &emoji);
    /// Re-attribute parked annotations (raises and reactions) once their
    /// membership arrives.
    void retryPendingAnnotations();
    /// The routing key for a participant's `source` track ("camera" /
    /// "screen_share"): the track sid if the SFU stated one, else empty. Never
    /// the participant sid, which is where the camera lands.
    QString trackKeyForSource(const QString &identity,
                              const QString &source) const;

    /// Merge one LiveKit ParticipantUpdate delta and rebuild the models.
    /// Returns true when the identity set changed (which rotates the key). No
    /// `active()` gate; the slot owns that.
    bool mergeParticipants(const QVariantList &updates);
    /// Log remote tracks' mute transitions, so SFU-injected frame bursts can
    /// be matched to a mute. Sids and a boolean only.
    void noteRemoteTrackMutes(const QVariantList &updates);
    /// Diff the SFU list into the participant and share models; the single
    /// derivation of both.
    void rebuildModels();
    /// Share rows from the current participants plus our own live share.
    void rebuildShareModel();

    /// Redistribute the media key if the addressable device set changed since
    /// the last distribution (membership and SFU list arrive independently).
    /// Idempotent via `m_lastKeyTargets`.
    void distributeKeyIfNeeded();
    /// Rotate and redistribute the media key: on join and whenever the
    /// participant set changes, so a leaver cannot keep decrypting.
    void rotateAndDistributeKey();
    /// Unpublish `cid` and clear it (by reference, so it cannot go stale).
    void unpublishTrack(QString &cid);
    /// Our own row in the SFU participant list, or an empty map.
    QVariantMap ownParticipantRow() const;
    /// Sync our microphone's mute state to the SFU in both directions, so
    /// other clients' indicators match. Compares with what the server reports
    /// and sends only on a difference, so it converges.
    void syncMicMuteToSfu();
    /// The Matrix user id behind an SFU identity, or empty (unknown) until the
    /// membership is read. Nothing is stored or read under a guess.
    QString userIdForIdentity(const QString &identity) const;
    /// Push stored per-person volumes into the engine and the model whenever
    /// the participant set changes; new rows start at unity.
    void applyStoredVolumes();
    void applyStoredShareVolumes();
    /// The one place a participant level reaches the audio graph, and where
    /// that is recorded. Returns whether the engine was told (false when the
    /// stream id is unknown or there is no engine).
    bool applyEngineParticipantVolume(const QString &identity, int percent);
    /// Share counterpart: addressed by track key as well as stream id; does
    /// nothing until both are known.
    void applyEngineShareVolume(const QString &shareId,
                                const QString &identity, int percent);
    /// Mute every track of `source` the SFU still reports live. Never unmutes;
    /// see the definition.
    void muteOwnTrackIfLive(const QString &source);
    /// Push our camera/share intent to the SFU. Nothing else tells the server
    /// a video track ended: the wire has only AddTrack and Mute.
    void applyVideoState();
    /// Begin capturing the camera by the route this machine needs. Direct is
    /// synchronous. The portal route sends an AccessCamera request and
    /// publishes in the `ready` handler; until then `m_cameraOn` is true with
    /// no track, tracked by `m_cameraAwaitingPortal`.
    void startCameraCapture();
    /// Declare and publish the camera track. `pipewireFd` is a portal
    /// descriptor or -1; ownership passes to SfuMediaEngine::publishVideo().
    void publishCameraTrack(int pipewireFd);
    /// Give up on a camera turned on but never published (declined or failed
    /// dialog, call ended): reset `cameraOn` and tell the SFU.
    void abandonPendingCamera();
    /// Hand a local self-view surface an empty frame; see the definition.
    void clearLocalVideoSurface(const QString &streamId);
    QString userFacingError(const QString &category) const;
    /// Plain wording for a `RtcController::joinBlockReason` token when join()
    /// is reached with a block standing.
    static QString joinRefusalMessage(const QString &block);

    QPointer<MatrixClient> m_client;
    QPointer<RtcController> m_rtc;
    /// Owned. Created eagerly because tiles can attach before any media
    /// exists.
    SfuVideoRouter *m_videoRouter = nullptr;
    QPointer<SfuMediaEngine> m_engine;
    QPointer<ScreenCastPortal> m_portal;
    QPointer<CameraPortal> m_cameraPortal;
    /// The camera is on but its portal grant has not arrived: `m_cameraOn` is
    /// true, but nothing is declared or published.
    bool m_cameraAwaitingPortal = false;
    QPointer<SettingsManager> m_settings;

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    /// A demo call is staged; rebuildModels() must leave the model alone.
    bool m_demoCall = false;
#endif
    State m_state = State::Idle;
    QString m_roomId;
    QString m_lastError;
    /// A user-facing failure has been announced and not yet withdrawn; see
    /// setState().
    bool m_failureAnnounced = false;
    QString m_focusUrl;
    QString m_membershipEventId;
    /// Per-share levels chosen during this call, so the slider reads back its
    /// position; cleared with the call. The persistent preference is stored
    /// per owner in settings.
    QHash<QString, int> m_shareVolumes;
    /// What reached the engine; see engineParticipantVolumeForTest(). Cleared
    /// with the call.
    QHash<QString, int> m_engineParticipantVolume;
    QHash<QString, int> m_engineShareVolume;
    /// Sampled before our membership publishes: were we first in? Only then
    /// is the call announced.
    bool m_announceOnPublish = false;
    /// "video" or "audio": the announcement's intent.
    QString m_announceIntent;
    QString m_delayId;
    /// Why `m_delayId` is empty, in rtc.rs's vocabulary; empty when armed.
    /// `unrecognized`/`not_found`/`no_delay_id` mean no usable MSC4140
    /// endpoint; anything else is transient or room-specific.
    QString m_delayedCategory;
    QString m_ownIdentity;
    /// Populated while the picker is open. On Linux only on the no-portal X11
    /// fallback.
    QVariantList m_screenShareSources;
    bool m_withVideo = false;

    bool m_micMuted = false;
    bool m_deafened = false;
    bool m_micMutedBeforeDeafen = false;
    bool m_cameraOn = false;
    bool m_screenSharing = false;
    /// Whether a new share carries the computer's audio. Defaults on: a
    /// silent video share is the surprising outcome.
    bool m_shareAudioEnabled = true;
    bool m_handRaised = false;
    /// The `m.reaction` our raise produced; lowering redacts exactly this.
    QString m_handReactionId;
    quint64 m_handOp = 0;
    /// Reaction event id -> SFU identity whose hand it is. A redaction names
    /// only what it removed, so this answers whose hand went down (and
    /// rejects unrelated redactions with one lookup).
    QHash<QString, QString> m_handReactions;
    /// Annotations whose membership had not been read yet, retried from the
    /// sessionChanged handler. Raises and reactions share this store (an empty
    /// `emoji` is a raise). Bounded; only genuinely unknown memberships are
    /// parked, so forgeries cannot fill it. Reactions also carry their arrival
    /// time and are not drawn once their window has passed.
    struct PendingAnnotation {
        QString sender;
        QString membershipEventId;
        /// Empty for a raised hand; the reaction's emoji otherwise.
        QString emoji;
        /// Arrival time, for the reaction window only.
        qint64 receivedAtMs = 0;
    };
    /// Keyed by the annotating reaction event id for raises; see
    /// onCallReactionReceived() for reactions.
    QHash<QString, PendingAnnotation> m_pendingAnnotations;
    /// The reaction window: `kReactionActiveMs`, or shorter in tests.
    int m_reactionWindowMs = kReactionActiveMs;
    /// Our in-flight reaction send, to recognise its answer.
    quint64 m_reactionOp = 0;
    /// When this device last sent a reaction; another inside the window is
    /// refused locally.
    qint64 m_lastReactionSentMs = 0;
    bool m_mediaEncrypted = false;
    // Streams whose frames are being dropped, by LiveKit sid. Cleared per
    // stream on recovery and entirely on teardown.
    QSet<QString> m_blockedStreams;
    /// Remote track sid -> last reported mute state; see
    /// noteRemoteTrackMutes(). Bounded; cleared with the participants.
    struct RemoteTrackMute {
        bool muted = false;
        int changes = 0;
    };
    QHash<QString, RemoteTrackMute> m_remoteTrackMuted;
    bool m_microphoneSilent = false;
    /// Whether the room is encrypted, so call media must be. Captured at join;
    /// unknown fails closed to true.
    bool m_roomEncrypted = true;

    /// Bumped on every join/leave; async replies from another generation are
    /// dropped.
    quint64 m_generation = 0;
    quint64 m_publishOp = 0;
    /// A membership publish still in flight when the call was torn down, and
    /// its room. If the server applies it after our retraction, it recreates
    /// a ghost membership, so its answer is retracted. Outlives the call, like
    /// `m_retract*`.
    quint64 m_abandonedPublishOp = 0;
    QString m_abandonedPublishRoomId;
    /// Whether a membership state event was actually written, so teardown
    /// does not issue a doomed retraction. Set whenever the server named an
    /// event id, including on a reported failure (see onMembershipPublished).
    bool m_membershipPublished = false;
    /// A heartbeat re-publish; its answer must not re-run the join sequence.
    quint64 m_refreshOp = 0;
    /// The in-flight delayed-leave restart, so its failure is actionable.
    quint64 m_delayedRestartOp = 0;
    /// The in-flight retraction and what is needed to re-issue it. Outlives
    /// the call: an unacknowledged retraction is still owed to the room.
    quint64 m_retractOp = 0;
    QString m_retractRoomId;
    QString m_retractDelayId;
    int m_retractAttempts = 0;
    /// When the membership was last (re-)published, for the re-publish
    /// cadence, independent of the 5 s delayed-restart tick.
    qint64 m_lastPublishMs = 0;

    QVariantList m_participants;
    /// The last SpeakersChanged round, re-applied to newly appeared rows.
    /// `level` (0..1) drives the volume-reactive ring; `active` still lights
    /// a binary ring for an SFU that reports only the flag. A level is never
    /// invented from the flag.
    QHash<QString, bool> m_speaking;
    QHash<QString, qreal> m_speakingLevel;
    QHash<QString, QString> m_connectionQuality;

    /// Owned. Created once and emptied on leave, never replaced.
    CallParticipantModel *m_participantModel = nullptr;
    CallShareModel *m_shareModel = nullptr;
    CallStageState *m_stageState = nullptr;

    /// Bumped each time we start sharing, giving local shares distinct ids
    /// (`local:<n>`): our share exists before the SFU names its track, and a
    /// reused id would let an old dismissal suppress a new share.
    quint64 m_localShareEpoch = 0;

    /// Refreshes the membership and restarts the delayed retraction.
    QTimer m_refreshTimer;
    /// Bounded retry for a retraction the server did not accept.
    QTimer m_retractRetryTimer;
    /// Track ids we published, so leave can unpublish them.
    QStringList m_publishedTrackIds;
    /// cid -> sid LiveKit assigned (TrackPublished). A cid missing here was
    /// declared but never published.
    QHash<QString, QString> m_publishedTrackSids;

    /// A media key delivered before we were in the call; held in memory and
    /// replayed on join. See onMediaKeyReceived().
    struct ParkedKey {
        QString roomId;
        QString sender;
        QString deviceId;
        int index = 0;
        QString keyBase64;
        /// Monotonic, from m_parkClock, so the age limit survives clock steps.
        qint64 arrivedMs = 0;
    };
    static constexpr int kMaxParkedKeys = 8;
    /// At most two indices per sending device, so no sender crowds out
    /// another.
    static constexpr int kMaxParkedKeysPerDevice = 2;
    /// A key nobody claimed within this long was not for a call we joined.
    static constexpr qint64 kParkedKeyTtlMs = 120000;
    QList<ParkedKey> m_parkedKeys;
    QElapsedTimer m_parkClock;
    /// Keep a validated key that arrived too early. Bounded.
    void parkMediaKey(const QString &roomId, const QString &sender,
                      const QString &deviceId, int index,
                      const QString &keyBase64);
    /// Drop parked keys older than the TTL.
    void expireParkedKeys();
    /// Replay whatever arrived early, then forget it.
    void applyParkedKeys();
    /// The published track id per kind, so stopping one never stops another.
    QString m_audioCid;
    QString m_cameraCid;
    QString m_screenCid;
    /// The share's audio track, empty when there is none. Retires with the
    /// share but is a separate track.
    QString m_shareAudioCid;
    int m_keyIndex = 0;
    /// The newest outbound key index known to have reached a device, or -1.
    /// Decides whether an undelivered key may be adopted; see
    /// rotateAndDistributeKey().
    int m_deliveredKeyIndex = -1;
    /// The device set the last media key reached; see distributeKeyIfNeeded().
    /// An empty set is never recorded.
    QString m_lastKeyTargets;
    int m_keyLaneReconciles = 0;
    /// Local ICE candidates this session. Zero on the publisher means the peer
    /// connection never started (LiveKit's 60 s JOIN_FAILURE).
    int m_candidatesSent = 0;
};
