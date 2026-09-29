// Stopping a call's webrtcbin while its ICE gathering can still emit a
// candidate aborts the process ("double free or corruption", GitHub #3):
// gstwebrtcnice fills a candidate's credentials into uninitialised locals,
// libnice 0.1.23 returns TRUE without writing them once the stream is gone,
// and the garbage is freed. Upstream gstreamer#5138, fixed on main by !11758
// and in no release yet, so every GStreamer we ship has it.
//
// webrtcbin removes its ICE streams only in its own READY -> NULL. So a call's
// pipeline still goes to NULL at once, taking every device, probe and sink of
// ours with it, but its webrtcbin is locked out of that change and retired:
// kept running with nothing of ours attached until its gathering has ended
// (bounded), then stopped on a GStreamer pool thread.
//
// Both engines use it: GstCallMediaBackend (1:1) and SfuMediaEngine (MatrixRTC).
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QElapsedTimer>
#include <QTimer>

typedef struct _GstElement GstElement;

namespace lightning::webrtc {

/// Our callbacks on a webrtcbin (signal handlers and promise change
/// functions) run inside a scope. Once the element is being retired a new
/// scope is not live and the callback must return without touching its engine
/// or queueing work on the element; retiring waits (bounded) for scopes
/// already inside. Without it a create-offer queued before the retirement
/// could still queue a set-local-description, and gathering, after it.
void installGate(GstElement *webrtc);

struct GateState;

class CallbackScope
{
public:
    explicit CallbackScope(GstElement *webrtc);
    ~CallbackScope();
    CallbackScope(const CallbackScope &) = delete;
    CallbackScope &operator=(const CallbackScope &) = delete;

    bool live() const { return m_live; }

private:
    std::shared_ptr<GateState> m_state;
    bool m_live = true;
};

/// Counters shared with the pool-thread jobs, so a job that ends after the
/// retirer is gone still has somewhere to count.
struct RetireStats {
    /// Retired and not yet at NULL (waiting, or a job in flight).
    std::atomic<int> retiring{0};
    /// NULL issued while webrtcbin still reported GATHERING: the crash window.
    std::atomic<int> whileGathering{0};
    /// Stopped because the bound ran out, not because gathering ended.
    std::atomic<int> boundExpired{0};
    /// Retirements that found gathering still running and waited for it.
    std::atomic<int> waitedOnGathering{0};
    /// webrtcbins that reached NULL.
    std::atomic<int> stopped{0};
};

class Retirer
{
public:
    /// An unanswered STUN or TURN-over-UDP server holds gathering for about
    /// 2 s (measured, libnice 0.1.23); TURN over TCP or TLS waits out
    /// libnice's 7.9 s reliable timer. The bound covers both. Past it, and
    /// past kQuitBudgetMs at quit, webrtcbin is stopped mid-gathering anyway:
    /// the upstream double free (gstreamer#5138) is still reachable there if
    /// a candidate lands during the stop. Delete this class once every
    /// shipped GStreamer carries the upstream fix (!11758).
    static constexpr int kGatheringBoundMs = 10000;
    /// What the destructor may spend at quit before stopping the rest anyway.
    static constexpr int kQuitBudgetMs = 2000;
    /// How long retire() waits for callbacks already running.
    static constexpr int kCallbackWaitMs = 2000;

    Retirer();
    ~Retirer();
    Retirer(const Retirer &) = delete;
    Retirer &operator=(const Retirer &) = delete;

    /// Takes over one reference on each. Disconnect every handler and clear
    /// the bus sync handler first. Sets the pipeline to NULL here, on the
    /// calling thread, except for `webrtc`, which stops once its gathering
    /// has ended. `pipeline` must be `webrtc`'s parent.
    void retire(GstElement *pipeline, GstElement *webrtc);

    /// Waits up to `budgetMs` for everything retired, then stops the rest
    /// synchronously and waits for jobs in flight. Needs no event loop.
    void drain(int budgetMs);

    int retiringForTest() const { return m_stats->retiring.load(); }
    int whileGatheringForTest() const { return m_stats->whileGathering.load(); }
    int boundExpiredForTest() const { return m_stats->boundExpired.load(); }
    int waitedOnGatheringForTest() const
    {
        return m_stats->waitedOnGathering.load();
    }
    int stoppedForTest() const { return m_stats->stopped.load(); }
    void setBoundForTest(int ms) { m_boundMs = ms; }

private:
    struct Entry {
        GstElement *pipeline = nullptr; // owns a ref
        GstElement *webrtc = nullptr;   // owns a ref
        /// get-stats barriers replied to, written on webrtcbin's thread.
        std::shared_ptr<std::atomic<int>> replied;
        int sent = 0;
        bool waitCounted = false;
        QElapsedTimer age;
    };
    enum class Step { Wait, Ready };

    Step advance(Entry &entry);
    void sendBarrier(Entry &entry);
    void finish(Entry &entry, bool expired, bool async);
    void poll();

    std::vector<Entry> m_entries;
    std::shared_ptr<RetireStats> m_stats = std::make_shared<RetireStats>();
    QTimer m_timer;
    int m_boundMs = kGatheringBoundMs;
};

} // namespace lightning::webrtc
