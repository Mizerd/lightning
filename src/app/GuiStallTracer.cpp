#include "GuiStallTracer.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QObject>
#include <QThread>
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
// Written by the GUI thread, read by the watchdog. String literals only, so
// no lifetime concerns.
std::atomic<const char *> g_category{nullptr};

std::atomic<int> g_stallCount{0};
std::atomic<qint64> g_lastStallMs{0};
std::atomic<const char *> g_lastStallCategory{nullptr};

std::atomic<bool> g_installed{false};
// Scopes on other threads are inert, or a GUI stall would be attributed to
// whatever a worker happened to be doing.
std::atomic<QThread *> g_guiThread{nullptr};

std::thread g_watchdog;
std::mutex g_stopMutex;
std::condition_variable g_stopCv;
bool g_stopRequested = false;

class BeatHost;
BeatHost *g_host = nullptr;

void stopWatchdog();

// Parented to the application so shutdown joins the watchdog before the
// event loop dies (which would otherwise read as one final stall).
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
                // Sampled mid-stall: the section the GUI thread is stuck in.
                categoryAtStall =
                    g_category.load(std::memory_order_relaxed);
            }
        } else if (lastBeat != beatAtStallStart) {
            // The stall is over; its duration is accurate to one beat.
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
    // Without an application object the watchdog thread is never joined and
    // terminates the process at static destruction.
    if (!QCoreApplication::instance())
        return;
    if (g_installed.exchange(true))
        return;

    const int thresholdMs = thresholdMsOverride >= kMinThresholdMs
        ? thresholdMsOverride
        : thresholdFromEnvironment();

    g_guiThread.store(QCoreApplication::instance()->thread(),
                      std::memory_order_relaxed);
    auto *host = new BeatHost(QCoreApplication::instance());
    g_host = host;
    auto *beat = new QTimer(host);
    beat->setInterval(kBeatIntervalMs);
    QObject::connect(beat, &QTimer::timeout, host, [] {
        g_lastBeatNs.store(nowNs(), std::memory_order_relaxed);
    });
    beat->start();
    // Beat now so the watchdog has a baseline before the first tick.
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
    // Retire the beat timer too, so a re-install does not stack timers.
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
    // Inert off the GUI thread, so it is safe in code that runs on either.
    if (QThread::currentThread()
        != g_guiThread.load(std::memory_order_relaxed))
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
