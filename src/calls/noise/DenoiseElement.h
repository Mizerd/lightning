// `lightningdenoise`: Lightning's in-process GStreamer element that runs the
// selected neural noise suppressor (RNNoise or DeepFilterNet) on the outgoing
// microphone (GitHub #20).
//
// In place on mono F32 at 48 kHz, any buffer size. Internally it frames the
// stream into the suppressor's 480-sample frames through a one-frame delay
// line, so every buffer leaves with exactly the samples and timestamps it
// arrived with, and the stream is delayed by a constant 480 samples (10 ms)
// plus the backend's own latency, which it reports in the LATENCY query.
//
// Properties:
//   mode            (enum, read/write, mutable in PLAYING) off | webrtc |
//                   rnnoise | deepfilternet. webrtc and off are PASS-THROUGH
//                   here: WebRTC suppression lives in `webrtcdsp`. Setting a
//                   neural mode builds the backend on a GStreamer thread-pool
//                   thread; the streaming thread adopts it between frames.
//   active-mode     (enum, read) the backend actually running, `off` when
//                   none (pass-through)
//   backend-ok      (bool, read) the requested mode is in effect; false while
//                   a requested backend failed and the stream passes through
//   frames          (uint64, read) frames the backend has processed
//   latency-samples (int, read) the delay this element currently adds
//
// When a requested mode takes effect (or fails), the element posts an element
// message named kStatusMessageName with `requested` and `active` (mode keys),
// `ok` (boolean) and `latency-samples` (int).
//
// Pass-through is bit-exact: with no backend the buffer is not touched.
#pragma once

#include "calls/noise/NoiseSuppressor.h"

#include <functional>
#include <memory>

typedef struct _GstElement GstElement;

namespace calls::noise {

/// Register `lightningdenoise`. Idempotent, thread-safe; after gst_init.
void registerDenoiseElement();

/// The factory name to use in a pipeline description.
const char *denoiseElementName();

/// The caps the element accepts, for the capsfilter in front of it.
const char *denoiseCaps();

/// Element message name; see the header comment.
inline constexpr const char *kStatusMessageName = "lightning-denoise";

/// Sets `mode` on a `lightningdenoise` element. Any thread but the element's
/// own streaming thread. Never blocks on backend construction.
void setDenoiseMode(GstElement *element, Mode mode);

struct DenoiseStatus {
    Mode requested = Mode::Off;
    /// Off when nothing is running (pass-through).
    Mode active = Mode::Off;
    bool ok = true;
    int latencySamples = 0;
    unsigned long long frames = 0;
};
/// Reads the element's status properties. Any thread.
DenoiseStatus denoiseStatus(GstElement *element);

/// `lightningdenoise` elements alive in this process (initialised and not yet
/// finalized), for leak tests. Any thread.
int liveDenoiseElements();

/// Test seam: replaces createSuppressor() process-wide for every element; an
/// empty function restores the real factory. Called on a thread-pool thread.
using SuppressorFactory = std::function<std::unique_ptr<NoiseSuppressor>(Mode)>;
void setSuppressorFactoryForTest(SuppressorFactory factory);

} // namespace calls::noise
