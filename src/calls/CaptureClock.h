#pragma once

#include <gst/gst.h>

#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

// A live capture stamps each buffer with the running time it was captured
// at, so its timestamps are never ahead of the pipeline clock. Some are
// anyway: wasapi2src on a Remote Desktop microphone delivered 1.9 % more audio
// than real time and was 0.9 s ahead of the clock 48 s into a call (measured
// 2026-09-30, Windows 11 guest).
//
// webrtcbin syncs every input to the clock (a clocksync with sync=TRUE on
// each sink pad, which releases a buffer at its running time plus the
// upstream latency), so a buffer that is ahead parks the thread of the
// bounded leaky queue in front of the encoder until the clock catches up, and
// the queue throws away everything that arrives meanwhile. Measured: 100
// buffers a second into the queue, 6 a second out of it, from ten seconds into
// every call; the far end heard the microphone fade out.
//
// Two probes. One on the capture's output only watches: the lead is visible
// there and nowhere later, because behind the queue the packet clocksync holds
// is an old one. The other sits on the RTP the payloader produced, right
// before webrtcbin: while the capture is ahead it hands clocksync a running
// time it releases at once. Only the time clocksync waits for changes; the RTP
// timestamps were already written from the capture's own sample count and
// stay 960 per 20 ms packet, so the far end sees a sender whose clock runs a
// little fast, which is ordinary drift. (Restamping the capture itself made
// every RTP step short.)
//
// A correct live source never stamps a buffer later than the moment it pushes
// it, so it is never ahead and the probes change nothing.
namespace lightning::calls {

/// A lead this small is timestamp jitter, not drift, and is left alone: it
/// is far inside the 100 ms leaky queues that a real lead starves.
constexpr GstClockTime kCaptureLeadTolerance = 20 * GST_MSECOND;

/// Packets between two upstream latency queries.
constexpr quint64 kCaptureLatencyRequery = 100;

/// Whether a capture whose latest buffer was stamped `leadNs` later than the
/// clock's running time is ahead by more than the tolerance. Pure, for the
/// tests.
inline bool captureIsAhead(qint64 leadNs)
{
    return leadNs > qint64(kCaptureLeadTolerance);
}

/// The running time clocksync is given for an RTP packet stamped `running`.
/// Untouched unless the capture is ahead; then no later than `now` minus the
/// upstream `latency` clocksync adds, so clocksync releases it at once, and
/// never earlier than the previous packet's `last` (GST_CLOCK_TIME_NONE for
/// none), so a lead hovering around the tolerance cannot step time backwards.
/// Pure, for the tests.
inline GstClockTime captureRunningTimeHeld(GstClockTime running,
                                           GstClockTime now,
                                           GstClockTime latency,
                                           bool captureAhead,
                                           GstClockTime last)
{
    if (!captureAhead)
        return running;
    const GstClockTime due = now > latency ? now - latency : 0;
    const GstClockTime held = due < running ? due : running;
    return GST_CLOCK_TIME_IS_VALID(last) && last > held ? last : held;
}

struct CaptureClockHold {
    /// RTP packets handed on earlier than stamped. Written on the streaming
    /// thread.
    std::atomic<quint64> held{0};
    /// How far the capture's latest buffer was ahead of the clock (ns, <= 0
    /// when not ahead). Written by the capture's thread, read by the RTP's.
    std::atomic<qint64> captureLeadNs{0};
    /// The last running time handed on; RTP thread only.
    GstClockTime last = GST_CLOCK_TIME_NONE;
    /// The upstream latency clocksync adds (0 for a non-live chain), and the
    /// packets since it was asked; RTP thread only.
    GstClockTime latency = GST_CLOCK_TIME_NONE;
    quint64 sinceLatencyQuery = 0;
    /// Called on the RTP thread for the first packet held, then rarely, with
    /// the count and how far ahead the capture was.
    std::function<void(quint64 count, qint64 leadMs)> report;
};

namespace detail {

/// The clock's running time for the element owning `pad`, or NONE.
inline GstClockTime padRunningNow(GstPad *pad)
{
    GstObject *parent = gst_pad_get_parent(pad);
    if (!parent)
        return GST_CLOCK_TIME_NONE;
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(parent));
    const GstClockTime base = gst_element_get_base_time(GST_ELEMENT(parent));
    gst_object_unref(parent);
    if (!clock)
        return GST_CLOCK_TIME_NONE;
    const GstClockTime clockNow = gst_clock_get_time(clock);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(clockNow) || !GST_CLOCK_TIME_IS_VALID(base)
        || clockNow < base) {
        return GST_CLOCK_TIME_NONE;
    }
    return clockNow - base;
}

/// The buffer's running time on `pad`'s current segment, or NONE. On success
/// `*segmentEvent` holds the segment event, which the caller unrefs.
inline GstClockTime padRunningTime(GstPad *pad, GstBuffer *buffer,
                                   GstEvent **segmentEvent)
{
    *segmentEvent = nullptr;
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer))
        return GST_CLOCK_TIME_NONE;
    GstEvent *event = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
    if (!event)
        return GST_CLOCK_TIME_NONE;
    const GstSegment *segment = nullptr;
    gst_event_parse_segment(event, &segment);
    if (!segment || segment->format != GST_FORMAT_TIME) {
        gst_event_unref(event);
        return GST_CLOCK_TIME_NONE;
    }
    *segmentEvent = event;
    return gst_segment_to_running_time(segment, GST_FORMAT_TIME,
                                       GST_BUFFER_PTS(buffer));
}

inline GstPadProbeReturn watchCapture(GstPad *pad, GstPadProbeInfo *info,
                                      gpointer user)
{
    auto *hold =
        static_cast<std::shared_ptr<CaptureClockHold> *>(user)->get();
    GstEvent *segment = nullptr;
    const GstClockTime running =
        padRunningTime(pad, GST_PAD_PROBE_INFO_BUFFER(info), &segment);
    if (segment)
        gst_event_unref(segment);
    const GstClockTime now = padRunningNow(pad);
    if (!GST_CLOCK_TIME_IS_VALID(running) || !GST_CLOCK_TIME_IS_VALID(now))
        return GST_PAD_PROBE_OK;
    hold->captureLeadNs.store(GST_CLOCK_DIFF(now, running));
    return GST_PAD_PROBE_OK;
}

/// Re-stamps one RTP buffer; `*buffer` may be replaced by a writable copy.
inline void holdOneBuffer(GstPad *pad, GstBuffer **buffer, GstClockTime now,
                          CaptureClockHold *hold)
{
    GstEvent *event = nullptr;
    const GstClockTime running = padRunningTime(pad, *buffer, &event);
    if (!GST_CLOCK_TIME_IS_VALID(running)) {
        if (event)
            gst_event_unref(event);
        return;
    }
    const qint64 lead = hold->captureLeadNs.load();
    const bool ahead = captureIsAhead(lead);
    const GstClockTime out = captureRunningTimeHeld(
        running, now, hold->latency, ahead, hold->last);
    hold->last = out;
    if (out != running) {
        const GstSegment *segment = nullptr;
        gst_event_parse_segment(event, &segment);
        const GstClockTime pts = gst_segment_position_from_running_time(
            segment, GST_FORMAT_TIME, out);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            *buffer = gst_buffer_make_writable(*buffer);
            GST_BUFFER_PTS(*buffer) = pts;
            GST_BUFFER_DTS(*buffer) = GST_CLOCK_TIME_NONE;
            if (out < running) {
                const quint64 n = ++hold->held;
                if (hold->report && (n == 1 || n == 100 || n % 10000 == 0))
                    hold->report(n, lead / qint64(GST_MSECOND));
            }
        }
    }
    gst_event_unref(event);
}

inline GstPadProbeReturn holdRtp(GstPad *pad, GstPadProbeInfo *info,
                                 gpointer user)
{
    auto *hold =
        static_cast<std::shared_ptr<CaptureClockHold> *>(user)->get();
    const GstClockTime now = padRunningNow(pad);
    if (!GST_CLOCK_TIME_IS_VALID(now))
        return GST_PAD_PROBE_OK;

    // What clocksync adds, asked the same way it asks: a latency query
    // upstream from here. Re-asked now and then; a restarted source can
    // change it.
    if (!GST_CLOCK_TIME_IS_VALID(hold->latency)
        || ++hold->sinceLatencyQuery >= kCaptureLatencyRequery) {
        hold->sinceLatencyQuery = 0;
        GstQuery *query = gst_query_new_latency();
        gboolean live = FALSE;
        GstClockTime min = 0;
        GstClockTime max = 0;
        if (gst_pad_query(pad, query))
            gst_query_parse_latency(query, &live, &min, &max);
        gst_query_unref(query);
        hold->latency = live && GST_CLOCK_TIME_IS_VALID(min) ? min : 0;
    }

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer) {
            holdOneBuffer(pad, &buffer, now, hold);
            GST_PAD_PROBE_INFO_DATA(info) = buffer;
        }
    } else if (GST_PAD_PROBE_INFO_TYPE(info)
               & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        if (list) {
            list = gst_buffer_list_make_writable(list);
            const guint n = gst_buffer_list_length(list);
            for (guint i = 0; i < n; ++i) {
                GstBuffer *buffer = gst_buffer_list_get_writable(list, i);
                holdOneBuffer(pad, &buffer, now, hold);
            }
            GST_PAD_PROBE_INFO_DATA(info) = list;
        }
    }
    return GST_PAD_PROBE_OK;
}

inline void addHoldProbe(GstPad *pad, GstPadProbeType type,
                         GstPadProbeCallback callback,
                         const std::shared_ptr<CaptureClockHold> &hold)
{
    gst_pad_add_probe(
        pad, type, callback, new std::shared_ptr<CaptureClockHold>(hold),
        [](gpointer user) {
            delete static_cast<std::shared_ptr<CaptureClockHold> *>(user);
        });
}

} // namespace detail

/// Watches the capture on `capturePad` (its src pad) and holds the RTP on
/// `rtpPad` (the payloader's output, before webrtcbin) to the clock while the
/// capture is ahead. Each probe lives as long as its pad and owns a
/// reference.
inline void holdCaptureToClock(GstPad *capturePad, GstPad *rtpPad,
                               const std::shared_ptr<CaptureClockHold> &hold)
{
    if (!capturePad || !rtpPad || !hold)
        return;
    detail::addHoldProbe(capturePad, GST_PAD_PROBE_TYPE_BUFFER,
                         &detail::watchCapture, hold);
    detail::addHoldProbe(
        rtpPad,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER
                                     | GST_PAD_PROBE_TYPE_BUFFER_LIST),
        &detail::holdRtp, hold);
}

} // namespace lightning::calls
