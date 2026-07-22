#include <QtTest>

#include "app/BackendSelection.h"

// The compiled default backend must be Rust in a Rust-enabled build (so a
// packaged desktop launcher gets SDK-owned E2EE with no flag) and HTTP in a
// build without the Rust SDK. --backend=NAME parsing must be case- and
// whitespace-insensitive and reject unknown names.
class BackendSelectionTest : public QObject
{
    Q_OBJECT
private slots:
    void compiledDefaultMatchesBuild()
    {
        // Compare as int so QTest does not pull in AppController::staticMetaObject
        // (the Q_ENUM reflection lives in AppController.cpp, which this focused
        // test deliberately does not link).
#ifdef ENABLE_RUST_SDK_BACKEND
        QCOMPARE(int(lightning::defaultBackend()), int(AppController::RustBackend));
#else
        QCOMPARE(int(lightning::defaultBackend()), int(AppController::HttpBackend));
#endif
    }

    void parsesKnownNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("expected");
        QTest::newRow("rust")        << "rust"   << int(AppController::RustBackend);
        QTest::newRow("http")        << "http"   << int(AppController::HttpBackend);
        QTest::newRow("mock")        << "mock"   << int(AppController::MockBackend);
        QTest::newRow("upper-rust")  << "RUST"   << int(AppController::RustBackend);
        QTest::newRow("mixed-http")  << "Http"   << int(AppController::HttpBackend);
        QTest::newRow("padded-mock") << "  mock" << int(AppController::MockBackend);
    }

    void parsesKnownNames()
    {
        QFETCH(QString, name);
        QFETCH(int, expected);
        bool ok = false;
        const AppController::Backend b = lightning::backendFromName(name, &ok);
        QVERIFY(ok);
        QCOMPARE(int(b), expected);
    }

    void rejectsUnknownNames()
    {
        bool ok = true;
        lightning::backendFromName(QStringLiteral("bogus"), &ok);
        QVERIFY(!ok);
        ok = true;
        lightning::backendFromName(QString(), &ok);
        QVERIFY(!ok);
    }

    void nameRoundTrips()
    {
        QCOMPARE(lightning::backendNameFor(AppController::RustBackend), QStringLiteral("rust"));
        QCOMPARE(lightning::backendNameFor(AppController::HttpBackend), QStringLiteral("http"));
        QCOMPARE(lightning::backendNameFor(AppController::MockBackend), QStringLiteral("mock"));
    }
};

QTEST_APPLESS_MAIN(BackendSelectionTest)
#include "BackendSelectionTest.moc"
