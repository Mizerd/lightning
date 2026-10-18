#include "accounts/RevocationProofs.h"

#include <QSet>

namespace lightning::accounts {

bool revocationNeedsKeyringProbe(const RevocationEventFacts &f)
{
    return f.oauthSession && f.hadRefreshToken.has_value() && !*f.hadRefreshToken;
}

const char *signOutCause(const SignOutFacts &f)
{
    if (f.hadRefreshToken.has_value() && !*f.hadRefreshToken)
        return "no_refresh_token";
    if (!f.rotations.has_value() || !f.unsavedRotation.has_value())
        return "refused_unknown";
    if (*f.rotations <= 0)
        return "refused_restored_token";
    return *f.unsavedRotation ? "refused_after_unsaved_rotation" : "refused_saved_token";
}

RevocationClassification classifyRevocation(const RevocationEventFacts &f,
                                            bool keyringHoldsOrCannotTell)
{
    const bool softLogoutReported = f.softLogout.has_value() && *f.softLogout;
    // had_refresh_token=false on an OAuth session: matrix-sdk had nothing to
    // renew with. Only a keyring that holds one (or cannot answer) turns that
    // into "the session never got it"; otherwise it is ended.
    const bool oauthWithoutRefreshToken = revocationNeedsKeyringProbe(f);
    RevocationClassification out{RevocationOutcome::Unproven, softLogoutReported,
                                 oauthWithoutRefreshToken};
    if (keyringHoldsOrCannotTell && oauthWithoutRefreshToken) {
        out.outcome = RevocationOutcome::KeyringHoldsOrCannotTell;
        return out;
    }
    // A soft logout keeps the device on the server, so signing in again may
    // resume it; only a hard one proves this store's device gone. An OAuth
    // sign-in never asks for the device again, so for it a soft logout is as
    // final as a hard one. A missing field proves neither.
    const bool hardLogout = (f.softLogout.has_value() && !*f.softLogout)
        || (softLogoutReported && f.oauthSession) || oauthWithoutRefreshToken;
    const bool softLogout = softLogoutReported && !f.oauthSession;
    if (hardLogout)
        out.outcome = RevocationOutcome::Hard;
    else if (softLogout)
        out.outcome = RevocationOutcome::Soft;
    return out;
}

void RevocationProofs::recordHard(const QString &userId, const QString &deviceId)
{
    if (userId.isEmpty())
        return;
    m_hard.insert(userId, HardProof{deviceId});
    // An ended device is never resumed, whatever was said before.
    const auto soft = m_soft.constFind(userId);
    if (soft != m_soft.cend() && soft->deviceId == deviceId)
        m_soft.erase(soft);
}

bool RevocationProofs::recordSoft(const QString &userId, const QString &homeserver,
                                  const QString &deviceId)
{
    if (userId.isEmpty())
        return false;
    const auto hard = m_hard.constFind(userId);
    if (hard != m_hard.cend() && hard->deviceId == deviceId)
        return false;
    m_soft.insert(userId, SoftProof{homeserver, deviceId});
    return true;
}

void RevocationProofs::clearHard(const QString &userId)
{
    m_hard.remove(userId);
}

void RevocationProofs::clearSoft(const QString &userId)
{
    m_soft.remove(userId);
}

void RevocationProofs::clear(const QString &userId)
{
    clearHard(userId);
    clearSoft(userId);
}

std::optional<HardProof> RevocationProofs::hardFor(const QString &userId) const
{
    const auto it = m_hard.constFind(userId);
    if (it == m_hard.cend())
        return std::nullopt;
    return *it;
}

std::optional<SoftProof> RevocationProofs::softFor(const QString &userId) const
{
    const auto it = m_soft.constFind(userId);
    if (it == m_soft.cend())
        return std::nullopt;
    return *it;
}

int RevocationProofs::size() const
{
    QSet<QString> uids;
    for (auto it = m_hard.cbegin(); it != m_hard.cend(); ++it)
        uids.insert(it.key());
    for (auto it = m_soft.cbegin(); it != m_soft.cend(); ++it)
        uids.insert(it.key());
    return int(uids.size());
}

} // namespace lightning::accounts
