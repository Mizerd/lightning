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

    /// Settings "Test" buttons: "ring" plays one bar of the ringer at the
    /// ringer volume, anything else one cue at the call-sound volume. Plays
    /// even with the switches off — the user asked to hear it.
    Q_INVOKABLE void preview(const QString &sound);

    /// Tests: a deterministic clock (milliseconds).
    void setClockForTest(std::function<qint64()> clock);
    const callsound::Policy &policy() const { return m_policy; }

private:
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
    /// A group call that has reached Connected once: a later Connecting is a
    /// RECONNECT, not a fresh join.
    bool m_groupWasConnected = false;
    QTimer m_rosterTimer;
    QTimer m_ringCap;
    QElapsedTimer m_clock;
    std::function<qint64()> m_testClock;
};
