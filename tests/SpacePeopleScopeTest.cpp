// The People list, scoped to the selected Space.
//
// A DM cannot be a Space's child in Matrix, so "the people of this Space" can
// only mean the people you have DMs WITH who are in it. That is Element's
// reading and it needs the Space's roster — which is not free, is not local,
// and arrives later than the list does. Everything difficult here is about
// that gap.
//
// THE TWO LAYOUTS FAIL IN OPPOSITE DIRECTIONS, and each suite half exists to
// pin its own:
//
//   * Classic REMOVES DMs from a list that already shows them, so an unknown
//     roster must change NOTHING. A People chip that empties itself while it
//     waits for an answer is the original "the people tab in spaces isn't
//     populated" report with a delay in front of it.
//   * Channels ADDS a People group to a Space view that has none, so an
//     unknown roster must add NOTHING. Fail that one open and every Space
//     lists every DM until its roster lands.
//
// And UNKNOWN has four spellings, all of them tested: never asked (a backend
// that cannot answer), asked and not yet answered, answered with a failure,
// and answered with a roster the bridge TRUNCATED at its 500-member cap. The
// last is the subtle one — a capped list cannot distinguish the people it
// dropped from the people who are not there.
//
// Every case drives the real triggers: SpaceManager::setActiveSpaceId is what
// asks for a roster, and MatrixClient::roomMembersReceived is what delivers
// one. Nothing calls the roster cache directly.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const auto kWork = QStringLiteral("!work:x");
const auto kGeneral = QStringLiteral("!general:x");
const auto kDmIn = QStringLiteral("!dm-in:x");
const auto kDmOut = QStringLiteral("!dm-out:x");
const auto kAda = QStringLiteral("@ada:x");
const auto kZoe = QStringLiteral("@zoe:x");

RoomInfo room(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
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

RoomInfo dm(const QString &id, const QString &name, const QString &peer)
{
    RoomInfo info = room(id, name);
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
    /// Every roster asked for, in order, and the op it was given.
    QStringList memberRequests;
    quint64 nextOp = 0;
    /// 0 = "this backend cannot answer", which is what a real client returns
    /// before it has an SDK handle. The manager must not mark such a Space as
    /// asked, or its People list is unscoped for the whole session.
    bool memberRequestsSupported = true;

    quint64 requestRoomMembers(const QString &roomId) override
    {
        if (!memberRequestsSupported)
            return 0;
        memberRequests.append(roomId);
        return ++nextOp;
    }

    /// One member snapshot, shaped exactly like the bridge's.
    void deliverRoster(quint64 op, const QString &roomId,
                       const QStringList &joined, bool partial = false,
                       bool truncated = false, bool ok = true)
    {
        QVariantList members;
        for (const QString &userId : joined) {
            members.append(QVariantMap{
                { QStringLiteral("userId"), userId },
                { QStringLiteral("membership"), QStringLiteral("joined") },
            });
        }
        QVariantMap snapshot{
            { QStringLiteral("ok"), ok },
            { QStringLiteral("partial"), partial },
            { QStringLiteral("truncated"), truncated },
            { QStringLiteral("members"), members },
        };
        Q_EMIT roomMembersReceived(op, roomId, snapshot);
    }

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:x"); }
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

/// One Space with one room in it, and two DMs: one with somebody who is in
/// the Space and one with somebody who is not. That second DM is the whole
/// experiment — it is the row a scope must remove and an unscoped list must
/// keep.
QList<RoomInfo> workspace()
{
    return {
        spaceRoom(kWork, QStringLiteral("Work"), { kGeneral }),
        room(kGeneral, QStringLiteral("general")),
        dm(kDmIn, QStringLiteral("Ada"), kAda),
        dm(kDmOut, QStringLiteral("Zoe"), kZoe),
    };
}

QStringList idsOf(const RoomListModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i),
                              RoomListModel::RoomIdRole).toString());
    }
    out.sort();
    return out;
}

QStringList namesOf(const SpaceChannelModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i, 0),
                              SpaceChannelModel::NameRole).toString());
    }
    return out;
}

} // namespace

class SpacePeopleScopeTest : public QObject
{
    Q_OBJECT

private:
    struct Classic {
        FakeClient client;
        SpaceManager spaces;
        RoomListModel model;

        void build()
        {
            client.mirror = workspace();
            spaces.setClient(&client);
            model.setSpaceManager(&spaces);
            model.setClient(&client);
        }
        /// The rail selecting a Space — the one thing that asks for a roster.
        void select(const QString &spaceId)
        {
            spaces.setActiveSpaceId(spaceId);
        }
    };

    struct Channels {
        FakeClient client;
        SpaceManager spaces;
        SettingsManager settings;
        RailLayoutStore layout{ &settings };
        SpaceChannelModel model;

        void build()
        {
            client.mirror = workspace();
            spaces.setClient(&client);
            model.setSettings(&settings);
            model.setSources(&client, &spaces, &layout);
        }
        /// The rail's selection reaches BOTH: SpaceManager (which asks for
        /// the roster) and the column's `scopeSpaceId` binding.
        void select(const QString &spaceId)
        {
            spaces.setActiveSpaceId(spaceId);
            model.setScopeSpaceId(spaceId);
        }
    };

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("space-people-scope-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ── Classic ──────────────────────────────────────────────────────────

    void theClassicPeopleChipIsScopedToTheSelectedSpacesPeople()
    {
        Classic f;
        f.build();
        f.select(kWork);
        // Selecting the Space is what asks, and it asks for the SPACE ROOM's
        // roster — not for any child room's.
        QCOMPARE(f.client.memberRequests, QStringList{ kWork });

        // Nothing has answered yet: the list is exactly what it was before
        // this feature existed.
        f.model.setFilterMode(1);
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));

        f.client.deliverRoster(1, kWork, { QStringLiteral("@me:x"), kAda });
        QVERIFY(f.spaces.spaceRosterKnown(kWork));

        // People: only the DM with somebody who is in the Space.
        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });
        // Rooms is untouched — it never held a DM.
        f.model.setFilterMode(2);
        QCOMPARE(idsOf(f.model), QStringList{ kGeneral });
        // And All is still exactly People plus Rooms, which is what the word
        // means and is the invariant the first attempt at this broke.
        f.model.setFilterMode(0);
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kGeneral }));
    }

    void aDirectMessageIsNeverScopedOutsideARealSpace()
    {
        Classic f;
        f.build();
        f.select(kWork);
        f.client.deliverRoster(1, kWork, { kAda });
        f.model.setFilterMode(1);
        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });

        // "All rooms" and the orphans row are VIEWS, not containers. Every DM
        // is in both, which is what makes scoping a Space safe: the chat is
        // always one click away.
        f.select(SpaceManager::allRoomsId());
        QTRY_COMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));
        f.select(SpaceManager::orphansId());
        QTRY_COMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));
        // Neither of them asked anybody for a roster.
        QCOMPARE(f.client.memberRequests, QStringList{ kWork });
    }

    void anUnansweredOrRefusedRosterLeavesEveryDirectMessageInPlace()
    {
        // A backend that cannot answer at all: the Space must not be recorded
        // as asked, or its People list is silently unscoped forever AND never
        // retried.
        Classic quiet;
        quiet.client.memberRequestsSupported = false;
        quiet.build();
        quiet.select(kWork);
        quiet.model.setFilterMode(1);
        QCOMPARE(idsOf(quiet.model), (QStringList{ kDmIn, kDmOut }));
        QVERIFY(!quiet.spaces.spaceRosterKnown(kWork));

        // A backend that answers with a failure: same list, and re-selecting
        // the Space asks again rather than failing closed for the session.
        Classic failed;
        failed.build();
        failed.select(kWork);
        failed.client.deliverRoster(1, kWork, {}, /*partial=*/false,
                                    /*truncated=*/false, /*ok=*/false);
        failed.model.setFilterMode(1);
        QCOMPARE(idsOf(failed.model), (QStringList{ kDmIn, kDmOut }));
        QVERIFY(!failed.spaces.spaceRosterKnown(kWork));
        failed.select(SpaceManager::allRoomsId());
        failed.select(kWork);
        QCOMPARE(failed.client.memberRequests, (QStringList{ kWork, kWork }));
    }

    void aPartialOrTruncatedRosterIsNotASmallerTruth()
    {
        // The bridge sends a cache-only PARTIAL snapshot first, under the same
        // op as the real one. It is a subset by construction, so acting on it
        // would hide DMs with everybody it happened not to hold.
        Classic f;
        f.build();
        f.select(kWork);
        f.model.setFilterMode(1);
        f.client.deliverRoster(1, kWork, { kAda }, /*partial=*/true);
        QVERIFY2(!f.spaces.spaceRosterKnown(kWork),
                 "a cache-only partial snapshot was published as a roster");
        QCOMPARE(idsOf(f.model), (QStringList{ kDmIn, kDmOut }));

        // The full answer lands under the same op and narrows the list.
        f.client.deliverRoster(1, kWork, { kAda });
        QTRY_COMPARE(idsOf(f.model), QStringList{ kDmIn });

        // A TRUNCATED roster is refused for the same reason, and this one is
        // easy to miss: it arrives ok, complete-looking, and 500 members
        // short. Zoe is absent from it and must still be listed.
        Classic capped;
        capped.build();
        capped.select(kWork);
        capped.model.setFilterMode(1);
        capped.client.deliverRoster(1, kWork, { kAda }, /*partial=*/false,
                                    /*truncated=*/true);
        QVERIFY2(!capped.spaces.spaceRosterKnown(kWork),
                 "a truncated roster was published as a complete one");
        QCOMPARE(idsOf(capped.model), (QStringList{ kDmIn, kDmOut }));

        // And a roster for a room nobody asked about is not a Space roster.
        // Rosters are accepted by ROOM rather than by op — so that the member
        // panel fetching the same Space's people counts, and so that a
        // synchronous answer cannot arrive before its op is recorded — which
        // makes "we asked for this one" the only thing keeping an ordinary
        // room's members out of the Space cache.
        capped.client.deliverRoster(99, kGeneral, { kZoe });
        QVERIFY(!capped.spaces.spaceRosterKnown(kGeneral));
        QCOMPARE(idsOf(capped.model), (QStringList{ kDmIn, kDmOut }));
    }

    // ── Channels ─────────────────────────────────────────────────────────

    void theChannelsSpaceViewGainsThePeopleOfThatSpace()
    {
        Channels f;
        f.build();
        f.select(kWork);
        // Before the roster: the Space view is exactly what it always was.
        // This is the fail-closed half — an unknown roster adds nothing.
        QTRY_VERIFY(!namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(!namesOf(f.model).contains(QStringLiteral("People")));

        f.client.deliverRoster(1, kWork, { kAda });

        // BEHIND THE FILTER. The group used to be pinned under every Space
        // view whether or not anyone asked for people, which is the opposite
        // of a filter — so under All and under Rooms it must be absent even
        // with a complete roster in hand.
        f.model.setFilterMode(0);   // All
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("People")),
                 "People is pinned to the Space view instead of filtered");
        f.model.setFilterMode(2);   // Rooms
        QVERIFY2(!namesOf(f.model).contains(QStringLiteral("People")),
                 "the Rooms filter still listed people");

        f.model.setFilterMode(1);   // People
        QTRY_VERIFY2(namesOf(f.model).contains(QStringLiteral("People")),
                     "the Space view never grew a People group");
        const QStringList names = namesOf(f.model);
        const int people = names.indexOf(QStringLiteral("People"));
        QCOMPARE(names.mid(people + 1), QStringList{ QStringLiteral("Ada") });
        QVERIFY2(!names.contains(QStringLiteral("Zoe")),
                 "a DM with somebody outside the Space was listed in it");
        // It is a GROUP, not a Space folder, and it carries the synthetic id
        // the host dispatches on.
        QCOMPARE(f.model.data(f.model.index(people, 0),
                              SpaceChannelModel::KindRole).toString(),
                 QStringLiteral("group"));
        QCOMPARE(f.model.data(f.model.index(people, 0),
                              SpaceChannelModel::RoomIdRole).toString(),
                 SpaceChannelModel::spacePeopleGroupId());

        // The DM tab is unchanged and still holds BOTH: a scope narrows a
        // view, it never takes the only route to a conversation away.
        f.select(SpaceManager::peopleId());
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Zoe")));
        // Home lists EVERY joined DM again since 2026-09-05 (its own Direct
        // Messages group, at the maintainer's request): the scope is the
        // Space view's alone.
        f.select(SpaceManager::allRoomsId());
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("Ada")));
        QVERIFY(namesOf(f.model).contains(QStringLiteral("Zoe")));
    }

    void aRosterIsAnAnswerAboutOneAccount()
    {
        // An account switch releases the client. Carrying the roster over
        // would scope the NEXT account's People list by the previous
        // account's Space membership — a cross-account leak of exactly the
        // kind the generation guards elsewhere exist to prevent.
        Channels f;
        f.build();
        f.select(kWork);
        f.client.deliverRoster(1, kWork, { kAda });
        f.model.setFilterMode(1);   // People
        QTRY_VERIFY(namesOf(f.model).contains(QStringLiteral("People")));

        FakeClient next;
        next.mirror = workspace();
        f.spaces.setClient(&next);
        QVERIFY2(!f.spaces.spaceRosterKnown(kWork),
                 "a roster survived the account it was fetched for");
        // The new client is asked for the selection the rail restored.
        QCOMPARE(next.memberRequests, QStringList{ kWork });
        f.model.setSources(&next, &f.spaces, &f.layout);
        QTRY_VERIFY(!namesOf(f.model).contains(QStringLiteral("People")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(SpacePeopleScopeTest)
#include "SpacePeopleScopeTest.moc"
