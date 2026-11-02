#pragma once

#include "calls/noise/NoiseSuppressor.h"

#include <QString>

#include <functional>

typedef struct _GstElement GstElement;

namespace calls::noise {

inline constexpr const char *kDenoiseName = "micdenoise";
inline constexpr const char *kDspName = "micdsp";

bool webrtcDspAvailable();
bool denoiseElementAvailable();

constexpr bool dspSuppressesNoise(Mode mode) { return mode == Mode::WebRtc; }

QString voiceProcessingDescription(Mode mode, bool dspAvailable,
                                   bool denoiseAvailable,
                                   bool echoCancel = false,
                                   bool autoGain = true);
QString voiceProcessingDescription(Mode mode);
QString voiceProcessingDescriptionWith(Mode mode, bool echoCancel,
                                       bool autoGain);

enum class Unavailable {
    None,
    NoCallEngine,
    NoWebrtcDsp,
    NotInThisBuild,
};
Unavailable availability(Mode mode, bool engineAvailable);
const char *unavailableKey(Unavailable reason);

enum class DspSwitch {
    Now,
    WaitForDenoiser,
    /// The chain is still starting on another thread: a webrtcdsp swap would
    /// add and start an element under that start, so none is made. A
    /// webrtcdsp not negotiated yet (the normal case while a live chain
    /// starts: caps come with its first buffer) takes the new configuration
    /// as a PROPERTY, which is all it needs, since it reads it when it
    /// negotiates. One already negotiated, or that negotiates meanwhile, is
    /// reported deferred (applyMode()'s `dspDeferred`) and the caller replaces
    /// it after the start with applyDspMode(..., forceReplace = true).
    Later,
};

constexpr bool dspWaitsForDenoiser(Mode mode)
{
    return mode == Mode::RNNoise || mode == Mode::DeepFilterNet;
}

QString applyMode(GstElement *binOrPipeline, Mode mode,
                  DspSwitch when = DspSwitch::Now,
                  bool *dspDeferred = nullptr, bool echoAllowed = true);

QString applyDspMode(GstElement *binOrPipeline, Mode mode,
                     bool forceReplace = false, bool echoAllowed = true);

/// Replaces a running webrtcdsp with one that cancels echo, keeping its
/// suppressor and gain switches. GUI thread. Its gain control starts over, as
/// for any replacement.
///
/// webrtcdsp takes its probe when it starts, and a start with none free is a
/// fatal error that silences the microphone, so the replacement is started
/// alone first. Refused, the running element stays and `refused` runs on the
/// thread that did the swap (a streaming thread when audio is flowing).
using EchoRefused = std::function<void()>;
QString engageEchoCancel(GstElement *binOrPipeline, EchoRefused refused = {});

QString dropEchoCancel(GstElement *binOrPipeline);

}
