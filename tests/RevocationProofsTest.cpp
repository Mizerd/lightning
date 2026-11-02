#include "accounts/RevocationProofs.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QtTest>

#include <optional>

using namespace lightning::accounts;

namespace {

struct OracleResult {
    bool keyringRead = false;
    bool keyringCard = false;
    bool hardLogout = false;
    bool softLogout = false;
    bool softLogoutReported = false;
    bool oauthWithoutRefreshToken = false;
    bool reasonExpired = false;
};

OracleResult oracleSessionTokenRevoked(const QJsonObject &event, bool oauthSession,
                                       bool keyringAnswer)
{
    OracleResult r;
    const QJsonValue softValue = event.value(QStringLiteral("soft_logout"));
    const bool softLogoutReported = softValue.isBool() && softValue.toBool();
    const QJsonValue hadRefresh = event.value(QStringLiteral("had_refresh_token"));
    bool keyringHoldsOrCannotTell = false;
    if (oauthSession && hadRefresh.isBool() && !hadRefresh.toBool()) {
        r.keyringRead = true;
        keyringHoldsOrCannotTell = keyringAnswer;
    }
    r.softLogoutReported = softLogoutReported;
    if (keyringHoldsOrCannotTell) {
        r.keyringCard = true;
        return r;
    }
    const bool oauthWithoutRefreshToken =
        oauthSession && hadRefresh.isBool() && !hadRefresh.toBool();
    const bool hardLogout = (softValue.isBool() && !softValue.toBool())
        || (softLogoutReported && oauthSession) || oauthWithoutRefreshToken;
    const bool softLogout = softLogoutReported && !oauthSession;
    r.oauthWithoutRefreshToken = oauthWithoutRefreshToken;
    r.hardLogout = hardLogout;
    r.softLogout = softLogout;
    r.reasonExpired = softLogout;
    return r;
}

RevocationEventFacts factsFrom(const QJsonObject &event, bool oauthSession)
{
    RevocationEventFacts facts;
    const QJsonValue softValue = event.value(QStringLiteral("soft_logout"));
    const QJsonValue hadRefresh = event.value(QStringLiteral("had_refresh_token"));
    if (softValue.isBool())
        facts.softLogout = softValue.toBool();
    if (hadRefresh.isBool())
        facts.hadRefreshToken = hadRefresh.toBool();
    facts.oauthSession = oauthSession;
    return facts;
}

enum class Field { Absent, True, False, NotBool };
const QList<Field> kFields{Field::Absent, Field::True, Field::False, Field::NotBool};

void put(QJsonObject &event, const QString &key, Field f)
{
    switch (f) {
    case Field::Absent: break;
    case Field::True: event.insert(key, true); break;
    case Field::False: event.insert(key, false); break;
    case Field::NotBool: event.insert(key, QStringLiteral("false")); break;
    }
}

}

class RevocationProofsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void classificationMatchesTheOldHandlerOnEveryRow();
    void aKeyringAnswerNobodyAskedForChangesNothing();
    void twoRevokedAccountsBothKeepTheirProofs();
    void aHardProofDropsOnlyTheSameDevicesSoftProof();
    void aSoftProofIsRefusedOverTheSameDevicesHardProof();
    void clearsArePerAccount();
    void anEmptyAccountIdRecordsNothing();
};

void RevocationProofsTest::classificationMatchesTheOldHandlerOnEveryRow()
{
    int rows = 0;
    QSet<int> outcomesSeen;
    int probesSeen = 0;
    for (Field soft : kFields) {
        for (Field hadRefresh : kFields) {
            for (bool oauth : {false, true}) {
                for (bool keyringAnswer : {false, true}) {
                    QJsonObject event;
                    event.insert(QStringLiteral("type"), QStringLiteral("session_token_revoked"));
                    put(event, QStringLiteral("soft_logout"), soft);
                    put(event, QStringLiteral("had_refresh_token"), hadRefresh);
                    const OracleResult old = oracleSessionTokenRevoked(event, oauth, keyringAnswer);

                    const RevocationEventFacts facts = factsFrom(event, oauth);
                    const bool probe = revocationNeedsKeyringProbe(facts);
                    const bool keyring = probe ? keyringAnswer : false;
                    const RevocationClassification now = classifyRevocation(facts, keyring);

                    const QByteArray row = QStringLiteral("soft=%1 had_refresh=%2 oauth=%3 keyring=%4")
                                               .arg(int(soft)).arg(int(hadRefresh))
                                               .arg(oauth).arg(keyringAnswer).toUtf8();
                    QVERIFY2(probe == old.keyringRead, row.constData());
                    QVERIFY2(now.softLogoutReported == old.softLogoutReported, row.constData());
                    if (old.keyringCard) {
                        QVERIFY2(now.outcome == RevocationOutcome::KeyringHoldsOrCannotTell,
                                 row.constData());
                    } else {
                        QVERIFY2(now.oauthWithoutRefreshToken == old.oauthWithoutRefreshToken,
                                 row.constData());
                        QVERIFY2(!(old.hardLogout && old.softLogout), row.constData());
                        const RevocationOutcome expected = old.hardLogout
                            ? RevocationOutcome::Hard
                            : old.softLogout ? RevocationOutcome::Soft
                                             : RevocationOutcome::Unproven;
                        QVERIFY2(now.outcome == expected, row.constData());
                        QVERIFY2((now.outcome == RevocationOutcome::Soft) == old.reasonExpired,
                                 row.constData());
                    }
                    outcomesSeen.insert(int(now.outcome));
                    if (probe)
                        ++probesSeen;
                    ++rows;
                }
            }
        }
    }
    QCOMPARE(rows, 4 * 4 * 2 * 2);
    QCOMPARE(outcomesSeen.size(), 4);
    QCOMPARE(probesSeen, 8);
}

void RevocationProofsTest::aKeyringAnswerNobodyAskedForChangesNothing()
{
    int rows = 0;
    for (Field soft : kFields) {
        for (Field hadRefresh : kFields) {
            for (bool oauth : {false, true}) {
                QJsonObject event;
                put(event, QStringLiteral("soft_logout"), soft);
                put(event, QStringLiteral("had_refresh_token"), hadRefresh);
                const RevocationEventFacts facts = factsFrom(event, oauth);
                if (revocationNeedsKeyringProbe(facts))
                    continue;
                const auto withFalse = classifyRevocation(facts, false);
                const auto withTrue = classifyRevocation(facts, true);
                QVERIFY(withTrue.outcome == withFalse.outcome);
                QVERIFY(withTrue.outcome != RevocationOutcome::KeyringHoldsOrCannotTell);
                ++rows;
            }
        }
    }
    QCOMPARE(rows, 4 * 4 * 2 - 4);
}

void RevocationProofsTest::twoRevokedAccountsBothKeepTheirProofs()
{
    RevocationProofs proofs;
    QCOMPARE(proofs.size(), 0);
    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DEVA"));
    proofs.recordHard(QStringLiteral("@b:hs"), QStringLiteral("DEVB"));
    QVERIFY(proofs.recordSoft(QStringLiteral("@c:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DEVC")));
    QCOMPARE(proofs.size(), 3);
    QCOMPARE(proofs.hardFor(QStringLiteral("@a:hs"))->deviceId, QStringLiteral("DEVA"));
    QCOMPARE(proofs.hardFor(QStringLiteral("@b:hs"))->deviceId, QStringLiteral("DEVB"));
    QCOMPARE(proofs.softFor(QStringLiteral("@c:hs"))->homeserver, QStringLiteral("https://hs"));
    QCOMPARE(proofs.softFor(QStringLiteral("@c:hs"))->deviceId, QStringLiteral("DEVC"));
    QVERIFY(!proofs.softFor(QStringLiteral("@a:hs")).has_value());
    QVERIFY(!proofs.hardFor(QStringLiteral("@c:hs")).has_value());

    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DEVA2"));
    QCOMPARE(proofs.hardFor(QStringLiteral("@a:hs"))->deviceId, QStringLiteral("DEVA2"));
    QCOMPARE(proofs.hardFor(QStringLiteral("@b:hs"))->deviceId, QStringLiteral("DEVB"));
    QCOMPARE(proofs.size(), 3);
}

void RevocationProofsTest::aHardProofDropsOnlyTheSameDevicesSoftProof()
{
    RevocationProofs proofs;
    QVERIFY(proofs.recordSoft(QStringLiteral("@a:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DEV1")));
    QVERIFY(proofs.recordSoft(QStringLiteral("@b:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DEV1")));
    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DEV2"));
    QVERIFY(proofs.softFor(QStringLiteral("@a:hs")).has_value());
    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DEV1"));
    QVERIFY(!proofs.softFor(QStringLiteral("@a:hs")).has_value());
    QCOMPARE(proofs.hardFor(QStringLiteral("@a:hs"))->deviceId, QStringLiteral("DEV1"));
    QVERIFY(proofs.softFor(QStringLiteral("@b:hs")).has_value());
    QCOMPARE(proofs.size(), 2);
}

void RevocationProofsTest::aSoftProofIsRefusedOverTheSameDevicesHardProof()
{
    RevocationProofs proofs;
    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DEV1"));
    QVERIFY(!proofs.recordSoft(QStringLiteral("@a:hs"), QStringLiteral("https://hs"),
                               QStringLiteral("DEV1")));
    QVERIFY(!proofs.softFor(QStringLiteral("@a:hs")).has_value());
    QVERIFY(proofs.recordSoft(QStringLiteral("@a:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DEV2")));
    QCOMPARE(proofs.softFor(QStringLiteral("@a:hs"))->deviceId, QStringLiteral("DEV2"));
    QVERIFY(proofs.recordSoft(QStringLiteral("@b:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DEV1")));
    QCOMPARE(proofs.size(), 2);
}

void RevocationProofsTest::clearsArePerAccount()
{
    RevocationProofs proofs;
    proofs.recordHard(QStringLiteral("@a:hs"), QStringLiteral("DA"));
    QVERIFY(proofs.recordSoft(QStringLiteral("@a:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DA2")));
    proofs.recordHard(QStringLiteral("@b:hs"), QStringLiteral("DB"));
    QVERIFY(proofs.recordSoft(QStringLiteral("@b:hs"), QStringLiteral("https://hs"),
                              QStringLiteral("DB2")));

    proofs.clearHard(QStringLiteral("@a:hs"));
    QVERIFY(!proofs.hardFor(QStringLiteral("@a:hs")).has_value());
    QVERIFY(proofs.softFor(QStringLiteral("@a:hs")).has_value());
    QVERIFY(proofs.hardFor(QStringLiteral("@b:hs")).has_value());
    QCOMPARE(proofs.size(), 2);

    proofs.clearSoft(QStringLiteral("@a:hs"));
    QVERIFY(!proofs.softFor(QStringLiteral("@a:hs")).has_value());
    QVERIFY(proofs.softFor(QStringLiteral("@b:hs")).has_value());
    QCOMPARE(proofs.size(), 1);

    proofs.clear(QStringLiteral("@b:hs"));
    QVERIFY(!proofs.hardFor(QStringLiteral("@b:hs")).has_value());
    QVERIFY(!proofs.softFor(QStringLiteral("@b:hs")).has_value());
    QCOMPARE(proofs.size(), 0);

    proofs.clear(QStringLiteral("@nobody:hs"));
    QCOMPARE(proofs.size(), 0);
}

void RevocationProofsTest::anEmptyAccountIdRecordsNothing()
{
    RevocationProofs proofs;
    proofs.recordHard(QString(), QStringLiteral("D"));
    QVERIFY(!proofs.recordSoft(QString(), QStringLiteral("https://hs"), QStringLiteral("D")));
    QCOMPARE(proofs.size(), 0);
}

QTEST_GUILESS_MAIN(RevocationProofsTest)
#include "RevocationProofsTest.moc"
