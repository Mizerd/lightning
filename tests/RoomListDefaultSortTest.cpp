// The room list's default sort (Rokas, 2026-10-20: "the order of rooms for a
// space in the left sidebar differs from the order in the space's lobby").
//
// While the user has NEVER chosen a sort the host hands the model -1
// (kSortUnset): a real Space then lists its rooms in its own m.space.child
// order, as its lobby does, and Home / pseudo-selections stay on Activity. Any
// explicit choice, Activity included, applies everywhere as before.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/ConversationOrder.h"
#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const auto kWork = QStringLiteral("!work:x");
const auto kOther = QStringLiteral("!other:x");
const auto kR1 = QStringLiteral("!r1:x");
const auto kR2 = QStringLiteral("!r2:x");
const auto kR3 = QStringLiteral("!r3:x");

QDateTime minutesAgo(int minutes)
{
    return QDateTime::currentDateTimeUtc().addSecs(-60 * minutes);
}

RoomInfo room(const QString &id, const QString &name, int ageMinutes)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
    info.lastActivity = minutesAgo(ageMinutes);
    info.lastMessagePreview = QStringLiteral("hello");
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

    bool supportsMarkRoomRead() const override { return true; }
    QStringList markReadAsked;
    /// A rejected FFI call announces failure from inside the call itself.
    bool rejectMarkRead = false;
    void markRoomRead(const QString &roomId) override
    {
        markReadAsked.append(roomId);
        if (rejectMarkRead)
            Q_EMIT markRoomReadFailed(roomId);
    }

    RoomInfo *find(const QString &id)
    {
        for (RoomInfo &r : mirror) {
            if (r.id == id)
                return &r;
        }
        return nullptr;
    }
    /// A message arrives: the sort key moves, the preview changes, a badge
    /// appears. Announced the way the Rust bridge announces it.
    void message(const QString &id, const QDateTime &when,
                 const QString &preview)
    {
        RoomInfo *r = find(id);
        QVERIFY2(r, "no such room");
        r->raiseActivity(when);
        r->lastMessagePreview = preview;
        r->unreadCount += 1;
        r->hasUnreadMessages = true;
        Q_EMIT roomUpdated(id);
    }

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



QStringList idsOf(const RoomListModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i),
                              RoomListModel::RoomIdRole).toString());
    }
    return out;
}

/// Work lists r3, r1, r2 (activity r1, r2, r3). Other lists r2, r3, r1.
QList<RoomInfo> twoSpaces()
{
    RoomInfo work = room(kWork, QStringLiteral("Work"), 500);
    work.isSpace = true;
    work.childRoomIds = { kR3, kR1, kR2 };
    RoomInfo other = room(kOther, QStringLiteral("Other"), 501);
    other.isSpace = true;
    other.childRoomIds = { kR2, kR3, kR1 };
    return { work, other, room(kR1, QStringLiteral("alpha"), 10),
             room(kR2, QStringLiteral("bravo"), 60),
             room(kR3, QStringLiteral("charlie"), 120) };
}

} // namespace

class RoomListDefaultSortTest : public QObject
{
    Q_OBJECT

private:
    struct Classic {
        FakeClient client;
        SpaceManager spaces;
        RoomListModel model;

        void build()
        {
            client.mirror = twoSpaces();
            spaces.setClient(&client);
            model.setSpaceManager(&spaces);
            model.setClient(&client);
            model.setDeferReordering(false);
        }
    };

    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    }

    // End to end through SettingsManager (MASTER-1 H2): nothing stored is
    // "never chosen" (-1, the Space's order inside a Space); an explicit
    // Activity is stored even though it equals the default and stays
    // Activity inside the Space. Old code: roomListSort() normalised the
    // absent value to Activity and setRoomListSort(Activity) stored nothing,
    // so a Space listed r1, r2, r3 from the start.
    void theSettingHandsTheModelsNeverChosenUntilAChoice()
    {
        QSettings().remove(QStringLiteral("shell/roomListSort"));
        SettingsManager settings;
        QVERIFY(!settings.roomListSortChosen());
        QCOMPARE(settings.roomListSort(), int(conversation::SortByActivity));
        QCOMPARE(settings.roomListSortForModels(), -1);

        Classic c;
        c.build();
        c.model.setSortMode(settings.roomListSortForModels());
        c.spaces.setActiveSpaceId(kWork);
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));

        QSignalSpy spy(&settings, &SettingsManager::roomListSortChanged);
        settings.setRoomListSort(conversation::SortByActivity);
        QCOMPARE(spy.count(), 1);
        QVERIFY(settings.roomListSortChosen());
        QCOMPARE(settings.roomListSortForModels(), int(conversation::SortByActivity));
        c.model.setSortMode(settings.roomListSortForModels());
        QCOMPARE(idsOf(c.model), (QStringList{ kR1, kR2, kR3 }));

        // Remembered as a choice by the next session.
        SettingsManager next;
        QVERIFY(next.roomListSortChosen());
        QCOMPARE(next.roomListSortForModels(), int(conversation::SortByActivity));
        QSettings().remove(QStringLiteral("shell/roomListSort"));
    }

    // Old code: -1 normalises to Activity, so the Space lists r1, r2, r3.
    void unsetInARealSpaceFollowsTheSpaceOrderOfItsLobby()
    {
        Classic c;
        c.build();
        c.model.setSortMode(-1);
        c.spaces.setActiveSpaceId(kWork);
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));

        // The ranking IS SpaceManager::childOrder, whatever it holds.
        const QHash<QString, int> order = c.spaces.childOrder(kWork);
        QCOMPARE(order.value(kR3), 0);
        QCOMPARE(order.value(kR1), 1);
        QCOMPARE(order.value(kR2), 2);

        // A message does not move a room the Space has placed.
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
    }

    void unsetFollowsEachSpaceAndFallsBackToActivityAtHome()
    {
        Classic c;
        c.build();
        c.model.setSortMode(-1);
        c.spaces.setActiveSpaceId(kWork);
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
        c.spaces.setActiveSpaceId(kOther);
        QCOMPARE(idsOf(c.model), (QStringList{ kR2, kR3, kR1 }));
        c.spaces.setActiveSpaceId(SpaceManager::allRoomsId());
        QCOMPARE(idsOf(c.model), (QStringList{ kR1, kR2, kR3 }));
    }

    void unsetAtHomeIsActivity()
    {
        Classic c;
        c.build();
        c.model.setSortMode(-1);
        c.spaces.setActiveSpaceId(SpaceManager::allRoomsId());
        QCOMPARE(idsOf(c.model), (QStringList{ kR1, kR2, kR3 }));
    }

    // An explicit Activity is a choice and applies inside a Space too.
    void anExplicitActivityStaysActivityInsideASpace()
    {
        Classic c;
        c.build();
        c.model.setSortMode(-1);
        c.spaces.setActiveSpaceId(kWork);
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
        c.model.setSortMode(conversation::SortByActivity);
        QCOMPARE(idsOf(c.model), (QStringList{ kR1, kR2, kR3 }));
        // ...and going back to "never chosen" is a state of its own.
        c.model.setSortMode(-1);
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
    }

    void unsetIsItsOwnStateAndUnknownValuesStayActivity()
    {
        Classic c;
        c.build();
        c.model.setSortMode(-1);
        QCOMPARE(c.model.sortMode(), -1);
        c.model.setSortMode(99);
        QCOMPARE(c.model.sortMode(), int(conversation::SortByActivity));
        c.model.setSortMode(-2);
        QCOMPARE(c.model.sortMode(), int(conversation::SortByActivity));
    }
};

QTEST_MAIN(RoomListDefaultSortTest)
#include "RoomListDefaultSortTest.moc"
