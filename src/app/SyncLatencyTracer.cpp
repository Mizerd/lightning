#include "app/SyncLatencyTracer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <limits>

Q_LOGGING_CATEGORY(lcSyncTrace, "lightning.sync.trace")

namespace {

constexpr int kDefaultSlowMs = 2000;
constexpr int kMinSlowMs = 100;
// Bounded: a burst of a thousand events must not turn the tracer into the
// memory problem it exists to diagnose. Oldest journeys are dropped.
constexpr int kMaxTrackedJourneys = 512;

struct Journey {
    quint64 id = 0;
    QByteArray roomKey;
    qint64 sdkMs = 0;
    qint64 bridgeMs = -1;
    qint64 modelMs = -1;
};

struct State {
    QMutex mutex;
    QHash<quint64, Journey> journeys;
    quint64 nextId = 1;
    qint64 lastSyncResponseMs = -1;
    int completed = 0;
    int stalls = 0;
    qint64 lastTotalMs = 0;
    int thresholdMs = kDefaultSlowMs;
};

State &state()
{
    static State s;
    return s;
}

// Wall clock, deliberately: the SDK stage is stamped in RUST, and a
// QElapsedTimer's origin is not shared across that boundary. Both sides use
// milliseconds since the Unix epoch in the SAME process, so the delta is
// meaningful. Caveat, stated rather than hidden: a system clock step during a
// capture skews one interval. That is acceptable for a coarse diagnostic and
// is why the thresholds are seconds, not milliseconds.
qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

std::atomic<int> g_enabled{-1};   // -1 unread, 0 off, >0 threshold

int resolveEnabled()
{
    const QByteArray raw = qgetenv("LIGHTNING_SYNC_TRACE");
    if (raw.isEmpty())
        return 0;
    bool ok = false;
    const int value = raw.toInt(&ok);
    if (ok && value >= kMinSlowMs)
        return value;
    // "1" (or any true-ish non-threshold value) enables with the default.
    if (raw == "1" || raw.compare("true", Qt::CaseInsensitive) == 0
        || raw.compare("on", Qt::CaseInsensitive) == 0
        || raw.compare("yes", Qt::CaseInsensitive) == 0)
        return kDefaultSlowMs;
    return 0;
}

int enabledThreshold()
{
    int cached = g_enabled.load(std::memory_order_relaxed);
    if (cached < 0) {
        cached = resolveEnabled();
        g_enabled.store(cached, std::memory_order_relaxed);
        if (cached > 0) {
            State &s = state();
            QMutexLocker lock(&s.mutex);
            s.thresholdMs = cached;
        }
    }
    return cached;
}

// A room KEY, not a room id: eight hex characters of a SHA-256, enough to tell
// two rooms apart in one capture and useless for identifying either. The same
// discipline the support-diagnostics export uses.
QByteArray roomKeyFor(const QString &roomId)
{
    if (roomId.isEmpty())
        return QByteArrayLiteral("--------");
    return QCryptographicHash::hash(roomId.toUtf8(), QCryptographicHash::Sha256)
        .toHex()
        .left(8);
}

} // namespace

bool synctrace::enabled()
{
    return enabledThreshold() > 0;
}

int synctrace::slowThresholdMs()
{
    const int t = enabledThreshold();
    return t > 0 ? t : kDefaultSlowMs;
}

quint64 synctrace::beginEvent(const QString &roomId, qint64 sdkEpochMs)
{
    if (!enabled())
        return 0;
    State &s = state();
    QMutexLocker lock(&s.mutex);
    // Drop the oldest rather than grow without bound. A dropped journey simply
    // never reports; it is diagnostics, not bookkeeping anything depends on.
    if (s.journeys.size() >= kMaxTrackedJourneys) {
        quint64 oldest = 0;
        qint64 oldestMs = std::numeric_limits<qint64>::max();
        for (auto it = s.journeys.constBegin(); it != s.journeys.constEnd(); ++it) {
            if (it.value().sdkMs < oldestMs) {
                oldestMs = it.value().sdkMs;
                oldest = it.key();
            }
        }
        if (oldest != 0)
            s.journeys.remove(oldest);
    }
    Journey j;
    j.id = s.nextId++;
    j.roomKey = roomKeyFor(roomId);
    // A stamp from the far side of the FFI when we have one. Guarded against a
    // clock that disagrees: a "future" stamp would render every later delta
    // negative and read as an impossible journey, so it degrades to now.
    const qint64 now = nowMs();
    j.sdkMs = (sdkEpochMs > 0 && sdkEpochMs <= now) ? sdkEpochMs : now;
    s.journeys.insert(j.id, j);
    return j.id;
}

void synctrace::noteBridge(quint64 id)
{
    if (id == 0 || !enabled())
        return;
    State &s = state();
    QMutexLocker lock(&s.mutex);
    auto it = s.journeys.find(id);
    if (it == s.journeys.end())
        return;
    it->bridgeMs = nowMs();
}

void synctrace::noteModel(quint64 id)
{
    if (id == 0 || !enabled())
        return;
    State &s = state();
    QMutexLocker lock(&s.mutex);
    auto it = s.journeys.find(id);
    if (it == s.journeys.end())
        return;
    it->modelMs = nowMs();
}

void synctrace::noteUi(quint64 id)
{
    if (id == 0 || !enabled())
        return;
    State &s = state();
    Journey j;
    qint64 uiMs = 0;
    int threshold = 0;
    {
        QMutexLocker lock(&s.mutex);
        auto it = s.journeys.find(id);
        if (it == s.journeys.end())
            return;
        j = *it;
        s.journeys.erase(it);
        uiMs = nowMs();
        threshold = s.thresholdMs;
        s.completed++;
        s.lastTotalMs = uiMs - j.sdkMs;
    }

    // Per-stage deltas. A stage that never fired reports -1 rather than a
    // fabricated 0 — "we did not observe this" and "it took no time" are
    // different facts, and conflating them is how an instrument lies.
    const qint64 toBridge = j.bridgeMs >= 0 ? j.bridgeMs - j.sdkMs : -1;
    const qint64 toModel =
        (j.modelMs >= 0 && j.bridgeMs >= 0) ? j.modelMs - j.bridgeMs : -1;
    const qint64 toUi = j.modelMs >= 0 ? uiMs - j.modelMs : -1;
    const qint64 total = uiMs - j.sdkMs;

    const bool slow = total >= threshold;
    if (slow) {
        QMutexLocker lock(&s.mutex);
        s.stalls++;
    }

    // sdk -> bridge -> model -> ui, with the elapsed time between each pair.
    // Identifiers are a correlation id and a hashed room key; nothing here is
    // derived from the event's content.
    if (slow) {
        qCWarning(lcSyncTrace).nospace()
            << "SLOW event id=" << j.id << " room=" << j.roomKey
            << " sdk->bridge=" << toBridge << "ms"
            << " bridge->model=" << toModel << "ms"
            << " model->ui=" << toUi << "ms"
            << " total=" << total << "ms";
    } else {
        qCDebug(lcSyncTrace).nospace()
            << "event id=" << j.id << " room=" << j.roomKey
            << " sdk->bridge=" << toBridge << "ms"
            << " bridge->model=" << toModel << "ms"
            << " model->ui=" << toUi << "ms"
            << " total=" << total << "ms";
    }
}

void synctrace::noteSyncResponse()
{
    if (!enabled())
        return;
    State &s = state();
    qint64 gap = -1;
    int threshold = 0;
    {
        QMutexLocker lock(&s.mutex);
        const qint64 now = nowMs();
        if (s.lastSyncResponseMs >= 0)
            gap = now - s.lastSyncResponseMs;
        s.lastSyncResponseMs = now;
        threshold = s.thresholdMs;
    }
    if (gap < 0 || gap < threshold)
        return;
    {
        State &st = state();
        QMutexLocker lock(&st.mutex);
        st.stalls++;
    }
    // THE line to look for when chasing the minute-long lag. Sliding sync
    // issues its long poll with a 60 s request timeout (30 s poll + 30 s
    // network, matrix-sdk 0.18 defaults), so a silently dead connection
    // produces a gap of about 60 000 ms here — with no event journey beside
    // it, because nothing was delivered during the wait.
    qCWarning(lcSyncTrace).nospace()
        << "SLOW sync gap=" << gap << "ms"
        << " (a gap near 60000ms with no event journeys is the sliding-sync "
           "dead-connection timeout: poll 30s + network 30s)";
}

void synctrace::noteSyncState(const char *stateName)
{
    if (!enabled() || !stateName)
        return;
    qCDebug(lcSyncTrace).nospace() << "sync state=" << stateName;
}

int synctrace::completedJourneys()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.completed;
}

int synctrace::reportedStalls()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.stalls;
}

qint64 synctrace::lastJourneyTotalMs()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.lastTotalMs;
}

void synctrace::resetForTest(int thresholdMsOverride)
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    s.journeys.clear();
    s.nextId = 1;
    s.lastSyncResponseMs = -1;
    s.completed = 0;
    s.stalls = 0;
    s.lastTotalMs = 0;
    if (thresholdMsOverride > 0) {
        s.thresholdMs = thresholdMsOverride;
        g_enabled.store(thresholdMsOverride, std::memory_order_relaxed);
    } else {
        s.thresholdMs = kDefaultSlowMs;
        g_enabled.store(0, std::memory_order_relaxed);
    }
}
