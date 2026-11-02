#include "app/QuitSignalBridge.h"

#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QtTest>

#include <csignal>

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
    QTimer::singleShot(0, [] {
        ::raise(SIGTERM);
        ::raise(SIGINT);
    });
    QTimer::singleShot(5000, [] { QCoreApplication::exit(99); });
    const int status = QCoreApplication::exec();
    QCOMPARE(status, 0);
    QCOMPARE(m_received, int(SIGTERM));
#endif
}

QTEST_GUILESS_MAIN(QuitSignalBridgeTest)
#include "QuitSignalBridgeTest.moc"
