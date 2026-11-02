
#include "app/SwitchTrace.h"

#include <QFile>
#include <QJsonObject>
#include <QtTest>

namespace st = lightning::switchtrace;

namespace {

qint64 g_now = 1000;
QStringList g_lines;
QtMessageHandler g_previous = nullptr;

void captureHandler(QtMsgType type, const QMessageLogContext &ctx,
                    const QString &msg)
{
    if (ctx.category && qstrcmp(ctx.category, "lightning.switchtrace") == 0) {
        g_lines << msg;
        return;
    }
    if (g_previous)
        g_previous(type, ctx, msg);
}

QStringList traceLines()
{
    QStringList out;
    for (const QString &l : std::as_const(g_lines))
        if (l.startsWith(QLatin1String("switch-trace id=")))
            out << l;
    return out;
}

QStringList probeLines()
{
    QStringList out;
    for (const QString &l : std::as_const(g_lines))
        if (l.startsWith(QLatin1String("switch-trace loggedIn=")))
            out << l;
    return out;
}

QString fieldOf(const QString &line, const QString &key)
{
    const QStringList parts = line.split(QLatin1Char(' '));
    for (const QString &p : parts)
        if (p.startsWith(key + QLatin1Char('=')))
            return p.mid(key.size() + 1);
    return QStringLiteral("<absent>");
}

}

class SwitchTraceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        g_now = 1000;
        g_lines.clear();
        g_previous = qInstallMessageHandler(captureHandler);
        st::setClockForTest([] { return g_now; });
        st::setEnabledForTest(true);
        st::resetForTest();
    }

    void cleanup()
    {
        st::resetForTest();
        st::setEnabledForTest(false);
        st::setClockForTest(nullptr);
        qInstallMessageHandler(g_previous);
        g_previous = nullptr;
    }

    void theEnvRuleMatchesTheSharedRule()
    {
        for (const char *off : {"", "0", "false", "off", "no", "  OFF ", "No",
                                "FALSE", "\t0\n"})
            QVERIFY2(!st::enabledFromEnvValue(QByteArray(off)), off);
        for (const char *on : {"1", "true", "yes", "on", "2", "anything",
                               " 1 ", "TRUE"})
            QVERIFY2(st::enabledFromEnvValue(QByteArray(on)), on);
        QVERIFY(!st::enabledFromEnvValue(QByteArray("1\xff")));
        QVERIFY(!st::enabledFromEnvValue(QByteArray("\xc2\xa0off\xe2\x80\x83")));
        QVERIFY(!st::enabledFromEnvValue(QByteArray("\xc2\x85no")));
        QVERIFY(st::enabledFromEnvValue(QByteArray("\xef\xbb\xbfoff")));
    }

    void disabledIsInertAndWritesNothing()
    {
        st::setEnabledForTest(false);
        QVERIFY(!st::enabled());
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QVERIFY(!st::active());
        st::stepBegin(st::Step::S1);
        st::stepEnd(st::Step::S1);
        { st::StepScope scope(st::Step::S10); }
        st::markLoginSucceeded();
        st::noteRustRestore(QJsonObject{{QStringLiteral("restore_ms"), 5}}, 1);
        st::noteFirstReset(3, 1, 1, {}, 1);
        st::markFirstFrame();
        st::markRoomInteractive();
        st::noteLoggedIn(true);
        st::noteLoggedIn(false);
        st::finish(st::Outcome::Ok);
        QCOMPARE(st::currentRecordForTest().id, quint64(0));
        QVERIFY2(g_lines.isEmpty(), qPrintable(g_lines.join(QLatin1Char('\n'))));
    }

    void stepsAccumulateNestAndIgnoreAnUnmatchedEnd()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QVERIFY(st::active());
        st::stepBegin(st::Step::S6);
        g_now += 10;
        st::stepEnd(st::Step::S6);
        g_now += 100;
        st::stepBegin(st::Step::S6);
        g_now += 5;
        st::stepEnd(st::Step::S6);
        st::stepBegin(st::Step::S3);
        g_now += 7;
        st::stepBegin(st::Step::S3);
        g_now += 3;
        st::stepEnd(st::Step::S3);
        g_now += 2;
        st::stepEnd(st::Step::S3);
        st::stepEnd(st::Step::S4);
        {
            st::StepScope scope(st::Step::S1);
            g_now += 4;
        }
        const st::Record r = st::currentRecordForTest();
        QCOMPARE(r.step(st::Step::S6), qint64(15));
        QCOMPARE(r.s6Runs, 2);
        QCOMPARE(r.step(st::Step::S3), qint64(12));
        QCOMPARE(r.step(st::Step::S4), qint64(-1));
        QCOMPARE(r.step(st::Step::S1), qint64(4));
        QCOMPARE(r.step(st::Step::S5), qint64(-1));
    }

    void s8RunsFromS7sEndAndRustFieldsReadMinusOneWhenMissing()
    {
        st::begin({}, QStringLiteral("@b:example.org"));
        {
            st::StepScope s7(st::Step::S7);
            g_now += 20;
        }
        g_now += 250;
        const QJsonObject rust{
            {QStringLiteral("v"), 1},
            {QStringLiteral("discovery_ms"), 120},
            {QStringLiteral("store_open_ms"), 40},
            {QStringLiteral("store_opens"), 4},
            {QStringLiteral("offline_fallback"), false},
            {QStringLiteral("restore_ms"), 9},
            {QStringLiteral("resume_unsent_ms"), 2},
            {QStringLiteral("restore_total_ms"), 200},
            {QStringLiteral("enq_wall_ms"), 5000.0},
        };
        st::noteRustRestore(rust, 5030);
        g_now += 50;
        st::noteRustRestore(QJsonObject{{QStringLiteral("restore_ms"), 999}},
                            9999);
        const st::Record r = st::currentRecordForTest();
        QCOMPARE(r.step(st::Step::S7), qint64(20));
        QCOMPARE(r.step(st::Step::S8), qint64(250));
        QCOMPARE(r.rustDiscoveryMs, qint64(120));
        QCOMPARE(r.rustStoreOpenMs, qint64(40));
        QCOMPARE(r.rustStoreOpens, qint64(4));
        QCOMPARE(r.rustFallbackBuildMs, qint64(-1));
        QCOMPARE(r.rustOfflineFallback, 0);
        QCOMPARE(r.rustRestoreMs, qint64(9));
        QCOMPARE(r.rustResumeUnsentMs, qint64(2));
        QCOMPARE(r.rustRestoreTotalMs, qint64(200));
        QCOMPARE(r.restorePollMs, qint64(30));
    }

    void anEmptyRustTraceLeavesEveryRustFieldUnmeasured()
    {
        // The Rust trace off, or the mock backend: nothing is invented, and
        // with no S7 stamp there is no S8 either.
        st::begin({}, QStringLiteral("@b:example.org"));
        st::noteRustRestore({}, 5000);
        const st::Record r = st::currentRecordForTest();
        QCOMPARE(r.step(st::Step::S8), qint64(-1));
        QCOMPARE(r.rustDiscoveryMs, qint64(-1));
        QCOMPARE(r.rustOfflineFallback, -1);
        QCOMPARE(r.restorePollMs, qint64(-1));
    }

    void theFirstResetCountsOnlyAfterStartSync()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        st::noteFirstReset(77, 1, 1, {}, 0);
        QCOMPARE(st::currentRecordForTest().resetRooms, qint64(-1));

        st::stepBegin(st::Step::S9);
        g_now += 300;
        const QJsonObject rust{
            {QStringLiteral("since_start_sync_ms"), 280},
            {QStringLiteral("sync_service_build_ms"), 30},
            {QStringLiteral("serialize_ms"), 60},
            {QStringLiteral("rooms"), 1000},
            {QStringLiteral("enq_wall_ms"), 8000},
        };
        st::noteFirstReset(1000, 600, 40, rust, 8015);
        g_now += 10;
        st::noteFirstReset(5, 1, 1, {}, 0);
        const st::Record r = st::currentRecordForTest();
        QCOMPARE(r.step(st::Step::S9), qint64(300));
        QCOMPARE(r.resetRooms, qint64(1000));
        QCOMPARE(r.resetParseMs, qint64(600));
        QCOMPARE(r.modelMs, qint64(40));
        QCOMPARE(r.rustSinceStartSyncMs, qint64(280));
        QCOMPARE(r.rustSyncBuildMs, qint64(30));
        QCOMPARE(r.rustResetSerializeMs, qint64(60));
        QCOMPARE(r.resetPollMs, qint64(15));
    }

    void bothTargetsCloseTheTraceOkAndAreKeptSeparate()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        st::markFirstFrame();
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().firstFrameMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));

        g_now += 500;
        st::markLoginSucceeded();
        g_now += 100;
        st::markFirstFrame();
        QVERIFY(st::active());
        g_now += 50;
        st::markFirstFrame();
        st::stepBegin(st::Step::S11);
        g_now += 900;
        st::markRoomInteractive();
        QVERIFY(!st::active());
        const QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("outcome")),
                 QStringLiteral("ok"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("first_frame_ms")),
                 QStringLiteral("600"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("room_interactive_ms")),
                 QStringLiteral("1550"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("s11")),
                 QStringLiteral("900"));
    }

    void theIntentIsTZeroAndTheTotalNeverRestarts()
    {
        st::noteIntent(QStringLiteral("@b:example.org"));
        g_now += 300;
        st::noteIntent(QStringLiteral("@b:example.org"));
        g_now += 200;
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QCOMPARE(st::currentRecordForTest().intentLeadMs, qint64(500));
        st::markLoginSucceeded();
        g_now += 100;
        st::markFirstFrame();
        st::finish(st::Outcome::Ok);
        const QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("intent_lead_ms")),
                 QStringLiteral("500"));
        QCOMPARE(fieldOf(lines.first(),
                         QStringLiteral("first_frame_from_intent_ms")),
                 QStringLiteral("600"));

        st::noteIntent(QStringLiteral("@b:example.org"));
        g_now += 50;
        st::noteIntent(QStringLiteral("@c:example.org"));
        g_now += 50;
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QCOMPARE(st::currentRecordForTest().intentLeadMs, qint64(-1));
        st::finish(st::Outcome::Refused);
    }

    void s11BeginsOnceAndOnlyAfterLogin()
    {
        st::begin({}, QStringLiteral("@b:example.org"));
        st::stepBegin(st::Step::S11);
        g_now += 1000;
        st::markLoginSucceeded();
        st::stepBegin(st::Step::S11);
        g_now += 40;
        st::stepBegin(st::Step::S11);
        g_now += 60;
        st::markFirstFrame();
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(100));
    }

    void targetTwoNeedsAnOpenS11AndIsGivenUpWhenTheRoomIsLeft()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        g_now += 500;
        st::markLoginSucceeded();
        st::markFirstFrame();
        g_now += 100;
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(-1));
        QVERIFY(st::active());

        st::stepBegin(st::Step::S11);
        g_now += 200;
        st::abandonRoomInteractive();
        st::stepBegin(st::Step::S11);
        g_now += 300;
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(-1));
        QVERIFY(st::active());
        g_now += st::kDeadlineMs;
        QVERIFY(!st::active());
        const QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("outcome")),
                 QStringLiteral("partial"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("room_interactive_ms")),
                 QStringLiteral("-1"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("s11")),
                 QStringLiteral("-1"));

        st::begin({}, QStringLiteral("@c:example.org"));
        st::markLoginSucceeded();
        st::abandonRoomInteractive();
        st::stepBegin(st::Step::S11);
        g_now += 70;
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(70));
    }

    void onlyATraceAwaitingItsLoginCanFail()
    {
        QVERIFY(!st::awaitingLogin());
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QVERIFY(st::awaitingLogin());
        st::markLoginSucceeded();
        QVERIFY(!st::awaitingLogin());
        QVERIFY(st::active());
        st::setEnabledForTest(false);
        QVERIFY(!st::awaitingLogin());
        st::setEnabledForTest(true);
    }

    void theDeadlineClosesAnIncompleteTracePartial()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        g_now += 1000;
        st::markLoginSucceeded();
        st::markFirstFrame();
        g_now += st::kDeadlineMs - 1;
        QVERIFY(st::active());
        g_now += 1;
        QVERIFY(!st::active());
        const QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("outcome")),
                 QStringLiteral("partial"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("room_interactive_ms")),
                 QStringLiteral("-1"));
        st::begin({}, QStringLiteral("@c:example.org"));
        g_now += 10 * st::kDeadlineMs;
        QVERIFY(st::active());
    }

    void aNewBeginSupersedesTheOpenTrace()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        st::begin(QStringLiteral("@b:example.org"),
                  QStringLiteral("@c:example.org"), st::Path::Startup);
        QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("id")),
                 QStringLiteral("1"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("outcome")),
                 QStringLiteral("superseded"));
        const st::Record open = st::currentRecordForTest();
        QCOMPARE(open.id, quint64(2));
        QVERIFY(open.path == st::Path::Startup);
        st::finish(st::Outcome::Refused);
        st::finish(st::Outcome::Failed);
        lines = traceLines();
        QCOMPARE(lines.size(), 2);
        QCOMPARE(fieldOf(lines.last(), QStringLiteral("outcome")),
                 QStringLiteral("refused"));
        QCOMPARE(fieldOf(lines.last(), QStringLiteral("path")),
                 QStringLiteral("startup"));
    }

    void theLoggedInProbeLogsTransitionsAndCountsFlipsInATrace()
    {
        st::noteLoggedIn(true);
        st::noteLoggedIn(true);
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        g_now += 25;
        st::noteLoggedIn(false);
        g_now += 25;
        st::noteLoggedIn(false);
        st::noteLoggedIn(true);
        QCOMPARE(probeLines(),
                 QStringList({QStringLiteral("switch-trace loggedIn=1 t=-1"),
                              QStringLiteral("switch-trace loggedIn=0 t=25"),
                              QStringLiteral("switch-trace loggedIn=1 t=50")}));
        QCOMPARE(st::currentRecordForTest().loggedInFlips, 2);
    }

    void theLineIsExactAndCarriesSlugsAndNumbersOnly()
    {
        st::Record r;
        r.id = 7;
        r.path = st::Path::RetireThenCold;
        r.outcome = st::Outcome::Partial;
        r.fromSlug = QStringLiteral("alice_example.org");
        r.toSlug = QString();
        for (int i = 0; i < st::kStepCount; ++i)
            r.steps[i] = (i + 1) * 10;
        r.steps[4] = -1;
        r.s6Runs = 2;
        r.rustDiscoveryMs = 1;
        r.rustStoreOpenMs = 2;
        r.rustStoreOpens = 4;
        r.rustFallbackBuildMs = 0;
        r.rustOfflineFallback = 1;
        r.rustRestoreMs = 3;
        r.rustResumeUnsentMs = 5;
        r.rustRestoreTotalMs = 6;
        r.restorePollMs = 7;
        r.resetRooms = 1000;
        r.rustSyncBuildMs = 8;
        r.rustSinceStartSyncMs = 9;
        r.rustResetSerializeMs = 11;
        r.resetPollMs = 12;
        r.resetParseMs = 13;
        r.modelMs = 14;
        r.firstFrameMs = 15;
        r.roomInteractiveMs = -1;
        r.loggedInFlips = 3;
        QCOMPARE(st::formatLine(r),
                 QStringLiteral(
                     "switch-trace id=7 path=retire-then-cold outcome=partial "
                     "from=alice_example.org to=- "
                     "s1=10 s2=20 s3=30 s4=40 s5=-1 s6=60 s7=70 s8=80 s9=90 "
                     "s10=100 s11=110 s6_runs=2 "
                     "rust_discovery_ms=1 rust_store_open_ms=2 "
                     "rust_store_opens=4 rust_fallback_build_ms=0 "
                     "rust_offline_fallback=1 rust_restore_ms=3 "
                     "rust_resume_unsent_ms=5 rust_restore_total_ms=6 "
                     "restore_poll_ms=7 reset_rooms=1000 rust_sync_build_ms=8 "
                     "rust_since_start_sync_ms=9 rust_reset_serialize_ms=11 "
                     "reset_poll_ms=12 reset_parse_ms=13 model_ms=14 "
                     "first_frame_ms=15 room_interactive_ms=-1 "
                     "logged_in_flips=3 intent_lead_ms=-1 "
                     "first_frame_from_intent_ms=-1"));

        st::begin(QStringLiteral("@alice:example.org"),
                  QStringLiteral("@bob:example.org"));
        st::finish(st::Outcome::Failed);
        const QStringList lines = traceLines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("from")),
                 QStringLiteral("alice_example.org"));
        QCOMPARE(fieldOf(lines.first(), QStringLiteral("to")),
                 QStringLiteral("bob_example.org"));
        QVERIFY(!lines.first().contains(QLatin1Char('@')));
        QVERIFY(!lines.first().contains(QLatin1Char(':')));
    }

    void appControllerStampsEveryStepItOwns()
    {
#ifndef LIGHTNING_SOURCE_DIR
        QSKIP("LIGHTNING_SOURCE_DIR not defined for this target");
#else
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        const auto body = [&src](const char *signature) {
            const int at = src.indexOf(QLatin1String(signature));
            if (at < 0)
                return QString();
            const int end = src.indexOf(QStringLiteral("\n}\n"), at);
            return end > at ? src.mid(at, end - at) : QString();
        };

        const QString sw = body("void AppController::switchToAccount(");
        QVERIFY2(sw.contains(QStringLiteral("hasSavedAccount(target)")),
                 "the switchToAccount extent is not the switch");
        const int begin = sw.indexOf(QStringLiteral("trace::begin("));
        const int signIn = sw.indexOf(QStringLiteral("signInStateFor(target)"));
        QVERIFY2(begin >= 0 && signIn > begin,
                 "the trace does not open before the S1 keyring read");
        QCOMPARE(sw.count(QStringLiteral("trace::Outcome::Refused")), 2);
        for (const char *step : {"trace::Step::S1", "trace::Step::S2",
                                 "trace::Step::S3", "trace::Step::S4"})
            QVERIFY2(sw.contains(QLatin1String(step)), step);

        QVERIFY(body("void AppController::failAccountSwitch(")
                    .contains(QStringLiteral("Outcome::Failed")));
        const QString login = body("void AppController::onLoginSucceeded(");
        QVERIFY(login.contains(QStringLiteral("startSync()")));
        QVERIFY(login.contains(QStringLiteral("markLoginSucceeded()")));
        QVERIFY(login.contains(QStringLiteral("Step::S10")));
        QVERIFY(login.contains(QStringLiteral("armSwitchTraceFirstFrame()")));
        QVERIFY(body("void AppController::clearCrossAccountCaches(")
                    .contains(QStringLiteral("Step::S6")));
        const QString hydr = body("void AppController::setActiveRoomHydrating(");
        const int mark = hydr.indexOf(QStringLiteral("markRoomInteractive()"));
        QVERIFY2(mark >= 0, "target 2 is no longer stamped");
        QVERIFY2(hydr.lastIndexOf(QStringLiteral("->isActive()"), mark) >= 0
                     && hydr.lastIndexOf(
                            QStringLiteral("m_currentScreen == MainScreen"), mark) >= 0,
                 "target 2 takes a focus loss or a hidden shell for a fill");
        const QString room = body("void AppController::setCurrentRoomId(");
        const int abandon = room.indexOf(QStringLiteral("abandonRoomInteractive()"));
        const int s11 = room.indexOf(QStringLiteral("Step::S11"));
        QVERIFY2(s11 >= 0, "S11 is no longer begun");
        QVERIFY2(abandon >= 0 && abandon < s11,
                 "a room change does not give target 2 up first");
        const int failed = src.indexOf(QStringLiteral("&AuthManager::loginFailed"));
        QVERIFY2(failed >= 0, "the loginFailed handler moved; re-point this scan");
        const int fin = src.indexOf(
            QStringLiteral("switchtrace::finish(lightning::switchtrace::Outcome::Failed)"),
            failed);
        QVERIFY2(fin > failed && fin - failed < 1200,
                 "the loginFailed handler no longer closes the trace");
        const int gate = src.lastIndexOf(QStringLiteral("switchtrace::awaitingLogin()"), fin);
        QVERIFY2(gate > failed && fin - gate < 120,
                 "a login failure after the traced login relabels the switch Failed");
        const QString arm = body("void AppController::armSwitchTraceFirstFrame(");
        QVERIFY(arm.contains(QStringLiteral("QQuickWindow::frameSwapped")));
        QVERIFY(arm.contains(QStringLiteral("markFirstFrame()")));
        const int probe = src.indexOf(QStringLiteral("noteLoggedIn(loggedIn())"));
        QVERIFY2(probe >= 0, "the loggedIn probe is gone");
        const int guard = src.lastIndexOf(
            QStringLiteral("if (lightning::switchtrace::enabled())"), probe);
        QVERIFY2(guard >= 0 && probe - guard < 300,
                 "the loggedIn probe is connected with the trace off");
#endif
    }
};

QTEST_GUILESS_MAIN(SwitchTraceTest)
#include "SwitchTraceTest.moc"
