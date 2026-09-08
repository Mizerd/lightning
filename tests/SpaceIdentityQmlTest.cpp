// A Space's IDENTITY — its name, topic and avatar — as the Space settings
// dialog actually renders it, on the real compiled qml/SpaceSettingsDialog.qml
// against a real SpaceManager.
//
// WHY THIS FILE EXISTS. tests/SpaceSettingsContractTest.cpp reads
// SpaceSettingsDialog.qml as TEXT and drives RoomInfoController directly. Both
// halves are worth having and neither can see the defect this suite was
// written for: `root.info` is a binding that CALLS
// `app.spaces.spaceInfo(spaceId)`, and a method call creates no property
// dependency, so the binding's only dependency is `spaceId` itself
// (`app.spaces` is a CONSTANT property). A Space renamed, re-avatared or
// re-topiced under the dialog therefore left every reader of `root.info`
// showing the value it had when the dialog was first pointed at that Space —
// including `nameField.refreshName()` and `topicField.refreshTopic()`, which
// are CONNECTED to app.spaces.spacesChanged and read `root.info`, so the
// refresh the file's own comment promises ("a remote change … lands without
// destroying an edit in progress") was structurally a no-op.
//
// The file already documents the trap for its `rosterTick` counter. This is
// the same trap, in the binding that names the Space.
//
// WHAT IT PROVES AND WHAT IT DOES NOT. Everything here is offscreen QML on a
// local SpaceManager fed by a fake client: a real homeserver, a real
// m.room.name state event and Element interoperability are NOT TESTED. What is
// proved is that the change SIGNAL production emits (SpaceManager::rebuild →
// spacesChanged) reaches the dialog's rendered text, and that a subspace is
// rendered from its own state rather than its parent's.

#include <QtTest/QtTest>

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

// A client that answers with a fixed room list and can be told to change it.
// SpaceManager rebuilds on roomsChanged, which is production's own path for a
// renamed or re-avatared Space.
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

} // namespace

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

    // Popup contents live in the Overlay's visual tree, not the QObject parent
    // chain, so findChild alone cannot see them.
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

    // The canonical fixture: one root Space with one joined child Space, each
    // with its OWN name, topic and avatar.
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

    // A Space renamed under the open dialog is the Space the dialog names.
    //
    // Before the fix `root.info` was a spaceInfo() call with no change
    // dependency, so the header, the General card's avatar and the name/topic
    // fields all kept the value read when the dialog was first pointed here —
    // and refreshName()/refreshTopic(), which app.spaces.spacesChanged calls
    // for exactly this case, read the same stale map.
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

        // Somebody else renames it and re-topics it; sync rebuilds the model.
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

    // The same fact for the picture: a Space's avatar is read from the model
    // every time the model says it changed, not once when the dialog opened.
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

    // A half-typed name belongs to the person typing it. A remote change must
    // land in the fields the user has NOT touched and leave the one they have
    // alone — the rule the file already states for refreshName().
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

        // The untouched field follows the Space...
        QTRY_COMPARE(topicField->property("text").toString(),
                     QStringLiteral("A newer topic"));
        // ...and the edit in progress is not destroyed.
        QCOMPARE(nameField->property("text").toString(),
                 QStringLiteral("Half typed"));
    }

    // A SUBSPACE IS NOT ITS PARENT. Opening the dialog on a child Space right
    // after the parent must render the child's own name, topic and avatar —
    // never the values still cached from the Space above it.
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

    // ...and a rename of the PARENT while the CHILD's settings are open
    // changes nothing on screen. The dialog reads one Space; a shared change
    // signal must not smear one Space's state onto another's card.
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

    // Reopening the dialog on the SAME Space re-snaps its fields.
    //
    // `onSpaceIdChanged` does not fire when `spaceId` is assigned the value it
    // already had, so an abandoned edit — and the name the Space had when that
    // edit began — used to come back on the next open as though it were the
    // Space's own.
    void reopeningOnTheSameSpaceReSnapsTheFields()
    {
        openFor(QString::fromLatin1(kParentId));
        auto *nameField = item("spaceSettingsNameField");
        QVERIFY(nameField);
        nameField->setProperty("text", QStringLiteral("Abandoned edit"));
        QCoreApplication::processEvents();
        closeDialog();

        // ...and the Space is renamed by somebody else while it is shut.
        m_fake->roomList[0].name = QStringLiteral("Renamed Space");
        m_fake->announce();
        QCoreApplication::processEvents();

        openFor(QString::fromLatin1(kParentId));
        QCOMPARE(item("spaceSettingsNameField")->property("text").toString(),
                 QStringLiteral("Renamed Space"));
    }

    // A Space whose avatar is cleared renders the initials fallback rather
    // than the picture it used to have: an mxc that goes away has to reach the
    // Avatar, or a removed picture stays on screen until a restart.
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

    // ── The model under all of the above ────────────────────────────────
    //
    // SpaceManager::spaceInfo / childRoomsDetailed / childSpaceIds had no
    // behavioural coverage anywhere before 2026-09-08 — only two source scans
    // naming them. They are what decides whether a subspace shows its own
    // state or the state of the Space it hangs under, so they are pinned here
    // beside the dialog that renders them.

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
        // A Space nobody joined is NOTHING, never the nearest one that exists.
        QVERIFY(spaces->spaceInfo(QStringLiteral("!absent:example.org")).isEmpty());
    }

    // The hierarchy runs DOWNWARD only. A subspace lists the rooms under
    // itself; the ancestor's own rooms are not its.
    //
    // The other direction is deliberate and is pinned here so a future round
    // cannot invert it by accident: childRoomsDetailed on an ancestor IS
    // transitive ("show me everything in this Space"), which is why the
    // Channels layout uses directChildRoomsDetailed instead.
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

        // Only the DIRECT children are the subspace tree, and only downward.
        QCOMPARE(spaces->childSpaceIds(QString::fromLatin1(kParentId)),
                 QStringList{ QString::fromLatin1(kChildId) });
        QVERIFY(spaces->childSpaceIds(QString::fromLatin1(kChildId)).isEmpty());
    }
};

QTEST_MAIN(SpaceIdentityQmlTest)
#include "SpaceIdentityQmlTest.moc"
