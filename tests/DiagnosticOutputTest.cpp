// --log-file / --console contract, asserted against the REAL shipped binary.
//
// WHY THIS IS A PROCESS TEST AND NOT A SOURCE SCAN. src/main.cpp defines
// main() and cannot be linked into a test, which is why the two neighbouring
// preflight cases in DesktopIntegrationTest are scans. A scan could not have
// caught either defect below: both are about what the binary DOES with an
// argument vector, and both shipped through a tree whose scans were green.
// The binary path is injected by CMake (LIGHTNING_BINARY), exactly as
// ScreenshotDemoExclusionTest does it.
//
// THE TWO DEFECTS THIS PINS, measured 2026-09-15 on the packaged Windows
// build and reproduced on Linux with the same source:
//
//  1. preflightParse() was a single left-to-right walk in which every
//     terminating flag ended in `return r`. So --log-file standing AFTER one
//     of them was never read, r.logFilePath stayed empty, installLogFile("")
//     returned immediately and NO FILE WAS EVER CREATED:
//
//         Lightning.exe --call-media-status --log-file \\host.lan\Data\cms.log
//
//     exited 0 and produced nothing. The one command that answers "why can I
//     not call from this build" could not be captured from the one platform
//     that needed it. NOT Windows-specific: the same binary on Linux creates
//     no file either.
//
//  2. Even with the flags in the "right" order the file was useless. The
//     status commands PRINT to stdout with QTextStream; they do not log. The
//     --log-file message handler therefore never saw a byte of their output,
//     so `--log-file X --version` wrote a file containing its own header and
//     not the version string. On a GUI-subsystem Windows binary, where stdout
//     may reach nobody at all, that is the whole diagnostic lost.
//
// Every case below runs a preflight path, so none of them constructs
// QGuiApplication and none needs a display.
#include <QtTest/QtTest>

#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef LIGHTNING_BINARY
#define LIGHTNING_BINARY ""
#endif

class DiagnosticOutputTest : public QObject
{
    Q_OBJECT

    static QString binary() { return QStringLiteral(LIGHTNING_BINARY); }

    struct Run {
        int exitCode = -1;
        QString out;
        QString err;
        bool started = false;
    };

    static Run run(const QStringList &args)
    {
        Run r;
        QProcess p;
        // Preflight never constructs QGuiApplication, but force offscreen
        // defensively so a missing display cannot turn a clean exit into a
        // platform abort and make a real result look like a crash.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(binary(), args);
        if (!p.waitForStarted(5000) || !p.waitForFinished(60000))
            return r;
        r.started = true;
        r.exitCode = p.exitCode();
        r.out = QString::fromUtf8(p.readAllStandardOutput());
        r.err = QString::fromUtf8(p.readAllStandardError());
        return r;
    }

    static QString readAll(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return QString::fromUtf8(f.readAll());
    }

private Q_SLOTS:
    void binaryExists()
    {
        QVERIFY2(!binary().isEmpty(), "LIGHTNING_BINARY not defined");
        QVERIFY2(QFileInfo::exists(binary()),
                 qUtf8Printable("binary not found: " + binary()));
    }

    // DEFECT 1, headline case: the exact command line the tester typed.
    //
    // --log-file stands AFTER a flag that exits. On the unfixed binary this
    // produced no file at all, on every platform.
    void aLogFileFollowingATerminatingFlagIsStillOpened()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("after.log"));

        const Run r = run({ QStringLiteral("--version"),
                            QStringLiteral("--log-file"), path });
        QVERIFY2(r.started, "the binary did not run");
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(QFile::exists(path),
                 qUtf8Printable(
                     QStringLiteral(
                         "--version --log-file %1 created NO FILE. preflightParse "
                         "returned at --version before it ever read --log-file, "
                         "so installLogFile() got an empty path.").arg(path)));
    }

    // The same, for EVERY cheap terminating flag — so a new one added in front
    // of the reporting pass cannot silently reintroduce the defect for itself.
    // --help and --build-info exit in preflight; --call-media-status has its
    // own case below because it also has to reach a probe.
    void everyTerminatingFlagStillOpensTheLogFile()
    {
        const QStringList terminating = {
            QStringLiteral("--version"),
            QStringLiteral("-v"),
            QStringLiteral("--help"),
            QStringLiteral("-h"),
            QStringLiteral("--build-info"),
        };
        for (const QString &flag : terminating) {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString path = dir.filePath(QStringLiteral("t.log"));
            const Run r = run({ flag, QStringLiteral("--log-file"), path });
            QVERIFY2(r.started, qUtf8Printable("did not run: " + flag));
            QVERIFY2(QFile::exists(path),
                     qUtf8Printable(flag + QStringLiteral(
                         " exits before --log-file is read: no file created")));
        }
    }

    // The --log-file=PATH spelling has to survive a terminating flag too; it
    // is a separate branch and was equally unreachable.
    void theEqualsSpellingAlsoSurvivesATerminatingFlag()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("eq.log"));
        const Run r = run({ QStringLiteral("--version"),
                            QStringLiteral("--log-file=") + path });
        QVERIFY2(r.started, "the binary did not run");
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(QFile::exists(path), "--log-file=PATH after --version: no file");
    }

    // DEFECT 2: the file has to contain the DIAGNOSTIC, not just its own
    // header. --version prints through QTextStream(stdout), never through the
    // Qt message handler, so before the tee the log held the banner alone.
    void printedOutputReachesTheLogFileAndNotOnlyTheHeader()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("v.log"));

        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--version") });
        QVERIFY2(r.started, "the binary did not run");
        QCOMPARE(r.exitCode, 0);

        // What it printed to stdout, so the assertion below compares the log
        // against the real answer rather than a hardcoded version number.
        const QString printed = r.out.trimmed();
        QVERIFY2(printed.startsWith(QStringLiteral("Lightning ")),
                 qUtf8Printable("unexpected --version output: " + r.out));

        const QString log = readAll(path);
        QVERIFY2(!log.isEmpty(), "the log file is empty");
        QVERIFY2(log.contains(printed),
                 qUtf8Printable(QStringLiteral(
                     "--log-file captured none of the printed output. Expected "
                     "'%1' in the log; got:\n%2").arg(printed, log)));
    }

    // The user story from the defect report, end to end: the one command that
    // answers "why can I not call from this build", captured to a file, with
    // the flags in the order a person actually types them.
    void callMediaStatusIsCapturedByALogFileThatFollowsIt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("cms.log"));

        const Run r = run({ QStringLiteral("--call-media-status"),
                            QStringLiteral("--log-file"), path });
        QVERIFY2(r.started, "the binary did not run");
        QVERIFY2(QFile::exists(path),
                 "--call-media-status --log-file PATH created no file");

        // The needle is the first line the command prints in BOTH builds:
        // "...: yes" with the engine compiled in, "...: no" without.
        const QString log = readAll(path);
        QVERIFY2(log.contains(QStringLiteral("call media engine built in")),
                 qUtf8Printable(QStringLiteral(
                     "the log file exists but holds none of the status output; "
                     "got:\n%1").arg(log)));
    }

    // Regression guard for the shape that always worked: --log-file first,
    // app continues to run. Proves the added first pass did not consume the
    // flag in a way that stops the main loop from seeing the rest.
    void aLogFileBeforeATerminatingFlagStillWorks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("first.log"));
        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--build-info") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY(QFile::exists(path));
        QVERIFY2(r.out.contains(QStringLiteral("version:")),
                 qUtf8Printable("--build-info printed nothing useful:\n" + r.out));
    }

    // The first pass must step OVER --log-file's value, or a path that happens
    // to spell another flag is read as one.
    void theLogFilePathIsNotItselfParsedAsAFlag()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A file literally named "--console" inside the temp dir.
        const QString path = dir.filePath(QStringLiteral("--console"));
        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--version") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(QFile::exists(path),
                 "a --log-file path that looks like a flag was not honoured");
    }

    // --console must remain accepted after a terminating flag and must not be
    // rejected as an unknown option on any platform. It is a no-op off
    // Windows; the contract is that it parses.
    void consoleIsAcceptedAfterATerminatingFlag()
    {
        const Run r = run({ QStringLiteral("--version"),
                            QStringLiteral("--console") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(r.out.contains(QStringLiteral("Lightning ")),
                 qUtf8Printable("--version --console printed:\n" + r.out));
        QVERIFY2(!r.err.contains(QStringLiteral("Unknown option")),
                 qUtf8Printable("--console was rejected:\n" + r.err));
    }

    // Malformed values are still diagnosed exactly as before: the added first
    // pass RECORDS only, so the main loop stays the error authority and a
    // trailing --log-file with no path is still a preflight error.
    void aLogFileWithNoPathIsStillAnError()
    {
        const Run r = run({ QStringLiteral("--log-file") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("--log-file needs a path")),
                 qUtf8Printable("got:\n" + r.err));
    }

    void anEmptyEqualsLogFileIsStillAnError()
    {
        const Run r = run({ QStringLiteral("--log-file=") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains(QStringLiteral("--log-file= needs a path")),
                 qUtf8Printable("got:\n" + r.err));
    }

    // Preflight ERRORS must reach the log file too — on Windows stderr is as
    // unreachable as stdout, and "it printed an error you cannot read" is the
    // same as silence.
    void preflightErrorsAreCapturedByTheLogFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("err.log"));
        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--backend=not-a-backend") });
        QCOMPARE(r.exitCode, 2);
        QVERIFY(QFile::exists(path));
        const QString log = readAll(path);
        QVERIFY2(log.contains(QStringLiteral("not-a-backend")),
                 qUtf8Printable(QStringLiteral(
                     "the preflight error never reached the log:\n%1").arg(log)));
    }
};

QTEST_GUILESS_MAIN(DiagnosticOutputTest)
#include "DiagnosticOutputTest.moc"
