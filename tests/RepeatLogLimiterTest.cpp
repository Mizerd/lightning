#include "app/RepeatLogLimiter.h"

#include <QFile>
#include <QtTest/QtTest>

#include <atomic>
#include <thread>
#include <vector>

using lightning::logging::RepeatLogLimiter;

namespace {
const char *const kCategory = "qt.core.qobject.connect";
const QString kRunaway =
    QStringLiteral("QSocketNotifier: Invalid socket 124 and type 'Read', disabling...");

RepeatLogLimiter::Limits limits()
{
    RepeatLogLimiter::Limits l;
    l.burst = 20;
    l.windowMs = 1000;
    l.summaryEveryMs = 10000;
    return l;
}

struct Fed {
    int passed = 0;
    int summaries = 0;
    quint64 summarised = 0;
    qint64 endMs = 0;
};

quint64 countIn(const QString &summary)
{
    const QStringList words = summary.split(QLatin1Char(' '));
    return words.size() > 2 ? words.at(2).toULongLong() : 0;
}

Fed feed(RepeatLogLimiter &limiter, const QString &message, qint64 count,
         qint64 startMs, double stepMs)
{
    Fed fed;
    for (qint64 i = 0; i < count; ++i) {
        const qint64 now = startMs + qint64(double(i) * stepMs);
        const auto d = limiter.admit(QtWarningMsg, kCategory, message, now);
        fed.passed += d.pass ? 1 : 0;
        for (const auto &s : d.summaries) {
            ++fed.summaries;
            fed.summarised += countIn(s.text);
        }
        fed.endMs = now;
    }
    return fed;
}
}

class RepeatLogLimiterTest : public QObject
{
    Q_OBJECT

private slots:
    void aRunawayCostsTheBurstPlusOneLinePerInterval()
    {
        RepeatLogLimiter limiter(limits());
        const qint64 lines = 420LL * 2000;
        const Fed fed = feed(limiter, kRunaway, lines, 0, 420000.0 / double(lines));
        QCOMPARE(fed.passed, 20);
        QVERIFY2(fed.summaries >= 41 && fed.summaries <= 43,
                 qPrintable(QStringLiteral("%1 summaries").arg(fed.summaries)));

        const auto d = limiter.admit(QtInfoMsg, "lightning.calls.sfu",
                                     QStringLiteral("frames decrypted"),
                                     fed.endMs + 1500);
        QVERIFY(d.pass);
        QCOMPARE(d.summaries.size(), 1);
        const quint64 accounted = fed.summarised + countIn(d.summaries.first().text);
        QCOMPARE(accounted, quint64(lines - 20));
        QCOMPARE(limiter.suppressedTotal(), quint64(lines - 20));
    }

    void theSummaryNamesTheMessageAndItsCount()
    {
        RepeatLogLimiter limiter(limits());
        for (int i = 0; i < 25; ++i)
            limiter.admit(QtWarningMsg, kCategory, kRunaway, 100);
        RepeatLogLimiter::Decision d;
        for (qint64 t = 200; t <= 10100; t += 100) {
            d = limiter.admit(QtWarningMsg, kCategory, kRunaway, t);
            QVERIFY(!d.pass);
            if (t < 10100)
                QVERIFY(d.summaries.isEmpty());
        }
        QCOMPARE(d.summaries.size(), 1);
        const RepeatLogLimiter::Summary &s = d.summaries.first();
        QCOMPARE(s.type, QtWarningMsg);
        QCOMPARE(s.category, QByteArray(kCategory));
        QCOMPARE(s.text, QStringLiteral("message repeated 105 times in 10.0 s, "
                                        "not logged: ")
                             + kRunaway);
    }

    void aSteadyMessageBelowTheBurstIsNeverSuppressed()
    {
        RepeatLogLimiter limiter(limits());
        const Fed fed = feed(limiter, QStringLiteral("rtp stats"), 20 * 60, 0, 50.0);
        QCOMPARE(fed.passed, 20 * 60);
        QCOMPARE(fed.summaries, 0);
    }

    void aFloodNeverHidesADifferentMessage()
    {
        RepeatLogLimiter limiter(limits());
        int otherPassed = 0;
        for (int i = 0; i < 5000; ++i) {
            limiter.admit(QtWarningMsg, kCategory, kRunaway, i / 10);
            if (i % 100 == 0) {
                const auto d = limiter.admit(
                    QtInfoMsg, "lightning.calls.sfu",
                    QStringLiteral("participant %1 joined").arg(i), i / 10);
                otherPassed += d.pass ? 1 : 0;
            }
        }
        QCOMPARE(otherPassed, 50);
        QVERIFY(limiter.admit(QtWarningMsg, "other.category", kRunaway, 600).pass);
        QVERIFY(limiter.admit(QtCriticalMsg, kCategory, kRunaway, 600).pass);
    }

    void aQuietWindowEndsTheFlood()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 100, 0, 1.0);
        auto d = limiter.admit(QtWarningMsg, kCategory, kRunaway, 1200);
        QVERIFY(d.pass);
        QCOMPARE(d.summaries.size(), 1);
        QCOMPARE(countIn(d.summaries.first().text), quint64(80));
        const Fed again = feed(limiter, kRunaway, 30, 1201, 1.0);
        QCOMPARE(again.passed, 19);
    }

    void anEvictedFloodIsSummarisedNotLost()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 50, 0, 0.01);
        int summarised = 0;
        for (int i = 0; i < RepeatLogLimiter::kSlots; ++i) {
            const auto d = limiter.admit(QtInfoMsg, "x",
                                         QStringLiteral("distinct %1").arg(i), 1 + i);
            for (const auto &s : d.summaries)
                summarised += int(countIn(s.text));
            QVERIFY(d.pass);
        }
        QCOMPARE(summarised, 30);
    }

    void theQuoteIsBounded()
    {
        const QString longMessage(5000, QLatin1Char('x'));
        const QString text = RepeatLogLimiter::summaryText(3, 1500, longMessage);
        QVERIFY(text.size() < RepeatLogLimiter::kQuoteChars + 80);
        QVERIFY(text.endsWith(QStringLiteral("...")));

        const QString emoji = QString(RepeatLogLimiter::kQuoteChars - 1, QLatin1Char('x'))
            + QString::fromUcs4(U"\U0001F600") + QStringLiteral("tail");
        const QString cut = RepeatLogLimiter::summaryText(3, 1500, emoji);
        QVERIFY(cut.endsWith(QString(RepeatLogLimiter::kQuoteChars - 1, QLatin1Char('x'))
                             + QStringLiteral("...")));
        for (const QChar c : cut)
            QVERIFY(!c.isSurrogate());
    }

    void takePendingAccountsForWhatIsLeft()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 70, 0, 0.1);
        auto pending = limiter.takePending(10);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(countIn(pending.first().text), quint64(50));
        QVERIFY(limiter.takePending(20).isEmpty());
    }

    void concurrentFloodsAreCountedExactly()
    {
        RepeatLogLimiter limiter(limits());
        std::atomic<int> passed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 20000; ++i) {
                    if (limiter.admit(QtWarningMsg, kCategory, kRunaway, 5).pass)
                        passed.fetch_add(1);
                }
            });
        }
        for (std::thread &t : threads)
            t.join();
        QCOMPARE(passed.load(), 20);
        QCOMPARE(limiter.suppressedTotal(), quint64(4 * 20000 - 20));
    }

    void theApplicationInstallsItBetweenTheLogFileAndTheVaapiGate()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const int logFile = code.indexOf(QStringLiteral("installLogFile(pf.logFilePath);"));
        const int vaapi = code.indexOf(QStringLiteral("    installVaapiLogGate();"));
        const int limiter = code.indexOf(QStringLiteral("    installRepeatLimiter();"));
        QVERIFY2(limiter >= 0, "main() never installs the repeat limiter");
        QVERIFY(logFile >= 0 && vaapi >= 0);
        QVERIFY2(limiter > logFile,
                 "the repeat limiter is not around the --log-file handler");
        QVERIFY2(limiter < vaapi,
                 "the repeat limiter is outside the VAAPI gate and hides "
                 "the export warnings that gate counts");
        QVERIFY2(code.contains(QStringLiteral("std::atexit(flushRepeatLimiterAtExit);")),
                 "a flood that ends with the process is never summarised");
    }
};

QTEST_GUILESS_MAIN(RepeatLogLimiterTest)
#include "RepeatLogLimiterTest.moc"
