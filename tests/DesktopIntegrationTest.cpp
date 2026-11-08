
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QSet>
#include <QtTest/QtTest>
#include "app/ApplicationBadge.h"

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

}

class DesktopIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void launcherBadgeUsesAnEscapedObjectPath()
    {
        QCOMPARE(lightning::badgeObjectPath(QStringLiteral("matrix-client")),
                 QStringLiteral("/com/canonical/unity/launcherentry/matrix_2dclient"));
        QSet<QString> paths;
        const QRegularExpression valid(QStringLiteral("^(/[A-Za-z0-9_]+)+$"));
        for (const QString &name : {QString{}, QStringLiteral("matrix_client"),
                                    QStringLiteral("matrix_2dclient"),
                                    QStringLiteral("a/b.c"), QString::fromUtf8("ž")}) {
            const QString path = lightning::badgeObjectPath(name);
            QVERIFY(valid.match(path).hasMatch());
            QVERIFY(!paths.contains(path));
            paths.insert(path);
        }
#ifdef HAVE_QT_DBUS
        const auto signal = lightning::badgeSignal(QStringLiteral("matrix-client"),
                                                   QStringLiteral("lightning"), 12000);
        QCOMPARE(signal.path(), lightning::badgeObjectPath(QStringLiteral("matrix-client")));
        QCOMPARE(signal.interface(), QStringLiteral("com.canonical.Unity.LauncherEntry"));
        QCOMPARE(signal.member(), QStringLiteral("Update"));
        QCOMPARE(signal.arguments().at(0).toString(), QStringLiteral("application://lightning.desktop"));
        const auto properties = signal.arguments().at(1).toMap();
        QCOMPARE(properties.value(QStringLiteral("count")).toLongLong(), 9999);
        QVERIFY(properties.value(QStringLiteral("count-visible")).toBool());
        const auto clear = lightning::badgeSignal(QStringLiteral("matrix-client"),
                                                  QStringLiteral("lightning"), -1);
        QVERIFY(!clear.arguments().at(1).toMap().value(QStringLiteral("count-visible")).toBool());
        QVERIFY(!clear.arguments().at(1).toMap().contains(QStringLiteral("count")));
        QVERIFY(QDBusConnection::sessionBus().isConnected());
        QVERIFY(QDBusConnection::sessionBus().send(signal));
#endif
        const QString controller = readAll(QStringLiteral(SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(controller.contains(QStringLiteral("lightning::setApplicationBadge(*guiApp,")));
        QVERIFY(!controller.contains(QStringLiteral("guiApp->setBadgeNumber(")));
    }

    void desktopFileIsCoherent()
    {
        const QString desktop =
            readAll(QStringLiteral(SOURCE_DIR "/data/lightning.desktop"));
        QVERIFY2(!desktop.isEmpty(), "data/lightning.desktop missing");
        QVERIFY(desktop.contains(QStringLiteral("Type=Application")));
        QVERIFY(desktop.contains(QStringLiteral("Name=Lightning")));
        QVERIFY(desktop.contains(QStringLiteral("Icon=lightning")));
        QVERIFY(desktop.contains(
            QStringLiteral("Exec=lightning-matrix --backend=rust")));
        QVERIFY(desktop.contains(
            QStringLiteral("StartupWMClass=lightning-matrix")));
    }

    void bothLauncherEntriesHandleMatrixLinks()
    {
        for (const char *path : { SOURCE_DIR "/data/lightning.desktop",
                                  SOURCE_DIR "/packaging-ci/packaging/common/lightning.desktop" }) {
            const QString desktop = readAll(QString::fromUtf8(path));
            QVERIFY2(!desktop.isEmpty(), path);
            QVERIFY2(desktop.contains(QStringLiteral("\nMimeType=x-scheme-handler/matrix;\n")), path);
            QVERIFY2(desktop.contains(QStringLiteral("\nExec=lightning-matrix --backend=rust %u\n")), path);
        }
        const QString snap = readAll(QStringLiteral(SOURCE_DIR "/packaging-ci/scripts/build-snap.sh"));
        QVERIFY(snap.contains(QStringLiteral("Exec=lightning %u|")));
    }

    void theSceneGraphBackendIsReportedWhateverItTurnsOutToBe()
    {
        const QString main = readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY2(!main.isEmpty(), "src/main.cpp missing");
        QVERIFY2(main.contains(QStringLiteral("falling back to the software "
                                              "renderer")),
                 "the OpenGL probe no longer warns when it degrades");
        QVERIFY2(main.contains(QStringLiteral("scene graph backend=")),
                 "nothing reports which backend the scene graph got, so a "
                 "silent software fallback is unreadable from a log");
        QVERIFY2(main.contains(QStringLiteral("sceneGraphInitialized")),
                 "the backend line is not tied to the scene graph coming up, "
                 "so it cannot be reporting what Qt actually chose");
        QVERIFY2(main.contains(QStringLiteral("rendererInterface()")),
                 "the backend line reads the requested API rather than the "
                 "one in use; setGraphicsApi is a request, not an answer");
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

        const QString updater = readAll(QStringLiteral(SOURCE_DIR "/src/updater/main.cpp"));
        QVERIFY(updater.contains(QStringLiteral(
            "kPortableExecutableName = QStringLiteral(\"Lightning.exe\")")));

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
        QVERIFY(QFile::exists(
            QStringLiteral(SOURCE_DIR "/data/icons/lightning-source.png")));
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
        QVERIFY(cmake.contains(QStringLiteral(
            "data/icons/hicolor/256x256/apps/lightning.png")));
        QVERIFY(cmake.contains(QStringLiteral(
            "icons/hicolor/scalable/apps")));
    }

    void qtIdentityIsWired()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());
        QVERIFY(main.contains(QStringLiteral(
            "constexpr QLatin1String kAppId(\"lightning\")")));
        QVERIFY(main.contains(QStringLiteral(
            "setDesktopFileName(resolvedAppId())")));
        QVERIFY(main.contains(QStringLiteral("QIcon::fromTheme")));
        QVERIFY(main.contains(QStringLiteral(
            "icons/hicolor/256x256/apps/lightning.png")));
    }

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
        QVERIFY2(main.contains(QStringLiteral(
                     "flatpakId.isEmpty() ? QString(kAppId) : flatpakId")),
                 "resolvedAppId() does not fall back to kAppId: a deb, rpm, "
                 "AppImage or source run would lose its app id entirely");

        const int sites = main.count(QStringLiteral("setDesktopFileName("));
        QVERIFY2(sites >= 2,
                 "the app-id call sites vanished, so this case would pass on "
                 "a build that never sets a desktop file name at all");
        QCOMPARE(main.count(QStringLiteral(
                     "setDesktopFileName(resolvedAppId())")), sites);

        QVERIFY2(main.contains(QStringLiteral(
                     "const QString appId = resolvedAppId();")),
                 "--desktop-status does not resolve the app id, so its "
                 "launcher-entry and icon lookup searches for the wrong "
                 "basename inside a Flatpak");
        QVERIFY2(main.contains(QStringLiteral(
                     "\"launcher entry basename: \" << appId")),
                 "--desktop-status still reports the built-in basename");

        QVERIFY(main.contains(QStringLiteral(
            "kept.append(QStringLiteral(\"Icon=\") + kAppId);")));
        QVERIFY(main.contains(QStringLiteral(
            "kept.append(QStringLiteral(\"StartupWMClass=\") + kWmClass);")));
    }

    void anAppImageRunPublishesItsOwnLauncherEntry()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        QVERIFY2(main.contains(QStringLiteral(
                     "LauncherEntryReport publishAppImageLauncherEntry()")),
                 "publishAppImageLauncherEntry is not defined");
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

        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPIMAGE\")")));
        QVERIFY(main.contains(QStringLiteral("qgetenv(\"APPDIR\")")));
        QVERIFY(main.contains(QStringLiteral(
            "LIGHTNING_NO_DESKTOP_INTEGRATION")));

        QVERIFY(main.contains(QStringLiteral("X-Lightning-Generated=true")));

        QVERIFY(main.contains(QStringLiteral("systemLauncherEntry()")));
        QVERIFY(main.contains(QStringLiteral("TryExec=")));

        QVERIFY(main.contains(QStringLiteral("--desktop-status")));
    }

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

    void parseTimeFlagsSurviveIntoTheQtParser()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

        static const QRegularExpression branch(
            QStringLiteral("QLatin1String\\(\"--([a-z][a-z0-9-]*)=?\"\\)"));
        QStringList continuing;
        auto it = branch.globalMatch(main);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString flag = m.captured(1);
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
                continue;
            const QString body = main.mid(m.capturedEnd(0),
                                          end - m.capturedEnd(0));
            if (!body.contains(QStringLiteral("continue;")))
                continue;
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

        QVERIFY2(continuing.contains(QStringLiteral("console")),
                 "the scan did not find --console; the derivation is broken");
        QVERIFY2(continuing.contains(QStringLiteral("log-file")),
                 "the scan did not find --log-file; the derivation is broken");

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

    void theLogFileHandlerOnlyQueues()
    {
        const QString main =
            readAll(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        QVERIFY(!main.isEmpty());

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
        const QString mirror = bodyOf(QStringLiteral("void mirrorToLogFile("));
        const QString install = bodyOf(QStringLiteral("void installLogFile("));
        QVERIFY2(!handler.isEmpty() && !mirror.isEmpty() && !install.isEmpty(),
                 "the scan did not find the --log-file functions; the "
                 "derivation is broken, not the code");

        QVERIFY2(handler.contains(QStringLiteral("->log(")),
                 "logFileHandler no longer queues through the sink's log()");
        QVERIFY2(handler.contains(QStringLiteral("->logFatal(")),
                 "logFileHandler no longer drains the sink on QtFatalMsg: a "
                 "fatal line would be lost to the abort");
        QVERIFY2(mirror.contains(QStringLiteral("->writeVerbatim(")),
                 "mirrorToLogFile no longer queues through the sink");

        const QStringList forbidden = {
            QStringLiteral("QTextStream"), QStringLiteral("QMutexLocker"),
            QStringLiteral("QFile"),       QStringLiteral("->write("),
            QStringLiteral("flush()"),
        };
        for (const QString &needle : forbidden) {
            QVERIFY2(!handler.contains(needle),
                     qPrintable(QStringLiteral("logFileHandler uses %1: it "
                                               "must only queue").arg(needle)));
            QVERIFY2(!mirror.contains(needle),
                     qPrintable(QStringLiteral("mirrorToLogFile uses %1: it "
                                               "must only queue").arg(needle)));
        }

        const int sinkAt = install.indexOf(QStringLiteral("setProcessLogSink("));
        const QString ensure =
            bodyOf(QStringLiteral("void ensureLogHandlerInstalled("));
        QVERIFY2(ensure.contains(
                     QStringLiteral("qInstallMessageHandler(logFileHandler)")),
                 "ensureLogHandlerInstalled no longer installs logFileHandler");
        const int handlerAt =
            install.indexOf(QStringLiteral("ensureLogHandlerInstalled("));
        QVERIFY2(install.contains(QStringLiteral("new AsyncLogSink(")),
                 "installLogFile does not hand the file to an AsyncLogSink");
        QVERIFY2(install.contains(QStringLiteral("std::atexit(")),
                 "installLogFile registers no exit drain: the status "
                 "commands' output would not reach the file");
        QVERIFY2(sinkAt >= 0 && handlerAt > sinkAt,
                 "the handler is installed before the sink it reads is "
                 "published");
    }

    void anExitThatSkipsTeardownDrainsTheLogFirst()
    {
        const QString player = readAll(
            QStringLiteral(SOURCE_DIR "/src/calls/CallSoundPlayer.cpp"));
        QVERIFY(!player.isEmpty());
        const int exitAt = player.indexOf(QStringLiteral("std::_Exit("));
        QVERIFY2(exitAt >= 0, "CallSoundPlayer no longer calls std::_Exit");
        const int drainAt = player.lastIndexOf(
            QStringLiteral("flushProcessLog("), exitAt);
        const int bodyAt = player.lastIndexOf(
            QStringLiteral("void endWithoutTeardown()"), exitAt);
        QVERIFY2(bodyAt >= 0 && drainAt > bodyAt,
                 "endWithoutTeardown() reaches std::_Exit without draining "
                 "the --log-file sink");
    }

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
