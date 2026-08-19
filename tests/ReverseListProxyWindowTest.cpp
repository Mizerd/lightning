// 2026-08-19 scroll round 2: the sliding window in ReverseListProxyModel.
//
// WHY the window exists, measured rather than assumed: the timeline
// instantiates every row it is handed, and per-frame cost scales with the
// TOTAL instantiated item count — a perf capture put scene-graph node sync at
// ~10% and event delivery at ~12% of a frame at 1000 loaded rows, while layout
// was ~1%. Per-notch wheel cost measured 4.44 ms at 600 rows against 10.65 ms
// at 1000. Pacing cannot help: it only delays rows, it never takes any back.
//
// The window is two integers — how many of the NEWEST source rows are excluded
// (`windowSkip`) and how many are exposed — and EVERY transition between two
// windows must be a single insert-or-remove at one end. This suite is where
// that claim is proven, because the model is where an off-by-one silently
// becomes "the reader is looking at the wrong message".
//
// Model conventions under test (they are easy to invert by accident):
//   * source row 0 is the OLDEST event; source row total-1 is the NEWEST;
//   * proxy row 0 is the NEWEST source row the window includes;
//   * windowSkip 0 is the only state in which proxy row 0 is the live edge —
//     the pane must return to it before the reader can reach the bottom.
#include <QAbstractListModel>
#include <QSignalSpy>
#include <QtTest>

#include "models/ReverseListProxyModel.h"

namespace {

// Minimal source: row i carries the string "e<i>", so a proxy row's identity
// is checkable without any timeline machinery.
class FakeSource final : public QAbstractListModel
{
    Q_OBJECT
public:
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole)
            return {};
        return m_rows.at(index.row());
    }
    // Oldest-end insert: what a backward pagination page does.
    void prepend(int n)
    {
        beginInsertRows({}, 0, n - 1);
        for (int i = n - 1; i >= 0; --i)
            m_rows.prepend(QStringLiteral("p%1").arg(i));
        endInsertRows();
    }
    // Newest-end insert: a live message.
    void appendLive(const QString &id)
    {
        beginInsertRows({}, m_rows.size(), m_rows.size());
        m_rows.append(id);
        endInsertRows();
    }
    void removeAt(int row, int n = 1)
    {
        beginRemoveRows({}, row, row + n - 1);
        m_rows.remove(row, n);
        endRemoveRows();
    }
    void seed(int n)
    {
        beginResetModel();
        m_rows.clear();
        for (int i = 0; i < n; ++i)
            m_rows.append(QStringLiteral("e%1").arg(i));
        endResetModel();
    }

private:
    QVector<QString> m_rows;
};

QString proxyText(const ReverseListProxyModel &proxy, int row)
{
    return proxy.data(proxy.index(row, 0), Qt::DisplayRole).toString();
}

// Every exposed row must map to the source row the window says it does, and
// mapFromSource must be its exact inverse. Checked after EVERY mutation: a
// window whose arithmetic drifts is the whole risk of this feature.
void verifyMappingIsConsistent(const ReverseListProxyModel &proxy,
                              const FakeSource &source)
{
    const int rows = proxy.rowCount();
    const int total = source.rowCount();
    for (int r = 0; r < rows; ++r) {
        const QModelIndex src = proxy.mapToSource(proxy.index(r, 0));
        QVERIFY2(src.isValid(),
                 qPrintable(QStringLiteral("proxy row %1 maps nowhere").arg(r)));
        QCOMPARE(src.row(), total - 1 - proxy.windowSkip() - r);
        QCOMPARE(proxy.mapFromSource(src).row(), r);
        // ...and the data agrees, which catches an inverted reversal that
        // index arithmetic alone would not.
        QCOMPARE(proxyText(proxy, r),
                 source.data(src, Qt::DisplayRole).toString());
    }
    if (rows > 0)
        QCOMPARE(proxy.oldestExposedSourceRow(),
                 total - proxy.windowSkip() - rows);
    else
        QCOMPARE(proxy.oldestExposedSourceRow(), -1);
}

} // namespace

class ReverseListProxyWindowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Baseline: with no window the proxy is exactly what it always was —
    // newest-first over the whole source.
    void unwindowedProxyIsNewestFirstOverEverything()
    {
        FakeSource source;
        source.seed(10);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        QCOMPARE(proxy.rowCount(), 10);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e9"));  // newest
        QCOMPARE(proxyText(proxy, 9), QStringLiteral("e0"));  // oldest
        verifyMappingIsConsistent(proxy, source);
    }

    // The window's defining behaviour: skip the newest rows, expose a band.
    void windowExposesABandAndMapsItCorrectly()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        // Skip the 20 newest, expose 30 → source rows 50..79, newest-first.
        proxy.setWindow(/*skipNewest=*/20, /*rows=*/30);
        QCOMPARE(proxy.windowSkip(), 20);
        QCOMPARE(proxy.rowCount(), 30);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e79"));
        QCOMPARE(proxyText(proxy, 29), QStringLiteral("e50"));
        QCOMPARE(proxy.oldestExposedSourceRow(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // Each end must move by a single insert/remove — never a reset, and never
    // a renumbering of rows in the middle. A reset here would rebuild every
    // delegate, which is the cost this whole feature exists to avoid.
    void everyWindowTransitionIsOneInsertOrRemovePerEnd()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(20, 30);

        QSignalSpy reset(&proxy, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);

        // Grow the OLDEST end by 10 (scrolling further up): one insert at the
        // tail, which in the rotated view is the far edge — nothing the reader
        // is looking at can move.
        proxy.setWindow(20, 40);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 30);  // first
        QCOMPARE(inserted.at(0).at(2).toInt(), 39);  // last
        QCOMPARE(proxy.oldestExposedSourceRow(), 40);
        verifyMappingIsConsistent(proxy, source);
        inserted.clear();

        // Release the NEWEST end (reader moved further from the live edge):
        // one remove at the HEAD. This is the only transition that shifts the
        // kept rows, which is why the pane corrects contentY for it.
        proxy.setWindow(35, 40);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 0);
        QCOMPARE(removed.at(0).at(2).toInt(), 14);   // 15 rows dropped
        QCOMPARE(proxy.windowSkip(), 35);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.oldestExposedSourceRow(), 25);
        verifyMappingIsConsistent(proxy, source);
        removed.clear();
        inserted.clear();

        // Reader heads back toward the live edge, keeping the same window
        // SIZE: the band slides, so both ends move — 25 rows released at the
        // oldest end (the tail, free) and 25 restored at the newest end (the
        // head, which the pane corrects contentY for). Exactly one op per
        // end, and still no reset.
        proxy.setWindow(10, 40);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 15);   // tail released
        QCOMPARE(removed.at(0).at(2).toInt(), 39);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);   // head restored
        QCOMPARE(inserted.at(0).at(2).toInt(), 24);
        QCOMPARE(proxy.windowSkip(), 10);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.oldestExposedSourceRow(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // THE stability invariant. While the reader is deep in history the window
    // must keep covering the SAME events as live messages arrive — otherwise
    // every incoming message would slide the reader's content by one row.
    void liveMessagesDoNotSlideAWindowedReader()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);   // source rows 40..59

        const QString topEvent = proxyText(proxy, 0);
        const QString bottomEvent = proxyText(proxy, 19);
        QCOMPARE(topEvent, QStringLiteral("e59"));
        QCOMPARE(bottomEvent, QStringLiteral("e40"));

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        for (int i = 0; i < 5; ++i)
            source.appendLive(QStringLiteral("live%1").arg(i));

        // Nothing entered or left the view...
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 20);
        // ...the window absorbed them by growing its skip...
        QCOMPARE(proxy.windowSkip(), 45);
        // ...and the reader is still looking at exactly the same events.
        QCOMPARE(proxyText(proxy, 0), topEvent);
        QCOMPARE(proxyText(proxy, 19), bottomEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    // Backward pagination while windowed lands entirely older than the window:
    // invisible, and it must not disturb the exposed band.
    void backwardPaginationBelowTheWindowIsInvisible()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        const QString topEvent = proxyText(proxy, 0);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        source.prepend(20);            // 20 older rows at source row 0

        QCOMPARE(inserted.count(), 0); // outside the window
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), topEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    // Pacing must never undo a window. Without the cap the reveal timer would
    // walk the exposed count straight back up to the source total.
    void pacingNeverGrowsPastTheWindow()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);
        QCOMPARE(proxy.rowCount(), 40);

        // Give the reveal timer generous room to misbehave.
        QTest::qWait(400);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.windowSkip(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // Every jump/search path calls releaseAll() before addressing a row. With
    // a window that must also restore the newest end, or a jump to a recent
    // message would resolve to "no such row" — silently doing nothing, the
    // exact failure the pacing backlog already taught this codebase.
    void clearWindowRestoresEverythingIncludingTheLiveEdge()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.rowCount(), 20);

        proxy.clearWindow();
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 100);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e99"));  // live edge back
        verifyMappingIsConsistent(proxy, source);
    }

    // releaseAll() alone (the pre-existing jump path) must also lift the cap,
    // or the first jump after a window would expose nothing further.
    void releaseAllLiftsTheWindowCap()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(0, 40);        // capped at the live edge
        QCOMPARE(proxy.rowCount(), 40);

        proxy.releaseAll();
        QCOMPARE(proxy.rowCount(), 200);
        verifyMappingIsConsistent(proxy, source);
    }

    // Reaching the window's OLD edge must re-expose rows we already hold
    // rather than asking the homeserver for history. Paced, because a
    // synchronous release of a whole margin would build that many delegates
    // in one go (3-7 ms each in the real pane).
    void extendingAtTheOldEndIsPacedAndBoundedByTheRequestedRows()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);
        QCOMPARE(proxy.rowCount(), 40);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        proxy.extendWindowAtOldEnd(30);
        // Paced: not all of it lands synchronously.
        QTRY_COMPARE(proxy.rowCount(), 70);
        // And it STOPS there — generous extra time must not walk it further.
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 70);

        // The newest end must not move: the reader's contentY correction
        // depends on the skip, and this path deliberately performs none.
        QCOMPARE(proxy.windowSkip(), 50);
        // Every insert lands at the TAIL (beyond the reader), never at row 0.
        QVERIFY(inserted.count() > 0);
        for (const QList<QVariant> &args : inserted)
            QVERIFY2(args.at(1).toInt() >= 40,
                     "an extension inserted at the head, which would shift "
                     "every row the reader is looking at");
        verifyMappingIsConsistent(proxy, source);
    }

    // The PACING backlog also leaves rows unexposed, but it releases itself
    // on its own timer and the pane's near-top logic must keep working
    // normally around it. Conflating the two is not hypothetical: the first
    // version of the pane hook asked `rowWindowSkip + count < total`, which
    // is also true during an ordinary initial reveal with no window at all,
    // and it swallowed the near-top request on every such timeline
    // (timeline-pane-qml's nearTopProximityIsMeasuredFromLoadedHistoryNot
    // AbsoluteContentY failed with "an approach to the top must consume the
    // latch"). Only the window's own cap may answer yes.
    void extendingRefusesWhenOnlyThePacingBacklogWithholdsRows()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        // No window: m_windowCap is 0 (uncapped), so anything still unexposed
        // is the paced backlog.
        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);
        QTRY_COMPARE(proxy.rowCount(), 200);   // pacing gets there by itself
        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);

        // And with a window whose cap pacing has not yet reached, it is still
        // pacing's job, not the window's.
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);
        // total=200, skip=50 -> sourceRow = 149 - proxyRow, so the exposed
        // band is source [90, 149]. (200 is past the end and aborts.)
        source.removeAt(100, 20);              // inside the window
        QVERIFY(proxy.rowCount() < 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), false);

        // Once pacing has caught up to the cap, the window is the constraint.
        QTRY_COMPARE(proxy.rowCount(), 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), true);
        QTRY_COMPARE(proxy.rowCount(), 90);
        verifyMappingIsConsistent(proxy, source);
    }

    // The extension stops at the oldest row actually loaded, and asking again
    // once everything is out is a no-op rather than an unbounded reveal.
    void extendingAtTheOldEndStopsAtTheOldestLoadedRow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(0, 40);
        QCOMPARE(proxy.rowCount(), 40);

        proxy.extendWindowAtOldEnd(1000);   // far more than exists
        QTRY_COMPARE(proxy.rowCount(), 100);
        QCOMPARE(proxy.windowSkip(), 0);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        proxy.extendWindowAtOldEnd(50);     // nothing left to expose
        QTest::qWait(200);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(proxy.rowCount(), 100);
        verifyMappingIsConsistent(proxy, source);
    }

    // revealNextChunk() bounded its release loop on sourceRowTotal() rather
    // than revealTarget(). The guard at the top of that function stops the
    // timer from STARTING past the cap, so this only bites once the exposed
    // count drops BELOW the cap — a removal inside the window — after which a
    // single 3 ms tick released straight through the cap to the source total.
    // With no delegates to build in a unit test, that is hundreds of rows:
    // the "pacing undoes the window" failure the cap exists to prevent.
    void pacingNeverOvershootsTheCapAfterARemovalInsideTheWindow()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);

        // Drop rows from inside the exposed window, leaving the cap above the
        // exposed count — the state that lets the reveal loop run. With
        // total=300 and skip=50 the mapping is sourceRow = 249 - proxyRow,
        // so the exposed band is source [190, 249]; 150 is OLDER than the
        // window and removing there changes nothing (the guard below caught
        // exactly that).
        source.removeAt(200, 20);
        QVERIFY2(proxy.rowCount() < 60,
                 "the removal did not shrink the exposed window, so the "
                 "overshoot path is not reachable and this test would pass "
                 "on broken code");
        const int afterRemoval = proxy.rowCount();

        QTest::qWait(400);
        QVERIFY2(proxy.rowCount() <= 60,
                 qPrintable(QStringLiteral(
                     "pacing overshot the window cap: %1 rows exposed "
                     "(cap 60, was %2 after the removal)")
                                .arg(proxy.rowCount()).arg(afterRemoval)));
        QCOMPARE(proxy.windowSkip(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // A room switch must not carry a stale skip into the new room — that would
    // hide the new room's newest messages.
    void aSourceResetClearsTheWindow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.windowSkip(), 40);

        source.seed(12);               // fresh snapshot for another room
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e11"));
        verifyMappingIsConsistent(proxy, source);
    }

    // Redactions/removals land anywhere. Each region must be handled without
    // corrupting the mapping — inside the window, newer than it, older than it.
    void removalsAreHandledInEveryRegion()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);       // exposes source 40..59

        // NEWER than the window: shrinks the skip, exposes nothing new.
        source.removeAt(90);
        QCOMPARE(proxy.windowSkip(), 39);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));
        verifyMappingIsConsistent(proxy, source);

        // INSIDE the window: one row leaves the view.
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        source.removeAt(50);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);

        // OLDER than the window: invisible.
        removed.clear();
        source.removeAt(3);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);
    }

    // Windows are clamped, never asserted: a pane computing a window from a
    // stale row count must degrade, not corrupt the mapping.
    void outOfRangeWindowsAreClamped()
    {
        FakeSource source;
        source.seed(30);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        proxy.setWindow(/*skip=*/500, /*rows=*/500);
        QVERIFY(proxy.windowSkip() <= 30);
        QVERIFY(proxy.rowCount() <= 30);
        verifyMappingIsConsistent(proxy, source);

        proxy.setWindow(/*skip=*/-5, /*rows=*/-5);
        QCOMPARE(proxy.windowSkip(), 0);
        QVERIFY(proxy.rowCount() >= 0);
        verifyMappingIsConsistent(proxy, source);

        proxy.clearWindow();
        QCOMPARE(proxy.rowCount(), 30);
        verifyMappingIsConsistent(proxy, source);
    }
};

QTEST_MAIN(ReverseListProxyWindowTest)
#include "ReverseListProxyWindowTest.moc"
