// v0.7.x Space settings — source contract for the Space Home management
// surface in TimelinePane.qml.
//
// A Space IS a Matrix room, so editing its name, topic and avatar is the
// SAME permission-gated room-edit backend (`RoomInfoController::setRoomName`
// / `setRoomTopic` / `setRoomAvatar` / `removeRoomAvatar`) that Room
// Information uses. Lightning invents no Space-specific storage and no
// Space-specific permission model.
//
// Name and topic already existed; the avatar controls did not, which meant a
// Space could be renamed but never given a picture from inside Lightning.
// This pins all three, and pins that each is gated on the room's REAL
// required power level for that state event rather than on a role label or
// an optimistic default.
//
// HONEST SCOPE: a source contract, exactly like ContextMenuContractTest. It
// proves the controls exist, are wired to the shared backend, and carry the
// right permission gate and accessibility names. It does NOT instantiate the
// pane, does not upload an avatar, and does not talk to a homeserver — a real
// m.room.avatar round trip for a Space is NOT TESTED.

#include <QtTest/QtTest>

#include <QFile>

class SpaceSettingsContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                              : QString{};
    }

    // The Space settings card, bounded so an assertion meant for it cannot
    // match some other part of a 4000-line file.
    static QString spaceSettingsBlock(const QString &pane)
    {
        const int start = pane.indexOf(QStringLiteral("id: settingsCol"));
        if (start < 0)
            return {};
        const int end = pane.indexOf(QStringLiteral("ROOMS AND SPACES"), start);
        if (end < 0)
            return {};
        return pane.mid(start, end - start);
    }

private Q_SLOTS:
    void spaceHomeExposesAllThreeEdits()
    {
        const QString block = spaceSettingsBlock(read(QStringLiteral("TimelinePane.qml")));
        QVERIFY2(!block.isEmpty(), "the Space settings card is missing");
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceNameEditField\"")));
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceTopicEditField\"")));
        QVERIFY2(block.contains(QStringLiteral("objectName: \"spaceChangeAvatarButton\"")),
                 "a Space could be renamed but never given an avatar");
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceRemoveAvatarButton\"")));
    }

    // Every edit routes to the shared room-edit backend. A Space-specific
    // path would be a second implementation of the same Matrix state events,
    // free to disagree with the room one about permissions.
    void everyEditUsesTheSharedRoomBackend()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const QString block = spaceSettingsBlock(pane);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.setRoomName(")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.setRoomTopic(")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.removeRoomAvatar()")));
        // The file dialog hands the chosen file to the same setter Room
        // Information uses.
        QVERIFY(pane.contains(QStringLiteral("id: spaceAvatarDialog")));
        QVERIFY(pane.contains(QStringLiteral("onAccepted: app.roomInfo.setRoomAvatar(selectedFile)")));
    }

    // Each control is gated on the room's own permission flag for THAT state
    // event, and on no write already being in flight. canEditAvatar is the
    // SDK's real can_send_state answer, so a member who may rename a Space
    // but not change its picture sees exactly that.
    void editsAreGatedOnTheRealPerEventPermission()
    {
        const QString block = spaceSettingsBlock(read(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.canEditName")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.canEditTopic")));
        QVERIFY2(block.contains(QStringLiteral("app.roomInfo.canEditAvatar")),
                 "the avatar controls must be gated on the avatar permission, "
                 "not on the name or topic one");
        // Nothing is offered while a previous edit is still settling.
        QVERIFY(block.contains(QStringLiteral("!app.roomInfo.editPending")));
    }

    // A refused edit has to say so. The card renders the controller's own
    // sanitized error text rather than failing silently.
    void editFailuresAreDisclosed()
    {
        const QString block = spaceSettingsBlock(read(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.editError")));
    }

    void avatarControlsCarryAccessibleNames()
    {
        const QString block = spaceSettingsBlock(read(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("Change the Space avatar")));
        QVERIFY(block.contains(QStringLiteral("Remove the Space avatar")));
    }
};

QTEST_MAIN(SpaceSettingsContractTest)
#include "SpaceSettingsContractTest.moc"
