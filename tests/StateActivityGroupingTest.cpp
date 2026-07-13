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

    void consecutiveStateChangesFormOneGroup();
    void exposesTypedMembershipAndRoomStateEntries();
    void dateDividerDoesNotSplitGroup();
    void readMarkerDoesNotSplitGroup();
    void timelineStartDoesNotSplitGroup();
    void visibleTextMessageBreaksGroup();
    void imageMessageBreaksGroup();
    void fileMessageBreaksGroup();
    void nonLeaderRowsAreEmptyAndNotLeader();
    void groupIdPrefersItemIdOverEventId();
    void appendingContiguousStateChangeExtendsGroupForward();
    void prependingOlderStateChangesExtendsGroupBackward();
    void groupCountAlwaysMatchesAccessibleChildren();

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

QTEST_GUILESS_MAIN(StateActivityGroupingTest)
#include "StateActivityGroupingTest.moc"
