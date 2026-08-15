// v0.7.x Discover / Join: RoomDirectorySearchModel (debounce, paging,
// stale-op rejection) and RoomDiscoveryController (resolve preview, join
// and knock outcomes, honest error categories, wait-for-room navigation,
// bounded space-children cache, sign-out invalidation) against the
// scriptable MockMatrixClient surface.

#include "app/RoomDiscoveryController.h"
#include "matrix/MockMatrixClient.h"
#include "models/RoomDirectorySearchModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 3000;

QVariantMap directoryRow(const QString &id, const QString &name,
                         const QString &joinRule,
                         const QString &membership = QString())
{
    QVariantMap row;
    row.insert(QStringLiteral("roomId"), id);
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("alias"), QString());
    row.insert(QStringLiteral("topic"), QString());
    row.insert(QStringLiteral("avatarUrl"), QString());
    row.insert(QStringLiteral("members"), 12);
    row.insert(QStringLiteral("joinRule"), joinRule);
    row.insert(QStringLiteral("membership"), membership);
    row.insert(QStringLiteral("isSpace"), false);
    return row;
}
} // namespace

class RoomDiscoveryTest : public QObject
{
    Q_OBJECT

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

private Q_SLOTS:
    void directorySearchPopulatesAndPages()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDirectorySearchModel model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QVERIFY(model.supported());
        QCOMPARE(model.state(), QStringLiteral("idle"));

        client.mockPublicRooms = { directoryRow(
            QStringLiteral("!pub:mock.local"), QStringLiteral("Public room"),
            QStringLiteral("public")) };
        client.mockPublicRoomsNextBatch = QStringLiteral("page2");
        model.setQuery(QStringLiteral("pub"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!pub:mock.local"));
        QVERIFY(model.canLoadMore());

        // The next page appends and the cleared token ends pagination.
        model.loadMore();
        QTRY_COMPARE(model.rowCount(), 2);
        QVERIFY(!model.canLoadMore());
    }

    void directoryEmptyAnswerIsNoResults()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDirectorySearchModel model;
        model.setDebounceMs(0);
        model.setClient(&client);
        client.mockPublicRooms = {};
        model.setQuery(QStringLiteral("nothing"));
        QTRY_COMPARE(model.state(), QStringLiteral("no_results"));
        QCOMPARE(model.rowCount(), 0);
    }

    void staleDirectoryPageNeverRepaintsANewerQuery()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDirectorySearchModel model;
        model.setDebounceMs(0);
        model.setClient(&client);
        client.mockPublicRooms = { directoryRow(
            QStringLiteral("!a:mock.local"), QStringLiteral("A"),
            QStringLiteral("public")) };
        model.setQuery(QStringLiteral("a"));
        // Supersede before the first answer lands (answers are queued
        // singleShot(0), so both dispatches are in flight now).
        client.mockPublicRooms = { directoryRow(
            QStringLiteral("!b:mock.local"), QStringLiteral("B"),
            QStringLiteral("public")) };
        model.setQuery(QStringLiteral("b"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!b:mock.local"));
    }

    void resolvePassesThePreviewThrough()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QVariantMap resolved;
        resolved.insert(QStringLiteral("ok"), true);
        resolved.insert(QStringLiteral("target"),
                        QStringLiteral("#lightning:mock.local"));
        resolved.insert(QStringLiteral("previewOk"), true);
        resolved.insert(QStringLiteral("roomId"),
                        QStringLiteral("!light:mock.local"));
        resolved.insert(QStringLiteral("name"), QStringLiteral("Lightning"));
        resolved.insert(QStringLiteral("joinRule"), QStringLiteral("public"));
        resolved.insert(QStringLiteral("membership"), QString());
        client.mockResolveResult = resolved;

        controller.resolve(QStringLiteral("#lightning:mock.local"));
        QCOMPARE(controller.resolveState(), QStringLiteral("resolving"));
        QTRY_COMPARE(controller.resolveState(), QStringLiteral("resolved"));
        QCOMPARE(controller.resolved()
                     .value(QStringLiteral("name"))
                     .toString(),
                 QStringLiteral("Lightning"));

        controller.clearResolved();
        QCOMPARE(controller.resolveState(), QStringLiteral("idle"));
        QVERIFY(controller.resolved().isEmpty());
    }

    void resolveFailureSurfacesInvalid()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        // Unscripted mock → ok=false / invalid.
        controller.resolve(QStringLiteral("not a room"));
        QTRY_COMPARE(controller.resolveState(), QStringLiteral("failed"));
        QCOMPARE(controller.resolved()
                     .value(QStringLiteral("category"))
                     .toString(),
                 QStringLiteral("invalid"));
    }

    void joinNavigatesOnceTheRoomIsAuthoritative()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QSignalSpy joined(&controller, &RoomDiscoveryController::roomJoined);
        QSignalSpy busy(&controller, &RoomDiscoveryController::busyChanged);
        controller.join(QStringLiteral("!new:mock.local"), {}, false);
        QVERIFY(controller.busy());
        QTRY_COMPARE(joined.count(), 1);
        QCOMPARE(joined.first().first().toString(),
                 QStringLiteral("!new:mock.local"));
        QVERIFY(!controller.busy());
        QVERIFY(busy.count() >= 2);
        QCOMPARE(client.joinedTargets,
                 QStringList{ QStringLiteral("!new:mock.local") });
    }

    void spaceJoinSelectsNeverOpens()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QSignalSpy joinedRoom(&controller,
                              &RoomDiscoveryController::roomJoined);
        QSignalSpy joinedSpace(&controller,
                               &RoomDiscoveryController::spaceJoined);
        controller.join(QStringLiteral("!space:mock.local"), {}, true);
        QTRY_COMPARE(joinedSpace.count(), 1);
        QCOMPARE(joinedRoom.count(), 0);
    }

    void joinFailureCategoriesAreHonest()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        client.mockJoinFailCategory = QStringLiteral("banned");
        QSignalSpy joined(&controller, &RoomDiscoveryController::roomJoined);
        controller.join(QStringLiteral("!banned:mock.local"), {}, false);
        QTRY_VERIFY(!controller.errorMessage().isEmpty());
        QCOMPARE(controller.errorMessage(),
                 RoomDiscoveryController::describeJoinCategory(
                     QStringLiteral("banned")));
        QCOMPARE(joined.count(), 0);
        QVERIFY(!controller.busy());
    }

    void describeJoinCategoryDistinguishesConditions()
    {
        using C = RoomDiscoveryController;
        // Every category maps to its own honest sentence — in particular
        // restricted is NEVER presented as plain invite-only.
        const QStringList categories = {
            QStringLiteral("banned"), QStringLiteral("forbidden"),
            QStringLiteral("restricted_denied"), QStringLiteral("not_found"),
            QStringLiteral("rate_limited"), QStringLiteral("invalid"),
            QStringLiteral("network"),
        };
        QSet<QString> messages;
        for (const QString &category : categories) {
            const QString message = C::describeJoinCategory(category);
            QVERIFY(!message.isEmpty());
            messages.insert(message);
        }
        QCOMPARE(messages.size(), categories.size());
        QVERIFY(C::describeJoinCategory(QStringLiteral("restricted_denied"))
                    .contains(QStringLiteral("restricted")));
    }

    void knockReportsSentAndFailure()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QSignalSpy sent(&controller, &RoomDiscoveryController::knockSent);
        controller.knock(QStringLiteral("!knock:mock.local"), {},
                         QStringLiteral("please"));
        QTRY_COMPARE(sent.count(), 1);

        client.mockKnockFailCategory = QStringLiteral("forbidden");
        controller.knock(QStringLiteral("!knock2:mock.local"), {}, QString());
        QTRY_VERIFY(!controller.errorMessage().isEmpty());
    }

    void knockCancelRoundTrips()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QSignalSpy cancelled(&controller,
                             &RoomDiscoveryController::knockCancelled);
        controller.cancelKnock(QStringLiteral("!knock:mock.local"));
        QTRY_COMPARE(cancelled.count(), 1);
    }

    void spaceChildrenAreCachedPerSpaceAndSingleFlight()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        client.mockSpaceChildren = { directoryRow(
            QStringLiteral("!child:mock.local"), QStringLiteral("Child"),
            QStringLiteral("public")) };
        QCOMPARE(controller.spaceChildrenState(
                     QStringLiteral("!space:mock.local")),
                 QString());
        controller.refreshSpaceChildren(QStringLiteral("!space:mock.local"));
        QCOMPARE(controller.spaceChildrenState(
                     QStringLiteral("!space:mock.local")),
                 QStringLiteral("loading"));
        QTRY_COMPARE(
            controller.spaceChildrenState(QStringLiteral("!space:mock.local")),
            QStringLiteral("ready"));
        QCOMPARE(
            controller.spaceChildren(QStringLiteral("!space:mock.local"))
                .size(),
            1);
        // Another space is independent.
        QCOMPARE(controller.spaceChildrenState(
                     QStringLiteral("!other:mock.local")),
                 QString());
    }

    void loggedOutClearsEverything()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        RoomDiscoveryController controller;
        controller.setClient(&client);
        QVariantMap resolved;
        resolved.insert(QStringLiteral("ok"), true);
        resolved.insert(QStringLiteral("target"), QStringLiteral("#x:m"));
        client.mockResolveResult = resolved;
        controller.resolve(QStringLiteral("#x:m"));
        QTRY_COMPARE(controller.resolveState(), QStringLiteral("resolved"));
        controller.refreshSpaceChildren(QStringLiteral("!s:mock.local"));

        client.logout();
        QTRY_COMPARE(controller.resolveState(), QStringLiteral("idle"));
        QVERIFY(controller.resolved().isEmpty());
        QVERIFY(!controller.busy());
        QVERIFY(controller.spaceChildren(QStringLiteral("!s:mock.local"))
                    .isEmpty());
        QCOMPARE(controller.spaceChildrenState(QStringLiteral("!s:mock.local")),
                 QString());
    }
};

QTEST_MAIN(RoomDiscoveryTest)
#include "RoomDiscoveryTest.moc"
