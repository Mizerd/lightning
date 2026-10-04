// The read-only event context view and its routing: a jump target that is not
// in the loaded timeline opens the view instead of walking history; a loaded
// one is jumped to in place; a failed /context falls back to the walk; late
// results from a closed, replaced or room-switched view are ignored.

#include "matrix/MockMatrixClient.h"
#include "models/ContextController.h"
#include "models/PaginationController.h"
#include "models/TimelineModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 2000;
const QString kGeneral = QStringLiteral("!general:mock.local");
const QString kDevs = QStringLiteral("!devs:mock.local");
} // namespace

class ContextControllerTest : public QObject
{
    Q_OBJECT

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("unused"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

    // An event in the middle of the room's mock timeline, so the context
    // window has rows on both sides.
    static QString middleEventId(MockMatrixClient &client)
    {
        const auto events = client.timeline(kGeneral);
        for (int i = events.size() / 2; i < events.size(); ++i) {
            if (!events.at(i).eventId.isEmpty()
                && events.at(i).threadRootId.isEmpty())
                return events.at(i).eventId;
        }
        return {};
    }

private Q_SLOTS:
    void aLoadedTargetIsJumpedToInPlaceAndNeverOpensTheView()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        TimelineModel live;
        live.setClient(&client);
        live.setRoomId(kGeneral);
        PaginationController pagination;
        pagination.setClient(&client);
        pagination.setTimelineModel(&live);
        pagination.setRoomId(kGeneral);
        int opened = 0;
        pagination.setContextOpener([&](const QString &id) {
            ++opened;
            return context.open(kGeneral, id);
        });
        const QString target = middleEventId(client);
        QVERIFY(!target.isEmpty());
        QVERIFY(live.rowForStableId(target) >= 0);

        QSignalSpy located(&pagination, &PaginationController::targetLocated);
        pagination.jumpToEvent(target);
        QCOMPARE(opened, 0);
        QCOMPARE(client.eventContextOpenCount(), 0);
        QVERIFY(!context.active());
        QCOMPARE(located.count(), 1);
    }

    void aLoadedJumpClosesAnOpenViewSoItLandsVisibly()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        TimelineModel live;
        live.setClient(&client);
        live.setRoomId(kGeneral);
        PaginationController pagination;
        pagination.setClient(&client);
        pagination.setTimelineModel(&live);
        pagination.setRoomId(kGeneral);
        pagination.setContextCloser([&] { context.close(); });
        const QString target = middleEventId(client);
        QVERIFY(context.open(kGeneral, target));
        QCOMPARE(context.state(), ContextController::Ready);
        pagination.jumpToEvent(target); // loaded in the live timeline
        QCOMPARE(context.state(), ContextController::Closed);
    }

    void aTargetThatIsNotLoadedOpensTheViewInsteadOfWalking()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        TimelineModel emptyLive; // nothing loaded: every target is unloaded
        emptyLive.setClient(&client);
        PaginationController pagination;
        pagination.setClient(&client);
        pagination.setTimelineModel(&emptyLive);
        pagination.setRoomId(kGeneral);
        pagination.setContextOpener([&](const QString &id) {
            return context.open(kGeneral, id);
        });
        const QString target = middleEventId(client);
        QVERIFY(!target.isEmpty());

        QSignalSpy located(&context, &ContextController::targetLocated);
        pagination.jumpToEvent(target);

        QCOMPARE(client.eventContextOpenCount(), 1);
        QCOMPARE(context.state(), ContextController::Ready);
        QCOMPARE(context.eventId(), target);
        QCOMPARE(located.count(), 1);
        const int row = located.first().first().toInt();
        QCOMPARE(row, context.targetRow());
        QVERIFY(row >= 0);
        // Bounded: a window around the hit, not the room's history.
        QVERIFY(context.model()->rowCount() <= 2 * MockMatrixClient::kContextRadius + 1);
        QVERIFY(context.model()->rowForStableId(target) >= 0);
        // The history walk did not run.
        QVERIFY(!pagination.navigating());
    }

    void aBackendWithoutContextFallsBackToTheWalk()
    {
        MockMatrixClient client; // event context left disabled
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        TimelineModel emptyLive;
        emptyLive.setClient(&client);
        PaginationController pagination;
        pagination.setClient(&client);
        pagination.setTimelineModel(&emptyLive);
        pagination.setRoomId(kGeneral);
        pagination.setContextOpener([&](const QString &id) {
            return context.open(kGeneral, id);
        });
        QVERIFY(!context.open(kGeneral, QStringLiteral("$x:mock.local")));
        pagination.jumpToEvent(QStringLiteral("$x:mock.local"));
        QCOMPARE(client.eventContextOpenCount(), 0);
        QVERIFY(!context.active());
        // The walk ran and ended honestly rather than doing nothing.
        QVERIFY(pagination.navigating() || !pagination.navigationMessage().isEmpty());
    }

    void aFailedContextFallsBackToTheWalkWithAnHonestMessage()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        TimelineModel emptyLive;
        emptyLive.setClient(&client);
        PaginationController pagination;
        pagination.setClient(&client);
        pagination.setTimelineModel(&emptyLive);
        pagination.setRoomId(kGeneral);
        pagination.setContextOpener([&](const QString &id) {
            return context.open(kGeneral, id);
        });
        // The same routing AppController installs.
        connect(&context, &ContextController::openFailed, &pagination,
                [&](const QString &, const QString &eventId, const QString &) {
            pagination.walkToEvent(eventId);
        });
        QSignalSpy failed(&context, &ContextController::openFailed);
        pagination.jumpToEvent(QStringLiteral("$unknown:mock.local"));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!context.active());
        QVERIFY(!pagination.navigationMessage().isEmpty()
                || pagination.navigating());
    }

    void aResultThatLandsAfterCloseIsIgnored()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        client.setHoldEventContext(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        const QString target = middleEventId(client);
        QSignalSpy located(&context, &ContextController::targetLocated);
        QVERIFY(context.open(kGeneral, target));
        QCOMPARE(context.state(), ContextController::Opening);
        context.close();
        client.deliverHeldEventContext(); // the late reset
        QCOMPARE(context.state(), ContextController::Closed);
        QCOMPARE(context.model()->rowCount(), 0);
        QCOMPARE(located.count(), 0);
    }

    void aResultThatLandsAfterARoomSwitchIsIgnored()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        client.setHoldEventContext(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QVERIFY(context.open(kGeneral, middleEventId(client)));
        context.handleCurrentRoomChanged(kDevs);
        QCOMPARE(context.state(), ContextController::Closed);
        QCOMPARE(client.eventContextCloseCount(), 1);
        client.deliverHeldEventContext();
        QCOMPARE(context.state(), ContextController::Closed);
        QCOMPARE(context.model()->rowCount(), 0);
    }

    void theSameRoomKeepsTheViewOpen()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QVERIFY(context.open(kGeneral, middleEventId(client)));
        context.handleCurrentRoomChanged(kGeneral);
        QCOMPARE(context.state(), ContextController::Ready);
    }

    void signOutClosesTheView()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QVERIFY(context.open(kGeneral, middleEventId(client)));
        QCOMPARE(context.state(), ContextController::Ready);
        context.setClient(nullptr); // the account's client goes away
        QCOMPARE(context.state(), ContextController::Closed);
        QCOMPARE(client.eventContextCloseCount(), 1);
    }

    void jumpToLatestClosesTheViewAndAsksForTheLiveTimeline()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QVERIFY(context.open(kGeneral, middleEventId(client)));
        QSignalSpy latest(&context, &ContextController::latestRequested);
        context.jumpToLatest();
        QCOMPARE(context.state(), ContextController::Closed);
        QCOMPARE(latest.count(), 1);
        QCOMPARE(client.eventContextCloseCount(), 1);
        QVERIFY(context.eventId().isEmpty());
        QCOMPARE(context.model()->rowCount(), 0);
        // Closed: a second jump is a no-op, not a second close.
        context.jumpToLatest();
        QCOMPARE(latest.count(), 1);
    }

    void openingAnotherEventReplacesTheView()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        const auto events = client.timeline(kGeneral);
        QString a, b;
        for (const auto &e : events) {
            if (e.eventId.isEmpty() || !e.threadRootId.isEmpty())
                continue;
            if (a.isEmpty())
                a = e.eventId;
            else if (b.isEmpty() && e.eventId != a)
                b = e.eventId;
        }
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        QVERIFY(context.open(kGeneral, a));
        QVERIFY(context.open(kGeneral, b));
        QCOMPARE(context.eventId(), b);
        QCOMPARE(context.state(), ContextController::Ready);
        QVERIFY(context.model()->rowForStableId(b) >= 0);
        QCOMPARE(client.openEventContextTimelineId(),
                 MatrixClient::contextTimelineId(kGeneral, b));
    }

    void edgesAreBoundedAndReportTheirEnd()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QVERIFY(context.open(kGeneral, middleEventId(client)));
        QVERIFY(context.canLoadOlder());
        QVERIFY(context.canLoadNewer());
        context.loadOlder();
        QCOMPARE(client.eventContextPaginateCount(), 1);
        QVERIFY(context.reachedStart()); // the mock answers "start reached"
        QVERIFY(!context.canLoadOlder());
        context.loadOlder(); // refused, not dispatched
        QCOMPARE(client.eventContextPaginateCount(), 1);
        context.loadNewer();
        QVERIFY(context.reachedEnd());
        QVERIFY(!context.canLoadNewer());
        QCOMPARE(client.eventContextPaginateCount(), 2);
    }

    void aPaginationAnswerForAClosedViewIsIgnored()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        const QString target = middleEventId(client);
        QVERIFY(context.open(kGeneral, target));
        context.close();
        // A late answer from the closed view must not mark anything reached.
        Q_EMIT client.eventContextPagination(kGeneral, target, false,
                                             QStringLiteral("idle"), true);
        QVERIFY(!context.reachedStart());
        QVERIFY(!context.loadingOlder());
    }

    void aThreadReplyHitHandsOverToTheThreadPanel()
    {
        MockMatrixClient client;
        client.setEventContextEnabled(true);
        QVERIFY(login(client));
        ContextController context;
        context.setClient(&client);
        QString reply, root;
        for (const auto &e : client.timeline(kGeneral)) {
            if (!e.threadRootId.isEmpty() && !e.eventId.isEmpty()
                && e.eventId != e.threadRootId) {
                reply = e.eventId;
                root = e.threadRootId;
                break;
            }
        }
        QVERIFY2(!reply.isEmpty(), "fixture has no thread reply");
        QSignalSpy hit(&context, &ContextController::threadReplyHit);
        QVERIFY(context.open(kGeneral, reply));
        QCOMPARE(hit.count(), 1);
        QCOMPARE(hit.first().at(1).toString(), root);
        QCOMPARE(hit.first().at(2).toString(), reply);
        QCOMPARE(context.state(), ContextController::Closed);
    }

    void contextIdsNeverLookLikeThreadOrRoomIds()
    {
        const QString id = MatrixClient::contextTimelineId(kGeneral,
                                                           QStringLiteral("$e:x"));
        QVERIFY(MatrixClient::isContextTimelineId(id));
        QVERIFY(!MatrixClient::isThreadTimelineId(id));
        QVERIFY(!MatrixClient::isContextTimelineId(kGeneral));
        QVERIFY(!MatrixClient::isContextTimelineId(
            MatrixClient::threadTimelineId(kGeneral, QStringLiteral("$e:x"))));
        QCOMPARE(MatrixClient::threadTimelineRoomId(id), kGeneral);
    }
};

QTEST_MAIN(ContextControllerTest)
#include "ContextControllerTest.moc"
