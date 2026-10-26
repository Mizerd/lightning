// The opt-in account-switch tracer (LIGHTNING_SWITCH_TRACE): the env rule it
// shares with Rust, inert when off, accumulating and nesting steps, S8 and
// S9 taken from their real boundaries, Rust fields read with -1 for a missing
// key, the two targets kept separate, the 30 s deadline, supersession, the
// loggedIn probe, and a line that carries account slugs and numbers only.

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

// One "key=value" field of a written line.
QString fieldOf(const QString &line, const QString &key)
{
    const QStringList parts = line.split(QLatin1Char(' '));
    for (const QString &p : parts)
        if (p.startsWith(key + QLatin1Char('=')))
            return p.mid(key.size() + 1);
    return QStringLiteral("<absent>");
}

} // namespace

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

    // The contract's §1 rule, which Rust's reader applies too: one variable
    // must switch both halves of a trace on together.
    void theEnvRuleMatchesTheSharedRule()
    {
        for (const char *off : {"", "0", "false", "off", "no", "  OFF ", "No",
                                "FALSE", "\t0\n"})
            QVERIFY2(!st::enabledFromEnvValue(QByteArray(off)), off);
        for (const char *on : {"1", "true", "yes", "on", "2", "anything",
                               " 1 ", "TRUE"})
            QVERIFY2(st::enabledFromEnvValue(QByteArray(on)), on);
        // The edges where a byte-wise reading parted from Rust's: bytes that
        // are not UTF-8 (env::var fails: off), Unicode whitespace (trimmed),
        // and a leading BOM (kept, so the value is not "off").
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
        // S6 runs twice per switch (switch, then onLoginSucceeded): the
        // durations add and the runs are counted.
        st::stepBegin(st::Step::S6);
        g_now += 10;
        st::stepEnd(st::Step::S6);
        g_now += 100;
        st::stepBegin(st::Step::S6);
        g_now += 5;
        st::stepEnd(st::Step::S6);
        // Nested begin/end of one step: only the outermost pair counts.
        st::stepBegin(st::Step::S3);
        g_now += 7;
        st::stepBegin(st::Step::S3);
        g_now += 3;
        st::stepEnd(st::Step::S3);
        g_now += 2;
        st::stepEnd(st::Step::S3);
        // An end with no begin changes nothing.
        st::stepEnd(st::Step::S4);
        // RAII.
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
        // S5 is folded into S4 in Phase 0 and never measured.
        QCOMPARE(r.step(st::Step::S5), qint64(-1));
    }

    void s8RunsFromS7sEndAndRustFieldsReadMinusOneWhenMissing()
    {
        st::begin({}, QStringLiteral("@b:example.org"));
        {
            st::StepScope s7(st::Step::S7);
            g_now += 20;
        }
        g_now += 250; // restore thread + poll
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
            // fallback_build_ms deliberately absent
        };
        st::noteRustRestore(rust, 5030);
        // Only the first per trace is recorded.
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
        // Before startSync a reset can only be the previous account's.
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
        st::noteFirstReset(5, 1, 1, {}, 0); // not the first: ignored
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
        // Targets before the login are the previous account's frames.
        st::markFirstFrame();
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().firstFrameMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));

        g_now += 500;
        st::markLoginSucceeded();
        g_now += 100;
        st::markFirstFrame();
        QVERIFY(st::active()); // one target is not enough
        g_now += 50;
        st::markFirstFrame(); // first per trace only
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

    // T0 at the click (Part 4 §5): the lead before begin() (a live-account
    // release, a queue behind a running switch) is reported, and the first
    // frame is also measured from the click. A re-entry keeps the first T0;
    // a click on another account replaces it.
    void theIntentIsTZeroAndTheTotalNeverRestarts()
    {
        st::noteIntent(QStringLiteral("@b:example.org"));
        g_now += 300;
        st::noteIntent(QStringLiteral("@b:example.org")); // re-entry
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

        // Newest click wins; the old target's intent does not leak.
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
        st::stepBegin(st::Step::S11); // before login: ignored
        g_now += 1000;
        st::markLoginSucceeded();
        st::stepBegin(st::Step::S11);
        g_now += 40;
        // A repeated begin keeps the first start and needs no second end.
        // (AppController abandons first when the room really changes; see
        // targetTwoNeedsAnOpenS11AndIsGivenUpWhenTheRoomIsLeft.)
        st::stepBegin(st::Step::S11);
        g_now += 60;
        st::markFirstFrame();
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(100));
    }

    // Target 2 is S11's end and nothing else: a fill signal before the
    // first room's open is not one (its edge can be a focus change), and a
    // room left before it filled gives the target up rather than recording
    // the NEXT room's fill against the first one's start.
    void targetTwoNeedsAnOpenS11AndIsGivenUpWhenTheRoomIsLeft()
    {
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        g_now += 500;
        st::markLoginSucceeded();
        st::markFirstFrame();
        g_now += 100;
        // Logged in, no room opened yet: S11 is not open.
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(-1));
        QVERIFY(st::active());

        st::stepBegin(st::Step::S11);
        g_now += 200;
        st::abandonRoomInteractive(); // the user opened another room
        st::stepBegin(st::Step::S11); // ...which does not restart S11
        g_now += 300;
        st::markRoomInteractive(); // that room's fill
        QCOMPARE(st::currentRecordForTest().roomInteractiveMs, qint64(-1));
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(-1));
        QVERIFY(st::active());
        // So the trace ends Partial with target 2 unmeasured, not wrong.
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

        // Abandoning with nothing open is a no-op.
        st::begin({}, QStringLiteral("@c:example.org"));
        st::markLoginSucceeded();
        st::abandonRoomInteractive();
        st::stepBegin(st::Step::S11);
        g_now += 70;
        st::markRoomInteractive();
        QCOMPARE(st::currentRecordForTest().step(st::Step::S11), qint64(70));
    }

    // AppController finishes a trace Failed on loginFailed only while the
    // trace still awaits its login: an add-account attempt failing after a
    // switch succeeded must not relabel that switch.
    void onlyATraceAwaitingItsLoginCanFail()
    {
        QVERIFY(!st::awaitingLogin()); // nothing open
        st::begin(QStringLiteral("@a:example.org"),
                  QStringLiteral("@b:example.org"));
        QVERIFY(st::awaitingLogin());
        st::markLoginSucceeded();
        QVERIFY(!st::awaitingLogin());
        QVERIFY(st::active());
        st::setEnabledForTest(false);
        QVERIFY(!st::awaitingLogin()); // off: inert
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
        // Without a login there is no deadline: a switch waiting on a slow
        // restore is not cut short.
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
        st::finish(st::Outcome::Failed); // none open: no second line
        lines = traceLines();
        QCOMPARE(lines.size(), 2);
        QCOMPARE(fieldOf(lines.last(), QStringLiteral("outcome")),
                 QStringLiteral("refused"));
        QCOMPARE(fieldOf(lines.last(), QStringLiteral("path")),
                 QStringLiteral("startup"));
    }

    void theLoggedInProbeLogsTransitionsAndCountsFlipsInATrace()
    {
        st::noteLoggedIn(true); // no trace open: logged, not counted
        st::noteLoggedIn(true); // a repeat: ignored
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

    // The exact line, key order included: a log parser reads it by position
    // as well as by key, and the contract fixes the set.
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

        // A real trace: accounts appear as slugs, never as MXIDs.
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

    // AppController's stamps, by source scan (the controller needs a whole
    // application to construct). A stamp that silently disappears leaves a
    // step reading -1 in every trace, which looks like "not measured" rather
    // than "broken". Each extent is checked to be real first.
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
        // The sign-in refusal and the detach refusal.
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
        // Target 2 counts only an edge seen with the main window active on
        // the main screen: QML's hydrating value also drops on a focus loss.
        const QString hydr = body("void AppController::setActiveRoomHydrating(");
        const int mark = hydr.indexOf(QStringLiteral("markRoomInteractive()"));
        QVERIFY2(mark >= 0, "target 2 is no longer stamped");
        QVERIFY2(hydr.lastIndexOf(QStringLiteral("->isActive()"), mark) >= 0
                     && hydr.lastIndexOf(
                            QStringLiteral("m_currentScreen == MainScreen"), mark) >= 0,
                 "target 2 takes a focus loss or a hidden shell for a fill");
        // Leaving the measured room gives target 2 up BEFORE the next room
        // could (re)start S11.
        const QString room = body("void AppController::setCurrentRoomId(");
        const int abandon = room.indexOf(QStringLiteral("abandonRoomInteractive()"));
        const int s11 = room.indexOf(QStringLiteral("Step::S11"));
        QVERIFY2(s11 >= 0, "S11 is no longer begun");
        QVERIFY2(abandon >= 0 && abandon < s11,
                 "a room change does not give target 2 up first");
        // A login failure finishes the trace only while it awaits its login.
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
        // The probe is connected only when the trace is on.
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
