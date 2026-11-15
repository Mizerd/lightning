// A user link opens the profile card, not Discover.

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
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

}

class UserLinkProfileQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;

    QObject *popover() const
    {
        return m_root->findChild<QObject *>(QStringLiteral("inviterProfilePopover"));
    }

    // The link the Discover dialog was last asked to open.
    QString discoverLink() const
    {
        const auto objects = m_root->findChildren<QObject *>();
        for (QObject *object : objects) {
            if (object->metaObject()->indexOfProperty("activatedLink") >= 0)
                return object->property("activatedLink").toString();
        }
        return QStringLiteral("<no Discover dialog>");
    }

    void closePopover()
    {
        QVERIFY(popover());
        QMetaObject::invokeMethod(popover(), "close");
        QTRY_VERIFY(!popover()->property("visible").toBool());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("user-link-profile-qml-test"));

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
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("userlink.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(80);
        QVERIFY2(popover(), "the room list's no-room profile card was not found");
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void theUserIdIsReadFromEitherLinkForm_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("userId");
        QTest::newRow("uri") << "matrix:u/bob:mock.local" << "@bob:mock.local";
        QTest::newRow("uri with sigil") << "matrix:u/@bob:mock.local"
                                        << "@bob:mock.local";
        QTest::newRow("uri with action") << "matrix:u/bob:mock.local?action=chat"
                                         << "@bob:mock.local";
        QTest::newRow("matrix.to") << "https://matrix.to/#/@bob:mock.local"
                                   << "@bob:mock.local";
        QTest::newRow("matrix.to encoded")
            << "https://matrix.to/#/%40bob%3Amock.local" << "@bob:mock.local";
        QTest::newRow("room uri") << "matrix:r/general:mock.local" << "";
        QTest::newRow("room id uri") << "matrix:roomid/general:mock.local" << "";
        QTest::newRow("matrix.to room") << "https://matrix.to/#/#general:mock.local"
                                        << "";
        QTest::newRow("no server") << "matrix:u/bob" << "";
        QTest::newRow("empty server") << "matrix:u/bob:" << "";
        QTest::newRow("space") << "matrix:u/bo%20b:mock.local" << "";
        QTest::newRow("control") << "matrix:u/bo%07b:mock.local" << "";
        QTest::newRow("hash") << "matrix:u/bob:mock.local%23x" << "";
        QTest::newRow("question") << "matrix:u/bob:mock.local%3Fx" << "";
        QTest::newRow("slash") << "matrix:u/bob:mock.local%2Fx" << "";
        QTest::newRow("web") << "https://example.org/@bob:mock.local" << "";
    }

    void theUserIdIsReadFromEitherLinkForm()
    {
        QFETCH(QString, link);
        QFETCH(QString, userId);
        QCOMPARE(m_controller->userIdFromMatrixLink(link), userId);
    }

    void aUserLinkOpensTheProfileCardNotDiscover_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<bool>("external");
        QTest::newRow("clicked uri") << "matrix:u/bob:mock.local" << false;
        QTest::newRow("clicked matrix.to")
            << "https://matrix.to/#/@bob:mock.local" << false;
        QTest::newRow("command line") << "matrix:u/bob:mock.local" << true;
    }

    void aUserLinkOpensTheProfileCardNotDiscover()
    {
        QFETCH(QString, link);
        QFETCH(bool, external);
        if (external)
            m_controller->queueExternalMatrixLink(link);
        else
            m_controller->openMatrixLink(link);
        QTRY_VERIFY2(popover()->property("visible").toBool(),
                     "a link to a person did not open their profile card");
        QCOMPARE(popover()->property("userId").toString(),
                 QStringLiteral("@bob:mock.local"));
        QCOMPARE(popover()->property("roomScoped").toBool(), false);
        QCOMPARE(discoverLink(), QString());
        closePopover();
    }

    void aRoomLinkStillGoesToDiscover()
    {
        const QString link = QStringLiteral("matrix:r/nowhere:mock.local");
        m_controller->openMatrixLink(link);
        QCOMPARE(discoverLink(), link);
        QVERIFY(!popover()->property("visible").toBool());
    }
};

QTEST_MAIN(UserLinkProfileQmlTest)
#include "UserLinkProfileQmlTest.moc"
