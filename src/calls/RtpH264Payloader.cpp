#include "calls/RtpH264Payloader.h"

#include "calls/RtpVp8Payloader.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <gst/gst.h>
#include <gst/rtp/gstrtpbasepayload.h>
#include <gst/rtp/gstrtpbuffer.h>

namespace {

constexpr gint kClockRate = 90000;
constexpr guint8 kNalTypeMask = 0x1f;
constexpr guint8 kNalHeaderMask = 0xe0; // F and NRI
constexpr guint8 kFuA = 28;
constexpr guint8 kFuStart = 0x80;
constexpr guint8 kFuEnd = 0x40;
constexpr guint kFuHeaderBytes = 2;

struct LightningRtpH264Pay {
    GstRTPBasePayload parent;
};

struct LightningRtpH264PayClass {
    GstRTPBasePayloadClass parent;
};

GType lightning_rtp_h264_pay_get_type();

#define LIGHTNING_TYPE_RTP_H264_PAY (lightning_rtp_h264_pay_get_type())

G_DEFINE_TYPE(LightningRtpH264Pay, lightning_rtp_h264_pay,
              GST_TYPE_RTP_BASE_PAYLOAD)

GstStaticPadTemplate kSinkTemplate = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264, stream-format = (string) byte-stream, "
                    "alignment = (string) au"));

GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("application/x-rtp, "
                    "media = (string) video, "
                    "payload = (int) [ 96, 127 ], "
                    "clock-rate = (int) 90000, "
                    "encoding-name = (string) H264"));

gboolean setCaps(GstRTPBasePayload *payload, GstCaps *caps)
{
    (void)caps;
    gst_rtp_base_payload_set_options(payload, "video", TRUE, "H264", kClockRate);
    return gst_rtp_base_payload_set_outcaps(
        payload, "packetization-mode", G_TYPE_STRING, "1", "profile-level-id",
        G_TYPE_STRING, lightning::rtp::kH264ProfileLevelId,
        "level-asymmetry-allowed", G_TYPE_STRING, "1", nullptr);
}

struct Nal {
    gsize offset;
    gsize size;
};

/// One entry per NAL unit, start codes removed. Every unit but the last loses
/// its trailing zeros (a 4-byte start code's first byte lands there); the last
/// keeps them, because in an encrypted frame they are the trailer.
std::vector<Nal> splitNals(const guint8 *data, gsize size)
{
    std::vector<gsize> starts;
    for (gsize i = 0; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            starts.push_back(i + 3);
            i += 2;
        }
    }
    std::vector<Nal> nals;
    for (size_t n = 0; n < starts.size(); ++n) {
        const gsize begin = starts[n];
        gsize end = size;
        if (n + 1 < starts.size()) {
            end = starts[n + 1] - 3;
            while (end > begin && data[end - 1] == 0)
                --end;
        }
        if (end > begin)
            nals.push_back({begin, end - begin});
    }
    return nals;
}

/// Appends one packet: `prefix` then `size` bytes of `body`.
bool addPacket(GstBufferList *packets, GstBuffer *timing, const guint8 *prefix,
               guint prefixSize, const guint8 *body, gsize size, bool marker)
{
    GstBuffer *out = gst_rtp_buffer_new_allocate(
        static_cast<guint>(prefixSize + size), 0, 0);
    if (!out)
        return false;
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(out, GST_MAP_WRITE, &rtp)) {
        gst_buffer_unref(out);
        return false;
    }
    auto *dest = static_cast<guint8 *>(gst_rtp_buffer_get_payload(&rtp));
    if (prefixSize)
        memcpy(dest, prefix, prefixSize);
    memcpy(dest + prefixSize, body, size);
    gst_rtp_buffer_set_marker(&rtp, marker ? TRUE : FALSE);
    gst_rtp_buffer_unmap(&rtp);
    GST_BUFFER_PTS(out) = GST_BUFFER_PTS(timing);
    GST_BUFFER_DTS(out) = GST_BUFFER_DTS(timing);
    GST_BUFFER_DURATION(out) = GST_BUFFER_DURATION(timing);
    gst_buffer_list_add(packets, out);
    return true;
}

GstFlowReturn handleBuffer(GstRTPBasePayload *payload, GstBuffer *buffer)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    const guint mtu = GST_RTP_BASE_PAYLOAD_MTU(payload);
    const gsize overhead = gst_rtp_buffer_calc_header_len(0)
        + lightning::rtp::rtpHeaderExtensionBudget(GST_ELEMENT(payload), buffer);
    if (mtu <= overhead + kFuHeaderBytes) {
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    const gsize maxSingle = mtu - overhead;
    const gsize maxFragment = maxSingle - kFuHeaderBytes;

    GstBufferList *packets = gst_buffer_list_new();
    const std::vector<Nal> nals = splitNals(map.data, map.size);
    bool ok = true;
    for (size_t n = 0; ok && n < nals.size(); ++n) {
        const guint8 *nal = map.data + nals[n].offset;
        const gsize size = nals[n].size;
        const bool lastNal = n + 1 == nals.size();
        if (size <= maxSingle) {
            ok = addPacket(packets, buffer, nullptr, 0, nal, size, lastNal);
            continue;
        }
        const guint8 indicator =
            static_cast<guint8>((nal[0] & kNalHeaderMask) | kFuA);
        const guint8 type = nal[0] & kNalTypeMask;
        gsize offset = 1; // the NAL header travels in the FU bytes
        while (ok && offset < size) {
            const gsize take = std::min(maxFragment, size - offset);
            const bool end = offset + take >= size;
            const guint8 prefix[kFuHeaderBytes] = {
                indicator,
                static_cast<guint8>(type | (offset == 1 ? kFuStart : 0)
                                    | (end ? kFuEnd : 0))};
            ok = addPacket(packets, buffer, prefix, kFuHeaderBytes,
                           nal + offset, take, lastNal && end);
            offset += take;
        }
    }
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
    if (gst_buffer_list_length(packets) == 0) {
        gst_buffer_list_unref(packets);
        return GST_FLOW_OK;
    }
    return gst_rtp_base_payload_push_list(payload, packets);
}

void lightning_rtp_h264_pay_class_init(LightningRtpH264PayClass *klass)
{
    auto *element = GST_ELEMENT_CLASS(klass);
    auto *base = GST_RTP_BASE_PAYLOAD_CLASS(klass);

    gst_element_class_add_static_pad_template(element, &kSinkTemplate);
    gst_element_class_add_static_pad_template(element, &kSrcTemplate);
    gst_element_class_set_static_metadata(
        element, "Lightning H.264 RTP payloader",
        "Codec/Payloader/Network/RTP",
        "Payloads H.264 keeping every byte, so an end-to-end encrypted frame "
        "can be sent",
        "Lightning");

    base->set_caps = setCaps;
    base->handle_buffer = handleBuffer;
}

void lightning_rtp_h264_pay_init(LightningRtpH264Pay *self)
{
    g_object_set(self, "perfect-rtptime", FALSE, "mtu",
                 lightning::rtp::kRtpPayloadMtu, nullptr);
}

} // namespace

namespace lightning::rtp {

void registerH264Payloader()
{
    static std::once_flag once;
    std::call_once(once, [] {
        gst_element_register(nullptr, h264PayloaderName(), GST_RANK_NONE,
                             LIGHTNING_TYPE_RTP_H264_PAY);
    });
}

const char *h264PayloaderName() { return "lightningrtph264pay"; }

} // namespace lightning::rtp
