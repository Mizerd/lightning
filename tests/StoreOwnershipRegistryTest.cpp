// StoreOwnershipRegistry is RECORD-ONLY in live accounts Phase 0: it records
// which handle uses which store, keyed by what the store IS on disk, and
// refuses nothing. These cases pin the bookkeeping the Phase 1 claim will
// stand on, and that recordOpen() never refuses.
#include "accounts/FileIdentity.h"
#include "accounts/StoreOwnershipRegistry.h"

#include <QAtomicInt>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

using lightning::accounts::FileIdentity;
using lightning::accounts::StoreOwnershipRegistry;

namespace {

// A fake filesystem: the identity is the LOWER-CASED path, so two spellings
// that differ only in case are one store, as on a case-insensitive volume.
// Paths named in `missing` resolve invalid, as stat() on a missing path does.
struct CaseInsensitiveFake {
    QSet<QString> missing;
    FileIdentity operator()(const QString &path) const
    {
        FileIdentity id;
        if (path.isEmpty() || missing.contains(path))
            return id;
        id.volume = 7;
        id.file = qHash(path.toLower());
        id.valid = true;
        return id;
    }
};

FileIdentity alwaysInvalid(const QString &)
{
    return FileIdentity{};
}

} // namespace

class StoreOwnershipRegistryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void recordReleaseCloseLifecycle();
    void anUnconfirmedCloseStaysRetiringAndStillOwnsTheStore();
    void aCaseVariantPairIsOneStore();
    void invalidIdentitiesNeverCollide();
    void recordOpenNeverRefuses();
    void unknownAndZeroSerialsAreIgnored();
    void concurrentNoteClosedFromAPoolIsSafe();
    void theRealResolverNamesWhatThePathIs();

private:
    QTemporaryDir m_dir;
    QString path(const QString &leaf) const { return m_dir.filePath(leaf); }
};

void StoreOwnershipRegistryTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

void StoreOwnershipRegistryTest::recordReleaseCloseLifecycle()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/a/rust-sdk"));
    QVERIFY(!registry.wouldRefuse(store));

    const quint64 serial =
        registry.recordOpen(store, QStringLiteral("slug-a"), StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(serial > 0);
    QList<StoreOwnershipRegistry::Record> all = registry.records();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().serial, serial);
    QCOMPARE(all.first().slug, QStringLiteral("slug-a"));
    QCOMPARE(all.first().absPath, QFileInfo(store).absoluteFilePath());
    QVERIFY(all.first().id.valid);
    QVERIFY(all.first().role == StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(all.first().state == StoreOwnershipRegistry::State::Live);

    // Opening the same store again while it is Live is what Phase 1 refuses.
    quint64 conflict = 0;
    QVERIFY(registry.wouldRefuse(store, &conflict));
    QCOMPARE(conflict, serial);

    registry.noteReleasing(serial);
    all = registry.records();
    QCOMPARE(all.size(), 1);
    QVERIFY(all.first().state == StoreOwnershipRegistry::State::Retiring);
    // Retiring still owns the store: a quick switch back would meet it.
    conflict = 0;
    QVERIFY(registry.wouldRefuse(store, &conflict));
    QCOMPARE(conflict, serial);

    registry.noteClosed(serial, true);
    QVERIFY(registry.records().isEmpty());
    conflict = 99;
    QVERIFY(!registry.wouldRefuse(store, &conflict));
    QCOMPARE(conflict, quint64(0));
}

void StoreOwnershipRegistryTest::anUnconfirmedCloseStaysRetiringAndStillOwnsTheStore()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/b/rust-sdk"));
    const quint64 serial =
        registry.recordOpen(store, QStringLiteral("slug-b"), StoreOwnershipRegistry::Role::Foreground);
    registry.noteReleasing(serial);
    // store_closed=false: SQLite may still hold files open below it.
    registry.noteClosed(serial, false);
    const QList<StoreOwnershipRegistry::Record> all = registry.records();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().serial, serial);
    QVERIFY(all.first().state == StoreOwnershipRegistry::State::Retiring);
    QVERIFY(registry.wouldRefuse(store));

    // An unconfirmed close without a prior noteReleasing ends Retiring too.
    const QString other = path(QStringLiteral("accounts/c/rust-sdk"));
    const quint64 second =
        registry.recordOpen(other, QStringLiteral("slug-c"), StoreOwnershipRegistry::Role::Foreground);
    registry.noteClosed(second, false);
    const auto forOther = registry.recordsFor(CaseInsensitiveFake{}(other));
    QCOMPARE(forOther.size(), 1);
    QVERIFY(forOther.first().state == StoreOwnershipRegistry::State::Retiring);
}

void StoreOwnershipRegistryTest::aCaseVariantPairIsOneStore()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString typed = path(QStringLiteral("accounts/Alice/rust-sdk"));
    const QString canonical = path(QStringLiteral("accounts/alice/rust-sdk"));
    QVERIFY(typed != canonical);
    const quint64 serial = registry.recordOpen(typed, QStringLiteral("alice"),
                                               StoreOwnershipRegistry::Role::Foreground);
    quint64 conflict = 0;
    QVERIFY2(registry.wouldRefuse(canonical, &conflict),
             "two spellings of one store must resolve to one identity");
    QCOMPARE(conflict, serial);
    const QList<StoreOwnershipRegistry::Record> same =
        registry.recordsFor(CaseInsensitiveFake{}(canonical));
    QCOMPARE(same.size(), 1);
    QCOMPARE(same.first().serial, serial);
    // A genuinely different store does not collide.
    QVERIFY(!registry.wouldRefuse(path(QStringLiteral("accounts/bob/rust-sdk"))));
}

void StoreOwnershipRegistryTest::invalidIdentitiesNeverCollide()
{
    // Default-constructed ids compare equal, which is exactly why the
    // registry must never treat two invalid ids as one store.
    QVERIFY(FileIdentity{} == FileIdentity{});

    StoreOwnershipRegistry registry{alwaysInvalid};
    const QString a = path(QStringLiteral("unreadable/a"));
    const QString b = path(QStringLiteral("unreadable/b"));
    const quint64 first =
        registry.recordOpen(a, QStringLiteral("a"), StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(first > 0);
    QVERIFY(!registry.records().first().id.valid);
    quint64 conflict = 42;
    QVERIFY(!registry.wouldRefuse(a, &conflict));
    QCOMPARE(conflict, quint64(0));
    QVERIFY(!registry.wouldRefuse(b));
    const quint64 second =
        registry.recordOpen(b, QStringLiteral("b"), StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(second > first);
    QCOMPARE(registry.records().size(), 2);
    QVERIFY(registry.recordsFor(FileIdentity{}).isEmpty());

    // A missing path among valid ones: invalid, and collides with nothing.
    CaseInsensitiveFake fake;
    const QString gone = path(QStringLiteral("gone"));
    fake.missing.insert(gone);
    StoreOwnershipRegistry mixed{fake};
    mixed.recordOpen(gone, QStringLiteral("g"), StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(!mixed.wouldRefuse(gone));
}

void StoreOwnershipRegistryTest::recordOpenNeverRefuses()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/d/rust-sdk"));
    QList<quint64> serials;
    for (int i = 0; i < 3; ++i) {
        // Phase 0 records the double open; it does not stop it.
        const quint64 serial = registry.recordOpen(
            store, QStringLiteral("d"), StoreOwnershipRegistry::Role::Foreground);
        QVERIFY(serial > 0);
        QVERIFY(!serials.contains(serial));
        if (!serials.isEmpty())
            QVERIFY(serial > serials.last());
        serials << serial;
    }
    const QList<StoreOwnershipRegistry::Record> same =
        registry.recordsFor(CaseInsensitiveFake{}(store));
    QCOMPARE(same.size(), 3);
    for (const auto &record : same)
        QVERIFY(record.state == StoreOwnershipRegistry::State::Live);
    // The verdict names the OLDEST owner.
    quint64 conflict = 0;
    QVERIFY(registry.wouldRefuse(store, &conflict));
    QCOMPARE(conflict, serials.first());

    registry.clearForTest();
    QVERIFY(registry.records().isEmpty());
    QCOMPARE(registry.recordOpen(store, QStringLiteral("d"),
                                 StoreOwnershipRegistry::Role::Foreground),
             quint64(1));
}

void StoreOwnershipRegistryTest::unknownAndZeroSerialsAreIgnored()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/e/rust-sdk"));
    const quint64 serial = registry.recordOpen(store, QStringLiteral("e"),
                                               StoreOwnershipRegistry::Role::Foreground);
    registry.noteReleasing(0);
    registry.noteReleasing(serial + 100);
    registry.noteClosed(0, true);
    registry.noteClosed(serial + 100, true);
    const QList<StoreOwnershipRegistry::Record> all = registry.records();
    QCOMPARE(all.size(), 1);
    QVERIFY(all.first().state == StoreOwnershipRegistry::State::Live);
}

void StoreOwnershipRegistryTest::concurrentNoteClosedFromAPoolIsSafe()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    constexpr int kRecords = 400;
    QList<quint64> serials;
    for (int i = 0; i < kRecords; ++i) {
        serials << registry.recordOpen(path(QStringLiteral("s/%1").arg(i)), QStringLiteral("s"),
                                       StoreOwnershipRegistry::Role::Foreground);
        registry.noteReleasing(serials.last());
    }
    QThreadPool pool;
    pool.setMaxThreadCount(8);
    QAtomicInt ran = 0;
    for (int i = 0; i < kRecords; ++i) {
        const quint64 serial = serials.at(i);
        // Even: confirmed closed (removed). Odd: unconfirmed (stays Retiring).
        const bool closed = (i % 2) == 0;
        pool.start([&registry, &ran, serial, closed] {
            registry.noteClosed(serial, closed);
            // Readers racing the writers, as the GUI thread's next
            // ensureRustHandle would.
            (void)registry.wouldRefuse(QStringLiteral("/nowhere"));
            (void)registry.records();
            ran.fetchAndAddOrdered(1);
        });
    }
    // Opens on this thread while the pool closes.
    for (int i = 0; i < 50; ++i)
        registry.recordOpen(path(QStringLiteral("late/%1").arg(i)), QStringLiteral("l"),
                            StoreOwnershipRegistry::Role::Foreground);
    QVERIFY(pool.waitForDone(30000));
    QCOMPARE(ran.loadAcquire(), kRecords);

    const QList<StoreOwnershipRegistry::Record> left = registry.records();
    QCOMPARE(left.size(), kRecords / 2 + 50);
    int retiring = 0;
    QSet<quint64> seen;
    for (const auto &record : left) {
        QVERIFY(!seen.contains(record.serial));
        seen.insert(record.serial);
        if (record.state == StoreOwnershipRegistry::State::Retiring)
            ++retiring;
    }
    QCOMPARE(retiring, kRecords / 2);
}

void StoreOwnershipRegistryTest::theRealResolverNamesWhatThePathIs()
{
    QVERIFY(QDir().mkpath(path(QStringLiteral("real/one"))));
    QVERIFY(QDir().mkpath(path(QStringLiteral("real/two"))));
    const FileIdentity one = lightning::accounts::fileIdentityOf(path(QStringLiteral("real/one")));
    const FileIdentity oneAgain =
        lightning::accounts::fileIdentityOf(path(QStringLiteral("real/two/../one")));
    const FileIdentity two = lightning::accounts::fileIdentityOf(path(QStringLiteral("real/two")));
    const FileIdentity missing =
        lightning::accounts::fileIdentityOf(path(QStringLiteral("real/absent")));
    QVERIFY(one.valid);
    QVERIFY(two.valid);
    QVERIFY(oneAgain.valid);
    QVERIFY(one == oneAgain);
    QVERIFY(!(one == two));
    QCOMPARE(qHash(one), qHash(oneAgain));
    QVERIFY(!missing.valid);
    QVERIFY(!lightning::accounts::fileIdentityOf(QString()).valid);

#ifndef Q_OS_WIN
    // stat() follows a link to the store it names.
    const QString link = path(QStringLiteral("real/link-to-one"));
    QVERIFY(QFile::link(path(QStringLiteral("real/one")), link));
    QVERIFY(lightning::accounts::fileIdentityOf(link) == one);
#endif

    // The default registry uses the real resolver.
    StoreOwnershipRegistry registry;
    const quint64 serial = registry.recordOpen(path(QStringLiteral("real/one")),
                                               QStringLiteral("r"),
                                               StoreOwnershipRegistry::Role::Foreground);
    quint64 conflict = 0;
    QVERIFY(registry.wouldRefuse(path(QStringLiteral("real/two/../one")), &conflict));
    QCOMPARE(conflict, serial);
    QVERIFY(!registry.wouldRefuse(path(QStringLiteral("real/two"))));
}

QTEST_GUILESS_MAIN(StoreOwnershipRegistryTest)
#include "StoreOwnershipRegistryTest.moc"
