// Live accounts (Labs), Phase 1: Settings -> Labs -> "Run several accounts at
// once" (LabsConcurrentAccountsCard.qml).
//
// - Offered by default and OFF (lead decision 2026-10-20, replacing DECISION
//   1's "hidden until Phase 2"): the card is visible with its switch off and
//   says where the "always signed in" choice is; Settings indexes it for
//   search. It hides only when LIGHTNING_LABS_LIVE_ACCOUNTS forces Labs off
//   (a test override), and then even a stored true runs nothing.
// - The master switch writes the setting, the status line, the "Always
//   signed in" heading and the per-account rows follow, and a refused pin is
//   said under its own row and changes nothing.
// - The rows (AccountListModel::rowsSnapshot) and the per-account activity
//   (AccountActivityModel::activityFor) are Q_INVOKABLEs and are read only by
//   handlers, never inside a binding, in every file that reads them.
//
// Mutations that make each case fail are in the WP1-Q notes
// (Tasks/2026-10-16-la1-Q.md).

#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <QPointer>

#include <algorithm>
#include <iterator>

#include "accounts/RustBridgeApi.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "models/AccountActivityModel.h"
#include "models/AccountListModel.h"
#include "support/LabsQmlFixture.h"

namespace {

const QStringList kAccounts = {
    QStringLiteral("@one:hs1.example"),  QStringLiteral("@two:hs2.example"),
    QStringLiteral("@three:hs3.example"), QStringLiteral("@four:hs4.example"),
    QStringLiteral("@five:hs5.example"),
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 900
    visible: true
    color: AppTheme.background
    LabsConcurrentAccountsCard {
        objectName: "card"
        width: 600
    }
}
)QML";

QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    const auto children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *hit = findItem(child, name))
            return hit;
    }
    return nullptr;
}

QString readQml(const QString &name)
{
    QFile f(QStringLiteral(QML_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

class LabsConcurrentAccountsCardTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    labsqml::InertRustBridge m_bridge;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;

    void load(bool hiddenSwitch, int accounts)
    {
        if (hiddenSwitch)
            lightning::accounts::setRustBridgeApiForTest(&m_bridge);
        m_controller = labsqml::makeController(hiddenSwitch,
                                               kAccounts.mid(0, accounts));
        m_controller->switchToAccount(kAccounts.first());
        QTRY_VERIFY(!m_controller->accountSwitching());
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                     m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("labscardscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
    }

    QQuickItem *card() const
    {
        return m_root ? m_root->findChild<QQuickItem *>(QStringLiteral("card"))
                      : nullptr;
    }
    QQuickItem *item(const QString &name) const { return findItem(card(), name); }
    int rowCount() const
    {
        int n = 0;
        for (const QString &uid : kAccounts) {
            if (item(QStringLiteral("labsAccountRow_") + uid))
                ++n;
        }
        return n;
    }

    // A pin makes the pool schedule a start, and what that start does is
    // WP1-S's business (here, with InertRustBridge, a failed create and a
    // backoff; elsewhere a host that starts or pauses). Every state it passes
    // through is a rowsChanged and a rebuilt row. So this waits for the rows
    // to go QUIET — no rowsChanged for 250 ms — rather than for any
    // particular state, and the case holds no row pointer across it.
    void settleRows()
    {
        AccountListModel *rows = m_controller->accountList();
        QVERIFY(rows);
        QSignalSpy spy(rows, &AccountListModel::rowsChanged);
        int seen = -1;
        QElapsedTimer clock;
        clock.start();
        while (spy.count() != seen) {
            seen = spy.count();
            QTest::qWait(250);
            QVERIFY2(clock.elapsed() < 5000,
                     "AccountListModel kept signalling rowsChanged for 5 s "
                     "after one pin: a scheduling storm, not a card fault");
        }
    }

    // AppSwitch never writes its own state; a click is its toggled() signal.
    static void click(QQuickItem *appSwitch)
    {
        QVERIFY(appSwitch);
        QVERIFY(QMetaObject::invokeMethod(appSwitch, "toggled"));
    }

    // Labs on, through the card's own switch.
    void turnLabsOn()
    {
        auto *master = item(QStringLiteral("labsConcurrentAccountsSwitch"));
        QVERIFY(master);
        QVERIFY(!master->property("checked").toBool());
        click(master);
        QVERIFY(m_controller->settings()->labsConcurrentAccounts());
        QTRY_VERIFY(m_controller->liveAccountsActive());
        QTRY_VERIFY(master->property("checked").toBool());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("labs-concurrent-accounts-card-test"));
    }

    void cleanup()
    {
        // Labs off first: the pool is shut down asynchronously, never by a
        // destructor.
        if (m_controller && m_controller->liveAccountsActive()) {
            m_controller->settings()->setLabsConcurrentAccounts(false);
            QTRY_VERIFY(!m_controller->liveAccountsActive());
        }
        delete m_root;
        m_root = nullptr;
        delete m_engine;
        m_engine = nullptr;
        delete m_controller;
        m_controller = nullptr;
        lightning::accounts::setRustBridgeApiForTest(nullptr);
        labsqml::setHiddenSwitch(false);
        QSettings().clear();
    }

    // The default: offered and off. Nothing runs, the switch is reachable,
    // and the card says this is where the always-signed-in accounts are
    // chosen. Mutation: `visible: root.available && <anything off by
    // default>` hides it again.
    void theCardIsOfferedAndOffByDefault()
    {
        load(true, 2);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(m_controller->liveAccountsLabsAvailable());
        QVERIFY(!m_controller->settings()->labsConcurrentAccounts());
        QVERIFY(!m_controller->liveAccountsActive());
        QVERIFY(card()->isVisible());
        auto *master = item(QStringLiteral("labsConcurrentAccountsSwitch"));
        QVERIFY(master);
        QVERIFY(master->isVisible());
        QVERIFY(master->property("enabled").toBool());
        QVERIFY(!master->property("checked").toBool());
        QVERIFY(item(QStringLiteral("labsAlwaysSignedInHint"))->isVisible());
        QVERIFY(!item(QStringLiteral("labsAlwaysSignedInHeading"))->isVisible());
        QCOMPARE(rowCount(), 0);
    }

    // The test override forces Labs off: a stored `true` shows nothing and
    // runs nothing, and the card is gone.
    void theCardHidesOnlyWhenForcedOff()
    {
        load(false, 2);
        if (QTest::currentTestFailed())
            return;
        m_controller->settings()->setLabsConcurrentAccounts(true);
        QCoreApplication::processEvents();
        QVERIFY(!m_controller->liveAccountsLabsAvailable());
        QVERIFY(!m_controller->liveAccountsActive());

        QVERIFY(card());
        QVERIFY(!card()->property("visible").toBool());
        QVERIFY(!card()->isVisible());
        auto *master = item(QStringLiteral("labsConcurrentAccountsSwitch"));
        QVERIFY(master);
        QVERIFY(!master->isVisible());
        QCOMPARE(rowCount(), 0);

        // Settings places it once, without overriding its visibility, and
        // indexes it for search on the master switch.
        const QString settings = readQml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.size() > 100000);
        QCOMPARE(settings.count(QStringLiteral("LabsConcurrentAccountsCard {")), 1);
        QCOMPARE(settings.count(QStringLiteral("LabsConcurrentAccountsCard {}")), 1);
        QCOMPARE(settings.count(QStringLiteral("anchor: \"labsConcurrentAccountsSwitch\"")), 1);
    }

    // With the hidden switch: the master switch writes the setting, Labs
    // comes on, and the status line and the rows follow; off takes them away.
    void theSwitchWritesTheSettingAndTheRowsFollow()
    {
        load(true, 3);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(m_controller->liveAccountsLabsAvailable());
        QVERIFY(card()->isVisible());

        // Off: no rows, no status, no off note.
        QVERIFY(!item(QStringLiteral("labsConcurrentAccountsStatus"))->isVisible());
        QVERIFY(!item(QStringLiteral("labsConcurrentAccountsOffNote"))->isVisible());
        QCOMPARE(rowCount(), 0);
        QCOMPARE(item(QStringLiteral("labsConcurrentAccountsCap"))
                     ->property("text").toString(),
                 QStringLiteral("Up to %1 accounts can keep running.")
                     .arg(m_controller->maxPinnedAccounts()));

        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        auto *status = item(QStringLiteral("labsConcurrentAccountsStatus"));
        QTRY_VERIFY(status->isVisible());
        QTRY_COMPARE(status->property("text").toString(),
                     QStringLiteral("%1 of %2 running")
                         .arg(m_controller->liveAccountCount())
                         .arg(m_controller->maxLiveAccounts()));
        QVERIFY(item(QStringLiteral("labsConcurrentAccountsOffNote"))->isVisible());
        QVERIFY(!item(QStringLiteral("labsConcurrentAccountsUnavailable"))->isVisible());
        // The picker, under its own heading and plain words.
        QTRY_VERIFY(item(QStringLiteral("labsAlwaysSignedInHeading"))->isVisible());
        QVERIFY(item(QStringLiteral("labsAlwaysSignedInIntro"))->isVisible());
        QVERIFY(!item(QStringLiteral("labsAlwaysSignedInHint"))->isVisible());

        // One row per saved account, in the model's order; names are plain.
        QTRY_COMPARE(rowCount(), 3);
        QCOMPARE(m_controller->accountList()->rowsSnapshot().size(), 3);
        for (int i = 0; i < 3; ++i) {
            auto *row = item(QStringLiteral("labsAccountRow_") + kAccounts.at(i));
            QVERIFY(row);
            for (const char *label : { "labsAccountName", "labsAccountUserId" }) {
                auto *text = findItem(row, QString::fromLatin1(label));
                QVERIFY2(text, label);
                QCOMPARE(text->property("textFormat").toInt(), int(Qt::PlainText));
            }
            auto *pin = findItem(row, QStringLiteral("labsKeepRunning_") + kAccounts.at(i));
            QVERIFY(pin);
            QVERIFY(!pin->property("checked").toBool());
        }

        // Off again, through the same switch.
        click(item(QStringLiteral("labsConcurrentAccountsSwitch")));
        QVERIFY(!m_controller->settings()->labsConcurrentAccounts());
        QTRY_VERIFY(!m_controller->liveAccountsActive());
        QTRY_COMPARE(rowCount(), 0);
        QTRY_VERIFY(!status->isVisible());
    }

    // Four pins fit; the fifth is refused, the refusal is said under that
    // row only, and nothing changes. Turning one off clears it.
    void aRefusedPinIsSaidUnderItsRowAndChangesNothing()
    {
        load(true, 5);
        if (QTest::currentTestFailed())
            return;
        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(rowCount(), 5);
        const int cap = m_controller->maxPinnedAccounts();
        QCOMPARE(cap, 4);

        auto pinOf = [this](const QString &uid) {
            return item(QStringLiteral("labsKeepRunning_") + uid);
        };
        auto refusalOf = [this](const QString &uid) {
            return findItem(item(QStringLiteral("labsAccountRow_") + uid),
                            QStringLiteral("labsKeepRunningRefusal"));
        };

        for (int i = 0; i < cap; ++i) {
            const QString uid = kAccounts.at(i);
            click(pinOf(uid));
            QVERIFY(m_controller->settings()->keepRunningFor(uid));
            settleRows();
            if (QTest::currentTestFailed())
                return;
            // The rows are re-read on rowsChanged, so the delegate may be
            // new: every lookup below is fresh.
            QTRY_VERIFY(pinOf(uid) && pinOf(uid)->property("checked").toBool());
            QVERIFY(refusalOf(uid) && !refusalOf(uid)->isVisible());
        }

        const QString fifth = kAccounts.at(cap);
        click(pinOf(fifth));
        QVERIFY(!m_controller->settings()->keepRunningFor(fifth));
        settleRows();
        if (QTest::currentTestFailed())
            return;
        QTRY_VERIFY(refusalOf(fifth) && refusalOf(fifth)->isVisible());
        QCOMPARE(refusalOf(fifth)->property("text").toString(),
                 QStringLiteral("Up to %1 accounts can keep running. Turn one off first.")
                     .arg(cap));
        QVERIFY(pinOf(fifth) && !pinOf(fifth)->property("checked").toBool());
        // Said under that row only.
        for (int i = 0; i < cap; ++i)
            QVERIFY(refusalOf(kAccounts.at(i))
                    && !refusalOf(kAccounts.at(i))->isVisible());

        // Turning one off succeeds and clears the refusal.
        click(pinOf(kAccounts.at(1)));
        QVERIFY(!m_controller->settings()->keepRunningFor(kAccounts.at(1)));
        settleRows();
        if (QTest::currentTestFailed())
            return;
        QTRY_VERIFY(!refusalOf(fifth) || !refusalOf(fifth)->isVisible());
        QTRY_VERIFY(pinOf(kAccounts.at(1))
                    && !pinOf(kAccounts.at(1))->property("checked").toBool());
    }

    // C54-F01: a refusal is about the pin COUNT it was made at. When the count
    // moves by any route other than this card's own switch (a sign-out, a
    // removal, the account menu), the refusal goes. Old code kept it until the
    // card's own switch next succeeded: `refusalText` stayed set and the row
    // still said "Turn one off first" with three pinned.
    void aRefusalGoesWhenThePinCountMovesElsewhere()
    {
        load(true, 5);
        if (QTest::currentTestFailed())
            return;
        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(rowCount(), 5);
        const int cap = m_controller->maxPinnedAccounts();
        for (int i = 0; i < cap; ++i) {
            QVERIFY(m_controller->setAccountKeepRunning(kAccounts.at(i), true).isEmpty());
            settleRows();
            if (QTest::currentTestFailed())
                return;
        }
        const QString fifth = kAccounts.at(cap);
        click(item(QStringLiteral("labsKeepRunning_") + fifth));
        settleRows();
        if (QTest::currentTestFailed())
            return;
        auto refusal = [this, fifth] {
            return findItem(item(QStringLiteral("labsAccountRow_") + fifth),
                            QStringLiteral("labsKeepRunningRefusal"));
        };
        QTRY_VERIFY(refusal() && refusal()->isVisible());

        // Not through the card.
        QVERIFY(m_controller->setAccountKeepRunning(kAccounts.at(0), false).isEmpty());
        settleRows();
        if (QTest::currentTestFailed())
            return;
        QTRY_VERIFY(refusal() && !refusal()->isVisible());
    }

    // C54-D04/D05/F04: a delegate keeps its identity across a refresh (so
    // keyboard focus stays on the same account's switch), the order is the
    // order the card first showed (the model's running-first sort does not
    // move a row under the pointer), and every row reserves its state line so
    // a state appearing moves nothing. Old code: the whole array was replaced
    // on every rowsChanged (every delegate re-created), the order followed the
    // model, and the state label was `visible: text.length > 0`.
    void rowsKeepTheirIdentityOrderAndHeight()
    {
        load(true, 5);
        if (QTest::currentTestFailed())
            return;
        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(rowCount(), 5);

        auto rowOf = [this](const QString &uid) {
            return item(QStringLiteral("labsAccountRow_") + uid);
        };
        // The rows' uids top to bottom, checking on the way that every row
        // has the same height and reserves its state line.
        auto checkGeometry = [&](QStringList *order) {
            // Layout settles on the next polish.
            QTest::qWait(150);
            QList<QPair<qreal, QString>> byY;
            qreal height = -1;
            for (const QString &uid : kAccounts) {
                QQuickItem *row = rowOf(uid);
                QVERIFY(row);
                byY.append({ row->mapToItem(card(), QPointF(0, 0)).y(), uid });
                auto *state = findItem(row, QStringLiteral("labsAccountState"));
                QVERIFY(state);
                QVERIFY2(state->isVisible() && state->height() > 0,
                         "the state line is reserved on every row");
                if (height < 0)
                    height = row->height();
                QCOMPARE(row->height(), height);
            }
            std::sort(byY.begin(), byY.end());
            order->clear();
            for (const auto &entry : byY)
                order->append(entry.second);
        };
        QStringList before;
        checkGeometry(&before);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(before.size(), kAccounts.size());

        const QString last = before.last();
        const QString second = before.at(1);
        const QPointer<QQuickItem> secondRow = rowOf(second);
        const QPointer<QQuickItem> secondPin =
            item(QStringLiteral("labsKeepRunning_") + second);
        QVERIFY(secondRow && secondPin);
        // Pin the LAST row: the model may sort it up; the card must not.
        QVERIFY(m_controller->setAccountKeepRunning(last, true).isEmpty());
        settleRows();
        if (QTest::currentTestFailed())
            return;
        QVERIFY(secondRow);
        QVERIFY(secondPin);
        QCOMPARE(rowOf(second), secondRow.data());
        QStringList after;
        checkGeometry(&after);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(after, before);

        // The model's running-first sort cannot be provoked here (nothing
        // reaches Running over the inert bridge), so hand the card a snapshot
        // in the REVERSED order: it must keep the order it first showed.
        QVariantList reversed;
        for (int i = kAccounts.size() - 1; i >= 0; --i) {
            QVariantMap row;
            row.insert(QStringLiteral("userId"), kAccounts.at(i));
            row.insert(QStringLiteral("displayName"), QString());
            row.insert(QStringLiteral("keepRunning"), i == kAccounts.size() - 1);
            row.insert(QStringLiteral("liveState"), 0);
            reversed.append(row);
        }
        QVERIFY(QMetaObject::invokeMethod(card(), "syncRowModel",
                                          Q_ARG(QVariant, QVariant(reversed))));
        QStringList afterReorder;
        checkGeometry(&afterReorder);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(afterReorder, before);
        QCOMPARE(rowOf(second), secondRow.data());
    }

    // Feeds the card a snapshot the model cannot produce offscreen (a revoked
    // or locked account), through the card's own syncRowModel().
    void feed(const QList<int> &states)
    {
        QVariantList snapshot;
        for (int i = 0; i < states.size(); ++i) {
            QVariantMap row;
            row.insert(QStringLiteral("userId"), kAccounts.at(i));
            row.insert(QStringLiteral("displayName"), QStringLiteral("Account %1").arg(i));
            row.insert(QStringLiteral("keepRunning"), true);
            row.insert(QStringLiteral("liveState"), states.at(i));
            snapshot.append(row);
        }
        QVERIFY(QMetaObject::invokeMethod(card(), "syncRowModel",
                                          Q_ARG(QVariant, QVariant(snapshot))));
        QTest::qWait(150);
    }

    QColor themeColor(const char *token)
    {
        QQmlComponent c(m_engine);
        c.setData(QByteArray("import QtQuick\nimport MatrixClient\n"
                             "QtObject { property color c: AppTheme.")
                      + token + " }",
                  QUrl(QStringLiteral("tok.qml")));
        QScopedPointer<QObject> o(c.create());
        return o ? o->property("c").value<QColor>() : QColor();
    }

    // C54-D02/D07: an unhealthy state is said in the danger ink and a sign-in
    // that is gone offers the way back in place; a locked keyring is not a
    // missing sign-in (CLAUDE.md s.6) and offers none; a healthy row has
    // neither; and a state never changes a row's height. Real delegates fed a
    // snapshot per state. Old code: muted ink for every state, no button.
    void anUnhealthyStateIsDangerInkWithASignInOnlyWhereItHelps()
    {
        load(true, 5);
        if (QTest::currentTestFailed())
            return;
        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(rowCount(), 5);
        using S = AccountActivityModel;
        feed({ S::Running, S::NeedsSignIn, S::KeyringLocked, S::OpenToFinishSetup,
               S::StoreBusy });
        if (QTest::currentTestFailed())
            return;
        const QColor danger = themeColor("stormDanger");
        const QColor muted = themeColor("stormTextMuted");
        QVERIFY(danger.isValid() && muted.isValid() && danger != muted);
        struct Expect { int index; bool danger; bool signIn; };
        const Expect expected[] = { { 0, false, false }, { 1, true, true },
                                    { 2, true, false }, { 3, true, true },
                                    { 4, true, false } };
        qreal height = -1;
        for (const Expect &e : expected) {
            const QString uid = kAccounts.at(e.index);
            QQuickItem *row = item(QStringLiteral("labsAccountRow_") + uid);
            QVERIFY(row);
            auto *state = findItem(row, QStringLiteral("labsAccountState"));
            QVERIFY(state);
            QVERIFY2(!state->property("text").toString().isEmpty(), qPrintable(uid));
            QCOMPARE(state->property("color").value<QColor>(),
                     e.danger ? danger : muted);
            auto *button = findItem(row, QStringLiteral("labsSignInAgain_") + uid);
            QVERIFY(button);
            QCOMPARE(button->isVisible(), e.signIn);
            if (height < 0)
                height = row->height();
            QCOMPARE(row->height(), height);
        }
    }

    // C54-D04: after a keyboard toggle the same account's switch still has
    // focus (the delegate is not re-created), so Space can toggle it back.
    void keyboardFocusStaysOnTheSwitchAfterAToggle()
    {
        load(true, 3);
        if (QTest::currentTestFailed())
            return;
        turnLabsOn();
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(rowCount(), 3);
        const QString uid = kAccounts.at(1);
        QQuickItem *pin = item(QStringLiteral("labsKeepRunning_") + uid);
        QVERIFY(pin);
        pin->forceActiveFocus();
        QVERIFY(pin->hasActiveFocus());
        const QPointer<QQuickItem> before = pin;
        click(pin);
        settleRows();
        if (QTest::currentTestFailed())
            return;
        QQuickItem *after = item(QStringLiteral("labsKeepRunning_") + uid);
        QVERIFY(after);
        QCOMPARE(after, before.data());
        QVERIFY(after->hasActiveFocus());
    }

    // The two Q_INVOKABLE reads happen only in handler functions
    // (refresh*() / _read()), never in a binding or in a function a binding
    // calls. Counted per file, so a sweep that silently reads one file fewer
    // fails.
    void rowsAndActivityAreReadOnlyByHandlers()
    {
        static const QRegularExpression functionDecl(
            QStringLiteral("\\bfunction\\s+(\\w+)\\s*\\("));
        // A property binding or an assignment-free `name: value` line.
        static const QRegularExpression bindingLine(
            QStringLiteral("^\\s*(?:readonly\\s+)?(?:property\\s+[\\w.<>]+\\s+)?"
                           "[A-Za-z_][\\w.]*\\s*:(?!:)"));
        static const QRegularExpression handlerName(
            QStringLiteral("^(refresh\\w*|_read)$"));

        struct Expect { const char *file; const char *call; int count; };
        const Expect expected[] = {
            { "AccountMenu.qml", "rowsSnapshot(", 1 },
            { "QuickSwitcher.qml", "rowsSnapshot(", 1 },
            { "LoginScreen.qml", "rowsSnapshot(", 1 },
            { "LabsConcurrentAccountsCard.qml", "rowsSnapshot(", 1 },
            { "AccountActivityFeed.qml", "activityFor(", 1 },
            // Read nowhere else among the account surfaces.
            { "AccountMenu.qml", "activityFor(", 0 },
            { "QuickSwitcher.qml", "activityFor(", 0 },
            { "IdentityCard.qml", "activityFor(", 0 },
            { "SpacesRail.qml", "activityFor(", 0 },
            { "SpacesRail.qml", "rowsSnapshot(", 0 },
            { "IdentityCard.qml", "rowsSnapshot(", 0 },
        };

        int filesRead = 0;
        QStringList offenders;
        for (const Expect &e : expected) {
            const QString src = readQml(QString::fromLatin1(e.file));
            QVERIFY2(!src.isEmpty(), e.file);
            ++filesRead;
            const QStringList lines = src.split(QLatin1Char('\n'));
            int found = 0;
            for (int i = 0; i < lines.size(); ++i) {
                const QString line = lines.at(i);
                if (line.trimmed().startsWith(QLatin1String("//")))
                    continue;
                if (!line.contains(QLatin1String(e.call)))
                    continue;
                ++found;
                // Walk back to whatever this expression belongs to.
                QString owner;
                for (int j = i; j >= 0; --j) {
                    const QString l = lines.at(j);
                    if (l.trimmed().startsWith(QLatin1String("//")))
                        continue;
                    const auto fn = functionDecl.match(l);
                    if (fn.hasMatch()) {
                        owner = fn.captured(1);
                        break;
                    }
                    if (bindingLine.match(l).hasMatch()) {
                        owner = QStringLiteral("<binding>");
                        break;
                    }
                }
                if (!handlerName.match(owner).hasMatch()) {
                    offenders << QStringLiteral("%1:%2 %3 (in %4)")
                                     .arg(QString::fromLatin1(e.file))
                                     .arg(i + 1)
                                     .arg(line.trimmed(),
                                          owner.isEmpty() ? QStringLiteral("?")
                                                          : owner);
                }
            }
            QVERIFY2(found == e.count,
                     qPrintable(QStringLiteral("%1: %2 read %3 time(s), "
                                               "expected %4")
                                    .arg(QString::fromLatin1(e.file),
                                         QString::fromLatin1(e.call))
                                    .arg(found).arg(e.count)));
        }
        QCOMPARE(filesRead, int(std::size(expected)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("a Q_INVOKABLE read outside a "
                                           "handler:\n  ")
                                + offenders.join(QStringLiteral("\n  "))));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    LabsConcurrentAccountsCardTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "LabsConcurrentAccountsCardTest.moc"
