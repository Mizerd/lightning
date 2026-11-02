#pragma once

#include <memory>
#include <string_view>

namespace calls::noise {

enum class Mode { Off = 0, WebRtc = 1, RNNoise = 2, DeepFilterNet = 3 };

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
    virtual int frameSize() const = 0;
    virtual bool ok() const = 0;
    virtual void process(float *frame) noexcept = 0;
    virtual int latencySamples() const = 0;
    virtual void reset() noexcept = 0;
    virtual bool resetIsRealtimeSafe() const { return false; }
    virtual bool failed() const noexcept { return false; }
};

std::unique_ptr<NoiseSuppressor> createSuppressor(Mode mode);

bool backendCompiledIn(Mode mode);

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

inline bool isModeKey(std::string_view key)
{
    return key == "off" || key == "webrtc" || key == "rnnoise"
        || key == "deepfilternet";
}

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

constexpr Mode kDefaultMode = Mode::Off;

}
