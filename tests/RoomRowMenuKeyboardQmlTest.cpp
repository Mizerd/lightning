// Break-it chunk C06 (room row, room context menu, leave and close dialogs),
// the keyboard and invitation findings, on the real components:
//   F01/F03  the Leave dialog opens with Cancel holding VISIBLE keyboard
//            focus, and Return / Enter press the focused button (a Button
//            answers Space only); the same for the Close room dialog.
//   F02      the room row that holds keyboard focus draws a ring.
//   F06      a pending invitation's menu carries no read-state, notification,
//            background or room-link rows.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include <memory>

namespace {

const char *kLeaveScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 500
    height: 600
    visible: true
    LeaveRoomConfirmDialog { objectName: "dlg" }
}
)QML";

const char *kCloseApp = R"QML(
import QtQuick
QtObject {
    property QtObject roomClosure: QtObject {
        property string phase: "ready"
        property string mode: "close"
        property string title: "Close room"
        property string notice: ""
        property string consequenceText: "Members are removed."
        property string statusText: ""
        property int failedCount: 0
        property bool canConfirm: true
        property string confirmLabel: "Close room"
        property bool leaveAfter: false
        property bool unlistFromParents: false
        property var parentSpaceNames: []
        property var lockedParentNames: []
        property var rooms: []
        property int confirms: 0
        function begin(a, b, c, d) { return true }
        function confirm(reason) { confirms = confirms + 1 }
        function viewClosed() {}
        function setTypedConfirmation(t) {}
        function setRoomSelected(a, b) {}
    }
}
)QML";

const char *kCloseScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 700
    visible: true
    RoomCloseDialog { objectName: "closeDlg" }
}
)QML";

const char *kMenuApp = R"QML(
import QtQuick
QtObject {
    property QtObject roomList: QtObject {
        property bool roomFavouritesSupported: true
    }
    property QtObject backdrops: QtObject {}
    property bool serverRoomNotificationModes: false
    property QtObject settings: QtObject {
        signal roomNotificationModeChanged(string roomId)
        property string clockTimeFormat: "HH:mm"
        function roomNotificationMode(roomId) { return 1 }
    }
    property QtObject discovery: QtObject { function cancelKnock(id) {} }
    signal roomNotificationModeSyncStateChanged(string roomId)
    function roomNotificationModeSyncFailed(roomId) { return false }
    function requestRoomNotificationMode(roomId) {}
    function textFontWithEmoji(f) { return f }
}
)QML";

const char *kMenuScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: w
    width: 700
    height: 600
    visible: true
    RoomActionsMenu { id: m; objectName: "menu"; roomId: "!r:x"; roomName: "Room" }
    function openIt() { m.popup(w.contentItem, 40, 40) }
}
)QML";

const char *kRowScene = R"QML(
import QtQuick
import MatrixClient
Item {
    width: 300
    height: 200
    ListView {
        objectName: "list"
        anchors.fill: parent
        model: ListModel {
            ListElement { roomId: "!r:x"; name: "Room"; membership: "joined"
                          hasUnread: false; markedUnread: false
                          unreadCount: 0; highlightCount: 0
                          threadUnreadKind: 0; isDirect: false
                          lastMessagePreview: "hello"; lastActivity: 0
                          avatarUrl: ""; encrypted: false; isSpace: false
                          invitePending: false; inviter: ""; isFavourite: false
                          identityColorKey: "!r:x"; network: ""; networkLabel: ""
                          category: "conversation"; memberCount: 3 }
        }
        delegate: RoomDelegate { width: 300 }
    }
}
)QML";

QObject *makeApp(QQmlEngine &engine, const char *src,
                 std::unique_ptr<QObject> &owner)
{
    QQmlComponent c(&engine);
    c.setData(QByteArray(src), QUrl(QStringLiteral("qrc:/c06app.qml")));
    owner.reset(c.create());
    if (!owner)
        qWarning("%s", qPrintable(c.errorString()));
    engine.rootContext()->setContextProperty(QStringLiteral("app"), owner.get());
    return owner.get();
}

QQuickItem *focusItem(QObject *window)
{
    return qobject_cast<QQuickWindow *>(window)->activeFocusItem();
}

} // namespace

class RoomRowMenuKeyboardQmlTest : public QObject
{
    Q_OBJECT

private:
    void openLeave(QObject *dlg)
    {
        QMetaObject::invokeMethod(dlg, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("!r:x")),
                                  Q_ARG(QVariant, QStringLiteral("Den")),
                                  Q_ARG(QVariant, 3),
                                  Q_ARG(QVariant, QStringLiteral("public")),
                                  Q_ARG(QVariant, QVariant(0)));
        QTRY_VERIFY(dlg->property("opened").toBool());
    }

    void collectRows(QObject *root, QObject *menu, bool invite, QStringList &texts)
    {
        menu->setProperty("isInvite", invite);
        QMetaObject::invokeMethod(root, "openIt");
        QTRY_VERIFY(menu->property("opened").toBool());
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            if (row && row->isVisible()
                    && row->property("highlighted").isValid())
                texts << row->property("text").toString();
        }
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

private slots:
    void theLeaveDialogOpensWithCancelHoldingVisibleFocus()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(kLeaveScene), QUrl(QStringLiteral("leave.qml")));
        std::unique_ptr<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dlg = root->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(dlg);
        openLeave(dlg);
        QTRY_VERIFY(focusItem(root.get()));
        QQuickItem *item = focusItem(root.get());
        QCOMPARE(item->objectName(), QStringLiteral("leaveRoomConfirmCancel"));
        // The ring is drawn only for visualFocus: PopupFocusReason, which the
        // popup uses on its own, draws none.
        QVERIFY2(item->property("visualFocus").toBool(),
                 "Cancel has the focus but no ring shows it");
    }

    void returnPressesTheFocusedButtonAndSpaceOnCancelNeverConfirms()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(kLeaveScene), QUrl(QStringLiteral("leave.qml")));
        std::unique_ptr<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dlg = root->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(dlg);
        QSignalSpy confirmed(dlg, SIGNAL(confirmed(QString)));

        // Return on the default, Cancel: closes, leaves nothing.
        openLeave(dlg);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY2(!dlg->property("opened").toBool(),
                     "Return did nothing in the Leave dialog");
        QCOMPARE(confirmed.size(), 0);

        // Space on Cancel: closes, leaves nothing.
        openLeave(dlg);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!dlg->property("opened").toBool());
        QCOMPARE(confirmed.size(), 0);

        // Tab to Leave room, then Return: that is a deliberate confirm.
        openLeave(dlg);
        QTest::keyClick(window, Qt::Key_Tab);
        QCOMPARE(focusItem(root.get())->objectName(),
                 QStringLiteral("leaveRoomConfirmButton"));
        QTest::keyClick(window, Qt::Key_Enter);
        QTRY_VERIFY(!dlg->property("opened").toBool());
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.at(0).at(0).toString(), QStringLiteral("!r:x"));
    }

    void aStraySpaceOrReturnOnAnUnringedLeaveButtonNeverLeaves()
    {
        QQmlEngine engine;
        QQmlComponent c(&engine);
        c.setData(QByteArray(kLeaveScene), QUrl(QStringLiteral("leave.qml")));
        std::unique_ptr<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dlg = root->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(dlg);
        QSignalSpy confirmed(dlg, SIGNAL(confirmed(QString)));
        auto *leave = dlg->findChild<QQuickItem *>(
            QStringLiteral("leaveRoomConfirmButton"));
        QVERIFY(leave);
        for (int key : {int(Qt::Key_Space), int(Qt::Key_Return), int(Qt::Key_Enter)}) {
            openLeave(dlg);
            // Focus on Leave by any non-keyboard route: no ring, no consent.
            leave->forceActiveFocus(Qt::OtherFocusReason);
            QVERIFY(leave->hasActiveFocus());
            QVERIFY(!leave->property("visualFocus").toBool());
            QTest::keyClick(window, Qt::Key(key));
            QTest::qWait(50);
            QVERIFY2(confirmed.isEmpty(),
                     "a key on a ring-less Leave room button left the room");
            QVERIFY(dlg->property("opened").toBool());
            QMetaObject::invokeMethod(dlg, "close");
            QTRY_VERIFY(!dlg->property("opened").toBool());
        }
    }

    void returnPressesTheFocusedButtonInTheCloseDialog()
    {
        QQmlEngine engine;
        std::unique_ptr<QObject> app;
        QVERIFY(makeApp(engine, kCloseApp, app));
        QQmlComponent c(&engine);
        c.setData(QByteArray(kCloseScene), QUrl(QStringLiteral("close.qml")));
        std::unique_ptr<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dlg = root->findChild<QObject *>(QStringLiteral("closeDlg"));
        QVERIFY(dlg);
        QMetaObject::invokeMethod(dlg, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("!r:x")),
                                  Q_ARG(QVariant, QStringLiteral("Den")),
                                  Q_ARG(QVariant, false),
                                  Q_ARG(QVariant, QStringLiteral("close")));
        QTRY_VERIFY(dlg->property("opened").toBool());
        auto *ctl = app->property("roomClosure").value<QObject *>();
        QVERIFY(ctl);

        // A text field keeps its own Return.
        auto *reason = dlg->findChild<QQuickItem *>(QStringLiteral("roomCloseReason"));
        if (reason && reason->isVisible()) {
            reason->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Return);
            QCOMPARE(ctl->property("confirms").toInt(), 0);
        }

        auto *confirm = dlg->findChild<QQuickItem *>(QStringLiteral("roomCloseConfirm"));
        QVERIFY(confirm);
        // A ring-less focus (not reached by the keyboard) never starts it.
        confirm->forceActiveFocus(Qt::OtherFocusReason);
        QVERIFY(!confirm->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(ctl->property("confirms").toInt(), 0);
        // forceActiveFocus on the holder keeps the old reason: release first.
        confirm->setFocus(false);
        confirm->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(confirm->hasActiveFocus());
        QVERIFY(confirm->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY2(ctl->property("confirms").toInt() == 1,
                 "Return did not press the focused Close room button");

        auto *cancel = dlg->findChild<QQuickItem *>(QStringLiteral("roomCloseCancel"));
        QVERIFY(cancel);
        cancel->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!dlg->property("opened").toBool());
        QCOMPARE(ctl->property("confirms").toInt(), 1);
    }

    void aRoomRowHoldingKeyboardFocusDrawsARing()
    {
        QQmlEngine engine;
        std::unique_ptr<QObject> app;
        QVERIFY(makeApp(engine, kMenuApp, app));
        QQmlComponent c(&engine);
        c.setData(QByteArray(kRowScene), QUrl(QStringLiteral("qrc:/rowscene.qml")));
        std::unique_ptr<QObject> owner(c.create());
        QVERIFY2(owner, qPrintable(c.errorString()));
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QQuickWindow window;
        window.resize(400, 300);
        rootItem->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *list = rootItem->findChild<QQuickItem *>(QStringLiteral("list"));
        QVERIFY(list);
        QQuickItem *row = nullptr;
        QTRY_VERIFY([&] {
            const auto kids =
                list->property("contentItem").value<QQuickItem *>()->childItems();
            for (QQuickItem *k : kids)
                if (k->property("showGroupDivider").isValid()) {
                    row = k;
                    return true;
                }
            return false;
        }());
        window.requestActivate();
        QTRY_VERIFY(window.isActive());
        QImage before = window.grabWindow();
        row->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(row->hasActiveFocus());
        QTest::qWait(50);
        QImage after = window.grabWindow();
        QVERIFY(before.size() == after.size());
        int differing = 0;
        for (int y = 0; y < before.height(); ++y)
            for (int x = 0; x < before.width(); ++x)
                if (before.pixel(x, y) != after.pixel(x, y))
                    ++differing;
        QVERIFY2(differing > 40,
                 qPrintable(QStringLiteral("only %1 pixels changed when the row "
                                           "took keyboard focus").arg(differing)));
    }

    void anInvitationMenuOffersOnlyWhatAnInvitationCanUse()
    {
        QQmlEngine engine;
        std::unique_ptr<QObject> app;
        QVERIFY(makeApp(engine, kMenuApp, app));
        QQmlComponent c(&engine);
        c.setData(QByteArray(kMenuScene), QUrl(QStringLiteral("qrc:/menuscene.qml")));
        std::unique_ptr<QObject> root(c.create());
        QVERIFY2(root, qPrintable(c.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *menu = root->findChild<QObject *>(QStringLiteral("menu"));
        QVERIFY(menu);

        QStringList invite, joinedRows, joinedAgain;
        collectRows(root.get(), menu, true, invite);
        collectRows(root.get(), menu, false, joinedRows);
        const QString joined = joinedRows.join(QLatin1Char('|'));
        QVERIFY(joined.contains(QStringLiteral("Mark as read")));
        QVERIFY(joined.contains(QStringLiteral("Notifications")));
        QVERIFY(joined.contains(QStringLiteral("Copy room link")));
        for (const QString &t : invite)
            for (const char *banned : {"Mark as read", "Mark as unread",
                                       "Notifications", "Chat background",
                                       "Copy room link", "Leave room"})
                QVERIFY2(!t.contains(QLatin1String(banned)),
                         qPrintable(QStringLiteral("invitation menu offers '%1'")
                                        .arg(t)));
        QVERIFY(invite.contains(QStringLiteral("Accept invitation")));
        // The same delegate turning into a joined room gets the rows back.
        collectRows(root.get(), menu, false, joinedAgain);
        QVERIFY(joinedAgain.join(QLatin1Char('|'))
                    .contains(QStringLiteral("Notifications")));
    }
};

QTEST_MAIN(RoomRowMenuKeyboardQmlTest)
#include "RoomRowMenuKeyboardQmlTest.moc"
