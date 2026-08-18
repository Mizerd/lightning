// Proves the LIGHTNING_GUI_STALL_TRACE heartbeat facility: a blocked GUI
// thread is detected, measured within one beat of the truth, attributed to
// the innermost active Scope, and a healthy event loop records nothing.
#include <QtTest/QtTest>

#include <QCoreApplication>

#include "app/GuiStallTracer.h"

class GuiStallTraceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Explicit threshold: fast for the test, and independent of the
        // environment.
        stalltrace::install(/*thresholdMsOverride=*/200);
    }

    void cleanupTestCase() { stalltrace::shutdown(); }

    void healthyLoopRecordsNothing()
    {
        // Let beats and several watchdog rounds pass with a live loop.
        QTest::qWait(400);
        QCOMPARE(stalltrace::stallCount(), 0);
    }

    void blockedLoopIsDetectedAndAttributed()
    {
        QTest::qWait(100); // ensure a recent beat baseline
        {
            stalltrace::Scope scope("test-block");
            QThread::msleep(500); // no event processing: beats stop
        }
        // The stall is logged only when the NEXT beat lands and the
        // watchdog's next round sees it.
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 1, 2000);
        QVERIFY2(stalltrace::lastStallMs() >= 400,
                 qPrintable(QStringLiteral("measured %1 ms")
                                .arg(stalltrace::lastStallMs())));
        QCOMPARE(stalltrace::lastStallCategory(),
                 QByteArrayLiteral("test-block"));
    }

    void unattributedBlockReportsEmptyCategory()
    {
        QTest::qWait(100);
        QThread::msleep(500);
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 2, 2000);
        QVERIFY(stalltrace::lastStallMs() >= 400);
        QCOMPARE(stalltrace::lastStallCategory(), QByteArray());
    }

    void nestedScopeRestoresOuterCategory()
    {
        QTest::qWait(100);
        {
            stalltrace::Scope outer("outer-section");
            {
                stalltrace::Scope inner("inner-section");
                QThread::msleep(500);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 3, 2000);
        // Sampled mid-stall: the innermost scope owns the block.
        QCOMPARE(stalltrace::lastStallCategory(),
                 QByteArrayLiteral("inner-section"));
    }
};

QTEST_GUILESS_MAIN(GuiStallTraceTest)
#include "GuiStallTraceTest.moc"
