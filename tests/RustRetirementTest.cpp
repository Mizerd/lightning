// Retiring a Rust client (`mx_rust_shutdown_tasks`, and `mx_rust_destroy`
// dropping the tokio runtime) must not happen on the GUI thread.
//
// "An account switch is fast" cannot catch a regression: a small store closes
// in milliseconds either way. So this measures the property that the caller
// returns while the work is still outstanding, with a real Rust client,
// runtime and on-disk store.

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
    // The work is still outstanding when the caller returns: done inline, the
    // pool would be empty and `waitForRustRetirement(0)` would report drained.
    // Several clients are handed over so the answer does not depend on
    // winning a race against one fast close.
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

    // Rapid switching hands over several clients; each owns its own store and
    // all must close.
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
