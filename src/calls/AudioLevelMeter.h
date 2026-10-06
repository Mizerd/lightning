// A microphone level meter for the UI: the scale and the update rate.
//
// Shared by the in-call meter (SfuCallController, fed by the call's own
// `level` element) and the Settings microphone test (AudioDeviceTester), so
// both draw the same bar for the same sound.
//
// Header-only and free of GStreamer, so it is compiled and tested in every
// build, including one without the media engine.
#pragma once

#include <QtGlobal>

namespace lightning::calls {

/// dBFS at and below which the bar is empty. The bottom of a typical voice
/// meter: a quiet room peaks at -60..-78 dBFS and reads as empty, speech at a
/// normal distance lands around the middle.
inline constexpr double kMeterFloorDb = -60.0;

/// Shortest gap between two published readings: at most 25 per second. The
/// capture posts one every 50 ms (20 Hz); the margin keeps a reading that
/// arrives a millisecond early from being folded into the next one.
inline constexpr qint64 kMeterMinIntervalMs = 40;

/// 0..1 for a bar, linear in dB between kMeterFloorDb and 0 dBFS.
///
/// `level` reports exact digital silence as -350 dBFS (its own floor, a real
/// reading and not an error): that, NaN and anything at or below the floor
/// read as an empty bar.
inline double meterFraction(double peakDb)
{
    // Written as !(a > b) so NaN lands here too.
    if (!(peakDb > kMeterFloorDb))
        return 0.0;
    if (peakDb >= 0.0)
        return 1.0;
    return (peakDb - kMeterFloorDb) / -kMeterFloorDb;
}

/// Limits meter updates to kMeterMinIntervalMs without losing a peak.
///
/// Readings that arrive too soon are not thrown away: the loudest of them is
/// held and published with the next accepted one. A meter that dropped them
/// would miss a short word whenever the GUI thread delivered a backlog of
/// readings at once, which is exactly when it falls behind.
class MeterThrottle
{
public:
    /// Offers one reading taken at `nowMs`. Returns true when a reading
    /// should be published now, with the loudest peak since the last
    /// publication in `*publishDb`.
    bool offer(double peakDb, qint64 nowMs, double *publishDb)
    {
        if (!m_holding || peakDb > m_heldDb) {
            m_heldDb = peakDb;
            m_holding = true;
        }
        // A clock that went backwards publishes at once rather than never.
        if (m_lastMs >= 0 && nowMs >= m_lastMs
            && nowMs - m_lastMs < kMeterMinIntervalMs)
            return false;
        m_lastMs = nowMs;
        if (publishDb)
            *publishDb = m_heldDb;
        m_holding = false;
        return true;
    }

    /// Forgets the held peak and the rate history: a new capture starts
    /// fresh.
    void reset()
    {
        m_lastMs = -1;
        m_holding = false;
        m_heldDb = 0.0;
    }

private:
    qint64 m_lastMs = -1;
    bool m_holding = false;
    double m_heldDb = 0.0;
};

} // namespace lightning::calls
