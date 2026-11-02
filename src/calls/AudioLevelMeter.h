#pragma once

#include <QtGlobal>

namespace lightning::calls {

inline constexpr double kMeterFloorDb = -60.0;

inline constexpr qint64 kMeterMinIntervalMs = 40;

/// 0..1 for a bar, linear in dB between kMeterFloorDb and 0 dBFS.
///
/// `level` reports exact digital silence as -350 dBFS (its own floor, a real
/// reading and not an error): that, NaN and anything at or below the floor
/// read as an empty bar.
inline double meterFraction(double peakDb)
{
    if (!(peakDb > kMeterFloorDb))
        return 0.0;
    if (peakDb >= 0.0)
        return 1.0;
    return (peakDb - kMeterFloorDb) / -kMeterFloorDb;
}

class MeterThrottle
{
public:
    bool offer(double peakDb, qint64 nowMs, double *publishDb)
    {
        if (!m_holding || peakDb > m_heldDb) {
            m_heldDb = peakDb;
            m_holding = true;
        }
        if (m_lastMs >= 0 && nowMs >= m_lastMs
            && nowMs - m_lastMs < kMeterMinIntervalMs)
            return false;
        m_lastMs = nowMs;
        if (publishDb)
            *publishDb = m_heldDb;
        m_holding = false;
        return true;
    }

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

}
