// lightning::accounts::custody was factored out of RustSdkMatrixClient's
// three token paths in live accounts Phase 0, behaviour-identical. Each
// ORACLE below is a VERBATIM copy of the inline code it replaced (9177fc58,
// RustSdkMatrixClient.cpp: session_tokens_refreshed ~5626, the retirement
// worker ~627, persistRetiredSessionTokens ~683, retryRotatedTokenWrite ~743,
// tokenOwnerForRunningSession ~808), with `m_settings` replaced by a
// TokenStore and members by parameters. Do not "tidy" the oracles.
//
// A counting fake store logs every (call, uid) in order. For every row the
// oracle and the new code run against two identical stores and must leave the
// same call log, the same store contents and the same outcome. All token
// values here are fakes.
#include "accounts/SessionTokenCustody.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QtTest>

#include <functional>

using namespace lightning::accounts;

namespace {

class FakeStore final : public TokenStore
{
public:
    QHash<QString, QVariantMap> records;
    QHash<QString, QString> access;
    QHash<QString, QString> refresh;
    bool updateOk = true;
    mutable QStringList log;

    QVariantMap accountRecord(const QString &userId) const override
    {
        log << QStringLiteral("accountRecord:%1").arg(userId);
        return records.value(userId);
    }
    QString accessTokenFor(const QString &userId) const override
    {
        log << QStringLiteral("accessTokenFor:%1").arg(userId);
        return access.value(userId);
    }
    bool updateSessionTokens(const QString &userId, const QString &a,
                             const QString &r) override
    {
        log << QStringLiteral("updateSessionTokens:%1:%2:%3").arg(userId, a, r);
        if (!updateOk)
            return false;
        access.insert(userId, a);
        refresh.insert(userId, r);
        return true;
    }
    bool sameStateAs(const FakeStore &other) const
    {
        return log == other.log && access == other.access && refresh == other.refresh;
    }
};

QVariantMap recordFor(const QString &deviceId)
{
    QVariantMap record;
    record.insert(QStringLiteral("homeserver"), QStringLiteral("https://hs"));
    if (!deviceId.isNull())
        record.insert(QStringLiteral("deviceId"), deviceId);
    return record;
}

// Store contents every path is run against.
QList<FakeStore> storeScenarios()
{
    QList<FakeStore> out;
    for (bool updateOk : {true, false}) {
        FakeStore empty;
        empty.updateOk = updateOk;
        out << empty;

        FakeStore plain;
        plain.updateOk = updateOk;
        plain.records.insert(QStringLiteral("@a:hs"), recordFor(QStringLiteral("DEV")));
        plain.access.insert(QStringLiteral("@a:hs"), QStringLiteral("A1"));
        out << plain;

        // The record names another device under the raw id and this one
        // under the canonical id, whose keyring holds A2.
        FakeStore canonical;
        canonical.updateOk = updateOk;
        canonical.records.insert(QStringLiteral("@a:hs"), recordFor(QStringLiteral("OTHER")));
        canonical.records.insert(QStringLiteral("@A:hs"), recordFor(QStringLiteral("DEV")));
        canonical.access.insert(QStringLiteral("@A:hs"), QStringLiteral("A2"));
        out << canonical;

        // A record with no deviceId, and the settings account on DEV with an
        // empty keyring.
        FakeStore odd;
        odd.updateOk = updateOk;
        odd.records.insert(QStringLiteral("@a:hs"), recordFor(QString()));
        odd.records.insert(QStringLiteral("@s:hs"), recordFor(QStringLiteral("DEV")));
        out << odd;
    }
    return out;
}

const QStringList kFinalJsons{
    QString(),
    QStringLiteral("error: no client"),
    QStringLiteral("not json"),
    QStringLiteral("[1,2]"),
    QStringLiteral("{}"),
    QStringLiteral(R"({"access_token":"A1","refresh_token":"R1"})"),
    QStringLiteral(R"({"access_token":"A2","refresh_token":"R2"})"),
    QStringLiteral(R"({"access_token":"","refresh_token":"R3"})"),
    QStringLiteral(R"({"access_token":5,"refresh_token":"R5"})"),
    QStringLiteral(R"({"refresh_token":"R4"})"),
    QStringLiteral(R"({"access_token":"A1"})"),
};

// ======================= ORACLES: the old code =========================

// Old anonymous-namespace helper, verbatim.
QByteArray oracleAccessTokenDigest(const QString &accessToken)
{
    return accessToken.isEmpty()
        ? QByteArray()
        : QCryptographicHash::hash(accessToken.toUtf8(), QCryptographicHash::Sha256);
}

// Old tokenOwnerForRunningSession(); `candidates` is what it built from
// m_userId, the resolved canonical id and m_settings->userId().
QString oracleTokenOwner(const TokenStore *m_settings, const QString &m_userId,
                         const QString &m_deviceId, const QStringList &candidates)
{
    if (!m_settings || m_userId.isEmpty() || m_deviceId.isEmpty())
        return {};
    for (const QString &candidate : std::as_const(candidates)) {
        if (candidate.isEmpty())
            continue;
        const QVariantMap record = m_settings->accountRecord(candidate);
        if (!record.isEmpty()
            && record.value(QStringLiteral("deviceId")).toString() == m_deviceId)
            return candidate;
    }
    return {};
}

enum class OldLive { RotatedPast, Written, Failed, NoOwner };
// Old session_tokens_refreshed branch.
OldLive oracleLive(TokenStore *m_settings, const QString &m_userId, const QString &m_deviceId,
                   const QStringList &candidates, bool m_rustHandle, const QString &finalJson,
                   const QJsonObject &event, QByteArray *m_persistedAccessDigest)
{
    const QString tokenOwner = oracleTokenOwner(m_settings, m_userId, m_deviceId, candidates);
    const QString access = event.value(QStringLiteral("access_token")).toString();
    if (m_rustHandle) {
        const QString current = finalJson;
        const QString currentAccess = current.isEmpty()
                || current.startsWith(QLatin1String("error: "))
            ? QString()
            : QJsonDocument::fromJson(current.toUtf8()).object()
                  .value(QStringLiteral("access_token")).toString();
        if (!currentAccess.isEmpty()
            && oracleAccessTokenDigest(currentAccess) != oracleAccessTokenDigest(access)) {
            return OldLive::RotatedPast;
        }
    }
    if (m_settings && !tokenOwner.isEmpty()) {
        if (m_settings->updateSessionTokens(
                tokenOwner, access,
                event.value(QStringLiteral("refresh_token")).toString())) {
            *m_persistedAccessDigest = oracleAccessTokenDigest(access);
            return OldLive::Written;
        }
        return OldLive::Failed;
    }
    return OldLive::NoOwner;
}

enum class OldRetry { NoWrite, Written, Failed };
// Old retryRotatedTokenWrite() after its unchanged guards (handle, settings,
// signing out, owner non-empty).
OldRetry oracleRetry(TokenStore *m_settings, const QString &owner, const QString &json,
                     QByteArray *m_persistedAccessDigest)
{
    const QJsonObject tokens = json.isEmpty() || json.startsWith(QLatin1String("error: "))
        ? QJsonObject()
        : QJsonDocument::fromJson(json.toUtf8()).object();
    const QString access = tokens.value(QStringLiteral("access_token")).toString();
    if (access.isEmpty() || oracleAccessTokenDigest(access) == *m_persistedAccessDigest)
        return OldRetry::NoWrite;
    if (m_settings->updateSessionTokens(
            owner, access, tokens.value(QStringLiteral("refresh_token")).toString())) {
        *m_persistedAccessDigest = oracleAccessTokenDigest(access);
        return OldRetry::Written;
    }
    return OldRetry::Failed;
}

// Old retirement worker: whether it queued a handback, and with which pair.
bool oracleWorker(const QString &json, const QByteArray &accessDigest, QString *outAccess,
                  QString *outRefresh)
{
    const QJsonObject tokens = json.isEmpty() || json.startsWith(QLatin1String("error: "))
        ? QJsonObject()
        : QJsonDocument::fromJson(json.toUtf8()).object();
    const QString access = tokens.value(QStringLiteral("access_token")).toString();
    if (!access.isEmpty() && oracleAccessTokenDigest(access) != accessDigest) {
        *outAccess = access;
        *outRefresh = tokens.value(QStringLiteral("refresh_token")).toString();
        return true;
    }
    return false;
}

struct OldSignIn {
    QString userId;
    QString deviceId;
    QByteArray expectedDigest;
    QString accessToken;
    QString refreshToken;
};
enum class OldHandback { NoStore, SameDevice, RecordGone, KeyringChanged, Written, Failed };
// Old persistRetiredSessionTokens() loop body for one sign-in.
OldHandback oracleHandback(TokenStore *m_settings, const OldSignIn &signIn,
                           const QString &restoringUserId, bool m_rustHandle, bool m_loggedIn,
                           const QString &m_deviceId,
                           const std::function<QString()> &tokenOwnerForRunningSession)
{
    if (!m_settings)
        return OldHandback::NoStore;
    const bool restoring = !restoringUserId.isEmpty()
        && signIn.userId == restoringUserId;
    if (!restoring && m_rustHandle && m_loggedIn
        && m_deviceId == signIn.deviceId
        && tokenOwnerForRunningSession() == signIn.userId) {
        return OldHandback::SameDevice;
    }
    const QVariantMap record = m_settings->accountRecord(signIn.userId);
    if (record.isEmpty()
        || record.value(QStringLiteral("deviceId")).toString() != signIn.deviceId) {
        return OldHandback::RecordGone;
    }
    const QString stored = m_settings->accessTokenFor(signIn.userId);
    if (oracleAccessTokenDigest(stored) != signIn.expectedDigest) {
        return OldHandback::KeyringChanged;
    }
    const bool ok = m_settings->updateSessionTokens(
        signIn.userId, signIn.accessToken, signIn.refreshToken);
    return ok ? OldHandback::Written : OldHandback::Failed;
}

// ===================== end of oracles ===================================

// The client's new tokenOwnerForRunningSession(): its unchanged guard, then
// the custody function.
QString newTokenOwner(const TokenStore *store, const QString &userId, const QString &deviceId,
                      const QStringList &candidates)
{
    if (!store || userId.isEmpty() || deviceId.isEmpty())
        return {};
    return custody::tokenOwnerFor(*store, candidates, deviceId);
}

struct Running {
    QString userId;
    QString deviceId;
    QStringList candidates;
};

QList<Running> runningSessions()
{
    QList<Running> out;
    for (const QString &user : {QString(), QStringLiteral("@a:hs")}) {
        for (const QString &device : {QString(), QStringLiteral("DEV"), QStringLiteral("OTHER")}) {
            for (const QString &canonical :
                 {QString(), QStringLiteral("@A:hs"), QStringLiteral("@a:hs")}) {
                for (const QString &settingsUser : {QString(), QStringLiteral("@s:hs")})
                    out << Running{user, device, {user, canonical, settingsUser}};
            }
        }
    }
    return out;
}

QByteArray rowName(int row) { return QByteArray("row ") + QByteArray::number(row); }

} // namespace

class SessionTokenCustodyParityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anAdoptedSessionWritesOnlyARotationTheKeyringLacks();
    void theDigestIsTheOldDigest();
    void parsingMatchesTheOldInlineParse();
    void theOwnerLookupReadsWhatItAlwaysRead();
    void theLiveWriteMatchesTheOldHandler();
    void theRetryMatchesTheOldRetry();
    void theWorkerHandsBackWhatItAlwaysHandedBack();
    void theHandbackWriteMatchesTheOldLoop();
};

void SessionTokenCustodyParityTest::theDigestIsTheOldDigest()
{
    for (const QString &token : {QString(), QStringLiteral(""), QStringLiteral("A1"),
                                 QStringLiteral("syt_ünïcødé_token")}) {
        QCOMPARE(custody::accessTokenDigest(token), oracleAccessTokenDigest(token));
    }
    QVERIFY(custody::accessTokenDigest(QString()).isEmpty());
    QCOMPARE(custody::accessTokenDigest(QStringLiteral("A1")).size(), 32);
}

void SessionTokenCustodyParityTest::parsingMatchesTheOldInlineParse()
{
    int rows = 0;
    for (const QString &json : kFinalJsons) {
        const QJsonObject tokens = json.isEmpty() || json.startsWith(QLatin1String("error: "))
            ? QJsonObject()
            : QJsonDocument::fromJson(json.toUtf8()).object();
        const SessionTokenPair pair = custody::parseFinalSessionTokens(json);
        QCOMPARE(pair.accessToken, tokens.value(QStringLiteral("access_token")).toString());
        QCOMPARE(pair.refreshToken, tokens.value(QStringLiteral("refresh_token")).toString());
        ++rows;
    }
    QCOMPARE(rows, kFinalJsons.size());
    // An error string is never parsed, even one that happens to hold JSON.
    QVERIFY(custody::parseFinalSessionTokens(
                QStringLiteral(R"(error: {"access_token":"A1"})")).accessToken.isEmpty());
}

void SessionTokenCustodyParityTest::theOwnerLookupReadsWhatItAlwaysRead()
{
    int rows = 0;
    int owned = 0;
    for (const FakeStore &scenario : storeScenarios()) {
        for (const Running &running : runningSessions()) {
            for (bool withStore : {true, false}) {
                FakeStore oldStore = scenario;
                FakeStore newStore = scenario;
                const QString oldOwner = oracleTokenOwner(withStore ? &oldStore : nullptr,
                                                          running.userId, running.deviceId,
                                                          running.candidates);
                const QString newOwner = newTokenOwner(withStore ? &newStore : nullptr,
                                                       running.userId, running.deviceId,
                                                       running.candidates);
                QVERIFY2(oldOwner == newOwner, rowName(rows).constData());
                QVERIFY2(oldStore.sameStateAs(newStore), rowName(rows).constData());
                if (!oldOwner.isEmpty())
                    ++owned;
                ++rows;
            }
        }
    }
    QCOMPARE(rows, 8 * 36 * 2);
    QVERIFY(owned > 0);

    // The custody function on its own: an empty device owns nothing and
    // reads nothing, even against a record that names no device.
    FakeStore odd = storeScenarios().at(3);
    QVERIFY(custody::tokenOwnerFor(odd, {QStringLiteral("@a:hs")}, QString()).isEmpty());
    QVERIFY(odd.log.isEmpty());
}

void SessionTokenCustodyParityTest::theLiveWriteMatchesTheOldHandler()
{
    const QList<QJsonObject> events{
        QJsonObject{{QStringLiteral("access_token"), QStringLiteral("A1")},
                    {QStringLiteral("refresh_token"), QStringLiteral("R1")}},
        QJsonObject{{QStringLiteral("access_token"), QStringLiteral("A2")},
                    {QStringLiteral("refresh_token"), QStringLiteral("R2")}},
        QJsonObject{{QStringLiteral("access_token"), QStringLiteral("A1")}},
        QJsonObject{},
    };
    int rows = 0;
    QSet<int> outcomes;
    for (const FakeStore &scenario : storeScenarios()) {
        for (const Running &running : runningSessions()) {
            for (bool withStore : {true, false}) {
                for (bool hasHandle : {true, false}) {
                    for (const QString &json : kFinalJsons) {
                        for (const QJsonObject &event : events) {
                            FakeStore oldStore = scenario;
                            FakeStore newStore = scenario;
                            QByteArray oldDigest("unchanged");
                            const OldLive old = oracleLive(
                                withStore ? &oldStore : nullptr, running.userId,
                                running.deviceId, running.candidates, hasHandle, json, event,
                                &oldDigest);

                            // The client's new handler, in its order: owner,
                            // then the session's current pair, then writeLive.
                            QByteArray newDigest("unchanged");
                            const QString tokenOwner =
                                newTokenOwner(withStore ? &newStore : nullptr, running.userId,
                                              running.deviceId, running.candidates);
                            const SessionTokenPair rotated{
                                event.value(QStringLiteral("access_token")).toString(),
                                event.value(QStringLiteral("refresh_token")).toString()};
                            QString currentAccess;
                            if (hasHandle)
                                currentAccess = custody::parseFinalSessionTokens(json).accessToken;
                            const custody::LiveWrite now = custody::writeLive(
                                withStore ? &newStore : nullptr, tokenOwner, currentAccess,
                                rotated);
                            if (now == custody::LiveWrite::Written)
                                newDigest = custody::accessTokenDigest(rotated.accessToken);

                            const custody::LiveWrite expected =
                                old == OldLive::RotatedPast ? custody::LiveWrite::SkippedRotatedPast
                                : old == OldLive::Written   ? custody::LiveWrite::Written
                                : old == OldLive::Failed    ? custody::LiveWrite::WriteFailed
                                                            : custody::LiveWrite::SkippedNoOwner;
                            QVERIFY2(now == expected, rowName(rows).constData());
                            QVERIFY2(oldStore.sameStateAs(newStore), rowName(rows).constData());
                            QVERIFY2(oldDigest == newDigest, rowName(rows).constData());
                            outcomes.insert(int(now));
                            ++rows;
                        }
                    }
                }
            }
        }
    }
    QCOMPARE(rows, 8 * 36 * 2 * 2 * int(kFinalJsons.size()) * 4);
    QCOMPARE(outcomes.size(), 4);
}

void SessionTokenCustodyParityTest::theRetryMatchesTheOldRetry()
{
    const QList<QByteArray> persisted{QByteArray(), oracleAccessTokenDigest(QStringLiteral("A1")),
                                      oracleAccessTokenDigest(QStringLiteral("A2"))};
    int rows = 0;
    QSet<int> outcomes;
    for (const FakeStore &scenario : storeScenarios()) {
        for (const QString &json : kFinalJsons) {
            for (const QByteArray &digest : persisted) {
                FakeStore oldStore = scenario;
                FakeStore newStore = scenario;
                const QString owner = QStringLiteral("@a:hs");
                QByteArray oldDigest = digest;
                const OldRetry old = oracleRetry(&oldStore, owner, json, &oldDigest);

                // The client's new retry body.
                QByteArray newDigest = digest;
                OldRetry now = OldRetry::NoWrite;
                const SessionTokenPair tokens = custody::parseFinalSessionTokens(json);
                if (custody::retryWriteNeeded(tokens.accessToken, newDigest)) {
                    if (newStore.updateSessionTokens(owner, tokens.accessToken,
                                                     tokens.refreshToken)) {
                        newDigest = custody::accessTokenDigest(tokens.accessToken);
                        now = OldRetry::Written;
                    } else {
                        now = OldRetry::Failed;
                    }
                }
                QVERIFY2(now == old, rowName(rows).constData());
                QVERIFY2(oldStore.sameStateAs(newStore), rowName(rows).constData());
                QVERIFY2(oldDigest == newDigest, rowName(rows).constData());
                outcomes.insert(int(now));
                ++rows;
            }
        }
    }
    QCOMPARE(rows, 8 * int(kFinalJsons.size()) * 3);
    QCOMPARE(outcomes.size(), 3);
}

void SessionTokenCustodyParityTest::theWorkerHandsBackWhatItAlwaysHandedBack()
{
    const QList<QByteArray> lastWritten{QByteArray(),
                                        oracleAccessTokenDigest(QStringLiteral("A1")),
                                        oracleAccessTokenDigest(QStringLiteral("A2"))};
    int rows = 0;
    int handedBack = 0;
    for (const QString &json : kFinalJsons) {
        for (const QByteArray &digest : lastWritten) {
            QString oldAccess, oldRefresh;
            const bool old = oracleWorker(json, digest, &oldAccess, &oldRefresh);
            const SessionTokenPair tokens = custody::parseFinalSessionTokens(json);
            const bool now = custody::needsHandback(tokens.accessToken, digest);
            QVERIFY2(now == old, rowName(rows).constData());
            if (now) {
                QCOMPARE(tokens.accessToken, oldAccess);
                QCOMPARE(tokens.refreshToken, oldRefresh);
                ++handedBack;
            }
            ++rows;
        }
    }
    QCOMPARE(rows, int(kFinalJsons.size()) * 3);
    QVERIFY(handedBack > 0 && handedBack < rows);
}

void SessionTokenCustodyParityTest::theHandbackWriteMatchesTheOldLoop()
{
    const QList<QByteArray> expectedDigests{QByteArray(),
                                            oracleAccessTokenDigest(QStringLiteral("A1")),
                                            oracleAccessTokenDigest(QStringLiteral("A2"))};
    int rows = 0;
    QSet<int> outcomes;
    for (const FakeStore &scenario : storeScenarios()) {
        for (bool withStore : {true, false}) {
            for (const QString &user :
                 {QStringLiteral("@a:hs"), QStringLiteral("@A:hs"), QStringLiteral("@s:hs"),
                  QStringLiteral("@gone:hs")}) {
                for (const QString &device : {QStringLiteral("DEV"), QStringLiteral("OTHER")}) {
                    for (const QByteArray &expected : expectedDigests) {
                        for (const QString &restoring :
                             {QString(), user, QStringLiteral("@x:hs")}) {
                            for (bool hasHandle : {true, false}) {
                                for (bool loggedIn : {true, false}) {
                                    for (const QString &runningDevice :
                                         {QStringLiteral("DEV"), QStringLiteral("OTHER")}) {
                                        const Running running{
                                            QStringLiteral("@a:hs"), runningDevice,
                                            {QStringLiteral("@a:hs"), QStringLiteral("@A:hs"),
                                             QStringLiteral("@s:hs")}};
                                        FakeStore oldStore = scenario;
                                        FakeStore newStore = scenario;
                                        TokenStore *oldPtr = withStore ? &oldStore : nullptr;
                                        TokenStore *newPtr = withStore ? &newStore : nullptr;
                                        const OldSignIn signIn{user, device, expected,
                                                               QStringLiteral("A9"),
                                                               QStringLiteral("R9")};
                                        const OldHandback old = oracleHandback(
                                            oldPtr, signIn, restoring, hasHandle, loggedIn,
                                            runningDevice, [&] {
                                                return oracleTokenOwner(oldPtr, running.userId,
                                                                        running.deviceId,
                                                                        running.candidates);
                                            });

                                        // The client's new loop body.
                                        const bool isRestoring = !restoring.isEmpty()
                                            && signIn.userId == restoring;
                                        const bool sameDeviceRunning = newPtr && !isRestoring
                                            && hasHandle && loggedIn
                                            && runningDevice == signIn.deviceId
                                            && newTokenOwner(newPtr, running.userId,
                                                             running.deviceId,
                                                             running.candidates)
                                                == signIn.userId;
                                        const custody::HandbackEntry entry{
                                            signIn.userId, signIn.deviceId,
                                            signIn.expectedDigest,
                                            SessionTokenPair{signIn.accessToken,
                                                             signIn.refreshToken}};
                                        const custody::Handback now = custody::writeHandback(
                                            newPtr, entry, sameDeviceRunning);

                                        const custody::Handback mapped =
                                            old == OldHandback::NoStore
                                                ? custody::Handback::SkippedNoStore
                                            : old == OldHandback::SameDevice
                                                ? custody::Handback::SkippedSameDeviceRunning
                                            : old == OldHandback::RecordGone
                                                ? custody::Handback::SkippedRecordGoneOrOtherDevice
                                            : old == OldHandback::KeyringChanged
                                                ? custody::Handback::SkippedKeyringChanged
                                            : old == OldHandback::Written
                                                ? custody::Handback::Written
                                                : custody::Handback::WriteFailed;
                                        QVERIFY2(now == mapped, rowName(rows).constData());
                                        QVERIFY2(oldStore.sameStateAs(newStore),
                                                 rowName(rows).constData());
                                        outcomes.insert(int(now));
                                        ++rows;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    QCOMPARE(rows, 8 * 2 * 4 * 2 * 3 * 3 * 2 * 2 * 2);
    QCOMPARE(outcomes.size(), 6);
}

// Review M1 (wave 3): the SDK rotated after the background host's last drain,
// so the handle holds a newer pair than the keyring. Adoption must write that
// pair exactly once and answer the waiting rotation; an unrotated session
// writes nothing and still answers Saved; no live session does nothing.
void SessionTokenCustodyParityTest::anAdoptedSessionWritesOnlyARotationTheKeyringLacks()
{
    const QByteArray persisted = custody::accessTokenDigest(QStringLiteral("access-1"));

    const auto rotated = custody::reconcileAdopted(QStringLiteral("access-2"), persisted, 3);
    QVERIFY(rotated.write);
    QCOMPARE(rotated.answerGeneration, quint64(3));
    QVERIFY(!rotated.answerSavedIfNoWrite);

    const auto same = custody::reconcileAdopted(QStringLiteral("access-1"), persisted, 2);
    QVERIFY(!same.write);
    QCOMPARE(same.answerGeneration, quint64(2));
    QVERIFY(same.answerSavedIfNoWrite);

    const auto none = custody::reconcileAdopted(QString(), persisted, 0);
    QVERIFY(!none.write);
    QCOMPARE(none.answerGeneration, quint64(0));
    QVERIFY(!none.answerSavedIfNoWrite);

    // A host that never persisted (empty digest) still gets its pair written.
    QVERIFY(custody::reconcileAdopted(QStringLiteral("access-9"), QByteArray(), 0).write);
}

QTEST_GUILESS_MAIN(SessionTokenCustodyParityTest)
#include "SessionTokenCustodyParityTest.moc"
