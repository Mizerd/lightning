// Escape steps back out of a conversation (GitHub #27), in the real shell:
//   * Escape from the message box closes the room to its Space's overview,
//     a second Escape returns to Home, and a third does nothing;
//   * the room stops being the timeline's room, so nothing more is marked
//     read (ReadReceiptCoordinator only ever sends for the model's room);
//   * every earlier owner of Escape keeps it: a reply or an edit in the
//     composer, another text field, and an open dialog;
//   * an open popup (the read-receipt list, a profile card, the reaction
//     picker) takes the first Escape even without the keyboard focus, and a
//     hover tooltip does not.
// Keys are sent to the window, so delivery follows real focus and
// propagation, not a direct call.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "models/MessageComposer.h"
#include "models/TimelineModel.h"
#include "spaces/SpaceManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 720
    visible: true
    color: AppTheme.background

    MainScreen {
        objectName: "mainScreen"
        anchors.fill: parent
    }
}
)QML";

const QString kRoom = QStringLiteral("!general:mock.local");
const QString kSpace = QStringLiteral("!space-team:mock.local");

} // namespace

class EscapeNavigationQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    // Opens the room inside the Space with the message box focused, the
    // state the report starts from.
    void openRoomWithComposerFocused()
    {
        m_controller->openSpaceHome(kSpace);
        m_controller->setCurrentRoomId(kRoom);
        QTest::qWait(80);
        QQuickItem *composer = item("messageComposer");
        QVERIFY(composer);
        QVERIFY(composer->isVisible());
        QVERIFY(QMetaObject::invokeMethod(composer, "focusEditor"));
        QTest::qWait(20);
        QQuickItem *focused = m_window->activeFocusItem();
        QVERIFY2(focused, "nothing has the keyboard focus");
        bool inComposer = false;
        for (QQuickItem *it = focused; it; it = it->parentItem())
            inComposer = inComposer || it == composer;
        QVERIFY2(inComposer, "the message box did not take the focus");
    }

    void escape()
    {
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTest::qWait(30);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("escape-navigation-qml-test"));

        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(3000));
        QCOMPARE(m_controller->currentScreen(), AppController::MainScreen);

        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("escapenavigation.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QTest::qWait(80);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void init()
    {
        m_controller->composer()->cancelReplyOrEdit();
    }

    void escapeClosesTheRoomThenLeavesTheSpace()
    {
        openRoomWithComposerFocused();
        QCOMPARE(m_controller->timeline()->roomId(), kRoom);

        escape();
        QCOMPARE(m_controller->currentRoomId(), QString());
        QCOMPARE(m_controller->spaces()->activeSpaceId(), kSpace);
        // The timeline holds no room, so the read-receipt policy has nothing
        // to mark read.
        QCOMPARE(m_controller->timeline()->roomId(), QString());

        escape();
        QCOMPARE(m_controller->spaces()->activeSpaceId(), QString());

        // Home with nothing open: nothing further, and nothing breaks.
        escape();
        QCOMPARE(m_controller->currentRoomId(), QString());
        QCOMPARE(m_controller->spaces()->activeSpaceId(), QString());
        QCOMPARE(m_controller->navigateBack(), false);
    }

    void aReplyKeepsTheFirstEscape()
    {
        openRoomWithComposerFocused();
        m_controller->composer()->beginReply(QStringLiteral("$parent"),
                                             QStringLiteral("@bob:mock.local"),
                                             QStringLiteral("hello"));
        QVERIFY(m_controller->composer()->isReplying());

        escape();
        QVERIFY2(!m_controller->composer()->isReplying(),
                 "Escape did not cancel the reply");
        QCOMPARE(m_controller->currentRoomId(), kRoom);

        escape();
        QCOMPARE(m_controller->currentRoomId(), QString());
    }

    void anEditKeepsTheFirstEscape()
    {
        openRoomWithComposerFocused();
        m_controller->composer()->beginEdit(QStringLiteral("$mine"),
                                            QStringLiteral("draft"));
        QVERIFY(m_controller->composer()->isEditing());

        escape();
        QVERIFY2(!m_controller->composer()->isEditing(),
                 "Escape did not cancel the edit");
        QCOMPARE(m_controller->currentRoomId(), kRoom);
    }

    // The room-list search is a text field of its own: Escape there must not
    // close the conversation behind it.
    void anotherTextFieldKeepsEscape()
    {
        openRoomWithComposerFocused();
        QQuickItem *field = nullptr;
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty() && !field) {
            QQuickItem *it = stack.takeLast();
            if (it->property("placeholderText").toString() == QStringLiteral("Search")
                && it->isVisible())
                field = it;
            stack.append(it->childItems());
        }
        QVERIFY2(field, "the room-list search field was not found");
        field->forceActiveFocus();
        QTest::qWait(20);
        QCOMPARE(m_window->activeFocusItem(), field);

        escape();
        QCOMPARE(m_controller->currentRoomId(), kRoom);
    }

    // A dialog is a popup, not a child of the shell: its Escape closes it and
    // goes no further.
    void anOpenDialogKeepsEscape()
    {
        openRoomWithComposerFocused();
        QQuickItem *panel = item("mainScreen");
        QVERIFY(panel);
        QObject *dialog =
            panel->findChild<QObject *>(QStringLiteral("leaveRoomConfirmDialog"));
        QVERIFY2(dialog, "no dialog to open");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());

        escape();
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(m_controller->currentRoomId(), kRoom);
    }

    // The read-receipt list, opened by a real click on a row's facepile. The
    // same tap reaches the timeline's own TapHandler, which used to take the
    // focus back from the popover, so one Escape went to the shell and closed
    // the popover (with its room) AND the room.
    void theReceiptListTakesTheFirstEscape()
    {
        openRoomWithComposerFocused();
        TimelineEvent read;
        read.eventId = QStringLiteral("$escape-receipts");
        read.roomId = kRoom;
        read.sender = QStringLiteral("@bob:mock.local");
        read.body = QStringLiteral("Seen by somebody");
        read.timestamp = QDateTime::currentDateTimeUtc();
        read.readBy.append({ QStringLiteral("@bob:mock.local"),
                             QDateTime::currentMSecsSinceEpoch() });
        read.readByTotal = 1;
        QVERIFY(QMetaObject::invokeMethod(m_controller->timeline(),
                                          "onEventAppended",
                                          Q_ARG(QString, kRoom),
                                          Q_ARG(TimelineEvent, read)));
        QQuickItem *facepile = nullptr;
        QTRY_VERIFY2((facepile = visibleReceiptRow()) != nullptr,
                     "no row drew a read-receipt facepile");
        const QPointF centre = facepile->mapToScene(
            QPointF(facepile->width() / 2, facepile->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, {}, centre.toPoint());
        QObject *popover = item("mainScreen")->findChild<QObject *>(
            QStringLiteral("receiptListPopover"));
        QVERIFY(popover);
        QTRY_VERIFY2(popover->property("opened").toBool(),
                     "the facepile click did not open the reader list");

        escape();
        QTRY_VERIFY(!popover->property("visible").toBool());
        QCOMPARE(m_controller->currentRoomId(), kRoom);

        escape();
        QCOMPARE(m_controller->currentRoomId(), QString());
    }

    // Every popup a timeline row opens owns Escape even when the keyboard
    // focus is somewhere else under the shell (here the message box): the
    // first Escape closes the popup and only the next one leaves the room.
    void anOpenTimelinePopupKeepsEscapeWithoutTheFocus_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("receipt list") << QStringLiteral("receiptListPopover");
        QTest::newRow("profile card") << QStringLiteral("senderProfilePopover");
        QTest::newRow("reaction picker") << QStringLiteral("sharedReactionPicker");
    }
    void anOpenTimelinePopupKeepsEscapeWithoutTheFocus()
    {
        QFETCH(QString, name);
        openRoomWithComposerFocused();
        QObject *popup = item("mainScreen")->findChild<QObject *>(name);
        QVERIFY2(popup, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        // Wherever the open left the focus, put it back in the message box.
        QQuickItem *composer = item("messageComposer");
        QVERIFY(QMetaObject::invokeMethod(composer, "focusEditor"));
        QTest::qWait(20);
        QVERIFY(m_window->activeFocusItem());

        escape();
        QTRY_VERIFY2(!popup->property("visible").toBool(),
                     "Escape did not close the popup");
        QCOMPARE(m_controller->currentRoomId(), kRoom);
    }

    // A hover tooltip is not an Escape owner: it must not cost the user a
    // keypress.
    void aToolTipDoesNotKeepEscape()
    {
        openRoomWithComposerFocused();
        QQmlComponent component(m_engine);
        component.setData("import QtQuick.Controls\nToolTip { text: \"tip\" }\n",
                          QUrl(QStringLiteral("escapetooltip.qml")));
        std::unique_ptr<QObject> tip(component.create());
        QVERIFY2(tip, qPrintable(component.errorString()));
        tip->setProperty("parent", QVariant::fromValue(item("messageComposer")));
        QVERIFY(QMetaObject::invokeMethod(tip.get(), "open"));
        QTRY_VERIFY(tip->property("visible").toBool());

        escape();
        QCOMPARE(m_controller->currentRoomId(), QString());
    }

private:
    QQuickItem *visibleReceiptRow() const
    {
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            if (it->objectName() == QLatin1String("readReceiptRow")
                && it->isVisible() && it->width() > 0) {
                const QPointF c = it->mapToScene(
                    QPointF(it->width() / 2, it->height() / 2));
                if (c.y() > 0 && c.y() < m_window->height())
                    return it;
            }
            stack.append(it->childItems());
        }
        return nullptr;
    }
};

QTEST_MAIN(EscapeNavigationQmlTest)
#include "EscapeNavigationQmlTest.moc"
