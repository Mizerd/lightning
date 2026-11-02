#pragma once

#include "calls/noise/NoiseSuppressor.h"

#include <functional>
#include <memory>

typedef struct _GstElement GstElement;

namespace calls::noise {

void registerDenoiseElement();

const char *denoiseElementName();

const char *denoiseCaps();

inline constexpr const char *kStatusMessageName = "lightning-denoise";

/// Sets `mode` on a `lightningdenoise` element. Any thread but the element's
/// own streaming thread. Never blocks on backend construction.
void setDenoiseMode(GstElement *element, Mode mode);

struct DenoiseStatus {
    Mode requested = Mode::Off;
    Mode active = Mode::Off;
    bool ok = true;
    int latencySamples = 0;
    unsigned long long frames = 0;
};
DenoiseStatus denoiseStatus(GstElement *element);

int liveDenoiseElements();

using SuppressorFactory = std::function<std::unique_ptr<NoiseSuppressor>(Mode)>;
void setSuppressorFactoryForTest(SuppressorFactory factory);

}
