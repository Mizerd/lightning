// Holding the room list's ORDER still while it is being used.
//
// The tester's report: "when you click on something it moves it to the top,
// because the room order is sorted by time", and Element "only updates room
// order when you click away from it". Only the ORDER is held: a new message
// still updates its row's preview and badge at once, rooms still appear and
// disappear live, and a Space, tab or search change, a release, or the cap
// applies the held order.
//
// Covers BOTH layouts (RoomListModel and SpaceChannelModel), which must agree,
// and the open-room rule that applies with the hold off as well: a stamp that
// predates opening a room must not send it to the top.
//
// Every case drives the real triggers: the client announcing rooms, the rail's
// selection, the filter chips, the search box.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const auto kWork = QStringLiteral("!work:x");
const auto kR1 = QStringLiteral("!r1:x");
const auto kR2 = QStringLiteral("!r2:x");
const auto kR3 = QStringLiteral("!r3:x");
const auto kR4 = QStringLiteral("!r4:x");

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


/// Three rooms, newest first: r1, r2, r3.
QList<RoomInfo> rooms3()
{
    return {
        room(kR1, QStringLiteral("alpha"), 10),
        room(kR2, QStringLiteral("bravo"), 60),
        room(kR3, QStringLiteral("charlie"), 120),
    };
}

QStringList idsOf(const RoomListModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i),
                              RoomListModel::RoomIdRole).toString());
    }
    return out;
}

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

int rowOf(const RoomListModel &model, const QString &id)
{
    return idsOf(model).indexOf(id);
}

/// Room rows only (no headers, no command rows), in display order.
QStringList roomIdsOf(const SpaceChannelModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        const QModelIndex idx = model.index(i, 0);
        if (model.data(idx, SpaceChannelModel::KindRole).toString()
            == QLatin1String("room")) {
            out.append(model.data(idx, SpaceChannelModel::RoomIdRole)
                           .toString());
        }
    }
    return out;
}

const QStringList kLive{ kR2, kR1, kR3 };     // r2 raised above r1
const QStringList kStill{ kR1, kR2, kR3 };    // what a held list keeps

} // namespace

class RoomOrderDeferTest : public QObject
{
    Q_OBJECT

private:
    struct Classic {
        FakeClient client;
        SpaceManager spaces;
        RoomListModel model;

        void build(bool defer = true, QList<RoomInfo> rooms = rooms3())
        {
            client.mirror = rooms;
            spaces.setClient(&client);
            model.setSpaceManager(&spaces);
            model.setClient(&client);
            model.setDeferReordering(defer);
        }
    };

    struct Channels {
        FakeClient client;
        SpaceManager spaces;
        SettingsManager settings;
        RailLayoutStore layout{ &settings };
        SpaceChannelModel model;

        void build(bool defer = true, QList<RoomInfo> rooms = rooms3())
        {
            client.mirror = rooms;
            spaces.setClient(&client);
            model.setSettings(&settings);
            model.setSources(&client, &spaces, &layout);
            model.setDeferReordering(defer);
        }
        /// The model rebuilds on a zero timer after the client announces.
        void settle() { QTest::qWait(20); }
    };

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("room-order-defer-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // -- Classic ----------------------------------------------------------

    // Old code: there is no hold, so r2 jumps above r1 on the message and the
    // list reads {r2, r1, r3}.
    void classicAHeldOrderKeepsRowsStillWhileTheRowUpdatesInPlace()
    {
        Classic c;
        c.build();
        QCOMPARE(idsOf(c.model), kStill);
        QVERIFY(!c.model.orderHeld());

        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();

        QCOMPARE(idsOf(c.model), kStill);
        QVERIFY(c.model.orderHeld());
        // The row itself is live: preview, badge and time all moved.
        const int row = rowOf(c.model, kR2);
        QCOMPARE(c.model.data(c.model.index(row),
                              RoomListModel::LastMessagePreviewRole).toString(),
                 QStringLiteral("ping"));
        QCOMPARE(c.model.data(c.model.index(row),
                              RoomListModel::UnreadCountRole).toInt(), 1);
        QVERIFY(c.model.data(c.model.index(row),
                             RoomListModel::LastActivityRole)
                    .toDateTime() > minutesAgo(1));
    }

    void classicReleasingAppliesTheHeldOrder()
    {
        Classic c;
        c.build();
        QSignalSpy held(&c.model, &RoomListModel::orderHeldChanged);
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);
        QCOMPARE(held.count(), 1);

        c.model.releaseOrder();

        QCOMPARE(idsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
        QCOMPARE(held.count(), 2);
        // And it holds again from the new order.
        c.client.message(kR3, minutesAgo(0), QStringLiteral("pong"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kLive);
        QVERIFY(c.model.orderHeld());
    }

    void classicATabSwitchAppliesTheHeldOrder()
    {
        Classic c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        c.model.setFilterMode(2);   // Rooms: all three still qualify

        QCOMPARE(idsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void classicASearchAppliesTheHeldOrder()
    {
        Classic c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        c.model.setSearchQuery(QStringLiteral("a"));   // debounced 200 ms

        QTRY_COMPARE_WITH_TIMEOUT(idsOf(c.model), kLive, 2000);
    }

    void classicASpaceSwitchAppliesTheHeldOrder()
    {
        QList<RoomInfo> rooms = rooms3();
        RoomInfo work = room(kWork, QStringLiteral("Work"), 500);
        work.isSpace = true;
        work.childRoomIds = { kR1, kR2 };
        rooms.append(work);
        Classic c;
        c.build(true, rooms);
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        c.spaces.setActiveSpaceId(kWork);

        QCOMPARE(idsOf(c.model), (QStringList{ kR2, kR1 }));
        QVERIFY(!c.model.orderHeld());
    }

    void classicNewAndRemovedRoomsStillApplyLiveWhileHeld()
    {
        Classic c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        // A room joined just now enters where its own activity puts it.
        c.client.mirror.append(room(kR4, QStringLiteral("delta"), 1));
        c.client.announce();
        QCOMPARE(idsOf(c.model), (QStringList{ kR4, kR1, kR2, kR3 }));

        // A room left disappears at once, and the rest keep their places.
        c.client.mirror.removeIf([](const RoomInfo &r) { return r.id == kR1; });
        c.client.announce();
        QCOMPARE(idsOf(c.model), (QStringList{ kR4, kR2, kR3 }));
    }

    void classicWithTheSettingOffTheOrderIsLive()
    {
        Classic c;
        c.build(false);
        QVERIFY(!c.model.deferReordering());
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void classicTurningTheSettingOffAppliesTheHeldOrderAtOnce()
    {
        Classic c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        c.model.setDeferReordering(false);

        QCOMPARE(idsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void classicAHeldOrderIsCappedSoItNeverGoesStaleForEver()
    {
        Classic c;
        c.build();
        c.model.setHoldCapMs(50);
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), kStill);

        QTRY_COMPARE_WITH_TIMEOUT(idsOf(c.model), kLive, 2000);
        QVERIFY(!c.model.orderHeld());
    }

    // Old code: the stamp lands on the open room and it climbs to the top.
    void classicOpeningARoomDoesNotMoveItOnAStaleStamp()
    {
        Classic c;
        c.build(false);                       // the guard applies either way
        c.model.setPinnedRoomId(kR3);         // the user opens r3
        QCOMPARE(idsOf(c.model), kStill);

        // The SDK catches up a message r3 already had before it was opened.
        c.client.find(kR3)->raiseActivity(minutesAgo(30));
        c.client.announce();

        QCOMPARE(idsOf(c.model), kStill);
    }

    void classicARealMessageInTheOpenRoomStillMovesIt()
    {
        Classic c;
        c.build(false);
        c.model.setPinnedRoomId(kR3);
        c.client.message(kR3, minutesAgo(0), QStringLiteral("now"));
        c.client.announce();
        QCOMPARE(idsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
    }

    // -- Channels ---------------------------------------------------------

    void channelsAHeldOrderKeepsRowsStillWhileTheRowUpdatesInPlace()
    {
        Channels c;
        c.build();
        QCOMPARE(roomIdsOf(c.model), kStill);
        QVERIFY(!c.model.orderHeld());

        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();

        QCOMPARE(roomIdsOf(c.model), kStill);
        QVERIFY(c.model.orderHeld());
        // The badge is live.
        const int row = [&] {
            for (int i = 0; i < c.model.rowCount(); ++i) {
                if (c.model.data(c.model.index(i, 0),
                                 SpaceChannelModel::RoomIdRole).toString() == kR2)
                    return i;
            }
            return -1;
        }();
        QVERIFY(row >= 0);
        QCOMPARE(c.model.data(c.model.index(row, 0),
                              SpaceChannelModel::UnreadCountRole).toInt(), 1);
    }

    void channelsReleasingAppliesTheHeldOrder()
    {
        Channels c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kStill);

        c.model.releaseOrder();

        QCOMPARE(roomIdsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void channelsATabSwitchAppliesTheHeldOrder()
    {
        Channels c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kStill);

        c.model.setFilterMode(2);

        QCOMPARE(roomIdsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void channelsASpaceSwitchAppliesTheHeldOrder()
    {
        QList<RoomInfo> rooms = rooms3();
        RoomInfo work = room(kWork, QStringLiteral("Work"), 500);
        work.isSpace = true;
        work.childRoomIds = { kR1, kR2 };
        rooms.append(work);
        Channels c;
        c.build(true, rooms);
        c.spaces.setActiveSpaceId(kWork);
        c.model.setScopeSpaceId(kWork);
        QCOMPARE(roomIdsOf(c.model), (QStringList{ kR1, kR2 }));
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), (QStringList{ kR1, kR2 }));

        c.model.setScopeSpaceId(QString());   // Home
        c.model.setScopeSpaceId(kWork);       // and back: a fresh ordering

        QCOMPARE(roomIdsOf(c.model), (QStringList{ kR2, kR1 }));
    }

    void channelsWithTheSettingOffTheOrderIsLive()
    {
        Channels c;
        c.build(false);
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kLive);
        QVERIFY(!c.model.orderHeld());
    }

    void channelsTurningTheSettingOffAppliesTheHeldOrderAtOnce()
    {
        Channels c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kStill);

        c.model.setDeferReordering(false);

        QCOMPARE(roomIdsOf(c.model), kLive);
    }

    void channelsNewRoomsStillApplyLiveWhileHeld()
    {
        Channels c;
        c.build();
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        c.client.mirror.append(room(kR4, QStringLiteral("delta"), 1));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), (QStringList{ kR4, kR1, kR2, kR3 }));
    }

    void channelsAHeldOrderIsCapped()
    {
        Channels c;
        c.build();
        c.model.setHoldCapMs(50);
        c.client.message(kR2, minutesAgo(0), QStringLiteral("ping"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kStill);
        QTRY_COMPARE_WITH_TIMEOUT(roomIdsOf(c.model), kLive, 2000);
    }

    void channelsOpeningARoomDoesNotMoveItOnAStaleStamp()
    {
        Channels c;
        c.build(false);
        c.model.setOpenRoomId(kR3);
        c.client.find(kR3)->raiseActivity(minutesAgo(30));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), kStill);

        // A message sent after the open is real and moves it.
        c.client.message(kR3, minutesAgo(0), QStringLiteral("now"));
        c.client.announce();
        c.settle();
        QCOMPARE(roomIdsOf(c.model), (QStringList{ kR3, kR1, kR2 }));
    }

    // -- The setting and its wiring ----------------------------------------

    // Element's behaviour is the default, and the choice survives a restart.
    void theSettingIsOnByDefaultAndIsRemembered()
    {
        {
            SettingsManager settings;
            QVERIFY(settings.keepRoomListOrderStill());
            QSignalSpy changed(&settings,
                               &SettingsManager::keepRoomListOrderStillChanged);
            settings.setKeepRoomListOrderStill(false);
            QVERIFY(!settings.keepRoomListOrderStill());
            QCOMPARE(changed.count(), 1);
            settings.setKeepRoomListOrderStill(false);   // no-op
            QCOMPARE(changed.count(), 1);
        }
        SettingsManager again;
        QVERIFY(!again.keepRoomListOrderStill());
    }

    // The other ways out of a held order live in QML (the models cannot see
    // the window or the scroll position), and a source scan is the only thing
    // that can tell a dropped trigger from a working one here.
    void theHostReleasesOnFocusAndWhenTheListIsAtRest()
    {
        const QString host = readFile(QStringLiteral(QML_DIR "/RoomsPanel.qml"));
        QVERIFY(!host.isEmpty());
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("Qt.application.state === Qt.ApplicationActive")),
                 "returning to the window no longer releases a held order");
        QVERIFY2(flat.contains(QStringLiteral("interval: 3000")),
                 "the idle-at-top release is gone");
        QVERIFY2(flat.contains(QStringLiteral("running: root.orderHeld && listBody.listAtRest")),
                 "the idle release is not gated on a held order and a list at rest");
        QVERIFY2(flat.contains(QStringLiteral("app.roomList.releaseOrder()"))
                     && flat.contains(QStringLiteral("app.spaceChannels.releaseOrder()")),
                 "a release must reach both layouts' models");
        for (const QString &presenter : { QStringLiteral("RoomListClassicPresenter.qml"),
                                          QStringLiteral("RoomChannelsPresenter.qml") }) {
            const QString body = readFile(QStringLiteral(QML_DIR "/") + presenter);
            QVERIFY2(body.contains(QStringLiteral("readonly property bool atRest")),
                     qPrintable(presenter + " does not say when it is at rest"));
        }
    }

    void theSettingIsInSettingsAndFindableBySearch()
    {
        const QString screen =
            readFile(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        QVERIFY(!screen.isEmpty());
        QVERIFY2(screen.contains(QStringLiteral("objectName: \"keepRoomListOrderStillCheck\"")),
                 "the setting has no control in Settings");
        QVERIFY2(screen.contains(QStringLiteral("anchor: \"keepRoomListOrderStillCheck\"")),
                 "the setting is not findable in settings search");
        QVERIFY2(screen.contains(QStringLiteral("control: \"keepRoomListOrderStill\"")),
                 "the search result has no inline switch");
        QVERIFY2(screen.contains(QStringLiteral("app.settings.keepRoomListOrderStill = checked")),
                 "the control does not write the setting");
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(RoomOrderDeferTest)
#include "RoomOrderDeferTest.moc"
