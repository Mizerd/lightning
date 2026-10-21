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
    // Live accounts Phase 1: the claims. Not enforced they are Phase 0's
    // recordOpen() exactly; enforced (Labs) they refuse.
    void unenforcedClaimsRecordExactlyAsRecordOpen();
    void anEnforcedClaimRefusesEveryOwnerState();
    void anotherAccountsRecordOnTheSameStoreIsADuplicate();
    void deletionIsRefusedWhileAnotherRecordHoldsTheStore();
    void enforcementIsOneWay();
    void anUnreadableIdentityIsComparedByPathUnderEnforcement();
    void holdOnTellsAPendingCloseFromAnUnconfirmedOne();

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

namespace {
using Verdict = StoreOwnershipRegistry::ClaimVerdict;
using Role = StoreOwnershipRegistry::Role;

bool sameRecords(const QList<StoreOwnershipRegistry::Record> &a,
                 const QList<StoreOwnershipRegistry::Record> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const auto &x = a.at(i);
        const auto &y = b.at(i);
        if (x.serial != y.serial || !(x.id == y.id) || x.absPath != y.absPath
            || x.slug != y.slug || x.role != y.role || x.state != y.state
            || x.closeUnconfirmed != y.closeUnconfirmed)
            return false;
    }
    return true;
}
} // namespace

// Flag-off parity: with Labs off the client calls claimOpen() where Phase 0
// called recordOpen(). Same records, same serials, double opens recorded, no
// refusal, and every deletion Granted.
void StoreOwnershipRegistryTest::unenforcedClaimsRecordExactlyAsRecordOpen()
{
    StoreOwnershipRegistry recorded{CaseInsensitiveFake{}};
    StoreOwnershipRegistry claimed{CaseInsensitiveFake{}};
    QVERIFY(!claimed.enforced());
    const QStringList stores{path(QStringLiteral("accounts/p/rust-sdk")),
                             path(QStringLiteral("accounts/P/rust-sdk")),   // case variant
                             path(QStringLiteral("accounts/p/rust-sdk")),   // double open
                             path(QStringLiteral("accounts/q/rust-sdk"))};
    const QStringList slugs{QStringLiteral("p"), QStringLiteral("P"), QStringLiteral("p"),
                            QStringLiteral("q")};
    QList<quint64> serials;
    for (qsizetype i = 0; i < stores.size(); ++i) {
        const quint64 viaRecord = recorded.recordOpen(stores.at(i), slugs.at(i), Role::Foreground);
        quint64 viaClaim = 0;
        quint64 conflict = 77;
        QCOMPARE(claimed.claimOpen(stores.at(i), slugs.at(i), Role::Foreground, &viaClaim,
                                   &conflict),
                 Verdict::Granted);
        QCOMPARE(viaClaim, viaRecord);
        QCOMPARE(conflict, quint64(0));
        // Every deletion is Granted while not enforced, owners or not.
        QCOMPARE(claimed.claimForDeletion(stores.at(i)), Verdict::Granted);
        serials << viaClaim;
    }
    recorded.noteReleasing(serials.at(0));
    claimed.noteReleasing(serials.at(0));
    recorded.noteClosed(serials.at(1), false);
    claimed.noteClosed(serials.at(1), false);
    recorded.noteClosed(serials.at(3), true);
    claimed.noteClosed(serials.at(3), true);
    // promote() changes the role only; recordOpen's twin gets the same edit.
    claimed.promote(serials.at(2), Role::Foreground);
    QVERIFY(sameRecords(recorded.records(), claimed.records()));
    QCOMPARE(claimed.records().size(), 3);
}

void StoreOwnershipRegistryTest::anEnforcedClaimRefusesEveryOwnerState()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    registry.setEnforced(true);
    const QString store = path(QStringLiteral("accounts/h/rust-sdk"));
    quint64 host = 0;
    QCOMPARE(registry.claimOpen(store, QStringLiteral("h"), Role::Starting, &host), Verdict::Granted);
    QVERIFY(host > 0);

    const auto again = [&](quint64 *conflict) {
        quint64 serial = 99;
        const Verdict v = registry.claimOpen(store, QStringLiteral("h"), Role::Foreground, &serial,
                                             conflict);
        // A refusal records nothing and hands out no serial.
        if (v != Verdict::Granted)
            [&] { QCOMPARE(serial, quint64(0)); }();
        return v;
    };
    quint64 conflict = 0;
    QCOMPARE(again(&conflict), Verdict::RefusedStarting);
    QCOMPARE(conflict, host);
    registry.promote(host, Role::Host);
    QCOMPARE(again(&conflict), Verdict::RefusedLive);
    registry.noteReleasing(host);
    QCOMPARE(again(&conflict), Verdict::RefusedRetiring);
    // An unconfirmed close keeps it Retiring for the rest of the process.
    registry.noteClosed(host, false);
    QCOMPARE(again(&conflict), Verdict::RefusedRetiring);
    QCOMPARE(registry.records().size(), 1);

    // Only a confirmed close frees it.
    registry.noteClosed(host, true);
    quint64 next = 0;
    QCOMPARE(registry.claimOpen(store, QStringLiteral("h"), Role::Foreground, &next),
             Verdict::Granted);
    QVERIFY(next > host);
    QCOMPARE(registry.records().size(), 1);
}

void StoreOwnershipRegistryTest::anotherAccountsRecordOnTheSameStoreIsADuplicate()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    registry.setEnforced(true);
    quint64 first = 0;
    QCOMPARE(registry.claimOpen(path(QStringLiteral("accounts/Alice/rust-sdk")),
                                QStringLiteral("Alice"), Role::Host, &first),
             Verdict::Granted);
    quint64 conflict = 0;
    quint64 second = 5;
    QCOMPARE(registry.claimOpen(path(QStringLiteral("accounts/alice/rust-sdk")),
                                QStringLiteral("alice"), Role::Foreground, &second, &conflict),
             Verdict::RefusedDuplicateRecord);
    QCOMPARE(conflict, first);
    QCOMPARE(second, quint64(0));
    QCOMPARE(registry.records().size(), 1);
}

void StoreOwnershipRegistryTest::deletionIsRefusedWhileAnotherRecordHoldsTheStore()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/del/rust-sdk"));
    quint64 host = 0;
    registry.setEnforced(true);
    QCOMPARE(registry.claimOpen(store, QStringLiteral("del"), Role::Host, &host), Verdict::Granted);
    QCOMPARE(registry.claimForDeletion(store), Verdict::RefusedLive);
    // Spelled differently, still the same store.
    QCOMPARE(registry.claimForDeletion(path(QStringLiteral("accounts/DEL/rust-sdk"))),
             Verdict::RefusedLive);
    // The caller's own record does not block its own deletion.
    QCOMPARE(registry.claimForDeletion(store, host), Verdict::Granted);
    registry.noteReleasing(host);
    QCOMPARE(registry.claimForDeletion(store), Verdict::RefusedRetiring);
    registry.noteClosed(host, true);
    QCOMPARE(registry.claimForDeletion(store), Verdict::Granted);
    // Another store is never refused on this one's account.
    quint64 other = 0;
    QCOMPARE(registry.claimOpen(store, QStringLiteral("del"), Role::Host, &other), Verdict::Granted);
    QCOMPARE(registry.claimForDeletion(path(QStringLiteral("accounts/elsewhere/rust-sdk"))),
             Verdict::Granted);
}

void StoreOwnershipRegistryTest::enforcementIsOneWay()
{
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    registry.setEnforced(false);
    QVERIFY(!registry.enforced());
    registry.setEnforced(true);
    registry.setEnforced(false);
    QVERIFY2(registry.enforced(), "a runtime Labs off must keep the claims on until restart");
    const QString store = path(QStringLiteral("accounts/one-way/rust-sdk"));
    quint64 serial = 0;
    QCOMPARE(registry.claimOpen(store, QStringLiteral("w"), Role::Host, &serial), Verdict::Granted);
    QCOMPARE(registry.claimOpen(store, QStringLiteral("w"), Role::Host, nullptr), Verdict::RefusedLive);
}

void StoreOwnershipRegistryTest::anUnreadableIdentityIsComparedByPathUnderEnforcement()
{
    StoreOwnershipRegistry registry{alwaysInvalid};
    registry.setEnforced(true);
    const QString a = path(QStringLiteral("unreadable-enforced/a"));
    quint64 serial = 0;
    QCOMPARE(registry.claimOpen(a, QStringLiteral("a"), Role::Host, &serial), Verdict::Granted);
    // Two invalid identities prove nothing about sharing, so the spelling
    // decides: the same path is refused, never waved through.
    QCOMPARE(registry.claimOpen(a, QStringLiteral("a"), Role::Foreground, nullptr),
             Verdict::RefusedLive);
    QCOMPARE(registry.claimForDeletion(a), Verdict::RefusedLive);
    QCOMPARE(registry.claimOpen(path(QStringLiteral("unreadable-enforced/b")), QStringLiteral("b"),
                                Role::Foreground, nullptr),
             Verdict::Granted);
}

// Live accounts' pool waits for a store whose close may still come and gives
// up only on one REPORTED unconfirmed (review M6), and never confuses the
// foreground's own record of the same account with another account's
// (review M7: both claim under the recorded store slug).
void StoreOwnershipRegistryTest::holdOnTellsAPendingCloseFromAnUnconfirmedOne()
{
    using Hold = StoreOwnershipRegistry::Hold;
    StoreOwnershipRegistry registry{CaseInsensitiveFake{}};
    const QString store = path(QStringLiteral("accounts/h/rust-sdk"));
    QCOMPARE(registry.holdOn(store, QStringLiteral("h")), Hold::None);
    const quint64 live = registry.recordOpen(store, QStringLiteral("h"), Role::Foreground);
    QCOMPARE(registry.holdOn(store, QStringLiteral("h")), Hold::Held);
    // The same store spelled in another case is the same store.
    QCOMPARE(registry.holdOn(path(QStringLiteral("accounts/H/rust-sdk")), QStringLiteral("h")),
             Hold::Held);
    registry.noteReleasing(live);
    QCOMPARE(registry.holdOn(store, QStringLiteral("h")), Hold::Held);   // close in progress
    QVERIFY(!registry.records().first().closeUnconfirmed);
    registry.noteClosed(live, false);
    QCOMPARE(registry.holdOn(store, QStringLiteral("h")), Hold::Unconfirmed);
    QVERIFY(registry.records().first().closeUnconfirmed);
    QCOMPARE(registry.records().first().state, StoreOwnershipRegistry::State::Retiring);
    // Another account's record on this store is a duplicate, whatever its state.
    const QString other = path(QStringLiteral("accounts/o/rust-sdk"));
    registry.recordOpen(other, QStringLiteral("someone-else"), Role::Host);
    QCOMPARE(registry.holdOn(other, QStringLiteral("o")), Hold::Duplicate);
    QCOMPARE(registry.holdOn(other, QStringLiteral("someone-else")), Hold::Held);
    // A confirmed close leaves nothing.
    const quint64 closing = registry.recordOpen(path(QStringLiteral("accounts/c/rust-sdk")),
                                                QStringLiteral("c"), Role::Foreground);
    registry.noteReleasing(closing);
    registry.noteClosed(closing, true);
    QCOMPARE(registry.holdOn(path(QStringLiteral("accounts/c/rust-sdk")), QStringLiteral("c")),
             Hold::None);
}

QTEST_GUILESS_MAIN(StoreOwnershipRegistryTest)
#include "StoreOwnershipRegistryTest.moc"
