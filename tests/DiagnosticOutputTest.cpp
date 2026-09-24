// --log-file / --console contract, asserted against the real binary
// (LIGHTNING_BINARY, injected by CMake as in ScreenshotDemoExclusionTest).
// A process test, not a source scan, because main.cpp cannot be linked into
// a test and both behaviours are about what the binary does with argv:
//  1. --log-file is honoured wherever it stands, including after a flag that
//     exits in preflight (a single left-to-right walk used to return first).
//  2. Status commands print to stdout, not through the message handler, so
//     their output must be teed into the log file; on a GUI-subsystem Windows
//     binary stdout may reach nobody.
// Every case runs a preflight path, so no QGuiApplication or display is
// needed.
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
        // Force offscreen anyway, so a missing display cannot turn a clean exit
        // into a platform abort.
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

    // --log-file after a flag that exits still creates the file.
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

    // Every cheap terminating flag, so a new one cannot reintroduce the
    // problem for itself. --call-media-status has its own case because it
    // also reaches a probe.
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

    // The --log-file=PATH spelling survives a terminating flag too.
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

    // The file contains the diagnostic, not only its header: --version prints
    // through QTextStream(stdout), never the message handler.
    void printedOutputReachesTheLogFileAndNotOnlyTheHeader()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("v.log"));

        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--version") });
        QVERIFY2(r.started, "the binary did not run");
        QCOMPARE(r.exitCode, 0);

        // Compare against what was actually printed, not a hardcoded version.
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

    // --call-media-status is captured by a --log-file that follows it, in the
    // order people type them.
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

        // The first line the command prints in both builds ("...: yes" with the
        // engine, "...: no" without).
        const QString log = readAll(path);
        QVERIFY2(log.contains(QStringLiteral("call media engine built in")),
                 qUtf8Printable(QStringLiteral(
                     "the log file exists but holds none of the status output; "
                     "got:\n%1").arg(log)));
    }

    // --log-file first still works and does not stop the main loop from seeing
    // the rest.
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

    // The first pass steps over --log-file's value, so a path that spells a
    // flag is not read as one.
    void theLogFilePathIsNotItselfParsedAsAFlag()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A file literally named "--console" in the temp dir.
        const QString path = dir.filePath(QStringLiteral("--console"));
        const Run r = run({ QStringLiteral("--log-file"), path,
                            QStringLiteral("--version") });
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(QFile::exists(path),
                 "a --log-file path that looks like a flag was not honoured");
    }

    // --console is accepted after a terminating flag on every platform (a
    // no-op off Windows); it must parse.
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

    // The first pass only records, so the main loop still diagnoses malformed
    // values: a trailing --log-file with no path is an error.
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

    // Preflight errors reach the log file too: on Windows stderr is as
    // unreachable as stdout.
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
