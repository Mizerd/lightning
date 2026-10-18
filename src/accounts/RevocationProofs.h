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

// Why a session_token_revoked was declared, for the log line that ends a
// session and for LIGHTNING_TOKEN_TRACE (a word, never a token). matrix-sdk
// sends that event for an OAuth session only when the authorization server
// answered invalid_grant or it held no refresh token: a network error, a 5xx,
// a timeout or an unreachable token endpoint never reaches it (matrix-sdk
// 0.18 client/futures.rs). The Rust event's ledger counts
// (rust/src/sessiontokens.rs) then tell the refusals apart:
//   no_refresh_token         - nothing to renew with;
//   refused_restored_token   - the refresh token read from the keyring at this
//                              start: ended while Lightning was closed, or a
//                              rotation an earlier run failed to save;
//   refused_after_unsaved_rotation - refused after this session rotated past
//                              the last pair the keyring confirmed: likely ours;
//   refused_saved_token      - refused a pair the keyring holds: ended on the
//                              server (another client, an admin, expiry);
//   refused_unknown          - an event without the counts.
struct SignOutFacts {
    std::optional<bool> hadRefreshToken;
    std::optional<qint64> rotations;
    std::optional<bool> unsavedRotation;
};
const char *signOutCause(const SignOutFacts &f);

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
