#include "storage/BridgeLabelStore.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

// B017, reported 2026-09-08: "when I open lightning, the tags are shown how
// they used to show, missing on some chats, only when I click on chats does
// the correct tag show up, and it doesn't persist between restarting the
// application."
//
// The MSC2346 answer was held in the room list model and nowhere else, so
// every launch started from nothing. These cases pin the store that fixes the
// second half of that report, including the part that makes the first half
// affordable: a NEGATIVE answer is a result and is written down, or a sweep
// re-asks every room it has already answered for on every launch.
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
    void theCapDropsTheOldestRowsFirst();

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
    // A second process, a second launch: nothing is carried over in memory.
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
    // One second past the NEGATIVE lifetime: a negative row is stale here and
    // a positive one is not. This is the asymmetry, pinned.
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

QTEST_MAIN(BridgeLabelStoreTest)
#include "BridgeLabelStoreTest.moc"
