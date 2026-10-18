#include "accounts/SessionTokenCustody.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

namespace lightning::accounts::custody {

QByteArray accessTokenDigest(const QString &accessToken)
{
    return accessToken.isEmpty()
        ? QByteArray()
        : QCryptographicHash::hash(accessToken.toUtf8(), QCryptographicHash::Sha256);
}

SessionTokenPair parseFinalSessionTokens(const QString &json)
{
    const QJsonObject tokens = json.isEmpty() || json.startsWith(QLatin1String("error: "))
        ? QJsonObject()
        : QJsonDocument::fromJson(json.toUtf8()).object();
    return SessionTokenPair{tokens.value(QStringLiteral("access_token")).toString(),
                            tokens.value(QStringLiteral("refresh_token")).toString()};
}

QString tokenOwnerFor(const TokenStore &store, const QStringList &candidates,
                      const QString &deviceId)
{
    if (deviceId.isEmpty())
        return {};
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        const QVariantMap record = store.accountRecord(candidate);
        if (!record.isEmpty()
            && record.value(QStringLiteral("deviceId")).toString() == deviceId)
            return candidate;
    }
    return {};
}

LiveWrite writeLive(TokenStore *store, const QString &tokenOwner,
                    const QString &currentAccess, const SessionTokenPair &rotated)
{
    // Only the pair the session holds NOW is written: two rotations can wait
    // in one drain, and the older one must never land over the newer (its
    // refresh token is already used up).
    if (!currentAccess.isEmpty()
        && accessTokenDigest(currentAccess) != accessTokenDigest(rotated.accessToken))
        return LiveWrite::SkippedRotatedPast;
    if (!store || tokenOwner.isEmpty())
        return LiveWrite::SkippedNoOwner;
    return store->updateSessionTokens(tokenOwner, rotated.accessToken, rotated.refreshToken)
        ? LiveWrite::Written
        : LiveWrite::WriteFailed;
}

PersistAnswer persistAnswerFor(LiveWrite outcome)
{
    switch (outcome) {
    case LiveWrite::Written:
        return PersistAnswer::Saved;
    case LiveWrite::WriteFailed:
    case LiveWrite::SkippedNoOwner:
        return PersistAnswer::NotSaved;
    case LiveWrite::SkippedRotatedPast:
        break;
    }
    return PersistAnswer::None;
}

bool retryWriteNeeded(const QString &currentAccess, const QByteArray &persistedDigest)
{
    return !(currentAccess.isEmpty() || accessTokenDigest(currentAccess) == persistedDigest);
}

bool needsHandback(const QString &finalAccess, const QByteArray &lastWrittenDigest)
{
    return !finalAccess.isEmpty() && accessTokenDigest(finalAccess) != lastWrittenDigest;
}

Handback writeHandback(TokenStore *store, const HandbackEntry &entry,
                       bool sameDeviceRunningAndNotRestoring)
{
    if (!store)
        return Handback::SkippedNoStore;
    // A session of that very device running again already holds newer tokens
    // than the one that retired.
    if (sameDeviceRunningAndNotRestoring)
        return Handback::SkippedSameDeviceRunning;
    // Signed out, removed, or signed in again as another device since: what
    // was deleted must stay deleted (CLAUDE.md §6).
    const QVariantMap record = store->accountRecord(entry.userId);
    if (record.isEmpty()
        || record.value(QStringLiteral("deviceId")).toString() != entry.deviceId)
        return Handback::SkippedRecordGoneOrOtherDevice;
    // Compare and swap: only over the pair this handle itself last wrote.
    // Anything else there is newer and must never be replaced by an older one.
    const QString stored = store->accessTokenFor(entry.userId);
    if (accessTokenDigest(stored) != entry.expectedDigest)
        return Handback::SkippedKeyringChanged;
    return store->updateSessionTokens(entry.userId, entry.tokens.accessToken,
                                      entry.tokens.refreshToken)
        ? Handback::Written
        : Handback::WriteFailed;
}

} // namespace lightning::accounts::custody
