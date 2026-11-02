#include <QtTest/QtTest>

#include <QProcess>
#include <QString>

#ifndef SCREENSHOT_DEMO_BINARY
#define SCREENSHOT_DEMO_BINARY ""
#endif

class ScreenshotDemoExclusionTest : public QObject
{
    Q_OBJECT

    static QString binary() { return QStringLiteral(SCREENSHOT_DEMO_BINARY); }

    struct Run {
        int exitCode = -1;
        QString out;
        QString err;
    };

    static Run run(const QStringList &args)
    {
        Run r;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(binary(), args);
        if (!p.waitForStarted(5000) || !p.waitForFinished(15000))
            return r;
        r.exitCode = p.exitCode();
        r.out = QString::fromUtf8(p.readAllStandardOutput());
        r.err = QString::fromUtf8(p.readAllStandardError());
        return r;
    }

private Q_SLOTS:
    void binaryExists()
    {
        QVERIFY2(!binary().isEmpty(), "SCREENSHOT_DEMO_BINARY not defined");
        QVERIFY2(QFileInfo::exists(binary()),
                 qUtf8Printable("binary not found: " + binary()));
    }

    void buildInfoReportsScreenshotDemoFlag()
    {
        const Run r = run({ QStringLiteral("--build-info") });
        QCOMPARE(r.exitCode, 0);
#ifdef LIGHTNING_TEST_DEMO_COMPILED
        QVERIFY2(r.out.contains(QStringLiteral("screenshot_demo_compiled: true")),
                 qUtf8Printable("expected true; got:\n" + r.out));
#else
        QVERIFY2(r.out.contains(QStringLiteral("screenshot_demo_compiled: false")),
                 qUtf8Printable("expected false; got:\n" + r.out));
#endif
    }

#ifndef LIGHTNING_TEST_DEMO_COMPILED
    void screenshotDemoFlagRejectedWhenNotCompiled()
    {
        const Run r = run({ QStringLiteral("--screenshot-demo") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("development-only")),
                 qUtf8Printable("expected a development-only rejection; got:\n"
                                + r.err));
        QVERIFY(!r.out.contains(QStringLiteral("screenshot demo mode active")));
    }

    void helpMentionsDevelopmentOnly()
    {
        const Run r = run({ QStringLiteral("--help") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.contains(QStringLiteral("--screenshot-demo")));
        QVERIFY(r.out.contains(QStringLiteral("Development builds only")));
    }

    void demoArgumentsRejectedWhenNotCompiled()
    {
        const QStringList args = {
            QStringLiteral("--demo-scenario=main-chat"),
            QStringLiteral("--demo-account=work"),
            QStringLiteral("--demo-theme=ocean"),
            QStringLiteral("--demo-appearance=dark"),
            QStringLiteral("--demo-size=1440x900"),
            QStringLiteral("--demo-hide-controls"),
        };
        for (const QString &a : args) {
            const Run r = run({ a });
            QVERIFY2(r.exitCode != 0,
                     qUtf8Printable("expected rejection for " + a
                                    + "; exit=" + QString::number(r.exitCode)));
            QVERIFY(!r.out.contains(QStringLiteral("screenshot demo mode active")));
        }
    }
#endif

#ifdef LIGHTNING_TEST_DEMO_COMPILED
    void invalidDemoScenarioRejectedInPreflight()
    {
        const Run r = run({ QStringLiteral("--screenshot-demo"),
                            QStringLiteral("--demo-scenario=not-a-scenario") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("unknown --demo-scenario")),
                 qUtf8Printable("got:\n" + r.err));
        QVERIFY(!r.out.contains(QStringLiteral("screenshot demo mode active")));
    }

    void invalidDemoSizeRejectedInPreflight()
    {
        const Run r = run({ QStringLiteral("--screenshot-demo"),
                            QStringLiteral("--demo-size=99999x1") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("invalid --demo-size")),
                 qUtf8Printable("got:\n" + r.err));
    }
#endif
};

QTEST_GUILESS_MAIN(ScreenshotDemoExclusionTest)
#include "ScreenshotDemoExclusionTest.moc"
