// v0.6.5: proof for the redesigned InvitePeopleDialog token-field chips
// (SPEC 1t). This dialog previously had zero dedicated coverage — only
// ThemeTokensTest's hex-literal scan touched the file at all.
//
// Selection is driven directly through the shared UserPicker's userSelected
// signal (bypassing live search debounce/network, exactly like the
// production onUserSelected handler is wired) so the test stays fast and
// deterministic while still exercising the real chip/count/dispatch path
// against a real AppController(MockBackend) — no FakeClient needed, since
// nothing here depends on search results or member-list fetch timing: the
// invited ids are fabricated identities absent from the mock room's member
// map, so the already-member pre-check (unchanged by this restyle) never
// engages either path.

#include <QtTest/QtTest>

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

    InvitePeopleDialog {
        objectName: "inviteDialog"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class InvitePeopleDialogQmlTest : public QObject
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

    // Simulates the shared UserPicker choosing a result: emits its
    // userSelected(userId, displayName, avatarUrl) signal directly, exactly
    // as a real click/Enter on a search result row would.
    void selectUser(const QString &userId, const QString &displayName)
    {
        auto *picker = m_window->findChild<QObject *>(
            QStringLiteral("invitePeoplePicker"));
        QVERIFY(picker);
        // QML-declared signals expose exact QString parameters — QVariant
        // args make invokeMethod miss the overload entirely.
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
            QStringLiteral("invite-people-dialog-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());

        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("invitescene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();

        m_dialog = m_root->findChild<QObject *>(QStringLiteral("inviteDialog"));
        QVERIFY(m_dialog);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void loadsWithoutQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    void chipsRenderAndButtonTextTracksCount()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(QStringLiteral("!general:mock.local")))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());

        auto *submit = item("invitePeopleSubmitButton");
        QVERIFY(submit);
        QVERIFY(!submit->property("enabled").toBool());

        // "@erin"/"@frank" are absent from the mock room's member map, so
        // this exercises the "new invite" branch, not the already-member
        // short-circuit (unchanged by this restyle; not re-tested here).
        selectUser(QStringLiteral("@erin:mock.local"), QStringLiteral("Erin"));
        auto *chip0 = item("inviteChip_0");
        QVERIFY(chip0);
        QVERIFY(chip0->isVisible());
        QVERIFY(submit->property("enabled").toBool());
        QCOMPARE(submit->property("text").toString(), QStringLiteral("Invite"));

        selectUser(QStringLiteral("@frank:mock.local"), QString());
        auto *chip1 = item("inviteChip_1");
        QVERIFY(chip1);
        QCOMPARE(submit->property("text").toString(),
                 QStringLiteral("Invite 2 people"));

        // Removing a chip drops the count back to a single invite.
        auto *remove0 = item("inviteChipRemove_0");
        QVERIFY(remove0);
        QMetaObject::invokeMethod(remove0, "click");
        QTRY_VERIFY(!item("inviteChip_1"));
        QCOMPARE(submit->property("text").toString(), QStringLiteral("Invite"));

        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    void submitDispatchesThePlainUserIdList()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(QStringLiteral("!general:mock.local")))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());

        selectUser(QStringLiteral("@erin:mock.local"), QStringLiteral("Erin"));
        selectUser(QStringLiteral("@frank:mock.local"), QStringLiteral("Frank"));

        auto *submit = item("invitePeopleSubmitButton");
        QVERIFY(submit);
        QTRY_VERIFY(submit->property("enabled").toBool());
        QMetaObject::invokeMethod(submit, "click");

        // The mock backend honestly reports outgoing invites unsupported
        // (MatrixClient::inviteUsers base returns opId 0), so per-user
        // pending rows never populate here. What the click MUST prove on
        // this backend: the {id,name,avatar} chips were reduced to a plain
        // non-empty id list and the controller call went through to the
        // backend gate — that exact path sets the not-supported error.
        QTRY_VERIFY(!conversations()->errorMessage().isEmpty());
        QVERIFY(conversations()->errorMessage().contains(
            QStringLiteral("not supported")));
        QVERIFY(conversations()->inviteResults().isEmpty());
        // And the reduction itself stays pinned in source: ids only,
        // never the chip objects.
        QFile dialogFile(QStringLiteral(QML_DIR "/InvitePeopleDialog.qml"));
        QVERIFY(dialogFile.open(QIODevice::ReadOnly));
        const QString dialogSrc = QString::fromUtf8(dialogFile.readAll());
        QVERIFY(dialogSrc.contains(QStringLiteral(
            "root.selectedUsers.map(function(u) { return u.id })")));

        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    InvitePeopleDialogQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "InvitePeopleDialogQmlTest.moc"
