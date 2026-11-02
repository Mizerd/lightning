// A VP8 RTP payloader that does not parse the bitstream.
//
// GStreamer's `rtpvp8pay` parses the VP8 frame (frame tag, keyframe start
// code, bool-decoded partition fields) to build its descriptor, so an
// end-to-end encrypted frame, where only the first 10 (keyframe) or 3 (delta)
// bytes are clear, makes it fail with "Failed to parse VP8 frame".
//
// libwebrtc's RtpPacketizerVp8 takes the keyframe flag and picture id from
// the encoder as metadata instead. This element does the same: it prepends
// the RFC 7741 descriptor (see the .cpp for its layout) and fragments at the
// MTU without reading the payload. The RTP marker bit ends the frame.
#pragma once

typedef struct _GstBuffer GstBuffer;
typedef struct _GstElement GstElement;

namespace lightning::rtp {

inline constexpr unsigned int kRtpPayloadMtu = 1200;

void registerVp8Payloader();

const char *vp8PayloaderName();

unsigned long rtpHeaderExtensionBudget(GstElement *payloader, GstBuffer *buffer);

}
