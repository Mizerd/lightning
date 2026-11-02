#include "storage/BridgeLabelStore.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

// BridgeLabelStore persists MSC2346 bridge labels across launches, so badges
// show at startup without first opening each chat. A negative answer is also
// stored, or every launch would re-ask every room already answered.
class BridgeLabelStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void aLearnedLabelSurvivesAReopen();
    void aNegativeAnswerIsRememberedAsAResult();
    void anExpiredNegativeAnswerStopsCountingAsKnown();
    void aPositiveAnswerOutlivesANegativeOne();
    void aRowStampedInTheFutureIsNotFresh();
    void seedingReturnsOnlyFreshPositiveRows();
    void aCorruptFileIsDiscardedRatherThanHalfRead();
    void theStoreIsInertWithoutAPath();
    void removeStoreDeletesTheFile();
    void removingAnotherStoreDoesNotCancelThisOnesQueuedWrite();
    void aQueuedWriteForAStoreRemovedWithoutBeingOpenIsCancelled();
    void theCapEvictsAFutureStampedRowBeforeAFreshOne();
    void theCapDropsTheOldestRowsFirst();
    void aDeferredAnswerTouchesTheDiskOnlyWhenFlushed();
    void anAsyncFlushWritesOffThreadAndADeleteCancelsAQueuedOne();
    void closingDropsDeferredAnswersSoASignOutCannotResurrectTheFile();

private:
    QString path() const { return m_dir.filePath(QStringLiteral("bridge-labels.json")); }
    void writeRaw(const QJsonObject &root) const;

    QTemporaryDir m_dir;
};

void BridgeLabelStoreTest::init()
{
    QFile::remove(path());
}

void BridgeLabelStoreTest::writeRaw(const QJsonObject &root) const
{
    QFile file(path());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
}

void BridgeLabelStoreTest::aLearnedLabelSurvivesAReopen()
{
    {
        BridgeLabelStore store;
        QVERIFY(store.openFor(path()));
        store.remember(QStringLiteral("!room:example.org"),
                       QStringLiteral("whatsapp"), QStringLiteral("WhatsApp"));
    }
    // A second launch: nothing carried over in memory.
    BridgeLabelStore reopened;
    QVERIFY(reopened.openFor(path()));
    const auto entry = reopened.label(QStringLiteral("!room:example.org"));
    QVERIFY2(entry.has_value(),
             "a label learned in an earlier session was not remembered across "
             "a reopen, which is the whole defect: the badge is missing again "
             "at every startup until the room is visited");
    QCOMPARE(entry->networkId, QStringLiteral("whatsapp"));
    QCOMPARE(entry->label, QStringLiteral("WhatsApp"));
}

void BridgeLabelStoreTest::aNegativeAnswerIsRememberedAsAResult()
{
    {
        BridgeLabelStore store;
        QVERIFY(store.openFor(path()));
        store.remember(QStringLiteral("!plain:example.org"), QString(), QString());
    }
    BridgeLabelStore reopened;
    QVERIFY(reopened.openFor(path()));
    QVERIFY2(reopened.knows(QStringLiteral("!plain:example.org")),
             "'this room advertises no bridge' was not written down, so every "
             "launch re-reads /state for every unbridged room forever");
    // Known, but not labelled: a negative answer must never paint a badge.
    QVERIFY(!reopened.label(QStringLiteral("!plain:example.org")).has_value());
}

void BridgeLabelStoreTest::anExpiredNegativeAnswerStopsCountingAsKnown()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonObject root;
    QJsonObject row;
    row.insert(QStringLiteral("t"),
               static_cast<double>(now - BridgeLabelStore::kNegativeLifetimeSecs - 60));
    root.insert(QStringLiteral("!stale:example.org"), row);
    writeRaw(root);

    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    QVERIFY2(!store.knows(QStringLiteral("!stale:example.org")),
             "an expired negative answer still counted as known, so a room "
             "that has since gained a bridge would never be re-read");
}

void BridgeLabelStoreTest::aPositiveAnswerOutlivesANegativeOne()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // One second past the negative lifetime: a negative row is stale, a
    // positive one is not.
    const qint64 stamp = now - BridgeLabelStore::kNegativeLifetimeSecs - 1;

    BridgeLabelStore::Entry positive;
    positive.networkId = QStringLiteral("signal");
    positive.label = QStringLiteral("Signal");
    positive.learnedAt = stamp;

    BridgeLabelStore::Entry negative;
    negative.learnedAt = stamp;

    QVERIFY(BridgeLabelStore::entryIsFresh(positive, now));
    QVERIFY(!BridgeLabelStore::entryIsFresh(negative, now));
}

void BridgeLabelStoreTest::aRowStampedInTheFutureIsNotFresh()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    BridgeLabelStore::Entry entry;
    entry.networkId = QStringLiteral("telegram");
    entry.label = QStringLiteral("Telegram");
    entry.learnedAt = now + 60 * 60;
    QVERIFY2(!BridgeLabelStore::entryIsFresh(entry, now),
             "a row stamped in the future was treated as fresh, so a clock "
             "that moved backwards would freeze that room's badge");
}

void BridgeLabelStoreTest::seedingReturnsOnlyFreshPositiveRows()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonObject root;

    QJsonObject good;
    good.insert(QStringLiteral("n"), QStringLiteral("whatsapp"));
    good.insert(QStringLiteral("l"), QStringLiteral("WhatsApp"));
    good.insert(QStringLiteral("t"), static_cast<double>(now - 60));
    root.insert(QStringLiteral("!good:example.org"), good);

    QJsonObject expired;
    expired.insert(QStringLiteral("n"), QStringLiteral("signal"));
    expired.insert(QStringLiteral("l"), QStringLiteral("Signal"));
    expired.insert(QStringLiteral("t"),
                   static_cast<double>(now - BridgeLabelStore::kPositiveLifetimeSecs - 60));
    root.insert(QStringLiteral("!expired:example.org"), expired);

    QJsonObject negative;
    negative.insert(QStringLiteral("t"), static_cast<double>(now - 60));
    root.insert(QStringLiteral("!plain:example.org"), negative);

    writeRaw(root);

    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    const auto seed = store.positiveLabels();
    QCOMPARE(seed.size(), 1);
    QVERIFY(seed.contains(QStringLiteral("!good:example.org")));
    QVERIFY(!seed.contains(QStringLiteral("!expired:example.org")));
    QVERIFY(!seed.contains(QStringLiteral("!plain:example.org")));
}

void BridgeLabelStoreTest::aCorruptFileIsDiscardedRatherThanHalfRead()
{
    QFile file(path());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"!a:example.org\": {\"n\": \"whats");
    file.close();

    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    QCOMPARE(store.count(), 0);
    // Still usable: a discarded file must not close the store.
    store.remember(QStringLiteral("!b:example.org"), QStringLiteral("irc"),
                   QStringLiteral("IRC"));
    QCOMPARE(store.count(), 1);
}

void BridgeLabelStoreTest::theStoreIsInertWithoutAPath()
{
    BridgeLabelStore store;
    QVERIFY(!store.openFor(QString()));
    QVERIFY(!store.isOpen());
    // Must be a working state rather than a crash or an invented location.
    store.remember(QStringLiteral("!x:example.org"), QStringLiteral("irc"),
                   QStringLiteral("IRC"));
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.label(QStringLiteral("!x:example.org")).has_value());
    QVERIFY(!store.knows(QStringLiteral("!x:example.org")));
}

void BridgeLabelStoreTest::removeStoreDeletesTheFile()
{
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    store.remember(QStringLiteral("!a:example.org"), QStringLiteral("irc"),
                   QStringLiteral("IRC"));
    QVERIFY(QFile::exists(path()));
    QVERIFY(BridgeLabelStore::removeStore(path()));
    QVERIFY(!QFile::exists(path()));
    // Idempotent: an absent file is already gone.
    QVERIFY(BridgeLabelStore::removeStore(path()));
}

// A future-stamped row is evicted before a fresh one: after a clock moves
// backwards `entryIsFresh` already distrusts such rows, so the cap must not
// keep one at a real row's expense. Clamping the sort key to `now` does not
// achieve this; the clamped row still sorts at or above every real one.
void BridgeLabelStoreTest::theCapEvictsAFutureStampedRowBeforeAFreshOne()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonObject root;
    // One future-stamped row and kMaxEntries fresh ones: exactly one goes,
    // the future row.
    QJsonObject ahead;
    ahead.insert(QStringLiteral("n"), QStringLiteral("irc"));
    ahead.insert(QStringLiteral("l"), QStringLiteral("IRC"));
    ahead.insert(QStringLiteral("t"), static_cast<double>(now + 86400));
    root.insert(QStringLiteral("!future:example.org"), ahead);
    for (int i = 0; i < BridgeLabelStore::kMaxEntries; ++i) {
        QJsonObject row;
        row.insert(QStringLiteral("n"), QStringLiteral("irc"));
        row.insert(QStringLiteral("l"), QStringLiteral("IRC"));
        row.insert(QStringLiteral("t"), static_cast<double>(now - 1000 + i));
        root.insert(QStringLiteral("!fresh%1:example.org").arg(i), row);
    }
    writeRaw(root);

    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    QCOMPARE(store.count(), BridgeLabelStore::kMaxEntries);
    QVERIFY2(!store.knows(QStringLiteral("!future:example.org")),
             "the cap kept a row stamped in the future, which is one the "
             "freshness rule already refuses to believe, and evicted a "
             "genuinely fresh row to make room for it");
    QVERIFY2(store.knows(QStringLiteral("!fresh0:example.org")),
             "the oldest genuinely fresh row was evicted while a "
             "worthless future-stamped row survived");
}

void BridgeLabelStoreTest::theCapDropsTheOldestRowsFirst()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonObject root;
    for (int i = 0; i < BridgeLabelStore::kMaxEntries + 10; ++i) {
        QJsonObject row;
        row.insert(QStringLiteral("n"), QStringLiteral("irc"));
        row.insert(QStringLiteral("l"), QStringLiteral("IRC"));
        // Row 0 is the oldest, and the ten oldest are the ones that must go.
        row.insert(QStringLiteral("t"), static_cast<double>(now - 100000 + i));
        root.insert(QStringLiteral("!r%1:example.org").arg(i), row);
    }
    writeRaw(root);

    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    QCOMPARE(store.count(), BridgeLabelStore::kMaxEntries);
    QVERIFY2(!store.knows(QStringLiteral("!r0:example.org")),
             "the cap dropped an arbitrary row instead of the oldest one");
    QVERIFY(store.knows(QStringLiteral("!r%1:example.org")
                            .arg(BridgeLabelStore::kMaxEntries + 9)));
}

void BridgeLabelStoreTest::aDeferredAnswerTouchesTheDiskOnlyWhenFlushed()
{
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    // The first visit to every room used to rewrite and fsync the whole file
    // on the GUI thread; a deferred answer must not create or change it.
    for (int i = 0; i < 50; ++i)
        store.rememberDeferred(QStringLiteral("!r%1:example.org").arg(i),
                               QString(), QString());
    QVERIFY(store.isDirty());
    QVERIFY2(!QFile::exists(path()),
             "a deferred answer wrote the file before flush()");
    // Held in memory, so the app still treats the room as answered.
    QVERIFY(store.knows(QStringLiteral("!r7:example.org")));

    QVERIFY(store.flush());
    QVERIFY(!store.isDirty());
    QVERIFY(QFile::exists(path()));
    BridgeLabelStore reopened;
    QVERIFY(reopened.openFor(path()));
    QCOMPARE(reopened.count(), 50);
    // A clean store does not write again.
    QVERIFY(QFile::remove(path()));
    QVERIFY(store.flush());
    QVERIFY2(!QFile::exists(path()), "flush() of a clean store rewrote the file");
}

void BridgeLabelStoreTest::closingDropsDeferredAnswersSoASignOutCannotResurrectTheFile()
{
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    store.rememberDeferred(QStringLiteral("!a:example.org"),
                           QStringLiteral("whatsapp"), QStringLiteral("WhatsApp"));
    store.close();
    QVERIFY(!store.isDirty());
    QVERIFY(store.flush()); // nothing open, nothing to write
    QVERIFY2(!QFile::exists(path()),
             "a closed store wrote pending answers, which would put a signed-out "
             "account's room list back on disk");
}

void BridgeLabelStoreTest::anAsyncFlushWritesOffThreadAndADeleteCancelsAQueuedOne()
{
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    store.rememberDeferred(QStringLiteral("!a:example.org"), QString(), QString());
    store.flushAsync();
    QVERIFY(!store.isDirty());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path()), 5000);
    BridgeLabelStore reopened;
    QVERIFY(reopened.openFor(path()));
    QVERIFY(reopened.knows(QStringLiteral("!a:example.org")));

    // Queue many writes, delete at once: whatever has not run must not
    // resurrect the file afterwards.
    for (int i = 0; i < 200; ++i) {
        store.rememberDeferred(QStringLiteral("!r%1:example.org").arg(i), QString(), QString());
        store.flushAsync();
    }
    QVERIFY(BridgeLabelStore::removeStore(path()));
    QThreadPool::globalInstance()->waitForDone(10000);
    QVERIFY2(!QFile::exists(path()),
             "a write queued before the delete brought the file back");
}

void BridgeLabelStoreTest::removingAnotherStoreDoesNotCancelThisOnesQueuedWrite()
{
    // Hold the pool so the write is still queued when the other account's
    // store is removed.
    QThreadPool *global = QThreadPool::globalInstance();
    const int saved = global->maxThreadCount();
    global->setMaxThreadCount(1);
    global->start([] { QThread::msleep(300); });
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    store.rememberDeferred(QStringLiteral("!a:example.org"), QString(), QString());
    store.flushAsync();
    QVERIFY(BridgeLabelStore::removeStore(path() + QStringLiteral(".other")));
    global->waitForDone(10000);
    global->setMaxThreadCount(saved);
    QVERIFY2(QFile::exists(path()),
             "another account's removal cancelled this account's write");
}

void BridgeLabelStoreTest::aQueuedWriteForAStoreRemovedWithoutBeingOpenIsCancelled()
{
    QThreadPool *global = QThreadPool::globalInstance();
    const int saved = global->maxThreadCount();
    global->setMaxThreadCount(1);
    global->start([] { QThread::msleep(300); });
    BridgeLabelStore store;
    QVERIFY(store.openFor(path()));
    store.rememberDeferred(QStringLiteral("!a:example.org"), QString(), QString());
    store.flushAsync();
    store.close(); // as when another account became the active one
    QVERIFY(BridgeLabelStore::removeStore(path()));
    global->waitForDone(10000);
    global->setMaxThreadCount(saved);
    QVERIFY2(!QFile::exists(path()),
             "a queued write brought a removed account's file back");
}

QTEST_MAIN(BridgeLabelStoreTest)
#include "BridgeLabelStoreTest.moc"
