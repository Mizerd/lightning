// Live accounts (Labs), Phase 1: a background account whose sign-in the
// server revoked (R1-C1, design §3.3.2, contract §6 rule 4). The host
// classifies it with the foreground's own rule into the SAME per-account
// RevocationProofs the foreground client mirrors into (so signing in again
// takes today's move-aside or resume path), the account is marked, its host
// retired, and the user gets ONE notice "Sign-in expired · <account>".
// Nothing is deleted and no other account moves.
//
// Mock backend plus a scripted Rust bridge (tests/support/LabsAppHarness.h).
// NOT TESTED here (mock backend): that RustSdkMatrixClient::login() then
// consults the proof; that is WP1-S's and needs the Rust build.

#include "support/LabsAppHarness.h"

#include "auth/AccountManager.h"
#include "notifications/NotificationManager.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace labstest;

class RevocationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        isolateSettings(m_configHome.path(), m_dataHome.path(),
                        "revocation-test");
    }

    void init() { clearSettings(); }

    // Fails with: Deps::proofs not pointed at m_revocationProofs (no proof
    // in the controller's map); the hostSignInExpired connection removed (no
    // notice); the notice raised per poll instead of once.
    void aRevokedBackgroundAccountRaisesOneNoticeAndKeepsItsProof()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        NotificationManager *notifications = app.notificationsForTest();
        const int noticesBefore = notifications->genericNoticeCountForTest();

        bridge.revoke(kBob);

        QTRY_VERIFY_WITH_TIMEOUT(
            app.revocationProofsForTest().hardFor(kBob).has_value(), 10000);
        QCOMPARE(app.revocationProofsForTest().hardFor(kBob)->deviceId,
                 QStringLiteral("BOBDEV"));
        QTRY_COMPARE_WITH_TIMEOUT(notifications->genericNoticeCountForTest(),
                                  noticesBefore + 1, 10000);
        QCOMPARE(notifications->lastTrayTitleForTest(),
                 QStringLiteral("Sign-in expired · ") + iso(QStringLiteral("bob")));
        QVERIFY(app.accounts()->sessionRevoked(kBob));
        QTRY_VERIFY_WITH_TIMEOUT(!app.sessionPoolForTest()->holdsStore(kBob),
                                 10000);
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("revoked ") + kBob));
        // One notice, not one per drainer tick.
        QTest::qWait(600);
        QCOMPARE(notifications->genericNoticeCountForTest(), noticesBefore + 1);
        // Nothing deleted, nothing moved.
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QVERIFY(QFileInfo::exists(identity.rustStorePath + QStringLiteral("/marker")));
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QVERIFY(app.loggedIn());
        // A revoked account is not restarted.
        QCOMPARE(bridge.callsFor(QStringLiteral("createBackground"), kBob), 1);
    }

    // Two revoked background accounts both keep their proofs: the map is per
    // account, unlike the client's single slot. Fails with: the pool writing
    // proofs into a single-slot holder (the first is overwritten).
    void twoRevokedAccountsBothKeepTheirProofs()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob, kCarol }));
        NotificationManager *notifications = app.notificationsForTest();
        const int noticesBefore = notifications->genericNoticeCountForTest();

        bridge.revoke(kBob);
        bridge.revoke(kCarol);

        QTRY_VERIFY_WITH_TIMEOUT(
            app.revocationProofsForTest().hardFor(kBob).has_value()
                && app.revocationProofsForTest().hardFor(kCarol).has_value(),
            10000);
        QTRY_COMPARE_WITH_TIMEOUT(notifications->genericNoticeCountForTest(),
                                  noticesBefore + 2, 10000);
        QVERIFY(app.accounts()->sessionRevoked(kBob));
        QVERIFY(app.accounts()->sessionRevoked(kCarol));
        QVERIFY(!app.accounts()->sessionRevoked(kAlice));
    }

    // The notice is gated like every other generic notice, and names the
    // account by its display name when the record has one, isolated (FSI ...
    // PDI) because that name is server-controlled. Driven from the pool's
    // signal, so it isolates the app's half. Fails with: the
    // notificationsEnabled() check removed; the label read from the user id
    // instead of the record; the isolation removed.
    void theNoticeIsGatedAndNamesTheAccount()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        app.settings()->setLabsConcurrentAccounts(true);
        QVERIFY(app.liveAccountsActive());
        lightning::accounts::SessionPool *pool = app.sessionPoolForTest();
        QVERIFY(pool);
        NotificationManager *notifications = app.notificationsForTest();
        app.accounts()->updateProfile(kCarol, QStringLiteral("Carol Q"),
                                      QString());

        app.settings()->setNotificationsEnabled(false);
        int before = notifications->genericNoticeCountForTest();
        Q_EMIT pool->hostSignInExpired(kCarol);
        QCOMPARE(notifications->genericNoticeCountForTest(), before);

        app.settings()->setNotificationsEnabled(true);
        before = notifications->genericNoticeCountForTest();
        Q_EMIT pool->hostSignInExpired(kCarol);
        QCOMPARE(notifications->genericNoticeCountForTest(), before + 1);
        QCOMPARE(notifications->lastTrayTitleForTest(),
                 QStringLiteral("Sign-in expired · ") + iso(QStringLiteral("Carol Q")));
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(RevocationTest)
#include "RevocationTest.moc"
