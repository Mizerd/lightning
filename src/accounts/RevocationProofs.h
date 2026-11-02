#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace lightning::accounts {

struct RevocationEventFacts {
    std::optional<bool> softLogout;
    std::optional<bool> hadRefreshToken;
    bool oauthSession = false;
};

// oauthSession && hadRefreshToken == false. Only then may the caller read the keyring
// (same read, same order as today).
bool revocationNeedsKeyringProbe(const RevocationEventFacts &f);

enum class RevocationOutcome : quint8 {
    KeyringHoldsOrCannotTell,
    Hard,
    Soft,
    Unproven
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

struct SignOutFacts {
    std::optional<bool> hadRefreshToken;
    std::optional<qint64> rotations;
    std::optional<bool> unsavedRotation;
};
const char *signOutCause(const SignOutFacts &f);

struct HardProof { QString deviceId; };
struct SoftProof { QString homeserver; QString deviceId; };

class RevocationProofs
{
public:
    void recordHard(const QString &userId, const QString &deviceId);
    bool recordSoft(const QString &userId, const QString &homeserver,
                    const QString &deviceId);
    void clearHard(const QString &userId);
    void clearSoft(const QString &userId);
    void clear(const QString &userId);
    std::optional<HardProof> hardFor(const QString &userId) const;
    std::optional<SoftProof> softFor(const QString &userId) const;
    int size() const;

private:
    QHash<QString, HardProof> m_hard;
    QHash<QString, SoftProof> m_soft;
};

}
