// Live accounts (Labs), Phase 1: quitting with background accounts (design
// §6.3 Quit, contract §6 rule 6). Every host starts retiring when the quit
// starts, on its own pool and in parallel with each other and with the
// foreground's stop, and they all share ONE store-close budget. A host still
// restoring past it is named in the log. The foreground's lastLiveAt is
// written. A controller torn down without a quit (tests, a crash path)
// still retires its hosts before the pool goes.
//
// Mock backend plus a scripted Rust bridge (tests/support/LabsAppHarness.h).

#include "support/LabsAppHarness.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace labstest;

class QuitParallelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        isolateSettings(m_configHome.path(), m_dataHome.path(),
                        "quit-parallel-test");
    }

    void init() { clearSettings(); }

    // Two hosts whose retirement takes ~0.9 s each (three blocking steps of
    // 300 ms) retire in about 0.9 s, not 1.8, and both are done when
    // prepareForShutdown() returns. Fails with: waitForQuit() not called in
    // prepareForShutdown (not done on return); beginQuit() not called there
    // (nothing retires); a pool that retires one host at a time (the second
    // retirement's first step >= 900 ms after the first's).
    void backgroundAccountsRetireInParallelWithinOneBudget()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob, kCarol }));
        bridge.setRetireDelayMs(300);
        const qint64 quitWallMs = QDateTime::currentMSecsSinceEpoch();

        QElapsedTimer quit;
        quit.start();
        app.prepareForShutdown();
        const qint64 tookMs = quit.elapsed();

        QCOMPARE(bridge.callsFor(QStringLiteral("destroyAndReport"), kBob), 1);
        QCOMPARE(bridge.callsFor(QStringLiteral("destroyAndReport"), kCarol), 1);
        // In parallel, read from the order of the calls rather than the wall
        // clock alone (§18 round 1, L10): Carol's retirement began before
        // Bob's finished its first 300 ms step, and each began within one
        // step of the quit. One after another, the second would start at
        // least 900 ms after the first.
        const qint64 bobAt = bridge.firstAtMs(QStringLiteral("shutdownTasks"), kBob);
        const qint64 carolAt = bridge.firstAtMs(QStringLiteral("shutdownTasks"), kCarol);
        QVERIFY(bobAt >= 0 && carolAt >= 0);
        QVERIFY2(qAbs(bobAt - carolAt) < 600,
                 qPrintable(QStringLiteral("hosts retired one after another: %1 ms apart")
                                .arg(qAbs(bobAt - carolAt))));
        // A loose bound on the whole: far under two sequential retirements
        // plus the 15 s budget, safe under load.
        QVERIFY2(tookMs < 6000, qPrintable(QStringLiteral("%1 ms").arg(tookMs)));
        // Off the GUI thread.
        for (const auto &call : bridge.log()) {
            if (call.name == QStringLiteral("destroyAndReport"))
                QVERIFY(!call.onGuiThread);
        }
        // Begun before the foreground's stop, awaited after it.
        const QStringList steps = app.liveAccountsStepsForTest();
        const int begun = int(steps.indexOf(QStringLiteral("quit:begin")));
        const int stopped =
            int(steps.indexOf(QStringLiteral("quit:foreground-stopped")));
        const int drained = int(steps.indexOf(QStringLiteral("quit:drained")));
        QVERIFY(begun >= 0);
        QVERIFY(begun < stopped);
        QVERIFY(stopped < drained);
        // The foreground was live until the quit.
        QVERIFY(app.settings()->lastLiveAtFor(kAlice) >= quitWallMs);
    }

    // A host still restoring when the budget runs out is reported, and the
    // quit is bounded by the budget. The pool's waitForQuit() answers true
    // here (a Starting host's retirement is deferred, not failed), so the
    // controller counts the hosts still restoring itself. The pool names each
    // one in its own log (the first expected warning: WP1-S's line, asserted
    // so the contract's wording stays). Fails with: the controller's
    // still-restoring count removed, or read only when waitForQuit() says
    // false (step "quit:drained", no "still restoring: 1"); the budget not
    // passed to waitForQuit() (a 15 s quit).
    void aHostStillRestoringPastTheBudgetIsReported()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        bridge.setStartingHeld(kBob, true);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        app.setLiveAccountsQuitBudgetMsForTest(500);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "quit before restore finished, tokens current as of last "
                "persist acct=")));
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "background accounts did not all retire within \\d+ ms at "
                "quit; still restoring: 1")));

        QElapsedTimer quit;
        quit.start();
        app.prepareForShutdown();
        // Bounded by its 500 ms budget, not by the 15 s close budget; loose
        // enough for a loaded machine (§18 round 1, L10).
        QVERIFY2(quit.elapsed() < 8000, "the quit outlived its budget");
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("quit:over-budget")));
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("quit:drained")));
        // The restore never finished, so nothing was written back for it.
        QCOMPARE(bridge.callsFor(QStringLiteral("finalSessionTokens"), kBob), 0);
    }

    // §18 round 1, MUST 2: one host that never finishes restoring held the
    // whole budget in waitForQuit()'s Starting loop, and the other accounts'
    // handbacks were then dropped by a write pass that gave up on an expired
    // deadline: Carol's last rotation never reached the keyring. Now finished
    // retirements and their writes run inside that loop. Fails with the old
    // loop (the keyring keeps Carol's older token).
    void aHostStuckRestoringDoesNotDropAnotherAccountsHandback()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        // Carol first, to Running: the pool starts one host at a time and
        // a start that never finishes holds the next one back, so Bob is
        // pinned once Carol runs, and then sticks in Starting.
        QVERIFY(startLiveAccounts(app, bridge, { kCarol }));
        QTRY_COMPARE_WITH_TIMEOUT(int(app.sessionPoolForTest()->liveState(kCarol)),
                                  int(AccountActivityModel::Running), kHostStartTimeoutMs);
        bridge.setStartingHeld(kBob, true);
        bridge.setAccessToken(kBob, QStringLiteral("bob-token-fixture"));
        app.settings()->setKeepRunningFor(kBob, true);
        QTRY_VERIFY_WITH_TIMEOUT(app.sessionPoolForTest()->holdsStore(kBob),
                                 kHostStartTimeoutMs);
        QCOMPARE(int(app.sessionPoolForTest()->liveState(kBob)),
                 int(AccountActivityModel::Starting));
        const QString before = app.settings()->accessTokenFor(kCarol);
        QVERIFY(!before.isEmpty());
        // Carol rotated after her last saved write: only the handback has it.
        bridge.setAccessToken(kCarol, QStringLiteral("carol-rotated-at-quit"));
        app.setLiveAccountsQuitBudgetMsForTest(1500);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "quit before restore finished, tokens current as of last "
                "persist acct=")));
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "background accounts did not all retire within \\d+ ms at "
                "quit; still restoring: 1")));
        app.prepareForShutdown();
        QCOMPARE(bridge.callsFor(QStringLiteral("destroyAndReport"), kCarol), 1);
        QCOMPARE(app.settings()->accessTokenFor(kCarol),
                 QStringLiteral("carol-rotated-at-quit"));
        // Bob never finished restoring, so nothing was written for him.
        QCOMPARE(bridge.callsFor(QStringLiteral("finalSessionTokens"), kBob), 0);
    }

    // No quit at all: the controller's destructor retires and awaits the
    // hosts itself, before the pool and the members it points at go.
    // Fails with: the Labs block removed from ~AppController (the pool is
    // destroyed with a live host; nothing retired).
    void aControllerDestroyedWithoutAQuitStillRetiresItsHosts()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        {
            AppController app(AppController::MockBackend, false,
                              inMemoryKeyring());
            saveAccountsAndSignInAlice(app);
            QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        }
        QCOMPARE(bridge.callsFor(QStringLiteral("destroyAndReport"), kBob), 1);
    }

    // The foreground's own store close starts BEFORE the background
    // accounts are awaited, so the two run side by side within one budget
    // (contract §6.6) rather than one after the other. The mock backend has
    // no store to close, so this is read from the source: in
    // prepareForShutdown(), after the foreground's stopSync() and before
    // waitForQuit(). Fails with: the call moved after the wait, or removed.
    void theForegroundStartsClosingBeforeTheWait()
    {
        const QString source = sourceOf(QStringLiteral("src/app/AppController.cpp"));
        QVERIFY(!source.isEmpty());
        const QString body =
            bodyOf(source, QStringLiteral("void AppController::prepareForShutdown()"));
        QVERIFY(!body.isEmpty());
        const qsizetype stop = body.indexOf(QStringLiteral("m_client->stopSync()"));
        const qsizetype hook = body.indexOf(QStringLiteral("\"beginQuitRetirement\""));
        const qsizetype wait = body.indexOf(QStringLiteral("m_sessionPool->waitForQuit("));
        QVERIFY(stop >= 0);
        QVERIFY2(hook > stop, "the foreground store close is not started after its stopSync()");
        QVERIFY2(wait > hook, "the background accounts are awaited before the foreground starts closing");
    }

    // ...and the hook it asks for by name exists, so that call is not a
    // silent no-op (the controller then logs that the quit waits serially).
    // It is WP1-S's (RustSdkMatrixClient): until it lands this case FAILS on
    // purpose, naming the gap (la1-A notes, NEEDS FROM WP1-S). Live timing on
    // the Rust backend: NOT TESTED.
    void theForegroundQuitHookExists()
    {
        const QString header =
            sourceOf(QStringLiteral("src/matrix/RustSdkMatrixClient.h"));
        QVERIFY(!header.isEmpty());
        QVERIFY2(header.contains(QRegularExpression(QStringLiteral(
                     "Q_INVOKABLE\\s+void\\s+beginQuitRetirement\\s*\\(\\s*\\)"))),
                 "RustSdkMatrixClient has no Q_INVOKABLE beginQuitRetirement(): "
                 "the quit waits for the foreground and the background accounts "
                 "one after the other");
    }

private:
    static QString sourceOf(const QString &relative)
    {
        QFile file(QStringLiteral(SOURCE_DIR "/") + relative);
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(file.readAll());
    }
    // From the definition line to the next top-level closing brace.
    static QString bodyOf(const QString &source, const QString &signature)
    {
        const qsizetype start = source.indexOf(signature);
        if (start < 0)
            return QString();
        const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
        return source.mid(start, end < 0 ? -1 : end - start);
    }

    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(QuitParallelTest)
#include "QuitParallelTest.moc"
