// An H.264 RTP payloader (RFC 6184, packetization-mode=1) for an end-to-end
// encrypted frame.
//
// GStreamer's `rtph264pay` drops trailing zero bytes from a NAL unit, which
// H.264 forbids there. An encrypted frame ends in the LiveKit trailer, whose
// last byte is the key index, usually 0, so the receiver would read the IV
// length where the index belongs. libwebrtc's packetizer keeps every byte, and
// so does this element.
//
// Everything after the first slice's two clear bytes is ciphertext with RBSP
// escaping (CallFrameCryptor), so a start code can only be a real one and the
// frame splits at the same places as a clear frame.
#pragma once

typedef struct _GstElement GstElement;

namespace lightning::rtp {

/// Constrained Baseline, level 3.1, with level asymmetry allowed: what browsers
/// and LiveKit offer first. The level is the receive ceiling, not the stream's.
inline constexpr const char *kH264ProfileLevelId = "42e01f";

/// Register `lightningrtph264pay`. Idempotent, thread-safe; must run after
/// gst_init.
void registerH264Payloader();

/// The element name to use in a pipeline description.
const char *h264PayloaderName();

} // namespace lightning::rtp
