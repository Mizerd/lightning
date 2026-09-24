// Room-activity (state-change) grouping is transparent through read markers
// and the timeline-start marker, which matrix-sdk-ui interleaves freely. A
// date divider ends the run, so each day's activity leads its own group under
// its own date. A visible message/media/call event also ends a group.

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");

TimelineEvent makeStateChange(const QString &eventId, const QString &body,
                              const QString &kind = QStringLiteral("m.room.topic"),
                              const QString &target = {})
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = body;
    e.senderDisplayName = QStringLiteral("Alice");
    e.stateKind = kind;
    e.stateTarget = target;
    e.type = TimelineEvent::StateChange;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    return e;
}

TimelineEvent makeVirtual(TimelineEvent::Type type, const QString &id)
{
    TimelineEvent e;
    e.eventId = id;
    e.roomId = kRoom;
    e.type = type;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    return e;
}

constexpr qint64 kOneDayMs = 24 * 60 * 60 * 1000LL;

// Same rows as above, dated onto a chosen day so a multi-day run can be
// built. Day 0 is the fixed base timestamp every other helper here uses.
TimelineEvent onDay(TimelineEvent e, int day)
{
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000 + day * kOneDayMs);
    return e;
}

TimelineEvent makeMessage(const QString &eventId, const QString &body,
                          TimelineEvent::Type type = TimelineEvent::TextMessage)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = body;
    e.type = type;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    return e;
}

// Minimal scripted backend, matching the pattern in TimelineModelDiffTest.cpp.
class FakeClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit FakeClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<TimelineEvent> mirror;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &roomId) const override
    {
        return roomId == kRoom ? mirror : QList<TimelineEvent>{};
    }
    QString displayNameFor(const QString &, const QString &userId) const override { return userId; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

} // namespace

class StateActivityGroupingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void aCallIsNotARoomUpdate();

    void onlyTheNewestCallRowCanBeJoined();
    void aCallBreaksTheActivityRunAroundIt();
    void consecutiveStateChangesFormOneGroup();
    void exposesTypedMembershipAndRoomStateEntries();
    void aMembershipEntryCarriesWhatItDid();
    void dateDividerSplitsTheRunIntoDailyGroups();
    void multiDayRunYieldsOneGroupPerDay();
    void aDividerLeadingOnlyActivityStillIntroducesVisibleContent();
    void readMarkerDoesNotSplitGroup();
    void timelineStartDoesNotSplitGroup();
    void visibleTextMessageBreaksGroup();
    void undecryptableEventIsNotRoutineActivity();
    void untypedStateNotificationRemainsVisible();
    void imageMessageBreaksGroup();
    void fileMessageBreaksGroup();
    void nonLeaderRowsAreEmptyAndNotLeader();
    void groupIdPrefersItemIdOverEventId();
    void appendingContiguousStateChangeExtendsGroupForward();
    void prependingOlderStateChangesExtendsGroupBackward();
    void groupCountAlwaysMatchesAccessibleChildren();
    // A date divider must not draw when everything it introduces is hidden.
    void everyDayOfAMultiDayRunOwnsItsGroupAndItsDate();
    void hiddenRoutineActivityLeavesItsDividerWithNothingToIntroduce();
    void showRoomActivityFlipReAnnouncesTheDividerRows();

private:
    FakeClient *m_client = nullptr;
    TimelineModel *m_model = nullptr;
};

void StateActivityGroupingTest::init()
{
    m_client = new FakeClient(this);
    m_model = new TimelineModel(this);
    m_model->setClient(m_client);
}

void StateActivityGroupingTest::cleanup()
{
    delete m_model;
    delete m_client;
    m_model = nullptr;
    m_client = nullptr;
}

// Only the newest call row offers Join: `CallEventDelegate.sessionLive` is a
// per-room answer, so the model names the newest call row. The id must be the
// newest call row, ignore messages between them, and move when a newer call
// arrives.
void StateActivityGroupingTest::onlyTheNewestCallRowCanBeJoined()
{
    TimelineEvent older = makeStateChange(QStringLiteral("$call-older"),
                                          QString{}, QStringLiteral("m.call"));
    TimelineEvent chatter = makeMessage(QStringLiteral("$chatter"),
                                        QStringLiteral("unrelated"));
    TimelineEvent newer = makeStateChange(QStringLiteral("$call-newer"),
                                          QString{}, QStringLiteral("m.call"));
    m_client->mirror = { older, chatter, newer };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->latestCallEventId(), QStringLiteral("$call-newer"));

    // It must move on a live append (a call starting while the room is open),
    // which exercises the countChanged hook rather than modelReset.
    QSignalSpy moved(m_model, &TimelineModel::latestCallEventIdChanged);
    TimelineEvent newest = makeStateChange(QStringLiteral("$call-newest"),
                                           QString{}, QStringLiteral("m.call"));
    m_client->mirror.append(newest);
    Q_EMIT m_client->eventAppended(kRoom, newest);
    QCOMPARE(m_model->latestCallEventId(), QStringLiteral("$call-newest"));
    QCOMPARE(moved.count(), 1);

    // And on an in-place set, which changes no row count: redacting the newest
    // call row hands the button back to the one before it.
    TimelineEvent redacted = makeMessage(QStringLiteral("$call-newest"),
                                         QStringLiteral("[removed]"));
    Q_EMIT m_client->eventChangedAt(kRoom, 3, redacted);
    QCOMPARE(m_model->latestCallEventId(), QStringLiteral("$call-newer"));
    QVERIFY2(moved.count() >= 2,
             "an in-place Set that stopped a row being a call row did not "
             "move latestCallEventId, so the Join button stays on a row that "
             "is no longer a call");

    // And the other direction: a row that becomes a call row through an
    // in-place set (late decryption, re-set `stateKind`) reclaims the title.
    // Index 3 is the row the redaction above left as an ordinary message.
    const int before = moved.count();
    TimelineEvent decrypted = makeStateChange(QStringLiteral("$call-newest"),
                                              QString{}, QStringLiteral("m.call"));
    Q_EMIT m_client->eventChangedAt(kRoom, 3, decrypted);
    QCOMPARE(m_model->latestCallEventId(), QStringLiteral("$call-newest"));
    QVERIFY2(moved.count() > before,
             "an in-place Set that turned a row INTO a call row did not move "
             "latestCallEventId, so the newest call is the one row without a "
             "Join button");

    // A room with no calls names nothing — the delegate's permissive default
    // must not be defeated by an empty string comparing equal to an id. This
    // one goes through a reset deliberately: it is the room-switch path.
    m_client->mirror = { chatter };
    m_model->setRoomId(QString{});
    m_model->setRoomId(kRoom);
    QVERIFY(m_model->latestCallEventId().isEmpty());
}

void StateActivityGroupingTest::aCallIsNotARoomUpdate()
{
    TimelineEvent call = makeStateChange(QStringLiteral("$call"), QString{},
                                         QStringLiteral("m.call"));
    m_client->mirror = { call };
    m_model->setRoomId(kRoom);

    const QModelIndex idx = m_model->index(0);
    QCOMPARE(m_model->data(idx, TimelineModel::IsCallEventRole).toBool(), true);
    QCOMPARE(m_model->data(idx, TimelineModel::IsStateActivityRole).toBool(),
             false);
    QCOMPARE(m_model->data(idx, TimelineModel::IsRoutineActivityRole).toBool(),
             false);
    // Nothing may be left for the activity group to draw.
    QVERIFY(m_model->data(idx, TimelineModel::StateGroupEntriesRole)
                .toList().isEmpty());
    // The sentence is built HERE, with the resolved display name — not in the
    // bridge, and not as the words "call event".
    QCOMPARE(m_model->data(idx, TimelineModel::CallEventTextRole).toString(),
             QStringLiteral("Alice started a call."));
}

// A call is content, so annotations either side of it form two groups, not
// one group with the call swallowed in the middle.
void StateActivityGroupingTest::aCallBreaksTheActivityRunAroundIt()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("Alice changed the topic.")),
        makeStateChange(QStringLiteral("$call"), QString{},
                        QStringLiteral("m.call")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("Bob changed the room name.")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::StateGroupEntriesRole).toList().size(),
             1);
    QCOMPARE(m_model->data(m_model->index(1),
                           TimelineModel::IsCallEventRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(2),
                           TimelineModel::StateGroupEntriesRole).toList().size(),
             1);
}

void StateActivityGroupingTest::consecutiveStateChangesFormOneGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("Alice changed the topic.")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("Bob changed the room name.")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::StateGroupLeaderRole).toBool(), false);

    const QString leaderGroupId =
        m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString();
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::StateGroupIdRole).toString(),
             leaderGroupId);

    const QVariantList entries =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("Alice changed the topic."));
    QCOMPARE(entries.at(1).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("Bob changed the room name."));
}

void StateActivityGroupingTest::exposesTypedMembershipAndRoomStateEntries()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$join"), QStringLiteral("Bob joined the room."),
                        QStringLiteral("membership"), QStringLiteral("Bob")),
        makeStateChange(QStringLiteral("$topic"), QStringLiteral("Alice changed the room topic."),
                        QStringLiteral("m.room.topic")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::IsRoutineActivityRole).toBool(),
             true);
    const QVariantList entries =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 2);
    const QVariantMap membership = entries.at(0).toMap();
    QCOMPARE(membership.value(QStringLiteral("stableEventId")).toString(),
             QStringLiteral("uid-$join"));
    QCOMPARE(membership.value(QStringLiteral("eventId")).toString(),
             QStringLiteral("$join"));
    QCOMPARE(membership.value(QStringLiteral("eventKind")).toString(),
             QStringLiteral("membership"));
    QCOMPARE(membership.value(QStringLiteral("actorDisplayName")).toString(),
             QStringLiteral("Alice"));
    QCOMPARE(membership.value(QStringLiteral("affectedMemberDisplayName")).toString(),
             QStringLiteral("Bob"));
    QVERIFY(membership.value(QStringLiteral("timestamp")).toDateTime().isValid());
    QCOMPARE(entries.at(1).toMap().value(QStringLiteral("eventKind")).toString(),
             QStringLiteral("m.room.topic"));
}

// A membership row carries its action as a closed set beside the sentence, so
// the per-action glyph is not parsed out of a translated sentence.
void StateActivityGroupingTest::aMembershipEntryCarriesWhatItDid()
{
    TimelineEvent joined = makeStateChange(
        QStringLiteral("$join"), QStringLiteral("Bob joined the room."),
        QStringLiteral("membership"), QStringLiteral("Bob"));
    joined.membershipChange = QStringLiteral("joined");
    TimelineEvent banned = makeStateChange(
        QStringLiteral("$ban"), QStringLiteral("Alice banned Bob."),
        QStringLiteral("membership"), QStringLiteral("Bob"));
    banned.membershipChange = QStringLiteral("banned");
    // A membership change the bridge could not classify. It must stay
    // UNKNOWN rather than defaulting to anything: a wrong glyph is a wrong
    // claim about what somebody did.
    TimelineEvent odd = makeStateChange(
        QStringLiteral("$odd"), QStringLiteral("Membership for Bob changed."),
        QStringLiteral("membership"), QStringLiteral("Bob"));
    m_client->mirror = { joined, banned, odd };
    m_model->setRoomId(kRoom);

    const QVariantList entries =
        m_model->data(m_model->index(0),
                      TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).toMap()
                 .value(QStringLiteral("membershipChange")).toString(),
             QStringLiteral("joined"));
    QCOMPARE(entries.at(1).toMap()
                 .value(QStringLiteral("membershipChange")).toString(),
             QStringLiteral("banned"));
    QVERIFY2(entries.at(2).toMap()
                 .value(QStringLiteral("membershipChange")).toString().isEmpty(),
             "an unclassified membership change was given an action it does "
             "not have");
    // The sentence is untouched: this is BESIDE it, never instead of it.
    QCOMPARE(entries.at(0).toMap()
                 .value(QStringLiteral("description")).toString(),
             QStringLiteral("Bob joined the room."));
}

// A date divider ends a state run: each day's activity leads its own group
// under its own date. Read markers and the timeline-start row stay
// transparent (the two cases below).
void StateActivityGroupingTest::dateDividerSplitsTheRunIntoDailyGroups()
{
    m_client->mirror = {
        onDay(makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")), 0),
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$div0")),
        onDay(makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")), 1),
    };
    m_model->setRoomId(kRoom);

    // The divider itself is not state activity and is not swallowed into
    // either row's group.
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::IsStateActivityRole).toBool(), false);

    // TWO groups: each state row leads its own day.
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString()
            != m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString());
    QCOMPARE(m_model->stateGroupCount(), 2);

    // Each leader reports only its own day's entries.
    const QVariantList first =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("first"));
    const QVariantList second =
        m_model->data(m_model->index(2), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("second"));
}

void StateActivityGroupingTest::multiDayRunYieldsOneGroupPerDay()
{
    // Three days of pure state churn, divider-separated as the SDK delivers
    // them: four state rows, three groups, one per calendar day.
    m_client->mirror = {
        onDay(makeStateChange(QStringLiteral("$a0"), QStringLiteral("a0")), 0),
        onDay(makeStateChange(QStringLiteral("$a1"), QStringLiteral("a1")), 0),
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$d1")),
        onDay(makeStateChange(QStringLiteral("$b0"), QStringLiteral("b0")), 1),
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$d2")),
        onDay(makeStateChange(QStringLiteral("$c0"), QStringLiteral("c0")), 2),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->stateActivityRowCount(), 4);
    QCOMPARE(m_model->stateGroupCount(), 3);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 2);
    QCOMPARE(m_model->data(m_model->index(3), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 1);
    QCOMPARE(m_model->data(m_model->index(5), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 1);
}

void StateActivityGroupingTest::aDividerLeadingOnlyActivityStillIntroducesVisibleContent()
{
    // With the run split at the divider, the day's first state row leads a
    // group and draws its summary, so the divider above it keeps its date.
    m_client->mirror = {
        onDay(makeStateChange(QStringLiteral("$s0"), QStringLiteral("yesterday")), 0),
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$div0")),
        onDay(makeStateChange(QStringLiteral("$s1"), QStringLiteral("today")), 1),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(1),
                           TimelineModel::DividerIntroducesVisibleContentRole)
                 .toBool(),
             true);
}

void StateActivityGroupingTest::readMarkerDoesNotSplitGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeVirtual(TimelineEvent::ReadMarker, QStringLiteral("$rm0")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString(),
             m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString());
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 2);
}

void StateActivityGroupingTest::timelineStartDoesNotSplitGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeVirtual(TimelineEvent::TimelineStart, QStringLiteral("$start")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString(),
             m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString());
}

void StateActivityGroupingTest::visibleTextMessageBreaksGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeMessage(QStringLiteral("$m0"), QStringLiteral("hello")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QVERIFY(m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString()
            != m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString());

    const QVariantList firstEntries =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(firstEntries.size(), 1);
    QCOMPARE(firstEntries.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("first"));
}

void StateActivityGroupingTest::undecryptableEventIsNotRoutineActivity()
{
    TimelineEvent warning = makeMessage(QStringLiteral("$utd"), QString{});
    warning.type = TimelineEvent::Unknown;
    warning.isEncrypted = true;
    warning.undecryptable = true;
    warning.errorKind = QStringLiteral("no_key");
    m_client->mirror = { warning };
    m_model->setRoomId(kRoom);

    const QModelIndex idx = m_model->index(0);
    QCOMPARE(m_model->data(idx, TimelineModel::IsStateActivityRole).toBool(),
             false);
    QCOMPARE(m_model->data(idx, TimelineModel::UndecryptableRole).toBool(),
             true);
}

void StateActivityGroupingTest::untypedStateNotificationRemainsVisible()
{
    TimelineEvent call = makeStateChange(QStringLiteral("$call"),
                                         QStringLiteral("call event"),
                                         QString{});
    m_client->mirror = { call };
    m_model->setRoomId(kRoom);

    const QModelIndex idx = m_model->index(0);
    QCOMPARE(m_model->data(idx, TimelineModel::IsStateActivityRole).toBool(),
             true);
    QCOMPARE(m_model->data(idx, TimelineModel::IsRoutineActivityRole).toBool(),
             false);
}

void StateActivityGroupingTest::imageMessageBreaksGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeMessage(QStringLiteral("$m0"), QStringLiteral("cat.png"), TimelineEvent::Image),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    QVERIFY(m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString()
            != m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString());
}

void StateActivityGroupingTest::fileMessageBreaksGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeMessage(QStringLiteral("$m0"), QStringLiteral("notes.pdf"), TimelineEvent::File),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    QVERIFY(m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString()
            != m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString());
}

void StateActivityGroupingTest::nonLeaderRowsAreEmptyAndNotLeader()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
        makeStateChange(QStringLiteral("$s2"), QStringLiteral("third")),
    };
    m_model->setRoomId(kRoom);

    for (int row : { 1, 2 }) {
        QCOMPARE(m_model->data(m_model->index(row), TimelineModel::StateGroupLeaderRole).toBool(),
                 false);
        QVERIFY(m_model->data(m_model->index(row), TimelineModel::StateGroupEntriesRole)
                    .toList().isEmpty());
    }
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 3);
}

void StateActivityGroupingTest::groupIdPrefersItemIdOverEventId()
{
    m_client->mirror = { makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")) };
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString(),
             QStringLiteral("uid-$s0"));
}

void StateActivityGroupingTest::appendingContiguousStateChangeExtendsGroupForward()
{
    m_client->mirror = { makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")) };
    m_model->setRoomId(kRoom);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 1);

    const auto next = makeStateChange(QStringLiteral("$s1"), QStringLiteral("second"));
    m_client->mirror.append(next);
    Q_EMIT m_client->eventAppended(kRoom, next);

    // A batch arriving separately from the SDK must still merge into the
    // same group as the existing leader, not start a new one.
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::StateGroupIdRole).toString(),
             m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString());
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole)
                 .toList().size(), 2);
}

void StateActivityGroupingTest::prependingOlderStateChangesExtendsGroupBackward()
{
    m_client->mirror = { makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")) };
    m_model->setRoomId(kRoom);

    const QList<TimelineEvent> older = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
    };
    m_client->mirror.prepend(older.first());
    Q_EMIT m_client->eventsPrepended(kRoom, older);

    // A pagination boundary must not leave two separate one-entry groups.
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::StateGroupLeaderRole).toBool(), false);
    const QVariantList entries =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("first"));
    QCOMPARE(entries.at(1).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("second"));
}

void StateActivityGroupingTest::groupCountAlwaysMatchesAccessibleChildren()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeVirtual(TimelineEvent::ReadMarker, QStringLiteral("$read")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    int groupedStateRows = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->data(m_model->index(row), TimelineModel::IsStateActivityRole).toBool())
            ++groupedStateRows;
    }
    const QVariantList children =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(children.size(), groupedStateRows);
    QVERIFY(!children.isEmpty());
    for (const QVariant &child : children)
        QVERIFY(!child.toMap().value(QStringLiteral("description")).toString().isEmpty());
}

void StateActivityGroupingTest::everyDayOfAMultiDayRunOwnsItsGroupAndItsDate()
{
    // The run breaks at every date divider: no divider is an orphan and no
    // summary spans days its date separator does not own.
    QList<TimelineEvent> mirror;
    mirror.append(onDay(makeVirtual(TimelineEvent::DateDivider,
                                    QStringLiteral("$divA")), 0));
    mirror.append(onDay(makeMessage(QStringLiteral("$before"),
                                    QStringLiteral("before")), 0));

    // 51 state updates spread across days 1..3, one divider per day.
    int made = 0;
    for (int day = 1; day <= 3; ++day) {
        mirror.append(onDay(makeVirtual(TimelineEvent::DateDivider,
                                        QStringLiteral("$div%1").arg(day)), day));
        for (int i = 0; i < 17; ++i) {
            mirror.append(onDay(makeStateChange(
                QStringLiteral("$s%1").arg(made),
                QStringLiteral("update %1").arg(made),
                QStringLiteral("membership"), QStringLiteral("Bob")), day));
            ++made;
        }
    }
    QCOMPARE(made, 51);

    mirror.append(onDay(makeVirtual(TimelineEvent::DateDivider,
                                    QStringLiteral("$divAfter")), 4));
    mirror.append(onDay(makeMessage(QStringLiteral("$after"),
                                    QStringLiteral("after")), 4));
    m_client->mirror = mirror;
    m_model->setRoomId(kRoom);

    // THREE summaries — one per calendar day — because a date divider ends
    // the run.
    int leaders = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->data(m_model->index(row),
                          TimelineModel::IsStateActivityRole).toBool()
            && m_model->data(m_model->index(row),
                             TimelineModel::StateGroupLeaderRole).toBool())
            ++leaders;
    }
    QCOMPARE(leaders, 3);

    // Each answer is read at a known row and guarded by that row's id, so a
    // drift in the fixture fails instead of moving an assertion to another
    // row (an invalid QVariant equals neither true nor false).
    const auto dividerAt = [this](int row, const QString &eventId) {
        const QModelIndex idx = m_model->index(row);
        if (m_model->data(idx, TimelineModel::EventIdRole).toString() != eventId)
            return QVariant();
        return m_model->data(
            idx, TimelineModel::DividerIntroducesVisibleContentRole);
    };

    // EVERY day's divider introduces that day's own summary — no orphans,
    // and no suppressed dates.
    QCOMPARE(dividerAt(2, QStringLiteral("$div1")), QVariant(true));
    QCOMPARE(dividerAt(20, QStringLiteral("$div2")), QVariant(true));
    QCOMPARE(dividerAt(38, QStringLiteral("$div3")), QVariant(true));
    // The ordinary message boundaries on either side still work.
    QCOMPARE(dividerAt(0, QStringLiteral("$divA")), QVariant(true));
    QCOMPARE(dividerAt(56, QStringLiteral("$divAfter")), QVariant(true));

    // Each leader renders only ITS day: 17 entries, all on one calendar
    // day, so the summary sits truthfully under its own date separator and
    // never needs a range subtitle.
    const QList<int> leaderRows = { 3, 21, 39 }; // first state row after each divider
    const QStringList leaderIds = { QStringLiteral("$s0"),
                                    QStringLiteral("$s17"),
                                    QStringLiteral("$s34") };
    for (int i = 0; i < leaderRows.size(); ++i) {
        const int leaderRow = leaderRows.at(i);
        QCOMPARE(m_model->data(m_model->index(leaderRow),
                               TimelineModel::EventIdRole).toString(),
                 leaderIds.at(i));
        QCOMPARE(m_model->data(m_model->index(leaderRow),
                               TimelineModel::StateGroupLeaderRole).toBool(),
                 true);
        const QVariantList entries =
            m_model->data(m_model->index(leaderRow),
                          TimelineModel::StateGroupEntriesRole).toList();
        QCOMPARE(entries.size(), 17);
        const QDateTime first = entries.first().toMap()
                                    .value(QStringLiteral("timestamp")).toDateTime();
        const QDateTime last = entries.last().toMap()
                                   .value(QStringLiteral("timestamp")).toDateTime();
        QVERIFY(first.isValid() && last.isValid());
        QCOMPARE(first.date(), last.date());
    }
}

void StateActivityGroupingTest::hiddenRoutineActivityLeavesItsDividerWithNothingToIntroduce()
{
    m_client->mirror = {
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$div0")),
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("joined"),
                        QStringLiteral("membership"), QStringLiteral("Bob")),
        makeVirtual(TimelineEvent::ReadMarker, QStringLiteral("$read")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("left"),
                        QStringLiteral("membership"), QStringLiteral("Bob")),
    };
    m_model->setRoomId(kRoom);
    const QModelIndex divider = m_model->index(0);

    // Preference on: the group's leader draws its summary under this date.
    QCOMPARE(m_model->showRoomActivity(), true);
    QCOMPARE(m_model->data(divider,
                           TimelineModel::DividerIntroducesVisibleContentRole)
                 .toBool(),
             true);

    // Preference off: every row in the run is routine and hidden, so the
    // divider introduces nothing. A read marker is not content either.
    m_model->setShowRoomActivity(false);
    QCOMPARE(m_model->data(divider,
                           TimelineModel::DividerIntroducesVisibleContentRole)
                 .toBool(),
             false);

    // A non-routine state row (an untyped call/RTC notification) is never
    // hidden by the preference, so its divider keeps its date.
    m_client->mirror.append(makeStateChange(QStringLiteral("$call"),
                                            QStringLiteral("call event"),
                                            QString{}));
    Q_EMIT m_client->eventAppended(kRoom, m_client->mirror.last());
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::DividerIntroducesVisibleContentRole)
                 .toBool(),
             false);
    // ... but only when it LEADS a group: this one joins the existing run,
    // whose hidden leader draws the summary, so nothing new appears.
    QCOMPARE(m_model->data(m_model->index(4),
                           TimelineModel::StateGroupLeaderRole).toBool(),
             false);
}

void StateActivityGroupingTest::showRoomActivityFlipReAnnouncesTheDividerRows()
{
    m_client->mirror = {
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$div0")),
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("joined"),
                        QStringLiteral("membership"), QStringLiteral("Bob")),
    };
    m_model->setRoomId(kRoom);

    QSignalSpy dataSpy(m_model, &QAbstractItemModel::dataChanged);
    QSignalSpy settingSpy(m_model, &TimelineModel::showRoomActivityChanged);
    m_model->setShowRoomActivity(false);

    QCOMPARE(settingSpy.count(), 1);
    QVERIFY(!dataSpy.isEmpty());
    bool coveredDivider = false;
    for (const QList<QVariant> &emitted : dataSpy) {
        const int first = emitted.at(0).value<QModelIndex>().row();
        const int last = emitted.at(1).value<QModelIndex>().row();
        const QList<int> roles = emitted.at(2).value<QList<int>>();
        if (first <= 0 && last >= 0
            && roles.contains(
                   TimelineModel::DividerIntroducesVisibleContentRole))
            coveredDivider = true;
    }
    // The refresh must reach the divider row through the existing
    // presentation-grouping path.
    QVERIFY(coveredDivider);

    // Idempotent: writing the same value again is not a refresh.
    dataSpy.clear();
    m_model->setShowRoomActivity(false);
    QCOMPARE(settingSpy.count(), 1);
    QVERIFY(dataSpy.isEmpty());
}

QTEST_GUILESS_MAIN(StateActivityGroupingTest)
#include "StateActivityGroupingTest.moc"
