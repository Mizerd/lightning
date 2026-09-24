// Plays Lightning's call sounds on the right events.
//
// The DECISIONS are CallSoundPolicy's; this class only watches the two call
// lanes (SfuCallController for MatrixRTC group calls, CallController for
// legacy 1:1 calls), translates their state into the policy's vocabulary,
// and hands whatever the policy returns to a CallSoundSink. The sink is the
// only thing that touches audio, and it is injected: the real application
// installs CallSoundPlayer (Qt Multimedia, on its own thread), tests install
// a recorder, and a controller with NO sink decides everything and plays
// nothing — which is also what keeps the offscreen test fleet from opening
// an audio device.
//
// THE INCOMING RING IS ANNOUNCED, NOT INFERRED. Whether a call rings at all
// is decided in AppController (desktop notifications on, not a backlog
// invite, not an ignored sender, not a muted room, the per-sender cooldown,
// the ring switch) — the same place that raises the notification card. It
// calls startIncomingRing() when it decides to ring; the ring STOPS here, on
// its own, the moment CallController is no longer ringing that call, so no
// path that ends a ring (answer, decline, answered elsewhere, expiry, a
// newer call) can leave it playing.
//
// SILENCE IS PER CALL. silenceRing() stops the ringer for the call that is
// ringing NOW and remembers that call's id, so a re-announcement of the SAME
// call cannot start it again (startIncomingRing refuses it, and AppController
// asks isRingSilenced() before it re-raises the notification card with a
// themed sound). A different call id rings normally: the memory is one id,
// and a new call is by definition not it. It never touches the call itself —
// the card stays up and can still be answered or declined.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

#include "calls/CallSoundPolicy.h"

class CallController;
class SettingsManager;
class SfuCallController;

/// Where the sounds go. Names are CallSoundPolicy's soundName()s; volumes
/// are PERCEPTUAL 0..1 (the sink maps them to linear gain).
class CallSoundSink
{
public:
    virtual ~CallSoundSink() = default;
    /// Play `sound` once. `inCall`: route it to the call's output device
    /// rather than the system default.
    virtual void play(const QString &sound, qreal volume, bool inCall) = 0;
    /// Loop `sound` until replaced; an empty name stops the loop.
    virtual void loop(const QString &sound, qreal volume, bool inCall) = 0;
    /// Whether `sound` has loaded and can actually be heard. False while
    /// unknown: a caller with a fallback must keep it until this says yes.
    virtual bool canPlay(const QString &sound) const = 0;
};

class CallSoundController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallSoundController is exposed via app.callSounds")
    /// The call Lightning's OWN ringer is sounding for ("" when it is not
    /// ringing — including when the desktop's themed sound is the ringer, and
    /// after the call was silenced). IncomingCallPrompt offers Silence only
    /// while this names the ringing call.
    Q_PROPERTY(QString ringingCallId READ ringingCallId NOTIFY
                   ringingCallIdChanged)

public:
    explicit CallSoundController(QObject *parent = nullptr);
    ~CallSoundController() override;

    void setSettings(SettingsManager *settings);
    void setGroupCall(SfuCallController *groupCall);
    void setLegacyCalls(CallController *calls);
    /// Takes ownership. Null removes the sink (decide, play nothing).
    void setSink(std::unique_ptr<CallSoundSink> sink);
    CallSoundSink *sink() const { return m_sink.get(); }

    /// True once Lightning's own ringer has loaded. Until then the caller
    /// keeps the desktop's themed call sound, so a machine where Qt
    /// Multimedia cannot open an output still rings.
    bool ringerAvailable() const;
    /// AppController decided this call rings (see the header comment).
    void startIncomingRing(const QString &callId);
    void stopIncomingRing();
    QString ringingCallId() const { return m_ringCallId; }
    /// Silence the ring for `callId` only. A no-op (false) unless `callId`
    /// is the call ringing right now — a stale card, a crafted id or an
    /// already-ended call must not silence the NEXT call. Works whichever
    /// ringer is sounding: ours stops here, and the ringSilenced signal
    /// tells the notification card to drop its themed sound. The call keeps
    /// ringing visually and can still be answered or declined.
    Q_INVOKABLE bool silenceRing(const QString &callId);
    /// Whether `callId` was silenced. AppController reads it before
    /// (re-)announcing a call, so a re-post of the same call stays silent.
    bool isRingSilenced(const QString &callId) const
    {
        return !callId.isEmpty() && callId == m_silencedCallId;
    }

    /// Settings "Test" buttons: "ring" plays one bar of the ringer at the
    /// ringer volume, anything else one cue at the call-sound volume. Plays
    /// even with the switches off — the user asked to hear it.
    Q_INVOKABLE void preview(const QString &sound);

    /// Tests: a deterministic clock (milliseconds).
    void setClockForTest(std::function<qint64()> clock);
    const callsound::Policy &policy() const { return m_policy; }

Q_SIGNALS:
    void ringingCallIdChanged();
    /// A ring was silenced by the user. Emitted AFTER the state is recorded,
    /// so a handler that asks isRingSilenced() already gets true.
    void ringSilenced(const QString &callId);

private:
    void setRingCallId(const QString &callId);
    void onGroupState();
    void onGroupMedia();
    void scheduleRoster();
    void evaluateRoster();
    void onLegacyState();
    void onLegacyAudio();
    void onSettingsChanged();
    void refreshOutputCapture();
    void emitCues(const QList<callsound::Cue> &cues);
    void applyLoop();
    qint64 now() const;
    qreal cueVolume() const;
    qreal ringVolume() const;

    QPointer<SettingsManager> m_settings;
    QPointer<SfuCallController> m_groupCall;
    QPointer<CallController> m_calls;
    std::unique_ptr<CallSoundSink> m_sink;
    callsound::Policy m_policy;
    callsound::Loop m_loop = callsound::Loop::None;
    QString m_ringCallId;
    /// The one call the user silenced (see the header comment).
    QString m_silencedCallId;
    /// A group call that has reached Connected once: a later Connecting is a
    /// RECONNECT, not a fresh join.
    bool m_groupWasConnected = false;
    QTimer m_rosterTimer;
    QTimer m_ringCap;
    QElapsedTimer m_clock;
    std::function<qint64()> m_testClock;
};
