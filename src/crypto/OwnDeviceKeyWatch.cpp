#include "crypto/OwnDeviceKeyWatch.h"

namespace matrix::crypto {

void OwnDeviceKeyWatch::reset()
{
    m_broken = false;
    m_answered = false;
    m_lastDispatchMs = -1;
}

bool OwnDeviceKeyWatch::apply(KeyAgreement agreement)
{
    // UNKNOWN IS NOT AN ANSWER, IN EITHER DIRECTION.
    //
    // It must not raise a fault (that would tell a user who happens to be
    // offline that their encryption is destroyed) and it must not clear one
    // either: the fault is permanent until the account is signed in again,
    // so a later "could not establish" saying nothing is the honest
    // behaviour, not silently declaring the device healthy.
    if (agreement == KeyAgreement::Unknown)
        return false;

    m_answered = true;
    const bool broken = (agreement == KeyAgreement::Mismatch);
    if (broken == m_broken)
        return false;
    m_broken = broken;
    return true;
}

bool OwnDeviceKeyWatch::checkDue(qint64 nowMs) const
{
    if (m_lastDispatchMs < 0)
        return true;
    // A clock that went backwards (suspend/resume, an NTP step) must not
    // wedge the watch until it catches up.
    if (nowMs < m_lastDispatchMs)
        return true;
    return (nowMs - m_lastDispatchMs) >= kMinIntervalMs;
}

void OwnDeviceKeyWatch::noteDispatched(qint64 nowMs)
{
    m_lastDispatchMs = nowMs;
}

} // namespace matrix::crypto
