#include "GuiStallTracer.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

Q_LOGGING_CATEGORY(lcGuiStall, "lightning.guistall")

namespace stalltrace {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDefaultThresholdMs = 250;
constexpr int kMinThresholdMs = 50;
constexpr int kBeatIntervalMs = 25;
constexpr int kWatchdogIntervalMs = 50;

std::atomic<qint64> g_lastBeatNs{0};
// Innermost active category. Written only by the GUI thread (Scope), read by
// the watchdog. String literals only — safe to read from any thread at any
// time without lifetime concerns.
std::atomic<const char *> g_category{nullptr};

std::atomic<int> g_stallCount{0};
std::atomic<qint64> g_lastStallMs{0};
std::atomic<const char *> g_lastStallCategory{nullptr};

std::atomic<bool> g_installed{false};

std::thread g_watchdog;
std::mutex g_stopMutex;
std::condition_variable g_stopCv;
bool g_stopRequested = false;

class BeatHost;
BeatHost *g_host = nullptr;

void stopWatchdog();

// The beat timer's owner, parented to the application object so a normal
// shutdown joins the watchdog before Qt tears the event loop down —
// otherwise the dying event loop reads as one long final "stall".
class BeatHost : public QObject
{
public:
    using QObject::QObject;
    ~BeatHost() override
    {
        // Clear the global first so shutdown() cannot double-delete us
        // when the application object destroys its children.
        g_host = nullptr;
        stopWatchdog();
    }
};

qint64 nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void watchdogLoop(qint64 thresholdNs)
{
    qint64 beatAtStallStart = 0;
    const char *categoryAtStall = nullptr;
    bool inStall = false;
    std::unique_lock<std::mutex> lock(g_stopMutex);
    while (!g_stopRequested) {
        g_stopCv.wait_for(lock,
                          std::chrono::milliseconds(kWatchdogIntervalMs));
        if (g_stopRequested)
            break;
        const qint64 lastBeat = g_lastBeatNs.load(std::memory_order_relaxed);
        if (lastBeat == 0)
            continue; // no beat yet — the timer has not started
        if (!inStall) {
            if (nowNs() - lastBeat > thresholdNs) {
                inStall = true;
                beatAtStallStart = lastBeat;
                // Sampled MID-stall: whatever the GUI thread marked before
                // it stopped beating is the section it is stuck in.
                categoryAtStall =
                    g_category.load(std::memory_order_relaxed);
            }
        } else if (lastBeat != beatAtStallStart) {
            // A fresh beat landed: the stall is over. Its duration is the
            // gap between the last beat before the stall and the first one
            // after it (within one beat interval of the truth).
            const qint64 stallMs =
                (lastBeat - beatAtStallStart) / 1000000;
            g_lastStallMs.store(stallMs, std::memory_order_relaxed);
            g_lastStallCategory.store(categoryAtStall,
                                      std::memory_order_relaxed);
            g_stallCount.fetch_add(1, std::memory_order_relaxed);
            qCWarning(lcGuiStall)
                << "GUI stall" << stallMs << "ms category="
                << (categoryAtStall ? categoryAtStall : "unattributed");
            inStall = false;
            categoryAtStall = nullptr;
        }
    }
}

int thresholdFromEnvironment()
{
    const QByteArray raw = qgetenv("LIGHTNING_GUI_STALL_TRACE");
    bool ok = false;
    const int parsed = raw.toInt(&ok);
    if (ok && parsed >= kMinThresholdMs)
        return parsed;
    return kDefaultThresholdMs;
}

} // namespace

bool enabled()
{
    static const bool value = [] {
        const QByteArray raw = qgetenv("LIGHTNING_GUI_STALL_TRACE");
        return !raw.isEmpty() && raw != "0";
    }();
    return value;
}

void install(int thresholdMsOverride)
{
    if (thresholdMsOverride < 0 && !enabled())
        return;
    // Hard requirement, not just an assert: without an application object
    // nothing ever destroys the BeatHost, and an unjoined std::thread
    // terminates the process during static destruction.
    if (!QCoreApplication::instance())
        return;
    if (g_installed.exchange(true))
        return;

    const int thresholdMs = thresholdMsOverride >= kMinThresholdMs
        ? thresholdMsOverride
        : thresholdFromEnvironment();

    auto *host = new BeatHost(QCoreApplication::instance());
    g_host = host;
    auto *beat = new QTimer(host);
    beat->setInterval(kBeatIntervalMs);
    QObject::connect(beat, &QTimer::timeout, host, [] {
        g_lastBeatNs.store(nowNs(), std::memory_order_relaxed);
    });
    beat->start();
    // First beat immediately so the watchdog has a baseline even if the
    // event loop stalls before the first timer tick.
    g_lastBeatNs.store(nowNs(), std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_stopMutex);
        g_stopRequested = false;
    }
    g_watchdog = std::thread(watchdogLoop,
                             qint64(thresholdMs) * 1000000);
    qCInfo(lcGuiStall) << "GUI stall tracing enabled threshold_ms="
                       << thresholdMs;
}

namespace {
void stopWatchdog()
{
    if (!g_installed.load())
        return;
    {
        std::lock_guard<std::mutex> lock(g_stopMutex);
        g_stopRequested = true;
    }
    g_stopCv.notify_all();
    if (g_watchdog.joinable())
        g_watchdog.join();
    g_installed.store(false);
    g_lastBeatNs.store(0, std::memory_order_relaxed);
}
} // namespace

void shutdown()
{
    stopWatchdog();
    // Also retire the beat timer (its host), so an install() after a
    // shutdown() does not accumulate timers still stamping beats.
    if (g_host) {
        BeatHost *host = g_host;
        g_host = nullptr; // the destructor re-checks; avoid double delete
        delete host;
    }
}

int stallCount()
{
    return g_stallCount.load(std::memory_order_relaxed);
}

qint64 lastStallMs()
{
    return g_lastStallMs.load(std::memory_order_relaxed);
}

QByteArray lastStallCategory()
{
    const char *category =
        g_lastStallCategory.load(std::memory_order_relaxed);
    return category ? QByteArray(category) : QByteArray();
}

Scope::Scope(const char *category)
{
    if (!g_installed.load(std::memory_order_relaxed))
        return;
    m_active = true;
    m_previous = g_category.load(std::memory_order_relaxed);
    g_category.store(category, std::memory_order_relaxed);
}

Scope::~Scope()
{
    if (m_active)
        g_category.store(m_previous, std::memory_order_relaxed);
}

} // namespace stalltrace
