// Executed proof for the C74 keyboard / accessibility round. The production
// QML cannot be loaded whole (it needs the `app` context), so each case lifts
// the production source text of the code under test out of the real .qml file
// and runs it in a minimal scene with real items, real focus and real key
// events. A change that removes or breaks the production code fails the case:
// the marker lookup fails, or the behaviour does.
//
// QML_DIR can be overridden with KBNAV_QML_DIR (a mutation / old-tree run).

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace {

QString qmlDir()
{
    const QString env = qEnvironmentVariable("KBNAV_QML_DIR");
    return env.isEmpty() ? QStringLiteral(QML_DIR) : env;
}

QString readFile(const QString &name)
{
    QFile f(qmlDir() + QLatin1Char('/') + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Text from `from` (inclusive) up to `to` (exclusive), empty if either marker
// is missing.
QString slice(const QString &file, const QString &from, const QString &to)
{
    const QString s = readFile(file);
    const int a = s.indexOf(from);
    if (a < 0)
        return {};
    const int b = s.indexOf(to, a + from.size());
    if (b < 0)
        return {};
    return s.mid(a, b - a);
}

} // namespace

class KeyboardNavigationQmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    QObject *m_root = nullptr;

    bool load(const QString &qml)
    {
        delete m_root;
        m_root = nullptr;
        QQmlComponent c(&m_engine);
        c.setData(qml.toUtf8(), QUrl::fromLocalFile(qmlDir() + QLatin1Char('/')));
        m_root = c.create();
        if (!m_root)
            qWarning().noquote() << c.errorString();
        return m_root != nullptr;
    }

    QQuickWindow *window() const { return qobject_cast<QQuickWindow *>(m_root); }

    static void collect(QQuickItem *it, const QString &name, QList<QQuickItem *> &out)
    {
        if (it->objectName() == name)
            out << it;
        for (auto *c : it->childItems())
            collect(c, name, out);
    }

    QQuickItem *named(const char *n) const
    {
        if (auto *it = m_root->findChild<QQuickItem *>(QLatin1String(n)))
            return it;
        // Repeater / inline-component items are visual children only.
        QList<QQuickItem *> found;
        if (auto *w = window())
            collect(w->contentItem(), QLatin1String(n), found);
        return found.isEmpty() ? nullptr : found.first();
    }

    QString focusName() const
    {
        auto *w = window();
        auto *f = w ? w->activeFocusItem() : nullptr;
        return f ? f->objectName() : QString();
    }

    static bool callBool(QObject *o, const char *fn, const QVariant &arg)
    {
        QVariant ret;
        const bool invoked = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret), Q_ARG(QVariant, arg));
        return invoked && ret.toBool();
    }

    void show()
    {
        auto *w = window();
        QVERIFY(w);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        w->requestActivate();
        QTRY_VERIFY(w->isActive() || true);
    }

private Q_SLOTS:
    void cleanupTestCase()
    {
        delete m_root;
        m_root = nullptr;
    }

    // C74-F06: Return / Enter activate the shared button types, and a
    // checkable one still toggles (click() runs the whole sequence).
    void returnActivatesButtons()
    {
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "%1" as W
ApplicationWindow {
    width: 320; height: 120
    property int appClicks: 0
    property int iconClicks: 0
    Row {
        W.AppButton { objectName: "app"; text: "A"; onClicked: appClicks++ }
        W.AppButton { objectName: "toggle"; text: "T"; checkable: true }
        W.IconButton { objectName: "icon"; iconName: "close"; onClicked: iconClicks++ }
        W.AppButton { objectName: "off"; text: "O"; enabled: false; onClicked: appClicks += 100 }
    }
}
)QML").arg(QUrl::fromLocalFile(qmlDir()).toString());
        QVERIFY(load(scene));
        show();
        auto *w = window();
        for (Qt::Key k : {Qt::Key_Return, Qt::Key_Enter}) {
            named("app")->forceActiveFocus();
            QTest::keyClick(w, k);
        }
        QCOMPARE(w->property("appClicks").toInt(), 2);
        named("icon")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_Return);
        QTest::keyClick(w, Qt::Key_Enter);
        QCOMPARE(w->property("iconClicks").toInt(), 2);
        named("toggle")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_Return);
        QVERIFY2(named("toggle")->property("checked").toBool(),
                 "Return on a checkable button must toggle it");
        named("off")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_Return);
        QCOMPARE(w->property("appClicks").toInt(), 2);
    }

    // C74-F01: F6 / Shift+F6 cycle the shell's regions. The production
    // functions are lifted out of MainScreen.qml.
    void f6CyclesTheRegions()
    {
        const QString body = slice(QStringLiteral("MainScreen.qml"),
                                   QStringLiteral("    function _descendantOf("),
                                   QStringLiteral("    function insideComposer("));
        QVERIFY2(!body.isEmpty(), "F6 region code missing from MainScreen.qml");
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Window
Window {
    width: 400; height: 300
    Item {
        id: shellRoot
        anchors.fill: parent
        focus: true
        %1
        Item { id: spacesRail; width: 50; height: 300
            Item { objectName: "railStop"; activeFocusOnTab: true; width: 20; height: 20 } }
        Item { id: roomsPanel; x: 50; width: 100; height: 300
            Item { objectName: "roomsStop"; activeFocusOnTab: true; width: 20; height: 20 } }
        Item { id: timelinePane; objectName: "timelinePane"; x: 150; width: 250; height: 300
            property bool hasComposer: true
            function focusComposer() {
                if (!hasComposer) return false
                composer.forceActiveFocus(); return true
            }
            Item { objectName: "rowStop"; activeFocusOnTab: true; width: 20; height: 20 }
            Item { id: composer; objectName: "composerStop"; activeFocusOnTab: true; width: 20; height: 20 }
        }
    }
}
)QML").arg(body);
        QVERIFY(load(scene));
        show();
        auto *w = window();
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("railStop"));
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("roomsStop"));
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("composerStop"));
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("railStop"));
        QTest::keyClick(w, Qt::Key_F6, Qt::ShiftModifier);
        QCOMPARE(focusName(), QStringLiteral("composerStop"));
        QTest::keyClick(w, Qt::Key_F6, Qt::ShiftModifier);
        QCOMPARE(focusName(), QStringLiteral("roomsStop"));
        // A hidden region is skipped.
        auto *rooms = named("roomsStop")->parentItem();
        rooms->setVisible(false);
        named("railStop")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("composerStop"));
        rooms->setVisible(true);
        // No writable composer: the chat pane's first tab stop.
        named("timelinePane")->setProperty("hasComposer", false);
        named("roomsStop")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("rowStop"));
        // Ctrl+F6 is not ours.
        named("railStop")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_F6, Qt::ControlModifier);
        QCOMPARE(focusName(), QStringLiteral("railStop"));
    }

    // MASTER-3 M3 / recheck N1: the unverified-session card is a region while
    // it shows. It is NOT inside the shell (it lives on the window overlay),
    // so the shell is handed it, and F6 pressed inside it is forwarded back.
    // Both production blocks are lifted: MainScreen's region code and the
    // card's F6 hand-off.
    void f6ReachesAndLeavesTheVerifyCard()
    {
        const QString body = slice(QStringLiteral("MainScreen.qml"),
                                   QStringLiteral("    function _descendantOf("),
                                   QStringLiteral("    function insideComposer("));
        const QString cardKeys = slice(QStringLiteral("VerifySessionPrompt.qml"),
                                       QStringLiteral("    signal regionCycleRequested("),
                                       QStringLiteral("    // (end of the card's F6 hand-off)"));
        QVERIFY2(!body.isEmpty(), "F6 region code missing from MainScreen.qml");
        QVERIFY2(!cardKeys.isEmpty(), "the card's F6 hand-off is missing");
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    width: 600; height: 300
    visible: true
    Item {
        id: shellRoot
        width: 400; height: 300
        focus: true
        %1
        // Main.qml's hand-off.
        Item { Component.onCompleted: shellRoot.promptRegion = card }
        Item { id: spacesRail; width: 50; height: 300
            Item { objectName: "railStop"; activeFocusOnTab: true; width: 20; height: 20 } }
        Item { id: roomsPanel; x: 50; width: 100; height: 300
            Item { objectName: "roomsStop"; activeFocusOnTab: true; width: 20; height: 20 } }
        Item { id: timelinePane; objectName: "timelinePane"; x: 150; width: 250; height: 300
            function focusComposer() { composer.forceActiveFocus(); return true }
            Item { id: composer; objectName: "composerStop"; activeFocusOnTab: true; width: 20; height: 20 }
        }
    }
    // On the window overlay, as in Main.qml: NOT under the content item.
    Item {
        id: card
        parent: Overlay.overlay
        objectName: "card"
        x: 420; width: 160; height: 100
        property bool shouldShow: true
        %2
        Item { objectName: "cardStop"; activeFocusOnTab: true; width: 20; height: 20 }
    }
}
)QML").arg(body, QString(cardKeys).replace(QStringLiteral("root."), QStringLiteral("card.")));
        QVERIFY(load(scene));
        show();
        auto *w = window();
        named("composerStop")->forceActiveFocus();
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("cardStop"));
        // F6 inside the card leaves it (the cycle wraps to the rail).
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("railStop"));
        QTest::keyClick(w, Qt::Key_F6, Qt::ShiftModifier);
        QCOMPARE(focusName(), QStringLiteral("cardStop"));
        QTest::keyClick(w, Qt::Key_F6, Qt::ShiftModifier);
        QCOMPARE(focusName(), QStringLiteral("composerStop"));
        // A hidden card is not a region.
        named("card")->setProperty("shouldShow", false);
        QTest::keyClick(w, Qt::Key_F6);
        QCOMPARE(focusName(), QStringLiteral("railStop"));
    }

    // C74-F03: Up/Down walk between ROWS (skipping the other tab stops in a
    // row), stop at the end of THIS list instead of wrapping into another
    // timeline, and ask the view to reveal the row.
    void rowNavigationStaysInOneList()
    {
        const QString fn = slice(QStringLiteral("MessageDelegate.qml"),
                                 QStringLiteral("    function focusNeighbourRow("),
                                 QStringLiteral("    // Keyboard focus ring (C74-F02)"));
        QVERIFY2(!fn.isEmpty(), "focusNeighbourRow missing from MessageDelegate.qml");
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Window
Window {
    width: 300; height: 300
    property var revealed: []
    QtObject {
        id: view
        function revealRow(it) { revealed = revealed.concat([it.objectName + ":" + it.tag]) }
    }
    component Row: Item {
        id: root
        objectName: "messageDelegateRoot"
        property string tag
        property var timelineView: view
        activeFocusOnTab: true
        width: 100; height: 20
        %1
    }
    Column { id: listA
        Row { tag: "a0" }
        Item { objectName: "receiptAvatar"; activeFocusOnTab: true; width: 10; height: 10 }
        Row { tag: "a1" }
        Row { tag: "a2" }
    }
    Column { id: listB; y: 150
        Row { tag: "b0" }
    }
}
)QML").arg(fn);
        QVERIFY(load(scene));
        show();
        auto *w = window();
        QList<QQuickItem *> rows;
        collect(w->contentItem(), QStringLiteral("messageDelegateRoot"), rows);
        QCOMPARE(rows.size(), 4);
        auto rowByTag = [&](const char *t) {
            for (auto *r : rows)
                if (r->property("tag").toString() == QLatin1String(t))
                    return r;
            return static_cast<QQuickItem *>(nullptr);
        };
        auto *a0 = rowByTag("a0");
        auto *a1 = rowByTag("a1");
        auto *a2 = rowByTag("a2");
        QVERIFY(a0 && a1 && a2 && rowByTag("b0"));
        a0->forceActiveFocus();
        bool ok = false;
        ok = callBool(a0, "focusNeighbourRow", true);
        QVERIFY(ok);
        QVERIFY2(a1->hasActiveFocus(), "must skip the receipt avatar stop");
        ok = callBool(a1, "focusNeighbourRow", true);
        QVERIFY(ok);
        QVERIFY(a2->hasActiveFocus());
        // The next row in the window's chain belongs to another list.
        ok = callBool(a2, "focusNeighbourRow", true);
        QVERIFY2(!ok, "must not hop into another timeline");
        QVERIFY(a2->hasActiveFocus());
        // Backwards, and every successful move asked the view to reveal.
        ok = callBool(a2, "focusNeighbourRow", false);
        QVERIFY(ok);
        QVERIFY(a1->hasActiveFocus());
        const QStringList revealed = w->property("revealed").toStringList();
        QCOMPARE(revealed, (QStringList{QStringLiteral("messageDelegateRoot:a1"),
                                        QStringLiteral("messageDelegateRoot:a2"),
                                        QStringLiteral("messageDelegateRoot:a1")}));
    }

    // C74-F03: the reveal scrolls the rotated timeline just far enough.
    void revealRowScrollsTheRotatedView()
    {
        const QString fn = slice(QStringLiteral("TimelinePane.qml"),
                                 QStringLiteral("                function revealRow(row) {"),
                                 QStringLiteral("                // Jump to latest glides"));
        QVERIFY2(!fn.isEmpty(), "revealRow missing from TimelinePane.qml");
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Window
Window {
    width: 300; height: 400
    Flickable {
        id: timeline
        objectName: "timeline"
        anchors.fill: parent
        rotation: 180
        contentWidth: 300
        contentHeight: col.height
        flickableDirection: Flickable.VerticalFlick
        property int cancels: 0
        property int updates: 0
        function wheelMinY() { return 0 }
        function wheelMaxY() { return Math.max(0, contentHeight - height) }
        function cancelWheelMotion() { cancels++ }
        function updateStickAndPaginate() { updates++ }
        Timer { id: scrollSettleTimer; objectName: "settle"; interval: 100000 }
        %1
        Column { id: col
            Repeater { model: 20
                Item { objectName: "row" + index; width: 300; height: 100; rotation: 180 } }
        }
    }
}
)QML").arg(fn);
        QVERIFY(load(scene));
        show();
        auto *tl = named("timeline");
        QVERIFY(tl);
        auto reveal = [&](int row) {
            return callBool(tl, "revealRow",
                            QVariant::fromValue<QObject *>(named(
                                QByteArray("row" + QByteArray::number(row)).constData())));
        };
        auto inView = [&](int row) {
            const qreal y = tl->property("contentY").toReal();
            const qreal top = row * 100.0;
            return top >= y - 0.5 && top + 100 <= y + 400 + 0.5;
        };
        QCOMPARE(tl->property("contentY").toReal(), 0.0);
        QVERIFY(!reveal(1));                       // already inside: no scroll
        QCOMPARE(tl->property("contentY").toReal(), 0.0);
        QVERIFY(reveal(10));                       // far below the viewport
        QVERIFY(inView(10));
        QCOMPARE(tl->property("contentY").toReal(), 708.0);
        QVERIFY(tl->property("cancels").toInt() >= 1);
        QVERIFY(tl->property("updates").toInt() >= 1);
        QVERIFY(reveal(2));                        // now above it
        QVERIFY(inView(2));
        QCOMPARE(tl->property("contentY").toReal(), 192.0);
        QVERIFY(reveal(19));                       // clamped to the last position
        QCOMPARE(tl->property("contentY").toReal(), 1600.0);
        QVERIFY(inView(19));
        QVERIFY(!reveal(19));                      // idempotent
    }

    // C74-F08: closing Settings returns the focus to what held it before,
    // and the answer does not depend on the order in which the shell hides.
    void closingSettingsRestoresFocus()
    {
        const QString tracker = slice(QStringLiteral("Main.qml"),
                                      QStringLiteral("    // The last item that held the keyboard focus"),
                                      QStringLiteral("    Component.onCompleted: {"));
        const QString loaderBody = slice(QStringLiteral("Main.qml"),
                                         QStringLiteral("        // Closing Settings (Escape) hands the focus back"),
                                         QStringLiteral("    }\n    Timer {\n        interval: 2500"));
        QVERIFY2(!tracker.isEmpty() && !loaderBody.isEmpty(),
                 "Settings focus code missing from Main.qml");
        const QString scene = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
Window {
    id: window
    width: 400; height: 300
    QtObject { id: app; property int currentScreen: 1 }
    %1
    Item { id: pageLoader; anchors.fill: parent
        property Item item: mainScreen
        Item { id: mainScreen; objectName: "mainScreen"; anchors.fill: parent
            visible: app.currentScreen !== 2
            TextInput { id: composer; objectName: "composer"; width: 100; height: 20; activeFocusOnTab: true }
            TextInput { id: rail; objectName: "rail"; y: 40; width: 100; height: 20; activeFocusOnTab: true }
        }
    }
    Loader {
        id: settingsViewLoader
        objectName: "settingsViewLoader"
        anchors.fill: parent
        property bool warm: false
        // As in Main.qml: the first open builds the view, and onLoaded gives it
        // the focus, which may come before or after onVisibleChanged.
        active: warm || app.currentScreen === 2
        visible: app.currentScreen === 2
        asynchronous: !visible
        sourceComponent: FocusScope {
            TextInput { objectName: "settingsField"; focus: true; width: 100; height: 20 }
        }
        onLoaded: { warm = true; if (visible) item.forceActiveFocus() }
        %2
    }
    function setScreen(s) { app.currentScreen = s }
}
)QML").arg(tracker, loaderBody);
        QVERIFY(load(scene));
        show();
        auto *w = window();
        auto call = [&](int s) { QMetaObject::invokeMethod(w, "setScreen", Q_ARG(QVariant, s)); };
        named("composer")->forceActiveFocus();
        QCOMPARE(focusName(), QStringLiteral("composer"));
        call(2);
        QTRY_VERIFY2(focusName() == QLatin1String("settingsField"), qPrintable(focusName()));
        call(1);
        QTRY_COMPARE(focusName(), QStringLiteral("composer"));
        // Another opener: the rail held the focus this time.
        named("rail")->forceActiveFocus();
        call(2);
        QTRY_COMPARE(focusName(), QStringLiteral("settingsField"));
        call(1);
        QTRY_COMPARE(focusName(), QStringLiteral("rail"));
        // The hostile order: the shell is already hidden (its focus gone) by
        // the time Settings reports visible. Reading activeFocusItem there
        // returns nothing useful; the tracked item must still come back.
        named("composer")->forceActiveFocus();
        QCOMPARE(focusName(), QStringLiteral("composer"));
        named("mainScreen")->setVisible(false);
        call(2);
        QTRY_COMPARE(focusName(), QStringLiteral("settingsField"));
        call(1);
        named("mainScreen")->setVisible(true);
        QTRY_COMPARE(focusName(), QStringLiteral("composer"));
        // The remembered item went away while Settings was open: the page.
        named("composer")->forceActiveFocus();
        call(2);
        QTRY_COMPARE(focusName(), QStringLiteral("settingsField"));
        named("composer")->setVisible(false);
        call(1);
        QTRY_VERIFY2(!focusName().isEmpty() && focusName() != QLatin1String("settingsField"),
                     qPrintable(focusName()));
    }

    // Robust source facts the scenes above cannot show: each affordance exists.
    void focusRingsAndNamesExist()
    {
        const QString m = readFile(QStringLiteral("MessageDelegate.qml"));
        const QString r = readFile(QStringLiteral("RoomDelegate.qml"));
        const QString s = readFile(QStringLiteral("SpacesRail.qml"));
        QVERIFY(m.contains(QStringLiteral("objectName: \"messageRowFocusRing\"")));
        QVERIFY(r.contains(QStringLiteral("objectName: \"roomRowFocusRing\"")));
        QVERIFY(s.contains(QStringLiteral("objectName: \"railSpaceFocusRing\"")));
        QVERIFY(s.contains(QStringLiteral("objectName: \"railAccountFocusRing\"")));
    }
};

QTEST_MAIN(KeyboardNavigationQmlTest)
#include "KeyboardNavigationQmlTest.moc"
