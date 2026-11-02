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

class CallSoundSink
{
public:
    virtual ~CallSoundSink() = default;
    virtual void play(const QString &sound, qreal volume, bool inCall) = 0;
    virtual void loop(const QString &sound, qreal volume, bool inCall) = 0;
    virtual bool canPlay(const QString &sound) const = 0;
};

class CallSoundController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallSoundController is exposed via app.callSounds")
    Q_PROPERTY(QString ringingCallId READ ringingCallId NOTIFY
                   ringingCallIdChanged)

public:
    explicit CallSoundController(QObject *parent = nullptr);
    ~CallSoundController() override;

    void setSettings(SettingsManager *settings);
    void setGroupCall(SfuCallController *groupCall);
    void setLegacyCalls(CallController *calls);
    void setSink(std::unique_ptr<CallSoundSink> sink);
    CallSoundSink *sink() const { return m_sink.get(); }

    /// True once Lightning's own ringer has loaded; until then the desktop's
    /// themed call sound is kept, so a machine without Qt Multimedia output
    /// still rings.
    bool ringerAvailable() const;
    void startIncomingRing(const QString &callId);
    void stopIncomingRing();
    QString ringingCallId() const { return m_ringCallId; }
    /// Silence the ring for `callId` only; false unless it is the call ringing
    /// now, so a stale or crafted id cannot silence the next call. Stops our
    /// ringer and emits ringSilenced so the card drops its themed sound.
    Q_INVOKABLE bool silenceRing(const QString &callId);
    Q_INVOKABLE bool dismissRing(const QString &callId);
    bool isRingSilenced(const QString &callId) const
    {
        return !callId.isEmpty() && callId == m_silencedCallId;
    }

    Q_INVOKABLE void preview(const QString &sound);

    bool playNotificationSound(bool mention);
    Q_INVOKABLE void previewNotification(bool mention);

    void setClockForTest(std::function<qint64()> clock);
    const callsound::Policy &policy() const { return m_policy; }

Q_SIGNALS:
    void ringingCallIdChanged();
    void ringSilenced(const QString &callId);
    void ringDismissed(const QString &callId);

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
    qreal notificationVolume() const;

    QPointer<SettingsManager> m_settings;
    QPointer<SfuCallController> m_groupCall;
    QPointer<CallController> m_calls;
    std::unique_ptr<CallSoundSink> m_sink;
    callsound::Policy m_policy;
    callsound::Loop m_loop = callsound::Loop::None;
    QString m_ringCallId;
    QString m_silencedCallId;
    bool m_groupWasConnected = false;
    QTimer m_rosterTimer;
    QTimer m_ringCap;
    QElapsedTimer m_clock;
    std::function<qint64()> m_testClock;
};
