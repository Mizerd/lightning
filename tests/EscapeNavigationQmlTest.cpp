// Escape steps back out of a conversation (GitHub #27), in the real shell:
//   * Escape from the message box closes the room to its Space's overview,
//     a second Escape returns to Home, and a third does nothing;
//   * the room stops being the timeline's room, so nothing more is marked
//     read (ReadReceiptCoordinator only ever sends for the model's room);
//   * every earlier owner of Escape keeps it: a reply or an edit in the
//     composer, another text field, and an open dialog.
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
};

QTEST_MAIN(EscapeNavigationQmlTest)
#include "EscapeNavigationQmlTest.moc"
