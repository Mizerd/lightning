#include "media/QtAudioGate.h"

#include "calls/PulseServerProbe.h"

#include <QAudioDevice>
#include <QLoggingCategory>
#include <QMediaDevices>

#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
#include <QtMultimedia/private/qplatformmediaintegration_p.h>
#endif

#include <utility>

Q_LOGGING_CATEGORY(lcQtAudioGate, "lightning.media.audiogate")

namespace lightning::media {

QtAudioGate::QtAudioGate(Mode mode, Probes probes, QObject *parent)
    : QObject(parent)
    , m_mode(mode)
    , m_probes(std::move(probes))
{
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &QtAudioGate::poll);
    // No debounce for the first answer: there was no outage to recover from.
    m_ready = probeNow(true);
    schedule();
}

namespace {
QtAudioGate *g_installed = nullptr;
} // namespace

QtAudioGate *QtAudioGate::current()
{
    return g_installed;
}

QtAudioGate &QtAudioGate::install()
{
    static QtAudioGate *gate = [] {
        Mode mode = Mode::Ungated;
#if defined(Q_OS_LINUX)
        // Initialises Qt's audio backend (its PulseAudio backend connects
        // here), as the call sounds already do at start-up, so the thread
        // the probe looks for exists when the backend is PulseAudio.
        (void)QMediaDevices::defaultAudioOutput();
        mode = Mode::MaybePulse;
#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
        if (qstrcmp(qVersion(), QT_VERSION_STR) == 0) {
            mode = QString(QPlatformMediaIntegration::audioBackendName())
                       == QLatin1String("PulseAudio")
                ? Mode::Pulse
                : Mode::Ungated;
        }
#endif
#endif
        Probes probes{[] { return lightning::calls::qtPulseThreadRunning(); },
                      [] { return lightning::calls::pulseServerReachable(); }};
        auto *created = new QtAudioGate(mode, std::move(probes));
        qCInfo(lcQtAudioGate).noquote()
            << QStringLiteral("qt audio gate: %1, ready=%2")
                   .arg(mode == Mode::Pulse
                            ? QStringLiteral("PulseAudio backend, new streams "
                                             "wait for its connection")
                            : mode == Mode::MaybePulse
                            ? QStringLiteral("backend not known, held only if "
                                             "Qt's PulseAudio connection drops")
                            : QStringLiteral("not PulseAudio, never held"),
                        created->ready() ? QStringLiteral("yes")
                                         : QStringLiteral("no"));
        return created;
    }();
    g_installed = gate;
    return *gate;
}

bool QtAudioGate::probeNow(bool full)
{
    if (m_mode == Mode::Ungated)
        return true;
    const bool thread = m_probes.qtThreadRunning && m_probes.qtThreadRunning();
    if (thread)
        m_threadSeen = true;
    if (!thread)
        return m_mode == Mode::MaybePulse && !m_threadSeen;
    if (!full)
        return true;
    return m_probes.serverReachable && m_probes.serverReachable();
}

void QtAudioGate::setReady(bool ready)
{
    if (ready == m_ready)
        return;
    m_ready = ready;
    qCInfo(lcQtAudioGate).noquote()
        << (ready ? QStringLiteral("qt audio gate: Qt's PulseAudio connection "
                                   "is back; new audio streams may start")
                  : QStringLiteral("qt audio gate: Qt's PulseAudio connection "
                                   "is down; new audio streams wait"));
    Q_EMIT readyChanged();
}

bool QtAudioGate::checkNow()
{
    const bool yes = probeNow(true);
    if (!yes) {
        m_yesSince.invalidate();
        setReady(false);
    } else if (!m_ready) {
        if (!m_yesSince.isValid())
            m_yesSince.start();
        if (m_yesSince.elapsed() >= m_liveMs)
            setReady(true);
    }
    schedule();
    return m_ready;
}

void QtAudioGate::whenReady(QObject *context, std::function<void()> start,
                            std::function<void()> giveUp, int maxWaitMs)
{
    if (checkNow()) {
        if (start)
            start();
        return;
    }
    Pending pending;
    pending.context = context;
    pending.hadContext = context != nullptr;
    pending.start = std::move(start);
    pending.giveUp = std::move(giveUp);
    pending.maxWaitMs = maxWaitMs;
    pending.waited.start();
    m_pending.push_back(std::move(pending));
    qCInfo(lcQtAudioGate) << "qt audio gate: an audio start waits for Qt's "
                             "PulseAudio connection (at most"
                          << maxWaitMs << "ms)";
    schedule();
}

void QtAudioGate::cancel(QObject *context)
{
    std::erase_if(m_pending, [context](const Pending &p) {
        return p.context == context;
    });
    schedule();
}

void QtAudioGate::play(QObject *player)
{
    if (!player)
        return;
    cancel(player); // the newest request wins
    QPointer<QObject> target(player);
    whenReady(
        player,
        [target] {
            if (target)
                QMetaObject::invokeMethod(target, "play", Qt::DirectConnection);
        },
        [this, target] {
            if (target)
                Q_EMIT playGivenUp(target);
        });
}

void QtAudioGate::poll()
{
    checkNow();
    if (m_pending.empty()) {
        schedule();
        return;
    }
    // Taken out first: a callback may start another wait.
    std::vector<Pending> due;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const bool gone = it->hadContext && !it->context;
        const bool expired = it->waited.elapsed() >= it->maxWaitMs;
        if (gone || m_ready || expired) {
            if (!gone)
                due.push_back(std::move(*it));
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    schedule();
    for (Pending &p : due) {
        if (p.hadContext && !p.context)
            continue;
        if (m_ready) {
            if (p.start)
                p.start();
        } else {
            qCWarning(lcQtAudioGate)
                << "qt audio gate: Qt's PulseAudio connection did not come "
                   "back in"
                << p.maxWaitMs << "ms; the audio start was given up";
            if (p.giveUp)
                p.giveUp();
        }
    }
}

void QtAudioGate::schedule()
{
    // Polled only while a start waits: otherwise every answer is asked for on
    // demand (checkNow(), whenReady(), play()), so an idle process pays
    // nothing, and `ready` is the last answer given.
    if (m_mode == Mode::Ungated || m_pending.empty()) {
        m_timer.stop();
        return;
    }
    if (!m_timer.isActive() || m_timer.interval() != m_pollMs)
        m_timer.start(m_pollMs);
}

void QtAudioGate::setTimingForTest(int pollMs, int liveMs)
{
    m_pollMs = pollMs;
    m_liveMs = liveMs;
    schedule();
}

} // namespace lightning::media
