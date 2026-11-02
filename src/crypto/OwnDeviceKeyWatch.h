#pragma once

#include <QtGlobal>

namespace matrix::crypto {

enum class KeyAgreement {
    Unknown = 0,
    Matches,     // healthy
    Mismatch,
};

class OwnDeviceKeyWatch
{
public:
    static constexpr qint64 kMinIntervalMs = 60 * 1000;
    static constexpr qint64 kRecheckIntervalMs = 15 * 60 * 1000;

    void reset();

    bool apply(KeyAgreement agreement);

    bool broken() const { return m_broken; }
    bool answered() const { return m_answered; }

    bool checkDue(qint64 nowMs) const;
    void noteDispatched(qint64 nowMs);

private:
    bool m_broken = false;
    bool m_answered = false;
    qint64 m_lastDispatchMs = -1;
};

}
