#pragma once

#include <QtGlobal>

// Policy for "this device can never decrypt anything".
//
// A device can publish a curve25519 identity key its local Olm account does
// not hold. Peers encrypt to the published key, so every room key and call
// media key sent to it is unreadable, permanently, while sending still
// works. A fresh sign-in repairs it. This class holds the two decisions the
// application must get right, testable without a session:
//
//   1. Tri-state: Unknown is not broken. The check cannot answer offline,
//      before key upload, or on a /keys/query 5xx, and reporting that as a
//      fault would alarm healthy users. Only a disagreement sets the fault,
//      only an agreement clears it.
//
//   2. Rate limited. Event-driven checks (login, first sync, verification,
//      refresh) arrive in bursts and share a minimum gap; a periodic backstop
//      catches faults that appear after login.
//
// No key material passes through here, only whether the keys agree.
namespace matrix::crypto {

// Mirrors the Rust side's Option<bool>; the zero value is "not answered".
enum class KeyAgreement {
    Unknown = 0, // could not be established — NOT a fault
    Matches,     // healthy
    Mismatch,    // the fault: nothing addressed to this device can be read
};

class OwnDeviceKeyWatch
{
public:
    // Minimum gap between dispatched checks, bounding sign-in bursts.
    static constexpr qint64 kMinIntervalMs = 60 * 1000;
    // Periodic backstop: four single-device /keys/query requests an hour.
    static constexpr qint64 kRecheckIntervalMs = 15 * 60 * 1000;

    // A new session (sign-in, sign-out, account switch) knows nothing.
    void reset();

    // Returns true only when broken() changed, so callers notify on real
    // transitions.
    bool apply(KeyAgreement agreement);

    // The fault, and only the fault.
    bool broken() const { return m_broken; }
    // Whether the question has ever been answered either way this session.
    bool answered() const { return m_answered; }

    // Rate limit. True when a check may be dispatched now.
    bool checkDue(qint64 nowMs) const;
    void noteDispatched(qint64 nowMs);

private:
    bool m_broken = false;
    bool m_answered = false;
    // Negative: never dispatched this session, so always due.
    qint64 m_lastDispatchMs = -1;
};

} // namespace matrix::crypto
