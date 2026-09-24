// A rail tile's unread badge must count exactly what that tile's view lists.
//
//   A. The Direct Messages tile (a pseudo row synthesised in RailEntryModel)
//      must carry an unreadTotal.
//   B. Home counts only what the Channels Home view lists.
//   C. The "Other rooms" tile counts the unparented rooms it lists.
//   D. Activity (hasUnread, mentionCount): an unread room lights its tiles
//      whether or not it notifies, a muted one does not, mentions are the
//      count, and a folder counts a room once.
//
// The rule is layout-aware: Classic has no People tab, so Classic's Home
// still counts everything.

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
/// RailEntryModel, which SpacesRail.qml binds its badge to (the People tile's
/// total does not exist at the SpaceManager layer).
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

/// One role for every row, keyed by entryId.
QHash<QString, QVariant> railRole(const RailEntryModel &rail, int role)
{
    QHash<QString, QVariant> out;
    for (int row = 0; row < rail.rowCount(); ++row) {
        const QModelIndex index = rail.index(row, 0);
        out.insert(rail.data(index, RailEntryModel::EntryIdRole).toString(),
                   rail.data(index, role));
    }
    return out;
}

bool railHasUnread(const RailEntryModel &rail, const QString &entryId)
{
    return railRole(rail, RailEntryModel::HasUnreadRole)
        .value(entryId).toBool();
}

int railMentions(const RailEntryModel &rail, const QString &entryId)
{
    return railRole(rail, RailEntryModel::MentionCountRole)
        .value(entryId).toInt();
}

/// The entryId of the only folder row, or "".
QString onlyFolderEntry(const RailEntryModel &rail)
{
    QString found;
    for (int row = 0; row < rail.rowCount(); ++row) {
        const QModelIndex index = rail.index(row, 0);
        if (rail.data(index, RailEntryModel::KindRole).toString()
            != QLatin1String("folder")) {
            continue;
        }
        if (!found.isEmpty())
            return {};
        found = rail.data(index, RailEntryModel::EntryIdRole).toString();
    }
    return found;
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
    void homeCountsTheDirectMessagesItListsAgain();
    void homeStopsCountingRoomsThatBelongToASpace();
    void classicHomeStillCountsEverythingBecauseItListsEverything();
    void otherRoomsTileCountsTheRoomsItLists();
    void everyUnreadIsReachableFromSomeTile();

    void aSpaceLightsForAnUnreadRoomThatDoesNotNotify();
    void aMarkedUnreadRoomLightsItsSpace();
    void unreadInANestedSubspaceLightsEveryAncestorRow();
    void aMutedRoomDoesNotLightItsSpaceButItsMentionsCount();
    void unmutingARoomRelightsItsSpace();
    void mentionsAreTheCountAndPlainUnreadIsOnlyADot();
    void aRoomInTwoSpacesIsCountedOnceByTheirFolder();
    void directMessagesCountLikeMentionsUnlessMuted();

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
    m_rail->setRoomSources(m_client, m_settings);
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

void UnreadBadgeAttributionTest::homeCountsTheDirectMessagesItListsAgain()
{
    load(workspace(0, 0, /*dm*/ 3), /*channels*/ true);

    // Home lists the joined DMs (a Direct Messages group after Rooms), so its
    // badge counts them; a room in two views is counted by both tiles.
    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 3);
    QVERIFY(channelRoomIds(*m_channels, SpaceManager::allRoomsId())
                .contains(kDm));
    QCOMPARE(railUnread(*m_rail).value(SpaceManager::peopleId()), 3);
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
    // Classic reaches DMs through a filter chip over one list, so Home is
    // where an unread DM is found and counts it.
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
    // One unread of each kind: for every one there must be a tile that both
    // counts it and lists it.
    load(workspace(/*space room*/ 1, /*loose*/ 1, /*dm*/ 1),
         /*channels*/ true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    const struct { QString room; QString tile; } expected[] = {
        { kGeneral, kWork },
        { kLoose,   SpaceManager::orphansId() },
        { kDm,      SpaceManager::peopleId() },
        { kDm,      SpaceManager::allRoomsId() }, // listed at Home too
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

    // Nothing is counted into a tile that cannot show it: Home keeps the loose
    // room and the DM and drops the Space's room.
    QCOMPARE(unread.value(SpaceManager::allRoomsId()), 2);
    const QStringList home =
        channelRoomIds(*m_channels, SpaceManager::allRoomsId());
    QVERIFY(home.contains(kLoose));
    QVERIFY(home.contains(kDm));
    QVERIFY(!home.contains(kGeneral));
}

// ── D. activity: a dot for unread, a count for mentions ──────────────────

void UnreadBadgeAttributionTest::aSpaceLightsForAnUnreadRoomThatDoesNotNotify()
{
    // notification_count is 0 where push rules do not notify, so the count
    // badge never appeared for this room.
    QList<RoomInfo> rooms = workspace(0, 0, 0);
    rooms[1].hasUnreadMessages = true;
    load(rooms, /*channels*/ true);

    QCOMPARE(railUnread(*m_rail).value(kWork), 0);
    QVERIFY(railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 0);
    QVERIFY(channelRoomIds(*m_channels, kWork).contains(kGeneral));
    // Home in Channels does not list a Space's room, so it stays dark.
    QVERIFY(!railHasUnread(*m_rail, SpaceManager::allRoomsId()));
}

void UnreadBadgeAttributionTest::aMarkedUnreadRoomLightsItsSpace()
{
    QList<RoomInfo> rooms = workspace(0, 0, 0);
    rooms[1].markedUnread = true;
    load(rooms, /*channels*/ true);

    QVERIFY(railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 0);
}

void UnreadBadgeAttributionTest::unreadInANestedSubspaceLightsEveryAncestorRow()
{
    const QString org = QStringLiteral("!org:x");
    const QString team = QStringLiteral("!team:x");
    const QString quiet = QStringLiteral("!quiet:x");
    const QString deep = QStringLiteral("!deep:x");
    const QString calm = QStringLiteral("!calm:x");
    RoomInfo deepRoom = room(deep, QStringLiteral("deep"), 0, 1);
    deepRoom.hasUnreadMessages = true;
    const QList<RoomInfo> rooms = {
        spaceRoom(org, QStringLiteral("Org"), { team, quiet }),
        spaceRoom(team, QStringLiteral("Team"), { deep }),
        spaceRoom(quiet, QStringLiteral("Quiet"), { calm }),
        deepRoom,
        room(calm, QStringLiteral("calm")),
    };
    m_layout->setSpaceExpanded(org, true);
    load(rooms, /*channels*/ true);

    // The subspace rows exist, so each row answers for itself.
    QVERIFY(m_rail->rowForEntry(team) >= 0);
    QVERIFY(m_rail->rowForEntry(quiet) >= 0);
    QVERIFY(railHasUnread(*m_rail, org));
    QVERIFY(railHasUnread(*m_rail, team));
    QVERIFY(!railHasUnread(*m_rail, quiet));
    QCOMPARE(railMentions(*m_rail, org), 1);
    QCOMPARE(railMentions(*m_rail, team), 1);
    QCOMPARE(railMentions(*m_rail, quiet), 0);
    // The root's view reaches the room through its subspace section.
    QVERIFY(m_spaces->roomsInSpace(org).contains(deep));
    m_layout->setSpaceExpanded(org, false);
}

void UnreadBadgeAttributionTest::aMutedRoomDoesNotLightItsSpaceButItsMentionsCount()
{
    const QString muted = QStringLiteral("!muted-a:x");
    const QString noisy = QStringLiteral("!noisy-a:x");
    RoomInfo mutedRoom = room(muted, QStringLiteral("muted"), 4, 0);
    mutedRoom.hasUnreadMessages = true;
    const QList<RoomInfo> onlyMuted = {
        spaceRoom(kWork, QStringLiteral("Work"), { muted }),
        mutedRoom,
    };
    m_settings->setRoomNotificationMode(muted, 2);
    load(onlyMuted, /*channels*/ true);
    QVERIFY(!railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 0);

    // A mention is addressed to the user and survives the mute, as it does
    // in the room list.
    RoomInfo mentioned = room(noisy, QStringLiteral("noisy"), 1, 1);
    mentioned.hasUnreadMessages = true;
    m_settings->setRoomNotificationMode(noisy, 2);
    m_client->mirror = {
        spaceRoom(kWork, QStringLiteral("Work"), { muted, noisy }),
        mutedRoom,
        mentioned,
    };
    m_client->announce();
    QVERIFY(!railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 1);

    m_settings->setRoomNotificationMode(muted, 0);
    m_settings->setRoomNotificationMode(noisy, 0);
}

void UnreadBadgeAttributionTest::unmutingARoomRelightsItsSpace()
{
    // A mode change is not a room change: nothing from the client announces
    // it, so the rail must listen to the setting itself.
    const QString muted = QStringLiteral("!muted-b:x");
    RoomInfo mutedRoom = room(muted, QStringLiteral("muted"), 0, 0);
    mutedRoom.hasUnreadMessages = true;
    m_settings->setRoomNotificationMode(muted, 2);
    load({ spaceRoom(kWork, QStringLiteral("Work"), { muted }), mutedRoom },
         /*channels*/ true);
    QVERIFY(!railHasUnread(*m_rail, kWork));

    // 3 = follow the account default, what "Unmute space" writes.
    m_settings->setRoomNotificationMode(muted, 3);
    QTRY_VERIFY(railHasUnread(*m_rail, kWork));
    m_settings->setRoomNotificationMode(muted, 0);
}

void UnreadBadgeAttributionTest::mentionsAreTheCountAndPlainUnreadIsOnlyADot()
{
    const QString chatter = QStringLiteral("!chatter:x");
    RoomInfo mentioned = room(kGeneral, QStringLiteral("general"), 5, 2);
    mentioned.hasUnreadMessages = true;
    RoomInfo busy = room(chatter, QStringLiteral("chatter"), 3, 0);
    busy.hasUnreadMessages = true;
    load({ spaceRoom(kWork, QStringLiteral("Work"), { kGeneral, chatter }),
           mentioned, busy },
         /*channels*/ true);

    // Two mentions, not the eight notifications around them.
    QCOMPARE(railMentions(*m_rail, kWork), 2);
    QVERIFY(railHasUnread(*m_rail, kWork));

    // Without the mention, the Space is a dot and no number.
    mentioned.highlightCount = 0;
    m_client->mirror = { spaceRoom(kWork, QStringLiteral("Work"),
                                   { kGeneral, chatter }),
                         mentioned, busy };
    m_client->announce();
    QCOMPARE(railMentions(*m_rail, kWork), 0);
    QVERIFY(railHasUnread(*m_rail, kWork));
}

void UnreadBadgeAttributionTest::aRoomInTwoSpacesIsCountedOnceByTheirFolder()
{
    const QString alpha = QStringLiteral("!alpha:x");
    const QString beta = QStringLiteral("!beta:x");
    const QString shared = QStringLiteral("!shared:x");
    RoomInfo sharedRoom = room(shared, QStringLiteral("shared"), 1, 1);
    sharedRoom.hasUnreadMessages = true;
    load({ spaceRoom(alpha, QStringLiteral("Alpha"), { shared }),
           spaceRoom(beta, QStringLiteral("Beta"), { shared }),
           sharedRoom },
         /*channels*/ true);
    QVERIFY(!m_layout->createFolderWithSpaces({ alpha, beta }, 0,
                                              QStringLiteral("Both"))
                 .isEmpty());

    const QString folder = onlyFolderEntry(*m_rail);
    QVERIFY(!folder.isEmpty());
    // Each Space lists the room, so each counts it.
    QCOMPARE(railMentions(*m_rail, alpha), 1);
    QCOMPARE(railMentions(*m_rail, beta), 1);
    // The folder is one place holding one mention.
    QCOMPARE(railMentions(*m_rail, folder), 1);
    QVERIFY(railHasUnread(*m_rail, folder));
    m_layout->deleteFolder(folder);
}

void UnreadBadgeAttributionTest::directMessagesCountLikeMentionsUnlessMuted()
{
    const QString quietDm = QStringLiteral("!dm-quiet:x");
    RoomInfo quiet = dm(quietDm, QStringLiteral("Bo"),
                        QStringLiteral("@bo:x"), 4, 0);
    quiet.hasUnreadMessages = true;
    QList<RoomInfo> rooms = workspace(0, 0, /*dm*/ 3);
    rooms[3].hasUnreadMessages = true;
    rooms.append(quiet);
    m_settings->setRoomNotificationMode(quietDm, 2);
    load(rooms, /*channels*/ true);

    // Every notifying message in a DM is addressed to the user; the muted
    // DM adds nothing.
    QCOMPARE(railMentions(*m_rail, SpaceManager::peopleId()), 3);
    QVERIFY(railHasUnread(*m_rail, SpaceManager::peopleId()));
    // Channels' Home lists the DMs too.
    QCOMPARE(railMentions(*m_rail, SpaceManager::allRoomsId()), 3);
    QCOMPARE(railMentions(*m_rail, kWork), 0);
    QVERIFY(!railHasUnread(*m_rail, kWork));
    m_settings->setRoomNotificationMode(quietDm, 0);
}

QTEST_MAIN(UnreadBadgeAttributionTest)
#include "UnreadBadgeAttributionTest.moc"
