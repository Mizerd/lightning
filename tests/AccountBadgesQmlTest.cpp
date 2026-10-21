// Live accounts (Labs), Phase 1: the per-account activity badges on the
// account surfaces — the switcher rows (AccountMenu + IdentityCard), the quick
// switcher's "Switch to" entries and the rail's account tile.
//
// What is held here:
// - the ONE badge rule (AccountActivityBadge): a number only for mentions,
//   "N+" for a lower bound, a dot otherwise, nothing unless Running, nothing
//   on the active row;
// - the feed (AccountActivityFeed) copies AccountActivityModel's answers into
//   an unbound map on the model's signals: one read per change, and a change
//   the model did not signal is never picked up by a binding;
// - the row badge and the rail dot are placed geometrically: inside their
//   row/tile, clear of the text, the remove button and the presence dot;
// - with Labs off nothing changes (the parity case), and with Labs on the
//   badges follow the model while the menu is open.
//
// Mutations that make each case fail are in the WP1-Q notes
// (Tasks/2026-10-16-la1-Q.md).

#include <QtTest/QtTest>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QHash>
#include <QGuiApplication>
#include <QJSValue>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QStyleHints>
#include <QTemporaryDir>

#include <cmath>
#include <memory>

#include "accounts/RustBridgeApi.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "models/AccountListModel.h"
#include "support/LabsQmlFixture.h"

namespace {

const QString kAlice = QStringLiteral("@alice:hs1.example");
const QString kBob = QStringLiteral("@bob:hs2.example");
const QString kCarol = QStringLiteral("@carol:hs3.example");

// Any epoch at or above the row's; nothing retires a host in these suites.
constexpr quint64 kEpoch = 1;

QRectF sceneRect(const QQuickItem *item)
{
    return QRectF(item->mapToScene(QPointF(0, 0)), item->size());
}

// Repeater and layout children are found through the item tree, not the
// QObject tree.
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

QVariantList toList(const QVariant &v)
{
    if (v.canConvert<QJSValue>())
        return v.value<QJSValue>().toVariant().toList();
    return v.toList();
}

bool isNullish(const QVariant &v)
{
    if (!v.isValid() || v.isNull())
        return true;
    if (v.canConvert<QJSValue>()) {
        const QJSValue js = v.value<QJSValue>();
        return js.isNull() || js.isUndefined();
    }
    return false;
}

QStringList userIdsOf(const QVariantList &rows)
{
    QStringList ids;
    for (const QVariant &row : rows)
        ids << row.toMap().value(QStringLiteral("userId")).toString();
    return ids;
}

// The surfaces under test, in one window, against the real `app`.
const char *kSurfaces = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 1000
    height: 760
    visible: true
    color: AppTheme.background

    SpacesRail {
        objectName: "rail"
        x: 0
        y: 0
        width: 68
        height: 760
    }
    AccountMenu {
        id: menu
        objectName: "menu"
        x: 120
        y: 300
    }
    QuickSwitcher { id: switcher; objectName: "switcher" }
    function openMenu() { menu.open() }
    function openCommand() { switcher.openCommandMode() }
}
)QML";

} // namespace

class AccountBadgesQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    labsqml::InertRustBridge m_bridge;

    // One scene over one controller; torn down per case.
    struct Surfaces {
        AppController *controller = nullptr;
        QQmlEngine *engine = nullptr;
        QObject *root = nullptr;

        ~Surfaces()
        {
            delete root;
            delete engine;
            delete controller;
        }
        QObject *find(const QString &name) const
        {
            return root->findChild<QObject *>(name);
        }
        // The scene's own menu's list. SpacesRail carries a second
        // AccountMenu (railAccountMenu) whose list has the same objectName
        // and the same count, and it comes first in a window-wide
        // depth-first search — so every lookup is scoped to "menu".
        QQuickItem *menuList() const
        {
            QObject *menu = find(QStringLiteral("menu"));
            return menu ? menu->findChild<QQuickItem *>(
                              QStringLiteral("identityCardList"))
                        : nullptr;
        }
        QQuickItem *card(const QString &userId) const
        {
            auto *list = menuList();
            if (!list)
                return nullptr;
            const int count = list->property("count").toInt();
            for (int i = 0; i < count; ++i) {
                QQuickItem *item = nullptr;
                QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, item),
                                          Q_ARG(int, i));
                if (item && item->objectName()
                                == QStringLiteral("identityCard_") + userId)
                    return item;
            }
            return nullptr;
        }
        QQuickItem *badgeOf(const QString &userId) const
        {
            return findItem(card(userId),
                            QStringLiteral("identityCardActivityBadge"));
        }
    };

    std::unique_ptr<Surfaces> makeSurfaces(AppController *controller)
    {
        auto s = std::make_unique<Surfaces>();
        s->controller = controller;
        s->engine = new QQmlEngine;
        s->engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                      controller);
        QQmlComponent component(s->engine);
        component.setData(QByteArray(kSurfaces),
                          QUrl(QStringLiteral("accountbadgesscene.qml")));
        s->root = component.create();
        if (!s->root) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        auto *window = qobject_cast<QQuickWindow *>(s->root);
        if (!window || !QTest::qWaitForWindowExposed(window))
            return nullptr;
        // Park the pointer clear of every row: a hovered row hides its badge.
        QTest::mouseMove(window, QPoint(window->width() - 4, 4));
        return s;
    }

    static void openMenu(Surfaces &s, int expectedRows)
    {
        QMetaObject::invokeMethod(s.root, "openMenu");
        auto *menu = s.find(QStringLiteral("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        auto *list = s.menuList();
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), expectedRows);
        // The list's height follows its count through the popover's layout,
        // which runs at the next polish: until then no row past the first
        // has a delegate (integration).
        QTRY_VERIFY(list->height() > 0);
        // Every "no badge" check below is taken on THIS list; on a list
        // nobody opened, isVisible() is false for any badge rule at all.
        QTRY_VERIFY(list->isVisible());
        // And it is not the rail's (SpacesRail's own AccountMenu keeps the
        // component's objectName).
        if (QObject *railMenu = s.find(QStringLiteral("accountSwitcherPopover"))) {
            QVERIFY(railMenu != menu);
            QVERIFY(railMenu->findChild<QQuickItem *>(
                        QStringLiteral("identityCardList")) != list);
        }
    }

    // The quick switcher's "Switch to <uid>" entry, filtered to the top.
    static QQuickItem *switchEntry(Surfaces &s, const QString &userId)
    {
        auto *switcher = s.find(QStringLiteral("switcher"));
        if (!switcher->property("opened").toBool()) {
            QMetaObject::invokeMethod(s.root, "openCommand");
            // onOpened clears the query, so the text goes in after it.
            if (!QTest::qWaitFor([switcher] {
                    return switcher->property("opened").toBool(); }))
                return nullptr;
        }
        auto *field = switcher->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherField"));
        if (!field)
            return nullptr;
        field->setProperty("text", userId);
        auto *list = switcher->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        if (!list)
            return nullptr;
        // Filtered at once, laid out at the next polish: the entry's
        // delegate exists only once the list has a height (integration).
        QQuickItem *item = nullptr;
        QTest::qWaitFor([list, &item] {
            QMetaObject::invokeMethod(list, "forceLayout");
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, 0));
            return item != nullptr;
        }, 2000);
        return item;
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
            QStringLiteral("account-badges-qml-test"));
        QGuiApplication::styleHints()->setUseHoverEffects(true);
    }

    void cleanup()
    {
        lightning::accounts::setRustBridgeApiForTest(nullptr);
        labsqml::setHiddenSwitch(false);
        QSettings().clear();
    }

    // ── The rule ──────────────────────────────────────────────────────────

    void theBadgeRuleHoldsForEveryState_data()
    {
        QTest::addColumn<int>("liveState");
        QTest::addColumn<int>("mentions");
        QTest::addColumn<bool>("lowerBound");
        QTest::addColumn<bool>("hasUnread");
        QTest::addColumn<bool>("activeRow");
        QTest::addColumn<bool>("shown");
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("dotOnly");

        const int running = AccountActivityModel::Running;
        QTest::newRow("mentions are a number")
            << running << 3 << false << true << false << true << "@3" << false;
        QTest::newRow("a lower bound reads N+")
            << running << 3 << true << true << false << true << "@3+" << false;
        QTest::newRow("capped at 99+")
            << running << 120 << false << true << false << true << "@99+" << false;
        QTest::newRow("capped lower bound is still 99+")
            << running << 120 << true << true << false << true << "@99+" << false;
        QTest::newRow("unread without a mention is a dot")
            << running << 0 << false << true << false << true << "" << true;
        QTest::newRow("a lower bound of nothing is a dot")
            << running << 0 << true << true << false << true << "" << true;
        QTest::newRow("nothing new is nothing")
            << running << 0 << false << false << false << false << "" << false;
        QTest::newRow("the active row never")
            << running << 3 << false << true << true << false << "" << false;
        QTest::newRow("a paused account never")
            << int(AccountActivityModel::Paused) << 3 << false << true << false
            << false << "" << false;
        QTest::newRow("a starting account never")
            << int(AccountActivityModel::Starting) << 0 << false << true << false
            << false << "" << false;
        QTest::newRow("a stopped account never")
            << int(AccountActivityModel::Stopped) << 2 << false << true << false
            << false << "" << false;
        QTest::newRow("Active state never")
            << int(AccountActivityModel::Active) << 3 << false << true << false
            << false << "" << false;
        QTest::newRow("not live (Labs off) never")
            << int(AccountActivityModel::NotLive) << 3 << false << true << false
            << false << "" << false;
    }

    void theBadgeRuleHoldsForEveryState()
    {
        QFETCH(int, liveState);
        QFETCH(int, mentions);
        QFETCH(bool, lowerBound);
        QFETCH(bool, hasUnread);
        QFETCH(bool, activeRow);
        QFETCH(bool, shown);
        QFETCH(QString, text);
        QFETCH(bool, dotOnly);

        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 120; height: 40
    property alias badge: badge
    AccountActivityBadge { id: badge }
}
)QML"), QUrl(QStringLiteral("badgerule.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *badge = qvariant_cast<QObject *>(scene->property("badge"));
        QVERIFY(badge);
        badge->setProperty("liveState", liveState);
        badge->setProperty("mentionCount", mentions);
        badge->setProperty("mentionsLowerBound", lowerBound);
        badge->setProperty("hasUnread", hasUnread);
        badge->setProperty("activeAccount", activeRow);

        QCOMPARE(badge->property("shown").toBool(), shown);
        QCOMPARE(badge->property("visible").toBool(), shown);
        if (!shown) {
            QCOMPARE(badge->property("activityLabel").toString(), QString());
            return;
        }
        QCOMPARE(badge->property("text").toString(), text);
        QCOMPARE(badge->property("dotOnly").toBool(), dotOnly);
        // A number is a mention (the mention colour); a dot is not.
        QCOMPARE(badge->property("mention").toBool(), !dotOnly);
        const QString said = badge->property("activityLabel").toString();
        if (dotOnly) {
            QCOMPARE(said, QStringLiteral("New messages"));
        } else {
            QVERIFY2(said.contains(QString::number(mentions)), qPrintable(said));
            QCOMPARE(said.startsWith(QStringLiteral("At least")), lowerBound);
        }
    }

    // Every LiveState has words, and NotLive has none (so a row outside Labs
    // gains no accessible text).
    void everyLiveStateHasItsWords()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray("import MatrixClient\nAccountActivityBadge {}\n"),
                  QUrl(QStringLiteral("statewords.qml")));
        QScopedPointer<QObject> badge(c.create());
        QVERIFY2(badge, qPrintable(c.errorString()));
        const QList<QPair<int, QString>> expected = {
            { AccountActivityModel::NotLive, QString() },
            { AccountActivityModel::Active, QStringLiteral("Active") },
            { AccountActivityModel::Starting, QStringLiteral("Starting") },
            { AccountActivityModel::Running, QStringLiteral("Running") },
            { AccountActivityModel::Paused, QStringLiteral("Paused (limit reached)") },
            { AccountActivityModel::NeedsSignIn, QStringLiteral("Needs sign-in") },
            { AccountActivityModel::KeyringLocked, QStringLiteral("Keyring locked") },
            { AccountActivityModel::Stopped, QStringLiteral("Stopped (error)") },
            { AccountActivityModel::StoreBusy,
              QStringLiteral("Store busy (restart to reopen)") },
            { AccountActivityModel::EncryptionDataMissing,
              QStringLiteral("Encryption data missing") },
            { AccountActivityModel::OpenToFinishSetup,
              QStringLiteral("Open to finish setup") },
            { AccountActivityModel::Stopping, QStringLiteral("Stopping") },
        };
        // Every enumerator is listed: the count is the enum's.
        const QMetaEnum meta = QMetaEnum::fromType<AccountActivityModel::LiveState>();
        QCOMPARE(expected.size(), meta.keyCount());
        for (const auto &[state, words] : expected) {
            badge->setProperty("liveState", state);
            QCOMPARE(badge->property("stateLabel").toString(), words);
        }
    }

    // ── The feed ──────────────────────────────────────────────────────────

    // One read per signalled change; a change the model did not signal is
    // not seen (no binding reaches into the model); no source, no reads.
    void theFeedReadsOncePerSignalAndIsNeverABinding()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    id: scene
    property int reads: 0
    property var table: ({})
    property bool useSource: true
    QtObject {
        id: fakeModel
        signal summaryChanged(string userId)
        signal liveStateChanged(string userId)
        function activityFor(uid) {
            scene.reads++
            return scene.table[uid] || ({ liveState: 0, mentionCount: 0,
                                          hasUnread: false,
                                          mentionsLowerBound: false,
                                          invites: 0 })
        }
    }
    AccountActivityFeed {
        id: feed
        source: scene.useSource ? fakeModel : null
        userIds: ["@a:x", "@b:x"]
    }
    readonly property int bMentions: feed.entryFor("@b:x").mentionCount
    readonly property int bState: feed.entryFor("@b:x").liveState
    readonly property bool activityIsNull: feed.activity === null
    function setB(mentions, state) {
        var t = {}
        for (var k in scene.table) t[k] = scene.table[k]
        t["@b:x"] = { liveState: state, mentionCount: mentions,
                      hasUnread: mentions > 0, mentionsLowerBound: false,
                      invites: 0 }
        scene.table = t
    }
    function signalSummary(uid) { fakeModel.summaryChanged(uid) }
    function signalState(uid) { fakeModel.liveStateChanged(uid) }
}
)QML"), QUrl(QStringLiteral("feedscene.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        // Let any deferred refresh run, then take the baseline.
        QTest::qWait(0);
        QCoreApplication::processEvents();
        QVERIFY(scene->property("reads").toInt() >= 2); // both accounts read
        const int running = AccountActivityModel::Running;

        int baseline = scene->property("reads").toInt();
        QMetaObject::invokeMethod(scene.data(), "setB", Q_ARG(QVariant, 2),
                                  Q_ARG(QVariant, running));
        // The model changed and said nothing: the surface must not see it.
        QCOMPARE(scene->property("bMentions").toInt(), 0);
        QCOMPARE(scene->property("reads").toInt(), baseline);

        QMetaObject::invokeMethod(scene.data(), "signalSummary",
                                  Q_ARG(QVariant, QStringLiteral("@b:x")));
        QCOMPARE(scene->property("bMentions").toInt(), 2);
        QCOMPARE(scene->property("bState").toInt(), running);
        QCOMPARE(scene->property("reads").toInt(), baseline + 1);

        baseline = scene->property("reads").toInt();
        QMetaObject::invokeMethod(scene.data(), "setB", Q_ARG(QVariant, 5),
                                  Q_ARG(QVariant, running));
        QCOMPARE(scene->property("bMentions").toInt(), 2);
        QMetaObject::invokeMethod(scene.data(), "signalState",
                                  Q_ARG(QVariant, QStringLiteral("@b:x")));
        QCOMPARE(scene->property("bMentions").toInt(), 5);
        QCOMPARE(scene->property("reads").toInt(), baseline + 1);

        // Losing the source (Labs turned off) empties the map.
        scene->setProperty("useSource", false);
        QTRY_VERIFY(scene->property("activityIsNull").toBool());
        QCOMPARE(scene->property("bMentions").toInt(), 0);
        QCOMPARE(scene->property("bState").toInt(), 0);
    }

    void aFeedWithNoSourceReadsNothing()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    id: scene
    property int reads: 0
    QtObject {
        id: fakeModel
        signal summaryChanged(string userId)
        function activityFor(uid) { scene.reads++; return ({}) }
    }
    AccountActivityFeed {
        id: feed
        source: null
        userIds: ["@a:x", "@b:x"]
    }
    readonly property bool activityIsNull: feed.activity === null
    function poke() { fakeModel.summaryChanged("@a:x") }
}
)QML"), QUrl(QStringLiteral("nosourcefeed.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        QTest::qWait(0);
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(scene.data(), "poke");
        QCOMPARE(scene->property("reads").toInt(), 0);
        QVERIFY(scene->property("activityIsNull").toBool());
    }

    // ── Geometry ──────────────────────────────────────────────────────────

    // The row badge sits inside its row, clear of the name, the id and the
    // remove button; on the active row and outside Labs it is not drawn.
    void theRowBadgeSitsInsideItsRowClearOfTheText()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import QtQuick.Window
import MatrixClient
Window {
    width: 420; height: 300; visible: true
    property alias running: running
    property alias activeRow: activeRow
    property alias legacy: legacy
    // Away from the window's corner, and the pointer is parked outside the
    // rows below: a hovered row hides its badge for the remove button.
    Column {
        x: 20
        y: 60
        width: 352
        IdentityCard {
            id: running
            width: parent.width
            displayName: "A display name long enough to need eliding here"
            userId: "@someone.with.a.long.localpart:a-long-homeserver.example"
            liveState: AccountActivityModel.Running
            mentionCount: 7
        }
        IdentityCard {
            id: activeRow
            width: parent.width
            active: true
            displayName: "Active"
            userId: "@active:hs.example"
            liveState: AccountActivityModel.Running
            mentionCount: 7
        }
        IdentityCard {
            id: legacy
            width: parent.width
            displayName: "Labs off"
            userId: "@off:hs.example"
            mentionCount: 7
            activityDot: true
        }
    }
}
)QML"), QUrl(QStringLiteral("rowbadge.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(scene.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::mouseMove(window, QPoint(window->width() - 4, window->height() - 4));

        auto *running = qvariant_cast<QQuickItem *>(scene->property("running"));
        auto *activeRow = qvariant_cast<QQuickItem *>(scene->property("activeRow"));
        auto *legacy = qvariant_cast<QQuickItem *>(scene->property("legacy"));
        QVERIFY(running && activeRow && legacy);

        auto *badge = findItem(running, QStringLiteral("identityCardActivityBadge"));
        QVERIFY(badge);
        QTRY_VERIFY(badge->isVisible());
        QCOMPARE(badge->property("text").toString(), QStringLiteral("@7"));
        QTRY_VERIFY(badge->width() > 0 && badge->height() > 0);

        const QRectF row = sceneRect(running);
        const QRectF mark = sceneRect(badge);
        QVERIFY2(row.contains(mark),
                 qPrintable(QStringLiteral("badge %1,%2 %3x%4 is outside its "
                                           "row %5,%6 %7x%8")
                                .arg(mark.x()).arg(mark.y()).arg(mark.width())
                                .arg(mark.height()).arg(row.x()).arg(row.y())
                                .arg(row.width()).arg(row.height())));
        for (const char *name : { "identityCardName", "identityCardUserId",
                                  "identityCardRemoveButton" }) {
            auto *other = findItem(running, QString::fromLatin1(name));
            QVERIFY2(other, name);
            QVERIFY2(!sceneRect(other).intersects(mark),
                     qPrintable(QStringLiteral("the badge overlaps %1")
                                    .arg(QString::fromLatin1(name))));
        }

        // The active row: never.
        auto *activeBadge = findItem(activeRow,
                                     QStringLiteral("identityCardActivityBadge"));
        QVERIFY(activeBadge);
        QVERIFY(!activeBadge->isVisible());

        // Outside Labs (liveState 0): the counts are ignored, and the row's
        // accessible description is what it always was.
        auto *legacyBadge = findItem(legacy,
                                     QStringLiteral("identityCardActivityBadge"));
        QVERIFY(legacyBadge);
        QVERIFY(!legacyBadge->isVisible());
        QQmlExpression legacyDescription(qmlContext(legacy), legacy,
                                         QStringLiteral("Accessible.description"));
        QCOMPARE(legacyDescription.evaluate().toString(), QString());

        // In Labs the description carries the state and the badge's words.
        QQmlExpression runningDescription(qmlContext(running), running,
                                          QStringLiteral("Accessible.description"));
        const QString said = runningDescription.evaluate().toString();
        QVERIFY2(said.contains(QStringLiteral("Running"))
                     && said.contains(QStringLiteral("7")),
                 qPrintable(said));
    }

    // The rail's "new messages on another account" dot sits ON the avatar's
    // rim at the top-right — the presence dot's own rule (centre at avatarR +
    // dotR - ring along the 45° diagonal) mirrored — clear of the presence dot
    // and its connection fallback, and inside the rail, at two interface
    // scales. Containment in the tile's square alone is not the property: a
    // dot in the tile's empty corner passes it while barely touching the
    // avatar.
    void theRailDotSitsOnTheAvatarRimAwayFromThePresenceDot()
    {
        auto *controller = labsqml::makeController(false, { kAlice, kBob });
        controller->switchToAccount(kAlice);
        QTRY_VERIFY(!controller->accountSwitching());
        auto s = makeSurfaces(controller);
        QVERIFY(s);

        auto *rail = s->root->findChild<QQuickItem *>(QStringLiteral("rail"));
        auto *tile = s->root->findChild<QQuickItem *>(
            QStringLiteral("railAccountTile"));
        auto *dot = s->root->findChild<QQuickItem *>(
            QStringLiteral("railOtherAccountsDot"));
        auto *presence = s->root->findChild<QQuickItem *>(
            QStringLiteral("railSelfPresenceDot"));
        auto *connection = s->root->findChild<QQuickItem *>(
            QStringLiteral("railConnectionDot"));
        // The cog's attention badge is the nearest mark above the tile.
        auto *cogBadge = s->root->findChild<QQuickItem *>(
            QStringLiteral("railSettingsAlertBadge"));
        QVERIFY(rail && tile && dot && presence && connection && cogBadge);
        QTRY_VERIFY(tile->isVisible());

        QQmlExpression readScale(qmlContext(tile), tile,
                                 QStringLiteral("AppTheme.textScale"));
        const qreal original = readScale.evaluate().toReal();
        for (const qreal factor : { 1.0, 1.5 }) {
            QQmlExpression setScale(qmlContext(tile), tile,
                                    QStringLiteral("AppTheme.textScale = %1")
                                        .arg(factor));
            setScale.evaluate();
            QTest::qWait(30);
            QTRY_VERIFY(dot->width() > 0);
            // Let the rail's layout settle at the new scale before measuring.
            QTest::qWait(30);
            const QRectF t = sceneRect(tile);
            const QRectF d = sceneRect(dot);
            // The avatar fills the tile and is a circle.
            const qreal avatarR = t.width() / 2;
            const qreal dotR = d.width() / 2;
            QQmlExpression readRing(qmlContext(dot), dot,
                                    QStringLiteral("border.width"));
            const qreal ring = readRing.evaluate().toReal();
            QVERIFY(ring > 0);
            const QPointF dc = d.center() - t.center();
            const qreal dist = std::hypot(dc.x(), dc.y());
            const qreal onRim = avatarR + dotR - ring;
            QVERIFY2(std::abs(dist - onRim) <= 0.75,
                     qPrintable(QStringLiteral("scale %1: the dot's centre is "
                                               "%2 px from the avatar's centre, "
                                               "the rim rule puts it at %3 "
                                               "(avatar r %4, dot r %5)")
                                    .arg(factor).arg(dist).arg(onRim)
                                    .arg(avatarR).arg(dotR)));
            QVERIFY2(dc.x() > 0 && dc.y() < 0
                         && std::abs(std::abs(dc.x()) - std::abs(dc.y())) <= 0.75,
                     qPrintable(QStringLiteral("scale %1: the dot is not on "
                                               "the top-right diagonal "
                                               "(%2, %3)")
                                    .arg(factor).arg(dc.x()).arg(dc.y())));
            const QRectF r = sceneRect(rail);
            QVERIFY2(r.contains(d),
                     qPrintable(QStringLiteral("scale %1: the dot %2,%3 %4x%5 "
                                               "leaves the rail")
                                    .arg(factor).arg(d.x()).arg(d.y())
                                    .arg(d.width()).arg(d.height())));
            QVERIFY2(!d.intersects(sceneRect(presence)),
                     qPrintable(QStringLiteral("scale %1: the dot overlaps the "
                                               "presence dot").arg(factor)));
            QVERIFY2(!d.intersects(sceneRect(connection)),
                     qPrintable(QStringLiteral("scale %1: the dot overlaps the "
                                               "connection dot").arg(factor)));
            QVERIFY2(!d.intersects(sceneRect(cogBadge)),
                     qPrintable(QStringLiteral("scale %1: the dot overlaps the "
                                               "cog's attention badge")
                                    .arg(factor)));
        }
        QQmlExpression restore(qmlContext(tile), tile,
                               QStringLiteral("AppTheme.textScale = %1")
                                   .arg(original));
        restore.evaluate();
    }

    // ── Parity: Labs off ──────────────────────────────────────────────────

    // A stored `true` with Labs forced off (the test override) does nothing, and every
    // surface reads AccountManager's list and renders no badge, exactly as
    // before Labs existed.
    void withLabsOffEverySurfaceIsAsBefore()
    {
        auto *controller = labsqml::makeController(false, { kAlice, kBob });
        controller->settings()->setLabsConcurrentAccounts(true);
        controller->switchToAccount(kAlice);
        QTRY_VERIFY(!controller->accountSwitching());
        QVERIFY(!controller->liveAccountsLabsAvailable());
        QVERIFY(!controller->liveAccountsActive());
        QVERIFY(controller->accountActivity() == nullptr);
        QVERIFY(controller->accountList() == nullptr);

        auto s = makeSurfaces(controller);
        QVERIFY(s);
        openMenu(*s, 2);
        if (QTest::currentTestFailed())
            return;
        auto *menu = s->find(QStringLiteral("menu"));

        // The rows are AccountManager's, active first.
        QVariantList expected = controller->accounts()->accounts();
        QCOMPARE(expected.size(), 2);
        for (int i = 0; i < expected.size(); ++i) {
            if (expected.at(i).toMap().value(QStringLiteral("isActive")).toBool()) {
                expected.move(i, 0);
                break;
            }
        }
        const QVariantList shown = toList(menu->property("sortedAccounts"));
        QCOMPARE(userIdsOf(shown), userIdsOf(expected));
        for (int i = 0; i < shown.size(); ++i) {
            const QVariantMap a = shown.at(i).toMap();
            const QVariantMap b = expected.at(i).toMap();
            for (const char *key : { "userId", "displayName", "avatarUrl",
                                     "isActive", "needsSignIn",
                                     "sessionRevoked" }) {
                QCOMPARE(a.value(QLatin1String(key)).toString(),
                         b.value(QLatin1String(key)).toString());
            }
        }
        QVERIFY(isNullish(menu->property("labsRows")));
        auto *feed = qvariant_cast<QObject *>(menu->property("activityFeed"));
        QVERIFY(feed);
        QVERIFY(isNullish(feed->property("activity")));

        // No row carries Labs state or a badge.
        for (const QString &uid : { kAlice, kBob }) {
            auto *card = s->card(uid);
            QVERIFY(card);
            QCOMPARE(card->property("liveState").toInt(), 0);
            QCOMPARE(card->property("mentionCount").toInt(), 0);
            QCOMPARE(card->property("activityDot").toBool(), false);
            QCOMPARE(card->property("unreadCount").toInt(), 0);
            auto *badge = s->badgeOf(uid);
            QVERIFY(badge);
            QVERIFY(!badge->isVisible());
        }

        // The rail dot never shows.
        auto *dot = s->root->findChild<QQuickItem *>(
            QStringLiteral("railOtherAccountsDot"));
        QVERIFY(dot);
        QVERIFY(!dot->isVisible());

        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());

        // The quick switcher offers the same switch, and no badge.
        auto *switcher = s->find(QStringLiteral("switcher"));
        QVERIFY(isNullish(switcher->property("labsAccountRows")));
        auto *entry = switchEntry(*s, kBob);
        QVERIFY(entry);
        QQmlExpression label(qmlContext(entry), entry, QStringLiteral("model.label"));
        QCOMPARE(label.evaluate().toString(),
                 QStringLiteral("Switch to %1").arg(kBob));
        auto *entryBadge = findItem(entry, QStringLiteral("quickSwitcherAccountBadge"));
        QVERIFY(entryBadge);
        QVERIFY(!entryBadge->isVisible());
        QMetaObject::invokeMethod(switcher, "close");
    }

    // ── Labs on ───────────────────────────────────────────────────────────

    // Badges follow AccountActivityModel while the menu is open: a number for
    // mentions, N+, a dot, nothing for the active row or a paused account;
    // the rail dot counts the other accounts; the quick switcher entry shows
    // the same mark. Turning Labs off at runtime takes every badge away.
    void badgesFollowTheModelWithTheMenuOpen()
    {
        lightning::accounts::setRustBridgeApiForTest(&m_bridge);
        auto *controller = labsqml::makeController(true, { kAlice, kBob, kCarol });
        controller->switchToAccount(kAlice);
        QTRY_VERIFY(!controller->accountSwitching());
        QVERIFY(controller->liveAccountsLabsAvailable());
        controller->settings()->setLabsConcurrentAccounts(true);
        QTRY_VERIFY(controller->liveAccountsActive());
        AccountActivityModel *activity = controller->accountActivity();
        QVERIFY(activity);
        QVERIFY(controller->accountList() != nullptr);

        auto s = makeSurfaces(controller);
        QVERIFY(s);
        openMenu(*s, 3);
        if (QTest::currentTestFailed())
            return;
        auto *menu = s->find(QStringLiteral("menu"));

        // The rows are AccountListModel's (no secret read per row), active
        // first.
        const QVariantList rows = controller->accountList()->rowsSnapshot();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(toList(menu->property("labsRows")).size(), 3);
        const QStringList shownIds = userIdsOf(toList(menu->property("sortedAccounts")));
        QCOMPARE(shownIds.first(), kAlice);
        QCOMPARE(QSet<QString>(shownIds.begin(), shownIds.end()),
                 (QSet<QString>{ kAlice, kBob, kCarol }));

        // The open menu's delegates must survive a host starting, running or
        // pausing: AccountListModel signals rowsChanged for those too, and
        // replacing the rows would rebuild every row under the pointer.
        const QPointer<QQuickItem> bobRow = s->card(kBob);
        const QPointer<QQuickItem> carolRow = s->card(kCarol);
        QVERIFY(bobRow && carolRow);

        // Bob: two mentions, while the menu is open.
        activity->setLiveState(kBob, AccountActivityModel::Running);
        AccountActivityModel::Summary bob;
        bob.unreadRooms = 1;
        bob.mentions = 2;
        QVERIFY(activity->setSummary(kBob, kEpoch, bob));
        QTRY_VERIFY(s->badgeOf(kBob) && s->badgeOf(kBob)->isVisible());
        QCOMPARE(s->badgeOf(kBob)->property("text").toString(), QStringLiteral("@2"));
        QCOMPARE(s->card(kBob)->property("liveState").toInt(),
                 int(AccountActivityModel::Running));

        // A lower bound.
        bob.mentions = 3;
        bob.mentionsLowerBound = true;
        QVERIFY(activity->setSummary(kBob, kEpoch, bob));
        QTRY_COMPARE(s->badgeOf(kBob)->property("text").toString(),
                     QStringLiteral("@3+"));

        // Carol: unread, no mention -> a dot.
        activity->setLiveState(kCarol, AccountActivityModel::Running);
        AccountActivityModel::Summary carol;
        carol.unreadRooms = 4;
        QVERIFY(activity->setSummary(kCarol, kEpoch, carol));
        QTRY_VERIFY(s->badgeOf(kCarol) && s->badgeOf(kCarol)->isVisible());
        QVERIFY(s->badgeOf(kCarol)->property("dotOnly").toBool());

        // Alice is the active account: never a badge, whatever the model says.
        activity->setLiveState(kAlice, AccountActivityModel::Running);
        AccountActivityModel::Summary alice;
        alice.mentions = 9;
        activity->setSummary(kAlice, kEpoch, alice);
        QCoreApplication::processEvents();
        QVERIFY(s->badgeOf(kAlice));
        QVERIFY(!s->badgeOf(kAlice)->isVisible());

        // The rail dot: other accounts with activity (Bob and Carol).
        auto *dot = s->root->findChild<QQuickItem *>(
            QStringLiteral("railOtherAccountsDot"));
        QVERIFY(dot);
        QTRY_VERIFY(dot->isVisible());
        QQmlExpression dotName(qmlContext(dot), dot,
                               QStringLiteral("Accessible.name"));
        QVERIFY2(dotName.evaluate().toString().contains(
                     QString::number(activity->otherAccountsWithActivity())),
                 qPrintable(dotName.evaluate().toString()));

        // Bob paused: his badge goes; the menu stays open throughout.
        activity->setLiveState(kBob, AccountActivityModel::Paused);
        QTRY_VERIFY(!s->badgeOf(kBob)->isVisible());
        QVERIFY(menu->property("opened").toBool());

        // Three live-state changes later, the same delegates.
        QTest::qWait(50);
        QVERIFY2(bobRow && s->card(kBob) == bobRow.data(),
                 "a live-state change rebuilt the open menu's rows");
        QVERIFY2(carolRow && s->card(kCarol) == carolRow.data(),
                 "a live-state change rebuilt the open menu's rows");

        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());

        // The held change is applied on close: the rows carry Bob's state.
        auto liveStateIn = [](const QVariant &rows, const QString &uid) {
            for (const QVariant &row : toList(rows)) {
                const QVariantMap m = row.toMap();
                if (m.value(QStringLiteral("userId")).toString() == uid)
                    return m.value(QStringLiteral("liveState")).toInt();
            }
            return -1;
        };
        QTRY_COMPARE(liveStateIn(menu->property("labsRows"), kBob),
                     int(AccountActivityModel::Paused));

        // The quick switcher's entry for Carol carries the same dot.
        auto *switcher = s->find(QStringLiteral("switcher"));
        QTRY_VERIFY(toList(switcher->property("labsAccountRows")).size() == 3);
        auto *entry = switchEntry(*s, kCarol);
        QVERIFY(entry);
        auto *entryBadge = findItem(entry, QStringLiteral("quickSwitcherAccountBadge"));
        QVERIFY(entryBadge);
        QTRY_VERIFY(entryBadge->isVisible());
        QVERIFY(entryBadge->property("dotOnly").toBool());

        // A live-state change while the switcher is open does not rebuild
        // the command list under the keyboard.
        const QPointer<QQuickItem> entryBefore = entry;
        auto *commandList = switcher->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        QVERIFY(commandList);
        const int indexBefore = commandList->property("currentIndex").toInt();
        activity->setLiveState(kBob, AccountActivityModel::Running);
        QTest::qWait(50);
        QQuickItem *entryAfter = nullptr;
        QMetaObject::invokeMethod(commandList, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, entryAfter),
                                  Q_ARG(int, 0));
        QVERIFY2(entryBefore && entryAfter == entryBefore.data(),
                 "a live-state change rebuilt the open switcher's entries");
        QCOMPARE(commandList->property("currentIndex").toInt(), indexBefore);

        QMetaObject::invokeMethod(switcher, "close");
        QTRY_VERIFY(!switcher->property("visible").toBool());
        // Applied on close.
        QTRY_COMPARE(liveStateIn(switcher->property("labsAccountRows"), kBob),
                     int(AccountActivityModel::Running));

        // Runtime off: the models go, every badge with them, and the rows
        // are AccountManager's again.
        controller->settings()->setLabsConcurrentAccounts(false);
        QTRY_VERIFY(!controller->liveAccountsActive());
        QTRY_VERIFY(controller->accountActivity() == nullptr);
        QTRY_VERIFY(isNullish(menu->property("labsRows")));
        QTRY_VERIFY(!dot->isVisible());
        openMenu(*s, 3);
        if (QTest::currentTestFailed())
            return;
        for (const QString &uid : { kAlice, kBob, kCarol }) {
            QTRY_VERIFY(s->badgeOf(uid) && !s->badgeOf(uid)->isVisible());
            QTRY_COMPARE(s->card(uid)->property("liveState").toInt(), 0);
        }
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
    }

    // ── Contrast (la1-Q #5; badge colours fixed 2026-10-20, review round 1) ──

    // Every badge, measured from the colours REAL badge items paint, on each
    // of the eleven presets, with high contrast off and on, and on a custom
    // theme: a mention count's ink on its fill and an unread count's at 4.5:1
    // (7:1 high contrast), and the unread dot at 3:1 (4.5:1) against EVERY
    // surface it sits on: the account menu and its panels, room-list rows at
    // rest, hovered and selected, both layouts, the side panel, the header and
    // the conversation (AppTheme.badgeDotSurfaces). A mention count is not
    // colour alone: it reads "@N", its accessible name the number. On the
    // colours before 2026-10-20 this fails (white on the mention fill 3.20:1
    // on Indigo Night, Deep Teal and Storm; the count 3.62:1 on Purple Dusk;
    // the dot 2.08:1 on Nordic's panel; no "@"). Eleven distinct palettes are
    // asserted, so one palette measured eleven times cannot pass.
    void badgeColoursMeetWcagOnEveryPalette()
    {
        QQmlEngine engine;
        auto *theme = engine.singletonInstance<QObject *>(QStringLiteral("MatrixClient"),
                                                          QStringLiteral("AppTheme"));
        QVERIFY2(theme, "no AppTheme singleton: nothing could be measured");
        const auto luminance = [](const QColor &c) {
            const auto ch = [](double v) {
                return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
        };
        const auto contrast = [&luminance](const QColor &a, const QColor &b) {
            const double la = luminance(a) + 0.05;
            const double lb = luminance(b) + 0.05;
            return la > lb ? la / lb : lb / la;
        };
        const auto color = [theme](const char *name) {
            return theme->property(name).value<QColor>();
        };
        // A translucent surface as it shows over the room list.
        const auto over = [](const QColor &c, const QColor &base) {
            const double a = c.alphaF();
            return QColor::fromRgbF(float(c.redF() * a + base.redF() * (1 - a)),
                                    float(c.greenF() * a + base.greenF() * (1 - a)),
                                    float(c.blueF() * a + base.blueF() * (1 - a)));
        };
        const auto dotSurfaces = [&]() {
            const QColor list = color("roomList");
            QList<QPair<QString, QColor>> out;
            out << qMakePair(QStringLiteral("Spaces rail"), color("rail"))
                << qMakePair(QStringLiteral("account menu panel"), color("stormPanel"))
                << qMakePair(QStringLiteral("account menu"), color("stormCanvas"))
                << qMakePair(QStringLiteral("room list"), list);
            for (const char *name : { "roomHover", "roomSelected", "channelHover",
                                      "channelSelected", "sidePanel", "roomHeader",
                                      "timelineBackground" })
                out << qMakePair(QString::fromLatin1(name), over(color(name), list));
            return out;
        };

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(
            "import QtQuick\nimport MatrixClient\n"
            "Item {\n"
            "  property alias dotBadge: d; property alias countBadge: c\n"
            "  property alias mentionBadge: m; property alias threadMention: tm\n"
            "  property alias threadUnread: tu\n"
            "  UnreadBadge { id: d; dot: true }\n"
            "  UnreadBadge { id: c; count: 3 }\n"
            "  UnreadBadge { id: m; count: 3; mention: true }\n"
            "  ThreadUnreadBadge { id: tm; kind: 2 }\n"
            "  ThreadUnreadBadge { id: tu; kind: 1 }\n"
            "}\n"),
            QUrl(QStringLiteral("badgesites.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        const auto item = [&root](const char *which) {
            return root->property(which).value<QObject *>();
        };
        const auto fill = [&item](const char *which) {
            QObject *badge = item(which);
            QObject *background = badge ? badge->property("background").value<QObject *>()
                                        : nullptr;
            return background ? background->property("color").value<QColor>() : QColor();
        };
        const auto ring = [&item](const char *which) {
            QObject *badge = item(which);
            QObject *background = badge ? badge->property("background").value<QObject *>()
                                        : nullptr;
            QObject *border = background ? background->property("border").value<QObject *>()
                                         : nullptr;
            return border ? qMakePair(border->property("color").value<QColor>(),
                                      border->property("width").toReal())
                          : qMakePair(QColor(), qreal(-1));
        };
        const auto ink = [&item](const char *which) {
            QObject *badge = item(which);
            return badge ? badge->property("color").value<QColor>() : QColor();
        };

        QStringList failures;
        const auto measure = [&](const QString &where, double textTarget, double dotTarget) {
            const auto need = [&](const QString &what, const QColor &a, const QColor &b,
                                  double target) {
                const double r = contrast(a, b);
                if (!a.isValid() || !b.isValid() || r < target) {
                    failures << QStringLiteral("%1: %2: %3 on %4 is %5:1, below %6:1")
                                    .arg(where, what, a.name(), b.name())
                                    .arg(r, 0, 'f', 2).arg(target, 0, 'f', 1);
                }
            };
            need(QStringLiteral("mention count"), ink("mentionBadge"), fill("mentionBadge"),
                 textTarget);
            need(QStringLiteral("unread count"), ink("countBadge"), fill("countBadge"),
                 textTarget);
            // A dot keeps the theme's saturated colour; its RING carries the
            // non-text contrast on every surface it sits on (rest, hover,
            // selected, the rail), and so does the Channels unread mark.
            for (const auto &surface : dotSurfaces()) {
                need(QStringLiteral("unread dot ring on ") + surface.first,
                     color("unreadDotRing"), surface.second, dotTarget);
                need(QStringLiteral("mention dot ring on ") + surface.first,
                     color("mentionDotRing"), surface.second, dotTarget);
                need(QStringLiteral("rail dot ring on ") + surface.first,
                     color("railUnreadDotRing"), surface.second, dotTarget);
                need(QStringLiteral("channel unread mark on ") + surface.first,
                     color("channelUnreadMark"), surface.second, dotTarget);
            }
            need(QStringLiteral("\"New messages\""), color("unreadDividerInk"),
                 color("timelineBackground"), textTarget);
            // What the real items paint is the derived badge colours, and the
            // thread badge's mention fill is the one every mention badge uses.
            if (fill("mentionBadge") != color("roomMentionBadge")
                || fill("countBadge") != color("unreadCountFill")
                || fill("dotBadge") != color("unreadDot")
                || ink("mentionBadge") != color("mentionBadgeInk")
                || ink("countBadge") != color("unreadCountInk")
                || item("threadMention")->property("color").value<QColor>()
                       != color("mentionBadgeFill")
                || item("threadUnread")->property("color").value<QColor>()
                       != color("unreadCountFill"))
                failures << where + QStringLiteral(": a badge paints something else");
            // The dot's fill is the theme's own colour, not a pastel of it,
            // and the real dot draws the ring.
            if (color("unreadDot") != color("roomUnreadBadge")
                || color("mentionDot") != color("mentionBadge")
                || color("railUnreadDot") != color("unreadBadge"))
                failures << where + QStringLiteral(": a dot's fill is not the theme colour");
            const auto dotRing = ring("dotBadge");
            if (dotRing.first != color("unreadDotRing")
                || !qFuzzyCompare(dotRing.second,
                                  theme->property("badgeDotRingWidth").toReal())
                || dotRing.second < 1.0)
                failures << where + QStringLiteral(": the unread dot draws no ring");
            // Unread and mention stay two colours.
            if (color("unreadDot") == color("mentionDot")
                || color("unreadDotRing") == color("mentionDotRing"))
                failures << where + QStringLiteral(": unread and mention dots look alike");
        };

        const int originalMode = theme->property("mode").toInt();
        const bool originalHc = theme->property("highContrast").toBool();
        QSet<QString> palettes;
        int checked = 0;
        for (const bool hc : { false, true }) {
            theme->setProperty("highContrast", hc);
            for (int mode = 1; mode <= 11; ++mode) {
                theme->setProperty("mode", mode);
                QCoreApplication::processEvents();
                if (theme->property("mode").toInt() != mode)
                    continue;
                if (!hc) {
                    palettes.insert(color("stormCanvas").name() + color("stormPanel").name()
                                    + color("roomList").name()
                                    + color("mentionBadgeFill").name()
                                    + color("unreadCountFill").name()
                                    + color("unreadDotRing").name());
                    ++checked;
                    // Storm's Channels mark keeps its bolt yellow (its accent).
                    if (mode == 11 && color("channelUnreadMark") != color("accent"))
                        failures << QStringLiteral("theme 11: the channel mark lost its yellow");
                } else if (theme->property("badgeDotRingWidth").toReal() <= 1.5) {
                    failures << QStringLiteral("theme %1: high contrast does not thicken "
                                               "the dot ring").arg(mode);
                }
                // Every preset keeps its own (light) ink, high contrast on as
                // off: the dark ink is for colours like an orange, which none
                // ships (badges review round 3, R2).
                if (color("mentionBadgeInk") != color("mentionBadgeText")
                    || color("unreadCountInk") != color("unreadBadgeText"))
                    failures << QStringLiteral("theme %1%2: a preset's ink moved")
                                    .arg(mode).arg(hc ? " (high contrast)" : "");
                measure(QStringLiteral("theme %1%2").arg(mode).arg(hc ? " (high contrast)" : ""),
                        hc ? 7.0 : 4.5, hc ? 4.5 : 3.0);
            }
        }
        theme->setProperty("highContrast", false);
        QCOMPARE(checked, 11);
        QVERIFY2(palettes.size() == 11,
                 qPrintable(QStringLiteral("only %1 distinct palettes over %2 presets")
                                .arg(palettes.size()).arg(checked)));

        // A custom theme follows its own colours: Lightning Dark with a dark
        // slate accent (its unread colour) and an orange mention. The dot is
        // the slate itself and its ring keeps the slate's hue at 3:1; the
        // orange keeps its colour under a dark ink instead of turning brown
        // under white.
        const QVariant originalBase = theme->property("customBase");
        const QVariant originalOverrides = theme->property("customOverrides");
        theme->setProperty("customBase", 2);
        theme->setProperty("customOverrides",
                           QVariantMap{{QStringLiteral("accent"), QStringLiteral("#3A3F5A")},
                                       {QStringLiteral("mention"), QStringLiteral("#FFB000")}});
        theme->setProperty("mode", 11);
        theme->setProperty("mode", 12);
        QCoreApplication::processEvents();
        measure(QStringLiteral("custom theme"), 4.5, 3.0);
        const QColor accent(QStringLiteral("#3A3F5A"));
        QCOMPARE(color("unreadDot"), accent);
        QVERIFY2(qAbs(color("unreadDotRing").hslHueF() - accent.hslHueF()) < 0.02,
                 qPrintable(color("unreadDotRing").name()));
        QVERIFY(color("unreadDotRing") != accent);   // the ring had to move, and did
        QCOMPARE(color("mentionBadgeFill"), QColor(QStringLiteral("#FFB000")));
        QVERIFY(luminance(color("mentionBadgeInk")) < 0.05);
        // White would move #FF8800 by more than 0.14 lightness, the dark ink
        // not at all: it stays orange under the dark ink, with high contrast
        // on as off (the ink is chosen at 4.5:1 and never swapped by it).
        theme->setProperty("customOverrides",
                           QVariantMap{{QStringLiteral("mention"), QStringLiteral("#FF8800")}});
        theme->setProperty("mode", 11);
        theme->setProperty("mode", 12);
        QCoreApplication::processEvents();
        measure(QStringLiteral("custom theme #FF8800"), 4.5, 3.0);
        QCOMPARE(color("mentionBadgeFill"), QColor(QStringLiteral("#FF8800")));
        QVERIFY(luminance(color("mentionBadgeInk")) < 0.05);
        theme->setProperty("highContrast", true);
        QCoreApplication::processEvents();
        measure(QStringLiteral("custom theme #FF8800 (high contrast)"), 7.0, 4.5);
        QVERIFY(luminance(color("mentionBadgeInk")) < 0.05);
        theme->setProperty("highContrast", false);
        theme->setProperty("customOverrides", originalOverrides);
        theme->setProperty("customBase", originalBase);
        theme->setProperty("mode", originalMode);
        theme->setProperty("highContrast", originalHc);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));

        // Not colour alone: "@N", the accessible name the number.
        QCOMPARE(item("mentionBadge")->property("text").toString(), QStringLiteral("@3"));
        QCOMPARE(item("countBadge")->property("text").toString(), QStringLiteral("3"));

        // The badge sites that cannot be built alone here (a room row, the
        // rail, Home, the timeline header, the thread list, the Channels
        // rows, the "New messages" divider) paint the derived colours: each
        // file's CODE (comments stripped) names exactly these, and none of
        // these badge files paints the base mention or unread colour as a
        // badge. Mutation: revert any one site.
        QDir dir(QStringLiteral(QML_DIR));
        const QRegularExpression comment(QStringLiteral("(?<![:\"\\w])//[^\\n]*"));
        const auto code = [&dir, &comment](const QString &name) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::ReadOnly))
                return QString();
            return QString::fromUtf8(f.readAll()).remove(comment);
        };
        const auto uses = [](const QString &text, const QString &token) {
            return int(text.count(QRegularExpression(
                QStringLiteral("AppTheme\\.%1\\b").arg(token))));
        };
        // A file whose job is wider than the badge (the timeline, a message)
        // is read only in the badge's own block, from its objectName on, so a
        // later non-badge use of a base colour elsewhere in it is not flagged.
        struct Site {
            const char *file;
            QList<QPair<const char *, int>> tokens;
            const char *anchor = nullptr;
            int window = 0;
        };
        const QList<Site> sites = {
            { "UnreadBadge.qml", { { "roomMentionBadge", 1 }, { "mentionBadgeInk", 1 },
                                   { "unreadCountFill", 1 }, { "unreadCountInk", 1 },
                                   { "unreadDot", 1 }, { "unreadDotRing", 1 } } },
            { "RoomDelegate.qml", { { "roomMentionBadge", 1 }, { "mentionBadgeInk", 1 },
                                    { "unreadCountFill", 1 }, { "unreadCountInk", 1 },
                                    { "unreadDot", 2 }, { "unreadDotRing", 2 } } },
            { "HomePane.qml", { { "mentionBadgeFill", 1 }, { "mentionBadgeInk", 1 },
                                { "unreadCountFill", 1 }, { "unreadCountInk", 1 },
                                { "unreadDot", 1 }, { "unreadDotRing", 1 } } },
            { "SpacesRail.qml", { { "mentionBadgeFill", 1 }, { "mentionBadgeInk", 1 },
                                  { "mentionDot", 1 }, { "mentionDotRing", 1 },
                                  { "railUnreadDot", 2 }, { "railUnreadDotRing", 2 } } },
            { "ThreadUnreadBadge.qml", { { "mentionBadgeFill", 1 }, { "mentionBadgeInk", 1 },
                                         { "unreadCountFill", 1 }, { "unreadCountInk", 1 } } },
            { "TimelinePane.qml", { { "mentionDot", 1 }, { "unreadDot", 1 },
                                    { "mentionDotRing", 1 }, { "unreadDotRing", 1 } },
              "objectName: \"threadsUnreadDot\"", 1400 },
            { "ThreadPanel.qml", { { "unreadDot", 1 }, { "unreadDotRing", 1 } } },
            { "ChannelDelegate.qml", { { "channelUnreadMark", 1 } } },
            { "ChannelCategoryHeader.qml", { { "channelUnreadMark", 1 } } },
            { "MessageDelegate.qml", { { "unreadDividerInk", 1 } },
              "objectName: \"unreadDividerLabel\"", 400 },
            // The Space lobby: a folded section's count, and a room row's
            // mention and count pills.
            { "SpaceLobby.qml", { { "mentionBadgeFill", 1 }, { "mentionBadgeInk", 1 },
                                  { "unreadCountFill", 1 }, { "unreadCountInk", 1 } },
              "objectName: \"spaceLobbyFoldedUnread\"", 1400 },
            { "SpaceLobby.qml", { { "mentionBadgeFill", 1 }, { "mentionBadgeInk", 1 },
                                  { "unreadCountFill", 1 }, { "unreadCountInk", 1 } },
              "objectName: \"spaceLobbyRowMention\"", 2200 },
        };
        QStringList wrong;
        for (const Site &site : sites) {
            QString text = code(QLatin1String(site.file));
            if (text.isEmpty()) {
                wrong << QLatin1String(site.file) + QStringLiteral(" unreadable");
                continue;
            }
            if (site.anchor) {
                const int at = text.indexOf(QLatin1String(site.anchor));
                if (at < 0) {
                    wrong << QLatin1String(site.file) + QStringLiteral(" lost its badge block");
                    continue;
                }
                text = text.mid(at, site.window);
            }
            for (const auto &t : site.tokens) {
                const int n = uses(text, QLatin1String(t.first));
                if (n != t.second)
                    wrong << QStringLiteral("%1: %2 x%3, expected %4")
                                 .arg(QLatin1String(site.file), QLatin1String(t.first))
                                 .arg(n).arg(t.second);
            }
            // The base colours are not badge colours in these files (the
            // Channels mark is its own token, fitted where it is declared).
            for (const char *base : { "mentionBadge", "mentionBadgeText", "dangerText",
                                      "unreadBadgeText", "channelUnreadMark", "accentText" }) {
                bool expected = false;
                for (const auto &t : site.tokens)
                    expected = expected || qstrcmp(t.first, base) == 0;
                if (expected)
                    continue;
                const int n = uses(text, QLatin1String(base));
                if (n > 0)
                    wrong << QStringLiteral("%1: still paints AppTheme.%2")
                                 .arg(QLatin1String(site.file), QLatin1String(base));
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QLatin1Char('\n'))));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    AccountBadgesQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AccountBadgesQmlTest.moc"
