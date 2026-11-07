
#include "matrix/EventPreview.h"
#include "matrix/RoomRecencyRule.h"
#include "matrix/RustRoomRegistry.h"
#include "matrix/RustTimelineMirror.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest/QtTest>

using matrix::rust_rooms::Registry;
using matrix::rust_rooms::applyIndexReset;
using matrix::rust_rooms::applyRoomActivity;
using matrix::rust_rooms::applyRoomListDiff;
using matrix::rust_rooms::applyRoomRefresh;
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

QJsonObject activityJson(const QString &id, qint64 ms)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("last_activity_ms"), static_cast<double>(ms));
    return obj;
}

QJsonObject roomJsonAt(const QString &id, qint64 ms)
{
    QJsonObject obj = roomJson(id);
    obj.insert(QStringLiteral("last_activity_ms"), static_cast<double>(ms));
    return obj;
}

QJsonObject spaceJson(const QString &id)
{
    QJsonObject obj = roomJson(id);
    obj.insert(QStringLiteral("is_space"), true);
    return obj;
}

QJsonObject invitedSpaceJson(const QString &id)
{
    QJsonObject obj = spaceJson(id);
    obj.insert(QStringLiteral("membership"), QStringLiteral("invited"));
    return obj;
}

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

}

class RustRoomRegistryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aRoomNameLosesItsDirectionOverrides()
    {
        const QString rlo = QString(QChar(0x202E));
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), QStringLiteral("!r:example.org"));
        obj.insert(QStringLiteral("name"),
                   QStringLiteral("N") + rlo + QStringLiteral("edirrevo-ltr"));
        obj.insert(QStringLiteral("inviter_display_name"),
                   QString(QChar(0x2067)) + QStringLiteral("Mallory")
                       + QString(QChar(0x2069)));
        RoomInfo room = matrix::rust_rooms::roomInfoFromJson(obj, RoomInfo{});
        QCOMPARE(room.name, QStringLiteral("Nedirrevo-ltr"));
        QCOMPARE(room.inviterDisplayName, QStringLiteral("Mallory"));

        QString all;
        for (char16_t u = 0x202A; u <= 0x202E; ++u)
            all.append(QChar(u));
        for (char16_t u = 0x2066; u <= 0x2069; ++u)
            all.append(QChar(u));
        obj.insert(QStringLiteral("name"), QStringLiteral("a") + all
                                               + QStringLiteral("b"));
        room = matrix::rust_rooms::roomInfoFromJson(obj, RoomInfo{});
        QCOMPARE(room.name, QStringLiteral("ab"));

        const QString hebrew = QString::fromUtf8("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d")
            + QString(QChar(0x200F)) + QStringLiteral(" 2026 ")
            + QString::fromUtf8("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb");
        obj.insert(QStringLiteral("name"), hebrew);
        room = matrix::rust_rooms::roomInfoFromJson(obj, RoomInfo{});
        QCOMPARE(room.name, hebrew);
    }

    void aCallRowRaisesTheRoomButNeverReplacesThePreview()
    {
        TimelineEvent call;
        call.type = TimelineEvent::CallEvent;
        call.sender = QStringLiteral("@other:example.org");
        QVERIFY(roomrecency::raisesRecency(call));
        QVERIFY(!roomrecency::replacesPreview(call));
        call.sender = QStringLiteral("@me:example.org");
        QVERIFY(roomrecency::raisesRecency(call));
        QVERIFY(!roomrecency::replacesPreview(call));

        TimelineEvent divider;
        divider.type = TimelineEvent::DateDivider;
        QVERIFY(!roomrecency::raisesRecency(divider));
        QVERIFY(!roomrecency::replacesPreview(divider));

        TimelineEvent text;
        text.type = TimelineEvent::TextMessage;
        QVERIFY(roomrecency::raisesRecency(text));
        QVERIFY(roomrecency::replacesPreview(text));
    }

    void aSnapshotUsedAsTheIndexBaseMakesTheNextDiffFail()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(20));
        QCOMPARE(order.size(), 20);
        QCOMPARE(order.at(3), roomId(3));

        QJsonArray snapshot;
        for (int i = 39; i >= 0; --i)
            snapshot.append(roomJson(roomId(i)));
        applyIndexReset(registry, snapshot);

        QVERIFY2(order.at(3) != roomId(3),
                 "the fixture did not actually drift the index space");

        QVERIFY2(!applyRoomListDiff(registry,
                                    setDiff(3, roomId(3), QStringLiteral("new"))),
                 "the drifted registry accepted a Set aimed at another room");

        const QString bystander = order.at(1);
        QVERIFY(bystander != roomId(1));
        QVERIFY(!applyRoomListDiff(registry, removeDiff(1, roomId(1))));
        QVERIFY2(map.contains(bystander),
                 "a positional delete removed a room the SDK never named");
    }

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
        QCOMPARE(map.size(), 40);

        QVERIFY(applyRoomListDiff(registry, setDiff(3, roomId(3), QStringLiteral("renamed"))));
        QCOMPARE(order.at(3), roomId(3));
        QCOMPARE(map.value(roomId(3)).name, QStringLiteral("renamed"));
        QCOMPARE(order.size(), 20);
        for (const QString &id : before)
            QVERIFY2(map.contains(id), qPrintable(id));
    }

    void aRemoveIsRefusedUnlessItNamesTheRoomTheRegistryHolds()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applyIndexReset(registry, rooms(5));

        QVERIFY(applyRoomListDiff(registry, removeDiff(1, roomId(1))));
        QCOMPARE(order.size(), 4);
        QVERIFY(!map.contains(roomId(1)));

        const QStringList after = order;
        QVERIFY(!applyRoomListDiff(registry, removeDiff(1, roomId(99))));
        QCOMPARE(order, after);
        QCOMPARE(map.size(), 4);

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

    void aRefreshUpdatesOneKnownRoomAndNothingElse()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        applySnapshot(registry, rooms(3));

        QJsonObject read = roomJson(roomId(1), QStringLiteral("renamed"));
        read.insert(QStringLiteral("unread_count"), 0);
        QVERIFY(applyRoomRefresh(registry, read));
        QCOMPARE(map.value(roomId(1)).name, QStringLiteral("renamed"));
        QCOMPARE(map.size(), 3);
        QCOMPARE(map.value(roomId(0)).name, roomId(0));
        QVERIFY(order.isEmpty());

        QVERIFY(!applyRoomRefresh(registry, roomJson(roomId(9))));
        QVERIFY2(!map.contains(roomId(9)), "a refresh added a room");
        QVERIFY(!applyRoomRefresh(registry, QJsonObject{}));
    }

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

    void theThreadOnlyUnreadFlagsFollowThePayloadAndNeverLatch()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject held = roomJson(roomId(0));
        held.insert(QStringLiteral("unread_count"), 2);
        held.insert(QStringLiteral("unread_threads_only"), true);
        held.insert(QStringLiteral("mention_in_threads"), true);
        QJsonArray first;
        first.append(held);
        applyIndexReset(registry, first);
        QVERIFY(map.value(roomId(0)).unreadThreadsOnly);
        QVERIFY(map.value(roomId(0)).mentionInThreads);

        QJsonArray second;
        second.append(roomJson(roomId(0)));
        applySnapshot(registry, second);
        QVERIFY(!map.value(roomId(0)).unreadThreadsOnly);
        QVERIFY(!map.value(roomId(0)).mentionInThreads);
    }

    void theLowPriorityTagFollowsThePayloadAndNeverLatches()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject tagged = roomJson(roomId(0));
        tagged.insert(QStringLiteral("is_low_priority"), true);
        applyIndexReset(registry, QJsonArray{ tagged });
        QVERIFY(map.value(roomId(0)).lowPriority);

        applySnapshot(registry, QJsonArray{ roomJson(roomId(0)) });
        QVERIFY(!map.value(roomId(0)).lowPriority);
    }

    void theActiveMemberCountReachesTheRoom()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject room = roomJson(roomId(0));
        room.insert(QStringLiteral("member_count"), 2);
        room.insert(QStringLiteral("active_member_count"), 3);
        applyIndexReset(registry, QJsonArray{ room });
        QCOMPARE(map.value(roomId(0)).activeMemberCount, 3);

        QJsonObject older = roomJson(roomId(0));
        older.insert(QStringLiteral("member_count"), 2);
        applySnapshot(registry, QJsonArray{ older });
        QCOMPARE(map.value(roomId(0)).activeMemberCount, 2);

        applySnapshot(registry, QJsonArray{ roomJson(roomId(0)) });
        QCOMPARE(map.value(roomId(0)).activeMemberCount, 2);

        RoomInfo a;
        a.id = roomId(0);
        RoomInfo b = a;
        b.activeMemberCount = 3;
        QVERIFY(a != b);
    }

    void aLowPriorityChangeIsAChangeButACounterStampIsNot()
    {
        RoomInfo a;
        a.id = roomId(0);
        RoomInfo b = a;
        b.lowPriority = true;
        QVERIFY(a != b);
        RoomInfo c = a;
        c.countsWrittenAtMs = 12345;
        QVERIFY(a == c);
    }

    void onlyAPayloadCarryingTheCountersStampsThem()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, QJsonArray{ roomJson(roomId(0)) });
        QCOMPARE(map.value(roomId(0)).countsWrittenAtMs, qint64(0));

        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        QJsonObject counted = roomJson(roomId(0));
        counted.insert(QStringLiteral("unread_count"), 3);
        counted.insert(QStringLiteral("highlight_count"), 1);
        applySnapshot(registry, QJsonArray{ counted });
        const qint64 stamped = map.value(roomId(0)).countsWrittenAtMs;
        QVERIFY2(stamped >= before, "a payload with counters did not stamp them");
        QVERIFY(stamped <= QDateTime::currentMSecsSinceEpoch());

        applySnapshot(registry, QJsonArray{ roomJson(roomId(0), QStringLiteral("Renamed")) });
        QCOMPARE(map.value(roomId(0)).countsWrittenAtMs, stamped);
        QCOMPARE(map.value(roomId(0)).highlightCount, 1);
    }

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

    void anUndecryptableFlagFillsOnlyABlankRowWithThePlaceholder()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject flagged = roomJson(roomId(0));
        flagged.insert(QStringLiteral("last_message_preview"), QString());
        flagged.insert(QStringLiteral("last_message_undecryptable"), true);
        QJsonArray first;
        first.append(flagged);
        applyIndexReset(registry, first);
        QCOMPARE(map.value(roomId(0)).lastMessagePreview,
                 matrix::preview::undecryptablePreview());

        QJsonObject known = roomJson(roomId(1));
        known.insert(QStringLiteral("last_message_preview"), QStringLiteral("hello"));
        known.insert(QStringLiteral("last_message_undecryptable"), true);
        QJsonArray second;
        second.append(known);
        applyIndexReset(registry, second);
        QCOMPARE(map.value(roomId(1)).lastMessagePreview, QStringLiteral("hello"));
    }

    void anOwnJoinFlagFillsOnlyABlankRow()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        QJsonObject joined = roomJson(roomId(0));
        joined.insert(QStringLiteral("last_message_preview"), QString());
        joined.insert(QStringLiteral("last_message_own_join"), true);
        QJsonArray first;
        first.append(joined);
        applyIndexReset(registry, first);
        QCOMPARE(map.value(roomId(0)).lastMessagePreview,
                 matrix::preview::ownJoinPreview());
        QVERIFY(!matrix::preview::ownJoinPreview().isEmpty());

        QJsonObject known = roomJson(roomId(1));
        known.insert(QStringLiteral("last_message_preview"), QStringLiteral("hello"));
        QJsonArray second;
        second.append(known);
        applyIndexReset(registry, second);
        QJsonObject flagged = roomJson(roomId(1));
        flagged.insert(QStringLiteral("last_message_preview"), QString());
        flagged.insert(QStringLiteral("last_message_own_join"), true);
        QJsonArray third;
        third.append(flagged);
        applySnapshot(registry, third);
        QCOMPARE(map.value(roomId(1)).lastMessagePreview, QStringLiteral("hello"));

        QJsonObject plain = roomJson(roomId(2));
        plain.insert(QStringLiteral("last_message_preview"), QString());
        QJsonArray fourth;
        fourth.append(plain);
        applySnapshot(registry, fourth);
        QVERIFY(map.value(roomId(2)).lastMessagePreview.isEmpty());
    }

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
        QCOMPARE(mirror.first().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 4 - cap));
        QCOMPARE(mirror.last().eventId,
                 QStringLiteral("$e%1:example.org").arg(cap * 4 - 1));
    }

    void theMirrorBoundIsSmallEnoughToBoundTheDuplicateScan()
    {
        QVERIFY(matrix::rust_timeline::kBackgroundMirrorCap > 0);
        QVERIFY2(matrix::rust_timeline::kBackgroundMirrorCap <= 200,
                 "the background mirror bound has grown into a memory and "
                 "per-event-scan cost again");
    }

    void anOpenedMirrorIsTrimmedToTheBoundKeepingTheNewestRows()
    {
        const int cap = matrix::rust_timeline::kBackgroundMirrorCap;
        QList<TimelineEvent> mirror;
        for (int i = 0; i < cap * 15; ++i) {
            TimelineEvent event;
            event.eventId = QStringLiteral("$e%1:example.org").arg(i);
            mirror.append(event);
        }
        QCOMPARE(matrix::rust_timeline::trimToBackgroundBound(mirror),
                 qsizetype(cap * 15 - cap));
        QCOMPARE(mirror.size(), cap);
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

    void onlyTheNewestLargeMemberListsStayLoaded()
    {
        using namespace matrix::rust_timeline;
        QStringList recent;
        QHash<QString, qsizetype> counts;
        QSet<QString> open;
        const auto big = [&](const char *id) {
            counts.insert(QString::fromLatin1(id), kLargeMemberList + 5);
            return QString::fromLatin1(id);
        };
        QStringList gone;
        for (const char *id : { "a", "b", "c" })
            gone += memberListVictims(recent, big(id), counts, open);
        QVERIFY(gone.isEmpty());
        counts.insert(QStringLiteral("small"), 4);
        QVERIFY(memberListVictims(recent, QStringLiteral("small"), counts, open)
                    .isEmpty());
        open.insert(QStringLiteral("a"));
        QVERIFY(memberListVictims(recent, big("d"), counts, open).isEmpty());
        QCOMPARE(memberListVictims(recent, big("e"), counts, open),
                 QStringList{ QStringLiteral("b") });
        QVERIFY(recent.contains(QStringLiteral("a")));
        QVERIFY(!recent.contains(QStringLiteral("b")));
        QVERIFY(recent.contains(QStringLiteral("small")));
    }

    void trimmingAnEmptyMirrorIsANoOp()
    {
        QList<TimelineEvent> mirror;
        QCOMPARE(matrix::rust_timeline::trimToBackgroundBound(mirror),
                 qsizetype(0));
        QVERIFY(mirror.isEmpty());
    }

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
        QVERIFY(map.value(spaceId).membership == RoomInfo::Joined);

        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{}), 1);

        QVERIFY2(!map.contains(spaceId),
                 "a Space the user has left survived a complete space list");
        QVERIFY(map.contains(roomId(0)));
        QVERIFY(map.contains(roomId(1)));
    }

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
        QCOMPARE(map.value(spaceId).childRoomIds, QStringList{ roomId(0) });
        QCOMPARE(map.value(spaceId).parentSpaceIds,
                 QStringList{ QStringLiteral("!parent:example.org") });
    }

    void aSpaceTheIndexSpaceStillNamesIsBlankedAndNeverErased()
    {
        const QString spaceId = QStringLiteral("!space:example.org");
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(2));
        QJsonObject push = diff(QStringLiteral("room_list_push_back"));
        push.insert(QStringLiteral("room"), spaceJson(spaceId));
        QVERIFY(applyRoomListDiff(registry, push));
        QVERIFY(map.value(spaceId).isSpace);
        QCOMPARE(order.size(), 3);
        QCOMPARE(order.at(2), spaceId);
        map[spaceId].childRoomIds = { roomId(0) };
        map[spaceId].parentSpaceIds = { QStringLiteral("!parent:example.org") };

        const int erased = retireAbsentSpaces(registry, QSet<QString>{});

        QVERIFY2(map.contains(spaceId),
                 "an indexed entry was erased from the room map; the index "
                 "space now names a room that does not exist");
        QCOMPARE(erased, 0);
        QCOMPARE(order.size(), 3);
        QCOMPARE(order.at(2), spaceId);
        QVERIFY(map.value(spaceId).childRoomIds.isEmpty());
        QVERIFY(map.value(spaceId).parentSpaceIds.isEmpty());

        QVERIFY2(applyRoomListDiff(registry,
                                   setDiff(1, roomId(1), QStringLiteral("new"))),
                 "the index space stopped resolving after a space list");
        QCOMPARE(map.value(roomId(1)).name, QStringLiteral("new"));
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
        applySnapshot(registry, QJsonArray{ roomJson(roomId(9)) });
        QVERIFY(map.contains(roomId(9)));

        QCOMPARE(retireAbsentSpaces(registry, QSet<QString>{}), 0);
        QCOMPARE(map.size(), 4);
        for (int i = 0; i < 3; ++i)
            QVERIFY(map.contains(roomId(i)));
        QVERIFY(map.contains(roomId(9)));
        QCOMPARE(order.size(), 3);
    }

    void aHarvestedStampMovesARoomTheRoomListPayloadLeftStale()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        const qint64 twoDaysAgo  = 1'700'000'000'000LL;
        const qint64 twentyMins  = 1'700'172'800'000LL;

        applyIndexReset(registry, QJsonArray{ roomJsonAt(roomId(0), twoDaysAgo) });
        QCOMPARE(map.value(roomId(0)).lastActivity.toMSecsSinceEpoch(), twoDaysAgo);

        applyRoomListDiff(registry, [&] {
            QJsonObject obj = diff(QStringLiteral("room_list_set"));
            obj.insert(QStringLiteral("index"), 0);
            obj.insert(QStringLiteral("room"), roomJsonAt(roomId(0), twoDaysAgo));
            return obj;
        }());
        QCOMPARE(map.value(roomId(0)).lastActivity.toMSecsSinceEpoch(), twoDaysAgo);

        const QStringList moved =
            applyRoomActivity(registry, QJsonArray{ activityJson(roomId(0), twentyMins) });
        QCOMPARE(moved, QStringList{ roomId(0) });
        QCOMPARE(map.value(roomId(0)).lastActivity.toMSecsSinceEpoch(), twentyMins);
    }

    void aStampThatDoesNotMoveTheRoomReportsNothing()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        const qint64 now = 1'700'172'800'000LL;
        applyIndexReset(registry, QJsonArray{ roomJsonAt(roomId(0), now) });

        QVERIFY(applyRoomActivity(registry, QJsonArray{ activityJson(roomId(0), now) }).isEmpty());
        QVERIFY(applyRoomActivity(
                    registry, QJsonArray{ activityJson(roomId(0), now - 3'600'000) }).isEmpty());
        QVERIFY(applyRoomActivity(registry, QJsonArray{ activityJson(roomId(0), 0) }).isEmpty());
        QCOMPARE(map.value(roomId(0)).lastActivity.toMSecsSinceEpoch(), now);
    }

    void aStampForAnUnknownRoomCreatesNothing()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, rooms(2));
        QVERIFY(applyRoomActivity(
                    registry,
                    QJsonArray{ activityJson(roomId(7), 1'700'172'800'000LL) }).isEmpty());
        QVERIFY(!map.contains(roomId(7)));
        QCOMPARE(map.size(), 2);
        QCOMPARE(order.size(), 2);
    }

    void aStampChangesTheSortKeyAndNothingElse()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};

        applyIndexReset(registry, QJsonArray{
            roomJson(roomId(0), QStringLiteral("General")),
            roomJson(roomId(1), QStringLiteral("Random")),
        });
        const RoomInfo before = map.value(roomId(1));

        applyRoomActivity(registry, QJsonArray{ activityJson(roomId(1), 1'700'172'800'000LL) });

        const RoomInfo after = map.value(roomId(1));
        QCOMPARE(after.name, before.name);
        QCOMPARE(after.membership, before.membership);
        QCOMPARE(after.lastMessagePreview, before.lastMessagePreview);
        QCOMPARE(order, (QStringList{ roomId(0), roomId(1) }));
    }

    void aNewerUndecryptableEventReplacesAStalePreview()
    {
        QHash<QString, RoomInfo> map;
        QStringList order;
        Registry registry{map, order};
        const qint64 older = 1'700'000'000'000LL;
        const qint64 newer = 1'700'000'060'000LL;

        QJsonObject seeded = roomJsonAt(roomId(0), older);
        seeded.insert(QStringLiteral("last_message_preview"), QStringLiteral("hello from bk035"));
        applyIndexReset(registry, QJsonArray{ seeded });

        QJsonObject flagged = activityJson(roomId(0), newer);
        flagged.insert(QStringLiteral("last_message_undecryptable"), true);
        QCOMPARE(applyRoomActivity(registry, QJsonArray{ flagged }), QStringList{ roomId(0) });
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, matrix::preview::undecryptablePreview());

        QJsonObject payload = roomJsonAt(roomId(0), newer);
        payload.insert(QStringLiteral("last_message_preview"), QString());
        payload.insert(QStringLiteral("last_message_undecryptable"), true);
        applySnapshot(registry, QJsonArray{ payload });
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, matrix::preview::undecryptablePreview());

        QJsonObject decrypted = roomJsonAt(roomId(0), newer);
        decrypted.insert(QStringLiteral("last_message_preview"), QStringLiteral("the new one"));
        applySnapshot(registry, QJsonArray{ decrypted });
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, QStringLiteral("the new one"));

        QJsonObject stale = roomJsonAt(roomId(0), older);
        stale.insert(QStringLiteral("last_message_preview"), QString());
        stale.insert(QStringLiteral("last_message_undecryptable"), true);
        applySnapshot(registry, QJsonArray{ stale });
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, QStringLiteral("the new one"));
        QVERIFY(applyRoomActivity(registry, QJsonArray{ flagged }).isEmpty());
        QCOMPARE(map.value(roomId(0)).lastMessagePreview, QStringLiteral("the new one"));

        QJsonObject seeded1 = roomJsonAt(roomId(1), older);
        seeded1.insert(QStringLiteral("last_message_preview"), QStringLiteral("old text"));
        applySnapshot(registry, QJsonArray{ seeded1 });
        QJsonObject payload1 = roomJsonAt(roomId(1), newer);
        payload1.insert(QStringLiteral("last_message_preview"), QString());
        payload1.insert(QStringLiteral("last_message_undecryptable"), true);
        applySnapshot(registry, QJsonArray{ payload1 });
        QCOMPARE(map.value(roomId(1)).lastMessagePreview, matrix::preview::undecryptablePreview());
        QCOMPARE(map.value(roomId(1)).lastActivity.toMSecsSinceEpoch(), newer);
    }
};

QTEST_MAIN(RustRoomRegistryTest)
#include "RustRoomRegistryTest.moc"
