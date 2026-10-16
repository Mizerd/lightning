#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace lightning::accounts {

// Pure classification factored out of handleRustEvent "session_token_revoked"
// in RustSdkMatrixClient.cpp. Behaviour-identical to the inline logic it
// replaced; RevocationProofsTest holds a verbatim copy of that logic as the
// oracle and compares the whole truth table.
struct RevocationEventFacts {
    std::optional<bool> softLogout;        // event "soft_logout" iff isBool()
    std::optional<bool> hadRefreshToken;   // event "had_refresh_token" iff isBool()
    bool oauthSession = false;             // m_settings && !m_userId.isEmpty() && isOAuthAccount(m_userId)
};

// oauthSession && hadRefreshToken == false. Only then may the caller read the keyring
// (same read, same order as today).
bool revocationNeedsKeyringProbe(const RevocationEventFacts &f);

enum class RevocationOutcome : quint8 {
    KeyringHoldsOrCannotTell,   // -> SecretBackendUnavailable card, no proof change
    Hard,                       // -> AccessTokenRevoked + hard proof
    Soft,                       // -> AccessTokenExpired + soft proof
    Unproven                    // -> AccessTokenRevoked, no proof
};

struct RevocationClassification {
    RevocationOutcome outcome;
    bool softLogoutReported;
    bool oauthWithoutRefreshToken;
};

// hard = (soft present && !soft) || (softReported && oauth) || (oauth && hadRefresh==false)
// soft = softReported && !oauth ; keyring flag wins first.
// `keyringHoldsOrCannotTell` is the answer of the probe the caller ran when
// revocationNeedsKeyringProbe() said so. It is only ever an answer to THAT
// probe: when no probe was needed it is ignored, exactly as the inline code
// never computed it.
RevocationClassification classifyRevocation(const RevocationEventFacts &f,
                                            bool keyringHoldsOrCannotTell);

// Per-uid proof map (AppController-owned, process memory, never persisted).
//
// The client's single-slot fields (m_revokedUserId/DeviceId, m_softLogout*)
// remain the ONLY thing any decision reads in Phase 0. This map is a mirror
// of every write and clear of those fields, so Phase 1 can consult proofs for
// an account that is not the one in the slot. Holds no secret: user ids,
// device ids and a homeserver URL.
struct HardProof { QString deviceId; };
struct SoftProof { QString homeserver; QString deviceId; };   // homeserver normalised

class RevocationProofs
{
public:
    void recordHard(const QString &userId, const QString &deviceId);  // also drops that uid's
                                                                      // soft proof iff same device
    bool recordSoft(const QString &userId, const QString &homeserver,
                    const QString &deviceId);   // refused (false) iff that uid's hard proof has
                                                // the same device
    void clearHard(const QString &userId);
    void clearSoft(const QString &userId);
    void clear(const QString &userId);          // both
    std::optional<HardProof> hardFor(const QString &userId) const;
    std::optional<SoftProof> softFor(const QString &userId) const;
    int size() const;                           // uids holding any proof

private:
    QHash<QString, HardProof> m_hard;
    QHash<QString, SoftProof> m_soft;
};

} // namespace lightning::accounts
