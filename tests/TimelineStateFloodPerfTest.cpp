// Measurement for the reported "many consecutive room-activity state
// changes (e.g. a user changing their display name 50-100 times with no
// messages in between) makes scrolling up die" defect.
//
// This is a MEASUREMENT test, not a fix. It quantifies the cost of the
// existing state-activity grouping mechanism (TimelineModel::
// stateGroupLeaderRow / stateGroupEntriesFrom / emitPresentationGroupingChanged,
// see src/models/TimelineModel.cpp) under a long run of contiguous
// m.room.member-style state changes, and contrasts it with the same row
// count of ordinary messages, at the TimelineModel level only (no QML
// engine, no delegate instantiation — see TimelineStateFloodQmlPerfTest.cpp
// for the QML-level follow-up, and the completion report for what each
// file does and does not cover).
//
// CORRECTED (this file's first version over-predicted the cost — see the
// completion report for the wrong reading and how it was found): only the
// GROUP LEADER row's TimelineModel::data(..., StateGroupEntriesRole) query
// actually rebuilds the entries list — every non-leader row short-circuits
// to an empty list in O(1) (`if (leader != raw) return QVariantList{};`,
// TimelineModel.cpp). So:
//   - Hydrating N freshly-queried rows once costs exactly N total entries
//     produced (all from the one leader), not N² — LINEAR, not quadratic.
//   - TimelineModel::emitPresentationGroupingChanged still WIDENS
//     dataChanged() to cover the WHOLE contiguous run on every insertion
//     into it (this part of the original reading was right, and is proven
//     directly below) — but replaying the entries role over that widened
//     range only re-derives real content from the ONE leader row each time,
//     so the cumulative cost of building a group of n one append at a time
//     is O(n²) (an arithmetic series, Σk), not O(n³).
//   - At the measured n=100, that cumulative cost is ~5,049 "entries units"
//     and ~4.5ms of wall time — real, but far too small on its own to
//     explain "scrolling up kinda dies". What the widened dataChanged range
//     DOES still do, regardless of how cheap the C++ call underneath it is,
//     is force every one of the (up to n) already-instantiated
//     MessageDelegate rows in that range to re-evaluate their
//     `model.stateGroupEntries` / `model.stateGroupId` / `model.stateGroupLeader`
//     property bindings (MessageDelegate.qml:50,511,513) on every single
//     insertion — a QML-layer cost this pure-C++ test cannot see. See
//     TimelineStateFloodQmlPerfTest.cpp for that measurement.
//
// Every metric below is an OBSERVABLE proxy for real work, not a guess:
//   - "entries produced" is the exact size of the QVariantList that
//     TimelineModel::data(..., StateGroupEntriesRole) returns.
//   - the dataChanged() range width is read directly off the signal
//     TimelineModel actually emits — the same signal QML's role bindings
//     in MessageDelegate.qml re-evaluate against.

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
// [first, last] — the exact list every state-change row's MessageDelegate
// rebuilds via `model.stateGroupEntries` (MessageDelegate.qml:50). Only the
// range's group leader (if any) actually contributes a nonzero size; every
// other row short-circuits to an empty list in O(1).
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

    // Test 1: one-time hydration cost (what N freshly-created MessageDelegate
    // rows would each pay once, e.g. right after a roomId switch or a batch
    // reset) is LINEAR in row count for a contiguous state group — only the
    // leader row's entries-role query does real work — and stays linear
    // (zero) for the same row count of ordinary messages. This is a
    // regression guard: if a future change makes every row (not just the
    // leader) rebuild the full entries list, this test starts failing with
    // an n² total instead of n.
    void hydrationCostIsLinearForStateGroupsAndForMessages();

    // Test 2: TimelineModel::emitPresentationGroupingChanged widens
    // dataChanged() to cover the WHOLE contiguous state-change run on every
    // single insertion into it, regardless of insertion position — proven
    // directly off the real signal. Because only the leader's entries query
    // does real work, the CUMULATIVE cost of building a group of n state
    // changes one row at a time is O(n²) (an arithmetic series), not O(1)
    // (which a well-behaved incremental design would cost) and not O(n³).
    void perAppendDataChangedRangeCoversWholeContiguousGroup();
    void cumulativeAppendCostGrowsQuadraticallyForStateChanges();
    void perAppendCostIsConstantForOrdinaryMessages();

    // Test 3: the same pathology under batched delivery, matching how
    // backward pagination actually delivers pages (onEventsPrepended /
    // onEventsInsertedAt, one signal per page). Confirms the mechanism is
    // not merely a one-event-at-a-time artifact: even in pages of 20, each
    // new page's dataChanged() range re-covers the ENTIRE accumulated
    // group, not just the new page.
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
        // What N freshly-instantiated MessageDelegate rows each cost once,
        // at creation, via their unconditional
        // `model.stateGroupEntries` / stateGroupId / stateGroupLeader bindings.
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

        // The group is rows [0, i] after this append (i+1 rows). Assert the
        // signal actually observed by QML's role bindings spans the WHOLE
        // group, not just the newly touched row(s) — this is the mechanism,
        // read directly off the real signal, not inferred. MEASURED and
        // confirmed passing against the real implementation.
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
                // What every row's stateGroupEntries binding in the affected
                // range re-evaluates to, exactly as QML would on this signal.
                // Only the leader (row 0, always in range here) contributes
                // a nonzero amount; every other row in the range is an O(1)
                // empty-list re-evaluation this counter does not weight.
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

    // n=100: expected is 5,049 — real, quadratic-in-n cumulative cost (a
    // linear-total mechanism would be flat at ~n=100; this is ~50x that),
    // but measured at ~4.5ms of wall time for the whole 99-append sequence
    // on the machine this was authored on — nowhere near enough on its own
    // to explain a reported scrolling freeze. See
    // TimelineStateFloodQmlPerfTest.cpp for the QML-layer measurement this
    // motivates: the widened dataChanged RANGE (proven above to cover the
    // whole group every time) forces every already-instantiated delegate in
    // it to re-evaluate its grouping property bindings, regardless of how
    // cheap the underlying C++ call is for a non-leader row.
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
    // No row is ever a state change, so stateGroupEntriesFrom's O(1)
    // -1-leader short-circuit applies everywhere: zero entries produced,
    // and the dataChanged range never widens beyond the two rows touched by
    // a single append. MEASURED and confirmed passing — the direct control
    // proving this pathology is specific to state-change grouping, not "any
    // 100 rows are slow".
    QCOMPARE(totalReplayedEntries, qint64(0));
    QVERIFY(widestRange <= 2);
}

void TimelineStateFloodPerfTest::perBatchPrependReplaysWholeAccumulatedGroupEachPage()
{
    // Matches backward pagination: older pages arrive prepended, in the
    // page size the SDK actually returns (representative page size; the
    // real value is backend-controlled and not fixed here).
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
        // TimelineModel::onEventsPrepended expects `events` oldest-first
        // (it internally prepends back-to-front to land them in that
        // order) — matching TimelineModel::onEventsPrepended's real call
        // shape, this is one atomic batch per page.
        QList<TimelineEvent> older;
        older.reserve(pageSize);
        for (int i = 0; i < pageSize; ++i)
            older.append(makeStateChange(
                QStringLiteral("$p%1_%2").arg(page).arg(i), page * pageSize + i));
        for (auto it = older.crbegin(); it != older.crend(); ++it)
            m_client->mirror.prepend(*it);
        loaded += pageSize;
        Q_EMIT m_client->eventsPrepended(kRoom, older);

        // Every page after the first must observe a dataChanged range that
        // covers the FULL accumulated group so far, not just the new page,
        // because the two already-loaded state-change rows immediately
        // adjacent to the insertion point pull emitPresentationGroupingChanged's
        // expand-while-loop across the whole existing run. MEASURED.
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

QTEST_GUILESS_MAIN(TimelineStateFloodPerfTest)
#include "TimelineStateFloodPerfTest.moc"
