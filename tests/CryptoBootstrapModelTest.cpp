// Verified-session bootstrap state machine. The SDK owns secret gossip and
// backup download; this model only names the phase from sanitized state
// events. Pins the derivation, idempotency, key-count accumulation, the
// manual-recovery fallback and the per-session reset account switching needs.
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
    // The happy Element-like path: verify → SDK requests secrets → backup
    // key arrives → history downloads → ready. Manual recovery-key entry is
    // never required.
    void verifiedSessionFlowReachesReady()
    {
        CryptoBootstrapModel m;
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        QVERIFY(!m.active());

        apply(m, "verification_state", "unverified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Unverified);
        QVERIFY(!m.active());
        QVERIFY(!m.statusMessage().isEmpty());

        // SAS completes; the SDK's automatic secret requests go out.
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        QVERIFY(m.active());
        QVERIFY(m.statusMessage().contains(QStringLiteral("verified session")));

        // The other session answers; recovery becomes usable and the backup
        // starts downloading every key (OneShot).
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

    // A duplicate completion/state callback must not bounce the phase or
    // re-notify.
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

    // Verified but secret storage does not exist: nothing can be gossiped,
    // so the model surfaces the honest manual-recovery fallback.
    void verifiedWithoutBackupFallsBackToRecoveryKey()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "recovery_state", "disabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::NoBackupAvailable);
        QVERIFY(m.statusMessage().contains(QStringLiteral("recovery key")));

        // A backup appearing later (other session enabled it) still
        // promotes to the restoring/ready path.
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    // Ready without any observed key download (backup was already enabled
    // when the session started) is still an honest ready state.
    void alreadyEnabledBackupIsReady()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
        QCOMPARE(m.keysReceived(), 0);
        QVERIFY(!m.statusMessage().isEmpty());
    }

    // The homeserver says no backup exists: the model reaches the honest
    // terminal state immediately, without waiting for a gossip answer that
    // cannot restore anything, and stops promising a recovery key helps.
    void serverTruthNoBackupIsTerminal()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        apply(m, "backup_exists", "false");
        QCOMPARE(m.phase(), CryptoBootstrapModel::NoBackupAvailable);
        QVERIFY(m.statusMessage().contains(QStringLiteral("no encryption key backup")));

        // backup_exists=true keeps the ordinary waiting path.
        CryptoBootstrapModel n;
        apply(n, "verification_state", "verified");
        apply(n, "backup_exists", "true");
        QCOMPARE(n.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // The explicit download pass drives the phase: a started pass shows
    // restoring even when the backup is already enabled (a stored key), a
    // failed pass escalates instead of claiming Ready, and a later successful
    // pass still promotes.
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

        // Manual recovery forces a fresh pass; success promotes to Ready.
        apply(m, "backup_download", "started");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        apply(m, "backup_download", "ok");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    // A room that ran no pass must not erase a failed pass's escalation.
    // Skips are reported under their own kind; under `backup_download` they
    // would reach recompute(), which reads anything but "failed" as Ready,
    // so every later room switch would report Ready over unrestored history.
    void aSkippedPassDoesNotRetireTheEscalation()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "enabled");
        apply(m, "backup_download", "failed");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        // The room switch.
        apply(m, "backup_download_skipped", "skipped_already_attempted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        apply(m, "backup_download_skipped", "skipped_no_backup_key");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        // The automatic recovery pass concerns individual sessions, not the
        // room's history, so it does not retire it either.
        apply(m, "auto_key_recovery", "started");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "auto_key_recovery", "no_keys_found");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        // A skip misrouted under `backup_download` (which does not return
        // before recompute()) must not report Ready either.
        apply(m, "backup_download", "skipped_already_attempted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        // A real pass still speaks; the escalation is not permanent.
        apply(m, "backup_download", "ok");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);
    }

    // An unanswered automatic request escalates from the waiting spinner to
    // manual recovery after a bounded wait.
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

        // A no-progress event must not bounce back to the waiting spinner.
        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        // But real backup progress (the other device finally answered) still
        // promotes it out of manual recovery.
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    // Real progress before the bound cancels the escalation entirely.
    void progressBeforeTimeoutCancelsEscalation()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(20);
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        // The stale wait timer must not escalate now.
        QTest::qWait(60);
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    // Account switch or logout resets every remembered state and count.
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

        // Supervisor inputs are per-account too: no inherited "no backup" or
        // failed download pass.
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        m.reset();

        // Old-session events after reset (already rejected upstream by the
        // handle generation) could only re-derive from scratch.
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle); // not verified
    }

    // Unknown kinds/states are ignored safely.
    void unknownInputsAreIgnored()
    {
        CryptoBootstrapModel m;
        apply(m, "surprise_kind", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Idle);
        apply(m, "verification_state", "verified");
        apply(m, "backup_state", "some_future_state");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // The supervisor's bounded secrets_pending report refines the waiting
    // phase into "your other device has not answered": still waiting, with
    // distinct copy from WaitingForKeys.
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

        // Duplicate watchdog reports are idempotent.
        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(changed.count(), 0);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);
    }

    // The WaitingForKeys -> SecretsPending refinement does not restart or stop
    // the escalation timer, and ManualRecoveryRequired stays sticky against
    // late watchdog reports and no-progress events.
    void secretsPendingStillEscalatesOnTheSharedTimer()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(30);
        apply(m, "verification_state", "verified");   // timer armed here
        apply(m, "backup_exists", "true");
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        QSignalSpy changed(&m, &CryptoBootstrapModel::changed);
        QVERIFY(changed.wait(2000));
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        // Sticky: a late watchdog report cannot return to waiting...
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        // ...nor can a no-progress recovery event.
        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        // Real backup progress still promotes it out.
        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    // Real progress consumes the watchdog report: promotion to restoring or
    // ready, and a later wait starts from plain WaitingForKeys.
    void secretsPendingProgressPromotesAndClears()
    {
        CryptoBootstrapModel m;
        m.setWaitTimeoutMsForTest(30);
        apply(m, "verification_state", "verified");
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);

        apply(m, "backup_state", "downloading");
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
        // The stale wait timer must not escalate now.
        QTest::qWait(60);
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);

        apply(m, "backup_state", "enabled");
        QCOMPARE(m.phase(), CryptoBootstrapModel::Ready);

        // A backup-state regression re-enters plain waiting, not the consumed
        // refinement.
        apply(m, "backup_state", "unknown");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // Reset drops the watchdog flag, and a report while unverified cannot
    // mark the next verified wait.
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

        // While unverified the report is consumed without effect.
        CryptoBootstrapModel n;
        apply(n, "verification_state", "unverified");
        apply(n, "secrets_pending", "waiting");
        QCOMPARE(n.phase(), CryptoBootstrapModel::Unverified);
        apply(n, "verification_state", "verified");
        QCOMPARE(n.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // Each "requested" report counts an attempt, carries the eligible-device
    // count, refines the waiting message and offers the re-request action.
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

        // The coordinator's unanswered report refines, a second attempt
        // accumulates.
        apply(m, "secrets_pending", "waiting");
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretsPending);
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.requestAttempts(), 2);
    }

    // The coordinator's ladder-exhaustion report escalates to manual recovery
    // without waiting for the local backstop timer.
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
        // Manual state still offers the genuine re-request action.
        QVERIFY(m.canRequestKeys());
    }

    // A received m.secret.send answer names the processing state; backup
    // progress promotes out of it and consumes the flag.
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
        // Flag consumed: regressing to an idle backup returns to plain
        // waiting, not the stale received state.
        apply(m, "backup_state", "unknown");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // An unverified own identity blocks requesting (answers could not be
    // accepted) and names the remedy.
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

        // A later verified identity restores the normal waiting family.
        apply(m, "own_identity", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // A manual re-request leaves the sticky manual state once, and so does a
    // new coordinator request round.
    void manualRearmLeavesManualRecoveryOnce()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);

        m.rearmAfterManualRequest();
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);

        // Escalate again; the coordinator's fresh request round re-arms
        // waiting.
        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);

        // But a plain no-progress event stays sticky.
        apply(m, "secrets_pending", "exhausted");
        apply(m, "recovery_state", "incomplete");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
    }

    // A received answer that never turns into backup progress must not wedge
    // the model: exhaustion outranks it, a still-missing attempt clears it,
    // and the backstop timer keeps running.
    void secretReceivedCannotWedgeTheModel()
    {
        CryptoBootstrapModel m;
        apply(m, "verification_state", "verified");
        apply(m, "backup_exists", "true");
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);
        // The genuine re-request action stays available here.
        QVERIFY(m.canRequestKeys());

        // Exhaustion escalates even though an answer was seen.
        apply(m, "secrets_pending", "exhausted");
        QCOMPARE(m.phase(), CryptoBootstrapModel::ManualRecoveryRequired);
        QVERIFY(m.needsRecoveryKey());

        // A later attempt that still finds secrets missing clears the stale
        // received flag.
        apply(m, "secret_request", "requested", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        apply(m, "secret_response", "received", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::SecretReceived);
        apply(m, "secret_request", "already_pending", 1);
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
    }

    // The local backstop escalates a SecretReceived state the coordinator
    // never resolves.
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

    // Reset drops every coordinator diagnostic.
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
};

QTEST_MAIN(CryptoBootstrapModelTest)
#include "CryptoBootstrapModelTest.moc"
