#pragma once

namespace lightning::macaudio {
// Registered after gst_init. Capture starts only when the pipeline is played,
// so probing availability never requests screen/audio recording permission.
void registerSource();
int captureDiagnostic();
}
