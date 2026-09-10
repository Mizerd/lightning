// v0.7: desktop-integration contract. Verifies the tracked launcher entry,
// the generated hicolor icon set, the CMake install rules that ship them,
// and the Qt-side identity wiring — so the taskbar/launcher association
// cannot silently regress. Source-tree contract checks in the style of
// ThemeTokensTest; real desktop-environment behaviour still needs a live
// desktop test.

#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

class DesktopIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void desktopFileIsCoherent()
    {
        const QString desktop =
            readAll(QStringLiteral(SOURCE_DIR "/data/lightning.desktop"));
        QVERIFY2(!desktop.isEmpty(), "data/lightning.desktop missing");
        QVERIFY(desktop.contains(QStringLiteral("Type=Application")));
        QVERIFY(desktop.contains(QStringLiteral("Name=Lightning")));
        // Icon name must match the installed hicolor icon basename.
        QVERIFY(desktop.contains(QStringLiteral("Icon=lightning")));
        QVERIFY(desktop.contains(
            QStringLiteral("Exec=lightning-matrix --backend=rust")));
        // X11 window association: Qt's xcb plugin takes the WM_CLASS
        // instance from argv[0], i.e. the binary name ("lightning-matrix");
        // the class comes from the persistent application name.
        QVERIFY(desktop.contains(
            QStringLiteral("StartupWMClass=lightning-matrix")));
    }

    // The application binary and CMake target are `lightning-matrix`; the
    // generic `matrix-client` build identity is retired. The PERSISTENT
    // identity (QSettings organization/application names, store roots) is a
    // separate literal that must NOT follow — renaming it would sign every
    // existing install out. Both halves are pinned here so a future rename
    // in either direction is a deliberate act.
    void productionBinaryIsLightningMatrixAndTheStoredIdentityIsNot()
    {
        const QString cmake = readAll(QStringLiteral(SOURCE_DIR "/CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt missing");
        QVERIFY(cmake.contains(QStringLiteral("project(lightning\n")));
        QVERIFY(cmake.contains(
            QStringLiteral("qt_add_executable(lightning-matrix ${APP_MAIN_SOURCES})")));
        QVERIFY(cmake.contains(
            QStringLiteral("install(TARGETS lightning-matrix lightning-updater")));
        QVERIFY(!cmake.contains(QStringLiteral("project(matrix-client")));
        QVERIFY(!cmake.contains(QStringLiteral("qt_add_executable(matrix-client")));
        QVERIFY(!cmake.contains(QStringLiteral("add_executable(matrix-client")));

        const QString runDev = readAll(QStringLiteral(SOURCE_DIR "/scripts/run-dev.sh"));
        QVERIFY(runDev.contains(QStringLiteral("./build-rust/lightning-matrix")));
        QVERIFY(!runDev.contains(QStringLiteral("build-rust/matrix-client")));

        // Packaged Windows keeps Lightning.exe; the updater must look for
        // exactly that, whatever the source binary is called.
        const QString updater = readAll(QStringLiteral(SOURCE_DIR "/src/updater/main.cpp"));
        QVERIFY(updater.contains(QStringLiteral(
            "kPortableExecutableName = QStringLiteral(\"Lightning.exe\")")));

        // The stored identity: unchanged on purpose.
        const QString main = readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(main.contains(QStringLiteral(
            "QCoreApplication::setOrganizationName(\"MatrixClient\");")));
        QVERIFY(main.contains(QStringLiteral(
            "QCoreApplication::setApplicationName(\"matrix-client\");")));
        QVERIFY(!main.contains(QStringLiteral("setApplicationName(\"lightning-matrix\")")));
        const QString paths = readAll(QStringLiteral(SOURCE_DIR "/src/storage/AppDataPaths.cpp"));
        QVERIFY(paths.contains(QStringLiteral(
            "constexpr QLatin1String kApplicationName{\"matrix-client\"};")));
    }

    void hicolorIconsExistAndMatchDeclaredSizes()
    {
        const QList<int> sizes = {16, 32, 48, 64, 128, 192, 256, 512};
        for (int size : sizes) {
            const QString path = QStringLiteral(
                SOURCE_DIR "/data/icons/hicolor/%1x%1/apps/lightning.png")
                .arg(size);
            QVERIFY2(QFile::exists(path), qPrintable(path));
            const QImage image(path);
            QCOMPARE(image.width(), size);
            QCOMPARE(image.height(), size);
        }
        // The exact supplied artwork stays tracked as the generation source.
        QVERIFY(QFile::exists(
            QStringLiteral(SOURCE_DIR "/data/icons/lightning-source.png")));
        // The scalable vector of the same mark (launchers prefer it; the
        // README renders it). A real inline SVG, never an embedded raster.
        QFile svg(QStringLiteral(SOURCE_DIR "/data/icons/lightning.svg"));
        QVERIFY(svg.open(QIODevice::ReadOnly));
        const QByteArray svgBytes = svg.readAll();
        QVERIFY(svgBytes.contains("<svg"));
        QVERIFY(!svgBytes.contains("base64"));
    }

    void installRulesShipDesktopFileAndIcons()
    {
        const QString cmake =
            readAll(QStringLiteral(SOURCE_DIR "/CMakeLists.txt"));
        QVERIFY(!cmake.isEmpty());
        QVERIFY(cmake.contains(QStringLiteral("data/lightning.desktop")));
        QVERIFY(cmake.contains(
            QRegularExpression(QStringLiteral(
                "icons/hicolor/\\$\\{_icon_size\\}x\\$\\{_icon_size\\}/apps"))));
        // The window icon is bundled into the QML module resources so a
        // source run gets it without an installed icon theme.
        QVERIFY(cmake.contains(QStringLiteral(
            "data/icons/hicolor/256x256/apps/lightning.png")));
        // The scalable SVG installs beside the raster sizes.
        QVERIFY(cmake.contains(QStringLiteral(
            "icons/hicolor/scalable/apps")));
    }

    void qtIdentityIsWired()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());
        // Wayland app_id ↔ desktop entry. ONE literal, in ONE constant: the
        // app id, the basename of the entry the compositor looks it up in and
        // the Icon= key that entry carries all come from kAppId, so they
        // cannot drift apart into a generic window icon.
        QVERIFY(main.contains(QStringLiteral(
            "constexpr QLatin1String kAppId(\"lightning\")")));
        QVERIFY(main.contains(QStringLiteral(
            "setDesktopFileName(kAppId)")));
        // Themed icon with the bundled fallback.
        QVERIFY(main.contains(QStringLiteral("QIcon::fromTheme")));
        QVERIFY(main.contains(QStringLiteral(
            "icons/hicolor/256x256/apps/lightning.png")));
    }

    // THE WINDOW ICON ON WAYLAND, which setWindowIcon() above cannot supply.
    //
    // Qt's Wayland client implements no icon protocol (`xdg_toplevel_icon`
    // appears zero times in libQt6WaylandClient), so the compositor's only
    // route is the toplevel's app id resolved against installed launcher
    // entries. An AppImage installs nothing, so it has to publish one itself
    // — reported as a generic placeholder icon once 0.9.x stopped falling
    // back to XWayland, where _NET_WM_ICON had been doing the job.
    //
    // The properties pinned here are the ones whose absence is SILENT: a
    // publication that never runs, one that runs for a deb as well, and one
    // that overwrites a launcher entry somebody else wrote.
    void anAppImageRunPublishesItsOwnLauncherEntry()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        // It is called ON THE NORMAL STARTUP PATH, not only from the status
        // flag that reports it. A publication function nothing production
        // invokes is the exact shape of the row window that shipped as a
        // permanent no-op, covered six ways and never once reached.
        QVERIFY2(main.contains(QStringLiteral(
                     "LauncherEntryReport publishAppImageLauncherEntry()")),
                 "publishAppImageLauncherEntry is not defined");
        // From the LAST application of the app id — main()'s, since
        // --desktop-status applies it too, earlier in the file.
        const int appIdAt =
            main.lastIndexOf(QStringLiteral("setDesktopFileName(kAppId);"));
        QVERIFY2(appIdAt > 0, "the app id is never applied");
        QVERIFY2(main.indexOf(QStringLiteral(
                     "publishAppImageLauncherEntry();"), appIdAt) > 0,
                 "the launcher entry is never published after the app id is "
                 "set on the startup path: an AppImage would carry an app id "
                 "nothing in the session can resolve, and the window icon "
                 "would be a generic placeholder");

        // Scoped to an AppImage run by BOTH variables the runtime exports. A
        // deb, rpm, flatpak, snap or source run installs a real launcher entry
        // through its own packaging and must never have files written into the
        // user's data directory behind its back.
        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPIMAGE\")")));
        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPDIR\")")));
        QVERIFY(main.contains(QStringLiteral(
            "LIGHTNING_NO_DESKTOP_INTEGRATION")));

        // It never clobbers an entry it did not write. Without the marker
        // there is no way to tell ours from the user's or a distribution's,
        // and overwriting theirs would be data loss dressed as an icon fix.
        QVERIFY(main.contains(QStringLiteral("X-Lightning-Generated=true")));

        // ...and it defers to an entry an installed package published rather
        // than shadowing it. A user-level lightning.desktop wins over
        // /usr/share's by basename, so publishing over a deb's entry would
        // repoint it at the AppImage — and the TryExec key would then HIDE it
        // the day that file is deleted, taking the installed package's
        // launcher with it.
        QVERIFY(main.contains(QStringLiteral("systemLauncherEntry()")));
        QVERIFY(main.contains(QStringLiteral("TryExec=")));

        // And the shipped artifact can be asked whether any of it worked;
        // validate-appimage.sh runs exactly this flag on the real bundle.
        QVERIFY(main.contains(QStringLiteral("--desktop-status")));
    }

    // Qt routes logging to the systemd journal when stderr is not a TTY,
    // which silently hides category logs from piped/offscreen harness runs.
    // main() must keep forcing stderr logging for headless/self-test runs
    // (while an explicit user-provided value still wins) or harness
    // diagnostics regress to producing no output at all.
    void headlessRunsForceStderrLogging()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());
        QVERIFY(main.contains(QStringLiteral(
            "!qEnvironmentVariableIsSet(\"QT_FORCE_STDERR_LOGGING\")")));
        QVERIFY(main.contains(QStringLiteral(
            "platform.startsWith(\"offscreen\")")));
        QVERIFY(main.contains(QStringLiteral(
            "qputenv(\"QT_FORCE_STDERR_LOGGING\", \"1\")")));
    }

    // Every preflight flag that does NOT exit must also be registered with
    // QCommandLineParser, or process() rejects it as unknown and quits.
    //
    // main() parses its own flags twice: a preflight pass before
    // QGuiApplication exists (so --help and a bad --backend are not masked by
    // a platform-plugin abort), and QCommandLineParser afterwards. Most
    // preflight flags EXIT, so they never reach the second parser. The few
    // that are consumed and let the app go on must be declared in both places.
    //
    // `--console` shipped broken for exactly this reason and reached a tester
    // as "matrix-client: Unknown option 'console'." — on the one flag whose
    // whole job is getting a log out of an installed build. It had never
    // worked in any build.
    //
    // DERIVED, not a needle list: the flags come out of the preflight source
    // itself, so a flag added tomorrow is covered without editing this test.
    void parseTimeFlagsSurviveIntoTheQtParser()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        // Each `if (a == QLatin1String("--x"))` / `a.startsWith(...("--x="))`
        // branch, walked to its closing brace so we can ask whether the body
        // assigns r.action (exits) or falls through into the running app.
        static const QRegularExpression branch(
            QStringLiteral("QLatin1String\\(\"--([a-z][a-z0-9-]*)=?\"\\)"));
        QStringList continuing;
        auto it = branch.globalMatch(main);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString flag = m.captured(1);
            // Walk from the match to the end of the enclosing block.
            int depth = 0;
            bool started = false;
            int i = m.capturedEnd(0);
            int end = main.size();
            for (; i < main.size(); ++i) {
                if (main.at(i) == QLatin1Char('{')) { ++depth; started = true; }
                else if (main.at(i) == QLatin1Char('}')) {
                    --depth;
                    if (started && depth <= 0) { end = i; break; }
                }
            }
            if (!started)
                continue;   // not a branch (a help string, a comparison)
            const QString body = main.mid(m.capturedEnd(0),
                                          end - m.capturedEnd(0));
            // `continue;` is the discriminator, not `r.action =`: a flag can
            // set an ERROR action on a bad value (--log-file with no path)
            // and still fall through on a good one. Every branch that lets
            // the app run ends in `continue`; every branch that exits ends in
            // `return r`.
            if (!body.contains(QStringLiteral("continue;")))
                continue;
            // A flag can DEFER its exit: --rust-sdk-smoke-test only records
            // `r.smokeTestRequested` here and the action is decided further
            // down, after the other flags have been read. Those still never
            // reach the Qt parser, so the field it sets is the discriminator:
            // if that field is later turned into an r.action, this flag exits.
            static const QRegularExpression field(
                QStringLiteral("r\\.([A-Za-z]\\w*)\\s*="));
            bool deferredExit = false;
            auto fit = field.globalMatch(body);
            while (fit.hasNext()) {
                const QString name = fit.next().captured(1);
                if (name == QStringLiteral("action"))
                    continue;
                const int at = main.indexOf(
                    QStringLiteral("r.%1)").arg(name), end);
                if (at > 0 && main.mid(at, 900)
                                  .contains(QStringLiteral("r.action ="))) {
                    deferredExit = true;
                    break;
                }
            }
            if (!deferredExit)
                continuing << flag;
        }

        // The derivation has to have found something, or an assertion over an
        // empty list would pass while measuring nothing.
        QVERIFY2(continuing.contains(QStringLiteral("console")),
                 "the scan did not find --console; the derivation is broken");
        QVERIFY2(continuing.contains(QStringLiteral("log-file")),
                 "the scan did not find --log-file; the derivation is broken");

        // Look only AFTER the parser is declared, and for the bare quoted
        // name: the demo flags are registered from a `for (const char *name :
        // {...})` list rather than one QCommandLineOption each, so a needle
        // shaped like QStringLiteral("x") would miss them and report a defect
        // that is not there.
        const int parserAt =
            main.indexOf(QStringLiteral("QCommandLineParser parser;"));
        QVERIFY(parserAt > 0);
        const QString registrations = main.mid(parserAt);

        for (const QString &flag : std::as_const(continuing)) {
            const QString needle = QStringLiteral("\"%1\"").arg(flag);
            QVERIFY2(registrations.contains(needle),
                     qPrintable(QStringLiteral(
                         "--%1 is consumed by preflight, does not exit, and is "
                         "not registered with QCommandLineParser: process() "
                         "will reject it as an unknown option").arg(flag)));
        }
    }

    // --log-file's message handler must SERIALIZE its write.
    //
    // Qt calls message handlers from ARBITRARY THREADS, and two writers are
    // shipped and unconditional: the GUI-stall watchdog logs from a raw
    // std::thread (src/app/GuiStallTracer.cpp) and PlayableWriteWorker from
    // its own QThread (src/media/PlayableFileWriter.cpp). The handler owns a
    // QFile, and neither QFile nor QTextStream is thread-safe. CLAUDE.md's
    // own capture recipe pairs LIGHTNING_GUI_STALL_TRACE with --log-file, so
    // the documented diagnostic procedure IS the concurrent configuration.
    //
    // WHAT THIS CASE IS: a source scan. It says the lock is written and that
    // it covers the write, the flush and the publish — nothing about
    // behaviour under contention. src/main.cpp defines main() and cannot be
    // linked into a test (CMakeLists.txt records the same limitation), which
    // is why the preflight-flag case above is a scan too. Every step is
    // derived from the source and self-checked against its own needles, so
    // it cannot pass by matching nothing.
    void theLogFileHandlerSerializesItsWrite()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        // The brace-matched body of one free function.
        auto bodyOf = [&main](const QString &signature) -> QString {
            const int at = main.indexOf(signature);
            if (at < 0)
                return {};
            const int open = main.indexOf(QLatin1Char('{'), at);
            if (open < 0)
                return {};
            int depth = 0;
            for (int j = open; j < main.size(); ++j) {
                if (main.at(j) == QLatin1Char('{')) {
                    ++depth;
                } else if (main.at(j) == QLatin1Char('}')) {
                    --depth;
                    if (depth == 0)
                        return main.mid(open, j - open + 1);
                }
            }
            return {};
        };

        const QString handler = bodyOf(QStringLiteral("void logFileHandler("));
        // Self-check: if these needles are gone the handler was renamed or
        // rewritten, and every assertion below would be measuring nothing.
        QVERIFY2(handler.contains(QStringLiteral("QTextStream(g_logFile)")),
                 "the scan did not find the log-file stream write; the "
                 "derivation is broken, not the code");
        QVERIFY2(handler.contains(QStringLiteral("g_logFile->flush()")),
                 "the scan did not find the log-file flush; the derivation "
                 "is broken, not the code");

        static const QRegularExpression locker(
            QStringLiteral("QMutexLocker\\s+\\w+\\(&(\\w+)\\)"));
        const QRegularExpressionMatch held = locker.match(handler);
        QVERIFY2(held.hasMatch(),
                 "src/main.cpp's --log-file handler writes a shared QFile "
                 "with no lock. Qt calls message handlers from arbitrary "
                 "threads and this app logs from at least two non-GUI ones.");

        const QString mutexName = held.captured(1);
        QVERIFY2(main.contains(QStringLiteral("QMutex %1;").arg(mutexName)),
                 qPrintable(QStringLiteral("the handler locks %1, which is "
                                           "not declared as a file-scope "
                                           "QMutex").arg(mutexName)));

        // The lock must cover BOTH the stream write and the flush. One
        // without the other still interleaves.
        const int lockAt = held.capturedStart(0);
        const int writeAt = handler.indexOf(QStringLiteral("QTextStream(g_logFile)"));
        const int flushAt = handler.indexOf(QStringLiteral("g_logFile->flush()"));
        QVERIFY2(lockAt < writeAt && lockAt < flushAt,
                 "the --log-file lock is taken after part of the write; it "
                 "must be held across the stream AND the flush");

        // And the pointer must be PUBLISHED under the same lock, or the
        // first line a thread that was already running logs is an
        // unsynchronized read of it.
        const QString install = bodyOf(QStringLiteral("void installLogFile("));
        const int assignAt = install.indexOf(QStringLiteral("g_logFile = file;"));
        QVERIFY2(assignAt > 0,
                 "the scan did not find where g_logFile is published; the "
                 "derivation is broken, not the code");
        const int publishLockAt = install.lastIndexOf(
            QStringLiteral("QMutexLocker"), assignAt);
        QVERIFY2(publishLockAt >= 0,
                 "g_logFile is published without taking the lock that guards "
                 "every read of it");
        QVERIFY2(install.mid(publishLockAt, assignAt - publishLockAt)
                     .contains(mutexName),
                 qPrintable(QStringLiteral("g_logFile is published under a "
                                           "different lock than %1")
                                .arg(mutexName)));
    }

    // THE APPRUN HOOK AND UrlLauncher ARE ONE CONTRACT IN TWO FILES.
    //
    // The hook overrides loader variables for the bundle and preserves each
    // one's session value as APPIMAGE_ORIGINAL_<NAME>; UrlLauncher hands them
    // back to anything Lightning spawns -- the browser for OAuth, a media
    // player -- because a child that keeps them looks inside an AppImage mount
    // that may already be gone. A name added to ONE list and not the other is
    // silent both ways, and it happened immediately: the scanner fix added
    // GST_PLUGIN_SCANNER_1_0 to the hook while UrlLauncher carried only the
    // unversioned spelling, which is the one GStreamer consults SECOND.
    //
    // Derived from both sources rather than written down here, so a name added
    // to the hook is covered without editing this test.
    void everyVariableTheAppRunHookOverridesIsRestoredForChildren()
    {
        const QString hook = readAll(
            QStringLiteral(SOURCE_DIR "/packaging-ci/scripts/build-appimage.sh"));
        QVERIFY2(!hook.isEmpty(), "packaging-ci/scripts/build-appimage.sh missing");
        const int listAt = hook.indexOf(QStringLiteral("for _lightning_var in"));
        QVERIFY2(listAt > 0, "the hook's preserve loop was not found; the "
                             "derivation is broken, not the code");
        const int endAt = hook.indexOf(QStringLiteral("; do"), listAt);
        QVERIFY(endAt > listAt);
        const QStringList preserved =
            hook.mid(listAt, endAt - listAt)
                .remove(QStringLiteral("for _lightning_var in"))
                .remove(QLatin1Char('\\'))
                .split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        // MUTATION GUARD: a scan that matches nothing passes vacuously, and
        // this project has shipped exactly that. Eight names today — the
        // count is deliberately NOT asserted exactly, because the point of
        // deriving the list is that adding a name needs no edit here.
        QVERIFY2(preserved.size() >= 5,
                 qPrintable(QStringLiteral("only %1 names parsed out of the "
                                           "hook; the parse is wrong")
                                .arg(preserved.size())));

        const QString launcher =
            readAll(QStringLiteral(SOURCE_DIR "/src/app/UrlLauncher.cpp"));
        QVERIFY2(!launcher.isEmpty(), "src/app/UrlLauncher.cpp missing");
        for (const QString &name : preserved) {
            QVERIFY2(launcher.contains(QLatin1Char('"') + name + QLatin1Char('"')),
                     qPrintable(QStringLiteral(
                         "the AppRun hook overrides %1 but UrlLauncher never "
                         "restores it, so a spawned child keeps the bundle's "
                         "value and looks inside a mount that may be gone")
                                    .arg(name)));
        }
    }
};

QTEST_GUILESS_MAIN(DesktopIntegrationTest)
#include "DesktopIntegrationTest.moc"
