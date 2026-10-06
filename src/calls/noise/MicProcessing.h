// The microphone's voice-processing stage, shared by the MatrixRTC publish
// chain and the Settings microphone test (GitHub #20).
//
//   ... ! audio/x-raw,channels=1 ! [valve]
//   ! audioconvert ! audioresample ! <F32 mono 48 kHz>
//   ! lightningdenoise name=micdenoise mode=<mode>        RNNoise | DeepFilterNet
//   ! audioconvert ! audio/x-raw,format=S16LE,rate=48000
//   ! webrtcdsp name=micdsp echo-cancel=false gain-control=true
//               noise-suppression=<mode == webrtc>       high-pass + AGC always
//   ! audioconvert ! [volume, level, opusenc ...]
//
// Order: noise suppression BEFORE gain control, which is the order WebRTC's
// own audio processing module runs (high-pass, echo, noise, gain). The AGC
// then adapts to speech rather than to a fan, and never lifts a noise floor
// the suppressor has to fight. Both elements are optional (webrtcdsp is
// gst-plugins-bad); the chain degrades to whichever exists.
//
// Exactly one suppressor runs: in Off both are pass-through for noise; in
// WebRTC only webrtcdsp suppresses; in RNNoise/DeepFilterNet only
// lightningdenoise does. High-pass filtering and gain control are not noise
// suppression and run in every mode, exactly as before the selector.
#pragma once

#include "calls/noise/NoiseSuppressor.h"

#include <QString>

typedef struct _GstElement GstElement;

namespace calls::noise {

/// Element names inside the chain; the call and the mic test both use them.
inline constexpr const char *kDenoiseName = "micdenoise";
inline constexpr const char *kDspName = "micdsp";

/// Whether the WebRTC suppressor's element exists in this GStreamer.
/// Probed once; needs GStreamer initialised.
bool webrtcDspAvailable();
/// Registers lightningdenoise and says whether it can be built.
bool denoiseElementAvailable();

/// Does `webrtcdsp` suppress noise in this mode?
constexpr bool dspSuppressesNoise(Mode mode) { return mode == Mode::WebRtc; }

/// The parse fragment, starting with "! " and ending with a space, for the
/// given availability. Pure, for tests.
QString voiceProcessingDescription(Mode mode, bool dspAvailable,
                                   bool denoiseAvailable);
/// The same for this runtime.
QString voiceProcessingDescription(Mode mode);

/// Why a mode cannot be chosen, or None.
enum class Unavailable {
    None,
    NoCallEngine,   ///< built without GStreamer or the engine refused to start
    NoWebrtcDsp,    ///< this GStreamer has no webrtcdsp (gst-plugins-bad)
    NotInThisBuild, ///< the backend was not compiled in
};
/// Settings: can this mode be selected here? `engineAvailable` is whether a
/// call engine exists at all.
Unavailable availability(Mode mode, bool engineAvailable);
const char *unavailableKey(Unavailable reason);

/// When applyMode() changes webrtcdsp's suppressor.
enum class DspSwitch {
    /// At once.
    Now,
    /// For a neural mode, not until the denoiser reports its backend running
    /// (or failed): the caller then calls applyDspMode(). A neural backend
    /// takes up to a second to build (DeepFilterNet unpacks its model), and
    /// switching WebRTC off at once left that second unsuppressed (measured
    /// live, 2026-10-06). Into Off or WebRTC the switch is always at once.
    WaitForDenoiser,
};

/// Whether a switch to `mode` under DspSwitch::WaitForDenoiser leaves
/// webrtcdsp as it is until applyDspMode().
constexpr bool dspWaitsForDenoiser(Mode mode)
{
    return mode == Mode::RNNoise || mode == Mode::DeepFilterNet;
}

/// Applies `mode` to a running chain: `binOrPipeline` is the call's
/// publishing bin or the mic test's pipeline. GUI thread. The denoise element
/// switches backends between frames; webrtcdsp only reads its configuration
/// when it negotiates, so a live change of its suppressor REPLACES the
/// element under an idle pad probe (its gain control starts over). A chain
/// without a denoiser always switches webrtcdsp at once: nothing would ever
/// report. Returns a short description of what it did, for the log.
QString applyMode(GstElement *binOrPipeline, Mode mode,
                  DspSwitch when = DspSwitch::Now);

/// The webrtcdsp half of applyMode(), for a switch that waited for the
/// denoiser. GUI thread.
QString applyDspMode(GstElement *binOrPipeline, Mode mode);

} // namespace calls::noise
