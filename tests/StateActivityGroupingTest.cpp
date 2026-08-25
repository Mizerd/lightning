// 0.5.14 checkpoint 2: room-activity (state-change) grouping must be
// transparent through virtual SDK rows (date dividers, read markers, the
// timeline-start marker) — matrix-sdk-ui freely interleaves those between
// real events, and they must not fragment one logical activity group into
// several. Only a visible message/media event actually ends a group.

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
    void aCallBreaksTheActivityRunAroundIt();
    void consecutiveStateChangesFormOneGroup();
    void exposesTypedMembershipAndRoomStateEntries();
    void dateDividerDoesNotSplitGroup();
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
    // v0.7.4 (C4): a date divider must not draw when everything it
    // introduces is hidden.
    void dividersInsideOneCollapsedRunAreOrphansAndReportNoContent();
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

// THE REPORTED ROW. A call arrived as a state event whose kind was "m.call"
// and whose body was the literal words "call event", so MessageDelegate hosted
// it in RoomActivityDelegate and it drew "1 room update" expanding to "call
// event" — reported as "also room event look bleak".
//
// ON THE UNFIXED TREE this whole case fails at its first line: IsCallEventRole
// does not exist, isStateActivity is TRUE, and stateGroupEntriesFrom(0) yields
// one entry whose description is the row's body.
//
// It is written against the LEGACY shape (a StateChange carrying stateKind
// "m.call") deliberately: that is what a cached row and the mock/HTTP backends
// still produce, so this pins that those render identically to the new typed
// row rather than falling back into the activity group.
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

// A call is content, so annotations either side of it are TWO groups, not one
// group with a call swallowed in the middle. ON THE UNFIXED TREE this is one
// group of three entries whose middle row reads "call event".
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

void StateActivityGroupingTest::dateDividerDoesNotSplitGroup()
{
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), QStringLiteral("first")),
        makeVirtual(TimelineEvent::DateDivider, QStringLiteral("$div0")),
        makeStateChange(QStringLiteral("$s1"), QStringLiteral("second")),
    };
    m_model->setRoomId(kRoom);

    // The divider itself is not state activity and is not swallowed into
    // either row's group.
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::IsStateActivityRole).toBool(), false);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::StateGroupLeaderRole).toBool(), true);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupLeaderRole).toBool(), false);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StateGroupIdRole).toString(),
             m_model->data(m_model->index(0), TimelineModel::StateGroupIdRole).toString());

    const QVariantList entries =
        m_model->data(m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("first"));
    QCOMPARE(entries.at(1).toMap().value(QStringLiteral("description")).toString(),
             QStringLiteral("second"));
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

void StateActivityGroupingTest::dividersInsideOneCollapsedRunAreOrphansAndReportNoContent()
{
    // The reported defect: a long run of membership churn spread over
    // several days draws ONE collapsed summary, but the SDK still emits a
    // date divider at every day boundary inside it — so the timeline showed
    // a stack of bare date labels introducing nothing at all.
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

    // Collapsed: exactly ONE summary for the whole 51-row run, because a
    // group is transparent through the dividers between its rows.
    int leaders = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->data(m_model->index(row),
                          TimelineModel::IsStateActivityRole).toBool()
            && m_model->data(m_model->index(row),
                             TimelineModel::StateGroupLeaderRole).toBool())
            ++leaders;
    }
    QCOMPARE(leaders, 1);

    // The fixture's layout is explicit, so each answer is read at a KNOWN
    // row and guarded by that row's own id: a drift in the fixture must
    // fail the test, never quietly move an assertion onto another row.
    // Returning an invalid QVariant on a mismatch does exactly that,
    // because neither QVariant(true) nor QVariant(false) equals it.
    const auto dividerAt = [this](int row, const QString &eventId) {
        const QModelIndex idx = m_model->index(row);
        if (m_model->data(idx, TimelineModel::EventIdRole).toString() != eventId)
            return QVariant();
        return m_model->data(
            idx, TimelineModel::DividerIntroducesVisibleContentRole);
    };

    // Day 1's divider introduces the group's leader — its summary is drawn
    // there, so the date belongs.
    QCOMPARE(dividerAt(2, QStringLiteral("$div1")), QVariant(true));
    // Days 2 and 3 introduce only NON-leader rows of that same collapsed
    // group: nothing is drawn, so the label is an orphan.
    QCOMPARE(dividerAt(20, QStringLiteral("$div2")), QVariant(false));
    QCOMPARE(dividerAt(38, QStringLiteral("$div3")), QVariant(false));
    // The ordinary message boundaries on either side still work.
    QCOMPARE(dividerAt(0, QStringLiteral("$divA")), QVariant(true));
    QCOMPARE(dividerAt(56, QStringLiteral("$divAfter")), QVariant(true));

    // Expanded, the leader renders the whole run itself — which is why the
    // inner dividers have nothing to introduce, and why the summary needs a
    // date RANGE rather than a single day. The entries carry the timestamps
    // that range is built from.
    const int leaderRow = 3;   // the first state row, right after $div1
    QCOMPARE(m_model->data(m_model->index(leaderRow),
                           TimelineModel::EventIdRole).toString(),
             QStringLiteral("$s0"));
    QCOMPARE(m_model->data(m_model->index(leaderRow),
                           TimelineModel::StateGroupLeaderRole).toBool(), true);
    const QVariantList entries =
        m_model->data(m_model->index(leaderRow),
                      TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 51);
    const QDateTime first =
        entries.first().toMap().value(QStringLiteral("timestamp")).toDateTime();
    const QDateTime last =
        entries.last().toMap().value(QStringLiteral("timestamp")).toDateTime();
    QVERIFY(first.isValid() && last.isValid());
    QVERIFY(first.date() != last.date());
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
    // The refresh must reach the divider row THROUGH the existing
    // presentation-grouping path — a role nothing re-announces is a role
    // that silently keeps its old answer in every live view.
    QVERIFY(coveredDivider);

    // Idempotent: writing the same value again is not a refresh.
    dataSpy.clear();
    m_model->setShowRoomActivity(false);
    QCOMPARE(settingSpy.count(), 1);
    QVERIFY(dataSpy.isEmpty());
}

QTEST_GUILESS_MAIN(StateActivityGroupingTest)
#include "StateActivityGroupingTest.moc"
