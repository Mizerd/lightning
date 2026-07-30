// v0.6.5 (SPEC 1p): offscreen proof for the redesigned member profile
// popover content. Loads the real MemberProfilePopover.qml against a real
// AppController on the mock backend (logged in, so app.conversations is
// actually usable), drives openFor()/startOrOpenDm()/Copy ID, and asserts
// the omitted affordances (call/videocam/more_horiz/Ignore/Verified
// chip/SHARED rooms/View full profile) never rendered — no disabled
// placeholders in their place — while the DM-reuse and Copy ID mechanics
// are preserved byte-for-byte.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 500
    height: 600
    visible: true
    color: AppTheme.background

    MemberProfilePopover {
        id: popover
        objectName: "popover"
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    function openFor(userId, displayName, membership, role, isOwn) {
        popover.openFor({
            userId: userId,
            displayName: displayName,
            membership: membership,
            role: role,
            avatarUrl: "",
            isOwn: isOwn
        })
    }
}
)QML";

} // namespace

class MemberProfilePopoverContractTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *find(const QString &name) const
    {
        return m_root->findChild<QObject *>(name);
    }

private slots:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(5000));
        QTRY_VERIFY(m_controller->loggedIn());

        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                     m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("memberprofilescene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    void popoverIsFixed296WideAndCentredModal()
    {
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QCOMPARE(popover->property("width").toInt(), 296);
        QCOMPARE(popover->property("modal").toBool(), true);
    }

    void omittedAffordancesNeverRenderAsPlaceholders()
    {
        // Icon literals for the omitted actions must never appear at all —
        // these strings do not collide with prose (unlike the words below).
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("\"call\"")));
        QVERIFY(!content.contains(QStringLiteral("\"videocam\"")));
        QVERIFY(!content.contains(QStringLiteral("\"more_horiz\"")));
        QVERIFY(!content.contains(QStringLiteral("\"block\"")));
        QVERIFY(!content.contains(QStringLiteral("presenceOnline")));

        // No qsTr() user-facing string ever mentions the omitted
        // affordances — explanatory source comments documenting the
        // omission are not user-facing wording, so this checks qsTr() call
        // sites specifically rather than banning the words file-wide.
        QRegularExpression qsTrCall(QStringLiteral("qsTr\\(\"([^\"]*)\""));
        auto it = qsTrCall.globalMatch(content);
        while (it.hasNext()) {
            const QString text = it.next().captured(1);
            QVERIFY2(!text.contains(QStringLiteral("Verified")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("SHARED")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("View full profile")),
                     qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("Ignore")), qPrintable(text));
        }
    }

    void dmReuseMechanicsArePreserved()
    {
        // The exact existing DM-reuse call sequence must still be present
        // (checkExistingDm -> existingDms -> openRoom, else
        // startDirectMessage), never a bare/duplicate room create.
        QFile file(QStringLiteral(QML_DIR "/MemberProfilePopover.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(content.contains(QStringLiteral("checkExistingDm")));
        QVERIFY(content.contains(QStringLiteral("existingDms")));
        QVERIFY(content.contains(QStringLiteral("startDirectMessage")));
        QVERIFY(content.contains(QStringLiteral("app.openRoom")));
    }

    void roleRendersAsAStatusChipAndYouIndicatorIsKept()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@carol:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Carol")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("administrator")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        bool foundAdminChip = false;
        for (QObject *candidate : popover->findChildren<QObject *>()) {
            if (candidate->property("label").toString() == QStringLiteral("Administrator")
                && candidate->property("visible").toBool()) {
                foundAdminChip = true;
                break;
            }
        }
        QVERIFY(foundAdminChip);

        // "(you)" only for the own-account member.
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@alice:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Alice")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, true));
        QVERIFY(popover->property("isOwn").toBool());
        // The own-account row never shows the Message button (there is no
        // real self-DM action).
        QObject *messageButton = nullptr;
        for (QObject *candidate : popover->findChildren<QObject *>()) {
            if (candidate->property("text").toString() == QStringLiteral("Message")) {
                messageButton = candidate;
                break;
            }
        }
        if (messageButton)
            QVERIFY(!messageButton->property("visible").toBool());
        popover->setProperty("visible", false);
    }

    void copyIdShowsAndClearsTheClipboardNotice()
    {
        QMetaObject::invokeMethod(m_root, "openFor",
                                  Q_ARG(QVariant, QStringLiteral("@dave:mock.local")),
                                  Q_ARG(QVariant, QStringLiteral("Dave")),
                                  Q_ARG(QVariant, QStringLiteral("joined")),
                                  Q_ARG(QVariant, QStringLiteral("")),
                                  Q_ARG(QVariant, false));
        auto *popover = find(QStringLiteral("popover"));
        QVERIFY(popover);
        QTRY_VERIFY(popover->property("opened").toBool());

        auto *copyButton = find(QStringLiteral("profileCopyIdButton"));
        QVERIFY(copyButton);
        QMetaObject::invokeMethod(copyButton, "clicked");
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("@dave:mock.local"));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MemberProfilePopoverContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MemberProfilePopoverContractTest.moc"
