// Element's download workflow (DownloadsController) and the Open / Show in
// folder actions behind it (FileLauncher).
//
// Reported 2026-10-06: "add an option to open files when downloaded, same as
// in Element — just copy their workflow", and "downloading files doesn't
// prefill their names or file formats". Element: Download saves without a
// dialog, never over an existing file ("name (1).ext"), then a "Download
// completed" toast offers Open. Here: the same, plus Show in folder, plus
// "Always ask where to save files", and never Open for a file that can run
// code.
//
// Nothing here opens a real file manager or a real dialog: every platform
// call goes through injected hooks, and the save dialog through an injected
// runner.

#include "app/DownloadsController.h"
#include "app/FileChooser.h"
#include "app/FileLauncher.h"
#include "app/SandboxEnvironment.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#if defined(Q_OS_LINUX)
#include <sys/xattr.h>
#endif

namespace {

// The MediaBridge test's fake: records fetches, answers on demand.
class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch { quint64 opId; QString key; };
    QList<Fetch> fetches;
    QList<quint64> cancels;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int, int) override
    {
        const quint64 op = nextOp++;
        fetches.append({op, mediaKey});
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &, int, int) override
    {
        return nextOp++;
    }
    void cancelMediaFetch(quint64 opId) override { cancels.append(opId); }
    void succeed(quint64 opId, const QByteArray &bytes)
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          QStringLiteral("application/octet-stream"),
                          QString());
    }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

// What the launcher was asked to do.
struct LaunchLog
{
    QList<QUrl> opened;
    QList<QPair<QString, QStringList>> started;
    QList<QUrl> shown;
    QStringList portalDirs;
    QStringList portalFiles;
    QStringList hostPathAsked;
    /// What the Documents portal says a document's real path is.
    QString hostPath;
    bool fileManagerAnswers = true;
    bool portalAnswers = true;
    bool portalFileAnswers = true;
    bool startSucceeds = true;
};

FileLauncher::Hooks hooksFor(LaunchLog &log)
{
    FileLauncher::Hooks hooks;
    hooks.openUrl = [&log](const QUrl &url) {
        log.opened.append(url);
        return true;
    };
    hooks.startDetached = [&log](const QString &program,
                                 const QStringList &args) {
        log.started.append({program, args});
        return log.startSucceeds;
    };
    hooks.showItems = [&log](const QUrl &uri, std::function<void(bool)> done) {
        log.shown.append(uri);
        done(log.fileManagerAnswers);
    };
    hooks.portalOpenDirectory = [&log](const QString &path,
                                       std::function<void(bool)> done) {
        log.portalDirs.append(path);
        done(log.portalAnswers);
    };
    // (2026-10-07 hooks: drop these two to build the document-portal cases
    // against the tree before that fix.)
    hooks.portalOpenFile = [&log](const QString &path,
                                  std::function<void(bool)> done) {
        log.portalFiles.append(path);
        done(log.portalFileAnswers);
    };
    hooks.documentHostPath = [&log](const QString &, const QString &docId,
                                    std::function<void(const QString &)> done) {
        log.hostPathAsked.append(docId);
        done(log.hostPath);
    };
    return hooks;
}

// $XDG_RUNTIME_DIR pointed at a scratch folder holding a fake document-portal
// mount, restored afterwards.
class FakeDocumentPortal
{
public:
    FakeDocumentPortal()
        : m_saved(qgetenv("XDG_RUNTIME_DIR")),
          m_had(qEnvironmentVariableIsSet("XDG_RUNTIME_DIR"))
    {
        // Canonical, because MediaBridge canonicalizes the folder it writes
        // into and the path it reports must still be inside this mount.
        m_root = QFileInfo(m_dir.path()).canonicalFilePath();
        qputenv("XDG_RUNTIME_DIR", m_root.toUtf8());
        QDir().mkpath(documentFolder());
    }
    ~FakeDocumentPortal()
    {
        if (m_had)
            qputenv("XDG_RUNTIME_DIR", m_saved);
        else
            qunsetenv("XDG_RUNTIME_DIR");
    }
    bool isValid() const { return m_dir.isValid() && !m_root.isEmpty(); }
    QString documentFolder() const
    {
        return m_root + QStringLiteral("/doc/fd37b80a");
    }
    QString file(const QString &leaf) const
    {
        return documentFolder() + QLatin1Char('/') + leaf;
    }

private:
    QTemporaryDir m_dir;
    QString m_root;
    QByteArray m_saved;
    bool m_had;
};

QString writeFile(const QString &dir, const QString &name,
                  const QByteArray &bytes = QByteArray("old"))
{
    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(bytes);
    return path;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class DownloadsControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_config.isValid());
        qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
        // No bus: a real portal hook that slips past the fakes must not
        // reach (or D-Bus activate) the desktop's portals.
        qputenv("DBUS_SESSION_BUS_ADDRESS",
                QByteArrayLiteral("unix:path=/nonexistent/lightning-test-bus"));
    }

    // ── FileLauncher: the right platform call ──

    void showInFolderAsksTheFileManagerFirstOnLinux()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir.path(), QStringLiteral("a.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QSignalSpy how(&launcher, &FileLauncher::revealFinished);
        QVERIFY(launcher.showInFolder(path));
        QCOMPARE(log.shown, QList<QUrl>{QUrl::fromLocalFile(path)});
        QVERIFY(log.portalDirs.isEmpty());
        QVERIFY(log.opened.isEmpty());
        QCOMPARE(how.count(), 1);
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("file-manager"));
    }

    void withoutAFileManagerTheLinuxRevealFallsBackInOrder()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir.path(), QStringLiteral("a.pdf"));
        LaunchLog log;
        log.fileManagerAnswers = false;
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QSignalSpy how(&launcher, &FileLauncher::revealFinished);
        QVERIFY(launcher.showInFolder(path));
        QCOMPARE(log.portalDirs, QStringList{QFileInfo(path).absoluteFilePath()});
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("portal"));

        log.portalAnswers = false;
        log.portalDirs.clear();
        how.clear();
        QVERIFY(launcher.showInFolder(path));
        // Last resort: the folder itself, never the file.
        QCOMPARE(log.opened,
                 QList<QUrl>{QUrl::fromLocalFile(QFileInfo(path).absolutePath())});
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("folder"));
    }

    void windowsSelectsTheFileInExplorer()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir.path(), QStringLiteral("a.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::Windows, hooksFor(log));
        QVERIFY(launcher.showInFolder(path));
        QCOMPARE(log.started.size(), 1);
        QCOMPARE(log.started.at(0).first, QStringLiteral("explorer.exe"));
        QCOMPARE(log.started.at(0).second,
                 (QStringList{QStringLiteral("/select,"),
                              QDir::toNativeSeparators(
                                  QFileInfo(path).absoluteFilePath())}));
        QVERIFY(log.shown.isEmpty());
    }

    void macOSRevealsTheFileInFinder()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir.path(), QStringLiteral("a.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::MacOS, hooksFor(log));
        QVERIFY(launcher.showInFolder(path));
        QCOMPARE(log.started.size(), 1);
        QCOMPARE(log.started.at(0).first, QStringLiteral("/usr/bin/open"));
        QCOMPARE(log.started.at(0).second,
                 (QStringList{QStringLiteral("-R"),
                              QFileInfo(path).absoluteFilePath()}));
    }

    void openHandsTheFileToTheDesktopAndAMissingFileToNobody()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir.path(), QStringLiteral("a.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QVERIFY(launcher.openFile(path));
        QCOMPARE(log.opened, QList<QUrl>{QUrl::fromLocalFile(path)});
        log.opened.clear();
        QVERIFY(!launcher.openFile(dir.path() + QStringLiteral("/gone.pdf")));
        QVERIFY(!launcher.showInFolder(dir.path() + QStringLiteral("/gone.pdf")));
        QVERIFY(log.opened.isEmpty());
        QVERIFY(log.shown.isEmpty());
    }

    // ── Files the document portal granted (Snap, Flatpak) ──
    //
    // Reported 2026-10-07 from the Ubuntu 26 snap: "Saved to fd37b80a" (the
    // document portal's id for the file), and Show in folder opened nothing.

    void showInFolderHandsAPortalFileToThePortalNotByName()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        const QString path = writeFile(portal.documentFolder(),
                                       QStringLiteral("report.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QSignalSpy how(&launcher, &FileLauncher::revealFinished);
        QVERIFY(launcher.showInFolder(path));
        // OpenDirectory with the file; the host's file manager is never
        // handed the sandbox's path.
        QCOMPARE(log.portalDirs, QStringList{path});
        QVERIFY(log.shown.isEmpty());
        QVERIFY(log.opened.isEmpty());
        QCOMPARE(how.count(), 1);
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("portal"));
    }

    void withoutThePortalAPortalFileIsShownByItsRealPathOrNotAtAll()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        const QString path = writeFile(portal.documentFolder(),
                                       QStringLiteral("report.pdf"));
        LaunchLog log;
        log.portalAnswers = false;
        log.hostPath = QStringLiteral("/home/someone/Downloads/report.pdf");
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QSignalSpy how(&launcher, &FileLauncher::revealFinished);
        QVERIFY(launcher.showInFolder(path));
        QCOMPARE(log.hostPathAsked, QStringList{QStringLiteral("fd37b80a")});
        QCOMPARE(log.shown, QList<QUrl>{QUrl::fromLocalFile(log.hostPath)});
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("file-manager"));

        // Nothing names the real folder: an honest failure, never the
        // document's own folder (a directory named by its id).
        log.hostPath.clear();
        log.shown.clear();
        how.clear();
        QString reported;
        QVERIFY(launcher.showInFolder(path, [&reported](const QString &h) {
            reported = h;
        }));
        QVERIFY(log.shown.isEmpty());
        QVERIFY(log.opened.isEmpty());
        QCOMPARE(how.at(0).at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(reported, QStringLiteral("failed"));
    }

    void openHandsAPortalFileToTheOpenUriPortal()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        const QString path = writeFile(portal.documentFolder(),
                                       QStringLiteral("report.pdf"));
        LaunchLog log;
        FileLauncher launcher(FileLauncher::Platform::Linux, hooksFor(log));
        QVERIFY(launcher.openFile(path));
        QCOMPARE(log.portalFiles, QStringList{path});
        QVERIFY(log.opened.isEmpty());
        // A portal that fails: Qt's own route, once.
        log.portalFiles.clear();
        log.portalFileAnswers = false;
        QVERIFY(launcher.openFile(path));
        QCOMPARE(log.portalFiles, QStringList{path});
        QCOMPARE(log.opened, QList<QUrl>{QUrl::fromLocalFile(path)});
    }

    void aPortalSaveNamesTheFileNotTheDocumentId()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.settings->setAlwaysAskWhereToSave(true);
        // What the portal's Save dialog hands a Snap or a Flatpak.
        f.answer = QUrl::fromLocalFile(portal.file(QStringLiteral("report.pdf")));
        f.downloads->download(QStringLiteral("$report"),
                              QStringLiteral("report.pdf"),
                              QStringLiteral("application/pdf"));
        QTRY_COMPARE(f.client.fetches.size(), 1);
        f.client.succeed(f.client.fetches.at(0).opId, "pdf");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        QVariantMap item = f.downloads->items().value(0).toMap();
        QCOMPARE(readFile(portal.file(QStringLiteral("report.pdf"))),
                 QByteArray("pdf"));
        QCOMPARE(item.value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("report.pdf"));
        // Not "fd37b80a": the card then says only "Saved".
        QCOMPARE(item.value(QStringLiteral("folderName")).toString(), QString());
        QCOMPARE(f.log.hostPathAsked, QStringList{QStringLiteral("fd37b80a")});
        QCOMPARE(item.value(QStringLiteral("canOpen")).toBool(), true);

        // Show in folder, with no portal and no real path: the row says so.
        f.log.portalAnswers = false;
        const int id = item.value(QStringLiteral("id")).toInt();
        QVERIFY(f.downloads->showInFolder(id));
        item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("revealFailed")).toBool(), true);
        QVERIFY(f.log.opened.isEmpty());
        // A second try that works clears it.
        f.log.portalAnswers = true;
        QVERIFY(f.downloads->showInFolder(id));
        item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("revealFailed")).toBool(), false);

        // Open goes to the portal with the granted file.
        QVERIFY(f.downloads->open(id));
        QCOMPARE(f.log.portalFiles,
                 QStringList{portal.file(QStringLiteral("report.pdf"))});
    }

    void thePortalsRealFolderIsNamedWhenItSaysOne()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.log.hostPath = QStringLiteral("/home/someone/Downloads/report.pdf");
        f.answer = QUrl::fromLocalFile(portal.file(QStringLiteral("report.pdf")));
        f.downloads->saveAs(QStringLiteral("$report"),
                            QStringLiteral("report.pdf"),
                            QStringLiteral("application/pdf"));
        QTRY_COMPARE(f.client.fetches.size(), 1);
        f.client.succeed(f.client.fetches.at(0).opId, "pdf");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        QCOMPARE(f.downloads->items().value(0).toMap()
                     .value(QStringLiteral("folderName")).toString(),
                 QStringLiteral("Downloads"));
    }

    void aRiskyPortalFileIsNeverOpenedEither()
    {
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.answer = QUrl::fromLocalFile(portal.file(QStringLiteral("setup.exe")));
        f.downloads->saveAs(QStringLiteral("$setup"), QStringLiteral("setup.exe"),
                            QStringLiteral("application/x-msdownload"));
        QTRY_COMPARE(f.client.fetches.size(), 1);
        f.client.succeed(f.client.fetches.at(0).opId, "MZ");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        const QVariantMap item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("canOpen")).toBool(), false);
        const int id = item.value(QStringLiteral("id")).toInt();
        QVERIFY(!f.downloads->open(id));
        QVERIFY(f.log.portalFiles.isEmpty());
        QVERIFY(f.log.opened.isEmpty());
        // Shown, through the portal.
        QVERIFY(f.downloads->showInFolder(id));
        QCOMPARE(f.log.portalDirs,
                 QStringList{portal.file(QStringLiteral("setup.exe"))});
    }

    // The real reader: the attribute the document portal sets on every
    // document. Needs a filesystem with user xattrs (skips without one); no
    // bus is reachable here, so GetHostPaths answers nothing.
    void theDefaultReaderTakesTheHostPathFromThePortalsAttribute()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("the document portal is Linux only");
#else
        FakeDocumentPortal portal;
        QVERIFY(portal.isValid());
        const QString path = writeFile(portal.documentFolder(),
                                       QStringLiteral("report.pdf"));
        FileLauncher launcher(FileLauncher::Platform::Linux,
                              FileLauncher::Hooks{});
        QString host = QStringLiteral("unset");
        launcher.resolveHostPath(path, [&host](const QString &h) { host = h; });
        QTRY_COMPARE(host, QString()); // no attribute, no bus: nothing

        const QByteArray value("/home/someone/Downloads/report.pdf");
        if (::setxattr(QFile::encodeName(path).constData(),
                       "user.document-portal.host-path", value.constData(),
                       static_cast<size_t>(value.size()), 0)
            != 0)
            QSKIP("this filesystem refuses user extended attributes");
        host = QStringLiteral("unset");
        launcher.resolveHostPath(path, [&host](const QString &h) { host = h; });
        QCOMPARE(host, QString::fromUtf8(value));
#endif
    }

    // ── Sandboxes that cannot write to Downloads always ask ──

    void aSandboxWithoutDownloadsAccessAlwaysAsks_data()
    {
        QTest::addColumn<bool>("flatpak");
        QTest::addColumn<QString>("filesystems");
        QTest::addColumn<bool>("snap");
        QTest::addColumn<bool>("blocked");
        QTest::newRow("not sandboxed") << false << "" << false << false;
        QTest::newRow("flatpak, nothing") << true << "" << false << true;
        QTest::newRow("flatpak, pipewire only")
            << true << "xdg-run/pipewire-0;" << false << true;
        QTest::newRow("flatpak, xdg-download")
            << true << "xdg-run/pipewire-0;xdg-download;" << false << false;
        QTest::newRow("flatpak, xdg-download:ro")
            << true << "xdg-download:ro;" << false << true;
        QTest::newRow("flatpak, home") << true << "home;" << false << false;
        QTest::newRow("flatpak, host:rw") << true << "host:rw;" << false << false;
        QTest::newRow("flatpak, revoked")
            << true << "xdg-download;!xdg-download;" << false << true;
        QTest::newRow("snap") << false << "" << true << true;
    }
    void aSandboxWithoutDownloadsAccessAlwaysAsks()
    {
        QFETCH(bool, flatpak);
        QFETCH(QString, filesystems);
        QFETCH(bool, snap);
        QFETCH(bool, blocked);
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = flatpak;
        sandbox.flatpakFilesystems = filesystems;
        sandbox.snap = snap;
        QCOMPARE(DownloadsController::sandboxBlocksDownloads(sandbox), blocked);
    }

    // ── Sandbox detection: never stat-gated (live Flatpak FAIL 2026-10-07) ──

    void hostSandboxReadsAnUnlinkedFlatpakInfo()
    {
#if defined(Q_OS_LINUX)
        // In the Flathub build /.flatpak-info "did not exist" to Qt (an
        // unlinked bind mount, st_nlink == 0), so hostSandbox() said "not
        // sandboxed" and Download wrote into the sandbox's private home. An
        // open fd to an unlinked file is the same shape. Fails if the
        // exists() gate comes back.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = dir.filePath(QStringLiteral("flatpak-info"));
        writeFile(dir.path(), QStringLiteral("flatpak-info"),
                  "[Application]\nname=org.example.App\n\n[Context]\n"
                  "filesystems=xdg-run/pipewire-0;\n");
        QFile held(name);
        QVERIFY(held.open(QIODevice::ReadOnly));
        QVERIFY(QFile::remove(name));
        const DownloadsController::Sandbox sandbox =
            DownloadsController::hostSandbox(
                QStringLiteral("/proc/self/fd/%1").arg(held.handle()));
        QVERIFY(sandbox.flatpak);
        QCOMPARE(sandbox.appId, QStringLiteral("org.example.App"));
        QCOMPARE(sandbox.flatpakFilesystems,
                 QStringLiteral("xdg-run/pipewire-0;"));
        QVERIFY(DownloadsController::sandboxBlocksDownloads(sandbox));
#else
        QSKIP("Linux only");
#endif
    }

    void aLeakedFlatpakEnvironmentAloneDoesNotSandboxDownloads()
    {
        // The description's contents decide; $FLATPAK_ID leaked into a native
        // process must not refuse its every save.
        const QByteArray saved = qgetenv("FLATPAK_ID");
        const bool wasSet = qEnvironmentVariableIsSet("FLATPAK_ID");
        qputenv("FLATPAK_ID", QByteArrayLiteral("org.example.LightningTest"));
        QTemporaryDir dir;
        const DownloadsController::Sandbox sandbox =
            DownloadsController::hostSandbox(
                dir.filePath(QStringLiteral("no-such-flatpak-info")));
        if (wasSet)
            qputenv("FLATPAK_ID", saved);
        else
            qunsetenv("FLATPAK_ID");
        QVERIFY(!sandbox.flatpak);
        QVERIFY(!DownloadsController::sandboxBlocksDownloads(sandbox));
    }

    void sandboxDetectionReadsAnUnlinkedFlatpakInfo()
    {
#if defined(Q_OS_LINUX)
        // The real /.flatpak-info: st_nlink == 0, which Qt reports as absent
        // though it opens. An open fd to an unlinked file is the same shape.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = dir.filePath(QStringLiteral("flatpak-info"));
        writeFile(dir.path(), QStringLiteral("flatpak-info"),
                  "[Application]\nname=org.example.App\n\n[Context]\n"
                  "shared=network;ipc;\n"
                  "filesystems=xdg-run/pipewire-0;xdg-config/kdeglobals:ro;\n");
        QFile held(name);
        QVERIFY(held.open(QIODevice::ReadOnly));
        QVERIFY(QFile::remove(name));
        const QString unlinked =
            QStringLiteral("/proc/self/fd/%1").arg(held.handle());
        qInfo() << "QFileInfo::exists() on an unlinked file:"
                << QFileInfo::exists(unlinked);

        const sandboxenv::Detection d = sandboxenv::detect(
            sandboxenv::Environment{}, sandboxenv::readFileUngated(unlinked));
        QVERIFY(d.flatpak);
        QCOMPARE(d.appId, QStringLiteral("org.example.App"));
        QCOMPARE(d.flatpakFilesystems,
                 QStringLiteral("xdg-run/pipewire-0;xdg-config/kdeglobals:ro;"));
        QVERIFY(sandboxenv::pathPresent(unlinked));

        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = d.flatpak;
        sandbox.flatpakFilesystems = d.flatpakFilesystems;
        QVERIFY(DownloadsController::sandboxBlocksDownloads(sandbox));
#else
        QSKIP("Linux only");
#endif
    }

    void theContainerVariableAloneMeansFlatpak()
    {
        sandboxenv::Environment env;
        env.container = QStringLiteral("flatpak");
        QVERIFY(sandboxenv::detect(env, {}).flatpak);
        env.container = QStringLiteral("podman");
        QVERIFY(!sandboxenv::detect(env, {}).flatpak);
        sandboxenv::Environment snap;
        snap.snap = QStringLiteral("/snap/lightning/1");
        QVERIFY(!sandboxenv::detect(snap, {}).snap); // needs SNAP_NAME too
        snap.snapName = QStringLiteral("lightning");
        QVERIFY(sandboxenv::detect(snap, {}).snap);
    }

    // ── Only a path the host can see may be called saved ──

    void aPathReachesTheHostOnlyThroughAGrantThePortalOrTheAppsOwnFolder_data()
    {
        QTest::addColumn<QString>("filesystems");
        QTest::addColumn<QString>("relative"); // under the fake home
        QTest::addColumn<bool>("reaches");
        QTest::newRow("no grant, home") << "" << "notes.txt" << false;
        QTest::newRow("no grant, Downloads") << "" << "Downloads/a.pdf" << false;
        QTest::newRow("no grant, own .var/app")
            << "" << ".var/app/org.example.App/a.pdf" << true;
        QTest::newRow("no grant, another app's .var/app")
            << "" << ".var/app/org.other.App/a.pdf" << false;
        QTest::newRow("xdg-download, Downloads")
            << "xdg-download;" << "Downloads/a.pdf" << true;
        QTest::newRow("xdg-download, home") << "xdg-download;" << "a.pdf" << false;
        QTest::newRow("xdg-download:ro") << "xdg-download:ro;" << "Downloads/a.pdf"
                                         << false;
        QTest::newRow("xdg-download revoked")
            << "xdg-download;!xdg-download;" << "Downloads/a.pdf" << false;
        QTest::newRow("xdg-download/sub") << "xdg-download/Lightning;"
                                          << "Downloads/Lightning/a.pdf" << true;
        QTest::newRow("xdg-download/sub, outside")
            << "xdg-download/Lightning;" << "Downloads/a.pdf" << false;
        QTest::newRow("home") << "home;" << "notes.txt" << true;
        QTest::newRow("~/Stuff") << "~/Stuff;" << "Stuff/a.pdf" << true;
        QTest::newRow("~/Stuff, elsewhere") << "~/Stuff;" << "Other/a.pdf" << false;
        QTest::newRow("xdg-config does not count")
            << "xdg-config/kdeglobals;" << ".config/kdeglobals" << false;
    }
    void aPathReachesTheHostOnlyThroughAGrantThePortalOrTheAppsOwnFolder()
    {
        QFETCH(QString, filesystems);
        QFETCH(QString, relative);
        QFETCH(bool, reaches);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.flatpakFilesystems = filesystems;
        sandbox.appId = QStringLiteral("org.example.App");
        sandbox.home = home.path();
        sandbox.xdgDirs.insert(QStringLiteral("xdg-download"),
                               home.path() + QStringLiteral("/Downloads"));
        const QString path = home.path() + QLatin1Char('/') + relative;
        QCOMPARE(DownloadsController::pathReachesHost(sandbox, path), reaches);
        // Outside a sandbox everything reaches the host.
        QVERIFY(DownloadsController::pathReachesHost(
            DownloadsController::Sandbox{}, path));
    }

    void theHostGrantStopsAtFlatpaksReservedTrees_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("reaches");
        QTest::newRow("home") << "/home/tester/notes.txt" << true;
        QTest::newRow("Silverblue home") << "/var/home/tester/a.pdf" << true;
        QTest::newRow("removable media") << "/run/media/tester/usb/a.pdf" << true;
        QTest::newRow("mnt") << "/mnt/data/a.pdf" << true;
        QTest::newRow("tmp") << "/tmp/a.pdf" << false;
        QTest::newRow("etc") << "/etc/a.conf" << false;
        QTest::newRow("root") << "/root/a.pdf" << false;
        QTest::newRow("var") << "/var/lib/a.pdf" << false;
        QTest::newRow("run") << "/run/user/1000/a.pdf" << false;
        QTest::newRow("boot") << "/boot/a" << false;
        QTest::newRow("lib64") << "/lib64/a" << false;
        QTest::newRow("usr") << "/usr/share/a" << false;
        QTest::newRow("app") << "/app/bin/a" << false;
    }
    void theHostGrantStopsAtFlatpaksReservedTrees()
    {
        QFETCH(QString, path);
        QFETCH(bool, reaches);
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.flatpakFilesystems = QStringLiteral("host;");
        sandbox.home = QStringLiteral("/home/tester");
        QCOMPARE(DownloadsController::pathReachesHost(sandbox, path), reaches);
    }

    void aSymlinkInsideAGrantIntoThePrivateTreeDoesNotReachTheHost()
    {
        // The canonical form decides: Downloads is granted, but a link in it
        // pointing into the sandbox-private home sends the bytes to tmpfs.
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QVERIFY(QDir().mkpath(home.path() + QStringLiteral("/Downloads")));
        QVERIFY(QDir().mkpath(home.path() + QStringLiteral("/private")));
        QVERIFY(QFile::link(home.path() + QStringLiteral("/private"),
                            home.path() + QStringLiteral("/Downloads/escape")));
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.flatpakFilesystems = QStringLiteral("xdg-download;");
        sandbox.home = home.path();
        sandbox.xdgDirs.insert(QStringLiteral("xdg-download"),
                               home.path() + QStringLiteral("/Downloads"));
        QVERIFY(DownloadsController::pathReachesHost(
            sandbox, home.path() + QStringLiteral("/Downloads/a.pdf")));
        QVERIFY(!DownloadsController::pathReachesHost(
            sandbox, home.path() + QStringLiteral("/Downloads/escape/a.pdf")));
    }

    void aDocumentPortalPathReachesTheHost()
    {
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.home = QStringLiteral("/home/tester");
        QVERIFY(DownloadsController::pathReachesHost(
            sandbox, QStringLiteral("/run/flatpak/doc/514b5633/lt-test.png")));
        QVERIFY(!DownloadsController::pathReachesHost(
            sandbox, QStringLiteral("/tmp/lt-test.png")));
    }

    void aSaveIntoTheSandboxOnlyHomeIsRefusedNotReportedAsSaved()
    {
        // Debian KDE with no xdg-desktop-portal: Qt's own dialog runs inside
        // the sandbox and shows its private tmpfs home. Live 2026-10-07 the
        // card said "Saved to tester" for a file lost at exit.
        QTemporaryDir home;
        QVERIFY(home.isValid());
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.flatpakFilesystems = QStringLiteral("xdg-run/pipewire-0;");
        sandbox.appId = QStringLiteral("org.example.App");
        sandbox.home = home.path();
        Fixture f(home.path() + QStringLiteral("/Downloads"), sandbox);
        QVERIFY(f.downloads->asksWhereToSave());
        f.answer = QUrl::fromLocalFile(home.path() + QStringLiteral("/notes.txt"));

        f.downloads->saveAs(QStringLiteral("$doc"), QStringLiteral("notes.txt"),
                            QStringLiteral("text/plain"));
        QTRY_COMPARE(f.runnerCalls, 1);
        QTRY_COMPARE(f.downloads->items().size(), 1);
        const QVariantMap item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));
        QVERIFY(item.value(QStringLiteral("message")).toString().contains(
            QStringLiteral("sandbox")));
        QCOMPARE(item.value(QStringLiteral("canOpen")).toBool(), false);
        QTest::qWait(20);
        QVERIFY(f.client.fetches.isEmpty()); // nothing fetched, nothing written
        QVERIFY(!QFileInfo::exists(home.path() + QStringLiteral("/notes.txt")));
    }

    void aSaveIntoTheAppsOwnFolderIsAllowedInTheSandbox()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString own = home.path() + QStringLiteral("/.var/app/org.example.App");
        QVERIFY(QDir().mkpath(own));
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.appId = QStringLiteral("org.example.App");
        sandbox.home = home.path();
        Fixture f(home.path() + QStringLiteral("/Downloads"), sandbox);
        f.answer = QUrl::fromLocalFile(own + QStringLiteral("/notes.txt"));
        f.downloads->saveAs(QStringLiteral("$doc"), QStringLiteral("notes.txt"),
                            QStringLiteral("text/plain"));
        QTRY_COMPARE(f.client.fetches.size(), 1);
        f.client.succeed(f.client.fetches.at(0).opId, "hello");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        QCOMPARE(readFile(own + QStringLiteral("/notes.txt")), QByteArray("hello"));
    }

    void aDownloadsFolderOnlyTheSandboxSeesIsNeverWrittenDirectly()
    {
        // xdg-download is granted, but the folder the setting names is not
        // under it: Download must ask (the portal) rather than write there.
        QTemporaryDir home;
        QVERIFY(home.isValid());
        DownloadsController::Sandbox sandbox;
        sandbox.flatpak = true;
        sandbox.flatpakFilesystems = QStringLiteral("xdg-download;");
        sandbox.home = home.path();
        sandbox.xdgDirs.insert(QStringLiteral("xdg-download"),
                               home.path() + QStringLiteral("/Downloads"));
        Fixture f(home.path() + QStringLiteral("/Elsewhere"), sandbox);
        QVERIFY(!f.downloads->asksWhereToSave()); // the grant covers Downloads
        f.answer = QUrl(); // the user cancels the dialog
        f.downloads->download(QStringLiteral("$pic"), QStringLiteral("a.png"),
                              QStringLiteral("image/png"));
        QTRY_COMPARE(f.runnerCalls, 1);
        QTest::qWait(20);
        QVERIFY(f.client.fetches.isEmpty());
        QVERIFY(!QFileInfo::exists(home.path() + QStringLiteral("/Elsewhere")));
    }

    // ── Element's Download ──

    void downloadSavesStraightToTheFolderAndNeverReplacesAFile()
    {
        QTemporaryDir dir;
        writeFile(dir.path(), QStringLiteral("report.pdf"), "first");
        Fixture f(dir.path());

        f.downloads->download(QStringLiteral("$report"),
                              QStringLiteral("report.pdf"),
                              QStringLiteral("application/pdf"));
        QCOMPARE(f.runnerCalls, 0); // no dialog
        QCOMPARE(f.client.fetches.size(), 1);
        QVariantMap item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("state")).toString(),
                 QStringLiteral("saving"));

        f.client.succeed(f.client.fetches.at(0).opId, "second");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("report (1).pdf"));
        QCOMPARE(item.value(QStringLiteral("folderName")).toString(),
                 QFileInfo(dir.path()).fileName());
        QCOMPARE(item.value(QStringLiteral("canOpen")).toBool(), true);
        QCOMPARE(readFile(dir.path() + QStringLiteral("/report.pdf")),
                 QByteArray("first"));
        QCOMPARE(readFile(dir.path() + QStringLiteral("/report (1).pdf")),
                 QByteArray("second"));

        // Open: the desktop's handler, with the file that was written.
        const int id = item.value(QStringLiteral("id")).toInt();
        QVERIFY(f.downloads->open(id));
        QCOMPARE(f.log.opened.size(), 1);
        QCOMPARE(QFileInfo(f.log.opened.at(0).toLocalFile()).canonicalFilePath(),
                 QFileInfo(dir.path() + QStringLiteral("/report (1).pdf"))
                     .canonicalFilePath());
        QVERIFY(f.downloads->showInFolder(id));
        QCOMPARE(f.log.shown.size(), 1);
    }

    void aNameWithoutAnExtensionGetsOneFromTheType()
    {
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.downloads->download(QStringLiteral("$photo"), QStringLiteral("photo"),
                              QStringLiteral("image/jpeg"));
        f.client.succeed(f.client.fetches.at(0).opId, "jpeg");
        QTRY_VERIFY(QFileInfo::exists(dir.path() + QStringLiteral("/photo.jpg")));
    }

    void aFileThatCanRunCodeIsNeverOpened()
    {
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.downloads->download(QStringLiteral("$setup"), QStringLiteral("setup.exe"),
                              QStringLiteral("application/x-msdownload"));
        f.client.succeed(f.client.fetches.at(0).opId, "MZ");
        QTRY_COMPARE(f.downloads->items().value(0).toMap()
                         .value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        const QVariantMap item = f.downloads->items().value(0).toMap();
        QCOMPARE(item.value(QStringLiteral("risky")).toBool(), true);
        QCOMPARE(item.value(QStringLiteral("canOpen")).toBool(), false);
        const int id = item.value(QStringLiteral("id")).toInt();
        QVERIFY(!f.downloads->open(id));
        QVERIFY(f.log.opened.isEmpty());
        // Show in folder is still offered.
        QVERIFY(f.downloads->showInFolder(id));
        QCOMPARE(f.log.shown.size(), 1);
    }

    void cancellingADownloadRemovesItWithoutAFailure()
    {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QSignalSpy cancelled(f.bridge.get(), &MediaBridge::saveCancelled);
        f.downloads->download(QStringLiteral("$big"), QStringLiteral("big.zip"),
                              QStringLiteral("application/zip"));
        const int id = f.downloads->items().value(0).toMap()
                           .value(QStringLiteral("id")).toInt();
        f.downloads->cancel(id);
        QCOMPARE(cancelled.count(), 1);
        QVERIFY(f.downloads->items().isEmpty());
        QCOMPARE(f.client.cancels.size(), 1);
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    // ── Always ask / Save as: the dialog is prefilled ──

    void alwaysAskOpensASaveDialogPrefilledWithTheNameAndType()
    {
        QTemporaryDir dir;
        Fixture f(dir.path());
        f.settings->setAlwaysAskWhereToSave(true);
        QVERIFY(f.downloads->asksWhereToSave());
        // The user deletes the extension in the dialog.
        f.answer = QUrl::fromLocalFile(dir.path() + QStringLiteral("/holiday"));

        f.downloads->download(QStringLiteral("$pic"), QStringLiteral("IMG_0042"),
                              QStringLiteral("image/jpeg"));
        QTRY_COMPARE(f.runnerCalls, 1);
        QCOMPARE(f.lastRequest.mode, FileChooser::Mode::SaveFile);
        QCOMPARE(f.lastRequest.currentName, QStringLiteral("IMG_0042.jpg"));
        QCOMPARE(f.lastRequest.nameFilters.value(0),
                 QStringLiteral("JPG file (*.jpg)"));
        QCOMPARE(f.lastRequest.selectedNameFilter,
                 QStringLiteral("JPG file (*.jpg)"));
        QCOMPARE(f.lastRequest.folder, QUrl::fromLocalFile(dir.path()));

        QTRY_COMPARE(f.client.fetches.size(), 1);
        f.client.succeed(f.client.fetches.at(0).opId, "jpeg");
        // The extension the user removed came back.
        QTRY_VERIFY(QFileInfo::exists(dir.path() + QStringLiteral("/holiday.jpg")));
        QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/holiday")));
    }

    void saveAsAlwaysAsksEvenWhenDownloadWouldNot()
    {
        QTemporaryDir dir;
        Fixture f(dir.path());
        QVERIFY(!f.downloads->asksWhereToSave());
        f.answer = QUrl(); // the user cancels
        f.downloads->saveAs(QStringLiteral("$doc"), QStringLiteral("notes.txt"),
                            QStringLiteral("text/plain"));
        QTRY_COMPARE(f.runnerCalls, 1);
        QCOMPARE(f.lastRequest.currentName, QStringLiteral("notes.txt"));
        QTest::qWait(20);
        QVERIFY(f.client.fetches.isEmpty());
        QVERIFY(f.downloads->items().isEmpty());
    }

private:
    struct Fixture
    {
        explicit Fixture(const QString &folder,
                         const DownloadsController::Sandbox &sandbox = {})
        {
            bridge = std::make_unique<MediaBridge>();
            bridge->setClient(&client);
            settings = std::make_unique<SettingsManager>();
            settings->setAlwaysAskWhereToSave(false);
            settings->setDownloadFolder(QString());
            chooser = std::make_unique<FileChooser>(FileChooser::Environment{});
            chooser->setQtRunner([this](const FileChooser::Request &request,
                                        bool, FileChooser::Completion done) {
                ++runnerCalls;
                lastRequest = request;
                if (answer.isEmpty())
                    done(false, {}, {});
                else
                    done(true, {answer}, request.selectedNameFilter);
            });
            launcher = std::make_unique<FileLauncher>(
                FileLauncher::Platform::Linux, hooksFor(log));
            downloads = std::make_unique<DownloadsController>(
                bridge.get(), chooser.get(), launcher.get(), sandbox);
            downloads->setSettings(settings.get());
            downloads->setDefaultFolderProvider([folder] { return folder; });
        }
        ~Fixture()
        {
            downloads.reset();
            bridge.reset();
        }

        FakeClient client;
        LaunchLog log;
        std::unique_ptr<MediaBridge> bridge;
        std::unique_ptr<SettingsManager> settings;
        std::unique_ptr<FileChooser> chooser;
        std::unique_ptr<FileLauncher> launcher;
        std::unique_ptr<DownloadsController> downloads;
        int runnerCalls = 0;
        FileChooser::Request lastRequest;
        QUrl answer;
    };

    QTemporaryDir m_config;
};

QTEST_GUILESS_MAIN(DownloadsControllerTest)
#include "DownloadsControllerTest.moc"
