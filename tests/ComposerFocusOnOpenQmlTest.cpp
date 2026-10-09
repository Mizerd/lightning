// A room opened by clicking it is ready to type in (GitHub #29), in the real
// shell on the mock backend:
//   * a mouse click on a room-list row (Channels and Classic layouts) puts the
//     keyboard in the message box;
//   * a room opened without a pointer (a notification, app.openRoom) leaves
//     the keyboard where it was, so someone typing elsewhere keeps typing;
//   * keyboard activation of a row (Space) opens the room without moving the
//     keyboard out of the list;
//   * a request for a room that is not the open one does nothing.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
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

const QString kRoom = QStringLiteral("!dm-bob:mock.local"); // a DM: shown at Home in both layouts
const QString kOther = QStringLiteral("!devs:mock.local");

} // namespace

class ComposerFocusOnOpenQmlTest : public QObject
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

    // The visible room-list row for `roomId`: a ChannelDelegate (has
    // channelName) or a RoomDelegate (has showGroupDivider).
    QQuickItem *row(const QString &roomId) const
    {
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            const bool isRow = it->metaObject()->indexOfProperty("channelName") >= 0
                || it->metaObject()->indexOfProperty("showGroupDivider") >= 0;
            if (isRow && it->isVisible() && it->width() > 0
                && it->property("roomId").toString() == roomId)
                return it;
            stack.append(it->childItems());
        }
        return nullptr;
    }

    bool composerHasFocus() const
    {
        QQuickItem *composer = findItem(m_window->contentItem(),
                                        QStringLiteral("messageComposer"));
        QQuickItem *focused = m_window->activeFocusItem();
        for (QQuickItem *it = focused; it; it = it->parentItem()) {
            if (it == composer)
                return true;
        }
        return false;
    }

    QQuickItem *roomSearchField() const
    {
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            if (it->property("placeholderText").toString() == QStringLiteral("Search")
                && it->isVisible())
                return it;
            stack.append(it->childItems());
        }
        return nullptr;
    }

    // Home, no room, keyboard parked on the shell: the start of every case.
    void reset(int layout)
    {
        m_controller->settings()->setRoomNavigationLayout(layout);
        m_controller->openSpaceHome(QString());
        QTest::qWait(120);
        QQuickItem *shell = findItem(m_window->contentItem(), QStringLiteral("mainScreen"));
        QVERIFY(shell);
        shell->forceActiveFocus();
        QTest::qWait(20);
        QVERIFY(!composerHasFocus());
    }

    void clickRow(const QString &roomId)
    {
        QQuickItem *target = row(roomId);
        QVERIFY2(target, qPrintable(QStringLiteral("no visible row for ") + roomId));
        const QPointF centre = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, {}, centre.toPoint());
        // The focus lands on the next event-loop turn (Qt.callLater).
        QTest::qWait(60);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("composer-focus-on-open-qml-test"));

        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(3000));

        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("composerfocus.qml")));
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

    void aClickedRowFocusesTheMessageBox_data()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("channels") << 1;
        QTest::newRow("classic") << 0;
    }

    void aClickedRowFocusesTheMessageBox()
    {
        QFETCH(int, layout);
        reset(layout);
        clickRow(kRoom);
        QCOMPARE(m_controller->currentRoomId(), kRoom);
        QVERIFY2(composerHasFocus(),
                 "a room opened by a click does not put the keyboard in the "
                 "message box");
    }

    // A notification opens rooms through app.openRoom(): the keyboard stays
    // with whatever the user was typing in.
    void aRoomOpenedWithoutAPointerLeavesTheKeyboardAlone()
    {
        reset(1);
        QQuickItem *field = roomSearchField();
        QVERIFY2(field, "the room-list search field was not found");
        field->forceActiveFocus();
        QTest::qWait(20);
        QCOMPARE(m_window->activeFocusItem(), field);

        m_controller->openRoom(kOther);
        QTest::qWait(400); // past the composer's settle retry
        QCOMPARE(m_controller->currentRoomId(), kOther);
        QCOMPARE(m_window->activeFocusItem(), field);
    }

    // Space on a focused Channels row activates it like a click, but the
    // keyboard stays in the list for the next arrow or Space.
    void keyboardActivationKeepsTheKeyboardInTheList()
    {
        reset(1);
        QQuickItem *target = row(kRoom);
        QVERIFY(target);
        target->forceActiveFocus();
        QTest::qWait(20);
        QVERIFY(target->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Space);
        QTest::qWait(400);
        QCOMPARE(m_controller->currentRoomId(), kRoom);
        QVERIFY2(!composerHasFocus(),
                 "keyboard activation moved the keyboard into the message box");
    }

    void aRequestForAnotherRoomDoesNothing()
    {
        reset(1);
        m_controller->openRoom(kRoom);
        QTest::qWait(60);
        QSignalSpy requested(m_controller, &AppController::composerFocusRequested);
        m_controller->requestComposerFocus(kOther);
        QCOMPARE(requested.count(), 0);
        m_controller->requestComposerFocus(kRoom);
        QCOMPARE(requested.count(), 1);
    }
};

QTEST_MAIN(ComposerFocusOnOpenQmlTest)
#include "ComposerFocusOnOpenQmlTest.moc"
