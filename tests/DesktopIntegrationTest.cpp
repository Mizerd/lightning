// Desktop-integration contract: the tracked launcher entry, the generated
// hicolor icon set, the CMake install rules that ship them and the Qt-side
// identity wiring. Source-tree checks in the style of ThemeTokensTest; real
// desktop behaviour needs a live desktop test.

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
        // X11 association: Qt's xcb plugin takes the WM_CLASS instance from
        // argv[0] ("lightning-matrix") and the class from the application name.
        QVERIFY(desktop.contains(
            QStringLiteral("StartupWMClass=lightning-matrix")));
    }

    // One startup line names the scene graph backend actually in use, not
    // only the software-fallback warning: an absent warning is otherwise
    // indistinguishable from one never written, and setGraphicsApi() is a
    // request, not an answer.
    void theSceneGraphBackendIsReportedWhateverItTurnsOutToBe()
    {
        const QString main = readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY2(!main.isEmpty(), "src/main.cpp missing");
        // The failure branch stays.
        QVERIFY2(main.contains(QStringLiteral("falling back to the software "
                                              "renderer")),
                 "the OpenGL probe no longer warns when it degrades");
        // The positive line exists, runs when the scene graph comes up, and
        // reads the renderer interface rather than the request.
        QVERIFY2(main.contains(QStringLiteral("scene graph backend=")),
                 "nothing reports which backend the scene graph got, so a "
                 "silent software fallback is unreadable from a log");
        QVERIFY2(main.contains(QStringLiteral("sceneGraphInitialized")),
                 "the backend line is not tied to the scene graph coming up, "
                 "so it cannot be reporting what Qt actually chose");
        QVERIFY2(main.contains(QStringLiteral("rendererInterface()")),
                 "the backend line reads the requested API rather than the "
                 "one in use; setGraphicsApi is a request, not an answer");
        // Every backend Qt can report is named (a new one reads "unknown").
        // Searched after the anchor, since bare "Software"/"OpenGL" also
        // appear in the probe earlier in the file.
        const int at = main.indexOf(QStringLiteral("sceneGraphInitialized"));
        QVERIFY2(at > 0, "no anchor to measure the backend names against");
        for (const char *api : { "QSGRendererInterface::Software",
                                 "QSGRendererInterface::OpenGL",
                                 "QSGRendererInterface::Vulkan",
                                 "QSGRendererInterface::Metal",
                                 "QSGRendererInterface::Direct3D11" }) {
            QVERIFY2(main.indexOf(QLatin1String(api), at) > at,
                     qPrintable(QStringLiteral("the backend line does not "
                                               "name %1").arg(QLatin1String(api))));
        }
    }

    // The binary and CMake target are `lightning-matrix`, but the persistent
    // identity (QSettings names, store roots) must not follow: renaming it
    // would sign every existing install out. Both halves are pinned.
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

        // Packaged Windows keeps Lightning.exe; the updater looks for exactly
        // that.
        const QString updater = readAll(QStringLiteral(SOURCE_DIR "/src/updater/main.cpp"));
        QVERIFY(updater.contains(QStringLiteral(
            "kPortableExecutableName = QStringLiteral(\"Lightning.exe\")")));

        // The stored identity is unchanged on purpose.
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
        // The scalable vector of the mark: a real inline SVG, never an
        // embedded raster.
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
        // Wayland app_id and desktop entry come from one constant (kAppId):
        // the app id, the entry basename and its Icon= key cannot drift.
        // Applied through resolvedAppId(), where a Flatpak's exported id
        // overrides it (see theAppIdFollowsTheEntryAFlatpakActuallyExports).
        QVERIFY(main.contains(QStringLiteral(
            "constexpr QLatin1String kAppId(\"lightning\")")));
        QVERIFY(main.contains(QStringLiteral(
            "setDesktopFileName(resolvedAppId())")));
        // Themed icon with the bundled fallback.
        QVERIFY(main.contains(QStringLiteral("QIcon::fromTheme")));
        QVERIFY(main.contains(QStringLiteral(
            "icons/hicolor/256x256/apps/lightning.png")));
    }

    // Inside a Flatpak the app id must be $FLATPAK_ID. A Flatpak exports only
    // app-id-prefixed files (org.lightning_matrix.Lightning.desktop), and the
    // compositor resolves QGuiApplication::desktopFileName() against installed
    // entries, so the built-in "lightning" gets a generic icon. $FLATPAK_ID is
    // the exported entry's basename; elsewhere it is unset and kAppId stands.
    void theAppIdFollowsTheEntryAFlatpakActuallyExports()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        QVERIFY2(main.contains(QStringLiteral("QString resolvedAppId()")),
                 "no resolvedAppId(): nothing can make the app id follow the "
                 "entry a Flatpak actually exports");
        QVERIFY2(main.contains(QStringLiteral(
                     "qEnvironmentVariable(\"FLATPAK_ID\")")),
                 "resolvedAppId() does not read FLATPAK_ID, so a Flatpak "
                 "build still stamps an app id the session cannot resolve. "
                 "Same variable, same reason, as the notification "
                 "desktop-entry hint in NotificationManager.cpp");
        // ...falling back to the built-in id for every other install type.
        QVERIFY2(main.contains(QStringLiteral(
                     "flatpakId.isEmpty() ? QString(kAppId) : flatpakId")),
                 "resolvedAppId() does not fall back to kAppId: a deb, rpm, "
                 "AppImage or source run would lose its app id entirely");

        // Every application of the app id goes through it: main()'s and
        // --desktop-status's call sites are counted.
        const int sites = main.count(QStringLiteral("setDesktopFileName("));
        QVERIFY2(sites >= 2,
                 "the app-id call sites vanished, so this case would pass on "
                 "a build that never sets a desktop file name at all");
        QCOMPARE(main.count(QStringLiteral(
                     "setDesktopFileName(resolvedAppId())")), sites);

        // --desktop-status (run by validate-appimage.sh on the artifact) must
        // look for the id the application actually stamps.
        QVERIFY2(main.contains(QStringLiteral(
                     "const QString appId = resolvedAppId();")),
                 "--desktop-status does not resolve the app id, so its "
                 "launcher-entry and icon lookup searches for the wrong "
                 "basename inside a Flatpak");
        QVERIFY2(main.contains(QStringLiteral(
                     "\"launcher entry basename: \" << appId")),
                 "--desktop-status still reports the built-in basename");

        // What must not follow it:
        //  * the AppImage's self-published lightning.desktop with
        //    Icon=lightning (FLATPAK_ID is never set there);
        //  * WM_CLASS, which xcb takes from argv[0], not the desktop-file name.
        QVERIFY(main.contains(QStringLiteral(
            "kept.append(QStringLiteral(\"Icon=\") + kAppId);")));
        QVERIFY(main.contains(QStringLiteral(
            "kept.append(QStringLiteral(\"StartupWMClass=\") + kWmClass);")));
    }

    // Wayland window icon for an AppImage. Qt's Wayland client implements no
    // icon protocol, so the compositor resolves the app id against installed
    // launcher entries, and an AppImage must publish its own. Pinned: it
    // runs, only for AppImages, and never overwrites someone else's entry.
    void anAppImageRunPublishesItsOwnLauncherEntry()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        // Called on the normal startup path, not only from the status flag.
        QVERIFY2(main.contains(QStringLiteral(
                     "LauncherEntryReport publishAppImageLauncherEntry()")),
                 "publishAppImageLauncherEntry is not defined");
        // After the last application of the app id (main()'s;
        // --desktop-status applies it earlier in the file).
        const int appIdAt =
            main.lastIndexOf(QStringLiteral(
                "setDesktopFileName(resolvedAppId());"));
        QVERIFY2(appIdAt > 0, "the app id is never applied");
        QVERIFY2(main.indexOf(QStringLiteral(
                     "publishAppImageLauncherEntry();"), appIdAt) > 0,
                 "the launcher entry is never published after the app id is "
                 "set on the startup path: an AppImage would carry an app id "
                 "nothing in the session can resolve, and the window icon "
                 "would be a generic placeholder");

        // Scoped to an AppImage run by both runtime variables: other install
        // types have real launcher entries and must not get files written
        // into the user's data directory.
        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPIMAGE\")")));
        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPDIR\")")));
        QVERIFY(main.contains(QStringLiteral(
            "LIGHTNING_NO_DESKTOP_INTEGRATION")));

        // It never overwrites an entry it did not write (the marker tells ours
        // apart).
        QVERIFY(main.contains(QStringLiteral("X-Lightning-Generated=true")));

        // It defers to an installed package's entry: a user-level
        // lightning.desktop would shadow /usr/share's, and its TryExec would
        // hide the package's launcher once the AppImage is deleted.
        QVERIFY(main.contains(QStringLiteral("systemLauncherEntry()")));
        QVERIFY(main.contains(QStringLiteral("TryExec=")));

        // validate-appimage.sh runs this flag on the real bundle.
        QVERIFY(main.contains(QStringLiteral("--desktop-status")));
    }

    // Qt routes logging to the journal when stderr is not a TTY, hiding logs
    // from piped or offscreen harness runs; main() forces stderr for headless
    // and self-test runs unless the user set a value explicitly.
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

    // Every preflight flag that does not exit must also be registered with
    // QCommandLineParser, or process() rejects it as unknown and quits. main()
    // parses flags twice: a preflight pass before QGuiApplication exists, and
    // QCommandLineParser afterwards. The flag set is derived from the
    // preflight source, so new flags are covered automatically.
    void parseTimeFlagsSurviveIntoTheQtParser()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        // Each `if (a == QLatin1String("--x"))` / `a.startsWith(...("--x="))`
        // branch, walked to its closing brace to see whether it exits.
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
        // `continue;` is the discriminator, not `r.action =`: a flag can set
        // an error action on a bad value and fall through on a good one.
        // Continuing branches end in `continue`, exiting ones in `return r`.
            if (!body.contains(QStringLiteral("continue;")))
                continue;
        // A flag can defer its exit (--rust-sdk-smoke-test sets
        // `r.smokeTestRequested` and the action is decided later); if that
        // field later becomes an r.action, the flag exits.
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

        // The derivation found something, or the assertions check nothing.
        QVERIFY2(continuing.contains(QStringLiteral("console")),
                 "the scan did not find --console; the derivation is broken");
        QVERIFY2(continuing.contains(QStringLiteral("log-file")),
                 "the scan did not find --log-file; the derivation is broken");

        // Look after the parser is declared, for the bare quoted name: demo
        // flags are registered from a `for (const char *name : {...})` list.
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

    // --log-file's message handler must serialize its write: Qt calls handlers
    // from arbitrary threads (the GUI-stall watchdog's std::thread,
    // PlayableWriteWorker's QThread), and QFile/QTextStream are not
    // thread-safe. A source scan, since main.cpp cannot be linked into a
    // test; every step self-checks its needles.
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
        // Self-check: missing needles mean the handler changed and the
        // assertions below would measure nothing.
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

        // The lock covers both the stream write and the flush.
        const int lockAt = held.capturedStart(0);
        const int writeAt = handler.indexOf(QStringLiteral("QTextStream(g_logFile)"));
        const int flushAt = handler.indexOf(QStringLiteral("g_logFile->flush()"));
        QVERIFY2(lockAt < writeAt && lockAt < flushAt,
                 "the --log-file lock is taken after part of the write; it "
                 "must be held across the stream AND the flush");

        // The pointer is published under the same lock, or a thread already
        // running reads it unsynchronized.
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

    // Every loader variable the AppRun hook overrides (saving the session
    // value as APPIMAGE_ORIGINAL_<NAME>) must be restored by UrlLauncher for
    // children, or a spawned browser or player looks inside a possibly
    // unmounted AppImage. Both lists are derived from the sources.
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
        // Mutation guard: a scan that matches nothing passes vacuously. The
        // count is a floor, not exact, so adding a name needs no edit here.
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
