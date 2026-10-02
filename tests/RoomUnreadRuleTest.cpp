// GitHub #18: the tray badge painted a red dot for muted rooms, which the
// rail's unread rule has always skipped. Both now read one predicate
// (matrix/RoomUnreadRule.h); these cases pin that predicate, the tray's walk
// over it, and that AppController::refreshTrayUnread goes through it instead
// of re-deriving unread from the raw fields.

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

} // namespace

class RoomUnreadRuleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A muted room has unreadCount 0 (mute suppresses notifications) but a
    // receipt-derived hasUnreadMessages: exactly the countless red dot.
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

    void aMutedRoomWithACountDoesNotAddToTheTotal()
    {
        const QList<RoomInfo> rooms{ room(QStringLiteral("!muted:x"), 7) };
        const roomunread::Summary s = roomunread::summarize(
            rooms, [](const QString &) { return roomunread::kModeMute; },
            [](const RoomInfo &, bool) {});
        QVERIFY(!s.any);
        QCOMPARE(s.total, 0);
    }

    // Element: a muted room is count 0, level None, before highlights are read.
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
        // Read rooms and muted ones are reported as not unread, so their
        // notifications are withdrawn.
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
