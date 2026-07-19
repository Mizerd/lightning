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
            QStringLiteral("Exec=matrix-client --backend=rust")));
        // X11 window association: Qt derives WM_CLASS from the application
        // name ("matrix-client").
        QVERIFY(desktop.contains(
            QStringLiteral("StartupWMClass=matrix-client")));
    }

    void hicolorIconsExistAndMatchDeclaredSizes()
    {
        const QList<int> sizes = {16, 32, 48, 64, 128, 192};
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
            "data/icons/hicolor/192x192/apps/lightning.png")));
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
            "icons/hicolor/192x192/apps/lightning.png")));
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
};

QTEST_GUILESS_MAIN(DesktopIntegrationTest)
#include "DesktopIntegrationTest.moc"
