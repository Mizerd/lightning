// Microphone noise suppression: the backend contract (GitHub #20).
//
// Exactly one suppressor runs on the outgoing microphone: Off, WebRTC (the
// `webrtcdsp` element's own suppressor), RNNoise or DeepFilterNet. WebRTC is
// not a NoiseSuppressor: it lives inside `webrtcdsp`, which also carries the
// high-pass filter and the gain control every mode keeps. RNNoise and
// DeepFilterNet run inside Lightning's own `lightningdenoise` element
// (DenoiseElement.h), which frames the stream for them.
//
// Pure C++: no Qt, no GStreamer, so a backend can include it freely.
#pragma once

#include <memory>
#include <string_view>

namespace calls::noise {

enum class Mode { Off = 0, WebRtc = 1, RNNoise = 2, DeepFilterNet = 3 };

/// Samples per process() call: 10 ms at 48 kHz, what both neural backends
/// work in. The element refuses a suppressor with any other frame size.
constexpr int kFrameSamples = 480;
constexpr int kSampleRate = 48000;

// Processes mono float32 at 48 kHz in fixed frames of frameSize() samples
// (480 = 10 ms for both RNNoise and DeepFilterNet), in place, samples
// nominally in [-1, 1].
//
// Threading, as the element uses it: constructed and destroyed on a GStreamer
// thread-pool thread (never the streaming thread, never the GUI thread);
// process() and reset() on the streaming thread only, so neither may
// allocate, lock or log; the const accessors from any thread.
class NoiseSuppressor {
public:
    virtual ~NoiseSuppressor() = default;
    virtual Mode mode() const = 0;
    virtual int frameSize() const = 0;             // samples per call
    virtual bool ok() const = 0;                    // init succeeded
    virtual void process(float *frame) noexcept = 0; // exactly frameSize() samples, in place
    /// The algorithmic delay the backend adds, in samples, BEYOND the
    /// element's own one-frame framing delay.
    virtual int latencySamples() const = 0;
    virtual void reset() noexcept = 0;              // drop internal state (flush / call restart)
    /// Whether reset() may run on the streaming thread in the middle of a
    /// call: no allocation, no lock, bounded time. The element then calls it
    /// when the stream resumes after a gap (the mute valve reopening), so the
    /// backend's own history (its lookahead and overlap buffers) does not
    /// replay the moment of muting after unmute. False unless the backend
    /// has been checked: a false here costs only that replay.
    virtual bool resetIsRealtimeSafe() const { return false; }
    /// The backend has stopped processing for good (a caught panic, a
    /// poisoned state) and now passes audio through. Read by the streaming
    /// thread after every frame, so it must be a plain load. The element then
    /// retires the backend and reports the mode as failed, which brings in
    /// the WebRTC fallback; a re-select of the mode builds a fresh one.
    virtual bool failed() const noexcept { return false; }
};

/// nullptr when the mode has no in-process backend (Off, WebRtc), when the
/// backend is not compiled into this build, or when it failed to initialise
/// (ok() false). nullptr means pass-through. Never throws. May be slow
/// (model setup): call it off the GUI and streaming threads.
std::unique_ptr<NoiseSuppressor> createSuppressor(Mode mode);

/// Whether this build carries the backend at all. Off and WebRtc: true; the
/// WebRTC suppressor's presence is a runtime GStreamer question, answered by
/// MicProcessing's availability().
bool backendCompiledIn(Mode mode);

// Header-only, so SettingsManager can validate the stored key without linking
// any audio code.

/// The persisted spelling: "off", "webrtc", "rnnoise", "deepfilternet".
inline const char *modeKey(Mode mode)
{
    switch (mode) {
    case Mode::Off:
        return "off";
    case Mode::WebRtc:
        return "webrtc";
    case Mode::RNNoise:
        return "rnnoise";
    case Mode::DeepFilterNet:
        return "deepfilternet";
    }
    return "webrtc";
}

/// Whether `key` is exactly one of modeKey()'s spellings.
inline bool isModeKey(std::string_view key)
{
    return key == "off" || key == "webrtc" || key == "rnnoise"
        || key == "deepfilternet";
}

/// Parses modeKey()'s spelling. Anything else, including a mode a newer build
/// may add, is `fallback` — never the nearest enum value (an enum is not a
/// quantity to clamp).
inline Mode modeFromKey(std::string_view key, Mode fallback)
{
    if (key == "off")
        return Mode::Off;
    if (key == "webrtc")
        return Mode::WebRtc;
    if (key == "rnnoise")
        return Mode::RNNoise;
    if (key == "deepfilternet")
        return Mode::DeepFilterNet;
    return fallback;
}

/// What a fresh install uses: Off (Rokas, 2026-10-07). The suppressors are a
/// Labs choice; RNNoise is the one recommended in the UI, measured best on real
/// speech (LibriSpeech + DEMAND/MUSAN mixes: the only mode above the input's
/// STOI at every SNR). Before 2026-10-07 the default was WebRTC.
constexpr Mode kDefaultMode = Mode::Off;

} // namespace calls::noise
