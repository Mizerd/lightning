// User search (debounce, stale-result rejection, dedup, exact-MXID handling)
// and conversation flows (existing-DM reuse, DM/room creation, duplicate
// submission blocking, invite batches with partial success, sign-out
// invalidation).

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
    // Off by default: the base returns 0 (no lookup), which the controller
    // treats as "nothing to ask" and creates at once.
    bool profileLookupSupported = false;
    int profileCalls = 0;
    QString lastProfileUser;
    quint64 lastProfileOp = 0;
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

    // Command surface.
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
    quint64 fetchUserProfile(const QString &userId) override
    {
        if (!profileLookupSupported)
            return 0;
        ++profileCalls;
        lastProfileUser = userId;
        lastProfileOp = nextOp++;
        return lastProfileOp;
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

        // The directory does not know the user; the typed MXID still appears.
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

    // Break-it s04: the duplicate-DM card read "Existing conversation" with an
    // "EC" avatar. m.direct rooms rarely carry an explicit name, and the
    // backend reports only that; the entry takes the room list's computed name
    // and the room avatar instead, and says whether the room is 1:1.
    void existingDmIsPresentedTheWayTheRoomListShowsIt()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);

        const auto dmEntry = [](const QString &roomId) {
            QVariantMap dm;
            dm.insert(QStringLiteral("roomId"), roomId);
            dm.insert(QStringLiteral("name"), QString());
            return dm;
        };
        RoomInfo oneToOne = makeRoom(QStringLiteral("!dm:example.org"));
        oneToOne.name = QStringLiteral("Alice");
        oneToOne.avatarUrl = QStringLiteral("mxc://example.org/room");
        oneToOne.isDirect = true;
        oneToOne.directUserIds = { QStringLiteral("@alice:example.org") };
        RoomInfo group = makeRoom(QStringLiteral("!group:example.org"));
        group.name = QStringLiteral("Alice and Bob");
        group.isDirect = true;
        group.directUserIds = { QStringLiteral("@alice:example.org"),
                                QStringLiteral("@bob:example.org") };
        // A nameless room: RustRoomRegistry stores its id as the name.
        RoomInfo nameless = makeRoom(QStringLiteral("!bare:example.org"));
        client.mirror = { oneToOne, group, nameless };
        client.dmRooms = { dmEntry(oneToOne.id), dmEntry(group.id),
                           dmEntry(nameless.id) };

        controller.checkExistingDm(QStringLiteral("@alice:example.org"));
        const QVariantList found = controller.existingDms();
        QCOMPARE(found.size(), 3);
        const QVariantMap first = found.at(0).toMap();
        QCOMPARE(first.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alice"));
        QCOMPARE(first.value(QStringLiteral("avatarUrl")).toString(),
                 QStringLiteral("mxc://example.org/room"));
        QCOMPARE(first.value(QStringLiteral("oneToOne")).toBool(), true);
        const QVariantMap second = found.at(1).toMap();
        QCOMPARE(second.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alice and Bob"));
        QCOMPARE(second.value(QStringLiteral("oneToOne")).toBool(), false);
        // An id is not a name: left empty for the dialog's person fallback.
        QVERIFY(found.at(2).toMap().value(QStringLiteral("name")).toString().isEmpty());
    }

    // Break-it s04: a DM to @nobodyhere999 was created, encrypted, with no
    // warning, and a retry made a second dead room. As Element does, the
    // target's profile is asked for first and a 404 stops the create.
    void aDmToAUserTheServerDoesNotKnowAsksFirst()
    {
        FakeClient client;
        client.profileLookupSupported = true;
        ConversationController controller;
        controller.setClient(&client);
        const QString nobody = QStringLiteral("@nobodyhere999:example.org");

        controller.startDirectMessage(nobody);
        QCOMPARE(client.profileCalls, 1);
        QCOMPARE(client.lastProfileUser, nobody);
        QCOMPARE(client.createDmCalls, 0);
        QVERIFY(controller.busy());

        QSignalSpy asked(&controller, &ConversationController::unknownTargetChanged);
        Q_EMIT client.userProfileFinished(client.lastProfileOp, false, nobody,
                                          QString(), QString(),
                                          QStringLiteral("not_found"));
        QCOMPARE(client.createDmCalls, 0);
        QCOMPARE(controller.unknownTarget(), nobody);
        QCOMPARE(asked.count(), 1);
        QVERIFY(!controller.busy());

        // Cancel: nothing is created.
        controller.dismissUnknownTarget();
        QVERIFY(controller.unknownTarget().isEmpty());
        QCOMPARE(client.createDmCalls, 0);

        // Asked again, and this time the person goes ahead.
        controller.startDirectMessage(nobody);
        Q_EMIT client.userProfileFinished(client.lastProfileOp, false, nobody,
                                          QString(), QString(),
                                          QStringLiteral("not_found"));
        QCOMPARE(controller.unknownTarget(), nobody);
        controller.startDirectMessageAnyway(nobody);
        QCOMPARE(client.createDmCalls, 1);
        QCOMPARE(client.lastDmUser, nobody);
        QVERIFY(controller.unknownTarget().isEmpty());
        QVERIFY(controller.busy());
    }

    // Only "not found" and "their server did not answer" stop the create. A
    // server that refuses profile lookups (403), a rate limit or an endpoint
    // it lacks must not block, and a user who exists is created without
    // asking.
    void aProfileLookupThatIsNotANotFoundNeverBlocks()
    {
        const QStringList outcomes = { QStringLiteral("ok"),
                                       QStringLiteral("forbidden"),
                                       QStringLiteral("rate_limited"),
                                       QStringLiteral("unrecognized") };
        for (const QString &outcome : outcomes) {
            FakeClient client;
            client.profileLookupSupported = true;
            ConversationController controller;
            controller.setClient(&client);
            const QString user = QStringLiteral("@alice:example.org");
            controller.startDirectMessage(user);
            const bool ok = outcome == QLatin1String("ok");
            Q_EMIT client.userProfileFinished(client.lastProfileOp, ok, user,
                                              ok ? QStringLiteral("Alice") : QString(),
                                              QString(),
                                              ok ? QString() : outcome);
            QVERIFY2(client.createDmCalls == 1, qPrintable(outcome));
            QVERIFY2(controller.unknownTarget().isEmpty(), qPrintable(outcome));
            QVERIFY2(controller.busy(), qPrintable(outcome));
        }
    }

    // Decided 2026-10-18 (C7): /createRoom federates the invite before it
    // answers, so a DM to a homeserver that is down or slow left an empty room
    // that can never be reused. When the profile lookup could not reach their
    // server (our server's federation request failed: "network", or the 10 s
    // bound ran out: "timeout") the dialog asks first, as for "not found",
    // and says why. Old code created the room at once.
    void aDmToAServerThatDoesNotAnswerAsksFirst()
    {
        for (const QString &category : { QStringLiteral("network"),
                                          QStringLiteral("timeout") }) {
            FakeClient client;
            client.profileLookupSupported = true;
            ConversationController controller;
            controller.setClient(&client);
            const QString far = QStringLiteral("@bob:down.example");

            controller.startDirectMessage(far);
            Q_EMIT client.userProfileFinished(client.lastProfileOp, false, far,
                                              QString(), QString(), category);
            QVERIFY2(client.createDmCalls == 0, qPrintable(category));
            QCOMPARE(controller.unknownTarget(), far);
            QVERIFY2(controller.unknownTargetUnreachable(), qPrintable(category));
            QVERIFY(!controller.busy());

            controller.dismissUnknownTarget();
            QVERIFY(controller.unknownTarget().isEmpty());
            QVERIFY(!controller.unknownTargetUnreachable());
            QCOMPARE(client.createDmCalls, 0);

            // The person may still go ahead.
            controller.startDirectMessage(far);
            Q_EMIT client.userProfileFinished(client.lastProfileOp, false, far,
                                              QString(), QString(), category);
            controller.startDirectMessageAnyway(far);
            QCOMPARE(client.createDmCalls, 1);
            QCOMPARE(client.lastDmUser, far);
            QVERIFY(!controller.unknownTargetUnreachable());
        }

        // "Not found" keeps its own wording.
        FakeClient client;
        client.profileLookupSupported = true;
        ConversationController controller;
        controller.setClient(&client);
        const QString nobody = QStringLiteral("@nobody:example.org");
        controller.startDirectMessage(nobody);
        Q_EMIT client.userProfileFinished(client.lastProfileOp, false, nobody,
                                          QString(), QString(),
                                          QStringLiteral("not_found"));
        QCOMPARE(controller.unknownTarget(), nobody);
        QVERIFY(!controller.unknownTargetUnreachable());
    }

    // The check has done nothing server-side, so closing the dialog abandons
    // it: the late answer must not create a room the person walked away from.
    // Other profile answers (search, avatars, pills) are not ours.
    void aCheckAbandonedByClosingTheDialogCreatesNothing()
    {
        FakeClient client;
        client.profileLookupSupported = true;
        ConversationController controller;
        controller.setClient(&client);
        const QString user = QStringLiteral("@alice:example.org");

        controller.startDirectMessage(user);
        const quint64 checkOp = client.lastProfileOp;
        Q_EMIT client.userProfileFinished(checkOp + 100, true, user,
                                          QStringLiteral("Alice"), QString(),
                                          QString());
        QCOMPARE(client.createDmCalls, 0);
        QVERIFY(controller.busy());

        controller.reset();
        QVERIFY(!controller.busy());
        Q_EMIT client.userProfileFinished(checkOp, true, user,
                                          QStringLiteral("Alice"), QString(),
                                          QString());
        QCOMPARE(client.createDmCalls, 0);
    }

    // No answer at all is not "does not exist", and not "reachable" either
    // (decided 2026-10-18, C7): the bound asks with the unreachable wording,
    // as rooms.rs's own "timeout" does, and a late answer creates nothing.
    // Old code created the room when the bound ran out.
    void aCheckThatNeverAnswersAsksFirst()
    {
        FakeClient client;
        client.profileLookupSupported = true;
        ConversationController controller;
        controller.setClient(&client);
        controller.setTargetCheckTimeoutMsForTest(20);
        const QString user = QStringLiteral("@alice:example.org");
        controller.startDirectMessage(user);
        const quint64 checkOp = client.lastProfileOp;
        QCOMPARE(client.createDmCalls, 0);
        QVERIFY(controller.busy());
        QTRY_COMPARE(controller.unknownTarget(), user);
        QVERIFY(controller.unknownTargetUnreachable());
        QVERIFY(!controller.busy());
        QCOMPARE(client.createDmCalls, 0);

        Q_EMIT client.userProfileFinished(checkOp, true, user,
                                          QStringLiteral("Alice"), QString(),
                                          QString());
        QCOMPARE(client.createDmCalls, 0);

        controller.startDirectMessageAnyway(user);
        QCOMPARE(client.createDmCalls, 1);
        QCOMPARE(client.lastDmUser, user);
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
        // Not open yet: the room is not in the authoritative list.
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

        // Sign-out clears the pending operation...
        Q_EMIT client.loggedOut();
        QVERIFY(!controller.busy());

        // ...so the late completion opens nothing.
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

    // ---- Space creation routing -------------------------------------------

    void spaceCreateEmitsSpaceReadyNeverConversationReady()
    {
        // An m.space room is never opened as a message timeline: the
        // controller owns the isSpace decision (a dialog closed mid-create
        // cannot reroute it) and emits spaceReady.
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        QSignalSpy ready(&controller,
                         &ConversationController::conversationReady);
        QSignalSpy spaceReady(&controller,
                              &ConversationController::spaceReady);

        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("My Space") },
            { QStringLiteral("isSpace"), true },
        });
        QCOMPARE(client.createRoomCalls, 1);
        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!s:example.org"),
                                         QString(), QString());
        client.mirror = { makeRoom(QStringLiteral("!s:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(spaceReady.count(), 1);
        QCOMPARE(spaceReady.at(0).at(0).toString(),
                 QStringLiteral("!s:example.org"));
        QCOMPARE(ready.count(), 0);

        // The flag never leaks into the NEXT ordinary create.
        controller.createRoom({
            { QStringLiteral("name"), QStringLiteral("Plain") } });
        Q_EMIT client.roomCreateFinished(client.lastOpId, true,
                                         QStringLiteral("!p:example.org"),
                                         QString(), QString());
        client.mirror = { makeRoom(QStringLiteral("!p:example.org")) };
        Q_EMIT client.roomsChanged();
        QCOMPARE(spaceReady.count(), 1);
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

        // The room opens before the avatar upload completes: the avatar op is
        // outside busy() and cannot stall the open.
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

    // Closing the dialog must not unlock a second create. `Client::create_dm`
    // is one /createRoom carrying the invite, and an unreachable peer server
    // can hang it; the server still creates the room, without the m.direct
    // write that would let it be reused. So reset() keeps the pending guard,
    // bounded by the late answer.
    void closingTheDialogDoesNotUnlockASecondCreate()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);

        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        QCOMPARE(client.createDmCalls, 1);
        QVERIFY(controller.busy());

        // The create has not answered; the user closes the dialog.
        controller.reset();

        QVERIFY2(controller.busy(),
                 "closing the dialog cleared the in-flight guard; the create "
                 "is still running server-side and a second one would leave a "
                 "second empty room");
        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        QCOMPARE(client.createDmCalls, 1);

        // A third attempt after reopening is refused too.
        controller.reset();
        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        QCOMPARE(client.createDmCalls, 1);

        // The guard is not permanent: the late answer releases it.
        Q_EMIT client.dmCreateFinished(client.lastOpId, false, QString(),
                                       QStringLiteral("network"));
        QVERIFY2(!controller.busy(),
                 "the guard outlived the operation it was guarding");
        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        QCOMPARE(client.createDmCalls, 2);
    }

    // The 60 s bound released the guard while the create was still running
    // server-side (spawn_room_action has no timeout), so the retry the error
    // text invited made one more empty room per attempt.
    void aTimedOutCreateStillBlocksARetryForTheSamePerson()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.setOpTimeoutMsForTest(20);
        const QString user = QStringLiteral("@remote:other.example");

        controller.startDirectMessage(user);
        QCOMPARE(client.createDmCalls, 1);
        const quint64 firstOp = client.lastOpId;
        QTRY_VERIFY(!controller.busy());   // the bound fired

        controller.startDirectMessage(user);
        QCOMPARE(client.createDmCalls, 1);
        QVERIFY(!controller.errorMessage().isEmpty());

        // Someone else is a different action.
        controller.startDirectMessage(QStringLiteral("@other:other.example"));
        QCOMPARE(client.createDmCalls, 2);
        Q_EMIT client.dmCreateFinished(client.lastOpId, false, QString(),
                                       QStringLiteral("forbidden"));

        // The late answer for the first create lifts its guard, and does not
        // open or fail anything.
        Q_EMIT client.dmCreateFinished(firstOp, false, QString(),
                                       QStringLiteral("network"));
        controller.startDirectMessage(user);
        QCOMPARE(client.createDmCalls, 3);
    }

    // What the backend really produces: rooms.rs sends /createRoom once, with
    // no retry and a 120 s timeout (create_request_config), so it cannot answer
    // before this 60 s guard fires. The order is always: the guard fires and
    // arms the per-person block, the dialog may be closed and reopened, and the
    // answer arrives LATE (success: the room is made, filed as a DM by the
    // backend's own mark_as_dm and listed by sync; or a failure). The block
    // must hold until then, and the late answer must lift it without opening
    // or failing anything.
    void theGuardSurvivesAReopenAndALateSuccessLiftsIt()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.setOpTimeoutMsForTest(20);
        const QString user = QStringLiteral("@remote:other.example");

        controller.startDirectMessage(user);
        const quint64 firstOp = client.lastOpId;
        QTRY_VERIFY(!controller.busy());
        const QString shown = controller.errorMessage();
        QVERIFY(shown.contains(QLatin1String("taking longer")));

        // Closing and reopening the dialog does not forget the running create.
        controller.reset();
        controller.startDirectMessage(user);
        QCOMPARE(client.createDmCalls, 1);

        Q_EMIT client.dmCreateFinished(firstOp, true,
                                       QStringLiteral("!late:other.example"),
                                       QString());
        QVERIFY(!controller.busy());
        controller.startDirectMessage(user);
        QCOMPARE(client.createDmCalls, 2);
    }

    void twoTimedOutCreatesKeepBothGuards()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.setOpTimeoutMsForTest(20);
        const QString a = QStringLiteral("@a:other.example");
        const QString b = QStringLiteral("@b:other.example");
        controller.startDirectMessage(a);
        QTRY_VERIFY(!controller.busy());
        controller.startDirectMessage(b);
        QCOMPARE(client.createDmCalls, 2);
        QTRY_VERIFY(!controller.busy());
        controller.startDirectMessage(a);
        controller.startDirectMessage(b);
        QCOMPARE(client.createDmCalls, 2);
        // Sign-out clears every guard.
        Q_EMIT client.loggedOut();
        controller.startDirectMessage(a);
        QCOMPARE(client.createDmCalls, 3);
    }

    void aDmFailureThatMayHaveMadeARoomSaysSo()
    {
        FakeClient client;
        ConversationController controller;
        controller.setClient(&client);
        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        Q_EMIT client.dmCreateFinished(client.lastOpId, false, QString(),
                                       QStringLiteral("network"));
        QVERIFY(controller.errorMessage().contains(QLatin1String("empty room")));
        controller.startDirectMessage(QStringLiteral("@remote:other.example"));
        Q_EMIT client.dmCreateFinished(client.lastOpId, false, QString(),
                                       QStringLiteral("forbidden"));
        QVERIFY(!controller.errorMessage().contains(QLatin1String("empty room")));
    }
};

QTEST_GUILESS_MAIN(ConversationFlowTest)
#include "ConversationFlowTest.moc"
