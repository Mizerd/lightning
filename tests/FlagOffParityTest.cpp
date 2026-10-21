// Live accounts (Labs), Phase 1: with the flag off this controller is
// today's (contract §1, design §4 T-OFF-1..3).
//
// T-OFF-1: no SessionPool, host, worker or model exists, the Rust bridge is
// never asked anything (not even available()), and app.accountActivity /
// app.accountList are null, for every way the flag can be "off": the shipped
// default (Labs offered in Settings, the switch off), the setting stored true
// with LIGHTNING_LABS_LIVE_ACCOUNTS forcing Labs off (a test override), and
// the switch on with the bridge unavailable.
// T-OFF-2: an account switch emits the same signals, in the same order, on
// the controller and on the mock client in every one of those "off" shapes
// as in a controller that has never heard of Labs, and the same as the
// golden (tests/data/flag-off-switch-golden.txt; recorded on 02693a52, to be
// recorded again on the base this lands on, with the variable unset).
// Scan: every Labs call in AppController.cpp outside its own section sits
// behind a live-accounts guard.
//
// Golden capture (once, by the lead, on a tree at 34d76bc8 with only this
// file and tests/support/LabsAppHarness.h added, built with
// -DFLAG_OFF_GOLDEN_CAPTURE so nothing from Phase 1 is needed):
//   LIGHTNING_FLAG_OFF_GOLDEN_WRITE=$PWD/tests/data/flag-off-switch-golden.txt
//     QT_QPA_PLATFORM=offscreen ./flag-off-parity-test aSwitchMatchesTheGolden
//   (one command line)
// then commit that file with Phase 1.

#ifndef FLAG_OFF_GOLDEN_CAPTURE
#include "support/LabsAppHarness.h"
#endif

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "storage/InMemorySecretStore.h"

#include <QFile>
#include <QMetaMethod>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

namespace {
const QString kAliceId = QStringLiteral("@alice:one.example");
const QString kBobId = QStringLiteral("@bob:two.example");

// Records every signal of the watched objects, in emission order, by name.
class SignalRecorder : public QObject
{
    Q_OBJECT
public:
    void watch(QObject *object, const QString &prefix)
    {
        m_prefixes.insert(object, prefix);
        const QMetaObject *meta = object->metaObject();
        const int slot = metaObject()->indexOfSlot("record()");
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod method = meta->method(i);
            if (method.methodType() != QMetaMethod::Signal)
                continue;
            const QByteArray name = method.name();
            if (name == "destroyed" || name == "objectNameChanged")
                continue;
            QMetaObject::connect(object, i, this, slot);
        }
    }
    QStringList events;
    bool recording = false;

public Q_SLOTS:
    void record()
    {
        if (!recording)
            return;
        QObject *from = sender();
        const int index = senderSignalIndex();
        if (!from || index < 0)
            return;
        events.append(m_prefixes.value(from) + QLatin1Char(':')
                      + QString::fromLatin1(
                          from->metaObject()->method(index).methodSignature()));
    }

private:
    QHash<QObject *, QString> m_prefixes;
};

std::unique_ptr<SecretStore> memoryKeyring(QObject *parent)
{
    return std::make_unique<InMemorySecretStore>(parent);
}

struct SwitchRecord {
    // Emitted inside the switchToAccount() call itself: deterministic.
    QStringList synchronous;
    // Emitted after it, until the switch settled: compared as a set, since
    // timers may interleave differently between runs.
    QStringList settled;
};

// Signs Alice in, then records a switch to Bob. False when the mock never
// settled (a harness failure, reported by the caller).
bool recordSwitch(AppController &app, SwitchRecord *out)
{
    SettingsManager *s = app.settings();
    s->saveSession(QStringLiteral("https://one.example"), kAliceId,
                   QStringLiteral("ALICEDEV"),
                   QStringLiteral("alice-token-fixture"));
    s->saveSession(QStringLiteral("https://two.example"), kBobId,
                   QStringLiteral("BOBDEV"),
                   QStringLiteral("bob-token-fixture"));
    app.switchToAccount(kAliceId);
    if (!QTest::qWaitFor([&app] { return !app.accountSwitching(); }, 5000))
        return false;
    QTest::qWait(100);
    SignalRecorder recorder;
    recorder.watch(&app, QStringLiteral("app"));
    if (MatrixClient *client = app.findChild<MatrixClient *>())
        recorder.watch(client, QStringLiteral("client"));
    recorder.recording = true;
    app.switchToAccount(kBobId);
    out->synchronous = recorder.events;
    recorder.events.clear();
    if (!QTest::qWaitFor([&app] { return !app.accountSwitching(); }, 5000))
        return false;
    QTest::qWait(100);
    recorder.recording = false;
    QStringList settled = recorder.events;
    settled.removeDuplicates();
    settled.sort();
    out->settled = settled;
    return true;
}

QString goldenPath()
{
    return QStringLiteral(SOURCE_DIR "/tests/data/flag-off-switch-golden.txt");
}

QString serialize(const SwitchRecord &record)
{
    return QStringLiteral("[synchronous]\n") + record.synchronous.join(QLatin1Char('\n'))
        + QStringLiteral("\n[settled]\n") + record.settled.join(QLatin1Char('\n'))
        + QLatin1Char('\n');
}
} // namespace

class FlagOffParityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        qputenv("DBUS_SESSION_BUS_ADDRESS",
                "unix:path=/nonexistent/lightning-no-daemon");
        qunsetenv("LIGHTNING_LABS_LIVE_ACCOUNTS");
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("flag-off-parity-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
        qunsetenv("LIGHTNING_LABS_LIVE_ACCOUNTS");
    }

    // T-OFF-2 against the golden from 34d76bc8. Compiles on that tree too
    // (FLAG_OFF_GOLDEN_CAPTURE), which is how the golden is written.
    void aSwitchMatchesTheGolden()
    {
        AppController app(AppController::MockBackend, false, memoryKeyring);
        SwitchRecord record;
        QVERIFY(recordSwitch(app, &record));
        const QString recorded = serialize(record);
        const QByteArray writeTo = qgetenv("LIGHTNING_FLAG_OFF_GOLDEN_WRITE");
        if (!writeTo.isEmpty()) {
            QFile out(QString::fromLocal8Bit(writeTo));
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(recorded.toUtf8());
            QSKIP("golden written; not a comparison");
        }
        QFile golden(goldenPath());
        QVERIFY2(golden.open(QIODevice::ReadOnly),
                 "tests/data/flag-off-switch-golden.txt is missing: capture it "
                 "on 34d76bc8 (see the header of this file)");
        QCOMPARE(recorded, QString::fromUtf8(golden.readAll()));
    }

#ifndef FLAG_OFF_GOLDEN_CAPTURE
    // T-OFF-1. Positive control first: the same probes DO see a pool when
    // live accounts are really on, so their silence below means something.
    void theProbesSeeLiveAccountsWhenTheyAreOn()
    {
        labstest::ScriptedLabsBridge bridge;
        labstest::BridgeScope scope(&bridge);
        labstest::LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, memoryKeyring);
        app.settings()->setLabsConcurrentAccounts(true);
        QVERIFY(app.liveAccountsActive());
        QVERIFY(app.findChild<lightning::accounts::SessionPool *>() != nullptr);
        QVERIFY(app.accountActivity() != nullptr);
        QVERIFY(app.accountList() != nullptr);
        QVERIFY(bridge.totalCalls() > 0);
    }

    void nothingExistsWithTheFlagOff_data()
    {
        QTest::addColumn<bool>("hiddenSwitch");
        QTest::addColumn<bool>("setting");
        QTest::addColumn<bool>("bridgeAvailable");
        QTest::addColumn<bool>("bridgeAsked");
        // hiddenSwitch: the column keeps its name; since 2026-10-20 true is
        // the shipped default (Labs offered, LIGHTNING_LABS_LIVE_ACCOUNTS
        // unset) and false the test override that forces it off.
        QTest::newRow("stored true, forced off") << false << true << true << false;
        QTest::newRow("the default: offered, switch off") << true << false << true << false;
        QTest::newRow("switch on, bridge unavailable") << true << true << false << true;
        QTest::newRow("forced off, switch off") << false << false << true << false;
    }

    // Fails with: the force-off check dropped from applyLiveAccountsLabs
    // (row 1 starts a pool); the order of its conditions changed so the
    // bridge is asked first (rows 1, 2 and 4 count a call); a Labs object
    // constructed unconditionally (the child count differs from "neither").
    void nothingExistsWithTheFlagOff()
    {
        QFETCH(bool, hiddenSwitch);
        QFETCH(bool, setting);
        QFETCH(bool, bridgeAvailable);
        QFETCH(bool, bridgeAsked);
        labstest::ScriptedLabsBridge bridge;
        bridge.setAvailable(bridgeAvailable);
        labstest::BridgeScope scope(&bridge);
        // The setting is stored before the controller exists, as after a
        // restart, and also changed while it runs.
        if (setting) {
            // Labs forced off for the seed: storing the switch must not count
            // as this controller asking the bridge.
            labstest::LabsEnvScope seedEnv(false);
            AppController seed(AppController::MockBackend, false, memoryKeyring);
            seed.settings()->setLabsConcurrentAccounts(true);
        }
        labstest::LabsEnvScope env(hiddenSwitch);
        AppController app(AppController::MockBackend, false, memoryKeyring);
        app.settings()->setLabsConcurrentAccounts(setting);
        SwitchRecord switched;
        QVERIFY(recordSwitch(app, &switched));
        QVERIFY(!app.liveAccountsActive());
        QVERIFY(app.findChild<lightning::accounts::SessionPool *>() == nullptr);
        QVERIFY(app.findChild<AccountActivityModel *>() == nullptr);
        QVERIFY(app.findChild<AccountListModel *>() == nullptr);
        QVERIFY(app.accountActivity() == nullptr);
        QVERIFY(app.accountList() == nullptr);
        QCOMPARE(app.liveAccountCount(), 0);
        QVERIFY(app.liveAccountsStepsForTest().isEmpty());
        QVERIFY(!app.notificationsForTest()->liveAccountsMode());
        if (bridgeAsked) {
            // Only available(), and nothing after its "no".
            QCOMPARE(bridge.totalCalls(), 1);
        } else {
            QCOMPARE(bridge.totalCalls(), 0);
        }
        QCOMPARE(app.settings()->lastLiveAtFor(kAliceId), qint64(0));
        // Exactly the objects a controller that never heard of Labs has.
        int childrenWithout = 0;
        {
            labstest::LabsEnvScope noSwitch(false);
            AppController plain(AppController::MockBackend, false, memoryKeyring);
            SwitchRecord plainSwitch;
            QVERIFY(recordSwitch(plain, &plainSwitch));
            childrenWithout = int(
                plain.findChildren<QObject *>(Qt::FindDirectChildrenOnly).size());
        }
        QCOMPARE(int(app.findChildren<QObject *>(Qt::FindDirectChildrenOnly).size()),
                 childrenWithout);
    }

    // T-OFF-2, differential: every "off" shape switches with exactly the
    // signals of a controller that never heard of Labs. Fails with: any Labs
    // signal or connection made outside the guard (an extra
    // liveAccountCountChanged, a reordered sessionChanged reaction).
    void aSwitchIsTheSameInEveryOffShape()
    {
        QSettings().clear();
        SwitchRecord baseline;
        {
            labstest::LabsEnvScope env(false);
            AppController app(AppController::MockBackend, false, memoryKeyring);
            QVERIFY(recordSwitch(app, &baseline));
        }
        QVERIFY(!baseline.synchronous.isEmpty());
        struct Shape {
            const char *name;
            bool hiddenSwitch;
            bool setting;
            bool bridgeAvailable;
        };
        const Shape shapes[] = {
            { "stored true, forced off", false, true, true },
            { "the default: offered, switch off", true, false, true },
            { "switch on, bridge unavailable", true, true, false },
        };
        for (const Shape &shape : shapes) {
            QSettings().clear();
            labstest::ScriptedLabsBridge bridge;
            bridge.setAvailable(shape.bridgeAvailable);
            labstest::BridgeScope scope(&bridge);
            labstest::LabsEnvScope env(shape.hiddenSwitch);
            AppController app(AppController::MockBackend, false, memoryKeyring);
            app.settings()->setLabsConcurrentAccounts(shape.setting);
            SwitchRecord record;
            QVERIFY(recordSwitch(app, &record));
            QVERIFY2(record.synchronous == baseline.synchronous,
                     shape.name);
            QVERIFY2(record.settled == baseline.settled, shape.name);
        }
    }

    // Every Labs call in AppController.cpp, outside the Labs section itself,
    // has a live-accounts guard on its own line or on an ENCLOSING line (less
    // indented) within the 60 lines above it: a guard of an unrelated block
    // at the same depth does not count, and `if (m_sessionPool->...)` is a
    // call, not a guard. Fails with: any guard (`m_sessionPool &&`,
    // `if (m_sessionPool)`, ...) removed from the shared paths, e.g. in
    // switchToAccount() or failAccountSwitch().
    void everyLabsCallIsBehindTheGuard()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        const QString sectionMarker =
            QStringLiteral("// ── Concurrent live accounts (Labs) ──");
        int sectionStart = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).startsWith(sectionMarker)) {
                sectionStart = i;
                break;
            }
        }
        QVERIFY2(sectionStart > 0, "the Labs section marker is gone");
        const QRegularExpression labsCall(QStringLiteral(
            "m_sessionPool->|m_accountActivity->|m_accountList->|"
            "setLiveAccountsMode\\(|setAccountLabelsShown\\(|"
            "observeRoomUnreadFor\\(|beginLiveAccountSwitch\\(|"
            "claimCrossAccountOpen\\(|removeLiveBackgroundAccount\\(|"
            "noteLiveAccountsStep\\(|cancelParkedOpenFor\\(|"
            "liveAccountLabel\\(|setLastLiveAtFor\\(|rustBridgeApi\\(|"
            "setLiveAccountsLabs\\(|applyLiveAccountsLabs\\(|"
            "startLiveAccounts\\(|stopLiveAccounts\\(|"
            "reopenLiveAccountActivity\\(|abandonLiveRelease\\(|"
            "releaseForForeground\\(|beginQuitRetirement"));
        const QRegularExpression guard(QStringLiteral(
            "if \\(m_sessionPool(?!->)|m_sessionPool &&|m_sessionPool \\?|"
            "if \\(m_accountActivity\\)|!liveForeground\\.isEmpty\\(\\)|"
            "if \\(m_liveAccountsLabsAvailable\\)"));
        const auto indentOf = [](const QString &line) {
            qsizetype n = 0;
            while (n < line.size() && line.at(n) == QLatin1Char(' '))
                ++n;
            return n;
        };
        QStringList unguarded;
        int calls = 0;
        for (int i = 0; i < sectionStart; ++i) {
            const QString line = lines.at(i);
            if (line.trimmed().startsWith(QLatin1String("//"))
                || !labsCall.match(line).hasMatch())
                continue;
            ++calls;
            bool guarded = false;
            for (int j = i; j >= std::max(0, i - 60) && !guarded; --j) {
                // The enclosing function's own lines only: a column-0 brace
                // above is the end of the previous function (or the start of
                // this one), and a guard beyond it guards nothing here (§18
                // round 1, L11).
                if (j < i && (lines.at(j).startsWith(QLatin1Char('}'))
                              || lines.at(j).startsWith(QLatin1Char('{'))))
                    break;
                guarded = guard.match(lines.at(j)).hasMatch()
                    && (j == i || indentOf(lines.at(j)) < indentOf(line));
            }
            if (!guarded)
                unguarded.append(QStringLiteral("%1: %2").arg(i + 1).arg(line.trimmed()));
        }
        QVERIFY2(unguarded.isEmpty(), qPrintable(unguarded.join(QLatin1Char('\n'))));
        // The scan saw the shared paths' Labs calls at all (the constructor,
        // the destructor, the quit, the switch, the removals, the tray).
        QVERIFY2(calls >= 20, qPrintable(QString::number(calls)));
    }
#endif

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(FlagOffParityTest)
#include "FlagOffParityTest.moc"
