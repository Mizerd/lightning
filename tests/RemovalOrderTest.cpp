// Live accounts (Labs), Phase 1: removing an account that runs in the
// background (DECISION 7, design §9.2, contract §6 rule 3). The order is the
// point: forget its cards, open and badge row; resolve its identity while the
// record exists; sign it out on the server and retire it without writing
// anything back; await THAT store; delete its local state only once the
// store registry agrees; remove the record last. A store that never closes
// deletes nothing and says so.
//
// Mock backend plus a scripted Rust bridge (tests/support/LabsAppHarness.h).
// Mutations that make each case fail: ~/Documents/LLM/Lightning/Tasks/
// 2026-10-16-la1-A.md.

#include "support/LabsAppHarness.h"

#include "accounts/HostRetirementPool.h"
#include "accounts/SessionPool.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "notifications/NotificationManager.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace labstest;

namespace {
QString markerOf(const matrix::app_data::AccountIdentity &identity)
{
    return identity.rustStorePath + QStringLiteral("/marker");
}

int stepIndex(const QStringList &steps, const QString &step)
{
    return int(steps.indexOf(step));
}

QVariantMap cardFor(const QString &uid)
{
    return QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!r:two.example") },
                        { QStringLiteral("eventId"), QStringLiteral("$e") },
                        { QStringLiteral("threadRootId"), QString() },
                        { QStringLiteral("accountUserId"), uid } };
}

bool anyErrorContains(const QSignalSpy &errors, const QString &needle)
{
    for (const QList<QVariant> &args : errors) {
        if (args.value(0).toString().contains(needle))
            return true;
    }
    return false;
}

QString sourceOf(const QString &relative)
{
    QFile file(QStringLiteral(SOURCE_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// The body of `function` (from its definition line to the next top-level
// closing brace).
QString bodyOf(const QString &source, const QString &signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0)
        return QString();
    const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
    return source.mid(start, end < 0 ? -1 : end - start);
}
} // namespace

class RemovalOrderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        isolateSettings(m_configHome.path(), m_dataHome.path(),
                        "removal-order-test");
    }

    void init() { clearSettings(); }

    // The whole order on a live background account, observed from both
    // sides: what had happened when the server sign-out ran, and what is
    // left after. Fails with: the Labs branch in removeAccount() removed
    // (today's path deletes at once, no sign-out); the record removed before
    // the latch (step 6 before 4); removeAccountLocalStateFiles() called
    // before retireForRemoval().
    void aLiveBackgroundAccountIsSignedOutReleasedThenDeleted()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));

        const auto identity = identityOf(app, kBob);
        QVERIFY(identity.isValid());
        plantStoreFile(identity);
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        bool storeOnDiskAtSignOut = false;
        bridge.setLogoutHook([&storeOnDiskAtSignOut, identity](const QString &uid) {
            if (uid == kBob)
                storeOnDiskAtSignOut = QFileInfo::exists(markerOf(identity));
        });
        bridge.setRetireDelayMs(150);
        NotificationManager *notifications = app.notificationsForTest();
        notifications->recordPayloadForTest(kBob, 501, cardFor(kBob));
        QCOMPARE(notifications->pendingPayloadCountForTest(kBob), 1);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        // Synchronously: step 1 and 2 done, nothing deleted, record kept.
        QCOMPARE(notifications->pendingPayloadCountForTest(kBob), 0);
        QVERIFY2(app.settings()->hasSavedAccount(kBob),
                 "the record went before the account was signed out and "
                 "released");
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        {
            const QStringList steps = app.liveAccountsStepsForTest();
            const int forget = stepIndex(steps, QStringLiteral("removal:1-forget ") + kBob);
            const int resolve = stepIndex(steps, QStringLiteral("removal:2-identity ") + kBob);
            const int retire = stepIndex(steps, QStringLiteral("removal:3-retire ") + kBob);
            QVERIFY(forget >= 0);
            QVERIFY(forget < resolve);
            QVERIFY(resolve < retire);
            QVERIFY(!steps.contains(QStringLiteral("removal:5-delete ") + kBob));
        }

        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);

        // The server sign-out ran first, while everything was still there.
        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kBob)
                     + bridge.callsFor(QStringLiteral("oauthLogout"), kBob),
                 1);
        QVERIFY2(storeOnDiskAtSignOut,
                 "the store was deleted before the server sign-out");
        const int signOut = std::max(bridge.firstIndexOf(QStringLiteral("logout"), kBob),
                                     bridge.firstIndexOf(QStringLiteral("oauthLogout"), kBob));
        const int destroyed = bridge.firstIndexOf(QStringLiteral("destroyAndReport"), kBob);
        QVERIFY(signOut >= 0);
        QVERIFY2(destroyed > signOut, "retired before the server sign-out");
        // Then deleted, then the record.
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        const QStringList steps = app.liveAccountsStepsForTest();
        const int released = stepIndex(steps, QStringLiteral("removal:4-released ") + kBob);
        const int deleted = stepIndex(steps, QStringLiteral("removal:5-delete ") + kBob);
        const int recordGone = stepIndex(steps, QStringLiteral("removal:6-record ") + kBob);
        QVERIFY(released >= 0);
        QVERIFY(released < deleted);
        QVERIFY(deleted < recordGone);
        QVERIFY(!anyErrorContains(errors, QStringLiteral("did not finish")));
        // The active account never moved.
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QVERIFY(app.loggedIn());
        QVERIFY(!app.sessionPoolForTest()->holdsStore(kBob));
    }

    // A store that never closes: the removal reports "not finished" and
    // deletes NOTHING, the record included, and the badge row comes back.
    // Fails with: the storeFree check removed from
    // finishLiveBackgroundRemoval(), or the record removed before it; the
    // reopenLiveAccountActivity() call removed from that branch.
    void aStoreThatNeverClosesDeletesNothing()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        bridge.setStoreHeldForever(kBob, true);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        // The latch's budget is the store-close budget (15 s).
        QTRY_VERIFY_WITH_TIMEOUT(
            anyErrorContains(errors, QStringLiteral("Removal did not finish")),
            30000);
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(steps.contains(QStringLiteral("removal:4-still-held ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:5-delete ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:6-record ") + kBob));
        // The account stays, so its badge row is not left tombstoned for the
        // rest of the session (step 1 tombstoned it).
        QVERIFY(app.accountActivity());
        QVERIFY2(!app.accountActivity()->isRemoved(kBob),
                 "a removal that did not finish left the badge row dead");
    }

    // A second click while the first removal runs adds nothing. Fails with:
    // the m_liveRemovals guard removed (two sign-outs).
    void aSecondClickWhileTheRemovalRunsDoesNothingMore()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(200);
        app.removeAccount(kBob);
        app.removeAccount(kBob);
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kBob)
                     + bridge.callsFor(QStringLiteral("oauthLogout"), kBob),
                 1);
        QCOMPARE(app.liveAccountsStepsForTest()
                     .filter(QStringLiteral("removal:1-forget"))
                     .size(),
                 1);
    }

    // An account that is not live keeps today's removal (immediate, no
    // server sign-out), and what it left from a background run goes with it.
    // Fails with: the Labs branch taken for a non-live account (no immediate
    // removal), or its cards left behind.
    void aNonLiveAccountKeepsTodaysRemoval()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        NotificationManager *notifications = app.notificationsForTest();
        notifications->recordPayloadForTest(kCarol, 601, cardFor(kCarol));

        app.removeAccount(kCarol);

        QVERIFY(!app.settings()->hasSavedAccount(kCarol));
        QCOMPARE(notifications->pendingPayloadCountForTest(kCarol), 0);
        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kCarol), 0);
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("removal:not-live ") + kCarol));
        // The live one is untouched.
        QVERIFY(app.sessionPoolForTest()->holdsStore(kBob));
    }

    // §18 round 4, R4-1 (§6): an account with no host whose sign-in is
    // still being written (here its parked pair, queued through the keyring
    // worker) took today's removal, which deletes the record and secrets at
    // once while that write may still land: a token at rest with no record.
    // Now it takes the live removal: writes forbidden first (the queued one
    // answers Cancelled), the parked pair discarded, and nothing deleted
    // until the write has answered. Fails with holdsStore() in place of
    // holdsSignIn() in removeAccount(): "removal:not-live".
    void aNonLiveAccountWhoseSignInIsStillBeingWrittenTakesTheLiveRemoval()
    {
        namespace la = lightning::accounts;
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        la::SessionPool *pool = app.sessionPoolForTest();
        // The pool starts one host at a time: Bob's catch-up must be over
        // before Carol's admission (and its parked write) can begin.
        QTRY_COMPARE_WITH_TIMEOUT(int(pool->liveState(kBob)),
                                  int(AccountActivityModel::Running), kHostStartTimeoutMs);
        const QString device =
            app.settings()->accountRecord(kCarol).value(QStringLiteral("deviceId")).toString();
        QVERIFY(!device.isEmpty());
        la::HostSignInRetirements::process().parkUnsaved(la::HostSignInRetirements::UnsavedHandback{
            kCarol, device, la::custody::accessTokenDigest(QStringLiteral("carol-older")), {},
            la::SessionTokenPair{QStringLiteral("carol-parked"), QString()}});
        // Pinned: admission writes the parked pair first (queued, not run yet).
        app.settings()->setKeepRunningFor(kCarol, true);
        // Admitted once the scheduler's catch-up cooldown after Bob is over
        // (up to 1 s); checked in the same turn, before the queued write runs.
        QTRY_VERIFY_WITH_TIMEOUT((pool->admitNowForTest(), pool->holdsSignIn(kCarol)),
                                 kHostStartTimeoutMs);
        QVERIFY2(!pool->holdsStore(kCarol), "a host started: not the case under test");

        app.removeAccount(kCarol);

        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY2(!steps.contains(QStringLiteral("removal:not-live ") + kCarol),
                 "today's removal ran while a keyring write of the account was pending");
        QVERIFY(steps.contains(QStringLiteral("removal:3-retire ") + kCarol));
        QVERIFY2(app.settings()->hasSavedAccount(kCarol),
                 "the record went before the pending write answered");
        QVERIFY(!la::HostSignInRetirements::process().hasUnsaved(kCarol));
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kCarol), 20000);
        QVERIFY(!pool->holdsSignIn(kCarol));
        // The parked pair never reached the keyring, before or after.
        QVERIFY(!la::HostSignInRetirements::process().hasUnsaved(kCarol));
        QCOMPARE(bridge.callsFor(QStringLiteral("createBackground"), kCarol), 0);
    }

    // Step 1 cancels a notification open waiting for that account and the
    // release a click started for it: nothing switches to an account being
    // removed. Fails with: cancelParkedOpenFor()/cancelRelease() dropped from
    // step 1.
    void aRemovalCancelsAnOpenAndASwitchWaitingForIt()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(300);
        NotificationManager *notifications = app.notificationsForTest();
        Q_EMIT notifications->openRequestedForAccount(
            kBob, QStringLiteral("!general:mock.local"), QStringLiteral("$e"),
            QString());
        Q_EMIT notifications->openRequested(QStringLiteral("!general:mock.local"),
                                            QStringLiteral("$e"), QString());
        QVERIFY(app.hasParkedOpenForTest());
        // The switch starts on the next turn, by freeing Bob's store.
        QTRY_VERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:release ") + kBob));

        app.removeAccount(kBob);
        QVERIFY(!app.hasParkedOpenForTest());
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(stepIndex(steps, QStringLiteral("open:cancelled ") + kBob)
                < stepIndex(steps, QStringLiteral("removal:2-identity ") + kBob));

        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QTest::qWait(200);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:free ") + kBob));
    }

    // The Labs removal never takes the pool-wide wait whose result today's
    // removal ignores, and deletes only once the store registry agrees:
    // removeAccountLocalStateFiles() asks claimForDeletion() before it
    // deletes anything, and a refusal returns before the record goes.
    // Source scan of the two functions. Fails with: removeAccountLocalState(
    // (which waits on the foreground pool) called there; the claim moved
    // after the deletion; the refusal check dropped or moved after the
    // record's removal.
    void theLiveRemovalDeletesOnlyAfterTheRegistryAgrees()
    {
        const QString source = sourceOf(QStringLiteral("src/app/AppController.cpp"));
        QVERIFY(!source.isEmpty());
        const QString body = bodyOf(
            source, QStringLiteral("void AppController::finishLiveBackgroundRemoval("));
        QVERIFY(!body.isEmpty());
        QVERIFY(!body.contains(QStringLiteral("removeAccountLocalState(")));
        QVERIFY(!body.contains(QStringLiteral("waitForRustRetirement")));
        const qsizetype freeCheck = body.indexOf(QStringLiteral("if (!storeFree)"));
        const qsizetype remove = body.indexOf(
            QStringLiteral("removeAccountLocalStateFiles(identity, &refused)"));
        const qsizetype refusedCheck = body.indexOf(QStringLiteral("if (refused)"));
        const qsizetype record = body.indexOf(QStringLiteral("m_accounts->removeAccount("));
        QVERIFY(freeCheck >= 0);
        QVERIFY(remove > freeCheck);
        QVERIFY(refusedCheck > remove);
        QVERIFY(record > refusedCheck);

        const QString files = bodyOf(
            source, QStringLiteral("bool AppController::removeAccountLocalStateFiles("));
        QVERIFY(!files.isEmpty());
        const qsizetype claim = files.indexOf(QStringLiteral("claimForDeletion("));
        const qsizetype deleted = files.indexOf(QStringLiteral("removeAccountRustState("));
        QVERIFY(claim >= 0);
        QVERIFY(deleted > claim);
    }

    // The server sign-out failed: the account is still removed from this
    // device, and the user is told it may still be listed elsewhere. Fails
    // with: the Failed/TimedOut notice removed; the local removal made to
    // depend on the sign-out's result.
    void aFailedServerSignOutStillRemovesAndSaysSo()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        bridge.setLogoutResult(QStringLiteral("failed"));
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        // Well inside the 15 s sign-out budget: the bridge answered.
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 10000);
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        QVERIFY(anyErrorContains(errors, QStringLiteral("signing it out on the server did not finish")));
        QVERIFY(!anyErrorContains(errors, QStringLiteral("Removal did not finish")));
    }

    // A click on an account whose removal is running is refused: its host is
    // retiring for a removal that deletes its store, and is never freed for
    // the foreground instead. Fails with: the m_liveRemovals check removed
    // from beginLiveAccountSwitch() (a "switch:release" for Bob).
    void aSwitchToAnAccountBeingRemovedIsRefused()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(300);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);
        app.switchToAccount(kBob);

        QVERIFY(anyErrorContains(errors, QStringLiteral("being removed")));
        QVERIFY(!app.accountSwitching());
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:release ") + kBob));
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    // ── The store registry enforcing (live accounts on a Rust build). The
    // mock backend never turns it on, so these cases do, directly. One-way
    // per process: they run LAST, and remove the records they add. ──

    // An account that is not live, whose store the registry still sees held
    // (a retirement that overran its budget): nothing is deleted, the record
    // and its sign-in stay, the badge row comes back, and the user is told.
    // Fails with: the refusal ignored in removeAccount() (the record goes,
    // the store stays: the §6 defect the review found); the out-parameter
    // never set in removeAccountLocalStateFiles().
    void aRefusedStoreDeletionKeepsTheRecord()
    {
        auto &registry = lightning::accounts::StoreOwnershipRegistry::process();
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, {}));
        const auto identity = identityOf(app, kCarol);
        QVERIFY(identity.isValid());
        plantStoreFile(identity);
        registry.setEnforced(true);
        const quint64 holder = registry.recordOpen(
            identity.rustStorePath, identity.slug,
            lightning::accounts::StoreOwnershipRegistry::Role::Host);
        registry.noteReleasing(holder);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kCarol);

        QVERIFY(app.settings()->hasSavedAccount(kCarol));
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(anyErrorContains(errors, QStringLiteral("Removal did not finish")));
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("removal:refused ") + kCarol));
        QVERIFY(!app.accountActivity()->isRemoved(kCarol));

        // Once the store is let go, the same removal completes.
        registry.noteClosed(holder, true);
        app.removeAccount(kCarol);
        QVERIFY(!app.settings()->hasSavedAccount(kCarol));
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        registry.clearForTest();
    }

    // The same on the live path: the store is free by its latch, but the
    // registry still names another holder at step 5. Nothing is deleted and
    // the record stays. Fails with: the refusal branch in
    // finishLiveBackgroundRemoval() removed (step 6 runs, the record goes).
    void aLiveRemovalTheRegistryRefusesDeletesNothing()
    {
        auto &registry = lightning::accounts::StoreOwnershipRegistry::process();
        registry.setEnforced(true);
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        // Another holder appears while the server sign-out runs (on the
        // pool's thread; the registry is thread-safe).
        quint64 holder = 0;
        bridge.setLogoutHook([&holder, identity](const QString &uid) {
            if (uid == kBob && holder == 0) {
                holder = lightning::accounts::StoreOwnershipRegistry::process()
                             .recordOpen(identity.rustStorePath, identity.slug,
                                         lightning::accounts::StoreOwnershipRegistry::
                                             Role::Foreground);
            }
        });
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        QTRY_VERIFY_WITH_TIMEOUT(
            anyErrorContains(errors, QStringLiteral("Removal did not finish")), 20000);
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(steps.contains(QStringLiteral("removal:4-released ") + kBob));
        QVERIFY(steps.contains(QStringLiteral("removal:5-refused ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:6-record ") + kBob));
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(!app.accountActivity()->isRemoved(kBob));
        registry.clearForTest();
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(RemovalOrderTest)
#include "RemovalOrderTest.moc"
