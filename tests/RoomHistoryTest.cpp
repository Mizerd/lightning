#include <QtTest>

#include "app/RoomHistory.h"

using lightning::RoomHistory;

namespace {

const RoomHistory::Openable kAll = [](const QString &) { return true; };

QString room(int n) { return QStringLiteral("!r%1:x").arg(n); }

// Opens rooms 1..n in order.
RoomHistory opened(int n)
{
    RoomHistory h;
    for (int i = 1; i <= n; ++i)
        h.visit(room(i));
    return h;
}

} // namespace

// Pure navigation state behind the mouse Back/Forward buttons and
// nav.historyBack/Forward: capped, truncating, skipping, in memory only.
class RoomHistoryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void empty_historyGoesNowhere()
    {
        RoomHistory h;
        QVERIFY(!h.canGoBack(kAll));
        QVERIFY(!h.canGoForward(kAll));
        QCOMPARE(h.back(kAll), QString());
        QCOMPARE(h.forward(kAll), QString());
    }

    void backAndForwardWalkTheVisitedRooms()
    {
        RoomHistory h = opened(3);
        QCOMPARE(h.back(kAll), room(2));
        QCOMPARE(h.back(kAll), room(1));
        QCOMPARE(h.back(kAll), QString()); // already at the oldest
        QCOMPARE(h.cursor(), 0);
        QCOMPARE(h.forward(kAll), room(2));
        QCOMPARE(h.forward(kAll), room(3));
        QCOMPARE(h.forward(kAll), QString());
    }

    void aNewVisitAfterGoingBackTruncatesTheForwardList()
    {
        RoomHistory h = opened(3);
        h.back(kAll);
        h.back(kAll); // at room 1, forward list is 2, 3
        QVERIFY(h.canGoForward(kAll));
        h.visit(room(9));
        QVERIFY(!h.canGoForward(kAll));
        QCOMPARE(h.entries(), (QStringList{room(1), room(9)}));
        QCOMPARE(h.back(kAll), room(1));
    }

    void reselectingTheOpenRoomIsNotANewNavigation()
    {
        RoomHistory h = opened(3);
        h.back(kAll); // at 2, forward list has 3
        h.visit(room(2));
        QVERIFY(h.canGoForward(kAll));
        QCOMPARE(h.entries().size(), 3);
    }

    void keepsAtMostTenBackEntries()
    {
        RoomHistory h = opened(25);
        int steps = 0;
        while (!h.back(kAll).isEmpty())
            ++steps;
        QCOMPARE(steps, RoomHistory::kMaxBackEntries);
        QCOMPARE(RoomHistory::kMaxBackEntries, 10);
        // The oldest reachable room is the 11th from the end, nothing older.
        QCOMPARE(h.entries().first(), room(15));
    }

    void theCapAlsoHoldsWhileNoRoomIsOpen()
    {
        RoomHistory h = opened(25);
        h.visit(QString()); // lobby: Back now returns to the room just left
        QVERIFY(h.detached());
        int steps = 0;
        while (!h.back(kAll).isEmpty())
            ++steps;
        QCOMPARE(steps, RoomHistory::kMaxBackEntries);
    }

    void backFromTheLobbyReturnsToTheLastRoom()
    {
        RoomHistory h = opened(2);
        h.visit(QString());
        QVERIFY(h.canGoBack(kAll));
        QCOMPARE(h.back(kAll), room(2));
        QVERIFY(!h.detached());
        QCOMPARE(h.back(kAll), room(1));
    }

    void forwardFromTheLobbyGoesToTheEntryAfterTheCursor()
    {
        RoomHistory h = opened(3);
        h.back(kAll); // at 2
        h.visit(QString());
        QCOMPARE(h.forward(kAll), room(3));
    }

    void openingARoomFromTheLobbyIsANewVisit()
    {
        RoomHistory h = opened(2);
        h.visit(QString());
        h.visit(room(7));
        QCOMPARE(h.entries(), (QStringList{room(1), room(2), room(7)}));
        QCOMPARE(h.back(kAll), room(2));
    }

    void closingWithNothingOpenedRecordsNothing()
    {
        RoomHistory h;
        h.visit(QString());
        QVERIFY(h.entries().isEmpty());
        QVERIFY(!h.canGoBack(kAll));
    }

    void leftOrForgottenRoomsAreSkippedAndDropped()
    {
        RoomHistory h = opened(4);
        const RoomHistory::Openable noRoom2And3 = [](const QString &id) {
            return id != room(2) && id != room(3);
        };
        QCOMPARE(h.back(noRoom2And3), room(1));
        QCOMPARE(h.entries(), (QStringList{room(1), room(4)}));
        QCOMPARE(h.forward(kAll), room(4));
    }

    void skippingReportsNothingWhenEveryEarlierRoomIsGone()
    {
        RoomHistory h = opened(3);
        const RoomHistory::Openable onlyCurrent = [](const QString &id) {
            return id == room(3);
        };
        QVERIFY(!h.canGoBack(onlyCurrent));
        QCOMPARE(h.back(onlyCurrent), QString());
        QCOMPARE(h.entries(), (QStringList{room(3)}));
    }

    void aForwardRoomThatWasLeftIsSkipped()
    {
        RoomHistory h = opened(3);
        h.back(kAll);
        h.back(kAll); // at 1
        const RoomHistory::Openable no2 = [](const QString &id) {
            return id != room(2);
        };
        QCOMPARE(h.forward(no2), room(3));
    }

    void pruningNeverLeavesTheSameRoomTwiceInARow()
    {
        RoomHistory h;
        for (const QString &id : {room(1), room(2), room(1)})
            h.visit(id);
        // 1, 2, 1 with 2 gone would make Back from 1 "go" to 1.
        const RoomHistory::Openable no2 = [](const QString &id) {
            return id != room(2);
        };
        QVERIFY(!h.canGoBack(no2));
        QCOMPARE(h.back(no2), QString());
        QCOMPARE(h.entries(), (QStringList{room(1)}));
    }

    void theOpenRoomItselfIsNeverPruned()
    {
        RoomHistory h = opened(2);
        const RoomHistory::Openable none = [](const QString &) { return false; };
        QCOMPARE(h.back(none), QString());
        QCOMPARE(h.entries(), (QStringList{room(2)}));
        QCOMPARE(h.cursor(), 0);
    }

    void canGoDoesNotMoveTheCursor()
    {
        RoomHistory h = opened(3);
        QVERIFY(h.canGoBack(kAll));
        QVERIFY(!h.canGoForward(kAll));
        QCOMPARE(h.cursor(), 2);
        const RoomHistory::Openable no1 = [](const QString &id) {
            return id != room(1);
        };
        QVERIFY(h.canGoBack(no1));
        QCOMPARE(h.entries().size(), 3); // not pruned by a query
    }

    void clearForgetsEverything()
    {
        RoomHistory h = opened(5);
        h.back(kAll);
        h.clear();
        QVERIFY(h.entries().isEmpty());
        QVERIFY(!h.canGoBack(kAll));
        QVERIFY(!h.canGoForward(kAll));
        QVERIFY(!h.detached());
        h.visit(room(1));
        QCOMPARE(h.entries().size(), 1);
    }
};

QTEST_APPLESS_MAIN(RoomHistoryTest)
#include "RoomHistoryTest.moc"
