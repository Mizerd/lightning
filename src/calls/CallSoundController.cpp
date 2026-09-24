#include "calls/CallSoundController.h"

#include <QLoggingCategory>

#include "app/SettingsManager.h"
#include "calls/CallController.h"
#include "calls/CallParticipantModel.h"
#include "calls/SfuCallController.h"

Q_LOGGING_CATEGORY(lcCallSound, "lightning.calls.sound")

using callsound::Cue;
using callsound::GroupPhase;
using callsound::LegacyPhase;
using callsound::Loop;

namespace {
// Backstop for a stop signal that never comes: no announced ring outlives the
// longest invite AppController honours (300 s).
constexpr int kRingCapMs = 300 * 1000;
} // namespace

CallSoundController::CallSoundController(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    // One deferred evaluation per burst of model signals. Deferred so it runs
    // after the controller finishes: on leave the participant model is
    // cleared before the state moves to Ended, which read synchronously would
    // look like everyone leaving.
    m_rosterTimer.setSingleShot(true);
    m_rosterTimer.setInterval(0);
    connect(&m_rosterTimer, &QTimer::timeout, this,
            &CallSoundController::evaluateRoster);
    m_ringCap.setSingleShot(true);
    m_ringCap.setInterval(kRingCapMs);
    connect(&m_ringCap, &QTimer::timeout, this,
            &CallSoundController::stopIncomingRing);
}

CallSoundController::~CallSoundController() = default;

void CallSoundController::setClockForTest(std::function<qint64()> clock)
{
    m_testClock = std::move(clock);
}

qint64 CallSoundController::now() const
{
    return m_testClock ? m_testClock() : m_clock.elapsed();
}

void CallSoundController::setSettings(SettingsManager *settings)
{
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::callSoundSettingsChanged, this,
                &CallSoundController::onSettingsChanged);
    }
    onSettingsChanged();
}

void CallSoundController::onSettingsChanged()
{
    callsound::Preferences prefs;
    if (m_settings) {
        prefs.enabled = m_settings->callSoundsEnabled();
        prefs.presence = m_settings->callSoundsPresence();
        prefs.controls = m_settings->callSoundsControls();
        prefs.shareAndHand = m_settings->callSoundsShareAndHand();
    }
    m_policy.setPreferences(prefs);
    // A playing loop picks up a new volume, and the ringback stops at once
    // when the master switch goes off.
    applyLoop();
}

void CallSoundController::setGroupCall(SfuCallController *groupCall)
{
    if (m_groupCall)
        disconnect(m_groupCall, nullptr, this, nullptr);
    m_groupCall = groupCall;
    if (!m_groupCall)
        return;
    connect(m_groupCall, &SfuCallController::stateChanged, this,
            &CallSoundController::onGroupState);
    connect(m_groupCall, &SfuCallController::mediaStateChanged, this,
            &CallSoundController::onGroupMedia);
    if (CallParticipantModel *model = m_groupCall->participantModel()) {
        // Every way the roster can change: rows in and out, hand or share flags,
        // and rebuilds.
        connect(model, &QAbstractItemModel::rowsInserted, this,
                &CallSoundController::scheduleRoster);
        connect(model, &QAbstractItemModel::rowsRemoved, this,
                &CallSoundController::scheduleRoster);
        connect(model, &QAbstractItemModel::dataChanged, this,
                &CallSoundController::scheduleRoster);
        connect(model, &QAbstractItemModel::modelReset, this,
                &CallSoundController::scheduleRoster);
        connect(model, &QAbstractItemModel::layoutChanged, this,
                &CallSoundController::scheduleRoster);
    }
    // Baseline silently: the call's current state is not news.
    m_policy.localAudioChanged(callsound::Lane::Group,
                               m_groupCall->microphoneMuted(),
                               m_groupCall->deafened());
    onGroupState();
}

void CallSoundController::setLegacyCalls(CallController *calls)
{
    if (m_calls)
        disconnect(m_calls, nullptr, this, nullptr);
    m_calls = calls;
    if (!m_calls)
        return;
    connect(m_calls, &CallController::stateChanged, this,
            &CallSoundController::onLegacyState);
    connect(m_calls, &CallController::audioStateChanged, this,
            &CallSoundController::onLegacyAudio);
    m_policy.localAudioChanged(callsound::Lane::Legacy,
                               m_calls->microphoneMuted(),
                               m_calls->deafened());
    onLegacyState();
}

void CallSoundController::setSink(std::unique_ptr<CallSoundSink> sink)
{
    m_sink = std::move(sink);
    m_loop = Loop::None;
    applyLoop();
}

bool CallSoundController::ringerAvailable() const
{
    return m_sink && m_sink->canPlay(callsound::soundName(Loop::Ring));
}

void CallSoundController::refreshOutputCapture()
{
    // A share carrying this computer's whole output mix would carry our cues
    // to everyone; per-application capture excludes Lightning. The capability
    // probes may start a device monitor, so they are only reached while a
    // share with audio is live (by then the answer is cached).
    bool captured = false;
    if (m_groupCall) {
        captured = m_groupCall->screenSharing()
            && m_groupCall->shareAudioEnabled()
            && m_groupCall->shareAudioSupported()
            && !m_groupCall->shareAudioExcludesOwnPlayback();
    }
    m_policy.setOutputCapturedByShare(captured);
}

void CallSoundController::onGroupState()
{
    if (!m_groupCall)
        return;
    refreshOutputCapture();
    GroupPhase phase = GroupPhase::Idle;
    switch (static_cast<SfuCallController::State>(m_groupCall->stateInt())) {
    case SfuCallController::State::Idle:
        phase = GroupPhase::Idle;
        break;
    case SfuCallController::State::Preparing:
    case SfuCallController::State::Authorizing:
    case SfuCallController::State::Connecting:
        // Re-signalling under a call that was up is a reconnect; do not replay
        // the join cue.
        phase = m_groupWasConnected ? GroupPhase::Reconnecting
                                    : GroupPhase::Joining;
        break;
    case SfuCallController::State::Connected:
        phase = GroupPhase::Connected;
        break;
    case SfuCallController::State::Reconnecting:
        phase = GroupPhase::Reconnecting;
        break;
    case SfuCallController::State::Ended:
    case SfuCallController::State::Failed:
        phase = GroupPhase::Ended;
        break;
    }
    if (phase == GroupPhase::Connected)
        m_groupWasConnected = true;
    else if (phase == GroupPhase::Idle || phase == GroupPhase::Ended)
        m_groupWasConnected = false;

    const GroupPhase before = m_policy.groupPhase();
    emitCues(m_policy.groupPhaseChanged(phase, now()));
    if (phase == GroupPhase::Connected && before != GroupPhase::Connected) {
        // The room at connect is the baseline; read it now.
        evaluateRoster();
    }
    applyLoop();
}

void CallSoundController::onGroupMedia()
{
    if (!m_groupCall)
        return;
    // Before the share cue: a share that starts capturing the output mix must
    // not announce itself through it.
    refreshOutputCapture();
    emitCues(m_policy.localAudioChanged(callsound::Lane::Group,
                                        m_groupCall->microphoneMuted(),
                                        m_groupCall->deafened()));
    emitCues(m_policy.localShareChanged(m_groupCall->screenSharing()));
    applyLoop();
}

void CallSoundController::scheduleRoster()
{
    m_rosterTimer.start();
}

void CallSoundController::evaluateRoster()
{
    if (!m_groupCall)
        return;
    CallParticipantModel *model = m_groupCall->participantModel();
    if (!model)
        return;
    callsound::Roster roster;
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, CallParticipantModel::LocalRole).toBool())
            continue;
        const QString identity =
            model->data(index, CallParticipantModel::IdentityRole).toString();
        if (identity.isEmpty())
            continue;
        callsound::RemoteParticipant p;
        p.handRaised =
            model->data(index, CallParticipantModel::HandRaisedRole).toBool();
        p.sharing = model->data(index, CallParticipantModel::ScreenSharingRole)
                        .toBool();
        roster.insert(identity, p);
    }
    emitCues(m_policy.rosterChanged(roster, now()));
}

void CallSoundController::onLegacyState()
{
    if (!m_calls)
        return;
    LegacyPhase phase = LegacyPhase::Idle;
    switch (m_calls->state()) {
    case CallController::State::Idle: phase = LegacyPhase::Idle; break;
    case CallController::State::Inviting:
        phase = LegacyPhase::OutgoingRinging;
        break;
    case CallController::State::Ringing:
        phase = LegacyPhase::IncomingRinging;
        break;
    case CallController::State::Connecting:
        phase = LegacyPhase::Connecting;
        break;
    case CallController::State::Active: phase = LegacyPhase::Active; break;
    case CallController::State::Ended: phase = LegacyPhase::Ended; break;
    }
    const CallController::EndReason reason = m_calls->endReason();
    const bool remoteEndedOutgoing =
        reason == CallController::EndReason::RemoteHangup
        || reason == CallController::EndReason::RemoteReject
        || reason == CallController::EndReason::InviteTimeout
        || reason == CallController::EndReason::Busy;
    emitCues(m_policy.legacyPhaseChanged(phase, remoteEndedOutgoing));

    // The announced ring ends as soon as that call stops ringing, however it
    // ended.
    if (!m_ringCallId.isEmpty()
        && (!m_calls->ringing() || m_calls->activeCallId() != m_ringCallId))
        stopIncomingRing();
    applyLoop();
}

void CallSoundController::onLegacyAudio()
{
    if (!m_calls)
        return;
    emitCues(m_policy.localAudioChanged(callsound::Lane::Legacy,
                                        m_calls->microphoneMuted(),
                                        m_calls->deafened()));
}

void CallSoundController::setRingCallId(const QString &callId)
{
    if (m_ringCallId == callId)
        return;
    m_ringCallId = callId;
    Q_EMIT ringingCallIdChanged();
}

void CallSoundController::startIncomingRing(const QString &callId)
{
    if (callId.isEmpty())
        return;
    // A ring for a call not ringing would never be stopped by onLegacyState.
    if (m_calls
        && (!m_calls->ringing() || m_calls->activeCallId() != callId))
        return;
    // The user silenced this call; a re-announcement must not undo that.
    if (isRingSilenced(callId))
        return;
    m_policy.setIncomingRing(true);
    m_ringCap.start();
    applyLoop();
    // Last, so ringingCallId is only set once the loop is playing.
    setRingCallId(callId);
}

void CallSoundController::stopIncomingRing()
{
    m_ringCap.stop();
    if (m_ringCallId.isEmpty() && !m_policy.incomingRing())
        return;
    m_policy.setIncomingRing(false);
    applyLoop();
    setRingCallId(QString());
}

bool CallSoundController::silenceRing(const QString &callId)
{
    if (callId.isEmpty())
        return false;
    // Only the call ringing now. With the legacy lane, CallController answers
    // (covering the desktop's themed ring, where our loop never started);
    // without it, only our own ring can be silenced.
    const bool current = m_calls
        ? (m_calls->ringing() && m_calls->activeCallId() == callId)
        : callId == m_ringCallId;
    if (!current) {
        qCInfo(lcCallSound) << "call ring silence refused: not the ringing "
                               "call";
        return false;
    }
    if (isRingSilenced(callId))
        return true;
    // Record before emitting: handlers run synchronously and a re-announcement
    // must already see it silenced.
    m_silencedCallId = callId;
    qCInfo(lcCallSound) << "call ring silenced by the user ours="
                        << (m_ringCallId == callId ? "yes" : "no");
    if (m_ringCallId == callId)
        stopIncomingRing();
    Q_EMIT ringSilenced(callId);
    return true;
}

qreal CallSoundController::cueVolume() const
{
    const int percent = m_settings ? m_settings->callSoundVolume()
                                   : SettingsManager::kDefaultCallSoundVolume;
    return qBound(0, percent, 100) / 100.0;
}

qreal CallSoundController::ringVolume() const
{
    const int percent = m_settings ? m_settings->ringerVolume()
                                   : SettingsManager::kDefaultRingerVolume;
    return qBound(0, percent, 100) / 100.0;
}

void CallSoundController::emitCues(const QList<Cue> &cues)
{
    for (Cue cue : cues) {
        const QString name = callsound::soundName(cue);
        // Cue name only, never a room or person; distinguishes "triggered"
        // from "heard" in a tester's log.
        qCInfo(lcCallSound) << "call sound cue=" << name
                            << "sink=" << (m_sink ? "yes" : "none");
        if (m_sink)
            m_sink->play(name, cueVolume(), /*inCall=*/true);
    }
}

void CallSoundController::applyLoop()
{
    const Loop wanted = m_policy.desiredLoop();
    const bool ringing = wanted == Loop::Ring || wanted == Loop::CallWaiting;
    // The ringer uses the system default output, so the machine rings where
    // its owner will hear it; in-call sounds (call waiting, ringback) follow
    // the call's output.
    const bool inCall = wanted != Loop::Ring;
    const qreal volume = ringing ? ringVolume() : cueVolume();
    if (wanted != m_loop)
        qCInfo(lcCallSound) << "call sound loop=" << callsound::soundName(wanted)
                            << "was=" << callsound::soundName(m_loop)
                            << "sink=" << (m_sink ? "yes" : "none");
    m_loop = wanted;
    if (m_sink)
        m_sink->loop(callsound::soundName(wanted), volume, inCall);
}

void CallSoundController::preview(const QString &sound)
{
    if (!m_sink)
        return;
    if (sound == callsound::soundName(Loop::Ring)) {
        m_sink->play(sound, ringVolume(), /*inCall=*/false);
        return;
    }
    m_sink->play(sound, cueVolume(), /*inCall=*/true);
}
