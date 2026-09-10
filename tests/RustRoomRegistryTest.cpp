// The ordered room registry the Rust bridge's room-list protocol feeds, and
// the bound on the background timeline mirror beside it.
//
// Both used to live inside private methods of RustSdkMatrixClient, which no
// test target builds, so neither had ever been exercised by anything. The
// registry's invariant — "only the producer that owns the index space may
// define it" — was violated by a dozen ordinary user actions, and the
// consequence was the "room_list malformed diff rejected" storm plus, worse,
// positional deletes landing on rooms the SDK never named.
//
// The first case below reproduces the defect using the same public API,
// because `applyIndexReset(snapshot)` IS what the old code did with a
// `client.rooms()` snapshot.

#include "matrix/RustRoomRegistry.h"
#include "matrix/RustTimelineMirror.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest/QtTest>

using matrix::rust_rooms::Registry;
using matrix::rust_rooms::applyIndexReset;
using matrix::rust_rooms::applyRoomListDiff;
using matrix::rust_rooms::applySnapshot;
using matrix::rust_rooms::retireAbsentSpaces;

namespace {

QString roomId(int n)
{
    return QStringLiteral("!room%1:example.org").arg(n);
}

QJsonObject roomJson(const QString &id, const QString &name = QString())
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("name"), name.isEmpty() ? id : name);
    obj.insert(QStringLiteral("membership"), QStringLiteral("joined"));
    return obj;
}

QJsonObject spaceJson(const QString &id)
{
    QJsonObject obj = roomJson(id);
    obj.insert(QStringLiteral("is_space"), true);
    return obj;
}

// A Space the user has been INVITED to but has not joined. It reaches
// `m_rooms` from the room payload (which carries Joined | Invited | Knocked)
// and is absent from the space list, which Rust builds from
// `joined_space_rooms()`.
QJsonObject invitedSpaceJson(const QString &id)
{
    QJsonObject obj = spaceJson(id);
    obj.insert(QStringLiteral("membership"), QStringLiteral("invited"));
    return obj;
}

// `count` rooms, ids 0..count-1, in index order.
QJsonArray rooms(int count)
{
    QJsonArray array;
    for (int i = 0; i < count; ++i)
        array.append(roomJson(roomId(i)));
    return array;
}

QJsonObject diff(const QString &type)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), type);
    return obj;
}

QJsonObject setDiff(int index, const QString &id, const QString &name)
{
    QJsonObject obj = diff(QStringLiteral("room_list_set"));
    obj.insert(QStringLiteral("index"), index);
    obj.insert(QStringLiteral("room"), roomJson(id, name));
    return obj;
}

QJsonObject removeDiff(int index, const QString &expectedId)
{
    QJsonObject obj = diff(QStringLiteral("room_list_remove"));
    obj.insert(QStringLiteral("index"), index);
    if (!expectedId.isNull())
        obj.insert(QStringLiteral("expected_id"), expectedId);
    return obj;
}

} // namespace

class RustRoomRegistryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // THE DEFECT, reproduced through the public API.
    //
    // The sliding list starts at 20 rooms and grows in pages of 100, is
    // filtered and is sorted by recency; `client.rooms()` is the whole state
    // store in the store's own order. Rebuilding the index base from the
    // second — which is what Rust's `enqueue_rooms` did by emitting a
    // `room_list_reset`, on mark-read, favourite, invite accept, room create,
    // leave, join and eight more — renumbers every position the SDK is about
    // to address.
    void aSnapshotUsedAsTheIndexBaseMakesTheNextDiffFail()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(20));
        QCOMPARE(order.size(), 20);
        QCOMPARE(order.at(3), roomId(3));

        // The state-store snapshot: 40 rooms, and not in the list's order.
        QJsonArray snapshot;
        for (int i = 39; i >= 0; --i)
            snapshot.append(roomJson(roomId(i)));
        applyIndexReset(registry, snapshot); // what the old code did

        QVERIFY2(order.at(3) != roomId(3),
                 "the fixture did not actually drift the index space");

        // The SDK now says "index 3 is room 3", addressing ITS list. That is
        // the logged "room_list malformed diff rejected" — and the rejection
        // asked Rust for another `client.rooms()` snapshot, which re-created
        // this exact drift, so the next diff was rejected too. Twelve a
        // minute on one account, each one re-emitting the whole room list and
        // its avatar fetches.
        QVERIFY2(!applyRoomListDiff(registry,
                                    setDiff(3, roomId(3), QStringLiteral("new"))),
                 "the drifted registry accepted a Set aimed at another room");

        // And the SDK's positional DELETE, which carries no room of its own.
        // Refused now because the producer names its target; the unchecked
        // form this replaces would have deleted whatever sat at index 1.
        const QString bystander = order.at(1);
        QVERIFY(bystander != roomId(1));
        QVERIFY(!applyRoomListDiff(registry, removeDiff(1, roomId(1))));
        QVERIFY2(map.contains(bystander),
                 "a positional delete removed a room the SDK never named");
    }

    // THE FIX. A snapshot updates the room map and leaves the index space to
    // the producer that owns it, so the very next diff still resolves.
    void aSnapshotLeavesTheIndexSpaceAloneSoTheNextSetStillResolves()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(20));
        const QStringList before = order;

        QJsonArray snapshot;
        for (int i = 39; i >= 0; --i)
            snapshot.append(roomJson(roomId(i)));
        applySnapshot(registry, snapshot);

        QCOMPARE(order, before);
        QCOMPARE(map.size(), 40); // the snapshot's extra rooms are still known

        QVERIFY(applyRoomListDiff(registry, setDiff(3, roomId(3), QStringLiteral("renamed"))));
        QCOMPARE(order.at(3), roomId(3));
        QCOMPARE(map.value(roomId(3)).name, QStringLiteral("renamed"));
        QCOMPARE(order.size(), 20);
        // Nothing was deleted: every id the reset established is still held.
        for (const QString &id : before)
            QVERIFY2(map.contains(id), qPrintable(id));
    }

    // A positional delete that cannot name its target must REFUSE. With a
    // drifted registry the unchecked form deleted whichever room happened to
    // sit at that index, silently and with no way to notice afterwards.
    void aRemoveIsRefusedUnlessItNamesTheRoomTheRegistryHolds()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(5));

        // Names the room we hold: applied.
        QVERIFY(applyRoomListDiff(registry, removeDiff(1, roomId(1))));
        QCOMPARE(order.size(), 4);
        QVERIFY(!map.contains(roomId(1)));

        // Names a different room: refused, and NOTHING is removed.
        const QStringList after = order;
        QVERIFY(!applyRoomListDiff(registry, removeDiff(1, roomId(99))));
        QCOMPARE(order, after);
        QCOMPARE(map.size(), 4);

        // Names nothing at all — the old wire format. Also refused: a delete
        // nobody can confirm is exactly the shape that removed wrong rooms.
        QVERIFY(!applyRoomListDiff(registry, removeDiff(1, QString())));
        QCOMPARE(order, after);
        QCOMPARE(map.size(), 4);
    }

    void popsAreCheckedTheSameWay()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(3));

        QJsonObject popFront = diff(QStringLiteral("room_list_pop_front"));
        QVERIFY2(!applyRoomListDiff(registry, popFront),
                 "an unnamed pop_front was applied");
        popFront.insert(QStringLiteral("expected_id"), roomId(1));
        QVERIFY2(!applyRoomListDiff(registry, popFront),
                 "a pop_front naming the wrong room was applied");
        popFront.insert(QStringLiteral("expected_id"), roomId(0));
        QVERIFY(applyRoomListDiff(registry, popFront));
        QStringList expected;
        expected << roomId(1) << roomId(2);
        QCOMPARE(order, expected);

        QJsonObject popBack = diff(QStringLiteral("room_list_pop_back"));
        popBack.insert(QStringLiteral("expected_id"), roomId(0));
        QVERIFY(!applyRoomListDiff(registry, popBack));
        popBack.insert(QStringLiteral("expected_id"), roomId(2));
        QVERIFY(applyRoomListDiff(registry, popBack));
        expected.removeLast();
        QCOMPARE(order, expected);
    }

    // A snapshot must not delete a room the index space still names: only a
    // diff may remove one of those, and it will.
    void aSnapshotNeverDropsARoomTheIndexSpaceStillNames()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(3));

        QJsonArray thin;
        thin.append(roomJson(roomId(0)));
        applySnapshot(registry, thin);

        QCOMPARE(order.size(), 3);
        for (int i = 0; i < 3; ++i)
            QVERIFY2(map.contains(roomId(i)), qPrintable(roomId(i)));
    }

    // The classic-sync fallback has no diffs at all, so there is no index
    // space and the snapshot IS the room set — including removals, or a room
    // the user left would stay in the list until sign-out.
    void withNoIndexSpaceTheSnapshotDefinesTheRoomSet()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applySnapshot(registry, rooms(3));
        QCOMPARE(map.size(), 3);
        QVERIFY2(order.isEmpty(), "a snapshot wrote the index space");

        QJsonArray afterLeaving;
        afterLeaving.append(roomJson(roomId(0)));
        afterLeaving.append(roomJson(roomId(2)));
        applySnapshot(registry, afterLeaving);
        QCOMPARE(map.size(), 2);
        QVERIFY(!map.contains(roomId(1)));
    }

    // Spaces are not in the SDK's room list at all, so neither a reset of it
    // nor a snapshot that omits them may drop the Space hierarchy.
    void spacesSurviveBothAResetAndASnapshot()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonArray withSpace = rooms(2);
        withSpace.append(spaceJson(QStringLiteral("!space:example.org")));
        applySnapshot(registry, withSpace);
        QVERIFY(map.value(QStringLiteral("!space:example.org")).isSpace);

        applyIndexReset(registry, rooms(2));
        QVERIFY2(map.contains(QStringLiteral("!space:example.org")),
                 "a room-list reset dropped the Space hierarchy");
        QVERIFY2(!order.contains(QStringLiteral("!space:example.org")),
                 "a Space entered the SDK's index space");

        applySnapshot(registry, rooms(2));
        QVERIFY(map.contains(QStringLiteral("!space:example.org")));
    }

    // The duplicate check belongs to the INDEX SPACE, not to the room map.
    // Checking the map refused a perfectly good Insert for any room the map
    // already knew — every Space, and any room a snapshot had learned about
    // before its diff arrived — and each refusal asked for a fresh snapshot,
    // which is the storm.
    void aRoomAlreadyKnownFromASnapshotCanStillEnterTheIndexSpace()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applySnapshot(registry, rooms(1));
        QVERIFY(map.contains(roomId(0)));

        QJsonObject push = diff(QStringLiteral("room_list_push_back"));
        push.insert(QStringLiteral("room"), roomJson(roomId(0)));
        QVERIFY2(applyRoomListDiff(registry, push),
                 "a diff was rejected because a snapshot got there first");
        QStringList expected;
        expected << roomId(0);
        QCOMPARE(order, expected);

        // A genuine duplicate — already IN the index space — is still refused.
        QVERIFY(!applyRoomListDiff(registry, push));
        QCOMPARE(order.size(), 1);
    }

    void outOfRangeDiffsAreRefusedAndChangeNothing()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(2));
        const QStringList before = order;

        QVERIFY(!applyRoomListDiff(registry, setDiff(7, roomId(9), QStringLiteral("x"))));
        QVERIFY(!applyRoomListDiff(registry, removeDiff(9, roomId(9))));
        QJsonObject truncate = diff(QStringLiteral("room_list_truncate"));
        truncate.insert(QStringLiteral("length"), 9);
        QVERIFY(!applyRoomListDiff(registry, truncate));
        QVERIFY(!applyRoomListDiff(registry, diff(QStringLiteral("room_list_nonsense"))));

        QCOMPARE(order, before);
        QCOMPARE(map.size(), 2);
    }

    // A Set that would put one room in two positions must be refused; that
    // check, too, is against the index space.
    void aSetThatWouldDuplicateAnIndexedRoomIsRefused()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(3));

        QVERIFY(!applyRoomListDiff(registry, setDiff(0, roomId(2), QStringLiteral("x"))));
        QCOMPARE(order.size(), 3);
        QCOMPARE(order.at(0), roomId(0));
    }

    // A present-but-empty preview must not clobber one already learned from
    // an open timeline: Rust sends "" whenever the SDK has no latest event
    // yet, and set/insert diffs arrive on every unread/order change.
    void anEmptyPreviewNeverClobbersAKnownOne()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject withPreview = roomJson(roomId(0));
        withPreview.insert(QStringLiteral("last_message_preview"),
                           QStringLiteral("hello"));
        QJsonArray first;
        first.append(withPreview);
        applyIndexReset(registry, first);
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, QStringLiteral("hello"));

        QJsonObject blank = roomJson(roomId(0));
        blank.insert(QStringLiteral("last_message_preview"), QString());
        QJsonArray second;
        second.append(blank);
        applySnapshot(registry, second);
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, QStringLiteral("hello"));
    }

    // ── The background timeline mirror's bound ──────────────────────────
    //
    // Every live event of every room the user has not opened accumulated
    // here, deduplicated by a linear scan over all of it, and only sign-out
    // ever emptied it.
    void theBackgroundMirrorStopsAtItsCapAndKeepsTheNewest()
    {
        QList<TimelineEvent> mirror;
        const int cap = matrix::rust_timeline::kBackgroundMirrorCap;
        for (int i = 0; i < cap * 4; ++i) {
            TimelineEvent event;
            event.eventId = QStringLiteral("$e%1:example.org").arg(i);
            matrix::rust_timeline::appendBounded(mirror, event);
        }
        QCOMPARE(mirror.size(), cap);
        // The NEWEST cap events, in order: a preview and a pre-snapshot
        // render both want the tail, never the head.
        QCOMPARE(mirror.first().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 4 - cap));
        QCOMPARE(mirror.last().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 4 - 1));
    }

    void theMirrorBoundIsSmallEnoughToBoundTheDuplicateScan()
    {
        // The scan that runs per incoming event is O(this). It is a preview
        // source and a pre-snapshot render, not a timeline: a few pagination
        // batches, not thousands of rows.
        QVERIFY(matrix::rust_timeline::kBackgroundMirrorCap > 0);
        QVERIFY2(matrix::rust_timeline::kBackgroundMirrorCap <= 200,
                 "the background mirror bound has grown into a memory and "
                 "per-event-scan cost again");
    }

    // ── An OPENED room's mirror goes back under the bound ────────────────
    //
    // The half that was missing: opening a room replaces the ring above with
    // the SDK snapshot and grows it per diff (600-900 rows after the
    // viewport fill), and nothing reduced it again. Fifty rooms in a sitting
    // kept every event of all fifty, while matrix-sdk's shrink_to_last_chunk
    // had already released Rust's own copy.
    void anOpenedMirrorIsTrimmedToTheBoundKeepingTheNewestRows()
    {
        const int cap = matrix::rust_timeline::kBackgroundMirrorCap;
        QList<TimelineEvent> mirror;
        for (int i = 0; i < cap * 15; ++i) {   // ~900 rows, the measured fill
            TimelineEvent event;
            event.eventId = QStringLiteral("$e%1:example.org").arg(i);
            mirror.append(event);              // NOT appendBounded: an open
        }                                      // writes the snapshot whole
        QCOMPARE(matrix::rust_timeline::trimToBackgroundBound(mirror),
                 qsizetype(cap * 15 - cap));
        QCOMPARE(mirror.size(), cap);
        // The NEWEST rows survive, contiguous and in order — the same end
        // appendBounded() keeps, because a preview and a pre-snapshot render
        // both want the tail.
        QCOMPARE(mirror.first().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 15 - cap));
        QCOMPARE(mirror.last().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 15 - 1));
        for (int i = 0; i < cap; ++i) {
            QCOMPARE(mirror.at(i).eventId,
                     QStringLiteral("$e%1:example.org").arg(cap * 15 - cap + i));
        }
    }

    void aMirrorAtOrUnderTheBoundIsLeftExactlyAsItIs()
    {
        const int cap = matrix::rust_timeline::kBackgroundMirrorCap;
        for (int size : { cap, cap - 1, 1 }) {
            QList<TimelineEvent> mirror;
            for (int i = 0; i < size; ++i) {
                TimelineEvent event;
                event.eventId = QStringLiteral("$e%1:example.org").arg(i);
                mirror.append(event);
            }
            QCOMPARE(matrix::rust_timeline::trimToBackgroundBound(mirror),
                     qsizetype(0));
            QCOMPARE(mirror.size(), size);
            QCOMPARE(mirror.first().eventId,
                     QStringLiteral("$e0:example.org"));
            QCOMPARE(mirror.last().eventId,
                     QStringLiteral("$e%1:example.org").arg(size - 1));
        }
    }

    void trimmingAnEmptyMirrorIsANoOp()
    {
        // Every room the user has never opened has one of these, and the
        // retirement path runs over whatever it finds.
        QList<TimelineEvent> mirror;
        QCOMPARE(matrix::rust_timeline::trimToBackgroundBound(mirror),
                 qsizetype(0));
        QVERIFY(mirror.isEmpty());
    }

    // ── Leaving a Space ─────────────────────────────────────────────────
    //
    // `space_list_reset` is complete, so a Space missing from it is one the
    // user has left. Blanking its children left the ENTRY, which still read
    // isSpace + Joined — the exact pair SpaceManager::rebuild turns into a
    // rail tile — so a left Space stayed on the rail for the session.
    void aSpaceAbsentFromACompleteSpaceListIsErasedNotBlanked()
    {
        const QString spaceId = QStringLiteral("!space:example.org");
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonArray withSpace = rooms(2);
        withSpace.append(spaceJson(spaceId));
        applySnapshot(registry, withSpace);
        map[spaceId].childRoomIds = { roomId(0), roomId(1) };
        QVERIFY(map.value(spaceId).isSpace);
        // isSpace + Joined is the exact pair SpaceManager::rebuild turns
        // into a rail tile, and blanking the children left both standing.
        QVERIFY(map.value(spaceId).membership == RoomInfo::Joined);

        // The user leaves it: the next complete list simply does not name it.
        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{}), 1);

        // GONE. Not "present with no children" — that entry is still a tile.
        QVERIFY2(!map.contains(spaceId),
                 "a Space the user has left survived a complete space list");
        // The rooms it contained are not Spaces and are not its property.
        QVERIFY(map.contains(roomId(0)));
        QVERIFY(map.contains(roomId(1)));
    }

    // AN INVITED SPACE IS ABSENT FROM THE SPACE LIST BY CONSTRUCTION.
    //
    // Raised in review. `present` comes from `joined_space_rooms()`, so an
    // invite can never appear in it — and it is not in the index space
    // either, so the guard below is vacuous for it. Erasing on absence alone
    // therefore created the invite row from the room payload and destroyed it
    // again on the very next space list, which lands after it on every sync:
    // a Space invitation could never be seen, let alone accepted. The room
    // list shows these rows today (passesScopeFilter drops only
    // isSpace && Joined) and sorts them first.
    void anInvitedSpaceSurvivesASpaceListThatCannotMentionIt()
    {
        const QString invited = QStringLiteral("!invited-space:example.org");
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonArray payload = rooms(1);
        payload.append(invitedSpaceJson(invited));
        applySnapshot(registry, payload);
        QVERIFY(map.value(invited).isSpace);
        QVERIFY2(map.value(invited).membership == RoomInfo::Invited,
                 "the fixture is wrong: this case needs an INVITED space");
        QVERIFY2(!order.contains(invited),
                 "the fixture is wrong: an unindexed space is the whole "
                 "point, or the index guard would mask the defect");

        // The space list cannot name it, because the user has not joined it.
        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{}), 0);

        QVERIFY2(map.contains(invited),
                 "a Space invitation was erased by a space list that lists "
                 "only JOINED spaces; the invite can never be accepted");
        QVERIFY(map.value(invited).membership == RoomInfo::Invited);
    }

    void aStillJoinedSpaceIsUntouchedByItsOwnSpaceList()
    {
        const QString spaceId = QStringLiteral("!space:example.org");
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonArray withSpace = rooms(2);
        withSpace.append(spaceJson(spaceId));
        applySnapshot(registry, withSpace);
        map[spaceId].childRoomIds = { roomId(0) };
        map[spaceId].parentSpaceIds = { QStringLiteral("!parent:example.org") };

        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{ spaceId }), 0);
        QVERIFY(map.contains(spaceId));
        // The hierarchy the caller just wrote is not disturbed either: this
        // function is the REMOVAL half and nothing else.
        QCOMPARE(map.value(spaceId).childRoomIds, QStringList{ roomId(0) });
        QCOMPARE(map.value(spaceId).parentSpaceIds,
                 QStringList{ QStringLiteral("!parent:example.org") });
    }

    // THE CASE THAT MATTERS MOST. Nothing may leave `rooms` while `order`
    // still names it: `order` is addressed BY INDEX, so dropping an indexed
    // entry from the map alone leaves a position pointing at nothing — the
    // shape of the wrong-room deletion this project has already shipped
    // once. Spaces are deliberately never appended to `order`, so this is
    // normally unreachable; the guard is what keeps it that way if a
    // producer ever does index one.
    void aSpaceTheIndexSpaceStillNamesIsBlankedAndNeverErased()
    {
        const QString spaceId = QStringLiteral("!space:example.org");
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(2));
        // A producer that DOES put a Space in the index space.
        QJsonObject push = diff(QStringLiteral("room_list_push_back"));
        push.insert(QStringLiteral("room"), spaceJson(spaceId));
        QVERIFY(applyRoomListDiff(registry, push));
        QVERIFY(map.value(spaceId).isSpace);
        QCOMPARE(order.size(), 3);
        QCOMPARE(order.at(2), spaceId);
        map[spaceId].childRoomIds = { roomId(0) };
        map[spaceId].parentSpaceIds = { QStringLiteral("!parent:example.org") };

        const int erased = retireAbsentSpaces(registry, QSet<QString>{});

        // The CONSEQUENCE is asserted before the count, so a regression here
        // reports the harm rather than an arithmetic mismatch.
        QVERIFY2(map.contains(spaceId),
                 "an indexed entry was erased from the room map; the index "
                 "space now names a room that does not exist");
        QCOMPARE(erased, 0);
        QCOMPARE(order.size(), 3);
        QCOMPARE(order.at(2), spaceId);
        // Blanked exactly as the old code blanked every Space — the entry's
        // lifetime belongs to the diffs that own the index space.
        QVERIFY(map.value(spaceId).childRoomIds.isEmpty());
        QVERIFY(map.value(spaceId).parentSpaceIds.isEmpty());

        // The index space is still intact and addressable, which is the
        // property the guard exists to protect.
        QVERIFY2(applyRoomListDiff(registry,
                                   setDiff(1, roomId(1), QStringLiteral("new"))),
                 "the index space stopped resolving after a space list");
        QCOMPARE(map.value(roomId(1)).name, QStringLiteral("new"));
        // And the diffs that DO own it can still retire it.
        QVERIFY(applyRoomListDiff(registry, removeDiff(2, spaceId)));
        QVERIFY(!map.contains(spaceId));
        QCOMPARE(order.size(), 2);
    }

    void retiringSpacesNeverTouchesAnOrdinaryRoom()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(3));
        // A room the snapshot lane knows and the index space does not — the
        // other population that lives in `rooms` alone, and the one a broad
        // "erase what the payload does not name" would have taken with it.
        applySnapshot(registry, QJsonArray{ roomJson(roomId(9)) });
        QVERIFY(map.contains(roomId(9)));

        // An empty joined-Space set says nothing whatever about rooms.
        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{}), 0);
        QCOMPARE(map.size(), 4);
        for (int i = 0; i < 3; ++i)
            QVERIFY(map.contains(roomId(i)));
        QVERIFY(map.contains(roomId(9)));
        QCOMPARE(order.size(), 3);
    }
};

QTEST_MAIN(RustRoomRegistryTest)
#include "RustRoomRegistryTest.moc"
