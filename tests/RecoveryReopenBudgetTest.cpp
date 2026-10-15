// The loop breaker on the reopens that recover a damaged timeline mirror
// (docs/open-items.md, "a LOOP BREAKER on recovery reopens, never an identity
// guard"). The reopens are deliberate, so the bound must never turn one into a
// no-op: past the cap it defers, once, and the mirror still recovers.

#include "matrix/RecoveryReopenBudget.h"

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

using matrix::RecoveryReopenBudget;
using Verdict = RecoveryReopenBudget::Verdict;

class RecoveryReopenBudgetTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void reopensUpToTheCapThenDefersExactlyOnce()
    {
        RecoveryReopenBudget budget;
        const QString room = QStringLiteral("!r:example.org");
        qint64 retry = -1;
        for (int i = 0; i < RecoveryReopenBudget::kMaxReopens; ++i)
            QCOMPARE(budget.request(room, 1000 + i * 100, &retry), Verdict::Reopen);
        // The next one, 50 ms after the third: deferred until the first leaves
        // the window.
        QCOMPARE(budget.request(room, 1250, &retry), Verdict::Defer);
        QCOMPARE(retry, 1000 + RecoveryReopenBudget::kWindowMs - 1250);
        // A storm meanwhile schedules nothing more.
        for (int i = 0; i < 50; ++i)
            QCOMPARE(budget.request(room, 1300 + i, &retry), Verdict::AlreadyDeferred);
        // The deferred reopen runs, and it counts.
        const qint64 fired = 1000 + RecoveryReopenBudget::kWindowMs;
        QVERIFY(budget.takeDeferred(room, fired));
        QVERIFY(!budget.takeDeferred(room, fired));
        QCOMPARE(budget.reopensInWindow(room, fired), RecoveryReopenBudget::kMaxReopens);
    }

    void timelinesAreBoundedSeparately()
    {
        RecoveryReopenBudget budget;
        qint64 retry = 0;
        for (int i = 0; i < RecoveryReopenBudget::kMaxReopens; ++i)
            QCOMPARE(budget.request(QStringLiteral("a"), i, &retry), Verdict::Reopen);
        QCOMPARE(budget.request(QStringLiteral("a"), 10, &retry), Verdict::Defer);
        QCOMPARE(budget.request(QStringLiteral("b"), 10, &retry), Verdict::Reopen);
    }

    void aQuietWindowRestoresTheFullBudget()
    {
        RecoveryReopenBudget budget;
        qint64 retry = 0;
        for (int i = 0; i < RecoveryReopenBudget::kMaxReopens; ++i)
            QCOMPARE(budget.request(QStringLiteral("a"), i, &retry), Verdict::Reopen);
        const qint64 later = RecoveryReopenBudget::kWindowMs + 10;
        for (int i = 0; i < RecoveryReopenBudget::kMaxReopens; ++i)
            QCOMPARE(budget.request(QStringLiteral("a"), later + i, &retry), Verdict::Reopen);
    }

    void aSessionEndDropsWhatWasOwed()
    {
        RecoveryReopenBudget budget;
        qint64 retry = 0;
        for (int i = 0; i <= RecoveryReopenBudget::kMaxReopens; ++i)
            budget.request(QStringLiteral("a"), i, &retry);
        budget.clear();
        QVERIFY(!budget.takeDeferred(QStringLiteral("a"), RecoveryReopenBudget::kWindowMs));
    }

    // Every automatic recovery reopen goes through the budget. Before it, all
    // four called openRoomTimeline / openThread / openEventContext directly.
    void everyRecoveryReopenIsBudgeted()
    {
        QFile f(QStringLiteral(SRC_DIR "/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        int cases = 0;
        const QRegularExpression invalid(QStringLiteral("case DiffOutcome::Invalid:"));
        auto it = invalid.globalMatch(src);
        while (it.hasNext()) {
            const auto m = it.next();
            const int end = src.indexOf(QStringLiteral("break;"), m.capturedEnd());
            QVERIFY(end > 0);
            const QString body = src.mid(m.capturedEnd(), end - m.capturedEnd());
            QVERIFY2(body.contains(QStringLiteral("admitRecoveryReopen(")),
                     qPrintable(body));
            ++cases;
        }
        QCOMPARE(cases, 3); // room, thread, context
        const int overflow = src.indexOf(QStringLiteral("type == QLatin1String(\"queue_overflow\")"));
        QVERIFY(overflow > 0);
        QVERIFY(src.mid(overflow, 2000).contains(QRegularExpression(
            QStringLiteral("admitRecoveryReopen\\(\\s*openRoom, \"queue_overflow\""))));
    }
};

QTEST_GUILESS_MAIN(RecoveryReopenBudgetTest)
#include "RecoveryReopenBudgetTest.moc"
