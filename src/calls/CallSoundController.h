// Plays Lightning's call sounds on the right events.
//
// Decisions belong to CallSoundPolicy. This class watches both call lanes
// (SfuCallController for group calls, CallController for 1:1), translates
// their state for the policy, and passes the result to an injected
// CallSoundSink: CallSoundPlayer in the app, a recorder in tests, or none
// (decide everything, play nothing, open no audio device).
//
// The incoming ring is announced, not inferred: AppController decides
// whether a call rings (the same gates as the notification card) and calls
// startIncomingRing(). The ring stops here as soon as CallController is no
// longer ringing that call, so no ending path can leave it playing.
//
// Silence is per call: silenceRing() stops the ringer for the call ringing now
// and remembers its id, so re-announcing the same call stays silent while a
// new call rings normally. The call itself can still be answered or declined.
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

/// Where the sounds go. Names are CallSoundPolicy's soundName()s; volumes are
/// perceptual 0..1 (the sink maps them to linear gain).
class CallSoundSink
{
public:
    virtual ~CallSoundSink() = default;
    /// Play `sound` once; `inCall` routes it to the call's output device
    /// rather than the system default.
    virtual void play(const QString &sound, qreal volume, bool inCall) = 0;
    /// Loop `sound` until replaced; an empty name stops the loop.
    virtual void loop(const QString &sound, qreal volume, bool inCall) = 0;
    /// Whether `sound` has loaded and can be heard. False while unknown, so a
    /// caller keeps its fallback until this says yes.
    virtual bool canPlay(const QString &sound) const = 0;
};

class CallSoundController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallSoundController is exposed via app.callSounds")
    /// The call Lightning's own ringer is sounding for, or "" (including when
    /// the desktop's themed sound is ringing, and after silencing).
    /// IncomingCallPrompt offers Silence only while this names the ringing
    /// call.
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

    /// True once Lightning's own ringer has loaded; until then the desktop's
    /// themed call sound is kept, so a machine without Qt Multimedia output
    /// still rings.
    bool ringerAvailable() const;
    /// AppController decided this call rings.
    void startIncomingRing(const QString &callId);
    void stopIncomingRing();
    QString ringingCallId() const { return m_ringCallId; }
    /// Silence the ring for `callId` only; false unless it is the call ringing
    /// now, so a stale or crafted id cannot silence the next call. Stops our
    /// ringer and emits ringSilenced so the card drops its themed sound.
    Q_INVOKABLE bool silenceRing(const QString &callId);
    /// Whether `callId` was silenced; checked before (re-)announcing a call.
    bool isRingSilenced(const QString &callId) const
    {
        return !callId.isEmpty() && callId == m_silencedCallId;
    }

    /// Settings "Test" buttons: "ring" plays the ringer at ringer volume,
    /// anything else one cue at call-sound volume, even with the switches off.
    Q_INVOKABLE void preview(const QString &sound);

    /// Tests: a deterministic millisecond clock.
    void setClockForTest(std::function<qint64()> clock);
    const callsound::Policy &policy() const { return m_policy; }

Q_SIGNALS:
    void ringingCallIdChanged();
    /// Emitted after the state is recorded, so isRingSilenced() is already
    /// true in handlers.
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
    /// The one call the user silenced.
    QString m_silencedCallId;
    /// Set once a group call reaches Connected; a later Connecting is a
    /// reconnect, not a fresh join.
    bool m_groupWasConnected = false;
    QTimer m_rosterTimer;
    QTimer m_ringCap;
    QElapsedTimer m_clock;
    std::function<qint64()> m_testClock;
};
