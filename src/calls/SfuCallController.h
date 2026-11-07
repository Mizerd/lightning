#pragma once

#include <functional>

#include <QElapsedTimer>
#include <QHash>
#include <QByteArray>
#include <QVariantMap>
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
#include "calls/AudioLevelMeter.h"
#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/WindowCaptureSrc.h"
#endif

class MatrixClient;
class QScreen;
class RtcController;
struct RoomInfo;
struct CallSignal;
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

    Q_PROPERTY(bool streamInfoVisible READ streamInfoVisible WRITE setStreamInfoVisible
                   NOTIFY streamInfoChanged)
    Q_PROPERTY(QVariantList streamInfo READ streamInfo NOTIFY streamInfoChanged)
    Q_PROPERTY(int state READ stateInt NOTIFY stateChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(bool microphoneMuted READ microphoneMuted
                   NOTIFY mediaStateChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY mediaStateChanged)
    Q_PROPERTY(bool cameraOn READ cameraOn NOTIFY mediaStateChanged)
    Q_PROPERTY(bool screenSharing READ screenSharing NOTIFY mediaStateChanged)
    Q_PROPERTY(bool shareAudioEnabled READ shareAudioEnabled
                   WRITE setShareAudioEnabled NOTIFY mediaStateChanged)
    Q_PROPERTY(bool shareAudioSupported READ shareAudioSupported
                   NOTIFY shareAudioCapabilityChanged)
    Q_PROPERTY(bool shareAudioExcludesOwnPlayback
                   READ shareAudioExcludesOwnPlayback
                   NOTIFY shareAudioCapabilityChanged)
    Q_PROPERTY(int shareAudioMode READ shareAudioMode WRITE setShareAudioMode
                   NOTIFY mediaStateChanged)
    Q_PROPERTY(QStringList shareAudioApps READ shareAudioApps
                   NOTIFY mediaStateChanged)
    Q_PROPERTY(bool shareAudioCanChooseApps READ shareAudioCanChooseApps
                   NOTIFY shareAudioCapabilityChanged)
    Q_PROPERTY(QVariantList shareAudioApplications
                   READ shareAudioApplications
                   NOTIFY shareAudioApplicationsChanged)
    Q_PROPERTY(QString shareAudioStatus READ shareAudioStatus
                   NOTIFY shareAudioStatusChanged)
    Q_PROPERTY(bool shareAudioHeard READ shareAudioHeard
                   NOTIFY shareAudioStatusChanged)
    Q_PROPERTY(bool shareAudioMuted READ shareAudioMuted
                   WRITE setShareAudioMuted NOTIFY mediaStateChanged)
    Q_PROPERTY(bool handRaised READ handRaised NOTIFY mediaStateChanged)
    Q_PROPERTY(bool mediaEncrypted READ mediaEncrypted NOTIFY mediaStateChanged)
    Q_PROPERTY(bool remoteMediaBlocked READ remoteMediaBlocked
                   NOTIFY remoteMediaBlockedChanged)
    Q_PROPERTY(bool microphoneSilent READ microphoneSilent
                   NOTIFY microphoneSilentChanged)
    Q_PROPERTY(QString noiseSuppressionFailedMode
                   READ noiseSuppressionFailedMode
                   NOTIFY noiseSuppressionFailedModeChanged)
    Q_PROPERTY(bool noiseSuppressionFallbackToWebrtc
                   READ noiseSuppressionFallbackToWebrtc
                   NOTIFY noiseSuppressionFailedModeChanged)
    Q_PROPERTY(double microphoneLevel READ microphoneLevel
                   NOTIFY microphoneLevelChanged)
    Q_PROPERTY(int participantCount READ participantCount
                   NOTIFY participantCountChanged)
    Q_PROPERTY(CallParticipantModel *participantModel READ participantModel
                   CONSTANT)
    Q_PROPERTY(CallShareModel *shareModel READ shareModel CONSTANT)
    Q_PROPERTY(CallStageState *stageState READ stageState CONSTANT)

public:
    enum class State {
        Idle,
        Preparing,
        Authorizing,
        Connecting,
        Connected,
        Reconnecting,
        Ended,
        Failed,
    };
    Q_ENUM(State)

    explicit SfuCallController(QObject *parent = nullptr);
    ~SfuCallController() override;

    void setClient(MatrixClient *client);
    void setRtcController(RtcController *rtc);
    void setMediaEngine(SfuMediaEngine *engine);
    bool streamInfoVisible() const { return m_streamInfoVisible; }
    void setStreamInfoVisible(bool visible);
    QVariantList streamInfo() const { return m_streamInfo; }
    Q_INVOKABLE void refreshStreamInfo();
    Q_INVOKABLE void resetStreamInfoRates();
    void setScreenCastPortal(ScreenCastPortal *portal);
    void setCameraPortal(CameraPortal *portal);
    void setSettings(SettingsManager *settings);

    Q_INVOKABLE void attachVideoSink(const QString &identity,
                                     QObject *videoSink);
    Q_INVOKABLE void attachScreenSink(const QString &identity,
                                      QObject *videoSink);
    Q_INVOKABLE void attachLocalCameraSink(QObject *videoSink);
    Q_INVOKABLE void attachLocalScreenSink(QObject *videoSink);

    Q_INVOKABLE void detachSink(QObject *videoSink);

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
    bool shareAudioEnabled() const { return m_shareAudioMode != 0; }
    void setShareAudioEnabled(bool on);
    bool shareAudioSupported() const;
    bool shareAudioExcludesOwnPlayback() const;
    int shareAudioMode() const { return m_shareAudioMode; }
    void setShareAudioMode(int mode);
    QStringList shareAudioApps() const { return m_shareAudioApps; }
    bool shareAudioCanChooseApps() const;
    QVariantList shareAudioApplications() const;
    QString shareAudioStatus() const;
    bool shareAudioHeard() const { return m_shareAudioHeard; }
    bool shareAudioMuted() const { return m_shareAudioMuted; }
    void setShareAudioMuted(bool muted);
    Q_INVOKABLE void toggleShareAudioMuted();
    Q_INVOKABLE void setShareAudioAppChosen(const QString &key,
                                            const QString &label, bool chosen);
    Q_INVOKABLE void chooseOnlyShareAudioApp(const QString &key,
                                             const QString &label);
    Q_INVOKABLE void restoreShareAudioChoice(int mode, const QStringList &keys);
    Q_INVOKABLE void restoreShareAudioChoiceAfterShare(int mode,
                                                       const QStringList &keys);
    Q_INVOKABLE void watchShareAudioApplications(bool on);

    Q_INVOKABLE void refreshShareAudioCapability();

    enum class ShareAudioUnavailable {
        NoCapture,
        MissingPlugin,
        NoPipeWire,
        OldWindows
    };

    struct ShareAudioStatusInput {
        bool supported = false;
        ShareAudioUnavailable why = ShareAudioUnavailable::NoCapture;
        int mode = 1;
        QStringList chosenLabels;
        bool trackLive = false;
        bool perApplication = false;
        bool carriedAny = false;
        bool levelKnown = false;
        bool heard = true;
        QStringList failedLabels;
        bool limitReached = false;
        bool muted = false;
    };
    static QString shareAudioStatusText(const ShareAudioStatusInput &in);

    static bool categoryIsShareAudioOnly(const QString &category);
    bool handRaised() const { return m_handRaised; }
    /// True only when every frame we publish is encrypted. Never optimistic.
    bool mediaEncrypted() const { return m_mediaEncrypted; }
    bool remoteMediaBlocked() const { return !m_blockedStreams.isEmpty(); }
    bool microphoneSilent() const { return m_microphoneSilent; }
    QString noiseSuppressionFailedMode() const
    {
        return m_noiseSuppressionFailedMode;
    }
    bool noiseSuppressionFallbackToWebrtc() const
    {
        return m_noiseSuppressionFallbackToWebrtc;
    }
    Q_INVOKABLE QVariantList noiseSuppressionChoices() const;
    Q_INVOKABLE void retryNoiseSuppression();
    double microphoneLevel() const { return m_microphoneLevel; }
    void noteMicrophoneLevelAt(double peakDb, qint64 nowMs);
    Q_INVOKABLE bool mediaBlockedFor(const QString &identity) const;
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
    Q_INVOKABLE void leave();
    bool membershipWritesPending() const;
    void abandonOutstandingMembershipWrites();

    Q_INVOKABLE void setMicrophoneMuted(bool muted);
    Q_INVOKABLE void toggleMicrophoneMuted();
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void toggleDeafened();
    Q_INVOKABLE void setCameraOn(bool on);
    Q_INVOKABLE void toggleCamera();
    Q_INVOKABLE void requestScreenShare();
    Q_PROPERTY(QVariantList screenShareSources READ screenShareSources
                   NOTIFY screenShareSourcesChanged)
    QVariantList screenShareSources() const { return m_screenShareSources; }
    Q_PROPERTY(bool windowCaptureSupported READ windowCaptureSupported
                   CONSTANT)
    bool windowCaptureSupported() const
    {
#ifdef HAVE_LIGHTNING_WEBRTC
        return lightning::wincap::available();
#else
        return false;
#endif
    }
    Q_INVOKABLE void chooseScreenShareSource(int index);

    enum class LinuxShareRoute {
        Portal,
        FallbackDisplays,
        RefuseWaylandNeedsPortal,
        RefuseNoCaptureElement,
        RefuseNoDisplayServer,
    };

    static LinuxShareRoute linuxShareRoute(bool portalAvailable,
                                           const QString &platformName,
                                           const QString &sessionType,
                                           const QString &waylandDisplay,
                                           const QString &x11Display,
                                           bool captureElementPresent);

    static bool portalFailureFallsBackToDisplays(
        const QString &category, const QString &platformName,
        const QString &sessionType, const QString &waylandDisplay,
        const QString &x11Display, bool captureElementPresent);

    static QString linuxShareRefusal(LinuxShareRoute route,
                                     bool sandboxed = false);

    enum class LinuxCameraRoute {
        Direct,
        Portal,
    };

    static LinuxCameraRoute linuxCameraRoute(bool sandboxed, bool portalUsable,
                                             bool directDeviceVisible,
                                             bool cameraChosen = false);

    static bool directCameraFallsBackToPortal(const QString &category,
                                              bool viaPortal,
                                              bool portalWired,
                                              bool alreadyFellBack);

    enum class CameraFailureStep {
        Notice,
        OpenDeviceNode,
    };

    static CameraFailureStep cameraFailureStep(const QString &category,
                                               bool viaPortal,
                                               bool directDeviceVisible,
                                               bool alreadyFellBack);

    static QString cameraNoticeCategory(const QString &category,
                                        bool viaPortal,
                                        bool portalReportedCamera);

    static QString cameraPortalNotice(const QString &category, bool flatpak);

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
    Q_INVOKABLE void cancelScreenShareSelection();
    Q_INVOKABLE bool startScreenShare(int pipewireNodeId,
                                      int pipewireFd = -1,
                                      quint64 windowHandle = 0,
                                      const QRect &captureRect = {});
    Q_INVOKABLE void stopScreenShare();
    void applyShareQualityToLiveShare();
    Q_INVOKABLE void setHandRaised(bool raised);
    Q_INVOKABLE void toggleHandRaised();
    Q_INVOKABLE void sendCallReaction(const QString &emoji,
                                      const QString &name);

    /// How long a reaction stays on a tile. element-call's
    /// `src/reactions/ReactionsReader.ts` sets
    /// `REACTION_ACTIVE_TIME_MS = 3000`; matching it keeps both clients in
    /// step. Also the window in which a second reaction from the same sender
    /// is dropped, and in which this device refuses to send another.
    static constexpr int kReactionActiveMs = 3000;
    Q_INVOKABLE void setParticipantVolume(const QString &identity,
                                          int percent);
    Q_INVOKABLE int participantVolume(const QString &identity) const;

    Q_INVOKABLE void setShareVolume(const QString &shareId, int percent);
    Q_INVOKABLE int shareVolume(const QString &shareId) const;
    Q_INVOKABLE bool shareHasAudio(const QString &shareId) const;

    Q_INVOKABLE QVariantList participants() const;

    void ingestParticipantsForTest(const QVariantList &updates);
    void ingestSubscriberParticipantsForTest(int connectionId,
        const QVariantList &updates, bool replace = false)
    { updateSubscriberParticipants(connectionId, updates, replace); }
    QString trackKeyForSourceForTest(const QString &identity,
                                    const QString &source) const
    { return trackKeyForSource(identity, source); }
    void ingestSpeakersForTest(const QVariantList &speakers);
    void ingestConnectionQualityForTest(const QVariantList &updates);
    void setOwnIdentityForTest(const QString &identity);
    static bool startsCallForAnnouncement(const QVariantList &participants);
    static QString announcementTypeFor(const RoomInfo &room);
    int keyLaneReconcilesForTest() const { return m_keyLaneReconciles; }
    int engineParticipantVolumeForTest(const QString &identity) const
    {
        return m_engineParticipantVolume.value(identity, -1);
    }
    int engineShareVolumeForTest(const QString &shareId) const
    {
        return m_engineShareVolume.value(shareId, -1);
    }
    void startRefreshTickForTest(int ms)
    {
        m_refreshTimer.setInterval(ms);
        m_refreshTimer.start();
    }
    void setCallStateForTest(State state);
    void setMembershipForTest(const QString &roomId, const QString &delayId);
    void setRetractionOwedForMsForTest(qint64 ms) { m_retractOwedForMs = ms; }
    void setRetractionAnswerTimeoutMsForTest(int ms)
    {
        m_retractAnswerTimeoutMs = ms;
    }
    QString delayedRefusalReason() const { return m_delayedCategory; }
    void setShareAudioCanChooseAppsForTest(bool on)
    {
        m_shareAudioCanChooseForTest = on ? 1 : 0;
    }
    void setShareAudioUnavailableForTest(ShareAudioUnavailable why)
    {
        m_shareAudioSupportedForTest = 0;
        m_shareAudioWhyForTest = why;
        Q_EMIT shareAudioCapabilityChanged();
        Q_EMIT shareAudioStatusChanged();
    }
    int shareAudioRebuildsForTest() const { return m_shareAudioRebuilds; }
    void setScreenSharingForTest(bool on)
    {
        if (m_screenSharing == on)
            return;
        m_screenSharing = on;
        Q_EMIT mediaStateChanged();
    }
    void setShareAudioCidForTest(const QString &cid)
    {
        m_shareAudioCid = cid;
        if (!cid.isEmpty() && !m_publishedTrackIds.contains(cid))
            m_publishedTrackIds.append(cid);
    }
    QString shareAudioCidForTest() const { return m_shareAudioCid; }
    int parkedKeyCountForTest() const { return m_parkedKeys.size(); }
    bool hasParkedKeyForTest(const QString &sender) const
    {
        for (const ParkedKey &k : m_parkedKeys) {
            if (k.sender == sender)
                return true;
        }
        return false;
    }
    quint64 beginMembershipPublishForTest(const QString &roomId,
                                          const QString &focusUrl,
                                          bool announce = false);
    QStringList connectionFocusUrlsForTest() const
    { return m_connectionFocusUrls; }
    void setConnectionFocusUrlsForTest(const QStringList &urls)
    { m_connectionFocusUrls = urls; }
    quint64 sfuGenerationForTest() const { return m_sfuGeneration; }
    QString streamIdForIdentityForTest(const QString &identity) const
    { return streamIdForIdentity(identity); }
    void setReactionWindowMsForTest(int ms)
    {
        m_reactionWindowMs = ms > 0 ? ms : 1;
    }
    void setLocalMediaStateForTest(bool cameraOn, bool screenSharing);
    int deliveredKeyIndexForTest() const { return m_deliveredKeyIndex; }
    void noteMediaKeySendForTest(quint64 op) { m_keySendOps.insert(op, {}); }
    void startKeyLaneForTest() { startKeyLane(); }
    int adoptedKeyIndexForTest() const { return m_adoptedKeyIndex; }
    int newestKeyIndexForTest() const { return m_newestKey.index; }
    int pendingKeySwitchMsForTest() const
    {
        int ms = m_useKeyTimer.isActive() ? m_useKeyTimer.remainingTime() : -1;
        if (m_keySwitchFallbackTimer.isActive()) {
            const int fallback = m_keySwitchFallbackTimer.remainingTime();
            if (ms < 0 || fallback < ms)
                ms = fallback;
        }
        return ms;
    }
    void setKeyResendBackoffBaseMsForTest(int ms) { m_keyResendBaseMs = ms; }
    int keyResendsHeldBackForTest() const { return m_keyResendsHeldBack; }
    void setKeyTimingForTest(int useKeyDelayMs, int joinKeyGraceMs,
                             int switchFallbackMs = -1)
    {
        m_useKeyTimer.setInterval(useKeyDelayMs);
        m_joinKeyGraceMs = joinKeyGraceMs;
        if (switchFallbackMs >= 0)
            m_keySwitchFallbackTimer.setInterval(switchFallbackMs);
    }
    void setCameraCidForTest(const QString &cid)
    {
        m_cameraCid = cid;
        m_cameraOn = !cid.isEmpty();
    }
    void setCameraRouteForTest(bool viaPortal, bool portalReportedCamera,
                               bool deviceNodeVisible)
    {
        m_cameraViaPortal = viaPortal;
        m_cameraPortalReportedCamera = portalReportedCamera;
        m_cameraFellBack = false;
        m_cameraFellBackToPortal = false;
        m_deviceNodeVisibleForTest = deviceNodeVisible ? 1 : 0;
    }
    void setCameraAwaitingPortalForTest()
    {
        m_cameraAwaitingPortal = true;
        m_cameraOn = true;
    }
    void setCameraPathForTest(std::function<void(int)> publish,
                              std::function<void()> request, bool sandboxed,
                              bool portalUsable)
    {
        m_cameraPublisherForTest = std::move(publish);
        m_cameraRequestForTest = std::move(request);
        m_cameraSandboxedForTest = sandboxed ? 1 : 0;
        m_cameraPortalUsableForTest = portalUsable ? 1 : 0;
    }
    bool cameraAwaitingPortalForTest() const { return m_cameraAwaitingPortal; }
    int cameraPublishesForTest() const { return m_cameraPublishesForTest; }
    void setCameraFallbackWindowForTest(int ms)
    { m_deviceFallbackWindowMs = ms; }
    void holdCameraPortalReleaseForTest(bool hold)
    { m_holdCameraPortalReleaseForTest = hold; }
    void releaseCameraPortalForTest(const QString &cid)
    { onCameraPortalReleased(cid); }
    void releaseCameraPortalOnUnpublishForTest(bool on)
    { m_releaseCameraPortalOnUnpublishForTest = on; }
    bool cameraFellBackForTest() const { return m_cameraFellBack; }
    QString cameraCidForTest() const { return m_cameraCid; }
    bool cameraFellBackToPortalForTest() const
    {
        return m_cameraFellBackToPortal;
    }
    void restartCameraCaptureForTest(bool deviceNodeVisible)
    {
        m_deviceNodeVisibleForTest = deviceNodeVisible ? 1 : 0;
        startCameraCapture();
    }
    void setCameraChosenForTest(bool chosen)
    {
        m_cameraChosenForTest = chosen ? 1 : 0;
    }
    bool cameraViaPortalForTest() const { return m_cameraViaPortal; }
    void setReconnectTimingForTest(int firstBackoffMs, int attemptTimeoutMs,
                                   int budgetMs,
                                   int iceDisconnectedGraceMs = -1);
    int reconnectAttemptsForTest() const { return m_reconnectAttempts; }
    void setReconnectEpisodeCapForTest(int maxEpisodes, int windowMs)
    {
        m_maxReconnectEpisodes = maxEpisodes > 0 ? maxEpisodes : 1;
        m_reconnectEpisodeWindowMs = windowMs > 0 ? windowMs : 1;
    }

Q_SIGNALS:
    void streamInfoChanged();
    void stateChanged();
    void membershipWritesSettled();
    void mediaStateChanged();
    void remoteMediaBlockedChanged();
    void microphoneSilentChanged();
    void noiseSuppressionFailedModeChanged();
    void microphoneLevelChanged();
    void participantsChanged();
    void participantCountChanged();
    void shareVolumeChanged(const QString &shareId, int percent);
    void screenShareSourcesChanged();
    void shareAudioApplicationsChanged();
    void shareAudioStatusChanged();
    void shareAudioCapabilityChanged();
    void screenShareSourcesAvailable();
    void callFailed(const QString &message);

private Q_SLOTS:
    void onHandResult(quint64 opId, bool ok, bool raised,
                      const QString &category, const QString &eventId);
    void onHandChanged(const QString &roomId, const QString &sender,
                       const QString &membershipEventId,
                       const QString &reactionEventId, bool raised);
    void onHandsReceived(quint64 opId, const QString &roomId,
                         const QVariantList &hands);
    void onCallReactionReceived(const QString &roomId, const QString &sender,
                                const QString &membershipEventId,
                                const QString &emoji);
    void onRtcSendFinished(quint64 opId, bool ok, const QString &category,
                           const QString &eventId);
    void onMembershipPublished(quint64 opId, bool ok, const QString &category,
                               const QString &eventId, const QString &delayId,
                               const QString &delayedCategory);
    void onSfuState(const QString &state, const QString &category);
    void onSfuConnectionEvent(const QVariantMap &event);
    void updateSubscriberParticipants(int id, const QVariantList &rows,
                                      bool replace);
    void logRemoteSubscription(int id, const QString &sid,
                               const QString &trackSid, const QString &kind) const;
    quint64 connectFocusPlan();
    void clearSubscriberEngines();
    void onSfuJoined(const QString &identity,
                     const QVariantList &participants,
                     const QVariantList &iceServers,
                     const QByteArray &sifTrailer = QByteArray());
    void onSfuParticipants(const QVariantList &updates);
    void onSfuSpeakers(const QVariantList &speakers);
    void mergeSpeakers(const QVariantList &speakers);
    void onSfuConnectionQuality(const QVariantList &updates);
    void onSfuRemoteDescription(const QString &kind, const QString &target,
                                const QString &sdp);
    void onSfuRemoteCandidate(const QString &target,
                              const QString &candidateInit);
    void onEngineLocalDescription(int target, const QString &kind,
                                  const QString &sdp);
    void onEngineLocalCandidate(int target, const QString &candidateInit);
    void onEngineFailed(const QString &category);
    void onShareAudioRebuildWanted(const QString &cid);
    void onEngineTransportState(int target, const QString &state);
    void onSignalResumed();
    void onEnginePublishFailed(const QString &cid, const QString &category);
    void onRemotePlaybackFailed(bool failed);
    void onMediaKeyReceived(const QString &roomId, const QString &sender,
                            const QString &claimedDeviceId, int keyIndex,
                            const QString &keyBase64);
    void onMembershipRetracted(quint64 opId, bool ok, const QString &category);
    void refreshMembership();
    void reconcileKeyLane();
    void retryRetraction();
    void onHomeserverReachable();

private:
    bool populateLinuxDisplaySources();
    void offerLinuxDisplayPicker(bool autoSelectSingle);
    static QRect physicalRectForScreenNamed(const QString &name);

    void setState(State state, const QString &error = QString());
    void teardown(State finalState, const QString &error = QString());
    /// A peer's MSC4075 decline of our ring (see m_announceEventId). Not a
    /// slot: connected by member pointer, so moc never needs CallSignal.
    void onCallSignal(const CallSignal &signal);
    void endCallDeclinedIfAlone();

    enum class ReconnectPhase { None, Waiting, Dialing, Signalling, Joined };
    QString connectionLostMessage() const;
    static QString shareWhileReconnectingMessage();
    bool admitReconnectEpisode();
    static bool sfuFailureIsRefusal(const QString &category);
    bool sfuSessionLive() const;
    void beginReconnect(const QString &reason);
    void suspendSfuSession();
    void scheduleReconnectAttempt();
    void launchReconnectAttempt();
    void reconnectAttemptFailed(const QString &why);
    void finishReconnect();
    void giveUpReconnect(const QString &error);
    void stopReconnect();
    void onReconnectSfuState(const QString &state, const QString &category);
    ReconnectPhase m_reconnectPhase = ReconnectPhase::None;
    int m_reconnectAttempts = 0;
    int m_reconnectFirstBackoffMs = 0;
    int m_reconnectMaxBackoffMs = 0;
    QElapsedTimer m_reconnectClock;
    QTimer m_reconnectTimer;
    QTimer m_reconnectAttemptTimer;
    QTimer m_reconnectDeadlineTimer;
    QTimer m_iceDisconnectedTimer;
    bool m_shareSourceReusable = false;
    int m_shareNodeId = -1;
    quint64 m_shareWindowHandle = 0;
    QRect m_shareCaptureRect;
    bool m_transportEverConnected = false;
    QHash<int, QString> m_transportStates;
    bool m_resumeSnapshotPending = false;
    QList<qint64> m_reconnectEpisodeStarts;
    QElapsedTimer m_episodeClock;
    int m_maxReconnectEpisodes = 0;
    qint64 m_reconnectEpisodeWindowMs = 0;
    void startRetraction(const QString &roomId, const QString &delayId,
                         qint64 acceptedAtMs, bool longExpiry = false);
    void dispatchRetraction();
    bool owedRetractionStillDue();
    void clearOwedRetraction();
    void supersedeOwedRetractionFor(const QString &roomId);
    void republishMembership();
    void publishTracks();
    void applyAudioState();
    QString streamIdForIdentity(const QString &identity) const;
    struct KeyTarget {
        QString userId;
        QString deviceId;
        QString session;
    };
    QList<KeyTarget> mediaKeyTargets() const;
    static QString mediaKeyTargetsJson(const QList<KeyTarget> &targets);
    static QString keyTargetDevice(const KeyTarget &target);
    static QString mediaKeyRingName(const QString &userId,
                                    const QString &deviceId);
    void noteParticipantIdentities();
    void applyRaisedHand(const QString &reactionEventId,
                         const QString &identity);
    void applyCallReaction(const QString &identity, const QString &emoji);
    void retryPendingAnnotations();
    QString trackKeyForSource(const QString &identity,
                              const QString &source) const;

    bool mergeParticipants(const QVariantList &updates);
    void noteRemoteTrackMutes(const QVariantList &updates);
    void rebuildModels();
    void rebuildShareModel();

    void startKeyLane();
    void distributeKeyIfNeeded();
    void rotateMediaKey(const QList<KeyTarget> &targets, const char *reason);
    void sendNewestKeyTo(const QList<KeyTarget> &targets);
    void startUseKeyDelay(quint64 serial, bool reachesKeyInUse);
    bool keySwitchPending() const;
    void adoptNewestKey();
    void installNewestKey();
    bool someonePresentHoldsAdoptedKey(const QList<KeyTarget> &targets) const;
    int sfuPeerCount() const;
    bool keyLaneReady() const;
    void unpublishTrack(QString &cid);
    QVariantMap ownParticipantRow() const;
    void syncMicMuteToSfu();
    QString userIdForIdentity(const QString &identity) const;
    void applyStoredVolumes();
    void applyStoredShareVolumes();
    bool applyEngineParticipantVolume(const QString &identity, int percent);
    void applyEngineShareVolume(const QString &shareId,
                                const QString &identity, int percent);
    void muteOwnTrackIfLive(const QString &source);
    /// Push our camera/share intent to the SFU. Nothing else tells the server
    /// a video track ended: the wire has only AddTrack and Mute.
    void applyVideoState();
    void startCameraCapture();
    void publishCameraTrack(int pipewireFd);
    void abandonPendingCamera();
    bool cameraDeviceNodeVisible() const;
    bool cameraEngineReady() const;
    bool cameraSandboxed() const;
    bool cameraPortalUsable() const;
    void requestCameraAccess();
    bool cameraChosen() const;
    void fallBackToPortalCamera(const QString &category);
    void fallBackToDeviceCamera(const QString &category,
                                const QString &retiredCid = {});
    void publishDeviceFallback(quint64 token);
    void onCameraPortalReleased(const QString &cid);
    void clearLocalVideoSurface(const QString &streamId);
    QString userFacingError(const QString &category) const;
    static QString joinRefusalMessage(const QString &block);

    QPointer<MatrixClient> m_client;
    QPointer<RtcController> m_rtc;
    SfuVideoRouter *m_videoRouter = nullptr;
    bool m_streamInfoVisible = false;
    QVariantList m_streamInfo;
    QPointer<SfuMediaEngine> m_engine;
    QHash<int, QPointer<SfuMediaEngine>> m_subscriberEngines;
    QHash<int, QHash<QString, QVariantMap>> m_subscriberParticipants;
    QHash<QString, QVariantMap> m_publisherParticipants;
    QHash<int, QString> m_connectionOwnIdentity;
    QStringList m_connectionFocusUrls;
    quint64 m_sfuGeneration = 0;
    quint64 m_callGeneration = 0;
    bool m_publishFallbackTried = false;
    QHash<QString, QHash<int, QByteArray>> m_inboundRingKeys;
    QPointer<ScreenCastPortal> m_portal;
    QPointer<CameraPortal> m_cameraPortal;
    bool m_cameraAwaitingPortal = false;
    bool m_cameraViaPortal = false;
    bool m_cameraPortalReportedCamera = false;
    bool m_cameraFellBack = false;
    bool m_cameraFellBackToPortal = false;
    QString m_waitingPortalReleaseCid;
    QString m_releasedPortalCid;
    bool m_releaseCameraPortalOnUnpublishForTest = false;
    qint64 m_deviceFallbackDeadlineMs = 0;
    int m_deviceFallbackBusyRetries = 0;
    quint64 m_deviceFallbackGeneration = 0;
    quint64 m_deviceFallbackToken = 0;
    int m_deviceFallbackWindowMs = 8000;
    bool m_holdCameraPortalReleaseForTest = false;
    int m_cameraChosenForTest = -1;
    int m_deviceNodeVisibleForTest = -1;
    int m_cameraSandboxedForTest = -1;
    int m_cameraPortalUsableForTest = -1;
    std::function<void(int)> m_cameraPublisherForTest;
    std::function<void()> m_cameraRequestForTest;
    int m_cameraPublishesForTest = 0;
    QPointer<SettingsManager> m_settings;

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    bool m_demoCall = false;
#endif
    State m_state = State::Idle;
    QString m_roomId;
    QString m_lastError;
    bool m_failureAnnounced = false;
    QString m_focusUrl;
    QString m_membershipEventId;
    QHash<QString, int> m_shareVolumes;
    QHash<QString, int> m_engineParticipantVolume;
    QHash<QString, int> m_engineShareVolume;
    bool m_announceOnPublish = false;
    QString m_announceIntent;
    quint64 m_announceOp = 0;
    QString m_announceEventId;
    QString m_announcePeer;
    QString m_pendingDeclineTarget;
    QString m_delayId;
    QString m_delayedCategory;
    QString m_ownIdentity;
    QVariantList m_screenShareSources;
    bool m_withVideo = false;

    bool m_micMuted = false;
    bool m_shareAudioMuted = false;
    int m_shareAudioRebuilds = 0;
    static constexpr int kMaxShareAudioRebuildsPerShare = 3;
    bool m_deafened = false;
    bool m_micMutedBeforeDeafen = false;
    bool m_cameraOn = false;
    bool m_screenSharing = false;
    int m_shareAudioMode = 1;
    int m_shareAudioLastOnMode = 1;
    QStringList m_shareAudioApps;
    QHash<QString, QString> m_shareAudioAppLabels;
    QVariantList m_shareAudioApplications;
    QTimer m_shareAudioAppsTimer;
    int m_shareAudioAppsWatchers = 0;
    int m_shareAudioCanChooseForTest = -1;
    int m_shareAudioSupportedForTest = -1;
    ShareAudioUnavailable m_shareAudioWhyForTest =
        ShareAudioUnavailable::NoCapture;
    bool m_lastShareAudioSupported = false;
    bool m_lastShareAudioCanChoose = false;
    bool m_shareAudioListInFlight = false;
    bool m_shareAudioListHangLogged = false;
    QElapsedTimer m_shareAudioListStarted;
    QVariantList m_shareAudioLastApps;
    bool m_shareAudioReportLive = false;
    bool m_shareAudioReportPerApp = false;
    bool m_shareAudioReportExcludesUs = false;
    QStringList m_shareAudioCarried;
    QStringList m_shareAudioFailed;
    bool m_shareAudioLimitReached = false;
    bool m_shareAudioHeard = false;
    bool m_shareAudioLevelKnown = false;
    QElapsedTimer m_shareAudioLastHeard;
    bool m_shareAudioRestorePending = false;
    int m_shareAudioRestoreMode = 1;
    QStringList m_shareAudioRestoreApps;
    void restoreStoredShareAudioChoice();
    void persistShareAudioChoice();
    void refreshShareAudioApplications();
    void rebuildShareAudioApplications();
    void updateShareAudioAppsPolling();
    void shareAudioShareEnded();
    void applyShareAudioToRunningShare();
    void publishShareAudioTrack();
    void resetShareAudioLevel();
    bool m_handRaised = false;
    QString m_handReactionId;
    quint64 m_handOp = 0;
    QHash<QString, QString> m_handReactions;
    struct PendingAnnotation {
        QString sender;
        QString membershipEventId;
        QString emoji;
        qint64 receivedAtMs = 0;
    };
    QHash<QString, PendingAnnotation> m_pendingAnnotations;
    int m_reactionWindowMs = kReactionActiveMs;
    quint64 m_reactionOp = 0;
    qint64 m_lastReactionSentMs = 0;
    bool m_mediaEncrypted = false;
    QSet<QString> m_blockedStreams;
    struct RemoteTrackMute {
        bool muted = false;
        int changes = 0;
    };
    QHash<QString, RemoteTrackMute> m_remoteTrackMuted;
    bool m_microphoneSilent = false;
    QString m_noiseSuppressionFailedMode;
    bool m_noiseSuppressionFallbackToWebrtc = false;
    void setNoiseSuppressionFailedMode(const QString &mode,
                                       bool fallbackToWebrtc = false);
    double m_microphoneLevel = 0.0;
    lightning::calls::MeterThrottle m_microphoneMeter;
    void resetMicrophoneLevel();
    bool m_roomEncrypted = true;

    quint64 m_generation = 0;
    quint64 m_publishOp = 0;
    /// A membership publish still in flight when the call was torn down, and
    /// its room. If the server applies it after our retraction, it recreates
    /// a ghost membership, so its answer is retracted. Outlives the call, like
    /// `m_retract*`.
    quint64 m_abandonedPublishOp = 0;
    QString m_abandonedPublishRoomId;
    bool m_membershipPublished = false;
    quint64 m_refreshOp = 0;
    quint64 m_delayedRestartOp = 0;
    quint64 m_retractOp = 0;
    QString m_retractRoomId;
    QString m_retractDelayId;
    QString m_retractUserId;
    QString m_retractDeviceId;
    qint64 m_retractDeadlineMs = 0;
    int m_retractAttempts = 0;
    bool m_retractRetryPromptly = false;
    qint64 m_retractOwedForMs = 0;
    qint64 m_retractOwedLongForMs = 0;
    QTimer m_retractAnswerTimer;
    int m_retractAnswerTimeoutMs = 0;
    quint64 m_supersededRetractOp = 0;
    QString m_supersededRetractRoomId;
    qint64 m_membershipAcceptedMs = 0;
    bool m_membershipLongExpiryUnguarded = false;
    qint64 m_lastPublishMs = 0;

    QVariantList m_participants;
    QHash<QString, bool> m_speaking;
    QHash<QString, qreal> m_speakingLevel;
    QHash<QString, QString> m_connectionQuality;

    CallParticipantModel *m_participantModel = nullptr;
    CallShareModel *m_shareModel = nullptr;
    CallStageState *m_stageState = nullptr;

    quint64 m_localShareEpoch = 0;

    QTimer m_refreshTimer;
    QTimer m_retractRetryTimer;
    QStringList m_publishedTrackIds;
    QHash<QString, QString> m_publishedTrackSids;

    struct ParkedKey {
        QString roomId;
        QString sender;
        QString deviceId;
        int index = 0;
        QString keyBase64;
        qint64 arrivedMs = 0;
    };
    static constexpr int kMaxParkedKeys = 8;
    static constexpr int kMaxParkedKeysPerDevice = 2;
    static constexpr qint64 kParkedKeyTtlMs = 120000;
    QList<ParkedKey> m_parkedKeys;
    QElapsedTimer m_parkClock;
    void parkMediaKey(const QString &roomId, const QString &sender,
                      const QString &deviceId, int index,
                      const QString &keyBase64);
    void expireParkedKeys();
    void applyParkedKeys();
    QString m_audioCid;
    QString m_cameraCid;
    QString m_screenCid;
    QString m_shareAudioCid;
    /// Our sending key. The raw bytes are kept (memory only, never logged) so
    /// the key can be shared with a joiner; scrubbed per call.
    struct OutboundKey {
        int index = -1;
        QByteArray raw;
        qint64 mintedMs = 0;
        quint64 serial = 0;
    };
    OutboundKey m_newestKey;
    quint64 m_adoptedKeySerial = 0;
    int m_adoptedKeyIndex = -1;
    QHash<QString, QString> m_keyRecipients;
    QHash<QString, QString> m_keyHolders;
    QSet<QString> m_adoptedKeyRecipients;
    struct KeyResendBackoff {
        int failures = 0;
        qint64 notBeforeMs = 0;
    };
    QHash<QString, KeyResendBackoff> m_keyResendBackoff;
    int m_keyResendBaseMs = 5000;
    static constexpr int kKeyResendCeilingMs = 60000;
    int m_keyResendsHeldBack = 0;
    void noteKeyDeliveryFailed(const QString &device);
    int m_keyCursor = 0;
    quint64 m_keySerialCounter = 0;
    QElapsedTimer m_keyClock;
    QTimer m_useKeyTimer;
    QTimer m_keySwitchFallbackTimer;
    int m_joinKeyGraceMs = 0;
    int m_deliveredKeyIndex = -1;
    int m_unaddressableLogged = -1;
    bool m_playbackLostAnnounced = false;
    QString m_shownNotice;
    QString m_announcedFailure;
    QString m_cameraNoticeCategory;
    QString m_cameraNoticeCid;
    void announceCameraNotice(const QString &category, const QString &cid);
    bool withdrawNotice(const QString &notice);
    static QString playbackLostNotice();
    struct KeySend {
        quint64 serial = 0;
        QStringList devices;
        QStringList keyInUseHolders;
    };
    void onMediaKeySent(quint64 op, bool ok, const QString &category,
                        int delivered, int keyIndex,
                        const QVariantList &undelivered);
    QHash<quint64, KeySend> m_keySendOps;
    void resetKeyLane();
    void refuseJoin(const QString &message, bool announce);
    int m_keyLaneReconciles = 0;
    int m_candidatesSent = 0;
};
