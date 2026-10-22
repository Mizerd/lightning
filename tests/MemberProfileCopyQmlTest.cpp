// MemberProfilePopover: the Matrix ID's double-click copy, with a real mouse.
// Selection goes through the shared
// UserPicker's userSelected signal (as the production handler is wired),
// bypassing search debounce and network, against a real
// AppController(MockBackend). The invited ids are absent from the mock
// room's member map, so the already-member pre-check never engages.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "auth/AuthManager.h"

namespace {

constexpr int kSignalTimeoutMs = 5000;

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    MemberProfilePopover {
        objectName: "profile"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class MemberProfileCopyQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;
    QStringList m_warnings;

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
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    // Emits the shared UserPicker's userSelected(userId, displayName,
    // avatarUrl) directly, as a click or Enter on a result row would.
    void selectUser(const QString &userId, const QString &displayName)
    {
        auto *picker = m_window->findChild<QObject *>(
            QStringLiteral("invitePeoplePicker"));
        QVERIFY(picker);
        // QML-declared signals take exact QString parameters; QVariant args
        // make invokeMethod miss the overload.
        QVERIFY(QMetaObject::invokeMethod(
            picker, "userSelected",
            Q_ARG(QString, userId),
            Q_ARG(QString, displayName),
            Q_ARG(QString, QString())));
        QCoreApplication::processEvents();
    }

    ConversationController *conversations() const
    {
        return m_controller->conversations();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("member-profile-copy-qml-test"));
        QSettings().clear();
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("scene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_dialog = m_root->findChild<QObject *>(QStringLiteral("profile"));
        QVERIFY(m_dialog);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void copyUserIdWritesTheClipboard()
    {
        QVariantMap member;
        member.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "copyUserId"));
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("@bob:mock.local"));
    }

    void doubleClickOnTheMatrixIdCopiesIt()
    {
        QVariantMap member;
        member.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        member.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));

        auto *id = item("profileUserId");
        QVERIFY(id);
        QTRY_VERIFY(id->isVisible() && id->width() > 0);
        const QPoint p = id->mapToScene(QPointF(id->width() / 2, id->height() / 2))
                             .toPoint();
        QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, p);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(),
                     QStringLiteral("@bob:mock.local"));
        auto *notice = item("profileCopiedNotice");
        QVERIFY(notice);
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->property("text").toString(),
                 QStringLiteral("Matrix ID copied"));
    }
};

QTEST_MAIN(MemberProfileCopyQmlTest)
#include "MemberProfileCopyQmlTest.moc"
