// How C++ maps the outcomes around an OAuth token rotation (fx7, the
// matrix.org "This session was signed out remotely" report):
//   - persistAnswerFor: what RustSdkMatrixClient tells Rust about a rotation
//     after its keyring write (mx_rust_session_tokens_persisted). A refused or
//     ownerless write is a transient keyring failure: it must release the
//     refresh (NotSaved) and must never claim the pair is in the keyring.
//   - signOutCause: the word the "signed out" log line carries, so the next
//     report tells a remote sign-out from a rotation Lightning failed to save.
//   - classifyRevocation on the inputs a transient failure can produce: none.
//     matrix-sdk never sends session_token_revoked for a network error, a 5xx
//     or a timeout (0.18 client/futures.rs), so the only OAuth event without a
//     refused refresh is "no refresh token", and an unreadable keyring there
//     keeps the non-destructive card.
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
        // The keyring refused (locked collection, dropped bus): answered, so
        // the SDK's refresh is not held for the wait's bound, and not Saved.
        QCOMPARE(custody::persistAnswerFor(LiveWrite::WriteFailed), PersistAnswer::NotSaved);
        QCOMPARE(custody::persistAnswerFor(LiveWrite::SkippedNoOwner), PersistAnswer::NotSaved);
        // Nothing waits on a rotation the session already moved past.
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
        // An older Rust event (no counts) never guesses.
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
        // The keyring holds a refresh token, or cannot answer: the session
        // never got it, which is ours to fix with "Try again", not a sign-out.
        QCOMPARE(classifyRevocation(f, true).outcome,
                 RevocationOutcome::KeyringHoldsOrCannotTell);
        // Conclusively none: the session cannot continue.
        QCOMPARE(classifyRevocation(f, false).outcome, RevocationOutcome::Hard);
        // A refused refresh (invalid_grant) never asks the keyring.
        f.hadRefreshToken = true;
        QVERIFY(!revocationNeedsKeyringProbe(f));
        QCOMPARE(classifyRevocation(f, true).outcome, RevocationOutcome::Hard);
    }
};

QTEST_GUILESS_MAIN(TokenRotationMappingTest)
#include "TokenRotationMappingTest.moc"
