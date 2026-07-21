// v0.5.9: deterministic tests for user search (debounce, stale-result
// rejection, deduplication, exact-MXID handling) and the conversation flows
// (existing-DM reuse, DM/room creation, duplicate-submission blocking,
// invite batches with partial success, sign-out invalidation).

#include "app/ConversationController.h"
#include "matrix/MatrixClient.h"
#include "models/UserSearchModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // Scripted state.
    QList<RoomInfo> mirror;
    QVariantList dmRooms;
    quint64 nextOp = 1;
    int searchCalls = 0;
    int createDmCalls = 0;
    int createRoomCalls = 0;
    int inviteCalls = 0;
    int avatarCalls = 0;
    bool avatarSupported = true;
    QString lastSearchQuery;
    QString lastDmUser;
    QVariantMap lastRoomOptions;
    QStringList lastInvitees;
    QString lastAvatarRoom;
    QString lastAvatarPath;
    quint64 lastOpId = 0;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    // v0.5.9 command surface.
    bool supportsRoomManagement() const override { return true; }
    quint64 searchUsers(const QString &query, int) override
    {
        ++searchCalls;
        lastSearchQuery = query;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QVariantList existingDirectRooms(const QString &) const override
    {
        return dmRooms;
    }
    quint64 createDirectChat(const QString &userId) override
    {
        ++createDmCalls;
        lastDmUser = userId;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 createRoom(const QVariantMap &options) override
    {
        ++createRoomCalls;
        lastRoomOptions = options;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 inviteUsers(const QString &, const QStringList &users) override
    {
        ++inviteCalls;
        lastInvitees = users;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomAvatar(const QString &roomId,
                          const QString &localPath) override
    {
        ++avatarCalls;
        lastAvatarRoom = roomId;
        lastAvatarPath = localPath;
        if (!avatarSupported)
            return 0;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QVariantMap searchRow(const QString &userId, const QString &name = {})
{
    QVariantMap row;
    row.insert(QStringLiteral("userId"), userId);
    row.insert(QStringLiteral("displayName"), name);
    row.insert(QStringLiteral("avatarUrl"), QString());
    return row;
}

RoomInfo makeRoom(const QString &id)
{
    RoomInfo room;
    room.id = id;
    room.name = id;
    return room;
}

} // namespace

class ConversationFlowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- user search -----------------------------------------------------

    void searchDebouncesAndDispatchesOnce()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);

        // Rapid keystrokes collapse into one dispatch.
        model.setQuery(QStringLiteral("al"));
        model.setQuery(QStringLiteral("ali"));
        model.setQuery(QStringLiteral("alic"));
        QCOMPARE(client.searchCalls, 0);
        QTRY_COMPARE(client.searchCalls, 1);
        QCOMPARE(client.lastSearchQuery, QStringLiteral("alic"));
        QCOMPARE(model.state(), QStringLiteral("loading"));
    }

    void searchShortQueryStaysIdle()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);
        model.setQuery(QStringLiteral("a"));
        QTest::qWait(30);
        QCOMPARE(client.searchCalls, 0);
        QCOMPARE(model.state(), QStringLiteral("idle"));
    }

    void searchRejectsStaleResults()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);

        model.setQuery(QStringLiteral("alice"));
        QTRY_COMPARE(client.searchCalls, 1);
        const quint64 firstOp = client.lastOpId;

        // A newer query supersedes the first before its result arrives.
        model.setQuery(QStringLiteral("bob"));
        QTRY_COMPARE(client.searchCalls, 2);

        // The stale completion must be dropped entirely.
        Q_EMIT client.userSearchFinished(
            firstOp, true, { searchRow(QStringLiteral("@alice:example.org")) },
            false, QString());
        QCOMPARE(model.rowCount(), 0);

        // The current completion populates the model.
        Q_EMIT client.userSearchFinished(
            client.lastOpId, true,
            { searchRow(QStringLiteral("@bob:example.org")) }, false, QString());
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.userIdAt(0), QStringLiteral("@bob:example.org"));
        QCOMPARE(model.state(), QStringLiteral("results"));
    }

    void searchDeduplicatesAndExcludesSelf()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);
        model.setQuery(QStringLiteral("example"));
        QTRY_COMPARE(client.searchCalls, 1);

        Q_EMIT client.userSearchFinished(
            client.lastOpId, true,
            { searchRow(QStringLiteral("@a:example.org")),
              searchRow(QStringLiteral("@a:example.org")),
              searchRow(QStringLiteral("@me:example.org")),
              searchRow(QStringLiteral("@b:example.org")) },
            false, QString());
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.userIdAt(0), QStringLiteral("@a:example.org"));
        QCOMPARE(model.userIdAt(1), QStringLiteral("@b:example.org"));
    }

    void searchOffersExactMxidFirst()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);
        model.setQuery(QStringLiteral("@carol:example.org"));
        QTRY_COMPARE(client.searchCalls, 1);

        // Directory does not know the user — the typed MXID still appears.
        Q_EMIT client.userSearchFinished(client.lastOpId, true, {}, false,
                                         QString());
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.userIdAt(0), QStringLiteral("@carol:example.org"));
        QCOMPARE(model.state(), QStringLiteral("results"));
    }

    void searchErrorAndNoResultStates()
    {
        FakeClient client;
        UserSearchModel model;
        model.setClient(&client);
        model.setDebounceMs(10);

        model.setQuery(QStringLiteral("zz"));
        QTRY_COMPARE(client.searchCalls, 1);
        Q_EMIT client.userSearchFinished(client.lastOpId, true, {}, false,
                                         QString());
        QCOMPARE(model.state(), QStringLiteral("no_results"));

        model.setQuery(QStringLiteral("zzz"));
        QTRY_COMPARE(client.searchCalls, 2);
        Q_EMIT client.userSearchFinished(client.lastOpId, false, {}, false,
                                         QStringLiteral("network"));
        QCOMPARE(model.state(), QStringLiteral("error"));
    }

    void mxidValidation()
    {
        QVERIFY(UserSearchModel::looksLikeMxid(QStringLiteral("@a:example.org")));
        QVERIFY(UserSearchModel::looksLikeMxid(QStringLiteral("@a:server:8448")));
        QVERIFY(!UserSearchModel::looksLikeMxid(QStringLiteral("alice")));
        QVERIFY(!UserSearchModel::looksLikeMxid(QStringLiteral("@alice")));
        QVERIFY(!UserSearchModel::looksLikeMxid(QStringLiteral("@a b:x.org")));
    }

    // ---- DM creation -------------------------------------------------------

    void existingDmIsDiscovered()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);

        QVariantMap dm;
        dm.insert(QStringLiteral("roomId"), QStringLiteral("!dm:example.org"));
        dm.insert(QStringLiteral("name"), QStringLiteral("Alice"));
        client.dmRooms = { dm };

        controller.checkExistingDm(QStringLiteral("@alice:example.org"));
        QCOMPARE(controller.existingDms().size(), 1);
        QCOMPARE(controller.existingDms().first().toMap()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!dm:example.org"));
    }

    void dmCreateOpensRoomWhenItAppears()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);

        controller.startDirectMessage(QStringLiteral("@alice:example.org"));
        QCOMPARE(client.createDmCalls, 1);
        QVERIFY(controller.busy());

        // Duplicate click while busy is blocked.
        controller.startDirectMessage(QStringLiteral("@alice:example.org"));
        QCOMPARE(client.createDmCalls, 1);

        Q_EMIT client.dmCreateFinished(client.lastOpId, true,
                                       QStringLiteral("!new:example.org"),
                                       QString());
        // Not open yet — the room is not in the authoritative list.
        QCOMPARE(ready.count(), 0);
        QVERIFY(controller.busy());

        client.mirror = { makeRoom(QStringLiteral("!new:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(),
                 QStringLiteral("!new:example.org"));
        QVERIFY(!controller.busy());
    }

    void dmSelfTargetIsRejected()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.startDirectMessage(QStringLiteral("@me:example.org"));
        QCOMPARE(client.createDmCalls, 0);
        QVERIFY(!controller.errorMessage().isEmpty());
    }

    void staleDmCompletionIsIgnored()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);

        controller.startDirectMessage(QStringLiteral("@alice:example.org"));
        const quint64 op = client.lastOpId;

        // Sign-out clears the pending operation…
        Q_EMIT client.loggedOut();
        QVERIFY(!controller.busy());

        // …so the late completion may not open anything.
        Q_EMIT client.dmCreateFinished(op, true,
                                       QStringLiteral("!late:example.org"),
                                       QString());
        client.mirror = { makeRoom(QStringLiteral("!late:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 0);
    }

    void dmFailureSurfacesSafeError()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.startDirectMessage(QStringLiteral("@alice:example.org"));
        Q_EMIT client.dmCreateFinished(client.lastOpId, false, QString(),
                                       QStringLiteral("rate_limited"));
        QVERIFY(!controller.busy());
        QVERIFY(controller.errorMessage().contains(QStringLiteral("rate-limiting")));
    }

    // ---- room creation ----------------------------------------------------

    void roomCreateRequiresName()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.createRoom({ { QStringLiteral("name"), QStringLiteral("  ") } });
        QCOMPARE(client.createRoomCalls, 0);
        QVERIFY(!controller.errorMessage().isEmpty());
    }

    void roomCreateReportsPartialSpaceFailure()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);
        QSignalSpy spaceFail(&controller,
                             &ConversationController::spacePlacementFailed);

        controller.createRoom({ { QStringLiteral("name"), QStringLiteral("Room") } });
        QCOMPARE(client.createRoomCalls, 1);

        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!r:example.org"),
                                         QString(),
                                         QStringLiteral("space_add_failed"));
        QCOMPARE(spaceFail.count(), 1);
        // Space failure is a warning; the room still opens once listed.
        client.mirror = { makeRoom(QStringLiteral("!r:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
    }

    // ---- optional room avatar (applied after create) ----------------------

    void roomCreateWithAvatarAppliesItAfterCreate()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);
        QSignalSpy warn(&controller, &ConversationController::avatarUploadFailed);

        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("Room") },
            { QStringLiteral("avatarPath"),
              QStringLiteral("file:///tmp/picture.png") },
        });
        QCOMPARE(client.createRoomCalls, 1);
        // The avatar never reaches the backend's create call.
        QVERIFY(!client.lastRoomOptions.contains(QStringLiteral("avatarPath")));
        // No avatar call before the room exists.
        QCOMPARE(client.avatarCalls, 0);

        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!r:example.org"),
                                         QString(), QString());
        QCOMPARE(client.avatarCalls, 1);
        QCOMPARE(client.lastAvatarRoom, QStringLiteral("!r:example.org"));
        // The file:// URL was converted to a plain local path.
        QCOMPARE(client.lastAvatarPath, QStringLiteral("/tmp/picture.png"));

        const quint64 avatarOp = client.lastOpId;
        Q_EMIT client.roomEditFinished(avatarOp, QStringLiteral("!r:example.org"),
                                       QStringLiteral("avatar"), true, QString());
        QCOMPARE(warn.count(), 0);

        client.mirror = { makeRoom(QStringLiteral("!r:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
        QVERIFY(!controller.busy());
        QVERIFY(controller.errorMessage().isEmpty());
    }

    void avatarUploadFailureWarnsButNeverBlocksOpening()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);
        QSignalSpy warn(&controller, &ConversationController::avatarUploadFailed);

        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("Room") },
            { QStringLiteral("avatarPath"), QStringLiteral("/tmp/pic.png") },
        });
        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!r:example.org"),
                                         QString(), QString());
        QCOMPARE(client.avatarCalls, 1);
        const quint64 avatarOp = client.lastOpId;

        // The room opens BEFORE the avatar upload completes — the pending
        // avatar op is outside busy() and can never stall the open.
        client.mirror = { makeRoom(QStringLiteral("!r:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
        QVERIFY(!controller.busy());

        // The late failure is a warning, not an error.
        Q_EMIT client.roomEditFinished(avatarOp, QStringLiteral("!r:example.org"),
                                       QStringLiteral("avatar"), false,
                                       QStringLiteral("network"));
        QCOMPARE(warn.count(), 1);
        QCOMPARE(warn.first().first().toString(),
                 QStringLiteral("!r:example.org"));
        QVERIFY(controller.errorMessage().isEmpty());
        QVERIFY(!controller.busy());
    }

    void avatarUnsupportedBackendWarnsImmediately()
    {
        FakeClient client;
        client.avatarSupported = false;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);
        QSignalSpy warn(&controller, &ConversationController::avatarUploadFailed);

        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("Room") },
            { QStringLiteral("avatarPath"), QStringLiteral("/tmp/pic.png") },
        });
        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!r:example.org"),
                                         QString(), QString());
        QCOMPARE(client.avatarCalls, 1);
        QCOMPARE(warn.count(), 1);

        client.mirror = { makeRoom(QStringLiteral("!r:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
        QVERIFY(controller.errorMessage().isEmpty());
    }

    void roomCreateWithoutAvatarNeverCallsSetRoomAvatar()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller, &ConversationController::conversationReady);

        controller.createRoom({ { QStringLiteral("name"), QStringLiteral("Room") } });
        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!r:example.org"),
                                         QString(), QString());
        client.mirror = { makeRoom(QStringLiteral("!r:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(ready.count(), 1);
        QCOMPARE(client.avatarCalls, 0);
    }

    void signOutClearsPendingAvatar()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy warn(&controller, &ConversationController::avatarUploadFailed);

        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("Room") },
            { QStringLiteral("avatarPath"), QStringLiteral("/tmp/pic.png") },
        });
        const quint64 op = client.lastOpId;
        Q_EMIT client.loggedOut();

        // The stale creation completion must not trigger an avatar upload
        // for the next session.
        Q_EMIT client.roomCreateFinished(op, true,
                                         QStringLiteral("!late:example.org"),
                                         QString(), QString());
        QCOMPARE(client.avatarCalls, 0);
        QCOMPARE(warn.count(), 0);
    }

    // ---- invites ------------------------------------------------------------

    void inviteBatchTracksPartialSuccess()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy done(&controller, &ConversationController::inviteBatchCompleted);

        // Duplicates collapse before dispatch.
        controller.inviteUsers(QStringLiteral("!room:example.org"),
                               { QStringLiteral("@a:example.org"),
                                 QStringLiteral("@a:example.org"),
                                 QStringLiteral("@b:example.org") });
        QCOMPARE(client.inviteCalls, 1);
        QCOMPARE(client.lastInvitees.size(), 2);
        QCOMPARE(controller.inviteResults().size(), 2);

        const quint64 op = client.lastOpId;
        Q_EMIT client.inviteUserFinished(op, QStringLiteral("!room:example.org"),
                                         QStringLiteral("@a:example.org"), true,
                                         QString());
        Q_EMIT client.inviteUserFinished(op, QStringLiteral("!room:example.org"),
                                         QStringLiteral("@b:example.org"), false,
                                         QStringLiteral("forbidden"));
        const QVariantList results = controller.inviteResults();
        QCOMPARE(results.at(0).toMap().value(QStringLiteral("state")).toString(),
                 QStringLiteral("ok"));
        QCOMPARE(results.at(1).toMap().value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(results.at(1).toMap().value(QStringLiteral("category")).toString(),
                 QStringLiteral("forbidden"));

        Q_EMIT client.inviteBatchFinished(op, QStringLiteral("!room:example.org"),
                                          1, 1);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().at(0).toInt(), 1);
        QCOMPARE(done.first().at(1).toInt(), 1);
        QVERIFY(!controller.busy());
    }
};

QTEST_GUILESS_MAIN(ConversationFlowTest)
#include "ConversationFlowTest.moc"
