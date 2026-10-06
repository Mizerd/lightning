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
    bool fileManagerAnswers = true;
    bool portalAnswers = true;
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
    return hooks;
}

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
        explicit Fixture(const QString &folder)
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
                bridge.get(), chooser.get(), launcher.get(),
                DownloadsController::Sandbox{});
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
