#include "calls/RtpVp8Payloader.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <gst/gst.h>
#include <gst/rtp/gstrtpbasepayload.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtphdrext.h>

namespace {

/// The RFC 7741 payload descriptor this element writes, as Chrome sends it:
/// X=1, I=1 and a 15-bit PictureID, with S=1 only on a frame's first packet.
///
/// The PictureID matters: LiveKit's SFU rewrites the descriptor when
/// forwarding (`codecmunger/vp8.go`) and sizes the new header from that
/// field, so without one the forwarded payload is corrupted after the first
/// frame.
///
///   byte 0 : |X|R|N|S|R| PID |   X=1, S on the first packet
///   byte 1 : |I|L|T|K| RSV  |   I=1
///   byte 2 : |M| PictureID hi|   M=1, 15-bit id
///   byte 3 : | PictureID lo  |
constexpr guint8 kFlagExtended = 0x80; // X
constexpr guint8 kFlagStart = 0x10;    // S
constexpr guint8 kFlagPictureId = 0x80; // I, in the extension byte
constexpr guint8 kFlagLongPictureId = 0x80; // M, in the id's first byte
constexpr guint kDescriptorBytes = 4;
/// 15 bits, wrapping as the format specifies.
constexpr guint16 kPictureIdMask = 0x7fff;
/// VP8 is always 90 kHz.
constexpr gint kClockRate = 90000;

struct LightningRtpVp8Pay {
    GstRTPBasePayload parent;
    /// Incremented once per frame and carried on every packet of it, as
    /// libwebrtc does.
    guint16 pictureId;
};

struct LightningRtpVp8PayClass {
    GstRTPBasePayloadClass parent;
};

GType lightning_rtp_vp8_pay_get_type();

#define LIGHTNING_TYPE_RTP_VP8_PAY (lightning_rtp_vp8_pay_get_type())

G_DEFINE_TYPE(LightningRtpVp8Pay, lightning_rtp_vp8_pay,
              GST_TYPE_RTP_BASE_PAYLOAD)

GstStaticPadTemplate kSinkTemplate = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-vp8"));

GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("application/x-rtp, "
                    "media = (string) video, "
                    "payload = (int) [ 96, 127 ], "
                    "clock-rate = (int) 90000, "
                    "encoding-name = (string) VP8"));

gboolean setCaps(GstRTPBasePayload *payload, GstCaps *caps)
{
    (void)caps;
    // "VP8" and 90 kHz are fixed by RFC 7741; TRUE marks a dynamic payload
    // type so the negotiated pt passes.
    gst_rtp_base_payload_set_options(payload, "video", TRUE, "VP8", kClockRate);
    return gst_rtp_base_payload_set_outcaps(payload, nullptr);
}

gsize headerExtensionBudget(GstRTPBasePayload *payload, GstBuffer *buffer)
{
    // GstRTPBasePayload adds extensions in push_list(), AFTER we fragment.
    // Reserve the four-byte extension prefix, each extension's worst-case
    // two-byte element header and data, rounded to a four-byte word boundary.
    gsize bytes = 0;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(payload), "extensions")) {
        GValue extensions = G_VALUE_INIT;
        g_value_init(&extensions, GST_TYPE_ARRAY);
        g_object_get_property(G_OBJECT(payload), "extensions", &extensions);
        for (guint i = 0; i < gst_value_array_get_size(&extensions); ++i) {
            auto *ext = GST_RTP_HEADER_EXTENSION(g_value_get_object(
                gst_value_array_get_value(&extensions, i)));
            bytes += 2 + gst_rtp_header_extension_get_max_size(ext, buffer);
        }
        g_value_unset(&extensions);
    } else {
        // GStreamer 1.20/1.22 exposes negotiated extmaps but not the extension
        // objects. Bound each by RFC 8285's maximum two-byte data length.
        GstCaps *caps = gst_pad_get_current_caps(payload->srcpad);
        if (caps && !gst_caps_is_empty(caps)) {
            const GstStructure *s = gst_caps_get_structure(caps, 0);
            for (int i = 0; i < gst_structure_n_fields(s); ++i) {
                if (g_str_has_prefix(gst_structure_nth_field_name(s, i), "extmap-"))
                    bytes += 2 + 255;
            }
        }
        if (caps)
            gst_caps_unref(caps);
    }
    return bytes ? 4 + ((bytes + 3) & ~gsize(3)) : 0;
}

GstFlowReturn handleBuffer(GstRTPBasePayload *payload, GstBuffer *buffer)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    // `mtu` is the whole packet budget: reserve the RTP header and the
    // descriptor and extensions from it. The encrypted frame (including its
    // authentication trailer) is already the input and is fragmented here.
    const guint mtu = GST_RTP_BASE_PAYLOAD_MTU(payload);
    const gsize overhead = gst_rtp_buffer_calc_header_len(0)
        + kDescriptorBytes + headerExtensionBudget(payload, buffer);
    if (mtu <= overhead) {
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    const guint maxFragment = mtu - overhead;

    auto *self = reinterpret_cast<LightningRtpVp8Pay *>(payload);
    // One id per frame, taken before packetising so all packets share it.
    const guint16 pictureId = self->pictureId;
    self->pictureId = (self->pictureId + 1) & kPictureIdMask;

    GstBufferList *packets = gst_buffer_list_new();
    gsize offset = 0;
    const gsize total = map.size;
    // A zero-length frame produces no packets.
    while (offset < total) {
        const gsize take = std::min<gsize>(maxFragment, total - offset);
        const bool first = offset == 0;
        const bool last = offset + take >= total;

        GstBuffer *out = gst_rtp_buffer_new_allocate(
            static_cast<guint>(take + kDescriptorBytes), 0, 0);
        if (!out)
            break;
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!gst_rtp_buffer_map(out, GST_MAP_WRITE, &rtp)) {
            gst_buffer_unref(out);
            break;
        }
        guint8 *dest = static_cast<guint8 *>(gst_rtp_buffer_get_payload(&rtp));
        dest[0] = kFlagExtended | (first ? kFlagStart : 0x00);
        dest[1] = kFlagPictureId;
        dest[2] = kFlagLongPictureId | static_cast<guint8>((pictureId >> 8) & 0x7f);
        dest[3] = static_cast<guint8>(pictureId & 0xff);
        memcpy(dest + kDescriptorBytes, map.data + offset, take);
        // The marker bit ends the frame.
        gst_rtp_buffer_set_marker(&rtp, last ? TRUE : FALSE);
        gst_rtp_buffer_unmap(&rtp);

        // All packets of a frame share the RTP timestamp derived from its PTS.
        GST_BUFFER_PTS(out) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DTS(out) = GST_BUFFER_DTS(buffer);
        GST_BUFFER_DURATION(out) = GST_BUFFER_DURATION(buffer);
        gst_buffer_list_add(packets, out);

        offset += take;
    }

    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);

    if (gst_buffer_list_length(packets) == 0) {
        gst_buffer_list_unref(packets);
        return GST_FLOW_OK;
    }
    return gst_rtp_base_payload_push_list(payload, packets);
}

void lightning_rtp_vp8_pay_class_init(LightningRtpVp8PayClass *klass)
{
    auto *element = GST_ELEMENT_CLASS(klass);
    auto *base = GST_RTP_BASE_PAYLOAD_CLASS(klass);

    gst_element_class_add_static_pad_template(element, &kSinkTemplate);
    gst_element_class_add_static_pad_template(element, &kSrcTemplate);
    gst_element_class_set_static_metadata(
        element, "Lightning VP8 RTP payloader", "Codec/Payloader/Network/RTP",
        "Payloads VP8 without reading the bitstream, so an end-to-end "
        "encrypted frame can be sent",
        "Lightning");

    base->set_caps = setCaps;
    base->handle_buffer = handleBuffer;
}

void lightning_rtp_vp8_pay_init(LightningRtpVp8Pay *self)
{
    // A random non-zero start, as libwebrtc does. LiveKit seeds its wrap
    // handler with PictureID - 1, and starting where Chrome does keeps it on
    // its well-exercised path.
    self->pictureId = static_cast<guint16>(
        (g_random_int() & kPictureIdMask) | 1u);
    // `perfect-rtptime` derives timestamps from byte offsets, which only suits
    // audio; video timestamps must follow the frame PTS.
    g_object_set(self, "perfect-rtptime", FALSE,
                 "mtu", lightning::rtp::kRtpPayloadMtu, nullptr);
}

} // namespace

namespace lightning::rtp {

void registerVp8Payloader()
{
    static std::once_flag once;
    std::call_once(once, [] {
        gst_element_register(nullptr, vp8PayloaderName(), GST_RANK_NONE,
                            LIGHTNING_TYPE_RTP_VP8_PAY);
    });
}

const char *vp8PayloaderName() { return "lightningrtpvp8pay"; }

} // namespace lightning::rtp
