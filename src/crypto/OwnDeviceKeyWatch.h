#pragma once

#include <QtGlobal>

// B011: the policy half of "this device can never decrypt anything".
//
// THE DEFECT. A device had published a curve25519 identity key that its own
// local Olm account did not hold. Peers encrypt to the key the SERVER
// publishes, so every room key and every call media key addressed to it was
// unreadable, permanently: encrypted messages sit on "Waiting for keys…"
// forever, and an encrypted call is silent one way while the other side hears
// you perfectly, because SENDING is unaffected. Diagnosed on a real account
// 2026-09-07 (audit B006); only matrix-sdk's own tracing could see it, and a
// fresh sign-in repaired it instantly.
//
// Detection shipped in 0d4578d and stopped at one qCCritical into a log the
// user never reads. This class holds the two decisions the application layer
// has to get right around that answer, in one place a test can drive without
// a session, a network, or a broken device:
//
//   1. THE LATCH IS TRI-STATE, AND "UNKNOWN" IS NOT "BROKEN". The check
//      cannot answer while offline, before keys are uploaded, or through a
//      5xx on /keys/query. Publishing that as a fault would tell healthy
//      users their encryption is destroyed — worse than saying nothing. Only
//      an explicit disagreement sets the fault; only an explicit agreement
//      clears it; Unknown changes nothing in either direction.
//
//   2. RE-CHECKING IS RATE LIMITED. The fault can appear after login, so one
//      check at sign-in is not enough — but the check costs a /keys/query,
//      and a request per minute is not acceptable. Event-driven callers
//      (login, first sync, after a verification, an explicit refresh) arrive
//      in bursts within seconds of each other, so they share one minimum
//      gap; the periodic backstop uses kRecheckIntervalMs.
//
// No key material passes through here. The only fact carried is whether the
// two keys agree, which is all the remedy depends on.
namespace matrix::crypto {

// Tri-state, mirroring the Rust side's Option<bool>. Absent is deliberately
// the zero value so a default-constructed watch is "not answered yet".
enum class KeyAgreement {
    Unknown = 0, // could not be established — NOT a fault
    Matches,     // healthy
    Mismatch,    // the fault: nothing addressed to this device can be read
};

class OwnDeviceKeyWatch
{
public:
    // The shortest gap between two dispatched checks. Bounds the burst from
    // the event-driven callers, which can all fire inside one second on a
    // fresh sign-in.
    static constexpr qint64 kMinIntervalMs = 60 * 1000;
    // The periodic backstop, for a fault that appears after login. Four
    // single-device /keys/query requests an hour.
    static constexpr qint64 kRecheckIntervalMs = 15 * 60 * 1000;

    // A new session (sign-in, sign-out, account switch) knows nothing.
    void reset();

    // Fold one answer in. Returns true when broken() actually changed, so
    // the caller emits its notify signal only on a real transition.
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
    // Negative means "never dispatched this session", which is always due.
    qint64 m_lastDispatchMs = -1;
};

} // namespace matrix::crypto
