#include "calls/noise/NoiseSuppressor.h"

// The backends are optional per build. Each header declares a create function
// returning nullptr when the backend is compiled out or failed, and an
// availability function; this file calls nothing else of theirs. A header
// absent from the tree means the backend does not exist in this checkout.
#if __has_include("calls/noise/RnnoiseSuppressor.h")
#include "calls/noise/RnnoiseSuppressor.h"
#define LIGHTNING_NOISE_HAS_RNNOISE_HEADER 1
#endif
#if __has_include("calls/noise/DeepFilterSuppressor.h")
#include "calls/noise/DeepFilterSuppressor.h"
#define LIGHTNING_NOISE_HAS_DFN_HEADER 1
#endif

namespace calls::noise {

namespace {

std::unique_ptr<NoiseSuppressor> checked(std::unique_ptr<NoiseSuppressor> s,
                                         Mode expected)
{
    // A backend that reports failure, the wrong mode, or a frame size the
    // element cannot frame for is no backend: pass-through instead.
    if (!s || !s->ok() || s->mode() != expected
        || s->frameSize() != kFrameSamples || s->latencySamples() < 0) {
        return nullptr;
    }
    return s;
}

} // namespace

std::unique_ptr<NoiseSuppressor> createSuppressor(Mode mode)
{
    try {
        switch (mode) {
        case Mode::Off:
        case Mode::WebRtc:
            return nullptr;
        case Mode::RNNoise:
#ifdef LIGHTNING_NOISE_HAS_RNNOISE_HEADER
            return checked(createRnnoiseSuppressor(), Mode::RNNoise);
#else
            return nullptr;
#endif
        case Mode::DeepFilterNet:
#ifdef LIGHTNING_NOISE_HAS_DFN_HEADER
            return checked(createDeepFilterSuppressor(), Mode::DeepFilterNet);
#else
            return nullptr;
#endif
        }
    } catch (...) {
        // An allocation failure in a backend constructor: pass-through.
    }
    return nullptr;
}

bool backendCompiledIn(Mode mode)
{
    switch (mode) {
    case Mode::Off:
    case Mode::WebRtc:
        return true;
    case Mode::RNNoise:
#ifdef LIGHTNING_NOISE_HAS_RNNOISE_HEADER
        return rnnoiseAvailable();
#else
        return false;
#endif
    case Mode::DeepFilterNet:
#ifdef LIGHTNING_NOISE_HAS_DFN_HEADER
        return deepFilterAvailable();
#else
        return false;
#endif
    }
    return false;
}

} // namespace calls::noise
