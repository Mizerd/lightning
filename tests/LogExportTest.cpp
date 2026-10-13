// Log export: the sanitizer, the rolling log's bounds, the zip, and what an
// exported file actually contains.
//
// Every sanitizer case plants the SECRET in the input and asserts the output
// does not contain it, and ALSO asserts a neighbouring harmless fact survived:
// a sanitizer that returns an empty string passes the first half alone.
#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QMutex>
#include <QProcess>
#include <QTimeZone>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <tuple>

#include "app/AsyncLogSink.h"
#include "app/LogExport.h"
#include "app/LogExportController.h"
#include "crypto/E2eeDiagnostics.h"

namespace lx = lightning::logexport;
using lightning::logging::AsyncLogSink;

namespace {

lx::SanitizeOptions opts(const QByteArray &salt = "fixed-salt",
                         const QStringList &literals = {})
{
    lx::SanitizeOptions o;
    o.salt = salt;
    o.literals = literals;
    return o;
}

QString sanitize(const QString &line, const lx::SanitizeOptions &o = opts())
{
    return lx::sanitizeLine(line, o).value_or(QStringLiteral("<DROPPED>"));
}

// Counts device writes: the cost the rolling log's coalescing is for.
class CountingDevice : public QIODevice
{
public:
    std::atomic<int> writes{0};
    QByteArray data;
    QMutex mutex;
    bool isSequential() const override { return true; }
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *d, qint64 len) override
    {
        ++writes;
        QMutexLocker lock(&mutex);
        data.append(d, len);
        return len;
    }
    QByteArray snapshot()
    {
        QMutexLocker lock(&mutex);
        return data;
    }
};

QByteArray sampleRawLog()
{
    return QByteArray(
        "2026-10-13T10:00:00.000Z info matrix.app: login succeeded slug=alice_example.org\n"
        "2026-10-13T10:00:01.000Z info matrix.rust: open room=!SecretRoomId:example.org "
        "by @alice:example.org store=/home/alice/.local/share/MatrixClient/x\n"
        "2026-10-13T10:00:02.000Z debug lightning.e2ee: session=AAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "2026-10-13T10:00:03.000Z warning matrix.http: GET https://hs.example.org/_matrix/x?access_token=syt_abc_def failed\n"
        "2026-10-13T10:00:04.000Z info matrix.app: typed body=my private message\n"
        "2026-10-13T10:00:05.000Z info matrix.app: recovery_key=EsTc 1234 abcd 5678 efgh 9012 ijkl 3456 mnop 7890 qrst 1234\n"
        "2026-10-13T10:00:06.000Z info matrix.calls: call state active\n");
}


} // namespace

class LogExportTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_settingsDir;

private Q_SLOTS:
    void initTestCase()
    {
        // Never the developer's real settings.
        QCoreApplication::setOrganizationName(QStringLiteral("LogExportTest"));
        QCoreApplication::setApplicationName(QStringLiteral("log-export-test"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDir.path());
        QSettings().clear();
    }
    void init()
    {
        lx::stopRollingLog();
        qunsetenv("LIGHTNING_NO_ROLLING_LOG");
        QSettings().remove(QStringLiteral("diagnostics"));
    }
    void cleanup() { lx::stopRollingLog(); }

    // ---- opt-in ------------------------------------------------------------

    void theLogIsOffByDefaultAndCreatesNothing()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.path() + QStringLiteral("/logs");
        lx::setRollingLogDirectory(dir);
        LogExportController controller;
        QVERIFY(!lx::keepLogEnabled());
        QVERIFY(!controller.keepLog());
        QVERIFY(!lx::rollingLogRunning());
        QVERIFY(lx::rollingLogSink() == nullptr);
        // What main() does at start with the setting at its default:
        if (lx::keepLogEnabled())
            lx::startRollingLog(dir);
        QVERIFY(!QFileInfo::exists(dir));
        QVERIFY(lx::RollingLogDevice::logFiles(dir).isEmpty());
    }

    void turningItOnWritesAndTurningItOffStops()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.path() + QStringLiteral("/logs");
        lx::setRollingLogDirectory(dir);
        LogExportController controller;
        controller.setDirectory(dir);

        controller.setKeepLog(true);
        QVERIFY(controller.keepLog());
        QVERIFY(lx::keepLogEnabled());      // persisted for the next start
        QVERIFY(lx::rollingLogRunning());
        auto *sink = lx::rollingLogSink();
        QVERIFY(sink);
        sink->log(QtInfoMsg, "t", QStringLiteral("while on"));
        QVERIFY(sink->flush(2000));
        QVERIFY(!lx::RollingLogDevice::logFiles(dir).isEmpty());
        QVERIFY(lx::readRecentLog(dir, 1 << 20).contains("while on"));

        controller.setKeepLog(false);
        QVERIFY(!controller.keepLog());
        QVERIFY(!lx::keepLogEnabled());
        QVERIFY(!lx::rollingLogRunning());
        QVERIFY(lx::rollingLogSink() == nullptr);
        // The detached sink refuses further lines, and the file keeps what
        // it had.
        QVERIFY(!sink->log(QtInfoMsg, "t", QStringLiteral("after off")));
        const QByteArray kept = lx::readRecentLog(dir, 1 << 20);
        QVERIFY(kept.contains("while on"));
        QVERIFY(!kept.contains("after off"));
    }

    void theEnvironmentIsAHardOff()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.path() + QStringLiteral("/logs");
        qputenv("LIGHTNING_NO_ROLLING_LOG", "1");
        LogExportController controller;
        controller.setDirectory(dir);
        controller.setKeepLog(true);
        QVERIFY(controller.keepLogForcedOff());
        QVERIFY(!controller.keepLog());
        QVERIFY(!lx::keepLogEnabled());
        QVERIFY(!lx::startRollingLog(dir));
        QVERIFY(!QFileInfo::exists(dir));
    }

    void deletingSavedLogsRemovesFilesAndKeepsTheLogRunning()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.path() + QStringLiteral("/logs");
        lx::setRollingLogDirectory(dir);
        QVERIFY(lx::startRollingLog(dir));
        lx::rollingLogSink()->log(QtInfoMsg, "t", QStringLiteral("old"));
        QVERIFY(lx::rollingLogSink()->flush(2000));
        QVERIFY(lx::deleteSavedLogs() >= 1);
        QVERIFY(lx::rollingLogRunning());
        QVERIFY(!lx::readRecentLog(dir, 1 << 20).contains("old"));
        lx::stopRollingLog();
        // Off: deleting what an earlier session left behind.
        {
            lx::RollingLogDevice dev(dir, lx::RollingLimits{});
            QVERIFY(dev.open(QIODevice::WriteOnly));
            dev.write("left over\n");
        }
        QVERIFY(lx::deleteSavedLogs() >= 1);
        QVERIFY(!lx::rollingLogRunning());
        QVERIFY(lx::RollingLogDevice::logFiles(dir).isEmpty());
    }

    // ---- sanitizer ---------------------------------------------------------

    void matrixIdsBecomeCodes()
    {
        const QString line = QStringLiteral(
            "2026-10-13T10:00:00.000Z info matrix.app: open "
            "room=!AbCdEfGhIj:example.org user=@alice:example.org "
            "alias=#lounge:example.org event=$abcdefghijklmnopqrstuvwxyz0123 "
            "legacy=$oldstyle:example.org twice=@alice:example.org");
        const QString out = sanitize(line);
        for (const char *secret : {"AbCdEfGhIj", "alice", "lounge",
                                   "abcdefghijklmnopqrstuvwxyz0123",
                                   "oldstyle", "example.org"}) {
            QVERIFY2(!out.contains(QLatin1String(secret)), secret);
        }
        QVERIFY(out.contains(QLatin1String("matrix.app: open room=!")));
        // The sigil survives, so a reader can still tell a room from a user.
        static const QRegularExpression user(QStringLiteral("user=(@[0-9a-f]{8})\\b"));
        static const QRegularExpression again(QStringLiteral("twice=(@[0-9a-f]{8})\\b"));
        const auto a = user.match(out);
        const auto b = again.match(out);
        QVERIFY2(a.hasMatch() && b.hasMatch(), qPrintable(out));
        QCOMPARE(a.captured(1), b.captured(1));       // correlates inside one export
        // ... and differs under another salt: not a global pseudonym.
        const QString other = sanitize(line, opts("another-salt"));
        QVERIFY(user.match(other).captured(1) != a.captured(1));
    }

    void emptySaltIsPlainRedactId()
    {
        const QString id = QStringLiteral("!room:example.org");
        QCOMPARE(lx::maskMatrixId(id, QByteArray()), matrix::e2ee::redactId(id));
        QVERIFY(lx::maskMatrixId(id, "s") != matrix::e2ee::redactId(id));
        QVERIFY(lx::maskMatrixId(id, "s").startsWith(QLatin1Char('!')));
    }

    void filesystemPathsAndLocalpartsAreRemoved()
    {
        const QString line = QStringLiteral(
            "2026-10-13T10:00:00.000Z warning matrix.rust: store "
            "/home/alice/.local/share/MatrixClient/matrix-client/alice_example.org/"
            "matrix-rust-sdk-store could not be opened; slug=alice_example.org; "
            "C:\\Users\\alice\\AppData\\Local\\MatrixClient\\x.db; "
            "file:///home/alice/Downloads/a.png");
        const QString out = sanitize(line, opts("s", {QStringLiteral("alice")}));
        QVERIFY2(!out.contains(QLatin1String("alice")), qPrintable(out));
        QVERIFY2(!out.contains(QLatin1String("/home")), qPrintable(out));
        QVERIFY2(!out.contains(QLatin1String("Users")), qPrintable(out));
        QVERIFY(out.contains(QLatin1String("could not be opened")));
        QVERIFY(out.contains(QLatin1String("<path>")));
    }

    void shortLiteralsAreIgnoredRatherThanShreddingText()
    {
        const QString out = sanitize(
            QStringLiteral("2026-10-13T10:00:00.000Z info matrix.app: a bc de"),
            opts("s", {QStringLiteral("a"), QStringLiteral("bc")}));
        QVERIFY(out.endsWith(QLatin1String("a bc de")));
    }

    void tokensCredentialsAndUrlsAreStripped()
    {
        const QString line = QStringLiteral(
            "2026-10-13T10:00:00.000Z debug matrix.http: GET "
            "https://matrix.example.org/_matrix/media/v3/download/x/y"
            "?access_token=syt_YWxpY2U_AbCdEf_0123456789 failed; "
            "https://bob:hunter2@proxy.example.net:8443/p; "
            "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig; "
            "password=hunter3; refresh_token: rt-secret-value; "
            "contact carol@mail.example.com; "
            "key QWxpY2VBbGljZUFsaWNlQWxpY2VBbGljZUFsaWNl");
        const QString out = sanitize(line);
        for (const char *secret :
             {"syt_", "YWxpY2U", "hunter2", "hunter3", "eyJhbGci", "rt-secret-value",
              "carol", "_matrix/media", "QWxpY2VBbGlj", "bob:"}) {
            QVERIFY2(!out.contains(QLatin1String(secret)),
                     qPrintable(QString::fromLatin1(secret) + " in " + out));
        }
        // The host is the useful part of "which server is unreachable".
        QVERIFY(out.contains(QLatin1String("matrix.example.org")));
        QVERIFY(out.contains(QLatin1String("proxy.example.net")));
        QVERIFY(out.contains(QLatin1String("failed")));
    }

    void sensitiveCategoriesAndBodiesAreDropped()
    {
        const QString ts = QStringLiteral("2026-10-13T10:00:00.000Z ");
        QCOMPARE(sanitize(ts + QStringLiteral("debug lightning.e2ee: session x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info lightning.e2ee.recovery: x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info matrix.secret.libsecret: x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info lightning.account.password: x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info lightning.crypto.bootstrap: x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("debug matrix.rust: matrix_sdk_crypto::machine x")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info matrix.app: sent body=hello there")),
                 QStringLiteral("<DROPPED>"));
        QCOMPARE(sanitize(ts + QStringLiteral("info matrix.app: {\"formatted_body\": \"<b>hi\"}")),
                 QStringLiteral("<DROPPED>"));
        // Neighbours that merely share a prefix or a word are kept.
        QVERIFY(sanitize(ts + QStringLiteral("info lightning.e2eeish: ok")) != QStringLiteral("<DROPPED>"));
        QVERIFY(sanitize(ts + QStringLiteral("info matrix.app: bodyLen=12 ok")) != QStringLiteral("<DROPPED>"));
        QVERIFY(sanitize(ts + QStringLiteral("info matrix.app: screen change 1 -> 2")).contains(QLatin1String("screen change")));
    }

    void controlCharactersAndHugeLinesAreTamed()
    {
        const QString ts = QStringLiteral("2026-10-13T10:00:00.000Z info matrix.app: ");
        const QString out = sanitize(ts + QStringLiteral("a\x1b[31mred\rb"));
        QVERIFY(!out.contains(QChar(0x1b)));
        QVERIFY(!out.contains(QLatin1Char('\r')));
        lx::SanitizeStats st;
        const auto big = lx::sanitizeLine(ts + QString(20000, QLatin1Char('x')) + QLatin1Char(' ') + QString(20000, QLatin1Char('y')),
                                          opts(), &st);
        QVERIFY(big.has_value());
        QVERIFY(big->size() < 4100);
        QCOMPARE(st.truncatedLines, 1);
    }

    void sanitizeLogCountsWhatItRemoved()
    {
        const QString raw = QStringLiteral(
            "2026-10-13T10:00:00.000Z info matrix.app: one @a1:x.org\n"
            "2026-10-13T10:00:01.000Z info lightning.e2ee: secret stuff\n"
            "\n"
            "2026-10-13T10:00:02.000Z info matrix.app: body=hi there\n"
            "2026-10-13T10:00:03.000Z info matrix.app: four\n");
        lx::SanitizeStats st;
        const QString out = lx::sanitizeLog(raw, opts(), &st);
        QCOMPARE(st.linesIn, 4);
        QCOMPARE(st.linesKept, 2);
        QCOMPARE(st.droppedSensitiveCategory, 1);
        QCOMPARE(st.droppedMessageBody, 1);
        QCOMPARE(st.idsReplaced, 1);
        QCOMPARE(out.count(QLatin1Char('\n')), 2);
        QVERIFY(!out.contains(QLatin1String("secret stuff")));
    }

    void lastLinesTakesTheTail()
    {
        const QString t = QStringLiteral("1\n2\n3\n4\n5\n");
        QCOMPARE(lx::lastLines(t, 2), QStringLiteral("4\n5"));
        QCOMPARE(lx::lastLines(t, 99), QStringLiteral("1\n2\n3\n4\n5"));
        QCOMPARE(lx::lastLines(QStringLiteral("a\nb"), 1), QStringLiteral("b"));
        QCOMPARE(lx::lastLines(t, 0), QString());
        QCOMPARE(lx::lastLines(QString(), 5), QString());
    }

    // ---- rolling log -------------------------------------------------------

    void rollingLogStaysInsideItsBounds()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path() + QStringLiteral("/logs");
        lx::RollingLimits lim;
        lim.maxFileBytes = 1000;
        lim.maxTotalBytes = 5000;
        lim.maxFiles = 100;
        {
            lx::RollingLogDevice dev(dir, lim);
            QVERIFY(dev.open(QIODevice::WriteOnly));
            for (int i = 0; i < 200; ++i) {
                const QByteArray line = QByteArray("line-") + QByteArray::number(i)
                    + QByteArray(190, '.') + "\n";
                QCOMPARE(dev.write(line), qint64(line.size()));
            }
        }
        const QStringList files = lx::RollingLogDevice::logFiles(dir);
        qint64 total = 0;
        for (const QString &f : files) {
            const qint64 sz = QFileInfo(f).size();
            // A file closes at the first batch that crosses the bound.
            QVERIFY2(sz <= lim.maxFileBytes + 200, qPrintable(f));
            total += sz;
        }
        // 200 lines * ~200 bytes = 40 kB written; the set holds a fraction.
        QVERIFY2(total <= lim.maxTotalBytes + lim.maxFileBytes, qPrintable(QString::number(total)));
        QVERIFY(files.size() >= 2);
        const QByteArray recent = lx::readRecentLog(dir, 1 << 20);
        QVERIFY(recent.contains("line-199"));   // the newest survived
        QVERIFY(!recent.contains("line-0."));   // the oldest was pruned
    }

    void rollingLogHonoursTheFileCountBound()
    {
        QTemporaryDir tmp;
        lx::RollingLimits lim;
        lim.maxFileBytes = 100;
        lim.maxTotalBytes = 1 << 30;
        lim.maxFiles = 3;
        lx::RollingLogDevice dev(tmp.path(), lim);
        QVERIFY(dev.open(QIODevice::WriteOnly));
        for (int i = 0; i < 30; ++i)
            dev.write(QByteArray(150, 'a') + "\n");
        QVERIFY(lx::RollingLogDevice::logFiles(tmp.path()).size() <= 3);
    }

    void aNewSessionNeverOverwritesAnOlderOne()
    {
        QTemporaryDir tmp;
        for (int session = 0; session < 3; ++session) {
            lx::RollingLogDevice dev(tmp.path(), lx::RollingLimits{});
            QVERIFY(dev.open(QIODevice::WriteOnly));
            dev.write(QByteArray("session-") + QByteArray::number(session) + "\n");
        }
        const QByteArray all = lx::readRecentLog(tmp.path(), 1 << 20);
        QVERIFY(all.contains("session-0"));
        QVERIFY(all.contains("session-1"));
        QVERIFY(all.contains("session-2"));
        QVERIFY(all.indexOf("session-0") < all.indexOf("session-2"));
    }

    void rollingFilesAreOwnerOnly()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir tmp;
        lx::RollingLogDevice dev(tmp.path() + QStringLiteral("/logs"), lx::RollingLimits{});
        QVERIFY(dev.open(QIODevice::WriteOnly));
        dev.write("x\n");
        const QFile::Permissions other = QFile::ReadGroup | QFile::WriteGroup
            | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
        for (const QString &f : lx::RollingLogDevice::logFiles(tmp.path() + QStringLiteral("/logs")))
            QVERIFY(!(QFile::permissions(f) & other));
        QVERIFY(!(QFile::permissions(tmp.path() + QStringLiteral("/logs")) & other));
#else
        QSKIP("POSIX permissions");
#endif
    }

    void readRecentLogCutsAtALineStart()
    {
        QTemporaryDir tmp;
        lx::RollingLogDevice dev(tmp.path(), lx::RollingLimits{});
        QVERIFY(dev.open(QIODevice::WriteOnly));
        dev.write("first line is long enough to be cut\nsecond\nthird\n");
        int files = 0;
        const QByteArray tail = lx::readRecentLog(tmp.path(), 16, &files);
        QCOMPARE(tail, QByteArray("second\nthird\n"));
        QCOMPARE(files, 1);
        QCOMPARE(lx::readRecentLog(tmp.path() + QStringLiteral("/missing"), 100), QByteArray());
    }

    // The disk-write cost: a trickle of lines is one write per interval.
    void coalescingTurnsATrickleIntoFewWrites()
    {
        const auto drive = [](int coalesceMs) {
            auto dev = std::make_unique<CountingDevice>();
            CountingDevice *raw = dev.get();
            dev->open(QIODevice::WriteOnly);
            AsyncLogSink::Limits lim;
            lim.coalesceMs = coalesceMs;
            AsyncLogSink sink(std::move(dev), lim);
            for (int i = 0; i < 60; ++i) {
                sink.log(QtInfoMsg, "t", QStringLiteral("line %1").arg(i));
                QThread::msleep(5);
            }
            QElapsedTimer t;
            t.start();
            const bool flushed = sink.flush(2000);
            const qint64 flushMs = flushed ? t.elapsed() : 99999;
            const int writes = raw->writes.load();
            const QByteArray bytes = raw->snapshot();
            sink.shutdown(1000);
            return std::tuple<int, qint64, int>{writes, flushMs, int(bytes.count('\n'))};
        };
        const auto [coalescedWrites, flushMs, lines] = drive(400);
        QCOMPARE(lines, 60);                  // nothing lost
        QVERIFY2(coalescedWrites <= 5, qPrintable(QString::number(coalescedWrites)));
        QVERIFY2(flushMs < 300, qPrintable(QString::number(flushMs)));
        const auto [plainWrites, ignored, plainLines] = drive(0);
        Q_UNUSED(ignored)
        QCOMPARE(plainLines, 60);
        // The control: without coalescing the same trickle costs many more.
        QVERIFY2(plainWrites > coalescedWrites * 3, qPrintable(QString::number(plainWrites)));
    }

    void shutdownWritesWhatCoalescingWasHolding()
    {
        auto dev = std::make_unique<CountingDevice>();
        CountingDevice *raw = dev.get();
        dev->open(QIODevice::WriteOnly);
        AsyncLogSink::Limits lim;
        lim.coalesceMs = 10000;
        AsyncLogSink sink(std::move(dev), lim);
        sink.log(QtInfoMsg, "t", QStringLiteral("held"));
        sink.shutdown(2000);
        QVERIFY(raw->snapshot().contains("held"));
    }

    // ---- zip ---------------------------------------------------------------

    void zipRoundTripsStoredAndDeflatedEntries()
    {
        QByteArray noise(5000, Qt::Uninitialized);
        QRandomGenerator rng(7);
        for (char &c : noise)
            c = char(rng.generate());
        QByteArray text;
        for (int i = 0; i < 20000; ++i)
            text += "2026-10-13T10:00:00.000Z info matrix.app: line " + QByteArray::number(i % 97) + "\n";
        const QList<lx::ZipEntry> in = {{QStringLiteral("text.txt"), text},
                                        {QStringLiteral("empty.txt"), QByteArray()},
                                        {QStringLiteral("noise.bin"), noise},
                                        {QStringLiteral("ünï.txt"), QByteArray("utf8 name")}};
        const QByteArray zip = lx::writeZip(in, QDateTime(QDate(2026, 10, 13), QTime(10, 0), QTimeZone::UTC));
        QVERIFY2(zip.size() < text.size() / 4, "text must be deflated");
        bool ok = false;
        const auto out = lx::readZip(zip, &ok);
        QVERIFY(ok);
        QCOMPARE(out.size(), in.size());
        for (int i = 0; i < in.size(); ++i) {
            QCOMPARE(out[i].name, in[i].name);
            QCOMPARE(out[i].data, in[i].data);
        }
        QByteArray bad = zip;
        bad[bad.size() / 3] = char(bad[bad.size() / 3] ^ 0x55);
        ok = true;
        lx::readZip(bad, &ok);
        QVERIFY2(!ok, "a corrupted archive must not read back");
    }

    void zipIsAcceptedByAnIndependentReader()
    {
        QProcess probe;
        probe.start(QStringLiteral("python3"), {QStringLiteral("-I"), QStringLiteral("-c"), QStringLiteral("import zipfile")});
        if (!probe.waitForFinished(5000) || probe.exitCode() != 0)
            QSKIP("python3 is not available");
        QTemporaryDir tmp;
        QByteArray text;
        for (int i = 0; i < 5000; ++i)
            text += "line " + QByteArray::number(i) + " of the log\n";
        QFile f(tmp.path() + QStringLiteral("/t.zip"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(lx::writeZip({{QStringLiteral("a.txt"), text}, {QStringLiteral("b.txt"), "hello"}}, QDateTime::currentDateTimeUtc()));
        f.close();
        QProcess p;
        p.start(QStringLiteral("python3"),
                {QStringLiteral("-I"), QStringLiteral("-c"),
                 QStringLiteral("import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); "
                                "assert z.testzip() is None; "
                                "assert z.read('b.txt')==b'hello'; "
                                "assert z.read('a.txt').count(b'\\n')==5000"),
                 f.fileName()});
        QVERIFY(p.waitForFinished(10000));
        QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
    }

    // ---- the export --------------------------------------------------------

    void theExportedFileHoldsNothingSecretAndSomethingUseful()
    {
        lx::ExportInputs in;
        in.rawLog = sampleRawLog();
        in.diagnostics = QStringLiteral(
            "version: 0.10.0\nactive: @alice:example.org\nstore: /home/alice/x\n");
        in.options = opts("s", {QStringLiteral("alice")});
        in.sessionFiles = 1;
        in.when = QDateTime::currentDateTimeUtc();
        const lx::ExportResult r = lx::buildExport(in);

        bool ok = false;
        const auto entries = lx::readZip(r.zip, &ok);
        QVERIFY(ok);
        QStringList names;
        QString all;
        for (const auto &e : entries) {
            names << e.name;
            all += QString::fromUtf8(e.data);
        }
        QCOMPARE(names, (QStringList{QLatin1String(lx::kReadmeName),
                                     QLatin1String(lx::kDiagnosticsName),
                                     QLatin1String(lx::kLogName)}));
        for (const char *secret :
             {"alice", "SecretRoomId", "/home", "AAAAAAAA", "syt_", "private message",
              "EsTc", "mnop", "example.org/_matrix"}) {
            QVERIFY2(!all.contains(QLatin1String(secret)),
                     qPrintable(QString::fromLatin1(secret) + " leaked:\n" + all));
        }
        QVERIFY(all.contains(QLatin1String("call state active")));
        QVERIFY(all.contains(QLatin1String("hs.example.org")));
        QVERIFY(all.contains(QLatin1String("version: 0.10.0")));
        QVERIFY(all.contains(QLatin1String("no messages, passwords or keys")));
        // Three lines removed by rule (e2ee, body, ... recovery_key is
        // redacted in place, not dropped).
        QCOMPARE(r.stats.droppedSensitiveCategory, 1);
        QCOMPARE(r.stats.droppedMessageBody, 1);
        QVERIFY(r.sanitizedLog.contains(QLatin1String("2 removed")));
        QVERIFY(r.stats.idsReplaced >= 2);
    }

    void aFileThroughTheRealSinkAndRollingDeviceExportsClean()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.path() + QStringLiteral("/logs");
        auto dev = std::make_unique<lx::RollingLogDevice>(dir, lx::RollingLimits{});
        QVERIFY(dev->open(QIODevice::WriteOnly));
        AsyncLogSink::Limits lim;
        lim.coalesceMs = 500;
        AsyncLogSink sink(std::move(dev), lim);
        sink.log(QtInfoMsg, "matrix.rust", QStringLiteral("room !Xyzzy:example.org for @alice:example.org"));
        sink.log(QtDebugMsg, "lightning.e2ee", QStringLiteral("megolm session material"));
        sink.log(QtInfoMsg, "matrix.calls", QStringLiteral("active"));
        QVERIFY(sink.flush(2000));

        LogExportController controller;
        controller.setDirectory(dir);
        controller.setDiagnosticsProvider([] { return QStringLiteral("version: x\nuser: @alice:example.org\n"); });
        controller.setLiteralsProvider([] { return QStringList{QStringLiteral("alice")}; });
        QString recent;
        const QByteArray zip = controller.buildZipNow(&recent);
        bool ok = false;
        const auto entries = lx::readZip(zip, &ok);
        QVERIFY(ok);
        QString all;
        for (const auto &e : entries)
            all += QString::fromUtf8(e.data);
        QVERIFY(!all.contains(QLatin1String("alice")));
        QVERIFY(!all.contains(QLatin1String("Xyzzy")));
        QVERIFY(!all.contains(QLatin1String("megolm")));
        QVERIFY(all.contains(QLatin1String("matrix.calls: active")));
        QVERIFY(recent.contains(QLatin1String("matrix.calls: active")));
        QVERIFY(!recent.contains(QLatin1String("alice")));
        sink.shutdown(1000);
    }
};

QTEST_GUILESS_MAIN(LogExportTest)
#include "LogExportTest.moc"
