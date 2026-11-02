#include <QAbstractListModel>
#include <QSignalSpy>
#include <QtTest>

#include "models/ReverseListProxyModel.h"

namespace {

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
    void prepend(int n)
    {
        beginInsertRows({}, 0, n - 1);
        for (int i = n - 1; i >= 0; --i)
            m_rows.prepend(QStringLiteral("p%1").arg(i));
        endInsertRows();
    }
    void appendLive(const QString &id)
    {
        beginInsertRows({}, m_rows.size(), m_rows.size());
        m_rows.append(id);
        endInsertRows();
    }
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
        QCOMPARE(proxyText(proxy, r),
                 source.data(src, Qt::DisplayRole).toString());
    }
    if (rows > 0)
        QCOMPARE(proxy.oldestExposedSourceRow(),
                 total - proxy.windowSkip() - rows);
    else
        QCOMPARE(proxy.oldestExposedSourceRow(), -1);
}

}

class ReverseListProxyWindowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void latestJumpNeverExposesTheSkippedBacklog()
    {
        FakeSource source;
        source.seed(2000);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(1600, 100);
        int peak = proxy.rowCount();
        auto sample = [&] { peak = qMax(peak, proxy.rowCount()); };
        connect(&proxy, &QAbstractItemModel::rowsInserted, this, sample);
        connect(&proxy, &QAbstractItemModel::modelReset, this, sample);
        proxy.showLatestWindow(320);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 320);
        QVERIFY(peak <= 320);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e1999"));
        verifyMappingIsConsistent(proxy, source);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 320);
        QSignalSpy reset(&proxy, &QAbstractItemModel::modelReset);
        proxy.showLatestWindow(320);
        QCOMPARE(reset.count(), 0);
        source.appendLive(QStringLiteral("live"));
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("live"));
        verifyMappingIsConsistent(proxy, source);
    }

    void unwindowedProxyIsNewestFirstOverEverything()
    {
        FakeSource source;
        source.seed(10);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        QCOMPARE(proxy.rowCount(), 10);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e9"));
        QCOMPARE(proxyText(proxy, 9), QStringLiteral("e0"));
        verifyMappingIsConsistent(proxy, source);
    }

    void windowExposesABandAndMapsItCorrectly()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        proxy.setWindow(20,30);
        QCOMPARE(proxy.windowSkip(), 20);
        QCOMPARE(proxy.rowCount(), 30);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e79"));
        QCOMPARE(proxyText(proxy, 29), QStringLiteral("e50"));
        QCOMPARE(proxy.oldestExposedSourceRow(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

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

        proxy.setWindow(20, 40);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 30);
        QCOMPARE(inserted.at(0).at(2).toInt(), 39);
        QCOMPARE(proxy.oldestExposedSourceRow(), 40);
        verifyMappingIsConsistent(proxy, source);
        inserted.clear();

        proxy.setWindow(35, 40);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 0);
        QCOMPARE(removed.at(0).at(2).toInt(), 14);
        QCOMPARE(proxy.windowSkip(), 35);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.oldestExposedSourceRow(), 25);
        verifyMappingIsConsistent(proxy, source);
        removed.clear();
        inserted.clear();

        proxy.setWindow(10, 40);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 15);
        QCOMPARE(removed.at(0).at(2).toInt(), 39);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);
        QCOMPARE(inserted.at(0).at(2).toInt(), 24);
        QCOMPARE(proxy.windowSkip(), 10);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.oldestExposedSourceRow(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

    void liveMessagesDoNotSlideAWindowedReader()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        const QString topEvent = proxyText(proxy, 0);
        const QString bottomEvent = proxyText(proxy, 19);
        QCOMPARE(topEvent, QStringLiteral("e59"));
        QCOMPARE(bottomEvent, QStringLiteral("e40"));

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        for (int i = 0; i < 5; ++i)
            source.appendLive(QStringLiteral("live%1").arg(i));

        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxy.windowSkip(), 45);
        QCOMPARE(proxyText(proxy, 0), topEvent);
        QCOMPARE(proxyText(proxy, 19), bottomEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    void backwardPaginationBelowTheWindowIsInvisible()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        const QString topEvent = proxyText(proxy, 0);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        source.prepend(20);

        QCOMPARE(inserted.count(), 0);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), topEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    void pacingNeverGrowsPastTheWindow()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);
        QCOMPARE(proxy.rowCount(), 40);

        QTest::qWait(400);
        QCOMPARE(proxy.rowCount(), 40);
        QCOMPARE(proxy.windowSkip(), 50);
        verifyMappingIsConsistent(proxy, source);
    }

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
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e99"));
        verifyMappingIsConsistent(proxy, source);
    }

    void releaseAllLiftsTheWindowCap()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(0, 40);
        QCOMPARE(proxy.rowCount(), 40);

        proxy.releaseAll();
        QCOMPARE(proxy.rowCount(), 200);
        verifyMappingIsConsistent(proxy, source);
    }

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
        QTRY_COMPARE(proxy.rowCount(), 70);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 70);

        QCOMPARE(proxy.windowSkip(), 50);
        QVERIFY(inserted.count() > 0);
        for (const QList<QVariant> &args : inserted)
            QVERIFY2(args.at(1).toInt() >= 40,
                     "an extension inserted at the head, which would shift "
                     "every row the reader is looking at");
        verifyMappingIsConsistent(proxy, source);
    }

    void extendingRefusesWhenOnlyThePacingBacklogWithholdsRows()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);
        QTRY_COMPARE(proxy.rowCount(), 200);
        QCOMPARE(proxy.extendWindowAtOldEnd(50), false);

        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);
        source.removeAt(100, 20);
        QVERIFY(proxy.rowCount() < 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), false);

        QTRY_COMPARE(proxy.rowCount(), 60);
        QCOMPARE(proxy.extendWindowAtOldEnd(30), true);
        QTRY_COMPARE(proxy.rowCount(), 90);
        verifyMappingIsConsistent(proxy, source);
    }

    void extendingAtTheOldEndStopsAtTheOldestLoadedRow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(0, 40);
        QCOMPARE(proxy.rowCount(), 40);

        proxy.extendWindowAtOldEnd(1000);
        QTRY_COMPARE(proxy.rowCount(), 100);
        QCOMPARE(proxy.windowSkip(), 0);

        QSignalSpy inserted(&proxy, &QAbstractItemModel::rowsInserted);
        proxy.extendWindowAtOldEnd(50);
        QTest::qWait(200);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(proxy.rowCount(), 100);
        verifyMappingIsConsistent(proxy, source);
    }

    void pacingNeverOvershootsTheCapAfterARemovalInsideTheWindow()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);

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

    void extendingAtTheNewEndRestoresRowsAsOneInsertAtTheHead()
    {
        FakeSource source;
        source.seed(200);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 40);
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
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(aboutToInsert.count(), 1);
        QCOMPARE(aboutToInsert.at(0).at(1).toInt(), 0);
        QCOMPARE(aboutToInsert.at(0).at(2).toInt(), 14);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 0);
        QCOMPARE(inserted.at(0).at(2).toInt(), 14);
        QCOMPARE(windowSpy.count(), 1);

        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e164"));
        QCOMPARE(proxyText(proxy, 14), QStringLiteral("e150"));
        QCOMPARE(proxyText(proxy, 15), QStringLiteral("e149"));
        QCOMPARE(proxy.oldestExposedSourceRow(), 110);
        verifyMappingIsConsistent(proxy, source);
    }

    void extendingBeyondTheSkipClampsAndRestoresTheLiveEdge()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(20, 30);

        QVERIFY(proxy.extendWindowAtNewEnd(500));
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 50);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e99"));
        QCOMPARE(proxy.mapToSource(proxy.index(0, 0)).row(),
                 source.rowCount() - 1);
        verifyMappingIsConsistent(proxy, source);

        QCOMPARE(proxy.extendWindowAtNewEnd(10), false);
    }

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

        proxy.setWindow(0, 20);
        inserted.clear();
        windowSpy.clear();
        QCOMPARE(proxy.extendWindowAtNewEnd(10), false);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.rowCount(), 20);

        proxy.setWindow(10, 20);
        QCOMPARE(proxy.extendWindowAtNewEnd(0), false);
        QCOMPARE(proxy.extendWindowAtNewEnd(-5), false);
        QCOMPARE(proxy.windowSkip(), 10);
        QCOMPARE(proxy.rowCount(), 20);
        verifyMappingIsConsistent(proxy, source);
    }

    void liveMessagesStillDoNotSlideTheReaderAfterANewEndExtension()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        QVERIFY(proxy.extendWindowAtNewEnd(10));
        QCOMPARE(proxy.windowSkip(), 30);
        QCOMPARE(proxy.rowCount(), 30);
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
        QCOMPARE(proxy.windowSkip(), 34);
        QCOMPARE(proxyText(proxy, 0), topEvent);
        QCOMPARE(proxyText(proxy, 29), bottomEvent);
        verifyMappingIsConsistent(proxy, source);
    }

    void pacingNeverGrowsPastTheWindowAfterANewEndExtension()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);

        QVERIFY(proxy.extendWindowAtNewEnd(20));
        QCOMPARE(proxy.windowSkip(), 30);
        QCOMPARE(proxy.rowCount(), 80);

        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 80);
        QCOMPARE(proxy.windowSkip(), 30);

        source.removeAt(200, 20);
        QVERIFY2(proxy.rowCount() < 80,
                 "the removal did not shrink the exposed window, so the "
                 "overshoot path is not reachable and this test would pass "
                 "on broken code");

        QTRY_COMPARE(proxy.rowCount(), 80);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 80);
        QCOMPARE(proxy.windowSkip(), 30);
        verifyMappingIsConsistent(proxy, source);
    }

    void aNewEndExtensionDoesNotRetireWhatTheOldEndStillOwes()
    {
        FakeSource source;
        source.seed(300);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(50, 60);
        QCOMPARE(proxy.rowCount(), 60);

        source.removeAt(200, 20);
        QCOMPARE(proxy.rowCount(), 40);

        QVERIFY(proxy.extendWindowAtNewEnd(10));
        QCOMPARE(proxy.windowSkip(), 40);
        QCOMPARE(proxy.rowCount(), 50);

        QTRY_COMPARE(proxy.rowCount(), 70);
        QTest::qWait(300);
        QCOMPARE(proxy.rowCount(), 70);
        QCOMPARE(proxy.windowSkip(), 40);
        verifyMappingIsConsistent(proxy, source);
    }

    void everyExposedRowStillMapsCorrectlyAcrossRepeatedExtensions()
    {
        FakeSource source;
        source.seed(150);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(60, 40);
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
            QCOMPARE(proxyText(proxy, 60 - proxy.windowSkip()), tracked);
            verifyMappingIsConsistent(proxy, source);
        }

        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 100);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e149"));
        QCOMPARE(proxy.extendWindowAtNewEnd(7), false);
        verifyMappingIsConsistent(proxy, source);
    }

    void aSourceResetAfterANewEndExtensionStillClearsTheWindow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QVERIFY(proxy.extendWindowAtNewEnd(15));
        QCOMPARE(proxy.windowSkip(), 25);

        source.seed(12);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e11"));
        QCOMPARE(proxy.extendWindowAtNewEnd(5), false);
        verifyMappingIsConsistent(proxy, source);
    }

    void aSourceResetClearsTheWindow()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.windowSkip(), 40);

        source.seed(12);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e11"));
        verifyMappingIsConsistent(proxy, source);
    }

    void removalsAreHandledInEveryRegion()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        source.removeAt(90);
        QCOMPARE(proxy.windowSkip(), 39);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));
        verifyMappingIsConsistent(proxy, source);

        QSignalSpy removed(&proxy, &QAbstractItemModel::rowsRemoved);
        source.removeAt(50);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);

        removed.clear();
        source.removeAt(3);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(proxy.rowCount(), 19);
        verifyMappingIsConsistent(proxy, source);
    }

    void outOfRangeWindowsAreClamped()
    {
        FakeSource source;
        source.seed(30);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);

        proxy.setWindow(500,500);
        QVERIFY(proxy.windowSkip() <= 30);
        QVERIFY(proxy.rowCount() <= 30);
        verifyMappingIsConsistent(proxy, source);

        proxy.setWindow(-5,-5);
        QCOMPARE(proxy.windowSkip(), 0);
        QVERIFY(proxy.rowCount() >= 0);
        verifyMappingIsConsistent(proxy, source);

        proxy.clearWindow();
        QCOMPARE(proxy.rowCount(), 30);
        verifyMappingIsConsistent(proxy, source);
    }

    void aSourceResetEmitsWindowChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.windowSkip(), 40);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.seed(12);

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 0);
        QCOMPARE(proxy.rowCount(), 12);
        verifyMappingIsConsistent(proxy, source);
    }

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
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e39"));
        verifyMappingIsConsistent(proxy, source);
    }

    void aRemovalNewerThanTheWindowEmitsWindowChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.removeAt(90);

        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 39);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));
        verifyMappingIsConsistent(proxy, source);
    }

    void aDataChangeUnderAWindowNamesTheRowThatChanged()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);
        QCOMPARE(proxy.rowCount(), 20);
        QCOMPARE(proxyText(proxy, 0), QStringLiteral("e59"));

        QSignalSpy changed(&proxy, &ReverseListProxyModel::dataChanged);
        source.touch(59);

        QCOMPARE(changed.count(), 1);
        const auto args = changed.takeFirst();
        QCOMPARE(args.at(0).toModelIndex().row(), 0);
        QCOMPARE(args.at(1).toModelIndex().row(), 0);

        source.touch(40);
        QCOMPARE(changed.count(), 1);
        const auto last = changed.takeFirst();
        QCOMPARE(last.at(0).toModelIndex().row(), 19);
    }

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

    void writingTheSameWindowSkipDoesNotEmit()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(30, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);

        proxy.setWindow(30, 20);
        QCOMPARE(windowSpy.count(), 0);

        proxy.setWindow(30, 25);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.rowCount(), 25);

        source.removeAt(2);
        QCOMPARE(windowSpy.count(), 0);
        QCOMPARE(proxy.windowSkip(), 30);

        proxy.setWindow(20, 25);
        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 20);
        verifyMappingIsConsistent(proxy, source);
    }

    void afterAResetTheSkipStaysZeroAndClearWindowIsASilentNoOp()
    {
        FakeSource source;
        source.seed(100);
        ReverseListProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setWindow(40, 20);

        QSignalSpy windowSpy(&proxy, &ReverseListProxyModel::windowChanged);
        source.seed(40);
        QCOMPARE(windowSpy.count(), 1);
        QCOMPARE(proxy.windowSkip(), 0);
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
