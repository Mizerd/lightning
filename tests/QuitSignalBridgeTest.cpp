#include "app/QuitSignalBridge.h"

#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QtTest>

#include <csignal>

// B117: SIGTERM had the default disposition and killed the process with no
// orderly shutdown (no aboutToQuit, no store close). The bridge turns it into
// a call on the event loop. Unix only, like the bridge.
class QuitSignalBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void aSignalTheProcessWasStartedIgnoringStaysIgnored();
    void sigtermQuitsThroughTheEventLoopAndASecondKindOfSignalDoesNotCutItShort();

private:
    QObject m_context;
    int m_received = 0;
    void (*m_sigtermBefore)(int) = nullptr;
};

void QuitSignalBridgeTest::initTestCase()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX signals only");
#else
    // As nohup leaves it.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    QCOMPARE(::sigaction(SIGHUP, &ignore, nullptr), 0);
    struct sigaction before {};
    QCOMPARE(::sigaction(SIGTERM, nullptr, &before), 0);
    m_sigtermBefore = before.sa_handler;

    QVERIFY(lightning::quitsignals::install(&m_context, [this](int signo) {
        m_received = signo;
        QCoreApplication::quit();
    }));
    // Once per process.
    QVERIFY(!lightning::quitsignals::install(&m_context, [](int) {}));
#endif
}

void QuitSignalBridgeTest::aSignalTheProcessWasStartedIgnoringStaysIgnored()
{
#ifdef Q_OS_UNIX
    struct sigaction now {};
    QCOMPARE(::sigaction(SIGHUP, nullptr, &now), 0);
    QVERIFY2(now.sa_handler == SIG_IGN, "an ignored SIGHUP was taken over");
    QCOMPARE(::sigaction(SIGTERM, nullptr, &now), 0);
    QVERIFY(now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN);
    QVERIFY2(now.sa_handler != m_sigtermBefore, "SIGTERM was not routed");
#endif
}

void QuitSignalBridgeTest::sigtermQuitsThroughTheEventLoopAndASecondKindOfSignalDoesNotCutItShort()
{
#ifdef Q_OS_UNIX
    // Without the bridge the SIGTERM kills the test process (QtTest reports the
    // signal): exactly the old behaviour. The SIGINT right behind it stands in
    // for logind's SIGHUP after the SIGTERM of a desktop logout (SIGHUP is
    // ignored in this process, as nohup leaves it): a different signal inside
    // the grace period must not _exit(130) over the orderly quit, which a
    // bridge that forces the exit on ANY second signal does.
    QTimer::singleShot(0, [] {
        ::raise(SIGTERM);
        ::raise(SIGINT);
    });
    // A watchdog, so a bridge that never delivers fails instead of hanging.
    QTimer::singleShot(5000, [] { QCoreApplication::exit(99); });
    const int status = QCoreApplication::exec();
    QCOMPARE(status, 0);
    QCOMPARE(m_received, int(SIGTERM));
#endif
}

QTEST_GUILESS_MAIN(QuitSignalBridgeTest)
#include "QuitSignalBridgeTest.moc"
