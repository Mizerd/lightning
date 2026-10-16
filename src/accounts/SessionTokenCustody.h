#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

namespace lightning::accounts {

// SENSITIVE: everything in this file handles access and refresh tokens. Never
// log any value here, nor a digest of one. Callers log slugs and outcomes only
// (CLAUDE.md §6).
//
// Factored out of RustSdkMatrixClient.cpp's three token paths (the live write
// on session_tokens_refreshed, the retry after a refused write, and the
// retirement handback), behaviour-identical: the same reads and writes against
// the store, in the same order, under the same conditions.
// SessionTokenCustodyParityTest holds a verbatim copy of the old inline logic
// as the oracle and compares call logs and outcomes.
struct SessionTokenPair { QString accessToken; QString refreshToken; };

class TokenStore {   // adapter; production impl over SettingsManager lives in RustSdkMatrixClient.cpp
public:
    virtual ~TokenStore() = default;
    virtual QVariantMap accountRecord(const QString &userId) const = 0;
    virtual QString accessTokenFor(const QString &userId) const = 0;
    virtual bool updateSessionTokens(const QString &userId, const QString &access,
                                     const QString &refresh) = 0;
};

namespace custody {

QByteArray accessTokenDigest(const QString &accessToken);   // "" -> empty; SHA-256(UTF-8).

SessionTokenPair parseFinalSessionTokens(const QString &json); // "", "error: ...", bad JSON -> {}

// First candidate (non-empty) whose record's "deviceId" == deviceId; "" if none.
// An empty deviceId owns nothing: a record without one is never this session's.
QString tokenOwnerFor(const TokenStore &store, const QStringList &candidates,
                      const QString &deviceId);

enum class LiveWrite : quint8 { Written, WriteFailed, SkippedRotatedPast, SkippedNoOwner };
// session_tokens_refreshed. currentAccess: final_session_tokens() access, "" if no handle
// or unreadable. Order: rotated-past check (no I/O) -> store && owner non-empty -> write.
// The caller resolves tokenOwner BEFORE reading currentAccess, as it always has.
LiveWrite writeLive(TokenStore *store /*nullable*/, const QString &tokenOwner,
                    const QString &currentAccess, const SessionTokenPair &rotated);

// retryRotatedTokenWrite: write only when currentAccess non-empty and its digest != persisted.
bool retryWriteNeeded(const QString &currentAccess, const QByteArray &persistedDigest);

// Retirement worker: hand back when finalAccess non-empty and digest != last written.
bool needsHandback(const QString &finalAccess, const QByteArray &lastWrittenDigest);

struct HandbackEntry {
    QString userId;
    QString deviceId;
    QByteArray expectedDigest;
    SessionTokenPair tokens;
};

enum class Handback : quint8 { Written, WriteFailed, SkippedNoStore, SkippedSameDeviceRunning,
                               SkippedRecordGoneOrOtherDevice, SkippedKeyringChanged };
// persistRetiredSessionTokens body. Caller computes sameDeviceRunningAndNotRestoring with
// today's short-circuit expression. I/O order: accountRecord -> accessTokenFor -> update (CAS).
Handback writeHandback(TokenStore *store /*nullable*/, const HandbackEntry &entry,
                       bool sameDeviceRunningAndNotRestoring);

} // namespace custody
} // namespace lightning::accounts
