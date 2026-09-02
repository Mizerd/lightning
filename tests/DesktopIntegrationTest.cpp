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
        // Wayland app_id ↔ desktop entry.
        QVERIFY(main.contains(QStringLiteral(
            "setDesktopFileName(QStringLiteral(\"lightning\"))")));
        // Themed icon with the bundled fallback.
        QVERIFY(main.contains(QStringLiteral("QIcon::fromTheme")));
        QVERIFY(main.contains(QStringLiteral(
            "icons/hicolor/256x256/apps/lightning.png")));
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
};

QTEST_GUILESS_MAIN(DesktopIntegrationTest)
#include "DesktopIntegrationTest.moc"
