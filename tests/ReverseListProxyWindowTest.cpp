// The sliding window in ReverseListProxyModel. The timeline instantiates every
// row it is handed and per-frame cost scales with the instantiated item count,
// so the window bounds how many rows are exposed.
//
// The window is two integers: how many of the newest source rows are excluded
// (`windowSkip`) and how many are exposed. Every transition between two
// windows must be a single insert or remove at one end.
//
// Conventions (easy to invert by accident):
//   * source row 0 is the oldest event; source row total-1 the newest;
//   * proxy row 0 is the newest source row the window includes;
//   * only windowSkip 0 makes proxy row 0 the live edge.
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
    // Announce that one source row's data changed, nothing structural (edits,
    // redactions and late decryptions arrive this way).
    void touch(int row)
    {
        const QModelIndex ix = index(row, 0);
        Q_EMIT dataChanged(ix, ix, { Qt::DisplayRole });
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

// Every exposed row maps to the source row the window says, and
// mapFromSource is its exact inverse. Checked after every mutation.
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
        // ...and the data agrees, which catches an inverted reversal.
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
    // With no window the proxy is newest-first over the whole source.
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

    // A window skips the newest rows and exposes a band.
    void windowExposesABandAndMapsItCorrectly()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        // Skip the 20 newest, expose 30: source rows 50..79, newest first.
        proxy.setWindow(/*skipNewest=*/20, /*rows=*/30);
        QCOMPARE(proxy.windowSkip(), 20);
        QCOMPARE(proxy.rowCount(), 30);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e79"));
        QCOMPARE(proxyText(proxy, 29), QStringLiteral("e50"));
        QCOMPARE(proxy.oldestExposedSourceRow(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // Each end moves by a single insert or remove: never a reset (which would
    // rebuild every delegate) and never a renumbering in the middle.
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

        // Grow the oldest end by 10: one insert at the tail, the far edge in
        // the rotated view.
        proxy.setWindow(20, 40);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 30);  // first
        QCOMPARE(inserted.at(0).at(2).toInt(), 39);  // last
        QCOMPARE(proxy.oldestExposedSourceRow(), 40);
        verifyMappingIsConsistent(proxy, source);
        inserted.clear();

        // Release the newest end: one remove at the head. This is the one
        // transition that shifts kept rows, so the pane corrects contentY.
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

        // Slide the band towards the live edge at the same size: 25 released
        // at the tail and 25 restored at the head, one op per end, no reset.
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

    // While the reader is deep in history, live messages must not change which
    // events the window covers, or each one would slide the reader by a row.
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
        // ...and the reader still sees the same events.
        QCOMPARE(proxyText(proxy, 0), topEvent);
        QCOMPARE(proxyText(proxy, 19), bottomEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    // Backward pagination while windowed lands older than the window: invisible
    // and without disturbing the band.
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

    // Pacing never grows past the window; without the cap the reveal timer
    // would walk the exposed count back up to the source total.
    void pacingNeverGrowsPastTheWindow()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);
        QCOMPARE(proxy.rowCount(), 40);

        // Give the reveal timer generous time to misbehave.
        QTest::qWait(400);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.windowSkip(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    // Jump and search paths call releaseAll() first; with a window it must also
    // restore the newest end, or a jump to a recent message finds no row.
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

    // releaseAll() alone also lifts the window cap.
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

    // Reaching the window's old edge re-exposes rows already held instead of
    // asking the homeserver. Paced, since releasing a whole margin at once
    // builds that many delegates in one frame.
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
        // And it stops there.
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 70);

        // The newest end does not move: this path performs no contentY
        // correction.
        QCOMPARE(proxy.windowSkip(), 50);
        // Every insert lands at the tail (beyond the reader), never at row 0.
        QVERIFY(inserted.count() > 0);
        for (const QList<QVariant> &args : inserted)
            QVERIFY2(args.at(1).toInt() >= 40,
                     "an extension inserted at the head, which would shift "
                     "every row the reader is looking at");
        verifyMappingIsConsistent(proxy, source);
    }

    // Extending refuses when only the pacing backlog withholds rows: that
    // backlog releases itself, and only the window's own cap may answer yes.
    // (A check like `rowWindowSkip + count < total` is also true during an
    // ordinary initial reveal.)
    void extendingRefusesWhenOnlyThePacingBacklogWithholdsRows()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        // No window (cap 0), so anything unexposed is pacing backlog.
        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);
        QTRY_COMPARE(proxy.rowCount(), 200);   // pacing gets there by itself
        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);

        // With a window whose cap pacing has not reached yet, it is still
        // pacing's job.
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);
        // total=200, skip=50: sourceRow = 149 - proxyRow, so the band is
        // source [90, 149].
        source.removeAt(100, 20);              // inside the window
        QVERIFY(proxy.rowCount() < 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), false);

        // Once pacing reaches the cap, the window is the constraint.
        QTRY_COMPARE(proxy.rowCount(), 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), true);
        QTRY_COMPARE(proxy.rowCount(), 90);
        verifyMappingIsConsistent(proxy, source);
    }

    // The extension stops at the oldest loaded row, and asking again once
    // everything is exposed is a no-op.
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

    // revealNextChunk() is bounded by revealTarget(), not sourceRowTotal():
    // after a removal inside the window drops the exposed count below the cap,
    // a single tick must not release straight through the cap.
    void pacingNeverOvershootsTheCapAfterARemovalInsideTheWindow()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);

        // Remove rows inside the exposed window, leaving the cap above the
        // exposed count. With total=300 and skip=50, sourceRow = 249 - proxyRow,
        // so the band is source [190, 249].
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

    // The newest end: with a window active the pane's wheelMinY() is the
    // window's synthetic edge, so a downward gesture needs the window extended
    // there. It shifts every kept row, so it must arrive as one insert at the
    // head (the pane corrects contentY by the inserted rows' heights).
    void extendingAtTheNewEndRestoresRowsAsOneInsertAtTheHead()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);       // exposes source 110..149
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e149"));

        QSignalSpy aboutToInsert(&proxy,
                                 &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(&proxy, &QAbstractItemModel::modelReset);
        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);

        QVERIFY(proxy.extendWindowAtNewEnd(15));

        QCOMPARE(proxy.windowSkip(), 35);
        QCOMPARE(proxy.rowCount(), 55);
        // Exactly one structural op, at the head, covering 0..14: no reset and
        // no renumbering in the middle.
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(aboutToInsert.count(), 1);
        QCOMPARE(aboutToInsert.at(0).at(1).toInt(), 0);
        QCOMPARE(aboutToInsert.at(0).at(2).toInt(), 14);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);
        QCOMPARE(inserted.at(0).at(2).toInt(), 14);
        QCOMPARE(windowSpy.count(), 1);

        // Fifteen newer events at the head; the former head is at 15 and
        // unchanged; the old end did not move.
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e164"));
        QCOMPARE(proxyText(proxy, 14), QStringLiteral("e150"));
        QCOMPARE(proxyText(proxy, 15), QStringLiteral("e149"));
        QCOMPARE(proxy.oldestExposedSourceRow(), 110);
        verifyMappingIsConsistent(proxy, source);
    }

    // Asking for more than the skip holds clamps to the live edge; skip 0 is
    // the only state where the view's bottom is the newest message.
    void extendingBeyondTheSkipClampsAndRestoresTheLiveEdge()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(20, 30);       // exposes source 50..79

        QVERIFY(proxy.extendWindowAtNewEnd(500));
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 50);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e99"));
        QCOMPARE(proxy.mapToSource(proxy.index(0, 0)).row(),
                 source.rowCount() - 1);
        verifyMappingIsConsistent(proxy, source);

        // Nothing left to give: the pane reads this as "already live".
        QCOMPARE(proxy.extendWindowAtNewEnd(10), false);
    }

    // At the live edge the extension refuses (returns false) rather than
    // reporting a no-op success, so the pane does not correct contentY for
    // rows that never arrived.
    void extendingAtTheNewEndRefusesWhenThereIsNothingToGive()
    {
        FakeSource source;
        source.seed(50);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        QCOMPARE(proxy.windowSkip(), 0);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        QCOMPARE(proxy.extendWindowAtNewEnd(10), false);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.rowCount(), 50);

        // A window bounding only the old end has no skip either.
        proxy.setWindow(0, 20);
        inserted.clear();
        windowSpy.clear();
        QCOMPARE(proxy.extendWindowAtNewEnd(10), false);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.rowCount(), 20);

        // A non-positive request is refused, not read as "everything".
        proxy.setWindow(10, 20);
        QCOMPARE(proxy.extendWindowAtNewEnd(0), false);
        QCOMPARE(proxy.extendWindowAtNewEnd(-5), false);
        QCOMPARE(proxy.windowSkip(), 10);
        QCOMPARE(proxy.rowCount(), 20);
        verifyMappingIsConsistent(proxy, source);
    }

    // After a newest-end extension, live messages are still absorbed into the
    // skip and do not slide the reader (see
    // liveMessagesDoNotSlideAWindowedReader).
    void liveMessagesStillDoNotSlideTheReaderAfterANewEndExtension()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);       // exposes source 40..59

        QVERIFY(proxy.extendWindowAtNewEnd(10));
        QCOMPARE(proxy.windowSkip(), 30);
        QCOMPARE(proxy.rowCount(), 30);   // exposes source 40..69
        const QString topEvent = proxyText(proxy, 0);
        const QString bottomEvent = proxyText(proxy, 29);
        QCOMPARE(topEvent, QStringLiteral("e69"));
        QCOMPARE(bottomEvent, QStringLiteral("e40"));

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        for (int i = 0; i < 4; ++i)
            source.appendLive(QStringLiteral("live%1").arg(i));

        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 30);
        QCOMPARE(proxy.windowSkip(), 34);   // absorbed, not shown
        QCOMPARE(proxyText(proxy, 0), topEvent);
        QCOMPARE(proxyText(proxy, 29), bottomEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    // After an extension the cap grows with what was restored, and pacing
    // still never overshoots it. Overshooting needs the exposed count below
    // the cap, so this drives that state.
    void pacingNeverGrowsPastTheWindowAfterANewEndExtension()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);       // exposes source 190..249, cap 60
        QCOMPARE(proxy.rowCount(), 60);

        QVERIFY(proxy.extendWindowAtNewEnd(20));
        QCOMPARE(proxy.windowSkip(), 30);
        QCOMPARE(proxy.rowCount(), 80);   // exposes source 190..269

        // Nothing may creep past the new cap on its own.
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 80);
        QCOMPARE(proxy.windowSkip(), 30);

        // Remove rows inside the band so the reveal loop can run. With
        // total=300 and skip=30, sourceRow = 269 - proxyRow, so the band is
        // source [190, 269].
        source.removeAt(200, 20);
        QVERIFY2(proxy.rowCount() < 80,
                 "the removal did not shrink the exposed window, so the "
                 "overshoot path is not reachable and this test would pass "
                 "on broken code");

        // Pacing restores exactly the cap the extension set: no fewer, no more.
        QTRY_COMPARE(proxy.rowCount(), 80);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 80);
        QCOMPARE(proxy.windowSkip(), 30);
        verifyMappingIsConsistent(proxy, source);
    }

    // The cap grows by what was restored rather than being reset to what is
    // exposed now: after a removal inside the window the old end still owes
    // rows, and resetting the cap would silently drop that.
    void aNewEndExtensionDoesNotRetireWhatTheOldEndStillOwes()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);       // exposes source 190..249, cap 60
        QCOMPARE(proxy.rowCount(), 60);

        // Remove rows inside the band without spinning the event loop, so
        // pacing cannot refill before the extension.
        source.removeAt(200, 20);
        QCOMPARE(proxy.rowCount(), 40);

        QVERIFY(proxy.extendWindowAtNewEnd(10));
        QCOMPARE(proxy.windowSkip(), 40);
        QCOMPARE(proxy.rowCount(), 50);

        // The 60 the window asked for, plus the 10 restored at the head.
        QTRY_COMPARE(proxy.rowCount(), 70);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 70);
        QCOMPARE(proxy.windowSkip(), 40);
        verifyMappingIsConsistent(proxy, source);
    }

    // Walk the window home in small steps, checking every exposed row's mapping
    // after each one: a skip change renumbers every view row.
    void everyExposedRowStillMapsCorrectlyAcrossRepeatedExtensions()
    {
        FakeSource source;
        source.seed(150);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(60, 40);       // exposes source 50..89
        const QString tracked = proxyText(proxy, 0);
        QCOMPARE(tracked, QStringLiteral("e89"));

        int guard = 0;
        while (proxy.windowSkip() > 0) {
            QVERIFY2(guard++ < 50, "extensions are not converging on the "
                                   "live edge");
            const int skipBefore = proxy.windowSkip();
            const int rowsBefore = proxy.rowCount();
            QVERIFY(proxy.extendWindowAtNewEnd(7));
            const int add = skipBefore < 7 ? skipBefore : 7;
            QCOMPARE(proxy.windowSkip(), skipBefore - add);
            QCOMPARE(proxy.rowCount(), rowsBefore + add);
            // The former head moved down by exactly the rows restored so far.
            QCOMPARE(proxyText(proxy, 60 - proxy.windowSkip()), tracked);
            verifyMappingIsConsistent(proxy, source);
        }

        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 100);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e149"));
        QCOMPARE(proxy.extendWindowAtNewEnd(7), false);
        verifyMappingIsConsistent(proxy, source);
    }

    // A reset after an extension still clears the window, so a room switch
    // does not hide the next room's newest messages.
    void aSourceResetAfterANewEndExtensionStillClearsTheWindow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QVERIFY(proxy.extendWindowAtNewEnd(15));
        QCOMPARE(proxy.windowSkip(), 25);

        source.seed(12);               // fresh snapshot for another room
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e11"));
        QCOMPARE(proxy.extendWindowAtNewEnd(5), false);
        verifyMappingIsConsistent(proxy, source);
    }

    // A room switch does not carry a stale skip into the new room.
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

    // Removals are handled in every region: inside the window, newer than it,
    // and older than it.
    void removalsAreHandledInEveryRegion()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);       // exposes source 40..59

        // Newer than the window: shrinks the skip, exposes nothing new.
        source.removeAt(90);
        QCOMPARE(proxy.windowSkip(), 39);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));
        verifyMappingIsConsistent(proxy, source);

        // Inside the window: one row leaves the view.
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        source.removeAt(50);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);

        // Older than the window: invisible.
        removed.clear();
        source.removeAt(3);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);
    }

    // Windows are clamped: a window computed from a stale row count degrades
    // instead of corrupting the mapping.
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

    // windowChanged() notifications. QML reads the skip through a NOTIFY-gated
    // binding (`rowWindowSkip`), so every change must emit or the pane keeps a
    // stale value (e.g. atBottomEdge() refusing at the real live edge). Each
    // case pins the signal, not just the value.

    // A room switch arrives as a source reset: the skip goes to 0 and that is
    // announced.
    void aSourceResetEmitsWindowChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.windowSkip(), 40);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.seed(12);               // the next room's snapshot

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        verifyMappingIsConsistent(proxy, source);
    }

    // A live message absorbed by the window moves the skip, so it is announced.
    void aLiveMessageAbsorbedByTheWindowEmitsWindowChanged()
    {
        FakeSource source;
        source.seed(60);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(20, 20);
        QCOMPARE(proxy.windowSkip(), 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.appendLive(QStringLiteral("live0"));

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 21);
        // ...and the absorption still holds: same rows, same head.
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e39"));
        verifyMappingIsConsistent(proxy, source);
    }

    // A removal newer than the window shrinks the skip and is announced.
    void aRemovalNewerThanTheWindowEmitsWindowChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.removeAt(90);           // newer than the window's newest edge

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 39);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));
        verifyMappingIsConsistent(proxy, source);
    }

    // With a window held, dataChanged names the proxy row that changed: the
    // forwarder must subtract the skip like every other mapping, or the
    // changed row is never told to re-read.
    void aDataChangeUnderAWindowNamesTheRowThatChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.rowCount(), 20);
        // Proxy row 0 is source row 100 - 1 - 40.
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));

        QSignalSpy changed(&proxy, &ReverseListProxyModel::dataChanged);
        source.touch(59);

        QCOMPARE(changed.count(), 1);
        const auto args = changed.takeFirst();
        QCOMPARE(args.at(0).toModelIndex().row(), 0);
        QCOMPARE(args.at(1).toModelIndex().row(), 0);

        // ...and the far end of the window maps too.
        source.touch(40);
        QCOMPARE(changed.count(), 1);
        const auto last = changed.takeFirst();
        QCOMPARE(last.at(0).toModelIndex().row(), 19);
    }

    // Re-pointing the proxy at another model drops the window and announces
    // it.
    void setSourceModelEmitsWindowChanged()
    {
        FakeSource first;
        first.seed(100);
        FakeSource second;
        second.seed(30);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&first);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.windowSkip(), 40);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        proxy.setSourceModel(&second);

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 30);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e29"));
        verifyMappingIsConsistent(proxy, second);
    }

    // Writing the value the skip already holds does not emit: this runs from
    // source signal handlers on every removal and live message.
    void writingTheSameWindowSkipDoesNotEmit()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(30, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);

        // Identical window: nothing moved.
        proxy.setWindow(30, 20);
        QCOMPARE(windowSpy.count(), 0);

        // Only the old end moved: the skip is unchanged, and the exposed count
        // is announced through rowsInserted/rowsRemoved.
        proxy.setWindow(30, 25);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.rowCount(), 25);

        // A removal older than the window touches neither.
        source.removeAt(2);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.windowSkip(), 30);

        // ...and a real move still announces itself.
        proxy.setWindow(20, 25);
        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 20);
        verifyMappingIsConsistent(proxy, source);
    }

    // After a reset the skip is 0 and stays 0, and clearWindow() (which only
    // acts on a non-zero skip) is a silent no-op.
    void afterAResetTheSkipStaysZeroAndClearWindowIsASilentNoOp()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.seed(40);               // room switch
        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 0);
        // Read it twice: the value is state, not a one-shot answer.
        QCOMPARE(proxy.windowSkip(), 0);

        windowSpy.clear();
        proxy.clearWindow();
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e39"));
        verifyMappingIsConsistent(proxy, source);
    }
};

QTEST_MAIN(ReverseListProxyWindowTest)
#include "ReverseListProxyWindowTest.moc"
