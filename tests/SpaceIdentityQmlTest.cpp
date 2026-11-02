
#include <QtTest/QtTest>

#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/RoomInfoController.h"
#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

namespace {

class SpaceFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomList;

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

RoomInfo space(const QString &id, const QString &name, const QString &topic,
               const QString &avatar)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.topic = topic;
    info.avatarUrl = avatar;
    info.isSpace = true;
    info.membership = RoomInfo::Joined;
    return info;
}

RoomInfo room(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
    return info;
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 760
    visible: true
    color: AppTheme.background

    SpaceSettingsDialog {
        objectName: "spaceSettings"
        parent: Overlay.overlay
    }
}
)QML";

constexpr const char *kParentId = "!space-parent:example.org";
constexpr const char *kChildId = "!space-child:example.org";
constexpr const char *kParentRoomId = "!room-parent:example.org";
constexpr const char *kChildRoomId = "!room-child:example.org";

}

class SpaceIdentityQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    SpaceFakeClient *m_fake = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;

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

    void openFor(const QString &spaceId)
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor",
            Q_ARG(QVariant, QVariant(spaceId))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void closeDialog()
    {
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void resetRooms()
    {
        RoomInfo parent = space(QString::fromLatin1(kParentId),
                                QStringLiteral("Parent Space"),
                                QStringLiteral("The parent topic"),
                                QStringLiteral("mxc://example.org/parent"));
        parent.childRoomIds = { QString::fromLatin1(kParentRoomId),
                                QString::fromLatin1(kChildId) };
        RoomInfo child = space(QString::fromLatin1(kChildId),
                               QStringLiteral("Child Space"),
                               QStringLiteral("The child topic"),
                               QStringLiteral("mxc://example.org/child"));
        child.childRoomIds = { QString::fromLatin1(kChildRoomId) };
        m_fake->roomList = {
            parent,
            child,
            room(QString::fromLatin1(kParentRoomId), QStringLiteral("Parent room")),
            room(QString::fromLatin1(kChildRoomId), QStringLiteral("Child room")),
        };
        m_fake->announce();
        QCoreApplication::processEvents();
    }

    static QStringList idsOf(const QVariantList &rows)
    {
        QStringList out;
        for (const QVariant &value : rows)
            out.append(value.toMap().value(QStringLiteral("roomId")).toString());
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("space-identity-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_fake = new SpaceFakeClient(this);
        // The dialog's `info` comes from app.spaces. Point that manager at a
        // hierarchy this suite can change; everything else in the controller
        // keeps the mock backend it was built with.
        m_controller->spaces()->setClient(m_fake);
        resetRooms();
        QCOMPARE(m_controller->spaces()->spaceCount(), 2);

        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("spaceidentityscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();

        m_dialog = m_root->findChild<QObject *>(QStringLiteral("spaceSettings"));
        QVERIFY(m_dialog);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void init()
    {
        resetRooms();
    }

    void cleanup()
    {
        if (m_dialog && m_dialog->property("visible").toBool())
            closeDialog();
    }

    void aRemoteRenameReachesTheOpenDialog()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *header = item("spaceSettingsHeaderName");
        auto *nameField = item("spaceSettingsNameField");
        auto *topicField = item("spaceSettingsTopicField");
        QVERIFY(header && nameField && topicField);
        QCOMPARE(header->property("text").toString(),
                 QStringLiteral("Parent Space"));
        QCOMPARE(nameField->property("text").toString(),
                 QStringLiteral("Parent Space"));
        QCOMPARE(topicField->property("text").toString(),
                 QStringLiteral("The parent topic"));

        m_fake->roomList[0].name = QStringLiteral("Renamed Space");
        m_fake->roomList[0].topic = QStringLiteral("A newer topic");
        m_fake->announce();
        QCoreApplication::processEvents();

        QTRY_COMPARE(header->property("text").toString(),
                     QStringLiteral("Renamed Space"));
        QCOMPARE(nameField->property("text").toString(),
                 QStringLiteral("Renamed Space"));
        QCOMPARE(topicField->property("text").toString(),
                 QStringLiteral("A newer topic"));
    }

    void defaultRoomOrderEditorFitsAndStagesMoves()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *editor = item("spaceDefaultOrderEditor");
        auto *body = item("spaceSettingsBodyScroll");
        QVERIFY(editor && body);
        QTRY_VERIFY(editor->width() > 100);
        QVERIFY(editor->width() <= body->width());
        QVERIFY(editor->height() > 0);
        const auto draft = [this] {
            const QVariant value = m_dialog->property("orderDraft");
            return value.metaType() == QMetaType::fromType<QJSValue>()
                ? value.value<QJSValue>().toVariant().toList() : value.toList();
        };
        const auto before = draft();
        QVERIFY(before.size() > 1);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "moveDefaultChild",
            Q_ARG(QVariant, 0), Q_ARG(QVariant, 1)));
        const auto after = draft();
        QCOMPARE(after.at(1), before.at(0));
        QVERIFY(m_dialog->property("orderDirty").toBool());
        closeDialog();
    }

    void aFailedDefaultOrderSaveLeavesARetryPath()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *save = item("saveDefaultRoomOrder");
        QVERIFY(save);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "moveDefaultChild",
            Q_ARG(QVariant, 0), Q_ARG(QVariant, 1)));
        QVERIFY(m_dialog->property("orderDirty").toBool());
        QVERIFY(!m_controller->roomInfo()->canManageSpaceChildren());
        QVERIFY(!save->property("enabled").toBool());

        m_dialog->setProperty("orderPending", true);
        Q_EMIT m_controller->spaces()->childOrderFinished(
            QString::fromLatin1(kParentId), false);
        QVERIFY(!m_dialog->property("orderPending").toBool());
        QVERIFY(m_dialog->property("orderDirty").toBool());
        QVERIFY(!m_dialog->property("orderStatus").toString().isEmpty());

        m_dialog->setProperty("orderPending", true);
        Q_EMIT m_controller->spaces()->childOrderFinished(
            QString::fromLatin1(kParentId), true);
        QVERIFY(!m_dialog->property("orderDirty").toBool());
        closeDialog();
    }

    void theSectionBodyGetsTheWidthBesideTheNav()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *nav = item("spaceSettingsNav");
        auto *body = item("spaceSettingsBodyScroll");
        QVERIFY(nav && body);
        QTRY_VERIFY(body->width() > 0);
        QVERIFY2(nav->width() <= 201.0,
                 qPrintable(QStringLiteral("nav is %1 px wide").arg(nav->width())));
        QVERIFY2(body->width() >= 300.0,
                 qPrintable(QStringLiteral("body is %1 px wide").arg(body->width())));
        closeDialog();
    }

    void aRemoteAvatarChangeReachesTheOpenDialog()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *avatar = item("spaceSettingsHeaderAvatar");
        QVERIFY(avatar);
        QCOMPARE(avatar->property("mxc").toString(),
                 QStringLiteral("mxc://example.org/parent"));

        m_fake->roomList[0].avatarUrl =
            QStringLiteral("mxc://example.org/parent-v2");
        m_fake->announce();
        QCoreApplication::processEvents();

        QTRY_COMPARE(avatar->property("mxc").toString(),
                     QStringLiteral("mxc://example.org/parent-v2"));
    }

    void anEditInProgressSurvivesARemoteChange()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *nameField = item("spaceSettingsNameField");
        auto *topicField = item("spaceSettingsTopicField");
        QVERIFY(nameField && topicField);
        nameField->setProperty("text", QStringLiteral("Half typed"));
        QCoreApplication::processEvents();

        m_fake->roomList[0].name = QStringLiteral("Renamed Space");
        m_fake->roomList[0].topic = QStringLiteral("A newer topic");
        m_fake->announce();
        QCoreApplication::processEvents();

        QTRY_COMPARE(topicField->property("text").toString(),
                     QStringLiteral("A newer topic"));
        QCOMPARE(nameField->property("text").toString(),
                 QStringLiteral("Half typed"));
    }

    void aSubspaceRendersItsOwnIdentityNotItsParents()
    {
        openFor(QString::fromLatin1(kParentId));
        QCOMPARE(item("spaceSettingsHeaderName")->property("text").toString(),
                 QStringLiteral("Parent Space"));
        closeDialog();

        openFor(QString::fromLatin1(kChildId));
        QCOMPARE(item("spaceSettingsHeaderName")->property("text").toString(),
                 QStringLiteral("Child Space"));
        QCOMPARE(item("spaceSettingsHeaderAvatar")->property("mxc").toString(),
                 QStringLiteral("mxc://example.org/child"));
        QCOMPARE(item("spaceSettingsNameField")->property("text").toString(),
                 QStringLiteral("Child Space"));
        QCOMPARE(item("spaceSettingsTopicField")->property("text").toString(),
                 QStringLiteral("The child topic"));
    }

    void renamingTheParentDoesNotTouchTheOpenSubspace()
    {
        openFor(QString::fromLatin1(kChildId));
        m_fake->roomList[0].name = QStringLiteral("Renamed Parent");
        m_fake->roomList[0].avatarUrl =
            QStringLiteral("mxc://example.org/parent-v2");
        m_fake->announce();
        QCoreApplication::processEvents();

        QCOMPARE(item("spaceSettingsHeaderName")->property("text").toString(),
                 QStringLiteral("Child Space"));
        QCOMPARE(item("spaceSettingsHeaderAvatar")->property("mxc").toString(),
                 QStringLiteral("mxc://example.org/child"));
        QCOMPARE(item("spaceSettingsNameField")->property("text").toString(),
                 QStringLiteral("Child Space"));
    }

    void reopeningOnTheSameSpaceReSnapsTheFields()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *nameField = item("spaceSettingsNameField");
        QVERIFY(nameField);
        nameField->setProperty("text", QStringLiteral("Abandoned edit"));
        QCoreApplication::processEvents();
        closeDialog();

        m_fake->roomList[0].name = QStringLiteral("Renamed Space");
        m_fake->announce();
        QCoreApplication::processEvents();

        openFor(QString::fromLatin1(kParentId));
        QCOMPARE(item("spaceSettingsNameField")->property("text").toString(),
                 QStringLiteral("Renamed Space"));
    }

    void aClearedAvatarClearsTheDialogsPicture()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *avatar = item("spaceSettingsHeaderAvatar");
        QVERIFY(avatar);
        QCOMPARE(avatar->property("mxc").toString(),
                 QStringLiteral("mxc://example.org/parent"));

        m_fake->roomList[0].avatarUrl.clear();
        m_fake->announce();
        QCoreApplication::processEvents();

        QTRY_COMPARE(avatar->property("mxc").toString(), QString());
    }

    void spaceInfoAnswersForTheSpaceItWasAskedAbout()
    {
        SpaceManager *spaces = m_controller->spaces();
        const QVariantMap parent = spaces->spaceInfo(QString::fromLatin1(kParentId));
        const QVariantMap child = spaces->spaceInfo(QString::fromLatin1(kChildId));

        QCOMPARE(parent.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Parent Space"));
        QCOMPARE(child.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Child Space"));
        QCOMPARE(child.value(QStringLiteral("topic")).toString(),
                 QStringLiteral("The child topic"));
        QCOMPARE(child.value(QStringLiteral("avatarUrl")).toString(),
                 QStringLiteral("mxc://example.org/child"));
        QCOMPARE(child.value(QStringLiteral("roomId")).toString(),
                 QString::fromLatin1(kChildId));
        QVERIFY(spaces->spaceInfo(QStringLiteral("!absent:example.org")).isEmpty());
    }

    void aSubspaceDoesNotInheritItsParentsRoomList()
    {
        SpaceManager *spaces = m_controller->spaces();

        const QStringList childRooms =
            idsOf(spaces->childRoomsDetailed(QString::fromLatin1(kChildId)));
        QCOMPARE(childRooms, QStringList{ QString::fromLatin1(kChildRoomId) });
        QVERIFY(!childRooms.contains(QString::fromLatin1(kParentRoomId)));
        QVERIFY(!spaces->includesRoom(QString::fromLatin1(kChildId),
                                      QString::fromLatin1(kParentRoomId)));

        const QStringList parentRooms =
            idsOf(spaces->childRoomsDetailed(QString::fromLatin1(kParentId)));
        QVERIFY(parentRooms.contains(QString::fromLatin1(kParentRoomId)));
        QVERIFY2(parentRooms.contains(QString::fromLatin1(kChildRoomId)),
                 "an ancestor's membership is transitive by design");

        QCOMPARE(spaces->childSpaceIds(QString::fromLatin1(kParentId)),
                 QStringList{ QString::fromLatin1(kChildId) });
        QVERIFY(spaces->childSpaceIds(QString::fromLatin1(kChildId)).isEmpty());
    }
};

QTEST_MAIN(SpaceIdentityQmlTest)
#include "SpaceIdentityQmlTest.moc"
