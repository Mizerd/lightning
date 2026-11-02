
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

}

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
    void aMutedRoomIsSilentMentionsIncluded();
    void unmutingARoomRelightsItsSpace();
    void mentionsAreTheCountAndPlainUnreadIsOnlyADot();
    void aRoomInTwoSpacesIsCountedOnceByTheirFolder();
    void directMessagesCountLikeMentionsUnlessMuted();
    void aMentionsOnlyDirectMessageCountsOnlyItsRealMentions();
    void aThreadOnlyDirectMessageShowsNoNumberAnywhereOnTheRail();
    void theRailRevealFollowsMuteAndMarkedUnread();

private:
    QTemporaryDir m_dir;
    FakeClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;
    SpaceManager *m_spaces = nullptr;
    RailLayoutStore *m_layout = nullptr;
    RailEntryModel *m_rail = nullptr;
    SpaceChannelModel *m_channels = nullptr;

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
    m_rail->setPeopleEntryVisible(channels);
    m_spaces->setDirectMessagesHaveOwnTile(channels);
    m_channels->setSources(m_client, m_spaces, m_layout);
    m_channels->setSettings(m_settings);
    m_client->announce();
}

void UnreadBadgeAttributionTest::directMessageUnreadReachesTheDirectMessagesTile()
{
    load(workspace( 0, 0, 3), true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    QVERIFY2(unread.contains(SpaceManager::peopleId()),
             "the Direct Messages tile is missing from the rail entirely");
    QCOMPARE(unread.value(SpaceManager::peopleId()), 3);

    QVERIFY(channelRoomIds(*m_channels, SpaceManager::peopleId())
                .contains(kDm));
}

void UnreadBadgeAttributionTest::directMessageHighlightReachesTheDirectMessagesTile()
{
    load(workspace(0, 0, 2, 2),
 true);

    QCOMPARE(railHighlight(*m_rail).value(SpaceManager::peopleId()), 2);
}

void UnreadBadgeAttributionTest::homeCountsTheDirectMessagesItListsAgain()
{
    load(workspace(0, 0, 3), true);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 3);
    QVERIFY(channelRoomIds(*m_channels, SpaceManager::allRoomsId())
                .contains(kDm));
    QCOMPARE(railUnread(*m_rail).value(SpaceManager::peopleId()), 3);
}

void UnreadBadgeAttributionTest::homeStopsCountingRoomsThatBelongToASpace()
{
    load(workspace( 4, 0, 0), true);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 0);
    QCOMPARE(railUnread(*m_rail).value(kWork), 4);
    QVERIFY(!channelRoomIds(*m_channels, SpaceManager::allRoomsId())
                 .contains(kGeneral));
    QVERIFY(channelRoomIds(*m_channels, kWork).contains(kGeneral));
}

void UnreadBadgeAttributionTest::classicHomeStillCountsEverythingBecauseItListsEverything()
{
    load(workspace( 4, 2, 3),
 false);

    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 9);
    QVERIFY(!railUnread(*m_rail).contains(SpaceManager::peopleId()));
}

void UnreadBadgeAttributionTest::otherRoomsTileCountsTheRoomsItLists()
{
    load(workspace(0, 5, 0), true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    QVERIFY2(unread.contains(SpaceManager::orphansId()),
             "expected an Other rooms tile once a Space and a loose room both "
             "exist");
    QCOMPARE(unread.value(SpaceManager::orphansId()), 5);
    QVERIFY(channelRoomIds(*m_channels, SpaceManager::orphansId())
                .contains(kLoose));
}

void UnreadBadgeAttributionTest::everyUnreadIsReachableFromSomeTile()
{
    load(workspace( 1, 1, 1),
 true);

    const QHash<QString, int> unread = railUnread(*m_rail);
    const struct { QString room; QString tile; } expected[] = {
        { kGeneral, kWork },
        { kLoose,   SpaceManager::orphansId() },
        { kDm,      SpaceManager::peopleId() },
        { kDm,      SpaceManager::allRoomsId() },
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

    QCOMPARE(unread.value(SpaceManager::allRoomsId()), 2);
    const QStringList home =
        channelRoomIds(*m_channels, SpaceManager::allRoomsId());
    QVERIFY(home.contains(kLoose));
    QVERIFY(home.contains(kDm));
    QVERIFY(!home.contains(kGeneral));
}

void UnreadBadgeAttributionTest::aSpaceLightsForAnUnreadRoomThatDoesNotNotify()
{
    QList<RoomInfo> rooms = workspace(0, 0, 0);
    rooms[1].hasUnreadMessages = true;
    load(rooms, true);

    QCOMPARE(railUnread(*m_rail).value(kWork), 0);
    QVERIFY(railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 0);
    QVERIFY(channelRoomIds(*m_channels, kWork).contains(kGeneral));
    QVERIFY(!railHasUnread(*m_rail, SpaceManager::allRoomsId()));
}

void UnreadBadgeAttributionTest::aMarkedUnreadRoomLightsItsSpace()
{
    QList<RoomInfo> rooms = workspace(0, 0, 0);
    rooms[1].markedUnread = true;
    load(rooms, true);

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
    load(rooms, true);

    QVERIFY(m_rail->rowForEntry(team) >= 0);
    QVERIFY(m_rail->rowForEntry(quiet) >= 0);
    QVERIFY(railHasUnread(*m_rail, org));
    QVERIFY(railHasUnread(*m_rail, team));
    QVERIFY(!railHasUnread(*m_rail, quiet));
    QCOMPARE(railMentions(*m_rail, org), 1);
    QCOMPARE(railMentions(*m_rail, team), 1);
    QCOMPARE(railMentions(*m_rail, quiet), 0);
    QVERIFY(m_spaces->roomsInSpace(org).contains(deep));
    m_layout->setSpaceExpanded(org, false);
}

void UnreadBadgeAttributionTest::aMutedRoomIsSilentMentionsIncluded()
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
    load(onlyMuted, true);
    QVERIFY(!railHasUnread(*m_rail, kWork));
    QCOMPARE(railMentions(*m_rail, kWork), 0);

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
    QCOMPARE(railMentions(*m_rail, kWork), 0);

    m_settings->setRoomNotificationMode(noisy, 1);
    m_client->announce();
    QCOMPARE(railMentions(*m_rail, kWork), 1);
    m_settings->setRoomNotificationMode(noisy, 2);

    m_settings->setRoomNotificationMode(muted, 0);
    m_settings->setRoomNotificationMode(noisy, 0);
}

void UnreadBadgeAttributionTest::unmutingARoomRelightsItsSpace()
{
    const QString muted = QStringLiteral("!muted-b:x");
    RoomInfo mutedRoom = room(muted, QStringLiteral("muted"), 0, 0);
    mutedRoom.hasUnreadMessages = true;
    m_settings->setRoomNotificationMode(muted, 2);
    load({ spaceRoom(kWork, QStringLiteral("Work"), { muted }), mutedRoom },
 true);
    QVERIFY(!railHasUnread(*m_rail, kWork));

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
 true);

    QCOMPARE(railMentions(*m_rail, kWork), 2);
    QVERIFY(railHasUnread(*m_rail, kWork));

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
 true);
    QVERIFY(!m_layout->createFolderWithSpaces({ alpha, beta }, 0,
                                              QStringLiteral("Both"))
                 .isEmpty());

    const QString folder = onlyFolderEntry(*m_rail);
    QVERIFY(!folder.isEmpty());
    QCOMPARE(railMentions(*m_rail, alpha), 1);
    QCOMPARE(railMentions(*m_rail, beta), 1);
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
    QList<RoomInfo> rooms = workspace(0, 0, 3);
    rooms[3].hasUnreadMessages = true;
    rooms.append(quiet);
    m_settings->setRoomNotificationMode(quietDm, 2);
    load(rooms, true);

    QCOMPARE(railMentions(*m_rail, SpaceManager::peopleId()), 3);
    QVERIFY(railHasUnread(*m_rail, SpaceManager::peopleId()));
    QCOMPARE(railMentions(*m_rail, SpaceManager::allRoomsId()), 3);
    QCOMPARE(railMentions(*m_rail, kWork), 0);
    QVERIFY(!railHasUnread(*m_rail, kWork));
    m_settings->setRoomNotificationMode(quietDm, 0);
}

void UnreadBadgeAttributionTest::aMentionsOnlyDirectMessageCountsOnlyItsRealMentions()
{
    const QString quietDm = QStringLiteral("!dm-mentions-only:x");
    RoomInfo mentionsOnly = dm(quietDm, QStringLiteral("Cy"),
                              QStringLiteral("@cy:x"), 5,
 1);
    mentionsOnly.hasUnreadMessages = true;
    QList<RoomInfo> rooms = workspace(0, 0, 3);
    rooms[3].hasUnreadMessages = true;
    rooms.append(mentionsOnly);
    m_settings->setRoomNotificationMode(quietDm, 1);
    load(rooms, true);

    QCOMPARE(railMentions(*m_rail, SpaceManager::peopleId()), 3 + 1);
    QVERIFY(railHasUnread(*m_rail, SpaceManager::peopleId()));
    QCOMPARE(railMentions(*m_rail, SpaceManager::allRoomsId()), 3 + 1);
    m_settings->setRoomNotificationMode(quietDm, 0);
}

void UnreadBadgeAttributionTest::aThreadOnlyDirectMessageShowsNoNumberAnywhereOnTheRail()
{
    QList<RoomInfo> rooms = workspace(0, 0, 5);
    rooms[3].unreadThreadsOnly = true;
    load(rooms, true);

    QCOMPARE(railMentions(*m_rail, SpaceManager::peopleId()), 0);
    QCOMPARE(railUnread(*m_rail).value(SpaceManager::peopleId()), 0);
    QCOMPARE(railUnread(*m_rail).value(SpaceManager::allRoomsId()), 0);
    QVERIFY(railHasUnread(*m_rail, SpaceManager::peopleId()));
}

void UnreadBadgeAttributionTest::theRailRevealFollowsMuteAndMarkedUnread()
{
    const QString muted = QStringLiteral("!muted:x");
    const QString marked = QStringLiteral("!marked:x");
    const QString threads = QStringLiteral("!threads:x");
    QList<RoomInfo> rooms{
        spaceRoom(kWork, QStringLiteral("Work"), { muted, marked, threads }),
        room(muted, QStringLiteral("muted"), 2, 1),
        room(marked, QStringLiteral("marked")),
        room(threads, QStringLiteral("threads"), 6),
    };
    rooms[1].hasUnreadMessages = true;
    rooms[2].markedUnread = true;
    rooms[3].unreadThreadsOnly = true;
    m_settings->setRoomNotificationMode(muted, 2);
    load(rooms, true);
    m_spaces->setNotificationModeSource([this](const QString &id) {
        return m_settings->roomNotificationMode(id);
    });

    QHash<QString, QVariantMap> byId;
    for (const QVariant &v : m_spaces->directChildRoomsDetailed(kWork)) {
        const QVariantMap row = v.toMap();
        byId.insert(row.value(QStringLiteral("roomId")).toString(), row);
    }
    QCOMPARE(byId.size(), 3);
    QVERIFY(!byId.value(muted).value(QStringLiteral("hasUnread")).toBool());
    QCOMPARE(byId.value(muted).value(QStringLiteral("highlightCount")).toInt(), 0);
    QCOMPARE(byId.value(muted).value(QStringLiteral("unreadCount")).toInt(), 0);
    QVERIFY(byId.value(marked).value(QStringLiteral("hasUnread")).toBool());
    QVERIFY(byId.value(threads).value(QStringLiteral("hasUnread")).toBool());
    QCOMPARE(byId.value(threads).value(QStringLiteral("unreadCount")).toInt(), 0);
    m_settings->setRoomNotificationMode(muted, 0);
}

QTEST_MAIN(UnreadBadgeAttributionTest)
#include "UnreadBadgeAttributionTest.moc"
