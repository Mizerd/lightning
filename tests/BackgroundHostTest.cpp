// Live accounts Phase 1: one background host's drain (design §3.3.1, §10.1).
//
//  * the badge summary is the foreground's own unread rule over the WHOLE
//    counts table (more than 200 unread rooms), with each account's own
//    notification modes, written as one model row and never accountsChanged;
//  * a host event's notification context is its own account's (uid, label,
//    mode, ignored list), never the active account's, with no avatar fetch;
//  * the backlog floor is max(appStart, lastLiveAt) - 30 s;
//  * the drain stays inside its budget and round-robins, and a 10k-event
//    flood loses nothing while badges keep moving;
//  * the sticky session state drives the host's lifecycle.
//
// UNRUN when written (WP agents do not build); the lead runs it offscreen.
#include "accounts/BackgroundDrainer.h"
#include "accounts/BackgroundSessionHost.h"
#include "accounts/RevocationProofs.h"
#include "accounts/SessionPool.h"
#include "accounts/SessionScheduler.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "matrix/RoomInfo.h"
#include "matrix/RoomUnreadRule.h"
#include "models/AccountActivityModel.h"
#include "notifications/NotificationManager.h"
#include "storage/InMemorySecretStore.h"
#include "support/FakeRustBridge.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using lightning::accounts::BackgroundDrainer;
using lightning::accounts::BackgroundSessionHost;
using lightning::accounts::SessionPool;
using lightning::accounts::testing::FakeHandle;
using lightning::accounts::testing::FakeRustBridge;

namespace {

const QString kAlice = QStringLiteral("@alice:example.org");
const QString kBob = QStringLiteral("@bob:example.org");
const QString kHs = QStringLiteral("https://example.org");

// counts-tuple flags (contract §3.2)
constexpr int kHasUnread = 1, kMarkedUnread = 2, kThreadsOnly = 4, kMentionInThreads = 8,
              kEncrypted = 16, kEncryptionKnown = 32, kLowerBound = 64;

struct Seen {
    int unreadRooms = -1;
    int mentions = -1;
    int invites = -1;
    bool lowerBound = false;
};

Seen seenFor(const AccountActivityModel &model, const QString &uid)
{
    Seen out;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row, 0);
        if (model.data(index, AccountActivityModel::UserIdRole).toString() != uid)
            continue;
        out.unreadRooms = model.data(index, AccountActivityModel::UnreadRoomsRole).toInt();
        out.mentions = model.data(index, AccountActivityModel::MentionCountRole).toInt();
        out.invites = model.data(index, AccountActivityModel::InvitesRole).toInt();
        out.lowerBound = model.data(index, AccountActivityModel::MentionsLowerBoundRole).toBool();
    }
    return out;
}

QJsonObject timelineEvent(const QString &roomId, const QString &eventId, const QString &sender,
                          bool highlight = false)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("timeline_event")},
        {QStringLiteral("room_id"), roomId},
        {QStringLiteral("event"),
         QJsonObject{{QStringLiteral("event_id"), eventId},
                     {QStringLiteral("sender"), sender},
                     {QStringLiteral("msgtype"), QStringLiteral("text")},
                     {QStringLiteral("body"), QStringLiteral("fixture body")},
                     {QStringLiteral("timestamp_ms"), 1760000000000.0},
                     {QStringLiteral("push_notify"), true},
                     {QStringLiteral("push_highlight"), highlight}}},
        {QStringLiteral("bg"),
         QJsonObject{{QStringLiteral("reason"), highlight ? QStringLiteral("mention")
                                                          : QStringLiteral("push")},
                     {QStringLiteral("room_name"), QStringLiteral("Fixture room")},
                     {QStringLiteral("is_direct"), false},
                     {QStringLiteral("sender_display_name"), QStringLiteral("Carol")}}}};
}

} // namespace

class BackgroundHostTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void theSummaryIsTheForegroundRuleOverTheWholeCountsTable();
    void modesAreReadPerAccountNeverFromTheActiveOne();
    void aBackgroundEventGetsItsOwnAccountsContext();
    void aLateForwardedDecryptionIsANewCandidate();
    void theBacklogFloorIsTheLaterOfAppStartAndLastLiveAt();
    void theDrainStaysInsideItsBudgetAndRoundRobins();
    void aFloodLosesNothingAndBadgesKeepMoving();
    void theStickySessionStateDrivesTheHost();
    void queueCopiesOfStickyTypesAreCountedAndDropped();
    void aVerificationRequestRaisesOneLabelledCard();
    void aCancelledVerificationRequestDropsItsCard();
    void aHostRetiredMidTickShiftsNoOtherHost();

private:
    std::unique_ptr<BackgroundSessionHost> makeHost(const QString &uid, const QString &label,
                                                    NotificationManager *notifications = nullptr);
    FakeHandle *handleOf(const BackgroundSessionHost &host) const
    {
        return static_cast<FakeHandle *>(host.handle());
    }
    void setModeFor(const QString &uid, const QString &roomId, int mode);

    QTemporaryDir m_config;
    InMemorySecretStore m_secrets;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<AccountActivityModel> m_activity;
    std::unique_ptr<FakeRustBridge> m_fake;
    qint64 m_now = 0;
};

void BackgroundHostTest::initTestCase()
{
    QVERIFY(m_config.isValid());
    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
    // A dead session bus, set before any NotificationManager first asks for
    // one: every card goes to the tray path these cases count, whatever bus
    // the machine running them has (§18 round 1, MUST 5; CMake sets the same).
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/lightning-no-daemon");
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("background-host-test"));
}

void BackgroundHostTest::init()
{
    {
        QSettings wipe;
        wipe.clear();
        wipe.sync();
    }
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(&m_secrets);
    m_settings->saveSession(kHs, kAlice, QStringLiteral("ALICEDEV"),
                            QStringLiteral("alice-token-fixture"));
    m_settings->saveSession(kHs, kBob, QStringLiteral("BOBDEV"),
                            QStringLiteral("bob-token-fixture"));
    m_activity = std::make_unique<AccountActivityModel>();
    m_fake = std::make_unique<FakeRustBridge>();
    m_now = 0;
}

void BackgroundHostTest::cleanup()
{
    m_activity.reset();
    m_fake.reset();
    m_settings.reset();
}

void BackgroundHostTest::setModeFor(const QString &uid, const QString &roomId, int mode)
{
    // The setter writes the active account's scope; the host must then read
    // it back through roomNotificationModeFor(uid, ...) whatever is active.
    const QString was = m_settings->activeAccountUserId();
    m_settings->setActiveAccountUserId(uid);
    m_settings->setRoomNotificationMode(roomId, mode);
    m_settings->setActiveAccountUserId(was);
}

std::unique_ptr<BackgroundSessionHost>
BackgroundHostTest::makeHost(const QString &uid, const QString &label,
                             NotificationManager *notifications)
{
    const QString store = m_config.filePath(QStringLiteral("stores/") + uid);
    void *handle = m_fake->createBackground(store, QByteArrayLiteral("{\"v\":1}"));
    BackgroundSessionHost::Deps deps;
    deps.api = m_fake.get();
    deps.settings = m_settings.get();
    deps.notifications = notifications;
    deps.activity = m_activity.get();
    deps.monotonicMs = [this] { return m_now; };
    BackgroundSessionHost::Identity identity;
    identity.userId = uid;
    identity.deviceId = uid == kAlice ? QStringLiteral("ALICEDEV") : QStringLiteral("BOBDEV");
    identity.homeserver = kHs;
    identity.storePath = store;
    identity.slug = uid.mid(1, uid.indexOf(QLatin1Char(':')) - 1);
    identity.label = label;
    return std::make_unique<BackgroundSessionHost>(std::move(deps), identity, handle, 1, 0);
}

// R2-M4: the counts table is the whole table (no 200 cap) and the totals are
// the foreground's own rule with this account's modes.
void BackgroundHostTest::theSummaryIsTheForegroundRuleOverTheWholeCountsTable()
{
    m_settings->setActiveAccountUserId(kBob);
    auto host = makeHost(kAlice, QStringLiteral("Alice"));
    m_activity->setLiveState(kAlice, AccountActivityModel::Running);

    QJsonArray counts;
    QHash<QString, int> modes;
    for (int i = 0; i < 260; ++i) {
        const QString room = QStringLiteral("!r%1:example.org").arg(i);
        int flags = kEncryptionKnown;
        int unread = 0;
        int highlight = 0;
        switch (i % 7) {
        case 0: unread = 3; flags |= kHasUnread; break;
        case 1: highlight = 2; unread = 2; flags |= kHasUnread | kEncrypted | kLowerBound; break;
        case 2: flags |= kMarkedUnread; break;
        case 3: flags |= kThreadsOnly | kMentionInThreads | kHasUnread; break;
        case 4: highlight = 1; unread = 1; flags |= kHasUnread; break;
        case 5: unread = 5; flags |= kHasUnread | kEncrypted; break;
        default: flags |= kHasUnread; break;
        }
        // Alice mutes every tenth room and sets every eleventh to mentions.
        int mode = roomunread::kModeAllMessages;
        if (i % 10 == 0)
            mode = roomunread::kModeMute;
        else if (i % 11 == 0)
            mode = roomunread::kModeMentions;
        if (mode != roomunread::kModeAllMessages)
            setModeFor(kAlice, room, mode);
        modes.insert(room, mode);
        counts.append(QJsonArray{room, unread, highlight, flags});
    }

    // The oracle: RoomInfo per tuple, through roomunread with Alice's modes.
    int expectedRooms = 0;
    int expectedMentions = 0;
    bool expectedLowerBound = false;
    for (const QJsonValue &v : counts) {
        const QJsonArray t = v.toArray();
        RoomInfo r;
        r.id = t.at(0).toString();
        r.unreadCount = t.at(1).toInt();
        r.highlightCount = t.at(2).toInt();
        const int flags = t.at(3).toInt();
        r.hasUnreadMessages = flags & kHasUnread;
        r.markedUnread = flags & kMarkedUnread;
        r.unreadThreadsOnly = flags & kThreadsOnly;
        r.mentionInThreads = flags & kMentionInThreads;
        r.encrypted = flags & kEncrypted;
        r.encryptionKnown = flags & kEncryptionKnown;
        const int mode = modes.value(r.id);
        if (roomunread::countsAsUnread(r, mode)) {
            ++expectedRooms;
            expectedMentions += roomunread::mentionCount(r, mode);
            expectedLowerBound = expectedLowerBound || (flags & kLowerBound);
        }
    }
    QVERIFY2(expectedRooms > 200, "the fixture must exceed the old 200-row cap");

    QSignalSpy accountsChanged(m_settings.get(), &SettingsManager::accountsChanged);
    QSignalSpy dataChanged(m_activity.get(), &QAbstractItemModel::dataChanged);
    QSignalSpy reset(m_activity.get(), &QAbstractItemModel::modelReset);
    m_fake->setCounts(handleOf(*host), counts, 3, true);
    QVERIFY(host->drainOnce());

    const Seen seen = seenFor(*m_activity, kAlice);
    QCOMPARE(seen.unreadRooms, expectedRooms);
    QCOMPARE(seen.mentions, expectedMentions);
    QCOMPARE(seen.invites, 3);
    QCOMPARE(seen.lowerBound, expectedLowerBound);
    QCOMPARE(host->unreadRoomsForTest().size(), expectedRooms);
    // One row, never a reset, never the account list.
    QVERIFY(dataChanged.size() >= 1);
    for (const auto &args : dataChanged) {
        const QModelIndex top = args.at(0).value<QModelIndex>();
        const QModelIndex bottom = args.at(1).value<QModelIndex>();
        QCOMPARE(top.row(), bottom.row());
    }
    QCOMPARE(reset.size(), 0);
    QCOMPARE(accountsChanged.size(), 0);
    // Unchanged counts on the next take: no row update at all.
    dataChanged.clear();
    m_fake->setCounts(handleOf(*host), counts, 3);
    host->drainOnce();
    QCOMPARE(dataChanged.size(), 0);
}

void BackgroundHostTest::modesAreReadPerAccountNeverFromTheActiveOne()
{
    const QString room = QStringLiteral("!shared:example.org");
    setModeFor(kAlice, room, roomunread::kModeMute);
    setModeFor(kBob, room, roomunread::kModeAllMessages);
    // Bob is active: a host reading the active account's mode would not see
    // Alice's mute.
    m_settings->setActiveAccountUserId(kBob);
    auto alice = makeHost(kAlice, QStringLiteral("Alice"));
    auto bob = makeHost(kBob, QStringLiteral("Bob"));
    m_activity->setLiveState(kAlice, AccountActivityModel::Running);
    m_activity->setLiveState(kBob, AccountActivityModel::Running);
    const QJsonArray counts{QJsonArray{room, 4, 1, kHasUnread | kEncryptionKnown}};
    m_fake->setCounts(handleOf(*alice), counts, 0, true);
    m_fake->setCounts(handleOf(*bob), counts, 0, true);
    alice->drainOnce();
    bob->drainOnce();
    QCOMPARE(seenFor(*m_activity, kAlice).unreadRooms, 0);
    QCOMPARE(seenFor(*m_activity, kAlice).mentions, 0);
    QCOMPARE(seenFor(*m_activity, kBob).unreadRooms, 1);
    QCOMPARE(seenFor(*m_activity, kBob).mentions, 1);

    // Alice unmutes it (her scope): the host re-reads when told.
    setModeFor(kAlice, room, roomunread::kModeAllMessages);
    alice->refreshModes(room);
    QCOMPARE(seenFor(*m_activity, kAlice).unreadRooms, 1);
}

// I8, R1-H7: the context is the host's own account's.
void BackgroundHostTest::aBackgroundEventGetsItsOwnAccountsContext()
{
    const QString room = QStringLiteral("!ctx:example.org");
    setModeFor(kAlice, room, roomunread::kModeMentions);
    setModeFor(kBob, room, roomunread::kModeMute);
    m_settings->setActiveAccountUserId(kBob);
    auto host = makeHost(kAlice, QStringLiteral("Alice (work)"));
    const QString ignored = QStringLiteral("@spam:example.org");
    m_fake->setSticky(handleOf(*host), QStringLiteral("ignored_users_list"),
                      QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("users"), QJsonArray{ignored}}});
    m_fake->pushEvent(handleOf(*host), timelineEvent(room, QStringLiteral("$e1"), ignored));
    host->drainOnce();
    QCOMPARE(host->notifyEventsForTest(), 1);
    const NotificationManager::Context ctx = host->lastContextForTest();
    QCOMPARE(ctx.accountUserId, kAlice);
    QCOMPARE(ctx.accountLabel, QStringLiteral("Alice (work)"));
    QCOMPARE(ctx.selfUserId, kAlice);
    QVERIFY2(ctx.avatarMxc.isEmpty(), "a host never fetches through the foreground's bridge");
    QCOMPARE(ctx.roomName, QStringLiteral("Fixture room"));
    QCOMPARE(int(ctx.roomMode), int(roomunread::kModeMentions));   // Alice's, not Bob's mute
    QVERIFY(!ctx.roomVisibleAtLatest);
    QVERIFY(!ctx.roomHydrating);
    QVERIFY(ctx.senderIsIgnored);
    QCOMPARE(ctx.senderMemberName, QStringLiteral("Carol"));
    QVERIFY2(!ctx.initialSyncComplete, "before initial_sync_done the backlog never notifies");

    m_fake->setSticky(handleOf(*host), QStringLiteral("initial_sync_done"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
    m_fake->pushEvent(handleOf(*host),
                      timelineEvent(room, QStringLiteral("$e2"), QStringLiteral("@carol:example.org")));
    host->drainOnce();
    QVERIFY(host->lastContextForTest().initialSyncComplete);
    QVERIFY(!host->lastContextForTest().senderIsIgnored);
}

// Review WP1-R N2 (contract §3.5): Rust forwards an event_decrypted for an id
// it never forwarded as a timeline_event once its decrypted facts pass the
// filter. The host must raise it as a new candidate; handing it to
// processDecrypted() drops it, because no live UTD was ever recorded for it.
// An id the host DID see undecryptable still goes the replacement way.
// Fails with: the lateForwarded branch removed (no card for $late).
void BackgroundHostTest::aLateForwardedDecryptionIsANewCandidate()
{
    NotificationManager notifications;
    notifications.setBalloonWithdrawableForTest(false);
    notifications.setTrayShowsForTest(true);
    notifications.setAccountUserId(kBob);
    notifications.setLiveAccountsMode(true);
    m_settings->setActiveAccountUserId(kBob);
    auto host = makeHost(kAlice, QStringLiteral("Alice"), &notifications);
    m_fake->setSticky(handleOf(*host), QStringLiteral("initial_sync_done"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
    const QString room = QStringLiteral("!late:example.org");
    QJsonObject late = timelineEvent(room, QStringLiteral("$late"),
                                     QStringLiteral("@carol:example.org"));
    late.insert(QStringLiteral("type"), QStringLiteral("event_decrypted"));
    const int before = notifications.trayAttemptsForTest();
    m_fake->pushEvent(handleOf(*host), late);
    host->drainOnce();
    QCOMPARE(host->lateDecryptedEventsForTest(), 1);
    QCOMPARE(notifications.trayAttemptsForTest(), before + 1);
    QCOMPARE(notifications.lastTrayPayloadForTest()
                 .value(QStringLiteral("eventId")).toString(),
             QStringLiteral("$late"));

    // Seen undecryptable first: its decryption is a replacement, not new.
    QJsonObject utd = timelineEvent(room, QStringLiteral("$utd"),
                                    QStringLiteral("@carol:example.org"));
    QJsonObject utdEvent = utd.value(QStringLiteral("event")).toObject();
    utdEvent.insert(QStringLiteral("undecryptable"), true);
    utdEvent.insert(QStringLiteral("body"), QString());
    utd.insert(QStringLiteral("event"), utdEvent);
    m_fake->pushEvent(handleOf(*host), utd);
    host->drainOnce();
    QJsonObject decrypted = timelineEvent(room, QStringLiteral("$utd"),
                                          QStringLiteral("@carol:example.org"));
    decrypted.insert(QStringLiteral("type"), QStringLiteral("event_decrypted"));
    m_fake->pushEvent(handleOf(*host), decrypted);
    host->drainOnce();
    QCOMPARE(host->lateDecryptedEventsForTest(), 1);
}

// R2-M5: the floor handed to Rust at create.
void BackgroundHostTest::theBacklogFloorIsTheLaterOfAppStartAndLastLiveAt()
{
    for (const bool lastLiveIsLater : {true, false}) {
        init();
        FakeRustBridge fake;
        AccountManager accounts(m_settings.get());
        lightning::accounts::RevocationProofs proofs;
        lightning::accounts::StoreOwnershipRegistry registry;
        const qint64 appStart = 1760000000000;
        const qint64 lastLive = lastLiveIsLater ? appStart + 600000 : appStart - 600000;
        m_settings->setLastLiveAtFor(kBob, lastLive);
        QCOMPARE(m_settings->setKeepRunningFor(kBob, true),
                 SettingsManager::KeepRunningResult::Ok);
        SessionPool::Deps deps;
        deps.settings = m_settings.get();
        deps.accounts = &accounts;
        deps.activity = m_activity.get();
        deps.proofs = &proofs;
        deps.registry = &registry;
        deps.api = &fake;
        deps.guiSecretStore = &m_secrets;   // process-local: GUI-queued reads
        deps.appStartMs = appStart;
        deps.monotonicMs = [this] { return m_now; };
        SessionPool pool(deps);
        pool.setForegroundUserId(kAlice);
        pool.start();
        // The clock is scripted: the real-time lag probe would read a jump as a
        // suspend. And the fd limit is not under test here.
        pool.schedulerForTest()->stopProbe();
        pool.schedulerForTest()->setFdSoftLimitForTest(0);
        pool.noteForegroundInitialSyncDone();
        pool.admitNowForTest();
        QTRY_COMPARE_WITH_TIMEOUT(fake.calls(QStringLiteral("createBackground")), 1, 3000);
        const QJsonObject profile = fake.lastHandle()->profile;
        QCOMPARE(qint64(profile.value(QStringLiteral("floor_ms")).toDouble()),
                 std::max(appStart, lastLive) - 30000);
        QCOMPARE(profile.value(QStringLiteral("require_account")).toObject()
                     .value(QStringLiteral("user_id")).toString(),
                 kBob);
        QCOMPARE(profile.value(QStringLiteral("receipt_privacy")).toInt(), 2);
        pool.shutdownAsync([] {});
    }
}

void BackgroundHostTest::theDrainStaysInsideItsBudgetAndRoundRobins()
{
    auto a = makeHost(kAlice, QStringLiteral("A"));
    auto b = makeHost(kBob, QStringLiteral("B"));
    auto c = makeHost(QStringLiteral("@carol:example.org"), QStringLiteral("C"));
    BackgroundDrainer drainer;
    drainer.addHost(a.get());
    drainer.addHost(b.get());
    drainer.addHost(c.get());
    QVERIFY(drainer.running());
    // Every host costs 3 ms: the budget (4 ms) is spent after the second.
    qint64 spent = 0;
    drainer.setElapsedForTest([&spent] { return spent += 3; });
    spent = 0;
    QCOMPARE(drainer.tick(), 2);
    // The host the budget did not reach still had its token step (one more
    // poll): a waiting rotation is answered every tick (§18 round 1, L6).
    QCOMPARE(m_fake->calls(QStringLiteral("bgPoll")), 3);
    // The next tick starts where this one stopped: c first, then a.
    spent = 0;
    QCOMPARE(drainer.tick(), 2);
    QCOMPARE(m_fake->calls(QStringLiteral("bgPoll")), 6);
    // An idle host costs only its poll: no take, no session state, no queue.
    QCOMPARE(m_fake->calls(QStringLiteral("bgTake")), 0);
    QCOMPARE(m_fake->calls(QStringLiteral("bgTakeSessionState")), 0);
    QCOMPARE(m_fake->calls(QStringLiteral("pollEvent")), 0);
    QCOMPARE(m_fake->calls(QStringLiteral("takeCustody")), 0);
    // A rotation on the host the next tick's budget will not reach (that
    // tick runs b then c, so a) is taken this tick all the same. Mutation:
    // drop the drainTokensOnly() loop and takeCustody stays 0.
    m_fake->setCustody(handleOf(*a), QJsonObject{{QStringLiteral("v"), 1},
                                                 {QStringLiteral("has_session"), true},
                                                 {QStringLiteral("access_token"), QStringLiteral("b-rotated")},
                                                 {QStringLiteral("generation"), 1},
                                                 {QStringLiteral("ledger_generation"), 1}});
    spent = 0;
    QCOMPARE(drainer.tick(), 2);
    QCOMPARE(m_fake->calls(QStringLiteral("takeCustody")), 1);
    QCOMPARE(m_fake->calls(QStringLiteral("bgTake")), 0);
    // With the budget never spent, every host is polled once per tick.
    drainer.setElapsedForTest([] { return qint64(0); });
    QCOMPARE(drainer.tick(), 3);
    drainer.removeHost(a.get());
    drainer.removeHost(b.get());
    drainer.removeHost(c.get());
    QVERIFY(!drainer.running());
}

// R2-H3: 10 000 queued notify events drain 64 per pass with nothing lost,
// and the badge take runs every pass the table is dirty, flood or not.
void BackgroundHostTest::aFloodLosesNothingAndBadgesKeepMoving()
{
    auto host = makeHost(kAlice, QStringLiteral("Alice"));
    m_activity->setLiveState(kAlice, AccountActivityModel::Running);
    FakeHandle *h = handleOf(*host);
    const QString room = QStringLiteral("!flood:example.org");
    int mentions = 0;
    for (int i = 0; i < 10000; ++i) {
        const bool mention = i % 100 == 37;
        mentions += mention ? 1 : 0;
        m_fake->pushEvent(h, timelineEvent(room, QStringLiteral("$f%1").arg(i),
                                           QStringLiteral("@carol:example.org"), mention));
    }
    QCOMPARE(mentions, 100);
    int pass = 0;
    while (h->status.queuedEvents > 0 && pass < 400) {
        // The badge moves every pass while the queue is far from empty.
        const QString unreadRoom = QStringLiteral("!badge%1:example.org").arg(pass);
        m_fake->setCounts(h, QJsonArray{QJsonArray{unreadRoom, pass + 1, 0,
                                                   kHasUnread | kEncryptionKnown}});
        const quint32 before = h->status.queuedEvents;
        host->drainOnce();
        QVERIFY2(before - h->status.queuedEvents <= quint32(BackgroundSessionHost::kMaxEventsPerPass),
                 "more than 64 events in one pass");
        QVERIFY(host->unreadRoomsForTest().contains(unreadRoom));
        ++pass;
    }
    QCOMPARE(h->status.queuedEvents, quint32(0));
    QCOMPARE(pass, (10000 + 63) / 64);
    QCOMPARE(host->notifyEventsForTest(), 10000);
    QCOMPARE(host->droppedEventsForTest(), 0);
}

void BackgroundHostTest::theStickySessionStateDrivesTheHost()
{
    auto host = makeHost(kAlice, QStringLiteral("Alice"));
    FakeHandle *h = handleOf(*host);
    QSignalSpy ok(host.get(), &BackgroundSessionHost::loginSucceeded);
    QSignalSpy lost(host.get(), &BackgroundSessionHost::connectionLost);
    QSignalSpy limited(host.get(), &BackgroundSessionHost::rateLimited);
    QCOMPARE(host->phase(), BackgroundSessionHost::Phase::Starting);
    m_fake->setSticky(h, QStringLiteral("login_ok"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("login_ok")}});
    host->drainOnce();
    QCOMPARE(ok.size(), 1);
    QCOMPARE(host->phase(), BackgroundSessionHost::Phase::Running);
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 1);
    // The same sticky payload again is not a second login.
    m_fake->setSticky(h, QStringLiteral("initial_sync_done"), QJsonObject{});
    host->drainOnce();
    QCOMPARE(ok.size(), 1);
    QVERIFY(host->initialSyncDone());
    m_fake->setSticky(h, QStringLiteral("sync_error"),
                      QJsonObject{{QStringLiteral("rate_limited"), true},
                                  {QStringLiteral("retry_after_ms"), 9000.0}});
    host->drainOnce();
    QCOMPARE(limited.size(), 1);
    QCOMPARE(limited.at(0).at(0).toLongLong(), 9000);
    m_fake->setSticky(h, QStringLiteral("room_list_sync_state"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("offline")}});
    host->drainOnce();
    QCOMPARE(lost.size(), 1);

    // The precheck's refusals are named, never a retry-able failure.
    const struct {
        QJsonObject failed;
        BackgroundSessionHost::FailureKind kind;
    } cases[] = {
        {QJsonObject{{QStringLiteral("stage"), QStringLiteral("store")},
                     {QStringLiteral("reason"), QStringLiteral("crypto_account_missing")}},
         BackgroundSessionHost::FailureKind::EncryptionDataMissing},
        {QJsonObject{{QStringLiteral("stage"), QStringLiteral("store")},
                     {QStringLiteral("reason"), QStringLiteral("crypto_account_mismatch")}},
         BackgroundSessionHost::FailureKind::EncryptionDataMissing},
        {QJsonObject{{QStringLiteral("stage"), QStringLiteral("store")},
                     {QStringLiteral("reason"), QStringLiteral("store_in_use")}},
         BackgroundSessionHost::FailureKind::StoreInUse},
        {QJsonObject{{QStringLiteral("message"), QStringLiteral("connection refused")}},
         BackgroundSessionHost::FailureKind::Transient},
    };
    for (const auto &c : cases) {
        auto starting = makeHost(kBob, QStringLiteral("Bob"));
        QSignalSpy failed(starting.get(), &BackgroundSessionHost::loginFailed);
        m_fake->setSticky(handleOf(*starting), QStringLiteral("login_failed"), c.failed);
        starting->drainOnce();
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).value<BackgroundSessionHost::FailureKind>(), c.kind);
        QCOMPARE(m_fake->log(handleOf(*starting)).count(QStringLiteral("startSync")), 0);
    }
}

void BackgroundHostTest::queueCopiesOfStickyTypesAreCountedAndDropped()
{
    auto host = makeHost(kAlice, QStringLiteral("Alice"));
    m_fake->pushEvent(handleOf(*host), QJsonObject{{QStringLiteral("type"),
                                                    QStringLiteral("crypto_health")}});
    m_fake->pushEvent(handleOf(*host), QJsonObject{{QStringLiteral("type"),
                                                    QStringLiteral("login_ok")}});
    host->drainOnce();
    QCOMPARE(host->droppedEventsForTest(), 2);
    QCOMPARE(host->notifyEventsForTest(), 0);
    // A login_ok on the QUEUE is not the sticky one: still starting.
    QCOMPARE(host->phase(), BackgroundSessionHost::Phase::Starting);
}

void BackgroundHostTest::aVerificationRequestRaisesOneLabelledCard()
{
    NotificationManager notifications;
    notifications.setBalloonWithdrawableForTest(false);
    notifications.setTrayShowsForTest(true);
    notifications.setAccountUserId(kBob);   // the foreground is someone else
    auto host = makeHost(kAlice, QStringLiteral("Alice"), &notifications);
    const int before = notifications.genericNoticeCountForTest();
    const QJsonObject request{{QStringLiteral("type"), QStringLiteral("verification_request_received")},
                              {QStringLiteral("flow_id"), QStringLiteral("flow-1")},
                              {QStringLiteral("other_user_id"), kAlice}};
    m_fake->pushEvent(handleOf(*host), request);
    m_fake->pushEvent(handleOf(*host), request);   // the same flow again
    host->drainOnce();
    QCOMPARE(notifications.genericNoticeCountForTest(), before + 1);
    // §18 round 1, L5: stamped for the background account (a click opens
    // it, not the one in front), with the label isolated (FSI .. PDI).
    QCOMPARE(notifications.lastTrayPayloadForTest()
                 .value(QStringLiteral("accountUserId")).toString(),
             kAlice);
    QVERIFY(notifications.lastTrayTitleForTest().contains(
        QChar(0x2068) + QStringLiteral("Alice") + QChar(0x2069)));
    // Notifications turned off: no card from a background account either.
    m_settings->setNotificationsEnabled(false);
    const QJsonObject another{{QStringLiteral("type"), QStringLiteral("verification_request_received")},
                              {QStringLiteral("flow_id"), QStringLiteral("flow-2")},
                              {QStringLiteral("other_user_id"), kAlice}};
    m_fake->pushEvent(handleOf(*host), another);
    host->drainOnce();
    QCOMPARE(notifications.genericNoticeCountForTest(), before + 1);
    m_settings->setNotificationsEnabled(true);
}

// Review L10: a request the other side cancelled withdraws its card (the
// withdrawal goes through NotificationManager::withdrawGeneric, keyed as the
// card was). Observed here by its other half: the flow's key is forgotten, so
// the same flow asking again raises a card again instead of being deduped.
void BackgroundHostTest::aCancelledVerificationRequestDropsItsCard()
{
    NotificationManager notifications;
    auto host = makeHost(kAlice, QStringLiteral("Alice"), &notifications);
    const int before = notifications.genericNoticeCountForTest();
    const QJsonObject request{{QStringLiteral("type"), QStringLiteral("verification_request_received")},
                              {QStringLiteral("flow_id"), QStringLiteral("flow-1")},
                              {QStringLiteral("other_user_id"), kAlice}};
    const QJsonObject cancelled{{QStringLiteral("type"), QStringLiteral("verification_cancelled")},
                                {QStringLiteral("flow_id"), QStringLiteral("flow-1")},
                                {QStringLiteral("message"), QStringLiteral("cancelled")}};
    const QJsonObject otherFlow{{QStringLiteral("type"), QStringLiteral("verification_cancelled")},
                                {QStringLiteral("flow_id"), QStringLiteral("flow-2")},
                                {QStringLiteral("message"), QStringLiteral("cancelled")}};
    m_fake->pushEvent(handleOf(*host), request);
    m_fake->pushEvent(handleOf(*host), otherFlow);   // another flow: no effect
    m_fake->pushEvent(handleOf(*host), request);
    host->drainOnce();
    QCOMPARE(notifications.genericNoticeCountForTest(), before + 1);
    m_fake->pushEvent(handleOf(*host), cancelled);
    m_fake->pushEvent(handleOf(*host), request);
    host->drainOnce();
    QCOMPARE(notifications.genericNoticeCountForTest(), before + 2);
    QCOMPARE(host->droppedEventsForTest(), 0);
}

// Review L13: a host retired by its own signal in the middle of a tick (here:
// its login result removes it) must not shift the hosts after it: the pass
// drains the hosts it started with, in order, and the next tick resumes with
// the right one.
void BackgroundHostTest::aHostRetiredMidTickShiftsNoOtherHost()
{
    auto a = makeHost(kAlice, QStringLiteral("A"));
    auto b = makeHost(kBob, QStringLiteral("B"));
    auto c = makeHost(QStringLiteral("@carol:example.org"), QStringLiteral("C"));
    BackgroundDrainer drainer;
    drainer.addHost(a.get());
    drainer.addHost(b.get());
    drainer.addHost(c.get());
    // Every host has a login result to act on; a's retires a at once.
    for (BackgroundSessionHost *host : {a.get(), b.get(), c.get()}) {
        m_fake->setSticky(handleOf(*host), QStringLiteral("login_ok"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("login_ok")}});
    }
    connect(a.get(), &BackgroundSessionHost::loginSucceeded, this,
            [&] { drainer.removeHost(a.get()); });
    const auto started = [this](BackgroundSessionHost *host) {
        return m_fake->log(handleOf(*host)).count(QStringLiteral("startSync"));
    };
    // Every host costs 3 ms: the 4 ms budget is spent after the second.
    qint64 spent = 0;
    drainer.setElapsedForTest([&spent] { return spent += 3; });
    QCOMPARE(drainer.tick(), 2);
    QCOMPARE(started(a.get()), 1);
    QCOMPARE(started(b.get()), 1);   // b, not c moved into a's place
    QCOMPARE(started(c.get()), 0);
    QCOMPARE(drainer.hostCount(), 2);
    spent = 0;
    QCOMPARE(drainer.tick(), 2);
    QCOMPARE(started(c.get()), 1);   // the next tick starts where this one stopped
    QCOMPARE(started(b.get()), 1);
}

// A GUI application: a card's decision reads the font database (the late-
// forwarded decryption case raises a real card).
QTEST_MAIN(BackgroundHostTest)
#include "BackgroundHostTest.moc"
