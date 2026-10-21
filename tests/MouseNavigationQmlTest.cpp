// Mouse Back/Forward buttons and Alt+Left/Right walk the rooms opened this
// session, in the real shell on the mock backend:
//   * Back and Forward revisit rooms, a new open after Back drops Forward;
//   * the buttons work over the timeline, the message box and the room-list
//     search, and a text field keeps its focus;
//   * nothing navigates while a dialog is open, and a Back press that
//     dismisses a click-away menu only dismisses it;
//   * a latched middle-click autoscroll does not survive the room change;
//   * a room the client does not list is skipped;
//   * the keyboard shortcuts do the same, and stay out of the composer's
//     completion popups, which Qt's own shortcut rules let through.
// Events are sent to the window, so delivery follows real hit-testing.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "auth/AuthManager.h"

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

const QString kGeneral = QStringLiteral("!general:mock.local");
const QString kDevs = QStringLiteral("!devs:mock.local");
const QString kDm = QStringLiteral("!dm-bob:mock.local");
// Not in the mock's room list: stands for a room that was left or forgotten.
const QString kGhost = QStringLiteral("!ghost:mock.local");

} // namespace

class MouseNavigationQmlTest : public QObject
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

    QPoint centerOf(QQuickItem *it) const
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2))
            .toPoint();
    }

    void click(Qt::MouseButton button, const QPoint &at)
    {
        QTest::mouseClick(m_window, button, Qt::NoModifier, at);
        QTest::qWait(30);
    }

    // A point inside the conversation pane, away from controls.
    QPoint timelinePoint() const
    {
        QQuickItem *composer = item("messageComposer");
        const QPoint c = centerOf(composer);
        return QPoint(c.x(), c.y() - 300);
    }

    // The room-history stack as the user left it: general, devs, dm.
    void openThreeRooms()
    {
        m_controller->setCurrentRoomId(kGeneral);
        m_controller->setCurrentRoomId(kDevs);
        m_controller->setCurrentRoomId(kDm);
        QTest::qWait(60);
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("mouse-navigation-qml-test"));

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
                          QUrl(QStringLiteral("mousenavigation.qml")));
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
        // Start every case with an empty history and no room.
        m_controller->openLobby();
        m_controller->clearRoomHistoryForTest();
        QVERIFY(m_controller->currentRoomId().isEmpty());
    }

    void backAndForwardButtonsWalkTheOpenedRooms()
    {
        openThreeRooms();
        const QPoint at = timelinePoint();

        click(Qt::BackButton, at);
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        click(Qt::BackButton, at);
        QCOMPARE(m_controller->currentRoomId(), kGeneral);
        // Nothing older: stays put.
        click(Qt::BackButton, at);
        QCOMPARE(m_controller->currentRoomId(), kGeneral);

        click(Qt::ForwardButton, at);
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        click(Qt::ForwardButton, at);
        QCOMPARE(m_controller->currentRoomId(), kDm);
        click(Qt::ForwardButton, at);
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

    void aNewOpenAfterGoingBackDropsTheForwardList()
    {
        openThreeRooms();
        click(Qt::BackButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        m_controller->openRoom(kGeneral);
        QCOMPARE(m_controller->currentRoomId(), kGeneral);
        QVERIFY(!m_controller->navigateRoomForward());
        QCOMPARE(m_controller->currentRoomId(), kGeneral);
        // Back from there is the room it was opened from.
        QVERIFY(m_controller->navigateRoomBack());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    void aHistoryMoveIsNotRecordedAsANewVisit()
    {
        openThreeRooms();
        QVERIFY(m_controller->navigateRoomBack());
        QVERIFY(m_controller->navigateRoomBack());
        // Had the moves been recorded, Forward would be gone.
        QVERIFY(m_controller->navigateRoomForward());
        QVERIFY(m_controller->navigateRoomForward());
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

    void aRoomTheClientDoesNotListIsSkipped()
    {
        m_controller->setCurrentRoomId(kGeneral);
        m_controller->setCurrentRoomId(kGhost);
        m_controller->setCurrentRoomId(kDm);
        QVERIFY(m_controller->navigateRoomBack());
        QCOMPARE(m_controller->currentRoomId(), kGeneral);
        QVERIFY(m_controller->navigateRoomForward());
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

    void backFromTheLobbyReturnsToTheRoomJustLeft()
    {
        m_controller->setCurrentRoomId(kGeneral);
        m_controller->setCurrentRoomId(kDevs);
        m_controller->openLobby();
        QVERIFY(m_controller->currentRoomId().isEmpty());
        click(Qt::BackButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    void theButtonsWorkOverTheMessageBoxAndKeepItsFocus()
    {
        openThreeRooms();
        QQuickItem *composer = item("messageComposer");
        QVERIFY(composer);
        QVERIFY(QMetaObject::invokeMethod(composer, "focusEditor"));
        QTest::qWait(20);
        QQuickItem *focused = m_window->activeFocusItem();
        QVERIFY(focused);

        click(Qt::BackButton, centerOf(composer));
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        // The navigation is queued after the release; let it and anything it
        // schedules run before judging the focus.
        QTest::qWait(60);
        QCOMPARE(m_window->activeFocusItem(), focused);
    }

    void theButtonsWorkOverTheRoomListSearch()
    {
        openThreeRooms();
        QQuickItem *field = nullptr;
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty() && !field) {
            QQuickItem *it = stack.takeLast();
            if (it->property("placeholderText").toString()
                    == QStringLiteral("Search")
                && it->isVisible())
                field = it;
            stack.append(it->childItems());
        }
        QVERIFY2(field, "the room-list search field was not found");
        click(Qt::BackButton, centerOf(field));
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    void otherButtonsDoNotNavigate()
    {
        openThreeRooms();
        click(Qt::MiddleButton, timelinePoint());
        click(Qt::RightButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

    void nothingNavigatesWhileADialogIsOpen()
    {
        openThreeRooms();
        QQuickItem *panel = item("mainScreen");
        QVERIFY(panel);
        QObject *dialog =
            panel->findChild<QObject *>(QStringLiteral("leaveRoomConfirmDialog"));
        QVERIFY2(dialog, "no dialog to open");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(m_controller->hasOpenPopup(panel));

        click(Qt::BackButton, timelinePoint());
        click(Qt::BackButton, QPoint(5, 5));
        QCOMPARE(m_controller->currentRoomId(), kDm);

        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(!m_controller->hasOpenPopup(panel));
        click(Qt::BackButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    // A browser's Back on an open menu only closes the menu. Every AppMenu is
    // a non-modal Menu that closes on a press outside, DURING the delivery of
    // that press, so only a gate asked before delivery (the window filter)
    // can tell. Fails if the filter stops asking hasOpenPopup().
    void aBackPressThatDismissesAMenuOnlyDismissesIt()
    {
        openThreeRooms();
        QObject *menu =
            m_root->findChild<QObject *>(QStringLiteral("roomListAddMenu"));
        QVERIFY2(menu, "no room-list menu to open");
        QVERIFY(!menu->property("modal").toBool());
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(m_controller->hasOpenPopup(item("mainScreen")));

        click(Qt::BackButton, timelinePoint());
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTest::qWait(30);
        QCOMPARE(m_controller->currentRoomId(), kDm);

        // Nothing open now: the next press navigates.
        click(Qt::BackButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    // A latched autoscroll ends on the Back press itself (any button ends a
    // latch), and the room change ends it too, for the keyboard path that no
    // press reaches (TimelinePane's onCurrentRoomIdChanged).
    void aLatchedAutoscrollDoesNotSurviveTheRoomChange()
    {
        openThreeRooms();
        QQuickItem *scroller = item("timelineMiddleClickScroller");
        QVERIFY(scroller);
        QVERIFY(scroller->setProperty("latched", true));
        QVERIFY(scroller->property("active").toBool());
        click(Qt::BackButton, timelinePoint());
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        QVERIFY(!scroller->property("active").toBool());

        QVERIFY(scroller->setProperty("latched", true));
        QVERIFY(m_controller->navigateRoomBack());
        QCOMPARE(m_controller->currentRoomId(), kGeneral);
        QTRY_VERIFY(!scroller->property("active").toBool());
    }

    void altLeftAndAltRightDoTheSame()
    {
        openThreeRooms();
        QTest::keyClick(m_window, Qt::Key_Left, Qt::AltModifier);
        QTest::qWait(30);
        QCOMPARE(m_controller->currentRoomId(), kDevs);
        QTest::keyClick(m_window, Qt::Key_Right, Qt::AltModifier);
        QTest::qWait(30);
        QCOMPARE(m_controller->currentRoomId(), kDm);
    }

    // The composer's mention popup (like the emoji-completion and slash
    // ones) is non-modal and NoAutoClose, so Qt's Shortcut context does NOT
    // block a window shortcut under it; only the C++ gate does. Fails if the
    // Shortcuts call navigateRoomBack() directly again.
    void altLeftDoesNotNavigateUnderAMentionPopup()
    {
        openThreeRooms();
        QObject *mention = nullptr;
        const auto all = m_root->findChildren<QObject *>();
        for (QObject *o : all) {
            if (QByteArray(o->metaObject()->className())
                    .startsWith("MentionPopup")) {
                mention = o;
                break;
            }
        }
        QVERIFY2(mention, "the composer's MentionPopup was not found");
        QVERIFY(!mention->property("modal").toBool());
        constexpr int kCloseOnEscape = 0x10; // Popup.CloseOnEscape
        QVERIFY2(!(mention->property("closePolicy").toInt() & kCloseOnEscape),
                 "the case needs a popup Qt's shortcut rules do not block");
        QVERIFY(QMetaObject::invokeMethod(mention, "open"));
        QTRY_VERIFY(mention->property("visible").toBool());
        QVERIFY(m_controller->hasOpenPopup(item("mainScreen")));

        QTest::keyClick(m_window, Qt::Key_Left, Qt::AltModifier);
        QTest::qWait(30);
        QCOMPARE(m_controller->currentRoomId(), kDm);

        QVERIFY(QMetaObject::invokeMethod(mention, "close"));
        QTRY_VERIFY(!mention->property("visible").toBool());
        QTest::keyClick(m_window, Qt::Key_Left, Qt::AltModifier);
        QTest::qWait(30);
        QCOMPARE(m_controller->currentRoomId(), kDevs);
    }

    void nothingNavigatesUnderSettings()
    {
        openThreeRooms();
        m_controller->showSettings();
        QVERIFY(m_controller->currentScreen() == AppController::SettingsScreen);
        QVERIFY(!m_controller->navigateRoomBack());
        QCOMPARE(m_controller->currentRoomId(), kDm);
        m_controller->showMain();
    }
};

QTEST_MAIN(MouseNavigationQmlTest)
#include "MouseNavigationQmlTest.moc"
