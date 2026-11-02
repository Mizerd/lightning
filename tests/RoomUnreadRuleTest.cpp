
#include "matrix/RoomUnreadRule.h"

#include <QFile>
#include <QtTest/QtTest>

namespace {

RoomInfo room(const QString &id, int unread, bool receiptUnread = false)
{
    RoomInfo r;
    r.id = id;
    r.membership = RoomInfo::Joined;
    r.unreadCount = unread;
    r.hasUnreadMessages = receiptUnread;
    return r;
}

}

class RoomUnreadRuleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aMentionsOnlyRoomIsClampedOnlyWhereTheCountCannotBeTrusted()
    {
        RoomInfo r = room(QStringLiteral("!enc:x"), 7, true);
        r.highlightCount = 2;
        r.encryptionKnown = true;
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeAllMessages), 7);
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeMentions), 7);
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeMentionsUnverified), 0);
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeMute), 0);
        r.encryptionKnown = false;
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeMentions), 0);
        QVERIFY(roomunread::countsAsUnread(r, roomunread::kModeMentionsUnverified));
        QCOMPARE(roomunread::mentionCount(r, roomunread::kModeMentionsUnverified), 2);

        const roomunread::Summary clamped = roomunread::summarize(
            QList<RoomInfo>{ r },
            [](const QString &) { return roomunread::kModeMentionsUnverified; },
            [](const RoomInfo &, bool) {});
        QVERIFY(clamped.any);
        QCOMPARE(clamped.total, 2);
        r.encryptionKnown = true;
        const roomunread::Summary trusted = roomunread::summarize(
            QList<RoomInfo>{ r },
            [](const QString &) { return roomunread::kModeMentions; },
            [](const RoomInfo &, bool) {});
        QCOMPARE(trusted.total, 7);
    }

    void aMutedRoomIsNeitherADotNorACount()
    {
        const QList<RoomInfo> rooms{ room(QStringLiteral("!muted:x"), 0, true),
                                     room(QStringLiteral("!loud:x"), 3, true) };
        const auto modeOf = [](const QString &id) {
            return id == QLatin1String("!muted:x") ? roomunread::kModeMute
                                                   : roomunread::kModeAllMessages;
        };
        const roomunread::Summary s = roomunread::summarize(
            rooms, modeOf, [](const RoomInfo &, bool) {});
        QVERIFY(s.any);
        QCOMPARE(s.total, 3);

        const roomunread::Summary onlyMuted = roomunread::summarize(
            QList<RoomInfo>{ rooms.first() }, modeOf,
            [](const RoomInfo &, bool) {});
        QVERIFY2(!onlyMuted.any, "a muted room painted the tray dot");
        QCOMPARE(onlyMuted.total, 0);
    }

    void aMutedMarkedUnreadRoomIsListedButNeverCounted()
    {
        RoomInfo r = room(QStringLiteral("!m:x"), 0);
        r.markedUnread = true;
        QVERIFY(roomunread::listsAsUnread(r, roomunread::kModeMute));
        QVERIFY(!roomunread::countsAsUnread(r, roomunread::kModeMute));
        const roomunread::Summary s = roomunread::summarize(
            { r }, [](const QString &) { return roomunread::kModeMute; },
            [](const RoomInfo &, bool) {});
        QVERIFY(!s.any);

        RoomInfo plain = room(QStringLiteral("!p:x"), 3, true);
        QVERIFY(!roomunread::listsAsUnread(plain, roomunread::kModeMute));
        QVERIFY(!roomunread::listsAsUnread(room(QStringLiteral("!r:x"), 0),
                                           roomunread::kModeAllMessages));
        QVERIFY(roomunread::listsAsUnread(plain, roomunread::kModeAllMessages));
    }

    void aMutedRoomWithACountDoesNotAddToTheTotal()
    {
        const QList<RoomInfo> rooms{ room(QStringLiteral("!muted:x"), 7) };
        const roomunread::Summary s = roomunread::summarize(
            rooms, [](const QString &) { return roomunread::kModeMute; },
            [](const RoomInfo &, bool) {});
        QVERIFY(!s.any);
        QCOMPARE(s.total, 0);
    }

    void aMutedRoomWithAMentionIsSilentAndMentionsOnlyModeStillShowsIt()
    {
        RoomInfo r = room(QStringLiteral("!m:x"), 2, true);
        r.highlightCount = 1;
        QVERIFY(!roomunread::countsAsUnread(r, roomunread::kModeMute));
        QCOMPARE(roomunread::mentionCount(r, roomunread::kModeMute), 0);
        const int mentionsOnly = 1;
        QVERIFY(roomunread::countsAsUnread(r, mentionsOnly));
        QCOMPARE(roomunread::mentionCount(r, mentionsOnly), 1);

        const roomunread::Summary muted = roomunread::summarize(
            QList<RoomInfo>{ r },
            [](const QString &) { return roomunread::kModeMute; },
            [](const RoomInfo &, bool) {});
        QVERIFY2(!muted.any, "a muted mention painted the tray dot");
        const roomunread::Summary shown = roomunread::summarize(
            QList<RoomInfo>{ r }, [&](const QString &) { return mentionsOnly; },
            [](const RoomInfo &, bool) {});
        QVERIFY(shown.any);
    }

    void theWalkReportsEveryJoinedRoomAndSkipsInvites()
    {
        RoomInfo invite = room(QStringLiteral("!inv:x"), 1, true);
        invite.membership = RoomInfo::Invited;
        const QList<RoomInfo> rooms{ invite, room(QStringLiteral("!a:x"), 0),
                                     room(QStringLiteral("!m:x"), 0, true) };
        QStringList seen;
        QList<bool> flags;
        roomunread::summarize(
            rooms,
            [](const QString &id) {
                return id == QLatin1String("!m:x") ? roomunread::kModeMute
                                                   : roomunread::kModeAllMessages;
            },
            [&](const RoomInfo &r, bool unread) {
                seen.append(r.id);
                flags.append(unread);
            });
        QCOMPARE(seen, (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!m:x") }));
        QCOMPARE(flags, (QList<bool>{ false, false }));
    }

    void anUnreadRoomAsksForItsModeAndAReadOneDoesNot()
    {
        int asked = 0;
        const QList<RoomInfo> rooms{ room(QStringLiteral("!read:x"), 0),
                                     room(QStringLiteral("!unread:x"), 1) };
        roomunread::summarize(
            rooms,
            [&](const QString &) {
                ++asked;
                return roomunread::kModeAllMessages;
            },
            [](const RoomInfo &, bool) {});
        QCOMPARE(asked, 1);
    }

    void threadUnreadKindIsOffUnlessThreadsAloneHoldTheRoom()
    {
        RoomInfo r = room(QStringLiteral("!t:x"), 4, true);
        QCOMPARE(roomunread::threadUnreadKind(r, roomunread::kModeAllMessages), 0);
        r.unreadThreadsOnly = true;
        QCOMPARE(roomunread::threadUnreadKind(r, roomunread::kModeAllMessages), 1);
        r.mentionInThreads = true;
        QCOMPARE(roomunread::threadUnreadKind(r, roomunread::kModeAllMessages), 2);
        QCOMPARE(roomunread::threadUnreadKind(r, roomunread::kModeMute), 0);
        r.unreadThreadsOnly = false;
        QCOMPARE(roomunread::threadUnreadKind(r, roomunread::kModeAllMessages), 0);
    }

    void theThreadsButtonCarriesTheDotAndBothRowsDrawTheGlyph()
    {
        const auto read = [](const char *path) {
            QFile f(QStringLiteral(SOURCE_DIR "/") + QLatin1String(path));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                               : QString();
        };
        const QString pane = read("qml/TimelinePane.qml");
        const int at = pane.indexOf(QStringLiteral("objectName: \"threadsUnreadDot\""));
        QVERIFY2(at >= 0, "the header's Threads button has no unread dot");
        QVERIFY(pane.contains(QStringLiteral("app.thread.roomThreadUnread")));
        for (const char *row : {"qml/RoomDelegate.qml", "qml/ChannelDelegate.qml"}) {
            const QString src = read(row);
            QVERIFY2(src.contains(QStringLiteral("ThreadUnreadBadge")), row);
            QVERIFY2(src.contains(QStringLiteral("hread")) &&
                         src.contains(QStringLiteral("unread in a thread")), row);
        }
    }

    void aThreadOnlyRoomIsUnreadButShowsNoNumber()
    {
        RoomInfo r = room(QStringLiteral("!t:x"), 5);
        r.unreadThreadsOnly = true;
        QVERIFY(roomunread::countsAsUnread(r, roomunread::kModeAllMessages));
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeAllMessages), 0);
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeMentions), 0);
        r.unreadThreadsOnly = false;
        QCOMPARE(roomunread::visibleUnreadCount(r, roomunread::kModeAllMessages), 5);

        r.unreadThreadsOnly = true;
        r.highlightCount = 2;
        r.encryptionKnown = true;
        const roomunread::Summary s = roomunread::summarize(
            QList<RoomInfo>{ r },
            [](const QString &) { return roomunread::kModeAllMessages; },
            [](const RoomInfo &, bool) {});
        QVERIFY(s.any);
        QCOMPARE(s.total, 2);
    }

    void aSpaceRoomIsNotCountedButStillReported()
    {
        RoomInfo space = room(QStringLiteral("!space:x"), 4, true);
        space.isSpace = true;
        space.markedUnread = true;
        QStringList seen;
        QList<bool> flags;
        const roomunread::Summary s = roomunread::summarize(
            QList<RoomInfo>{ space },
            [](const QString &) { return roomunread::kModeAllMessages; },
            [&](const RoomInfo &r, bool unread) {
                seen.append(r.id);
                flags.append(unread);
            });
        QVERIFY(!s.any);
        QCOMPARE(s.total, 0);
        QCOMPARE(seen, (QStringList{ QStringLiteral("!space:x") }));
        QCOMPARE(flags, (QList<bool>{ false }));
    }

    void aPendingInviteCountsOnceAndIsNotReported()
    {
        RoomInfo invite = room(QStringLiteral("!inv:x"), 0);
        invite.membership = RoomInfo::Invited;
        int reported = 0;
        const roomunread::Summary s = roomunread::summarize(
            QList<RoomInfo>{ invite, room(QStringLiteral("!a:x"), 3, true) },
            [](const QString &) { return roomunread::kModeAllMessages; },
            [&](const RoomInfo &, bool) { ++reported; });
        QVERIFY(s.any);
        QCOMPARE(s.total, 1 + 3);
        QCOMPARE(reported, 1);
    }

    void everyUnreadSurfaceReadsTheSharedRule()
    {
        const auto read = [](const char *path) {
            QFile f(QStringLiteral(SOURCE_DIR "/") + QLatin1String(path));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                               : QString();
        };
        const QString lists = read("src/models/RoomListModel.cpp");
        QVERIFY(!lists.isEmpty());
        const int chip = lists.indexOf(QStringLiteral("case 3:\n"));
        QVERIFY(chip >= 0);
        const QString chipBody = lists.mid(chip, 500);
        QVERIFY2(!chipBody.contains(QStringLiteral("r.hasUnreadMessages")),
                 "the Unreads chip pre-filters on the receipt flag");
        QVERIFY(chipBody.contains(QStringLiteral("roomunread::listsAsUnread(")));
        const QString rule = read("src/matrix/RoomUnreadRule.h");
        const int lists_ = rule.indexOf(QStringLiteral("inline bool listsAsUnread("));
        QVERIFY(lists_ >= 0);
        QVERIFY(rule.mid(lists_, 200).contains(QStringLiteral("countsAsUnread(r, mode) ||")));
        const int recent = lists.indexOf(QStringLiteral("QVariantList RoomListModel::recentRooms"));
        QVERIFY(recent >= 0);
        QVERIFY2(!lists.mid(recent, 1800).contains(
                     QStringLiteral("r.hasUnreadMessages }")),
                 "recentRooms derives hasUnread from the receipt flag alone");
        const int rec = lists.indexOf(QStringLiteral("void RoomListModel::reconcileRooms()"));
        const int end = lists.indexOf(QStringLiteral("\nvoid RoomListModel::resetRooms"), rec);
        QVERIFY(rec >= 0 && end > rec);
        QVERIFY2(lists.mid(rec, end - rec).contains(QStringLiteral("updateUnreadTotals()")),
                 "reconcileRooms never refreshes the account-wide totals");

        const QString spaces = read("src/spaces/SpaceManager.cpp");
        const int lobby = spaces.indexOf(QStringLiteral("QVariantMap lobbyRow("));
        QVERIFY(lobby >= 0);
        QVERIFY2(!spaces.mid(lobby).contains(QStringLiteral("it->unreadCount > 0")),
                 "the lobby row re-derives hasUnread from the raw count");
        const int direct = spaces.indexOf(QStringLiteral("SpaceManager::directChildRoomsDetailed"));
        QVERIFY(direct >= 0);
        QVERIFY2(!spaces.mid(direct, 2200).contains(QStringLiteral("it->highlightCount")),
                 "the rail reveal returns the raw highlight count");
    }

    void rowLabelsAndDisclaimersFollowTheContract()
    {
        const auto read = [](const char *path) {
            QFile f(QStringLiteral(SOURCE_DIR "/") + QLatin1String(path));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                               : QString();
        };
        const QString row = read("qml/RoomDelegate.qml");
        QVERIFY(row.contains(QStringLiteral("%n mention(s)")));
        QVERIFY(row.contains(QStringLiteral("%n unread message(s)")));
        QVERIFY(row.contains(QStringLiteral("marked as unread")));
        QVERIFY(!row.contains(QStringLiteral("%2 mentions")));
        QVERIFY2(row.contains(QStringLiteral("!roomUnreadDot.visible")),
                 "a marked-unread room can draw two dots");
        QVERIFY(!read("qml/ChannelDelegate.qml").contains(QStringLiteral("%2 mentions")));
        QVERIFY(!read("qml/ChannelCategoryHeader.qml").contains(QStringLiteral("%2 mentions")));
        const QString menu = read("qml/RoomActionsMenu.qml");
        QVERIFY2(menu.contains(QStringLiteral("currentMode === 3"))
                     && menu.contains(QStringLiteral("has no override")),
                 "the flyout claims a saved rule for 'follow account default'");
    }

    void theMenuOffersMarkReadForACountedRoomWithoutAReceiptFlag()
    {
        QFile f(QStringLiteral(SOURCE_DIR "/qml/RoomDelegate.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString row = QString::fromUtf8(f.readAll());
        const int at = row.indexOf(QStringLiteral("property bool canMarkRead:"));
        QVERIFY(at >= 0);
        const QString decl = row.mid(at, 200);
        QVERIFY(decl.contains(QStringLiteral("model.unreadCount > 0")));
        QVERIFY(decl.contains(QStringLiteral("model.highlightCount > 0")));
        QCOMPARE(row.count(QStringLiteral("isUnread: root.canMarkRead")), 2);
    }

    void refreshTrayUnreadUsesTheSharedRule()
    {
        QFile f(QStringLiteral(SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        const int at = src.indexOf(QStringLiteral("void AppController::refreshTrayUnread()"));
        QVERIFY(at >= 0);
        const int end = src.indexOf(QStringLiteral("\nvoid AppController::"), at + 10);
        const QString body = src.mid(at, end - at);
        QVERIFY(body.contains(QStringLiteral("roomunread::summarize")));
        QVERIFY2(!body.contains(QStringLiteral("hasUnreadMessages")),
                 "the tray re-derives unread instead of using the rail's rule");
    }
};

QTEST_GUILESS_MAIN(RoomUnreadRuleTest)
#include "RoomUnreadRuleTest.moc"
