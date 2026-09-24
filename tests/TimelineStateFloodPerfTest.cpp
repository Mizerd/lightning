// Measures the cost of state-activity grouping (TimelineModel::
// stateGroupLeaderRow / stateGroupEntriesFrom / emitPresentationGroupingChanged)
// under a long run of contiguous state changes, against the same row count of
// ordinary messages, at the TimelineModel level only.
// TimelineStateFloodQmlPerfTest.cpp covers the QML side.
//
// Only the group leader's StateGroupEntriesRole query builds the entries
// list; every other row returns an empty list in O(1). So hydrating N rows
// once is linear. emitPresentationGroupingChanged widens dataChanged() to the
// whole contiguous run on every insertion, so building a group of n one
// append at a time costs O(n²) entries in total.
//
// Metrics are observable: "entries produced" is the size of the list the
// entries role returns, and range widths are read off the real dataChanged()
// signal.

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QElapsedTimer>
#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!flood:example.org");

TimelineEvent makeStateChange(const QString &eventId, int n)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = QStringLiteral("Alice changed their display name to Alice%1.").arg(n);
    e.senderDisplayName = QStringLiteral("Alice");
    e.stateKind = QStringLiteral("membership");
    e.type = TimelineEvent::StateChange;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000 + n);
    return e;
}

TimelineEvent makeMessage(const QString &eventId, int n)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = QStringLiteral("ordinary message %1").arg(n);
    e.type = TimelineEvent::TextMessage;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000 + n);
    return e;
}

// Minimal scripted backend, matching the pattern in StateActivityGroupingTest.cpp
// and TimelineModelDiffTest.cpp.
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

// Sums TimelineModel::data(row, StateGroupEntriesRole).toList().size() over
// [first, last]. Only the range's group leader contributes a nonzero size.
qint64 replayEntriesRoleOverRange(TimelineModel *model, int first, int last)
{
    qint64 total = 0;
    for (int row = first; row <= last; ++row) {
        total += model->data(model->index(row),
                              TimelineModel::StateGroupEntriesRole)
                     .toList()
                     .size();
    }
    return total;
}

} // namespace

class TimelineStateFloodPerfTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    // One-time hydration cost is linear in row count for a contiguous state
    // group (only the leader does real work) and zero for messages.
    void hydrationCostIsLinearForStateGroupsAndForMessages();

    // The composition counters the opt-in scroll trace reports, which tell a
    // state-flood room from a merely long one. Counts only, never ids, bodies
    // or senders.
    void compositionCountersReportStateRowsAndGroups();

    // emitPresentationGroupingChanged widens dataChanged() to the whole
    // contiguous state run on every insertion, so building a group of n one
    // row at a time costs O(n²) in total.
    void perAppendDataChangedRangeCoversWholeContiguousGroup();
    void cumulativeAppendCostGrowsQuadraticallyForStateChanges();
    void perAppendCostIsConstantForOrdinaryMessages();

    // The same under batched prepends, as backward pagination delivers them:
    // each page's dataChanged() range re-covers the entire accumulated group.
    void perBatchPrependReplaysWholeAccumulatedGroupEachPage();

private:
    FakeClient *m_client = nullptr;
    TimelineModel *m_model = nullptr;
};

void TimelineStateFloodPerfTest::init()
{
    m_client = new FakeClient(this);
    m_model = new TimelineModel(this);
    m_model->setClient(m_client);
}

void TimelineStateFloodPerfTest::cleanup()
{
    delete m_model;
    delete m_client;
    m_model = nullptr;
    m_client = nullptr;
}

void TimelineStateFloodPerfTest::hydrationCostIsLinearForStateGroupsAndForMessages()
{
    // Two sizes at a clean 4x ratio, so a linear mechanism predicts a 4x
    // work ratio, quadratic predicts 16x, cubic 64x — this distinguishes
    // the hypotheses independent of machine speed.
    const int small = 25;
    const int large = 100;

    // Returns total entries produced, or -1 on a fixture mismatch. Not
    // QCOMPARE/QVERIFY inside: those macros expand to a bare `return;`,
    // which does not compile in a lambda whose deduced return type is
    // qint64. Callers check the sentinel.
    auto hydrationCost = [this](int n, bool stateChanges) -> qint64 {
        QList<TimelineEvent> events;
        events.reserve(n);
        for (int i = 0; i < n; ++i) {
            events.append(stateChanges
                              ? makeStateChange(QStringLiteral("$s%1").arg(i), i)
                              : makeMessage(QStringLiteral("$m%1").arg(i), i));
        }
        m_client->mirror = events;
        m_model->setRoomId(kRoom);
        if (m_model->rowCount() != n) {
            qWarning("fixture: expected %d rows, got %d", n, m_model->rowCount());
            return qint64(-1);
        }

        QElapsedTimer timer;
        timer.start();
        // What N freshly created MessageDelegate rows each cost once via their
        // grouping bindings.
        qint64 totalEntries = 0;
        for (int row = 0; row < n; ++row) {
            m_model->data(m_model->index(row), TimelineModel::StateGroupLeaderRole);
            m_model->data(m_model->index(row), TimelineModel::StateGroupIdRole);
            totalEntries += m_model->data(m_model->index(row),
                                          TimelineModel::StateGroupEntriesRole)
                                .toList()
                                .size();
        }
        const qint64 elapsedNs = timer.nsecsElapsed();
        qInfo("hydration n=%d stateChanges=%d totalEntriesProduced=%lld elapsedNs=%lld",
              n, stateChanges ? 1 : 0, static_cast<long long>(totalEntries),
              static_cast<long long>(elapsedNs));
        return totalEntries;
    };

    const qint64 stateSmall = hydrationCost(small, true);
    cleanup();
    init();
    const qint64 stateLarge = hydrationCost(large, true);
    cleanup();
    init();
    const qint64 msgSmall = hydrationCost(small, false);
    cleanup();
    init();
    const qint64 msgLarge = hydrationCost(large, false);

    QVERIFY2(stateSmall >= 0 && stateLarge >= 0 && msgSmall >= 0 && msgLarge >= 0,
             "fixture did not load the expected row count");

    // One contiguous group of size n: only the leader (row 0) contributes,
    // producing exactly n entries once. Total across all n row-queries = n.
    QCOMPARE(stateSmall, qint64(small));
    QCOMPARE(stateLarge, qint64(large));
    // Ordinary messages never touch stateGroupEntriesFrom (stateGroupLeaderRow
    // returns -1 in O(1) for a non-StateChange row) — zero entries at any n.
    QCOMPARE(msgSmall, qint64(0));
    QCOMPARE(msgLarge, qint64(0));

    const double stateRatio = double(stateLarge) / double(qMax<qint64>(1, stateSmall));
    qInfo("hydration work ratio (4x row count): stateChanges=%.1f (linear predicts 4, quadratic 16, cubic 64)",
          stateRatio);
    QCOMPARE(stateRatio, 4.0); // exact for this closed-form quantity.
}

void TimelineStateFloodPerfTest::perAppendDataChangedRangeCoversWholeContiguousGroup()
{
    const int n = 40;
    m_client->mirror = { makeStateChange(QStringLiteral("$s0"), 0) };
    m_model->setRoomId(kRoom);

    int lastFirst = -1, lastLast = -1;
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [&](const QModelIndex &tl, const QModelIndex &br, const QList<int> &roles) {
                if (!roles.contains(TimelineModel::StateGroupEntriesRole))
                    return;
                lastFirst = tl.row();
                lastLast = br.row();
            });

    for (int i = 1; i < n; ++i) {
        lastFirst = lastLast = -1;
        const auto next = makeStateChange(QStringLiteral("$s%1").arg(i), i);
        m_client->mirror.append(next);
        Q_EMIT m_client->eventAppended(kRoom, next);

        // The group is rows [0, i] after this append: the signal spans the
        // whole group, not just the touched row(s).
        QCOMPARE(lastFirst, 0);
        QCOMPARE(lastLast, i);
    }
}

void TimelineStateFloodPerfTest::cumulativeAppendCostGrowsQuadraticallyForStateChanges()
{
    const int n = 100;
    m_client->mirror = { makeStateChange(QStringLiteral("$s0"), 0) };
    m_model->setRoomId(kRoom);

    qint64 totalReplayedEntries = 0;
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [&](const QModelIndex &tl, const QModelIndex &br, const QList<int> &roles) {
                if (!roles.contains(TimelineModel::StateGroupEntriesRole))
                    return;
                // What every row's stateGroupEntries binding in the range
                // re-evaluates to on this signal; only the leader contributes.
                totalReplayedEntries +=
                    replayEntriesRoleOverRange(m_model, tl.row(), br.row());
            });

    QElapsedTimer timer;
    timer.start();
    for (int i = 1; i < n; ++i) {
        const auto next = makeStateChange(QStringLiteral("$s%1").arg(i), i);
        m_client->mirror.append(next);
        Q_EMIT m_client->eventAppended(kRoom, next);
    }
    const qint64 elapsedNs = timer.nsecsElapsed();

    // Closed form for appending events 1..n-1 into a group that already has
    // one leader row (sizes 2..n after each append): the leader's list is
    // rebuilt once per append, each time at the CURRENT group size k:
    // sum_{k=2}^{n} k.
    qint64 expected = 0;
    for (qint64 k = 2; k <= n; ++k)
        expected += k;

    qInfo("incremental append n=%d totalReplayedEntries=%lld expected=%lld elapsedNs=%lld",
          n, static_cast<long long>(totalReplayedEntries),
          static_cast<long long>(expected), static_cast<long long>(elapsedNs));
    QCOMPARE(totalReplayedEntries, expected);

    // n=100 gives 5,049: quadratic, but only a few milliseconds of wall time.
    // The QML-side cost of the widened range is measured in
    // TimelineStateFloodQmlPerfTest.cpp.
    QVERIFY(totalReplayedEntries > qint64(n) * 10);
}

void TimelineStateFloodPerfTest::perAppendCostIsConstantForOrdinaryMessages()
{
    const int n = 100;
    m_client->mirror = { makeMessage(QStringLiteral("$m0"), 0) };
    m_model->setRoomId(kRoom);

    qint64 totalReplayedEntries = 0;
    int widestRange = 0;
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [&](const QModelIndex &tl, const QModelIndex &br, const QList<int> &roles) {
                if (!roles.contains(TimelineModel::StateGroupEntriesRole))
                    return;
                widestRange = qMax(widestRange, br.row() - tl.row() + 1);
                totalReplayedEntries +=
                    replayEntriesRoleOverRange(m_model, tl.row(), br.row());
            });

    for (int i = 1; i < n; ++i) {
        const auto next = makeMessage(QStringLiteral("$m%1").arg(i), i);
        m_client->mirror.append(next);
        Q_EMIT m_client->eventAppended(kRoom, next);
    }

    qInfo("incremental append (ordinary messages) n=%d totalReplayedEntries=%lld widestRange=%d",
          n, static_cast<long long>(totalReplayedEntries), widestRange);
    // Control: no state changes, so zero entries and the range never widens
    // beyond the two rows a single append touches.
    QCOMPARE(totalReplayedEntries, qint64(0));
    QVERIFY(widestRange <= 2);
}

void TimelineStateFloodPerfTest::perBatchPrependReplaysWholeAccumulatedGroupEachPage()
{
    // Older pages arrive prepended; 20 is a representative page size.
    const int pageSize = 20;
    const int pages = 5; // 100 total state changes, none of them messages.

    m_client->mirror = {};
    m_model->setRoomId(kRoom);

    qint64 totalReplayedEntries = 0;
    QList<int> observedRangeWidths;
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [&](const QModelIndex &tl, const QModelIndex &br, const QList<int> &roles) {
                if (!roles.contains(TimelineModel::StateGroupEntriesRole))
                    return;
                observedRangeWidths.append(br.row() - tl.row() + 1);
                totalReplayedEntries +=
                    replayEntriesRoleOverRange(m_model, tl.row(), br.row());
            });

    int loaded = 0;
    for (int page = 0; page < pages; ++page) {
        // onEventsPrepended expects `events` oldest-first; one batch per page.
        QList<TimelineEvent> older;
        older.reserve(pageSize);
        for (int i = 0; i < pageSize; ++i)
            older.append(makeStateChange(
                QStringLiteral("$p%1_%2").arg(page).arg(i), page * pageSize + i));
        for (auto it = older.crbegin(); it != older.crend(); ++it)
            m_client->mirror.prepend(*it);
        loaded += pageSize;
        Q_EMIT m_client->eventsPrepended(kRoom, older);

        // Every page's dataChanged range covers the full accumulated group,
        // not just the new page.
        QVERIFY(!observedRangeWidths.isEmpty());
        QCOMPARE(observedRangeWidths.last(), loaded);
    }

    qInfo("paginated batches pages=%d pageSize=%d totalLoaded=%d totalReplayedEntries=%lld",
          pages, pageSize, loaded, static_cast<long long>(totalReplayedEntries));

    // Closed form: after page p (1-indexed), the group has p*pageSize rows;
    // the leader's list is rebuilt once per page, at that page's group size:
    // sum_{p=1}^{pages} p*pageSize.
    qint64 expected = 0;
    for (int p = 1; p <= pages; ++p)
        expected += qint64(p) * pageSize;
    QCOMPARE(totalReplayedEntries, expected);
}

void TimelineStateFloodPerfTest::compositionCountersReportStateRowsAndGroups()
{
    // Two separated runs of state changes with a message between them, plus
    // trailing messages: 3 state rows in 2 groups.
    m_client->mirror = {
        makeStateChange(QStringLiteral("$s0"), 0),
        makeStateChange(QStringLiteral("$s1"), 1),
        makeMessage(QStringLiteral("$m0"), 0),
        makeStateChange(QStringLiteral("$s2"), 2),
        makeMessage(QStringLiteral("$m1"), 1),
    };
    m_model->setRoomId(kRoom);
    QCoreApplication::processEvents();

    QCOMPARE(m_model->stateActivityRowCount(), 3);
    QCOMPARE(m_model->stateGroupCount(), 2);

    // A timeline with no state activity reports zero, not -1: -1 is reserved
    // by the trace for "could not be determined here", which is a different
    // claim from "there are none".
    m_client->mirror = {
        makeMessage(QStringLiteral("$m0"), 0),
        makeMessage(QStringLiteral("$m1"), 1),
    };
    m_model->setRoomId(QString());
    m_model->setRoomId(kRoom);
    QCoreApplication::processEvents();
    QCOMPARE(m_model->stateActivityRowCount(), 0);
    QCOMPARE(m_model->stateGroupCount(), 0);

    // One long contiguous run is ONE group however many rows it has — the
    // fact that makes a flood report legible.
    QList<TimelineEvent> flood;
    for (int i = 0; i < 50; ++i)
        flood.append(makeStateChange(QStringLiteral("$f%1").arg(i), i));
    m_client->mirror = flood;
    m_model->setRoomId(QString());
    m_model->setRoomId(kRoom);
    QCoreApplication::processEvents();
    QCOMPARE(m_model->stateActivityRowCount(), 50);
    QCOMPARE(m_model->stateGroupCount(), 1);
}

QTEST_GUILESS_MAIN(TimelineStateFloodPerfTest)
#include "TimelineStateFloodPerfTest.moc"
