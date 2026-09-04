// Retiring a Rust client must not happen on the thread that draws the window.
//
// It used to. `mx_rust_shutdown_tasks` joins managed tasks under budgets and
// `mx_rust_destroy` drops the tokio runtime — which blocks until every
// in-flight `spawn_blocking` finishes, SQLite closes included — and both ran
// inside `RustSdkMatrixClient::releaseRustHandle()`, on the GUI thread,
// reached from `AppController::switchToAccount`. That is the reported
// multi-second freeze on an account switch.
//
// The obvious test — "an account switch is fast" — is the one that cannot
// catch a regression, because on a small account the teardown is a few
// milliseconds whether it blocks or not. Measured on two real fixture
// accounts: 1-4 ms. A test built on that would pass on the old code.
//
// So this measures the PROPERTY instead: the caller returns while the work is
// still outstanding. It uses a REAL Rust client with a real tokio runtime and
// a real on-disk store, because a fake would not have the runtime whose drop
// is the expensive part.

#include "matrix/RustSdkMatrixClient.h"
#include "matrix_rust.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
// A real client, with its own store directory. mx_rust_create builds the
// bridge and its tokio runtime; it performs no network I/O, so this is
// deterministic and offline.
void *createRealClient(const QTemporaryDir &dir, const char *name)
{
    const QByteArray path = (dir.path() + QLatin1Char('/')
                             + QLatin1String(name)).toUtf8();
    return mx_rust_create(path.constData());
}
} // namespace

class RustRetirementTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // THE ONE THAT WOULD HAVE CAUGHT THE FREEZE.
    //
    // NOT a time budget. An empty store closes in about a millisecond, so a
    // "returned in under 250 ms" assertion passes whether the work ran inline
    // or not — verified by mutation: making retirement synchronous again left
    // that version of this test green. It was measuring nothing.
    //
    // The property that actually distinguishes the two is that the work is
    // STILL OUTSTANDING when the caller returns. Done inline, the pool is
    // empty by then and `waitForRustRetirement(0)` reports drained; deferred,
    // it cannot. Several clients are handed over so the answer does not
    // depend on winning a race against a single fast close.
    void retiringClientsLeavesTheWorkOutstanding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Drain anything a previous case left behind, so "not drained" below
        // can only be about the clients handed over in this one.
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));

        for (int i = 0; i < 6; ++i) {
            void *client = createRealClient(
                dir, qPrintable(QStringLiteral("outstanding-%1").arg(i)));
            QVERIFY(client);
            RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        }

        QVERIFY2(!RustSdkMatrixClient::waitForRustRetirement(0),
                 "retirement had already finished when the caller returned, "
                 "so it ran on the caller's thread");

        // ...and it really does finish, rather than being dropped.
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(60000));
    }

    // Rapid switching hands over several clients before any has finished.
    // Each owns its own store, so they must all close — a retirement that
    // dropped one on the floor would leave a tokio runtime and an open
    // SQLite store behind for the life of the process.
    void severalClientsCanBeRetiredAtOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QElapsedTimer handOff;
        handOff.start();
        for (int i = 0; i < 4; ++i) {
            void *client = createRealClient(
                dir, qPrintable(QStringLiteral("store-%1").arg(i)));
            QVERIFY(client);
            RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        }
        const qint64 callerMs = handOff.elapsed();
        QVERIFY2(callerMs < 500,
                 qPrintable(QStringLiteral("four hand-offs blocked for %1 ms")
                                .arg(callerMs)));

        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(60000));
    }

    // The deletion paths depend on this: waiting must actually mean the store
    // is closed, or removing the directory races an open SQLite connection.
    // After the wait, the store's files must be re-openable and removable.
    void afterWaitingTheStoreIsReallyClosed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.path() + QStringLiteral("/store-close");
        void *client = createRealClient(dir, "store-close");
        QVERIFY(client);

        RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));

        // The whole point of the wait: the directory can now be removed
        // without racing anything.
        if (QFileInfo::exists(storePath))
            QVERIFY(QDir(storePath).removeRecursively());
    }

    // A null handle is a no-op rather than a crash: releaseRustHandle() can
    // reach the hand-off with nothing to retire on a client that never
    // logged in.
    void retiringNothingIsSafe()
    {
        RustSdkMatrixClient::retireRustHandleAsync(nullptr, QString());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(5000));
    }
};

QTEST_MAIN(RustRetirementTest)
#include "RustRetirementTest.moc"
