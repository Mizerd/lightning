// Whether Qt Multimedia can take a NEW audio stream right now without taking
// the process down, and a way to wait for it.
//
// Qt's PulseAudio backend (which Lightning asks for on Linux with Qt 6.10+,
// QtAudioBackendChoice.h) drops its server connection when the server goes
// away and retries about 3 s later; a retry whose connect fails outright
// schedules nothing more. A QAudioSink or QAudioSource started while it has
// no connection asserts in libpulse (pa_context_get_state(NULL) /
// pa_stream_new(NULL), qpulseaudiosink.cpp, qpulseaudiosource.cpp): SIGABRT,
// measured for the call sounds (CallSoundMixer.h, commit 1efee79c). Every
// QMediaPlayer with an audio output and every QMediaRecorder with an audio
// input opens one of those streams, so every place that starts audio asks
// here first: the voice player and recorder in C++, and the QML players
// through the `qtAudio` context property (`qtAudio.play(player)` instead of
// `player.play()`, `qtAudio.cancelPlay(player)` on pause or stop).
//
// Qt cannot be asked (its own state getter asserts the same way). What is
// asked instead (PulseServerProbe.h): Qt's PulseAudio mainloop thread runs,
// which it does exactly while Qt holds a connection, and then the server's
// socket accepts. Asked in that order, so a Qt that gave up never costs a
// connection to the server. A "yes" after a "no" must hold for liveMs before
// the gate opens again.
//
// Where Qt does not say which backend it chose, the gate closes only on
// evidence: once the PulseAudio thread has been seen and is gone. A process
// on another backend (PipeWire, ALSA) never sees it and is never held.
//
// Known limits, accepted: the answer is a check, not a lock, so a connection
// that drops between the check and Qt opening the stream (a few ms) still
// aborts; and a stream Qt re-opens by itself (its FFmpeg renderer after an
// output change) is not asked about at all. With the backend unknown and the
// thread not yet seen, a start before Qt's first connection is not held.
//
// GUI thread only.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <vector>

namespace lightning::media {

class QtAudioGate : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    enum class Mode {
        Ungated,
        Pulse,
        MaybePulse,
    };
    struct Probes {
        std::function<bool()> qtThreadRunning;
        std::function<bool()> serverReachable;
    };

    QtAudioGate(Mode mode, Probes probes, QObject *parent = nullptr);

    static QtAudioGate &install();
    static QtAudioGate *current();

    Mode mode() const { return m_mode; }
    bool ready() const { return m_ready; }

    Q_INVOKABLE bool checkNow();

    void whenReady(QObject *context, std::function<void()> start,
                   std::function<void()> giveUp, int maxWaitMs = kDefaultWaitMs);
    void cancel(QObject *context);

    Q_INVOKABLE void play(QObject *player);
    Q_INVOKABLE void cancelPlay(QObject *player) { cancel(player); }

    static constexpr int kDefaultWaitMs = 8000;

    void setTimingForTest(int pollMs, int liveMs);
    int pendingForTest() const { return int(m_pending.size()); }
    bool pollingForTest() const { return m_timer.isActive(); }

Q_SIGNALS:
    void readyChanged();
    void playGivenUp(QObject *player);

private:
    struct Pending {
        QPointer<QObject> context;
        bool hadContext = false;
        std::function<void()> start;
        std::function<void()> giveUp;
        QElapsedTimer waited;
        int maxWaitMs = kDefaultWaitMs;
    };
    bool probeNow(bool full);
    void setReady(bool ready);
    void poll();
    void schedule();

    Mode m_mode;
    Probes m_probes;
    bool m_ready = true;
    bool m_threadSeen = false;
    QElapsedTimer m_yesSince;
    std::vector<Pending> m_pending;
    QTimer m_timer;
    int m_pollMs = 250;
    int m_liveMs = 500;
};

}
