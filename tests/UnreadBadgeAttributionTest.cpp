// A rail tile's unread badge must count exactly what that tile's view lists.
//
// Reported 2026-08-31: two important direct messages were missed outright.
// The rail said there was something unread, the user clicked through, and the
// message was in neither place — "when I click on home it shows 1 notification
// on space, when I click on space, 1 notification at home, but nowhere do I
// see the notifications, in channels mode".
//
// Three separate defects produced that one symptom, and each has its own case
// below:
//
//   A. The Direct Messages tile is a PSEUDO row synthesised in RailEntryModel
//      with no unreadTotal key at all, so it read 0 forever. An unread DM had
//      no way to show anywhere in the rail. This is the one that costs
//      messages, because a DM is the thing you cannot afford to miss.
//
//   B. Home's total summed EVERY joined non-space room — DMs included, rooms
//      inside Spaces included — while the Channels Home view lists only
//      non-DM rooms that no Space's view will list. So the badge counted
//      rooms that tile is structurally unable to show.
//
//   C. The "Other rooms" tile hardcoded its total to 0, and it is the one
//      scope that does list unparented rooms.
//
// The invariant these pin is deliberately layout-aware, and that is not a
// hack: Home must stop counting DMs precisely WHEN the DMs have a tile of
// their own to be counted on. Classic has no People tab, so Classic's Home
// legitimately still means "everything".

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/SpaceChannelModel.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const auto kWork    = QStringLiteral("!work:x");
const auto kGeneral = QStringLiteral("!general:x");
const auto kLoose   = QStringLiteral("!loose:x");
const auto kDm      = QStringLiteral("!dm:x");
const auto kAda     = QStringLiteral("@ada:x");

RoomInfo room(const QString &id, const QString &name, int unread = 0,
              int highlight = 0)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
    info.unreadCount = unread;
    info.highlightCount = highlight;
    info.lastActivity = QDateTime::currentDateTimeUtc();
    return info;
}

RoomInfo spaceRoom(const QString &id, const QString &name,
                   const QStringList &children)
{
    RoomInfo info = room(id, name);
    info.isSpace = true;
    info.childRoomIds = children;
    return info;
}

RoomInfo dm(const QString &id, const QString &name, const QString &peer,
            int unread = 0, int highlight = 0)
{
    RoomInfo info = room(id, name, unread, highlight);
    info.isDirect = true;
    info.directUserId = peer;
    info.directUserIds = { peer };
    return info;
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> mirror;

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:x"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
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
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

/// One Space holding one room, one room in no Space at all, and one DM. Every
/// case below moves the unread onto exactly one of those three and asks which
/// tile is allowed to show it.
QList<RoomInfo> workspace(int spaceRoomUnread, int looseUnread, int dmUnread,
                          int dmHighlight = 0)
{
    return {
        spaceRoom(kWork, QStringLiteral("Work"), { kGeneral }),
        room(kGeneral, QStringLiteral("general"), spaceRoomUnread),
        room(kLoose, QStringLiteral("loose"), looseUnread),
        dm(kDm, QStringLiteral("Ada"), kAda, dmUnread, dmHighlight),
    };
}

/// The rail as the user sees it: entryId -> unreadTotal, straight off
/// RailEntryModel, which is what SpacesRail.qml binds its badge to. Reading
/// the presentation model rather than SpaceManager is deliberate — the People
/// tile's total never existed at that layer, so a SpaceManager-only assertion
/// could not have caught defect A.
QHash<QString, int> railUnread(const RailEntryModel &rail)
{
    QHash<QString, int> out;
    for (int row = 0; row < rail.rowCount(); ++row) {
        const QModelIndex index = rail.index(row, 0);
        out.insert(rail.data(index, RailEntryModel::EntryIdRole).toString(),
                   rail.data(index, RailEntryModel::UnreadTotalRole).toInt());
    }
    return out;
}

QHash<QString, int> railHighlight(const RailEntryModel &rail)
{
    QHash<QString, int> out;
    for (int row = 0; row < rail.rowCount(); ++row) {
        const QModelIndex index = rail.index(row, 0);
        out.insert(rail.data(index, RailEntryModel::EntryIdRole).toString(),
                   rail.data(index, RailEntryModel::HighlightTotalRole).toInt());
    }
    return out;
}

/// Every room id the Channels column actually renders for a selection. This is
/// the other half of every assertion here: a badge is only correct if the view
/// behind it can show what it counted.
QStringList channelRoomIds(SpaceChannelModel &channels, const QString &selection)
{
    channels.setScopeSpaceId(selection);
    QStringList out;
    for (int row = 0; row < channels.rowCount(); ++row) {
        const QModelIndex index = channels.index(row, 0);
        const QString id =
            channels.data(index, SpaceChannelModel::RoomIdRole).toString();
        if (id.startsWith(QLatin1Char('!')))
            out.append(id);
    }
    return out;
}

} // namespace

class UnreadBadgeAttributionTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void directMessageUnreadReachesTheDirectMessagesTile();
    void directMessageHighlightReachesTheDirectMessagesTile();
    void homeStopsCountingDirectMessagesOnceTheyHaveTheirOwnTile();
    void homeStopsCountingRoomsThatBelongToASpace();
    void classicHomeStillCountsEverythingBecauseItListsEverything();
    void otherRoomsTileCountsTheRoomsItLists();
    void everyUnreadIsReachableFromSomeTile();

private:
    QTemporaryDir m_dir;
    FakeClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;
    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    RailEntryModel *m_rail = nullptr;
    SpaceChannelModel *m_channels = nullptr;

    /// Wire the real objects together the way AppController does, then load
    /// `rooms`. `channels` selects whether the Direct Messages tile exists,
    /// which is exactly what the Channels layout decides in SpacesRail.qml.
    void load(const QList<RoomInfo> &rooms, bool channels);
};

void UnreadBadgeAttributionTest::init()
{
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_dir.path());
    m_client = new FakeClient(this);
    m_settings = new SettingsManager(this);
    m_spaces = new SpaceManager(this);
    m_layout = new RailLayoutStore(m_settings, this);
    m_rail = new RailEntryModel(this);
    m_channels = new SpaceChannelModel(this);
}

void UnreadBadgeAttributionTest::cleanup()
{
    delete m_channels; m_channels = nullptr;
    delete m_rail;     m_rail = nullptr;
    delete m_layout;   m_layout = nullptr;
    delete m_spaces;   m_spaces = nullptr;
    delete m_settings; m_settings = nullptr;
    delete m_client;   m_client = nullptr;
}

void UnreadBadgeAttributionTest::load(const QList<RoomInfo> &rooms,
                                      bool channels)
{
    m_client->mirror = rooms;
    m_spaces->setClient(m_client);
    m_rail->setSources(m_spaces, m_layout);
    // The Direct Messages tab is Channels-only; this is the same value
    // SpacesRail.qml binds from roomNavigationLayout === 1.
    m_rail->setPeopleEntryVisible(channels);
    m_spaces->setDirectMessagesHaveOwnTile(channels);
    m_channels->setSources(m_client, m_spaces, m_layout);
    m_channels->setSettings(m_settings);
    m_client->announce();
}

// ── A. the defect that cost real messages ────────────────────────────────

void UnreadBadgeAttributionTest::directMessageUnreadReachesTheDirectMessagesTile()
{
    load(workspace(/*space*/ 0, /*loose*/ 0, /*dm*/ 3), /*channels*/ true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    QVERIFY2(unread.contains(SpaceManager::peopleId()),
             "the Direct Messages tile is missing from the rail entirely");
    QCOMPARE(unread.value(SpaceManager::peopleId()), 3);

    // And the tile it points at really does list that DM, so the badge is
    // actionable rather than merely non-zero.
    QVERIFY(channelRoomIds(*m_channels, SpaceManager::peopleId())
                .contains(kDm));
}

void UnreadBadgeAttributionTest::directMessageHighlightReachesTheDirectMessagesTile()
{
    load(workspace(0, 0, /*dm unread*/ 2, /*dm highlight*/ 2),
         /*channels*/ true);

    // A highlight is a mention. It drives the badge's COLOUR, so a tile that
    // reports the count and drops the highlight shows a mention as ordinary
    // traffic — quieter than the thing deserves.
    QCOMPARE(railHighlight(*m_rail).value(SpaceManager::peopleId()), 2);
}

// ── B. Home counted what Home cannot show ────────────────────────────────

void UnreadBadgeAttributionTest::homeStopsCountingDirectMessagesOnceTheyHaveTheirOwnTile()
{
    load(workspace(0, 0, /*dm*/ 3), /*channels*/ true);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 0);
    // The reason it must be 0: Home does not list the DM.
    QVERIFY(!channelRoomIds(*m_channels, SpaceManager::allRoomsId())
                 .contains(kDm));
}

void UnreadBadgeAttributionTest::homeStopsCountingRoomsThatBelongToASpace()
{
    load(workspace(/*space room*/ 4, 0, 0), /*channels*/ true);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 0);
    QCOMPARE(railUnread(*m_rail).value(kWork), 4);
    QVERIFY(!channelRoomIds(*m_channels, SpaceManager::allRoomsId())
                 .contains(kGeneral));
    QVERIFY(channelRoomIds(*m_channels, kWork).contains(kGeneral));
}

void UnreadBadgeAttributionTest::classicHomeStillCountsEverythingBecauseItListsEverything()
{
    // Classic has no Direct Messages tab — it reaches DMs through a filter
    // chip over one list — so Home is genuinely where an unread DM is found,
    // and dropping it from Home's total there would hide it completely. The
    // rule is not "Home never counts DMs"; it is "a tile counts what it
    // lists".
    load(workspace(/*space room*/ 4, /*loose*/ 2, /*dm*/ 3),
         /*channels*/ false);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 9);
    QVERIFY(!railUnread(*m_rail).contains(SpaceManager::peopleId()));
}

// ── C. the tile that always read zero ────────────────────────────────────

void UnreadBadgeAttributionTest::otherRoomsTileCountsTheRoomsItLists()
{
    load(workspace(0, /*loose*/ 5, 0), /*channels*/ true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    QVERIFY2(unread.contains(SpaceManager::orphansId()),
             "expected an Other rooms tile once a Space and a loose room both "
             "exist");
    QCOMPARE(unread.value(SpaceManager::orphansId()), 5);
    QVERIFY(channelRoomIds(*m_channels, SpaceManager::orphansId())
                .contains(kLoose));
}

// ── the whole point, stated once ─────────────────────────────────────────

void UnreadBadgeAttributionTest::everyUnreadIsReachableFromSomeTile()
{
    // Three unreads, one of each kind, live at once. For every one of them
    // there must be a tile that both counts it and lists it. This is the case
    // that would have caught the report: it fails if any unread is countable
    // nowhere, and equally if it is counted somewhere it cannot be found.
    load(workspace(/*space room*/ 1, /*loose*/ 1, /*dm*/ 1),
         /*channels*/ true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    const struct { QString room; QString tile; } expected[] = {
        { kGeneral, kWork },
        { kLoose,   SpaceManager::orphansId() },
        { kDm,      SpaceManager::peopleId() },
    };

    for (const auto &pair : expected) {
        QVERIFY2(unread.value(pair.tile) > 0,
                 qPrintable(QStringLiteral("tile %1 shows no badge for %2")
                                .arg(pair.tile, pair.room)));
        QVERIFY2(channelRoomIds(*m_channels, pair.tile).contains(pair.room),
                 qPrintable(QStringLiteral("tile %1 badges %2 but does not "
                                           "list it")
                                .arg(pair.tile, pair.room)));
    }

    // Nothing may be counted into a tile that cannot show it. Home keeps the
    // loose room — it lists that one — and must drop the other two, so its
    // total is exactly the unparented unread and not the account's.
    QCOMPARE(unread.value(SpaceManager::allRoomsId()), 1);
    const QStringList home =
        channelRoomIds(*m_channels, SpaceManager::allRoomsId());
    QVERIFY(home.contains(kLoose));
    QVERIFY(!home.contains(kDm));
    QVERIFY(!home.contains(kGeneral));
}

QTEST_MAIN(UnreadBadgeAttributionTest)
#include "UnreadBadgeAttributionTest.moc"
