#include "calls/WebrtcRetirer.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <QLoggingCategory>
#include <QThread>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

namespace lightning::webrtc {

struct GateState {
    bool retired = false;
    int inflight = 0;
};

namespace {

Q_LOGGING_CATEGORY(lcRetire, "lightning.calls.webrtc")

// One lock for every gate: it is held only to count, never across a callback.
std::mutex g_gateMutex;
std::condition_variable g_gateCv;
// Scopes open on this thread, so a callback that retires cannot wait for
// itself.
thread_local int t_openScopes = 0;

GQuark gateQuark()
{
    static const GQuark quark =
        g_quark_from_static_string("lightning-webrtc-gate");
    return quark;
}

// The holder is written once before any callback can run and freed with the
// element, so copying the pointer out needs no lock.
std::shared_ptr<GateState> gateOf(GstElement *webrtc)
{
    if (!webrtc)
        return {};
    auto *holder = static_cast<std::shared_ptr<GateState> *>(
        g_object_get_qdata(G_OBJECT(webrtc), gateQuark()));
    return holder ? *holder : nullptr;
}

// Marks the element retired and waits for callbacks already inside. Returns
// false when some were still running at the end of the wait.
bool closeGate(GstElement *webrtc, int waitMs)
{
    const std::shared_ptr<GateState> state = gateOf(webrtc);
    if (!state)
        return true;
    std::unique_lock<std::mutex> lock(g_gateMutex);
    state->retired = true;
    if (t_openScopes > 0)
        return state->inflight == 0;
    return g_gateCv.wait_for(lock, std::chrono::milliseconds(waitMs),
                             [&state] { return state->inflight == 0; });
}

using ReplyCount = std::shared_ptr<std::atomic<int>>;

void barrierReplied(GstPromise *, gpointer data)
{
    (*static_cast<ReplyCount *>(data))->fetch_add(1);
}

void barrierFree(gpointer data)
{
    delete static_cast<ReplyCount *>(data);
}

// FLUSH_START on every sink pad: it marks the pad flushing and wakes a
// thread blocked in a probe there, without waiting for anything itself.
void flushSinkPads(GstElement *webrtc)
{
    GstIterator *it = gst_element_iterate_sink_pads(webrtc);
    if (!it)
        return;
    GValue item = G_VALUE_INIT;
    bool done = false;
    // Bounded resyncs: pads are only added by our own publishes, which have
    // stopped.
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK:
            gst_pad_send_event(GST_PAD(g_value_get_object(&item)),
                               gst_event_new_flush_start());
            g_value_reset(&item);
            break;
        case GST_ITERATOR_RESYNC:
            if (resyncsLeft-- <= 0) {
                done = true;
                break;
            }
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

GstWebRTCICEGatheringState gatheringState(GstElement *webrtc)
{
    GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);
    return state;
}

struct StopJob {
    GstElement *pipeline = nullptr; // owns a ref
    GstElement *webrtc = nullptr;   // owns a ref
    std::shared_ptr<RetireStats> stats;
};

void stopNow(StopJob *job)
{
    // Sampled where the NULL is issued: this is the window the crash needs.
    if (gatheringState(job->webrtc) == GST_WEBRTC_ICE_GATHERING_STATE_GATHERING)
        job->stats->whileGathering.fetch_add(1);
    gst_element_set_state(job->webrtc, GST_STATE_NULL);
    gst_object_unref(job->webrtc);
    job->webrtc = nullptr;
    // The rest of the pipeline went to NULL in retire(); this ref was only
    // keeping webrtcbin's parent alive.
    if (job->pipeline) {
        gst_object_unref(job->pipeline);
        job->pipeline = nullptr;
    }
    job->stats->stopped.fetch_add(1);
    job->stats->retiring.fetch_sub(1);
}

void runStopJob(GstElement *, gpointer data)
{
    stopNow(static_cast<StopJob *>(data));
}

void stopJobFree(gpointer data)
{
    auto *job = static_cast<StopJob *>(data);
    // Set only when the job never ran.
    if (job->webrtc)
        gst_object_unref(job->webrtc);
    if (job->pipeline)
        gst_object_unref(job->pipeline);
    delete job;
}

} // namespace

void installGate(GstElement *webrtc)
{
    if (!webrtc)
        return;
    g_object_set_qdata_full(
        G_OBJECT(webrtc), gateQuark(),
        new std::shared_ptr<GateState>(std::make_shared<GateState>()),
        [](gpointer data) {
            delete static_cast<std::shared_ptr<GateState> *>(data);
        });
}

CallbackScope::CallbackScope(GstElement *webrtc)
{
    std::shared_ptr<GateState> state = gateOf(webrtc);
    if (!state)
        return; // not gated: always live
    std::lock_guard<std::mutex> lock(g_gateMutex);
    if (state->retired) {
        m_live = false;
        return;
    }
    ++state->inflight;
    m_state = std::move(state);
    ++t_openScopes;
}

CallbackScope::~CallbackScope()
{
    if (!m_state)
        return;
    --t_openScopes;
    {
        std::lock_guard<std::mutex> lock(g_gateMutex);
        --m_state->inflight;
    }
    g_gateCv.notify_all();
}

Retirer::Retirer()
{
    m_timer.setInterval(20);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { poll(); });
}

Retirer::~Retirer()
{
    drain(kQuitBudgetMs);
}

void Retirer::retire(GstElement *pipeline, GstElement *webrtc)
{
    if (!webrtc) {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        return;
    }
    m_stats->retiring.fetch_add(1);
    if (!closeGate(webrtc, kCallbackWaitMs)) {
        qCWarning(lcRetire) << "a callback on a closed call's webrtcbin was "
                               "still running after"
                            << kCallbackWaitMs << "ms";
    }
    if (pipeline) {
        // Everything else stops now, exactly as before: devices, probes and
        // sinks of ours. Only webrtcbin keeps running.
        gst_element_set_locked_state(webrtc, TRUE);
        // webrtcbin blocks each sink pad until its transceiver negotiates, so
        // an unanswered offer leaves our send chain's streaming thread parked
        // there holding our pads' stream locks. The pipeline NULL below waits
        // for those locks, and webrtcbin, which is not changing state, would
        // never release them: flush its sink pads first. (Measured: both call
        // suites hung here.)
        flushSinkPads(webrtc);
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    Entry entry;
    entry.pipeline = pipeline;
    entry.webrtc = webrtc;
    entry.replied = std::make_shared<std::atomic<int>>(0);
    entry.age.start();
    sendBarrier(entry);
    m_entries.push_back(std::move(entry));
    if (!m_timer.isActive())
        m_timer.start();
}

void Retirer::sendBarrier(Entry &entry)
{
    // get-stats runs on webrtcbin's own thread behind every task already
    // queued there, and replies (with an error) even once it is closed.
    GstPromise *promise = gst_promise_new_with_change_func(
        barrierReplied, new ReplyCount(entry.replied), barrierFree);
    ++entry.sent;
    g_signal_emit_by_name(entry.webrtc, "get-stats", nullptr, promise);
    gst_promise_unref(promise);
}

Retirer::Step Retirer::advance(Entry &entry)
{
    if (entry.replied->load() < entry.sent)
        return Step::Wait;
    // Two barriers. The first runs after any queued set-local-description,
    // which starts gathering; the GATHERING state that start produces is
    // published by a task queued behind it, which the second one waits out.
    if (entry.sent < 2) {
        sendBarrier(entry);
        return Step::Wait;
    }
    if (gatheringState(entry.webrtc) != GST_WEBRTC_ICE_GATHERING_STATE_GATHERING)
        return Step::Ready;
    if (!entry.waitCounted) {
        entry.waitCounted = true;
        m_stats->waitedOnGathering.fetch_add(1);
        qCInfo(lcRetire) << "a closed call's ICE gathering is still running; "
                            "its webrtcbin stops when that ends";
    }
    return Step::Wait;
}

void Retirer::finish(Entry &entry, bool expired, bool async)
{
    if (expired) {
        m_stats->boundExpired.fetch_add(1);
        qCWarning(lcRetire)
            << "a closed call's webrtcbin is stopped with ICE gathering "
               "unfinished after"
            << entry.age.elapsed()
            << "ms; the GStreamer teardown crash (gstreamer#5138) is "
               "possible here";
    }
    auto *job = new StopJob;
    job->pipeline = entry.pipeline;
    job->webrtc = entry.webrtc;
    job->stats = m_stats;
    entry.pipeline = nullptr;
    entry.webrtc = nullptr;
    if (async) {
        // Off this thread: webrtcbin's NULL closes the ICE agent (up to 2 s
        // with TURN) and joins its threads.
        gst_element_call_async(job->webrtc, runStopJob, job, stopJobFree);
    } else {
        stopNow(job);
        stopJobFree(job);
    }
}

void Retirer::poll()
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (advance(*it) == Step::Ready) {
            finish(*it, /*expired=*/false, /*async=*/true);
            it = m_entries.erase(it);
        } else if (it->age.elapsed() >= m_boundMs) {
            finish(*it, /*expired=*/true, /*async=*/true);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    if (m_entries.empty())
        m_timer.stop();
}

void Retirer::drain(int budgetMs)
{
    m_timer.stop();
    QElapsedTimer clock;
    clock.start();
    while (!m_entries.empty()) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (advance(*it) == Step::Ready) {
                finish(*it, /*expired=*/false, /*async=*/false);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
        if (m_entries.empty() || clock.elapsed() >= budgetMs)
            break;
        QThread::msleep(10);
    }
    // Out of budget: stop them anyway and accept the rare race at quit.
    for (Entry &entry : m_entries)
        finish(entry, /*expired=*/true, /*async=*/false);
    m_entries.clear();
    // Jobs already on the pool thread; a moment more even if the budget went.
    const qint64 deadline = qMax<qint64>(budgetMs, clock.elapsed() + 500);
    while (m_stats->retiring.load() > 0 && clock.elapsed() < deadline)
        QThread::msleep(5);
    if (m_stats->retiring.load() > 0) {
        qCWarning(lcRetire) << m_stats->retiring.load()
                            << "webrtcbin(s) still stopping on a pool thread";
    }
}

} // namespace lightning::webrtc
