// A VP8 RTP payloader that does NOT parse the bitstream.
//
// GStreamer's own `rtpvp8pay` reads the VP8 frame to build its payload
// descriptor: partition0's size out of the frame tag, a keyframe's
// `0x9d 0x01 0x2a` start code, then segmentation and loop-filter fields
// bool-decoded out of the compressed first partition
// (gstrtpvp8pay.c, gst_rtp_vp8_pay_parse_frame).
//
// That is incompatible with end-to-end encrypted media. LiveKit and Element
// Call encrypt the whole ENCODED FRAME, leaving only the 10 header bytes of a
// keyframe (3 of a delta frame) in the clear, so a payloader that reads
// further is handed ciphertext, fails, and posts STREAM/ENCODE
// "Failed to parse VP8 frame". Measured symptom: a screen share publishes
// exactly one frame and stops; a camera publishes nothing; the far end shows a
// grey rectangle.
//
// libwebrtc — which is what Chrome and therefore Element Call use — does not
// have this problem, because `RtpPacketizerVp8` takes the keyframe flag and
// picture id from the encoder as METADATA and never reads the payload. This
// element reproduces that behaviour: it prepends the RFC 7741 payload
// descriptor and fragments at the MTU, reading nothing.
//
// The descriptor is the minimal interoperable form Chrome sends in aggregate
// mode: X=0 (no extended control bits), N=0, PID=0, and S=1 only on the first
// packet of each frame. The RTP marker bit ends the frame.
#pragma once

typedef struct _GstElement GstElement;

namespace lightning::rtp {

/// Register `lightningrtpvp8pay` with GStreamer. Idempotent and safe to call
/// from any thread; the first call does the work. Must run after gst_init.
void registerVp8Payloader();

/// The element name to use in a pipeline description.
const char *vp8PayloaderName();

} // namespace lightning::rtp
