// BackupController's cross-signing flow state machine: the password challenge
// that setup/reset may raise, the OAuth approval URL (https only), the honest
// "keys_unavailable" failure (an identity exists that this session cannot sign
// with; Lightning does not replace it on its own), a cancel that waits for the
// backend's own report, the explicit choice about an existing recovery key,
// and that the password and recovery key are neither kept nor shown after use.

#include <QtTest/QtTest>

#include <QSet>
#include <QSignalSpy>

#include "crypto/BackupController.h"
#include "matrix/MockMatrixClient.h"

namespace {

class FakeBackupClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    quint64 backupAction(const QString &action) override
    {
        actions << action;
        lastOp = ++counter;
        return lastOp;
    }
    quint64 crossSigningAction(const QString &action, const QString &recoveryKey,
                               bool replaceRecoveryKeyConfirmed) override
    {
        actions << action;
        crossSigningCalls << action;
        recoveryKeyLength = recoveryKey.size();
        replaceConfirmed = replaceRecoveryKeyConfirmed;
        lastOp = ++counter;
        return lastOp;
    }
    bool uiaSubmitPassword(quint64 uiaId, const QString &password) override
    {
        submittedFor = uiaId;
        submittedLength = password.size();
        return acceptSubmit;
    }
    void uiaCancel(quint64 uiaId) override { cancelledFor = uiaId; }
    void acknowledgeInterruptedSetup() override { ++acknowledged; }
    int acknowledged = 0;
    quint64 requestBackupProgress() override { return nextProgressId; }
    quint64 nextProgressId = 0;

    QStringList actions;
    QStringList crossSigningCalls;
    int recoveryKeyLength = -1;
    bool replaceConfirmed = false;
    quint64 counter = 500;
    quint64 lastOp = 0;
    quint64 submittedFor = 0;
    int submittedLength = -1;
    quint64 cancelledFor = 0;
    bool acceptSubmit = true;
};

} // namespace

class BackupControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void crossSigningActionsAreAccepted()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        QCOMPARE(client.actions, QStringList{QStringLiteral("setup_cross_signing")});
        // Through the cross-signing entry point, with no key and no consent:
        // the backend decides, never this side.
        QCOMPARE(client.crossSigningCalls,
                 QStringList{QStringLiteral("setup_cross_signing")});
        QCOMPARE(client.recoveryKeyLength, 0);
        QVERIFY(!client.replaceConfirmed);
        QVERIFY(c.busy());
        QVERIFY(c.error().isEmpty());
    }

    void passwordChallengeOpensForItsOwnOperationOnly()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.uiaRequired(client.lastOp + 99, true, false, {});
        QVERIFY2(!c.passwordRequired(), "a foreign challenge opened our prompt");
        Q_EMIT client.uiaRequired(client.lastOp, true, false,
                                  { QStringLiteral("m.login.password") });
        QVERIFY(c.passwordRequired());
        QVERIFY(!c.wrongPassword());
    }

    void submitClosesThePromptAndAWrongAnswerReopensIt()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.uiaRequired(client.lastOp, true, false, {});
        c.submitPassword(QStringLiteral("hunter2"));
        QCOMPARE(client.submittedFor, client.lastOp);
        QCOMPARE(client.submittedLength, 7);
        QVERIFY(!c.passwordRequired());
        QVERIFY(c.busy());
        Q_EMIT client.uiaRequired(client.lastOp, true, true, {});
        QVERIFY(c.passwordRequired());
        QVERIFY(c.wrongPassword());
    }

    void emptyPasswordIsNotSent()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.uiaRequired(client.lastOp, true, false, {});
        c.submitPassword(QString());
        QCOMPARE(client.submittedFor, quint64(0));
        QVERIFY(c.passwordRequired());
    }

    void unrenderableAuthStageFailsInsteadOfHanging()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.uiaRequired(op, false, false, { QStringLiteral("m.login.sso") });
        QCOMPARE(client.cancelledFor, op);
        QVERIFY(!c.passwordRequired());
        // Stopped, and waiting for the backend to say what stopping left.
        QVERIFY(c.busy());
        QVERIFY(c.cancelling());
        Q_EMIT client.backupActionFinished(op, QStringLiteral("setup_cross_signing"),
                                           false, QString(), QStringLiteral("cancelled"));
        QVERIFY(!c.busy());
        QCOMPARE(c.errorCategory(), QStringLiteral("unsupported_auth"));
        QVERIFY(c.error().contains(QStringLiteral("management page")));
    }

    // Cancel asks the backend to stop and stays busy until it reports: the
    // review found Cancel cleared the op at once and said "Nothing was
    // changed" while the job ran on.
    void cancelWaitsForTheBackendAndReportsWhatItSays()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.uiaRequired(op, true, false, {});
        QSignalSpy stale(&c, &BackupController::cryptoHealthStale);
        c.cancelAuth();
        QCOMPARE(client.cancelledFor, op);
        QVERIFY(!c.passwordRequired());
        QVERIFY(c.busy());
        QVERIFY(c.cancelling());
        QVERIFY(c.error().isEmpty());
        // The one-at-a-time guard still holds while the backend winds down.
        c.runAction(QStringLiteral("setup_cross_signing"));
        QCOMPARE(client.actions.size(), 1);
        Q_EMIT client.backupActionFinished(op, QStringLiteral("setup_cross_signing"),
                                           false, QString(), QStringLiteral("cancelled"));
        QVERIFY(!c.busy());
        QVERIFY(!c.cancelling());
        QCOMPARE(c.errorCategory(), QStringLiteral("cancelled"));
        QVERIFY(c.error().contains(QStringLiteral("did not receive any new keys")));
        QVERIFY(c.recoveryKey().isEmpty());
        QCOMPARE(stale.count(), 1);
    }

    // The review's MAJOR 2: the browser approval lands after Cancel was
    // pressed. The job completed on the server, so its recovery key must be
    // shown, not dropped as a stale op.
    void cancelDuringApprovalThenALateOkResultIsShown()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.crossSigningApprovalRequired(
            op, QStringLiteral("https://mas.example/approve"));
        QVERIFY(!c.approvalUrl().isEmpty());
        c.cancelAuth();
        QCOMPARE(client.cancelledFor, op);
        QVERIFY(c.approvalUrl().isEmpty());
        QVERIFY(c.busy());
        // A late approval event for the cancelled op does not reopen the page.
        Q_EMIT client.crossSigningApprovalRequired(
            op, QStringLiteral("https://mas.example/approve"));
        QVERIFY(c.approvalUrl().isEmpty());
        Q_EMIT client.backupActionFinished(op, QStringLiteral("setup_cross_signing"),
                                           true, QStringLiteral("EsTx late key"),
                                           QString());
        QVERIFY(!c.busy());
        QCOMPARE(c.recoveryKey(), QStringLiteral("EsTx late key"));
        QVERIFY(c.error().isEmpty());
        QVERIFY(!c.notice().isEmpty());
    }

    // A cancelled RESET already replaced this session's keys locally; the
    // text must say what actually happened, not "nothing was changed".
    void aCancelledResetSaysWhatHappened()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("reset_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.uiaRequired(op, true, false, {});
        c.cancelAuth();
        Q_EMIT client.backupActionFinished(op, QStringLiteral("reset_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("cancelled_reset"));
        QCOMPARE(c.errorCategory(), QStringLiteral("cancelled_reset"));
        QVERIFY(c.error().contains(QStringLiteral("keeps its existing cross-signing identity")));
        QVERIFY(c.error().contains(QStringLiteral("discarded")));
        QVERIFY(!c.error().contains(QStringLiteral("Nothing was changed")));
    }

    // A reset that failed after it started already replaced this session's
    // keys locally; whatever the backend reports, the text must say what
    // happened and never "Nothing was changed".
    void aFailedResetNeverSaysNothingWasChanged()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        const QStringList categories = {
            QStringLiteral("reset_failed"), QStringLiteral("reset_failed_unconfirmed"),
            QStringLiteral("reset_incomplete"), QStringLiteral("reset_keys_lost"),
        };
        for (const QString &category : categories) {
            c.runAction(QStringLiteral("reset_cross_signing"));
            Q_EMIT client.backupActionFinished(client.lastOp,
                                               QStringLiteral("reset_cross_signing"),
                                               false, QString(), category);
            QCOMPARE(c.errorCategory(), category);
            QVERIFY2(!c.error().contains(QStringLiteral("Nothing was changed")),
                     qPrintable(category));
            QVERIFY2(!c.error().contains(QStringLiteral("Some of it may already")),
                     qPrintable(category));
        }
        c.runAction(QStringLiteral("reset_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("reset_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("reset_failed"));
        QVERIFY(c.error().contains(QStringLiteral("kept your existing cross-signing identity")));

        // Lost keys say so and name the way out; a device that could not be
        // signed points at "Finish setting up", never at another reset.
        c.runAction(QStringLiteral("reset_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("reset_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("reset_keys_lost"));
        QVERIFY(c.error().contains(QStringLiteral("nobody holds them")));
        QVERIFY(c.error().contains(QStringLiteral("\"Reset\" again")));
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("device_signature_failed"));
        QVERIFY(c.error().contains(QStringLiteral("Finish setting up")));
        QVERIFY(!c.error().contains(QStringLiteral("Try again")));
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("backup_without_recovery"));
        QVERIFY(c.error().contains(QStringLiteral("Delete backup")));
        QVERIFY(c.error().contains(QStringLiteral("unreadable")));
    }

    // The approval page goes to the system browser: https only (http only on
    // loopback), checked before it is ever exposed to QML.
    void onlyASecureApprovalPageIsExposed()
    {
        QVERIFY(BackupController::isSafeApprovalUrl(
            QStringLiteral("https://mas.example/account/?action=org.matrix.cross_signing_reset")));
        QVERIFY(BackupController::isSafeApprovalUrl(QStringLiteral("http://127.0.0.1:8080/a")));
        QVERIFY(BackupController::isSafeApprovalUrl(QStringLiteral("http://localhost/a")));
        QVERIFY(BackupController::isSafeApprovalUrl(QStringLiteral("http://[::1]:9/a")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QStringLiteral("http://mas.example/a")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QStringLiteral("javascript:alert(1)")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QStringLiteral("https://user:pw@mas.example/")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QStringLiteral("http://127.evil.example/")));
        QVERIFY(!BackupController::isSafeApprovalUrl(QString()));

        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.crossSigningApprovalRequired(op, QStringLiteral("http://mas.example/approve"));
        QVERIFY(c.approvalUrl().isEmpty());
        QCOMPARE(client.cancelledFor, op);
        Q_EMIT client.backupActionFinished(op, QStringLiteral("setup_cross_signing"),
                                           false, QString(), QStringLiteral("cancelled"));
        QCOMPARE(c.errorCategory(), QStringLiteral("unsafe_approval_url"));
        QVERIFY(!c.busy());
    }

    // An existing recovery key is kept (the user enters it) or replaced only
    // with explicit consent; the choice travels to the backend as arguments.
    void anExistingRecoveryKeyNeedsAnExplicitChoice()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("recovery_key_required"));
        QCOMPARE(c.errorCategory(), QStringLiteral("recovery_key_required"));
        QVERIFY(!c.busy());

        c.runCrossSigning(QStringLiteral("setup_cross_signing"),
                          QStringLiteral("EsTx current key"), false);
        QCOMPARE(client.recoveryKeyLength, 16);
        QVERIFY(!client.replaceConfirmed);
        QVERIFY(c.errorCategory().isEmpty());
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("recovery_key_wrong"));
        QVERIFY(c.error().contains(QStringLiteral("does not unlock")));

        c.runCrossSigning(QStringLiteral("setup_cross_signing"), QString(), true);
        QCOMPARE(client.recoveryKeyLength, 0);
        QVERIFY(client.replaceConfirmed);
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           true, QStringLiteral("EsTx new"), QString());

        // Both at once is a contradiction and never reaches the backend.
        const int before = client.crossSigningCalls.size();
        c.dismissRecoveryKey();
        c.runCrossSigning(QStringLiteral("setup_cross_signing"),
                          QStringLiteral("EsTx current key"), true);
        QCOMPARE(client.crossSigningCalls.size(), before);
        // Only the two cross-signing actions take this path.
        c.runCrossSigning(QStringLiteral("reset_key"), QString(), true);
        QCOMPARE(client.crossSigningCalls.size(), before);
        QVERIFY(client.actions.count(QStringLiteral("reset_key")) == 0);
    }

    void oauthApprovalUrlIsSurfacedAndClearedOnResult()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("reset_cross_signing"));
        Q_EMIT client.crossSigningApprovalRequired(
            client.lastOp, QStringLiteral("https://mas.example/approve"));
        QCOMPARE(c.approvalUrl(), QStringLiteral("https://mas.example/approve"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("reset_cross_signing"),
                                           true, QStringLiteral("EsTx k"),
                                           QString());
        QVERIFY(c.approvalUrl().isEmpty());
        QCOMPARE(c.recoveryKey(), QStringLiteral("EsTx k"));
        QVERIFY(!c.busy());
        c.dismissRecoveryKey();
        QVERIFY(c.recoveryKey().isEmpty());
    }

    // An identity this session cannot sign with is never replaced silently:
    // the failure says so and names the way out, and carries a category QML can
    // branch on without parsing prose.
    void keysUnavailableIsExplainedNotRetriedAsAReset()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("setup_cross_signing"),
                                           false, QString(),
                                           QStringLiteral("keys_unavailable"));
        QCOMPARE(c.errorCategory(), QStringLiteral("keys_unavailable"));
        QVERIFY(c.error().contains(QStringLiteral("will not replace")));
        QVERIFY(c.error().contains(QStringLiteral("recovery key")));
        // Exactly one action ran: no automatic follow-up reset.
        QCOMPARE(client.actions.size(), 1);
        QVERIFY(!c.busy());
    }

    void aStaleChallengeAfterLogoutCannotAnswerANewAccount()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("setup_cross_signing"));
        const quint64 op = client.lastOp;
        Q_EMIT client.uiaRequired(op, true, false, {});
        QVERIFY(c.passwordRequired());
        Q_EMIT client.loggedOut();
        QVERIFY(!c.passwordRequired());
        c.submitPassword(QStringLiteral("late"));
        QCOMPARE(client.submittedFor, quint64(0));
    }

    void unknownActionsAreStillRefused()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("reset_everything"));
        QVERIFY(client.actions.isEmpty());
        QVERIFY(!c.error().isEmpty());
    }

    // ---- Sign-out key-loss warning (SDK facts only) -----------------------
    static QVariantList sessions(bool otherVerified)
    {
        return {
            QVariantMap{ { "isCurrent", true }, { "verified", true } },
            QVariantMap{ { "isCurrent", false }, { "verified", otherVerified } },
        };
    }

    // The backup_exists_on_server argument is CryptoHealthModel::TriState:
    // 0 unknown, 1 no, 2 yes. State strings are the ones Rust really emits
    // (backup_state_name): the SDK has no "disabled" backup state.
    void signOutRiskIsCheckingUntilTheSdkAnswers()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("enabled"), 2, {})
                     .value("level").toString(), QStringLiteral("checking"));
        Q_EMIT client.backupProgress(QStringLiteral("enabled"),
                                     QStringLiteral("done"), 0, 0);
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("enabled"), 2, {})
                     .value("level").toString(), QStringLiteral("ok"));
        // A new request invalidates the old answer: a stale "ok" must not
        // describe the next dialog opening.
        c.requestProgress();
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("enabled"), 2, {})
                     .value("level").toString(), QStringLiteral("checking"));
    }

    void aLateAnswerToAnEarlierRequestCannotAnswerTheCurrentOne()
    {
        FakeBackupClient client;
        client.nextProgressId = 7;
        BackupController c;
        c.setClient(&client);
        c.requestProgress(); // request 7
        // An answer to request 6 arrives first: ignored.
        Q_EMIT client.backupProgress(QStringLiteral("enabled"),
                                     QStringLiteral("done"), 0, 0, 6);
        QVERIFY(!c.progressAnswered());
        Q_EMIT client.backupProgress(QStringLiteral("enabled"),
                                     QStringLiteral("done"), 0, 0, 7);
        QVERIFY(c.progressAnswered());
    }

    void signOutRiskNamesEachWayKeysCanBeLost()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        const auto level = [&](const QString &backup, const QString &upload,
                               qint64 done, qint64 total, int onServer,
                               const QString &recovery) {
            Q_EMIT client.backupProgress(backup, upload, done, total);
            return c.signOutKeyRisk(recovery, onServer, {});
        };
        // No backup at all: the server holds none. Rust reports the local
        // state as "unknown" then, which must NOT hide it.
        QCOMPARE(level("unknown", "unknown", 0, 0, 1, "disabled").value("level"),
                 QVariant("no_backup"));
        QVERIFY(level("unknown", "unknown", 0, 0, 1, "enabled").value("warns").toBool());
        const auto up = level("enabled", "uploading", 3, 10, 2, "enabled");
        QCOMPARE(up.value("level"), QVariant("uploading"));
        QCOMPARE(up.value("pendingKeys").toLongLong(), qint64(7));
        // Review L6: right after setup, enable() returns before the upload has
        // started; the store's counts still show keys the backup lacks.
        const auto justEnabled = level("enabled", "idle", 0, 40, 2, "enabled");
        QCOMPARE(justEnabled.value("level"), QVariant("uploading"));
        QCOMPARE(justEnabled.value("pendingKeys").toLongLong(), qint64(40));
        QVERIFY(justEnabled.value("warns").toBool());
        QCOMPARE(level("enabled", "error", 5, 10, 2, "enabled").value("level"),
                 QVariant("uploading"));
        QCOMPARE(level("enabled", "done", 0, 0, 2, "disabled").value("level"),
                 QVariant("no_recovery"));
        // "Not known" is never reported as "no backup" nor as "fine": a state
        // we could not read, an upload the SDK did not report in time, an
        // upload that failed, a backup still being set up.
        QCOMPARE(level("unknown", "unknown", 0, 0, 0, "enabled").value("level"),
                 QVariant("unknown"));
        QCOMPARE(level("unknown", "unknown", 0, 0, 2, "enabled").value("level"),
                 QVariant("unknown"));
        QCOMPARE(level("enabled", "unknown", 0, 0, 2, "enabled").value("level"),
                 QVariant("unknown"));
        QCOMPARE(level("enabled", "error", 0, 0, 2, "enabled").value("level"),
                 QVariant("unknown"));
        QCOMPARE(level("enabled", "done", 0, 0, 2, "unknown").value("level"),
                 QVariant("unknown"));
        QCOMPARE(level("creating", "idle", 0, 0, 2, "enabled").value("level"),
                 QVariant("unknown"));
        // Backed up and recoverable: no warning. "incomplete" is a recovery
        // key held elsewhere, so signing out loses nothing more.
        QVERIFY(!level("enabled", "done", 0, 0, 2, "enabled").value("warns").toBool());
        QVERIFY(!level("enabled", "idle", 0, 0, 2, "incomplete").value("warns").toBool());
    }

    void signOutRiskCountsOnlyOtherTrustedSessions()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        Q_EMIT client.backupProgress(QStringLiteral("unknown"),
                                     QStringLiteral("unknown"), 0, 0);
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("disabled"), 1, sessions(false))
                     .value("otherVerifiedDevices").toInt(), 0);
        // The current session is "verified" by construction in the SDK and
        // must never count as a second copy of the keys.
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("disabled"), 1, sessions(true))
                     .value("otherVerifiedDevices").toInt(), 1);
    }

    // An enable aborted by teardown after secret storage existed never
    // reported; the next session's backend finds its marker and sends op 0.
    // Before, op 0 was dropped and the user was told nothing.
    void anInterruptedSetupFromAnEarlierSessionIsReported()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        QSignalSpy stale(&c, &BackupController::cryptoHealthStale);
        Q_EMIT client.backupActionFinished(0, QStringLiteral("enable"), false,
                                           QString(),
                                           QStringLiteral("setup_interrupted"));
        QCOMPARE(c.errorCategory(), QStringLiteral("setup_interrupted"));
        QVERIFY2(c.error().contains(QStringLiteral("interrupted")), qPrintable(c.error()));
        QVERIFY(c.recoveryKey().isEmpty());
        QVERIFY(!c.busy());
        QCOMPARE(stale.count(), 1);
        // Shown, so the backend may drop its marker (review N3).
        QCOMPARE(client.acknowledged, 1);

        // Op 0 stays no operation of ours for anything else, and never
        // replaces what an operation in flight will report.
        c.runAction(QStringLiteral("create_backup"));
        QVERIFY(c.busy());
        Q_EMIT client.backupActionFinished(0, QStringLiteral("enable"), false,
                                           QString(),
                                           QStringLiteral("setup_interrupted"));
        Q_EMIT client.backupActionFinished(0, QStringLiteral("enable"), true,
                                           QStringLiteral("EsTx not ours"), QString());
        QVERIFY(c.busy());
        QVERIFY(c.error().isEmpty());
        QVERIFY(c.recoveryKey().isEmpty());
        // Queued, not shown: not acknowledged yet.
        QCOMPARE(client.acknowledged, 1);
        // The operation reports; the queued report follows it, and only now
        // is it acknowledged.
        Q_EMIT client.backupActionFinished(client.lastOp,
                                           QStringLiteral("create_backup"), true,
                                           QString(), QString());
        QVERIFY(!c.busy());
        QCOMPARE(c.errorCategory(), QStringLiteral("setup_interrupted"));
        QCOMPARE(client.acknowledged, 2);
    }

    // Review N3: a report queued behind an operation and lost to a sign-out is
    // never acknowledged, so the backend keeps its marker and the next session
    // reports it again.
    void aQueuedInterruptedSetupLostToSignOutIsNotAcknowledged()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        c.runAction(QStringLiteral("create_backup"));
        QVERIFY(c.busy());
        Q_EMIT client.backupActionFinished(0, QStringLiteral("enable"), false,
                                           QString(),
                                           QStringLiteral("setup_interrupted"));
        Q_EMIT client.loggedOut();
        QCOMPARE(client.acknowledged, 0);
        QVERIFY(c.error().isEmpty());
    }

    // A mistyped recovery key showed matrix-sdk's own English text ("The MAC
    // check for the secret storage key failed", or a Base58 error quoting the
    // bad character). Every category has its own translated sentence.
    void everyRecoveryFailureHasItsOwnPlainText()
    {
        const QStringList categories = {
            QStringLiteral("recovery_key_wrong"),
            QStringLiteral("recovery_key_malformed"),
            QStringLiteral("no_secret_storage"),
            QStringLiteral("secret_storage_unsupported"),
            QStringLiteral("secret_storage_mismatch"),
            QStringLiteral("network"),
            QStringLiteral("server_refused"),
        };
        const QString fallback = BackupController::recoveryFailureText(
            QStringLiteral("failed"));
        QSet<QString> seen;
        for (const QString &category : categories) {
            const QString text = BackupController::recoveryFailureText(category);
            QVERIFY2(text != fallback, qPrintable(category));
            QVERIFY2(!text.contains(QStringLiteral("MAC"))
                         && !text.contains(QStringLiteral("SDK"))
                         && !text.contains(category),
                     qPrintable(text));
            seen.insert(text);
        }
        QCOMPARE(seen.size(), categories.size());
        QVERIFY(BackupController::recoveryFailureText(
                    QStringLiteral("recovery_key_wrong"))
                    .contains(QStringLiteral("does not unlock")));
        // An unknown category is never shown raw.
        QCOMPARE(BackupController::recoveryFailureText(
                     QStringLiteral("something_new")), fallback);
    }

    void signOutRiskDoesNotSurviveLogout()
    {
        FakeBackupClient client;
        BackupController c;
        c.setClient(&client);
        Q_EMIT client.backupProgress(QStringLiteral("enabled"),
                                     QStringLiteral("done"), 0, 0);
        Q_EMIT client.loggedOut();
        QVERIFY(!c.progressAnswered());
        QCOMPARE(c.signOutKeyRisk(QStringLiteral("enabled"), 2, {})
                     .value("level").toString(), QStringLiteral("checking"));
    }
};

QTEST_MAIN(BackupControllerTest)
#include "BackupControllerTest.moc"
