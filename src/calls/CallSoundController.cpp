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
// An announced ring cannot outlive the longest invite the ring path honours
// (AppController bounds it at 300 s). This is a backstop for a stop signal
// that never comes, not the ordinary way a ring ends.
constexpr int kRingCapMs = 300 * 1000;
} // namespace

CallSoundController::CallSoundController(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    // ONE evaluation per burst of model signals, and — the reason it is
    // deferred at all — AFTER the controller finishes what it is doing. On
    // leave, SfuCallController clears its participant model BEFORE it moves
    // to Ended; read synchronously, that clear is everyone leaving at once.
    // Deferred, the evaluation sees the call already over and stays silent.
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
    // A loop already playing picks up a new volume, and the ringback stops
    // at once when the master switch goes off.
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
        // Every way the roster can move: people in and out, a hand or a
        // share flag on an existing row, and a wholesale rebuild.
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
    // Baseline without sound: whatever the call is doing right now is not
    // news.
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
    // A share carrying this computer's WHOLE output mix would carry our
    // cues to everyone in the call. Where the share captures each
    // application on its own and leaves Lightning out, nothing leaks.
    //
    // ORDER MATTERS: the two capability probes can start a GStreamer device
    // monitor the first time they are asked (bounded, but on this thread),
    // so they are reached only while a share with audio is actually live —
    // by which point the share menu has already asked and the answer is
    // cached.
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
        // The SFU re-signalling under a call that was already up is a
        // reconnect, and must not replay the join cue as if new.
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
        // The room as it stands at connect is the baseline; read it now
        // rather than waiting for the next model signal.
        evaluateRoster();
    }
    applyLoop();
}

void CallSoundController::onGroupMedia()
{
    if (!m_groupCall)
        return;
    // Before the share cue: a share that starts capturing the output mix
    // must not announce itself through that mix.
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

    // The announced ring ends the moment that call stops ringing, whatever
    // ended it.
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
    // A ring for a call that is not (or no longer) ringing would never be
    // stopped by onLegacyState, because nothing would change.
    if (m_calls
        && (!m_calls->ringing() || m_calls->activeCallId() != callId))
        return;
    // The user already silenced THIS call. A re-announcement of it (the
    // announcing path can run again for the same call) must not undo that.
    if (isRingSilenced(callId))
        return;
    m_policy.setIncomingRing(true);
    m_ringCap.start();
    applyLoop();
    // Last, so a QML binding reading ringingCallId sees the loop already
    // asserted rather than a ring that is announced and not yet playing.
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
    // ONLY the call ringing now. With the legacy lane attached that is
    // CallController's own answer — which also covers the fallback case,
    // where the desktop's themed sound rings and our loop never started, so
    // m_ringCallId is empty. Without it (no call lane at all) our own ring
    // is the only thing that can be silenced.
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
    // Recorded BEFORE anything is emitted: stopIncomingRing() and the
    // signal below both run their handlers synchronously, and any of them
    // that re-announces this call must already find it silenced.
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
        // The cue NAME only: never a room, a person or an identity. This is
        // the line a tester's log uses to tell "the sound was triggered"
        // from "the sound was heard", which only a person can confirm.
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
    // The ringer plays on the system default output — the machine should
    // ring where its owner will hear it, not in a headset on the desk. What
    // plays DURING a call (call waiting, ringback) goes where the call goes.
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
