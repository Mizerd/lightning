// The outermost message handler's limiter. Measured 2026-10-07: one Qt
// warning repeated ~95 million times in 7 minutes filled a Flatpak user's disk
// through --log-file and stderr at once. A message repeated without end must
// cost a bounded number of lines, say how many it suppressed, and never hide
// a different message.
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

/// Feeds `count` copies of `message` one `stepUs` apart from `startMs` and
/// counts what would reach the log.
struct Fed {
    int passed = 0;
    int summaries = 0;
    quint64 summarised = 0;
    qint64 endMs = 0;
};

quint64 countIn(const QString &summary)
{
    // "message repeated N times in ..."
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
} // namespace

class RepeatLogLimiterTest : public QObject
{
    Q_OBJECT

private slots:
    // The measured runaway, in virtual time: ~226,000 lines a second for 7
    // minutes. What reaches the log is the burst plus one summary per 10 s,
    // and every suppressed line is accounted for once the flood stops.
    void aRunawayCostsTheBurstPlusOneLinePerInterval()
    {
        RepeatLogLimiter limiter(limits());
        const qint64 lines = 420LL * 2000; // 7 min at 2000/s (scaled down)
        const Fed fed = feed(limiter, kRunaway, lines, 0, 420000.0 / double(lines));
        QCOMPARE(fed.passed, 20);
        QVERIFY2(fed.summaries >= 41 && fed.summaries <= 43,
                 qPrintable(QStringLiteral("%1 summaries").arg(fed.summaries)));

        // The flood stops; the next line of any kind brings the remainder.
        const auto d = limiter.admit(QtInfoMsg, "lightning.calls.sfu",
                                     QStringLiteral("frames decrypted"),
                                     fed.endMs + 1500);
        QVERIFY(d.pass);
        QCOMPARE(d.summaries.size(), 1);
        const quint64 accounted = fed.summarised + countIn(d.summaries.first().text);
        QCOMPARE(accounted, quint64(lines - 20));
        QCOMPARE(limiter.suppressedTotal(), quint64(lines - 20));
    }

    // The summary names the message, its category and its type, so the log
    // still says what went wrong and how often.
    void theSummaryNamesTheMessageAndItsCount()
    {
        RepeatLogLimiter limiter(limits());
        for (int i = 0; i < 25; ++i)
            limiter.admit(QtWarningMsg, kCategory, kRunaway, 100); // 5 held
        RepeatLogLimiter::Decision d;
        for (qint64 t = 200; t <= 10100; t += 100) { // 100 more, never quiet
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

    // Below the burst nothing is touched, however long it goes on: a message
    // logged 20 times a second for a minute is all logged.
    void aSteadyMessageBelowTheBurstIsNeverSuppressed()
    {
        RepeatLogLimiter limiter(limits());
        const Fed fed = feed(limiter, QStringLiteral("rtp stats"), 20 * 60, 0, 50.0);
        QCOMPARE(fed.passed, 20 * 60);
        QCOMPARE(fed.summaries, 0);
    }

    // A flood of one message never hides another one, before, during or
    // after it.
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
        // Same text, another category or type, is another message.
        QVERIFY(limiter.admit(QtWarningMsg, "other.category", kRunaway, 600).pass);
        QVERIFY(limiter.admit(QtCriticalMsg, kCategory, kRunaway, 600).pass);
    }

    // A flood that ends and later recurs: the first one is summarised, and the
    // recurrence gets its burst back once the message was quiet for a window.
    void aQuietWindowEndsTheFlood()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 100, 0, 1.0); // 20 pass, 80 suppressed
        auto d = limiter.admit(QtWarningMsg, kCategory, kRunaway, 1200);
        QVERIFY(d.pass);
        QCOMPARE(d.summaries.size(), 1);
        QCOMPARE(countIn(d.summaries.first().text), quint64(80));
        const Fed again = feed(limiter, kRunaway, 30, 1201, 1.0);
        QCOMPARE(again.passed, 19); // the line above opened this window
    }

    // Distinct messages beyond the slot count evict the oldest, and an evicted
    // flood is still summarised rather than lost.
    void anEvictedFloodIsSummarisedNotLost()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 50, 0, 0.01); // 30 suppressed at t=0
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

    // Long messages are quoted shortened in the summary, never cutting a
    // surrogate pair in half.
    void theQuoteIsBounded()
    {
        const QString longMessage(5000, QLatin1Char('x'));
        const QString text = RepeatLogLimiter::summaryText(3, 1500, longMessage);
        QVERIFY(text.size() < RepeatLogLimiter::kQuoteChars + 80);
        QVERIFY(text.endsWith(QStringLiteral("...")));

        // U+1F600 straddling the cut: 199 'x', then the pair at 199-200.
        const QString emoji = QString(RepeatLogLimiter::kQuoteChars - 1, QLatin1Char('x'))
            + QString::fromUcs4(U"\U0001F600") + QStringLiteral("tail");
        const QString cut = RepeatLogLimiter::summaryText(3, 1500, emoji);
        QVERIFY(cut.endsWith(QString(RepeatLogLimiter::kQuoteChars - 1, QLatin1Char('x'))
                             + QStringLiteral("...")));
        for (const QChar c : cut)
            QVERIFY(!c.isSurrogate());
    }

    // At exit, what is still suppressed is summarised, once.
    void takePendingAccountsForWhatIsLeft()
    {
        RepeatLogLimiter limiter(limits());
        feed(limiter, kRunaway, 70, 0, 0.1); // 50 suppressed
        auto pending = limiter.takePending(10);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(countIn(pending.first().text), quint64(50));
        QVERIFY(limiter.takePending(20).isEmpty());
    }

    // Qt calls handlers on any thread: concurrent floods are counted exactly.
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

    // The application installs it around the --log-file handler (so it
    // bounds the file and stderr alike) and inside the VAAPI gate (which
    // counts every export warning itself), and flushes it at exit. Source
    // scan: main() is not run by a test.
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
