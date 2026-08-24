#include "calls/RtpVp8Payloader.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <gst/gst.h>
#include <gst/rtp/gstrtpbasepayload.h>
#include <gst/rtp/gstrtpbuffer.h>

namespace {

/// The RFC 7741 payload descriptor this element writes, in the form Chrome
/// sends: X=1, I=1 and a 15-bit PictureID, with S=1 only on a frame's first
/// packet.
///
/// THE PICTURE ID IS NOT OPTIONAL IN PRACTICE. libwebrtc always emits one, and
/// LiveKit's SFU REWRITES the descriptor when it forwards — `codecmunger/vp8.go`
/// unwraps `vp8.PictureID`, munges it, and marshals a NEW header whose size it
/// computes from that field. Handed packets with no picture id, its idea of the
/// header size no longer matches ours and the forwarded payload is corrupted
/// from the second frame on: the far end renders the first keyframe and then
/// nothing, which is exactly how a share looked in Element.
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
/// 15 bits, so it wraps where the format says it does.
constexpr guint16 kPictureIdMask = 0x7fff;
/// VP8 is always 90 kHz.
constexpr gint kClockRate = 90000;

struct LightningRtpVp8Pay {
    GstRTPBasePayload parent;
    /// Incremented once per FRAME, and carried on every packet of it — which
    /// is what libwebrtc does and what the SFU's munger tracks.
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
    // "VP8" and 90 kHz are fixed by RFC 7741. `TRUE` marks it dynamic-payload,
    // which is what lets the negotiated pt through.
    gst_rtp_base_payload_set_options(payload, "video", TRUE, "VP8", kClockRate);
    return gst_rtp_base_payload_set_outcaps(payload, nullptr);
}

GstFlowReturn handleBuffer(GstRTPBasePayload *payload, GstBuffer *buffer)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    // Room for the RTP header AND our one descriptor byte. `mtu` is the whole
    // packet budget, so both come out of it — a fragment sized against the mtu
    // alone would produce packets one byte over on every full fragment.
    const guint mtu = GST_RTP_BASE_PAYLOAD_MTU(payload);
    const guint overhead =
        gst_rtp_buffer_calc_header_len(0) + kDescriptorBytes;
    if (mtu <= overhead) {
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    const guint maxFragment = mtu - overhead;

    auto *self = reinterpret_cast<LightningRtpVp8Pay *>(payload);
    // One id per frame, advanced BEFORE the packets so every packet of this
    // frame carries the same value.
    const guint16 pictureId = self->pictureId;
    self->pictureId = (self->pictureId + 1) & kPictureIdMask;

    GstBufferList *packets = gst_buffer_list_new();
    gsize offset = 0;
    const gsize total = map.size;
    // A zero-length frame still produces nothing rather than an empty packet.
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
        // The marker bit is what tells the receiver the frame is complete.
        gst_rtp_buffer_set_marker(&rtp, last ? TRUE : FALSE);
        gst_rtp_buffer_unmap(&rtp);

        // Timing comes from the input frame: every packet of one frame carries
        // the same RTP timestamp, which GstRTPBasePayload derives from the PTS.
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
    // A RANDOM, NON-ZERO start, which is what libwebrtc does.
    //
    // Not cosmetic. LiveKit's SFU rewrites picture ids when it forwards, and
    // it seeds its wrap handler with `Init(PictureID - 1)` — from zero that
    // is Init(-1), an edge no real Chrome stream ever presents it with.
    // Starting where Chrome starts keeps the SFU on the path it is exercised
    // against.
    self->pictureId = static_cast<guint16>(
        (g_random_int() & kPictureIdMask) | 1u);
    // `perfect-rtptime` derives the RTP timestamp from the OFFSET IN BYTES,
    // which is an audio idea: it only makes sense where the byte count and
    // the clock advance together. For video the timestamp has to come from
    // the frame's PTS, or a receiver's jitter buffer can see several frames
    // sharing one timestamp and treat them as a single picture.
    g_object_set(self, "perfect-rtptime", FALSE, nullptr);
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
