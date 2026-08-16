// v0.7.x read markers — "Mark as read" for a room that is NOT open.
//
// The room list has offered "Mark as read" for a long time, and on the Rust
// backend it did nothing at all for any room except the open one.
// RoomListModel::markRoomRead resolved its target event by walking
// `MatrixClient::timeline(roomId)`, and that only ever holds the ACTIVE
// room's loaded timeline — for every other room it is empty, the loop found
// no event, and the function returned having sent nothing. The menu item
// reported success by saying nothing, the room stayed unread, and the user's
// read position never moved on their other devices.
//
// The fix routes through a backend that can resolve the latest event WITHOUT
// a loaded timeline (the SDK's own `Room::latest_event()`), and keeps the
// old timeline-walking path for backends that cannot.
//
// What the Rust side then sends is deliberately BOTH markers: the public
// read receipt is what other people see, and `m.fully_read` is the user's
// own read position — the half that actually syncs their place across their
// own devices, which is the point of the feature.
//
// HONEST SCOPE: routing and fallback selection only. That a homeserver
// accepts the receipts, that `m.fully_read` really lands in account data,
// and that another client observes the moved marker are NOT exercised here
// and are NOT TESTED.

#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

#include <QtTest/QtTest>

namespace {

RoomInfo makeRoom(const QString &id)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.membership = RoomInfo::Joined;
    return info;
}

TimelineEvent makeEvent(const QString &eventId)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.type = TimelineEvent::TextMessage;
    return e;
}

// A backend that can resolve a closed room's latest event — the Rust shape.
class CapableClient : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomSet;
    QStringList markReadCalls;
    QStringList receiptCalls;

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
    QList<RoomInfo> rooms() const override { return roomSet; }
    // Deliberately empty for EVERY room, exactly like the Rust backend for
    // any room that is not open. This is the condition the old code could
    // not survive.
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
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &roomId, const QString &eventId) override
    { receiptCalls.append(roomId + QLatin1Char('|') + eventId); }
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsMarkRoomRead() const override { return true; }
    void markRoomRead(const QString &roomId) override
    { markReadCalls.append(roomId); }
};

// A backend WITHOUT that capability — Mock/HTTP, which do keep timelines.
class LegacyClient final : public CapableClient
{
    Q_OBJECT
public:
    using CapableClient::CapableClient;

    QHash<QString, QList<TimelineEvent>> timelines;

    QList<TimelineEvent> timeline(const QString &roomId) const override
    { return timelines.value(roomId); }

    bool supportsMarkRoomRead() const override { return false; }
};

const QString kRoom = QStringLiteral("!room:example.org");

} // namespace

class MarkRoomReadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The regression. With an empty loaded timeline — the normal state of
    // every room that is not open — the old code sent nothing at all. It
    // must now reach the backend that can resolve the event itself.
    void closedRoomIsMarkedReadThroughTheCapableBackend()
    {
        CapableClient client;
        client.roomSet = { makeRoom(kRoom) };
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);

        QCOMPARE(client.markReadCalls, QStringList{ kRoom });
        // And it must NOT fall through to the timeline-walking path, which
        // would send a receipt for whatever happened to be loaded.
        QVERIFY(client.receiptCalls.isEmpty());
    }

    // A backend that cannot resolve a closed room's latest event keeps the
    // old behaviour rather than losing the feature.
    void legacyBackendStillUsesTheLoadedTimeline()
    {
        LegacyClient client;
        client.roomSet = { makeRoom(kRoom) };
        client.timelines.insert(kRoom, { makeEvent(QStringLiteral("$a")),
                                         makeEvent(QStringLiteral("$b")) });
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);

        QVERIFY(client.markReadCalls.isEmpty());
        // Newest event wins — the walk is from the back.
        QCOMPARE(client.receiptCalls,
                 QStringList{ kRoom + QStringLiteral("|$b") });
    }

    // A local echo is not a real event and cannot carry a receipt; the walk
    // must skip it rather than send an id the server has never seen.
    void legacyBackendSkipsLocalEchoes()
    {
        LegacyClient client;
        client.roomSet = { makeRoom(kRoom) };
        client.timelines.insert(kRoom, { makeEvent(QStringLiteral("$real")),
                                         makeEvent(QStringLiteral("local:pending")) });
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);
        QCOMPARE(client.receiptCalls,
                 QStringList{ kRoom + QStringLiteral("|$real") });
    }

    // An unknown room, an empty id, and no client at all are all no-ops
    // rather than crashes or receipts aimed at nothing.
    void unknownTargetsAreInertNotFatal()
    {
        CapableClient client;
        client.roomSet = { makeRoom(kRoom) };
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(QString());
        QVERIFY(client.markReadCalls.isEmpty());
        QVERIFY(client.receiptCalls.isEmpty());

        // A room the model does not hold still reaches the capable backend:
        // it is the SDK, not this model, that knows which rooms exist, and
        // refusing here would reintroduce a silent no-op for a room the
        // list is merely filtering out.
        model.markRoomRead(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(client.markReadCalls,
                 QStringList{ QStringLiteral("!elsewhere:example.org") });

        RoomListModel detached;
        detached.markRoomRead(kRoom); // no client — must not crash
    }
};

QTEST_MAIN(MarkRoomReadTest)
#include "MarkRoomReadTest.moc"
