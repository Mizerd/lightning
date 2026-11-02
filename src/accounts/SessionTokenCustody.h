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

class TokenStore {
public:
    virtual ~TokenStore() = default;
    virtual QVariantMap accountRecord(const QString &userId) const = 0;
    virtual QString accessTokenFor(const QString &userId) const = 0;
    virtual bool updateSessionTokens(const QString &userId, const QString &access,
                                     const QString &refresh) = 0;
};

namespace custody {

QByteArray accessTokenDigest(const QString &accessToken);

SessionTokenPair parseFinalSessionTokens(const QString &json);

QString tokenOwnerFor(const TokenStore &store, const QStringList &candidates,
                      const QString &deviceId);

enum class LiveWrite : quint8 { Written, WriteFailed, SkippedRotatedPast, SkippedNoOwner };
LiveWrite writeLive(TokenStore *store, const QString &tokenOwner,
                    const QString &currentAccess, const SessionTokenPair &rotated);

enum class PersistAnswer : quint8 { None, Saved, NotSaved };
PersistAnswer persistAnswerFor(LiveWrite outcome);

bool retryWriteNeeded(const QString &currentAccess, const QByteArray &persistedDigest);

struct AdoptionReconcile {
    bool write = false;
    quint64 answerGeneration = 0;
    bool answerSavedIfNoWrite = false;
};
AdoptionReconcile reconcileAdopted(const QString &liveAccess, const QByteArray &persistedDigest,
                                   quint64 ledgerGeneration);

bool needsHandback(const QString &finalAccess, const QByteArray &lastWrittenDigest);

struct HandbackEntry {
    QString userId;
    QString deviceId;
    QByteArray expectedDigest;
    SessionTokenPair tokens;
};

enum class Handback : quint8 { Written, WriteFailed, SkippedNoStore, SkippedSameDeviceRunning,
                               SkippedRecordGoneOrOtherDevice, SkippedKeyringChanged };
QByteArray handbackExpectedDigest(TokenStore *store, const QString &userId,
                                  const QByteArray &expected,
                                  const QList<QByteArray> &accepted);

Handback writeHandback(TokenStore *store, const HandbackEntry &entry,
                       bool sameDeviceRunningAndNotRestoring);

}
}
