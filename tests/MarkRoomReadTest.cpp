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

#include <QTimeZone>
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
    // 2026-09-05, "no favorite tab exists in classic mode, it just says added
    // to favorite": a favourite ranks above the feed under a header of its
    // own — Element's shape — with invites still first and recency ordering
    // each rank. An invite cannot carry a tag, so a favourite flag on one
    // changes nothing.
    void aFavouriteRanksAboveTheFeedUnderItsOwnHeader()
    {
        CapableClient client;
        RoomInfo busy = makeRoom(QStringLiteral("!busy:x"));
        busy.lastActivity =
            QDateTime(QDate(2026, 9, 5), QTime(12, 0), QTimeZone::UTC);
        RoomInfo quiet = makeRoom(QStringLiteral("!quiet:x"));
        quiet.lastActivity = busy.lastActivity.addSecs(-3600);
        quiet.isFavourite = true;
        RoomInfo pending = makeRoom(QStringLiteral("!invite:x"));
        pending.membership = RoomInfo::Invited;
        pending.isFavourite = true;
        client.roomSet = { busy, quiet, pending };
        RoomListModel model;
        model.setClient(&client);
        Q_EMIT client.roomsChanged();

        QCOMPARE(model.rowCount(), 3);
        auto idAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::RoomIdRole).toString();
        };
        auto categoryAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::CategoryRole).toString();
        };
        QCOMPARE(idAt(0), QStringLiteral("!invite:x"));
        QCOMPARE(categoryAt(0), QStringLiteral("invite"));
        QCOMPARE(idAt(1), QStringLiteral("!quiet:x"));
        QCOMPARE(categoryAt(1), QStringLiteral("favourite"));
        QCOMPARE(idAt(2), QStringLiteral("!busy:x"));
        QCOMPARE(categoryAt(2), QStringLiteral("conversation"));
        QCOMPARE(RoomListModel::orderRankOf(quiet), 1);
        QCOMPARE(RoomListModel::orderRankOf(busy), 2);
    }

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

    // MARK ALL ROOMS READ.
    //
    // Per-room has existed for a long time and per-Space since the Element
    // parity round; the account-wide sweep did not, so an account that had
    // drifted could only be caught up one room at a time.
    //
    // What it must NOT do is as much of the contract as what it must: a
    // receipt for every joined room would be one request per room, most of
    // them telling the server what it already knows, and two of the skips
    // protect a deliberate user choice rather than just saving traffic.
    void markAllRoomsReadTouchesOnlyTheRoomsThatAreUnread()
    {
        CapableClient client;
        RoomInfo unreadCount = makeRoom(QStringLiteral("!a:example.org"));
        unreadCount.unreadCount = 3;
        RoomInfo highlighted = makeRoom(QStringLiteral("!b:example.org"));
        highlighted.highlightCount = 1;
        // No counts, but the SDK says there is unread MESSAGE content — the
        // state a muted room sits in, and still unread.
        RoomInfo quietUnread = makeRoom(QStringLiteral("!c:example.org"));
        quietUnread.hasUnreadMessages = true;
        RoomInfo caughtUp = makeRoom(QStringLiteral("!d:example.org"));
        // The user's own "leave this one for later". A sweep aimed at the
        // rooms they had not got to must not overrule the one they kept.
        RoomInfo keptUnread = makeRoom(QStringLiteral("!e:example.org"));
        keptUnread.unreadCount = 2;
        keptUnread.markedUnread = true;
        // An invite is a decision, not unread mail.
        RoomInfo invite = makeRoom(QStringLiteral("!f:example.org"));
        invite.membership = RoomInfo::Invited;
        invite.unreadCount = 1;
        // A Space is a room with no timeline; a receipt on it clears nothing.
        RoomInfo space = makeRoom(QStringLiteral("!g:example.org"));
        space.isSpace = true;
        space.unreadCount = 5;

        client.roomSet = { unreadCount, highlighted, quietUnread, caughtUp,
                           keptUnread, invite, space };
        RoomListModel model;
        model.setClient(&client);

        QCOMPARE(model.markAllRoomsRead(), 3);
        QCOMPARE(client.markReadCalls,
                 (QStringList{ QStringLiteral("!a:example.org"),
                               QStringLiteral("!b:example.org"),
                               QStringLiteral("!c:example.org") }));

        // Idempotent from the model's side: the counts have not moved (the
        // server has not answered yet), so a second press marks them again
        // rather than inventing a local "already done" state the server
        // never confirmed. What it must not do is grow the set.
        client.markReadCalls.clear();
        QCOMPARE(model.markAllRoomsRead(), 3);
        QCOMPARE(client.markReadCalls.size(), 3);

        // Nothing unread is nothing done, and it says so.
        CapableClient quiet;
        quiet.roomSet = { caughtUp };
        RoomListModel quietModel;
        quietModel.setClient(&quiet);
        QCOMPARE(quietModel.markAllRoomsRead(), 0);
        QVERIFY(quiet.markReadCalls.isEmpty());

        RoomListModel detached;
        QCOMPARE(detached.markAllRoomsRead(), 0); // no client — must not crash
    }
};

QTEST_MAIN(MarkRoomReadTest)
#include "MarkRoomReadTest.moc"
