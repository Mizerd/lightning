// Production-exclusion contract for the development-only screenshot/demo mode.
//
// Runs the REAL shipped matrix-client binary (path injected by CMake via
// SCREENSHOT_DEMO_BINARY) and asserts the compile-time boundary from the
// outside — the only way to prove what a user's binary actually does:
//
//   * --build-info always reports a machine-checkable `screenshot_demo_compiled`
//     line; its value matches how the binary was compiled.
//   * In a normal/release build (the demo option OFF — the default test config,
//     and every packaged build) the flag is `false` and --screenshot-demo is
//     REJECTED in preflight (exit 2) before any GUI/network/store is touched.
//
// Both invocations exit in preflight (before QGuiApplication), so this needs no
// display. A LIGHTNING_RUST_ONLY release additionally cannot even be compiled
// with the demo option (CMake fatal-errors), so `screenshot_demo_compiled:false`
// is guaranteed for every shipped artifact.
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
        // Preflight paths never construct QGuiApplication, but force offscreen
        // defensively so a missing display can never turn a clean exit into a
        // platform abort.
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
        // The line is present and machine-checkable in every build.
#ifdef LIGHTNING_TEST_DEMO_COMPILED
        QVERIFY2(r.out.contains(QStringLiteral("screenshot_demo_compiled: true")),
                 qUtf8Printable("expected true; got:\n" + r.out));
#else
        QVERIFY2(r.out.contains(QStringLiteral("screenshot_demo_compiled: false")),
                 qUtf8Printable("expected false; got:\n" + r.out));
#endif
    }

#ifndef LIGHTNING_TEST_DEMO_COMPILED
    // The core production-safety assertion: a build without the demo option
    // rejects --screenshot-demo in preflight (exit 2), before any UI, network,
    // or store access. This is the state of every release/default build.
    void screenshotDemoFlagRejectedWhenNotCompiled()
    {
        const Run r = run({ QStringLiteral("--screenshot-demo") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("development-only")),
                 qUtf8Printable("expected a development-only rejection; got:\n"
                                + r.err));
        // It must NOT have started the app / printed the demo-active banner.
        QVERIFY(!r.out.contains(QStringLiteral("screenshot demo mode active")));
    }

    // --help must not advertise the flag as available when it is not compiled;
    // it documents it as a development build option only.
    void helpMentionsDevelopmentOnly()
    {
        const Run r = run({ QStringLiteral("--help") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.contains(QStringLiteral("--screenshot-demo")));
        QVERIFY(r.out.contains(QStringLiteral("Development builds only")));
    }

    // A production/normal build rejects every development-only demo argument.
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
            // It must NOT have started the demo.
            QVERIFY(!r.out.contains(QStringLiteral("screenshot demo mode active")));
        }
    }
#endif

#ifdef LIGHTNING_TEST_DEMO_COMPILED
    // In a demo build, invalid demo values are rejected in preflight (exit 2)
    // before any GUI/network/store access.
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
