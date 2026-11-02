#include <QSignalSpy>
#include <QtTest/QtTest>

#include "crypto/CryptoBootstrapModel.h"

class CryptoBootstrapModelTest : public QObject
{
    Q_OBJECT

private:
    static void apply(CryptoBootstrapModel &m, const char *kind,
                      const char *state, quint64 count = 0)
    {
        m.applyEvent(QString::fromLatin1(kind), QString::fromLatin1(state),
                     count);
    }

private Q_SLOTS:
    void verifiedSessionFlowReachesReady()
    {
        CryptoBootstrapModel m;
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        QVERIFY(!m.active());

        apply(m, "verification_state", "unverified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Unverified);
        QVERIFY(!m.active());
        QVERIFY(!m.statusMessage().isEmpty());

        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        QVERIFY(m.active());
        QVERIFY(m.statusMessage().contains(QStringLiteral("verified session")));

        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        apply(m, "backup_state", "enabling");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);

        apply(m, "room_keys_received", "", 12);
        apply(m, "room_keys_received", "", 30);
        QCOMPARE(m.keysReceived(), 42);

        apply(m, "backup_state", "enabled");
        apply(m, "recovery_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
        QVERIFY(m.active());
        QVERIFY(m.statusMessage().contains(QStringLiteral("42")));
    }

    void duplicateEventsAreIdempotent()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        apply(m, "verification_state", "verified");
        apply(m, "recovery_state", "unknown");
        apply(m, "backup_state", "unknown");
        QCOMPARE(changed.count(), 0);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void verifiedWithoutBackupFallsBackToRecoveryKey()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "recovery_state", "disabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::NoBackupAvailable);
        QVERIFY(m.statusMessage().contains(QStringLiteral("recovery key")));

        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    void alreadyEnabledBackupIsReady()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
        QCOMPARE(m.keysReceived(), 0);
        QVERIFY(!m.statusMessage().isEmpty());
    }

    void serverTruthNoBackupIsTerminal()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        apply(m, "backup_exists", "false");
        QCOMPARE(m.phase(), CryptoBootstrapModel::NoBackupAvailable);
        QVERIFY(m.statusMessage().contains(QStringLiteral("no encryption key backup")));

        CryptoBootstrapModel n;
        apply(n, "verification_state", "verified");
        apply(n, "backup_exists", "true");
        QCOMPARE(n.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void downloadPassDrivesRestoreAndEscalation()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);

        apply(m, "backup_download", "started");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_download", "failed");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "backup_download", "started");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_download", "ok");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    void aSkippedPassDoesNotRetireTheEscalation()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "enabled");
        apply(m, "backup_download", "failed");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "backup_download_skipped", "skipped_already_attempted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "backup_download_skipped", "skipped_no_backup_key");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        apply(m, "auto_key_recovery", "started");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "auto_key_recovery", "no_keys_found");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        apply(m, "backup_download", "skipped_already_attempted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "backup_download", "ok");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    void unansweredRequestEscalatesToManualRecovery()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(20);
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        QVERIFY(!m.needsRecoveryKey());

        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        QVERIFY(changed.wait(2000));
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());
        QVERIFY(m.active());
        QVERIFY(m.statusMessage().contains(QStringLiteral("recovery key")));

        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    void progressBeforeTimeoutCancelsEscalation()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(20);
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        QTest::qWait(60);
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    void resetIsolatesSessions()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "downloading");
        apply(m, "room_keys_received", "", 7);
        apply(m, "backup_exists", "false");
        apply(m, "backup_download", "failed");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        QCOMPARE(m.keysReceived(), 7);

        m.reset();
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        QCOMPARE(m.keysReceived(), 0);
        QVERIFY(!m.active());
        QCOMPARE(m.statusMessage(), QString());

        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        m.reset();

        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
    }

    void unknownInputsAreIgnored()
    {
        CryptoBootstrapModel m;
        apply(m, "surprise_kind", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "some_future_state");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void secretsPendingNamesIntermediateWaitState()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        const QString waitingCopy = m.statusMessage();

        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);
        QVERIFY(m.active());
        QVERIFY(!m.needsRecoveryKey());
        QVERIFY(!m.statusMessage().isEmpty());
        QVERIFY(m.statusMessage() != waitingCopy);

        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(changed.count(), 0);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);
    }

    void secretsPendingStillEscalatesOnTheSharedTimer()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(30);
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        QVERIFY(changed.wait(2000));
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    void secretsPendingProgressPromotesAndClears()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(30);
        apply(m, "verification_state", "verified");
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        QTest::qWait(60);
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);

        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);

        apply(m, "backup_state", "unknown");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void secretsPendingResetAndUnverifiedIsolation()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        m.reset();
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);

        CryptoBootstrapModel n;
        apply(n, "verification_state", "unverified");
        apply(n, "secrets_pending", "waiting");
        QCOMPARE(n.phase(), CryptoBootstrapModel::Unverified);
        apply(n, "verification_state", "verified");
        QCOMPARE(n.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void secretRequestAttemptsRefineWaitingState()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        QCOMPARE(m.requestAttempts(), 0);

        apply(m, "own_identity", "verified");
        apply(m, "cross_signing_secrets", "incomplete");
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        QCOMPARE(m.requestAttempts(), 1);
        QCOMPARE(m.eligibleDevices(), 1);
        QCOMPARE(m.requestState(), QStringLiteral("requested"));
        QVERIFY(m.canRequestKeys());
        QVERIFY(m.statusMessage().contains(QStringLiteral("request sent")));

        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.requestAttempts(), 2);
    }

    void exhaustedLadderEscalatesExplicitly()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secret_request", "requested", 1);
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());
        QVERIFY(m.canRequestKeys());
    }

    void secretResponseNamesProcessingState()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secret_request", "requested", 1);
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);
        QVERIFY(m.active());
        QVERIFY(!m.needsRecoveryKey());

        apply(m, "backup_state", "enabling");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_state", "unknown");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void unverifiedIdentityBlocksRequests()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "own_identity", "unverified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::IdentityIncomplete);
        QVERIFY(!m.canRequestKeys());
        QVERIFY(m.needsRecoveryKey());
        QVERIFY(m.statusMessage().contains(QStringLiteral("Verify")));

        apply(m, "own_identity", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void manualRearmLeavesManualRecoveryOnce()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        m.rearmAfterManualRequest();
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);

        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);

        apply(m, "secrets_pending", "exhausted");
        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
    }

    void secretReceivedCannotWedgeTheModel()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);
        QVERIFY(m.canRequestKeys());

        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);
        apply(m, "secret_request", "already_pending", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    void secretReceivedStillEscalatesOnTheBackstop()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(30);
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);

        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        QVERIFY(changed.wait(2000));
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());
    }

    void resetClearsCoordinatorDiagnostics()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "own_identity", "verified");
        apply(m, "cross_signing_secrets", "incomplete");
        apply(m, "secret_request", "requested", 2);
        apply(m, "secret_response", "received", 1);
        QVERIFY(m.requestAttempts() > 0);

        m.reset();
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        QCOMPARE(m.requestAttempts(), 0);
        QCOMPARE(m.eligibleDevices(), 0);
        QVERIFY(m.requestState().isEmpty());
        QVERIFY(m.ownIdentity().isEmpty());
        QVERIFY(m.crossSigningSecrets().isEmpty());
    }

    void keyArrivalAndRedecryptionAreLoggedAndChangeNoPhase()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "unverified");
        const auto phase = m.phase();
        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);

        QTest::ignoreMessage(QtInfoMsg,
                             QRegularExpression(QStringLiteral(
                                 "room keys received count= 3")));
        apply(m, "room_keys_received", "", 3);
        QTest::ignoreMessage(QtInfoMsg,
                             QRegularExpression(QStringLiteral(
                                 "undecryptable events resolved count= 2")));
        apply(m, "utds_resolved", "", 2);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 "redecryption lagged")));
        apply(m, "redecryption_lagging", "");

        QCOMPARE(m.phase(), phase);
        QCOMPARE(changed.count(), 1);
    }
};

QTEST_MAIN(CryptoBootstrapModelTest)
#include "CryptoBootstrapModelTest.moc"
