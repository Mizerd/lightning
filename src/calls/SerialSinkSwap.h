// A received track's output replaced while the call runs (a speaker switch),
// shared by both call lanes (SfuMediaEngine, GstCallMediaBackend).
//
// Off the GUI thread: closing the old output and opening the new one both wait
// on the sound server (a pulsesink's cork, its context connect), and the IDLE
// pad probe that starts a switch runs on the thread that ADDS it whenever the
// pad is idle, which is nearly always the GUI thread. So the IDLE callback only
// blocks the pad and hands the work to a GStreamer pool thread.
//
// Serialized per receive bin: one job at a time per bin, and the newest
// request kept in the bin's slot. A request made while a job runs only
// replaces the slot's contents; the job re-reads it before it relinks and
// again after, so two picks in a row never run concurrently (a double
// gst_bin_remove, a failed link, an orphaned PLAYING sink) and the last pick
// always wins. The unlink/relink itself happens under the slot's mutex; the
// slow parts (opening the new output, stopping the old) never do. Lock order:
// the chain's state lock FIRST, then the slot's mutex, and nothing waits on a
// state lock while holding that mutex: a teardown can hold the state lock for
// as long as a sound server takes, and request() (the GUI thread) takes the
// mutex, so it must never be held across that wait.
//
// Header-only: both lanes include it, and it needs no build entry of its own.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include <QtGlobal>

#include <gst/gst.h>

namespace lightning::calls::sinkswap {

/// One output request.
struct Request {
    /// Builds the new output (a floating-sunk element with a "sink" pad,
    /// configured: binding, client name), or null when it cannot be built.
    std::function<GstElement *()> build;
    /// Counted once per completed relink. May be null.
    std::shared_ptr<std::atomic<int>> swaps;
    /// The session the request was made in; a job finding it false only
    /// releases the pad. May be null (the chain's own target state then
    /// decides).
    std::shared_ptr<std::atomic<bool>> live;
    /// Says why a switch did not happen (element names only). May be null.
    std::function<void(const char *)> warn;
};

struct Slot {
    std::mutex mutex;
    Request newest;
    quint64 serial = 0;
    /// A probe or job for this bin is outstanding: requests only update
    /// `newest`.
    bool queued = false;
    /// Test-only: relinks completed and new outputs discarded unlinked
    /// because a newer request arrived first.
    int relinks = 0;
    int discarded = 0;
};

inline GQuark slotQuark()
{
    static const GQuark quark =
        g_quark_from_static_string("lightning-sink-swap-slot");
    return quark;
}

/// The bin's slot, created on first use and freed with the bin. GUI thread
/// only: the get-or-create is not atomic (request() is its only caller).
inline std::shared_ptr<Slot> slotFor(GstElement *bin)
{
    auto *holder = static_cast<std::shared_ptr<Slot> *>(
        g_object_get_qdata(G_OBJECT(bin), slotQuark()));
    if (holder)
        return *holder;
    auto *created = new std::shared_ptr<Slot>(std::make_shared<Slot>());
    g_object_set_qdata_full(G_OBJECT(bin), slotQuark(), created,
                            [](gpointer data) {
                                delete static_cast<std::shared_ptr<Slot> *>(
                                    data);
                            });
    return *created;
}

namespace detail {

struct IdleCtx {
    std::shared_ptr<Slot> slot;
    GstElement *bin = nullptr; // owns a ref
    bool dispatched = false;
};

struct Job {
    std::shared_ptr<Slot> slot;
    GstPad *pad = nullptr; // owns a ref
    gulong blockProbe = 0;
    bool finished = false;
};

inline GstPadProbeReturn keepBlocked(GstPad *, GstPadProbeInfo *, gpointer)
{
    return GST_PAD_PROBE_OK;
}

inline void warn(const Request &request, const char *why)
{
    if (request.warn)
        request.warn(why);
}

inline bool stillWanted(const Request &request, GstElement *bin)
{
    return (!request.live || request.live->load())
        && GST_STATE_TARGET(bin) == GST_STATE_PLAYING;
}

inline void stopOutside(GstElement *element)
{
    if (!element)
        return;
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
}

/// The job: on a pool thread, the pad blocked.
inline void run(GstElement *bin, gpointer data)
{
    auto *job = static_cast<Job *>(data);
    Slot &slot = *job->slot;
    for (;;) {
        Request request;
        quint64 serial = 0;
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            request = slot.newest;
            serial = slot.serial;
        }
        if (!stillWanted(request, bin))
            break;
        GstElement *next = request.build ? request.build() : nullptr;
        if (!next) {
            warn(request, "the new output could not be built; this track "
                          "keeps its current one");
        } else if (gst_element_set_state(next, GST_STATE_READY)
                   == GST_STATE_CHANGE_FAILURE) {
            // Opened OUTSIDE the chain first: a sound server that does not
            // answer then holds only this thread, and a teardown setting the
            // pipeline to NULL never meets this element.
            warn(request, "the new output would not open; this track keeps "
                          "its current one");
            stopOutside(next);
            next = nullptr;
        }
        GstElement *old = nullptr;
        bool relinked = false;
        bool linked = false;
        {
            // Under the chain's state lock too: a state change of the chain
            // running meanwhile (a teardown's NULL) iterates its children and
            // could set the old output back up after it is stopped below,
            // then drop the last reference to it in that state. Taken BEFORE
            // the slot's mutex (the order above): waiting for a teardown here
            // must not hold up a new pick on the GUI thread.
            GST_STATE_LOCK(bin);
            std::unique_lock<std::mutex> lock(slot.mutex);
            if (next && serial == slot.serial && stillWanted(request, bin)) {
                if (GstPad *peer = gst_pad_get_peer(job->pad)) {
                    old = gst_pad_get_parent_element(peer);
                    gst_pad_unlink(job->pad, peer);
                    gst_object_unref(peer);
                    // Leaves the chain now; stopped outside it below.
                    if (old)
                        gst_bin_remove(GST_BIN(bin), old);
                }
                // `next` is not floating: the bin takes a reference of its
                // own, ours is dropped below.
                if (gst_bin_add(GST_BIN(bin), next)) {
                    relinked = true;
                    GstPad *sinkPad = gst_element_get_static_pad(next, "sink");
                    linked = sinkPad
                        && gst_pad_link(job->pad, sinkPad) == GST_PAD_LINK_OK;
                    if (sinkPad)
                        gst_object_unref(sinkPad);
                    ++slot.relinks;
                }
            } else if (next) {
                ++slot.discarded;
            }
            lock.unlock();
            GST_STATE_UNLOCK(bin);
        }
        if (relinked) {
            // Outside the slot's mutex (going to PLAYING can wait on the
            // server; a new pick must never wait for it), under the chain's
            // state lock so a teardown's NULL cannot interleave and leave it
            // PLAYING in a stopped chain. A teardown landing exactly here waits
            // for this open: accepted, as for a receive-chain rebuild.
            GST_STATE_LOCK(bin);
            if (stillWanted(request, bin))
                gst_element_sync_state_with_parent(next);
            GST_STATE_UNLOCK(bin);
            gst_object_unref(next);
            if (linked && request.swaps)
                request.swaps->fetch_add(1);
            if (!linked)
                warn(request, "the new output did not link");
        } else {
            stopOutside(next);
        }
        stopOutside(old);
        // Done only if nothing newer arrived meanwhile; otherwise the pad
        // stays blocked and the newest request is applied in turn.
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (serial == slot.serial) {
            slot.queued = false;
            job->finished = true;
            break;
        }
    }
    if (!job->finished) {
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.queued = false;
        job->finished = true;
    }
}

inline void jobFree(gpointer data)
{
    auto *job = static_cast<Job *>(data);
    if (job->pad) {
        // Always released: a pad left blocked silences the track for good.
        if (job->blockProbe != 0)
            gst_pad_remove_probe(job->pad, job->blockProbe);
        gst_object_unref(job->pad);
    }
    if (!job->finished) {
        // Never ran (the pool went away): the next request may queue again.
        std::lock_guard<std::mutex> lock(job->slot->mutex);
        job->slot->queued = false;
    }
    delete job;
}

inline GstPadProbeReturn onIdle(GstPad *pad, GstPadProbeInfo *, gpointer data)
{
    auto *ctx = static_cast<IdleCtx *>(data);
    // Nothing here may wait: this can be the GUI thread.
    auto *job = new Job;
    job->slot = ctx->slot;
    job->pad = GST_PAD(gst_object_ref(pad));
    job->blockProbe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                        keepBlocked, nullptr, nullptr);
    ctx->dispatched = true;
    gst_element_call_async(ctx->bin, run, job, jobFree);
    return GST_PAD_PROBE_REMOVE;
}

inline void idleFree(gpointer data)
{
    auto *ctx = static_cast<IdleCtx *>(data);
    if (!ctx->dispatched) {
        // The probe never fired (the pad went away): unblock the slot.
        std::lock_guard<std::mutex> lock(ctx->slot->mutex);
        ctx->slot->queued = false;
    }
    if (ctx->bin)
        gst_object_unref(ctx->bin);
    delete ctx;
}

} // namespace detail

/// Asks `bin`'s output (fed from `volumeSrc`) to become what `request`
/// builds. Any thread; never waits on a sound server. Returns false when a
/// switch was already outstanding for this bin and this one only replaced
/// its request.
inline bool request(GstElement *bin, GstPad *volumeSrc, Request request)
{
    const std::shared_ptr<Slot> slot = slotFor(bin);
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->newest = std::move(request);
        ++slot->serial;
        if (slot->queued)
            return false;
        slot->queued = true;
    }
    auto *ctx = new detail::IdleCtx;
    ctx->slot = slot;
    ctx->bin = GST_ELEMENT(gst_object_ref(bin));
    // At once if the pad is idle, otherwise right after the buffer in flight.
    gst_pad_add_probe(volumeSrc, GST_PAD_PROBE_TYPE_IDLE, detail::onIdle, ctx,
                      detail::idleFree);
    return true;
}

/// Test-only: the bin's relinks and discarded outputs so far.
inline int relinksForTest(GstElement *bin)
{
    auto *holder = static_cast<std::shared_ptr<Slot> *>(
        g_object_get_qdata(G_OBJECT(bin), slotQuark()));
    if (!holder)
        return 0;
    std::lock_guard<std::mutex> lock((*holder)->mutex);
    return (*holder)->relinks;
}

} // namespace lightning::calls::sinkswap
