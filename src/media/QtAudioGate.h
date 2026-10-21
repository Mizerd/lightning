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
    /// The last answer: a new audio stream could be started. Asked on demand
    /// (checkNow(), and every start) and while a start waits, never polled
    /// otherwise, so it can be stale; QML that needs the answer now calls
    /// checkNow(), and a player uses play(). Always true where no gate is
    /// needed.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    enum class Mode {
        /// The backend is known not to be PulseAudio: never held.
        Ungated,
        /// The backend is PulseAudio (Qt said so).
        Pulse,
        /// Not known: held only after the PulseAudio thread was seen to go.
        MaybePulse,
    };
    struct Probes {
        /// Qt's PulseAudio mainloop thread is running.
        std::function<bool()> qtThreadRunning;
        /// The PulseAudio server's socket accepts a connection.
        std::function<bool()> serverReachable;
    };

    QtAudioGate(Mode mode, Probes probes, QObject *parent = nullptr);

    /// Creates the process-wide gate, with the production probes and the
    /// mode read from Qt. main() calls it once the application exists; until
    /// then (and in tests that do not) current() is null and nothing is held.
    static QtAudioGate &install();
    /// The gate installed for this process, or null.
    static QtAudioGate *current();

    Mode mode() const { return m_mode; }
    bool ready() const { return m_ready; }

    /// Asks the probes now (a /proc read, and a socket connect only while
    /// Qt's thread runs) and updates `ready`. QML may call it before play().
    Q_INVOKABLE bool checkNow();

    /// Runs `start` on this thread once a new stream is safe: at once when it
    /// is now, otherwise as soon as the gate opens. Runs `giveUp` instead if
    /// it has not opened within `maxWaitMs`. Neither runs if `context` is
    /// destroyed first. Never calls into Qt Multimedia itself.
    void whenReady(QObject *context, std::function<void()> start,
                   std::function<void()> giveUp, int maxWaitMs = kDefaultWaitMs);
    /// Drops every waiting start of `context` (a cancel), running nothing.
    void cancel(QObject *context);

    /// For QML: calls `player`'s play() now, or once the gate opens (a
    /// MediaPlayer, or anything with a play() slot). Given up after
    /// kDefaultWaitMs with playGivenUp(player). Use instead of player.play().
    Q_INVOKABLE void play(QObject *player);
    /// For QML: a pause or stop drops a play() still waiting.
    Q_INVOKABLE void cancelPlay(QObject *player) { cancel(player); }

    static constexpr int kDefaultWaitMs = 8000;

    // Tests.
    void setTimingForTest(int pollMs, int liveMs);
    int pendingForTest() const { return int(m_pending.size()); }
    bool pollingForTest() const { return m_timer.isActive(); }

Q_SIGNALS:
    void readyChanged();
    /// A play() from QML waited kDefaultWaitMs and was dropped.
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
    /// The probes' raw answer, before the liveMs debounce.
    bool probeNow(bool full);
    void setReady(bool ready);
    void poll();
    void schedule();

    Mode m_mode;
    Probes m_probes;
    bool m_ready = true;
    bool m_threadSeen = false;
    /// When the probes last turned from "no" to "yes"; invalid otherwise.
    QElapsedTimer m_yesSince;
    std::vector<Pending> m_pending;
    QTimer m_timer;
    int m_pollMs = 250;
    int m_liveMs = 500;
};

} // namespace lightning::media
