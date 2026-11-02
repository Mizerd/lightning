
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
        const qint64 bobAt = bridge.firstAtMs(QStringLiteral("shutdownTasks"), kBob);
        const qint64 carolAt = bridge.firstAtMs(QStringLiteral("shutdownTasks"), kCarol);
        QVERIFY(bobAt >= 0 && carolAt >= 0);
        QVERIFY2(qAbs(bobAt - carolAt) < 600,
                 qPrintable(QStringLiteral("hosts retired one after another: %1 ms apart")
                                .arg(qAbs(bobAt - carolAt))));
        QVERIFY2(tookMs < 6000, qPrintable(QStringLiteral("%1 ms").arg(tookMs)));
        for (const auto &call : bridge.log()) {
            if (call.name == QStringLiteral("destroyAndReport"))
                QVERIFY(!call.onGuiThread);
        }
        const QStringList steps = app.liveAccountsStepsForTest();
        const int begun = int(steps.indexOf(QStringLiteral("quit:begin")));
        const int stopped =
            int(steps.indexOf(QStringLiteral("quit:foreground-stopped")));
        const int drained = int(steps.indexOf(QStringLiteral("quit:drained")));
        QVERIFY(begun >= 0);
        QVERIFY(begun < stopped);
        QVERIFY(stopped < drained);
        QVERIFY(app.settings()->lastLiveAtFor(kAlice) >= quitWallMs);
    }

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
        QVERIFY2(quit.elapsed() < 8000, "the quit outlived its budget");
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("quit:over-budget")));
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("quit:drained")));
        QCOMPARE(bridge.callsFor(QStringLiteral("finalSessionTokens"), kBob), 0);
    }

    void aHostStuckRestoringDoesNotDropAnotherAccountsHandback()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
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
        QCOMPARE(bridge.callsFor(QStringLiteral("finalSessionTokens"), kBob), 0);
    }

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
