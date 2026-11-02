#include "accounts/RevocationProofs.h"
#include "accounts/SessionTokenCustody.h"

#include <QtTest>

using namespace lightning::accounts;
using custody::LiveWrite;
using custody::PersistAnswer;

class TokenRotationMappingTest : public QObject
{
    Q_OBJECT

private slots:
    void aWrittenPairIsTheOnlySavedAnswer()
    {
        QCOMPARE(custody::persistAnswerFor(LiveWrite::Written), PersistAnswer::Saved);
        QCOMPARE(custody::persistAnswerFor(LiveWrite::WriteFailed), PersistAnswer::NotSaved);
        QCOMPARE(custody::persistAnswerFor(LiveWrite::SkippedNoOwner), PersistAnswer::NotSaved);
        QCOMPARE(custody::persistAnswerFor(LiveWrite::SkippedRotatedPast), PersistAnswer::None);
    }

    void theCauseNamesWhichRefreshTokenWasRefused()
    {
        SignOutFacts f;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_unknown"));
        f.hadRefreshToken = false;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("no_refresh_token"));
        f.hadRefreshToken = true;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_unknown"));
        f.rotations = 0;
        f.unsavedRotation = false;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_restored_token"));
        f.rotations = 3;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_saved_token"));
        f.unsavedRotation = true;
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_after_unsaved_rotation"));
        f.rotations.reset();
        QCOMPARE(QByteArray(signOutCause(f)), QByteArray("refused_unknown"));
    }

    void anUnreadableKeyringIsNeverASignOut()
    {
        RevocationEventFacts f;
        f.oauthSession = true;
        f.hadRefreshToken = false;
        f.softLogout = false;
        QVERIFY(revocationNeedsKeyringProbe(f));
        QCOMPARE(classifyRevocation(f, true).outcome,
                 RevocationOutcome::KeyringHoldsOrCannotTell);
        QCOMPARE(classifyRevocation(f, false).outcome, RevocationOutcome::Hard);
        f.hadRefreshToken = true;
        QVERIFY(!revocationNeedsKeyringProbe(f));
        QCOMPARE(classifyRevocation(f, true).outcome, RevocationOutcome::Hard);
    }
};

QTEST_GUILESS_MAIN(TokenRotationMappingTest)
#include "TokenRotationMappingTest.moc"
