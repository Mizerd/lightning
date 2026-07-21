// v0.7: verified-session bootstrap state machine. The SDK owns the actual
// secret gossip / backup download; this model only names the phase from
// sanitized state events — these tests pin the derivation, idempotency,
// key-count accumulation, the manual-recovery fallback signal, and the
// per-session reset that account switching relies on.
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

    // v0.7 supervisor: the homeserver-truth probe says no backup exists.
    // The model reaches the honest terminal state immediately — no
    // 30-second wait for a gossip answer that cannot restore anything —
    // and the copy stops promising that a recovery key will bring
    // history back.
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

    // v0.7 supervisor: the explicit download pass drives the phase. A
    // started pass shows restoring even when the backup state is already
    // steady at enabled (the F2 stored-key case); a failed pass escalates
    // honestly instead of claiming Ready; a later successful pass (e.g.
    // after manual recovery) still promotes.
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

    // The automatic request may never be answered. After the bounded wait,
    // the model escalates from the indefinite "waiting" spinner to an honest
    // manual-recovery state so the UI can offer the recovery key instead of
    // shimmering forever.
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
        // The stale wait timer must not fire us into manual recovery now.
        QTest::qWait(60);
        QCOMPARE(m.phase(), CryptoBootstrapModel::RestoringHistory);
    }

    // Account switch / logout: reset drops every remembered state and
    // count, so a stale phase can never describe the next account.
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

        // The v0.7 supervisor inputs are per-account too: the next account
        // must not inherit "no backup exists" or a failed download pass.
        apply(m, "verification_state", "verified");
        QCOMPARE(m.phase(), CryptoBootstrapModel::WaitingForKeys);
        m.reset();

        // Old-session events after the reset (already rejected upstream by
        // the handle generation) would at worst re-derive from scratch —
        // they can never resurrect the previous account's counters.
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
};

QTEST_MAIN(CryptoBootstrapModelTest)
#include "CryptoBootstrapModelTest.moc"
