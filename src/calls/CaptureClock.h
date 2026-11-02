#pragma once

#include <gst/gst.h>

#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace lightning::calls {

/// A lead this small is timestamp jitter, not drift, and is left alone: it
/// is far inside the 100 ms leaky queues that a real lead starves.
constexpr GstClockTime kCaptureLeadTolerance = 20 * GST_MSECOND;

constexpr quint64 kCaptureLatencyRequery = 100;

inline bool captureIsAhead(qint64 leadNs)
{
    return leadNs > qint64(kCaptureLeadTolerance);
}

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
    std::atomic<quint64> held{0};
    std::atomic<qint64> captureLeadNs{0};
    GstClockTime last = GST_CLOCK_TIME_NONE;
    GstClockTime latency = GST_CLOCK_TIME_NONE;
    quint64 sinceLatencyQuery = 0;
    std::function<void(quint64 count, qint64 leadMs)> report;
};

namespace detail {

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

}

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

inline void watchReplacementCapture(GstPad *capturePad,
                                    const std::shared_ptr<CaptureClockHold> &hold)
{
    if (!capturePad || !hold)
        return;
    hold->captureLeadNs.store(0);
    detail::addHoldProbe(capturePad, GST_PAD_PROBE_TYPE_BUFFER,
                         &detail::watchCapture, hold);
}

}
