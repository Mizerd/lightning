#pragma once

#include <cstdint>
#include <optional>

namespace lightning::macaudio {
// ScreenCaptureKit timestamps are on the host clock. Anchor that clock to
// pipeline running time once, rather than stamping each callback's arrival.
// This also works when a share joins a call that has been running for minutes.
class CaptureTimeline {
public:
    static constexpr std::int64_t maxAgeNs = 100'000'000;

    std::optional<std::uint64_t> map(std::int64_t captured,
                                    std::int64_t hostNow,
                                    std::uint64_t runningNow)
    {
        if (captured < 0 || captured > hostNow || hostNow - captured > maxAgeNs)
            return {};
        if (!anchored) {
            hostOrigin = hostNow;
            runningOrigin = static_cast<std::int64_t>(runningNow);
            anchored = true;
        }
        const auto pts = runningOrigin + captured - hostOrigin;
        if (pts < 0 || (lastCapture && captured <= *lastCapture))
            return {};
        lastCapture = captured;
        return static_cast<std::uint64_t>(pts);
    }

private:
    bool anchored = false;
    std::int64_t hostOrigin = 0;
    std::int64_t runningOrigin = 0;
    std::optional<std::int64_t> lastCapture;
};
}
