// InvitePeopleDialog token-field chips. Selection goes through the shared
// UserPicker's userSelected signal (as the production handler is wired),
// bypassing search debounce and network, against a real
// AppController(MockBackend). The invited ids are absent from the mock
// room's member map, so the already-member pre-check never engages.

#include <QtTest/QtTest>

#include <functional>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

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

        // "@erin"/"@frank" are not room members, so this is the new-invite
        // branch.
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

    // The roster check must read the room being invited TO. Inviting into a
    // Space from the rail while another room is open used to consult the open
    // room's members, refusing its members as "already in" the Space.
    void theAlreadyMemberCheckReadsTheRoomBeingInvitedTo()
    {
        // This scene contains only the dialog, so populate the roster that
        // TimelinePane/RoomInfoPanel normally loads. The mock backend does
        // not advertise room management or provide normalized memberships.
        struct RosterClient : MockMatrixClient {
            bool supportsRoomManagement() const override { return true; }
            quint64 requestRoomMembers(const QString &) override { return 1; }
        } roster;
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *info = m_controller->roomInfo();
        auto *original = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(original);
        const auto restore = qScopeGuard([&] {
            info->setClient(original);
            info->setRoomId(QString());
        });
        info->setClient(&roster);
        info->setRoomId(QStringLiteral("!general:mock.local"));
        Q_EMIT roster.roomMembersReceived(1, info->roomId(), QVariantMap{
            {QStringLiteral("ok"), true},
            {QStringLiteral("members"), QVariantList{QVariantMap{
                {QStringLiteral("userId"), QStringLiteral("@bob:mock.local")},
                {QStringLiteral("membership"), QStringLiteral("joined")}}}}});
        QTRY_COMPARE(info->roomId(), QStringLiteral("!general:mock.local"));
        QTRY_VERIFY(!info->members().isEmpty());
        bool bobIsMember = false;
        for (const QVariant &row : info->members())
            bobIsMember |= row.toMap().value(QStringLiteral("userId")).toString()
                           == QStringLiteral("@bob:mock.local")
                           && row.toMap().value(QStringLiteral("membership")).toString()
                              == QStringLiteral("joined");
        QVERIFY2(bobIsMember, "fixture: bob must be joined in the open room");

        // A different target (a Space): bob is not known to be in it.
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(QStringLiteral("!space:mock.local")))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        selectUser(QStringLiteral("@bob:mock.local"), QStringLiteral("Bob"));
        QVERIFY2(item("inviteChip_0"),
                 "the open room's roster must not veto an invite to another room");
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());

        // The room's own roster still does.
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(QStringLiteral("!general:mock.local")))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        selectUser(QStringLiteral("@bob:mock.local"), QStringLiteral("Bob"));
        QVERIFY(!item("inviteChip_0"));
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    // Master prompt Part 6, 5 (UI-029/030/033/040): an empty search offers
    // the account's recent one-to-one conversations; picking one only adds a
    // chip, never an invite; a picked person leaves the list; Return on an
    // empty search with nothing highlighted picks nobody.
    void anEmptySearchSuggestsRecentConversationsAndOnlySelects()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(QStringLiteral("!space:mock.local")))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        auto *picker = m_window->findChild<QObject *>(
            QStringLiteral("invitePeoplePicker"));
        QVERIFY(picker);
        const QVariantList suggestions = picker->property("suggestions").toList();
        QStringList ids;
        for (const QVariant &v : suggestions)
            ids << v.toMap().value(QStringLiteral("userId")).toString();
        QVERIFY2(ids.contains(QStringLiteral("@bob:mock.local")),
                 qPrintable(ids.join(QLatin1Char(','))));
        QVERIFY(!ids.contains(QStringLiteral("@alice:mock.local")));
        auto *list = item("invitePeoplePickerSuggestions");
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible());

        // Return with nothing highlighted selects nobody.
        auto *field = item("invitePeoplePickerSearchField");
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Return);
        QVERIFY(!item("inviteChip_0"));

        const int bobRow = int(ids.indexOf(QStringLiteral("@bob:mock.local")));
        QQuickItem *row = nullptr;
        for (int i = 0; i < 20 && !row; ++i) {
            QList<QQuickItem *> all;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
                if (it->objectName() == QStringLiteral("userPickerSuggestion_%1").arg(bobRow))
                    all << it;
                for (QQuickItem *c : it->childItems())
                    walk(c);
            };
            walk(list);
            row = all.isEmpty() ? nullptr : all.first();
            if (!row)
                QTest::qWait(20);
        }
        QVERIFY(row);
        QMetaObject::invokeMethod(row, "click");
        QCoreApplication::processEvents();
        QVERIFY2(item("inviteChip_0"), "a suggestion click did not add a chip");
        QVERIFY2(conversations()->inviteResults().isEmpty()
                     && conversations()->errorMessage().isEmpty(),
                 "a suggestion click must never send an invite");
        QStringList after;
        for (const QVariant &v : picker->property("suggestions").toList())
            after << v.toMap().value(QStringLiteral("userId")).toString();
        QVERIFY(!after.contains(QStringLiteral("@bob:mock.local")));

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

        // The mock reports outgoing invites unsupported
        // (MatrixClient::inviteUsers returns opId 0), so no pending rows
        // appear. The click must still reduce the chips to a non-empty id list
        // and reach the backend gate, which sets the not-supported error.
        QTRY_VERIFY(!conversations()->errorMessage().isEmpty());
        QVERIFY(conversations()->errorMessage().contains(
            QStringLiteral("not supported")));
        QVERIFY(conversations()->inviteResults().isEmpty());
        // The reduction is also pinned in source: ids only, never chip objects.
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
