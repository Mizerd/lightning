// GIF provider key configuration source of truth (the runtime-screenshot
// regression: both providers showed "off" because only the shell launcher —
// not the application — read lightning-gif.env). Covers the safe env-file
// parser, the environment > env-file > build-key precedence with
// empty-never-overrides, discovery via LIGHTNING_GIF_ENV_FILE, controller
// refresh signaling for a picker opened before configuration, one-provider
// configurations, and that no fixture key value ever reaches the log stream.

#include <QtTest/QtTest>

#include <QTemporaryDir>

#include "gif/GifKeyConfig.h"
#include "gif/GifSearchController.h"

using gif::KeySourceClass;
using gif::parseEnvAssignments;
using gif::resolveProviderKeyDetailed;

namespace {

// Synthetic fixture values only — never real keys.
const QByteArray kGiphyFixture = "giphy-fixture-0123456789abcdef";
const QByteArray kKlipyFixture = "klipy-fixture-fedcba9876543210";

class LogCapture
{
public:
    LogCapture()
    {
        s_messages.clear();
        m_previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &msg) {
                s_messages.append(msg);
            });
    }
    ~LogCapture() { qInstallMessageHandler(m_previous); }
    static QStringList messages() { return s_messages; }

private:
    QtMessageHandler m_previous = nullptr;
    inline static QStringList s_messages;
};

} // namespace

class GifKeyConfigTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeEnvFile(const QByteArray &content)
    {
        const QString path = m_dir.filePath(
            QStringLiteral("fixture-%1.env").arg(++m_fileCounter));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(content);
        return path;
    }
    int m_fileCounter = 0;

private slots:
    void init()
    {
        qunsetenv("LIGHTNING_GIPHY_API_KEY");
        qunsetenv("LIGHTNING_KLIPY_API_KEY");
        qunsetenv("LIGHTNING_GIF_ENV_FILE");
    }
    void cleanupTestCase() { init(); }

    void parserHandlesRealWorldEnvFileShapes()
    {
        const QByteArray content =
            "# comment line\r\n"
            "\r\n"
            "LIGHTNING_GIPHY_API_KEY=" + kGiphyFixture + "\r\n"
            "export LIGHTNING_KLIPY_API_KEY=\"" + kKlipyFixture + "\"\r\n"
            "  SPACED_KEY  =  'single quoted'  \n"
            "not a valid line\n"
            "=missing-name\n"
            "BAD NAME=x\n"
            "TRAILING_NO_NEWLINE=last"; // deliberately no final newline
        const auto values = parseEnvAssignments(content);
        QCOMPARE(values.value(QStringLiteral("LIGHTNING_GIPHY_API_KEY")),
                 QString::fromUtf8(kGiphyFixture));
        QCOMPARE(values.value(QStringLiteral("LIGHTNING_KLIPY_API_KEY")),
                 QString::fromUtf8(kKlipyFixture));
        QCOMPARE(values.value(QStringLiteral("SPACED_KEY")),
                 QStringLiteral("single quoted"));
        QCOMPARE(values.value(QStringLiteral("TRAILING_NO_NEWLINE")),
                 QStringLiteral("last"));
        QVERIFY(!values.contains(QStringLiteral("BAD NAME")));
        QVERIFY(!values.contains(QString()));
        // Nothing shell-like is ever evaluated: the invalid lines simply
        // don't exist in the result.
        QCOMPARE(values.size(), 4);
    }

    void missingOrInvalidFileYieldsNothing()
    {
        QCOMPARE(gif::readEnvFile(QString()).size(), 0);
        QCOMPARE(gif::readEnvFile(m_dir.filePath(
                     QStringLiteral("does-not-exist.env"))).size(), 0);
    }

    void environmentBeatsEnvFileBeatsBuildKey()
    {
        const QString file = writeEnvFile(
            "LIGHTNING_GIPHY_API_KEY=" + kGiphyFixture + "\n");
        qputenv("LIGHTNING_GIF_ENV_FILE", file.toUtf8());

        // File source alone.
        auto resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QVERIFY(resolved.configured());
        QCOMPARE(resolved.source, KeySourceClass::EnvFile);
        QCOMPARE(resolved.key, QString::fromUtf8(kGiphyFixture));

        // Process environment wins over the file.
        qputenv("LIGHTNING_GIPHY_API_KEY", "env-fixture-value");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QCOMPARE(resolved.source, KeySourceClass::Environment);
        QCOMPARE(resolved.key, QStringLiteral("env-fixture-value"));

        // An EMPTY environment value falls back to the file (never
        // overrides a valid lower-precedence source).
        qputenv("LIGHTNING_GIPHY_API_KEY", "   ");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QCOMPARE(resolved.source, KeySourceClass::EnvFile);
        QCOMPARE(resolved.key, QString::fromUtf8(kGiphyFixture));

        // An empty FILE value falls through to the build key / absent.
        const QString emptyFile =
            writeEnvFile("LIGHTNING_GIPHY_API_KEY=\"\"\n");
        qputenv("LIGHTNING_GIF_ENV_FILE", emptyFile.toUtf8());
        qunsetenv("LIGHTNING_GIPHY_API_KEY");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        // A source build has no compiled key, so this resolves to the build
        // key when one is embedded and absent otherwise — never the empty
        // file value pretending to be configured.
        if (!resolved.configured())
            QCOMPARE(resolved.source, KeySourceClass::Absent);
        else
            QCOMPARE(resolved.source, KeySourceClass::BuildKey);
    }

    void oneProviderConfiguredLeavesTheOtherIndependent()
    {
        const QString file = writeEnvFile(
            "LIGHTNING_KLIPY_API_KEY='" + kKlipyFixture + "'\n");
        qputenv("LIGHTNING_GIF_ENV_FILE", file.toUtf8());
        const auto klipy = resolveProviderKeyDetailed(QStringLiteral("klipy"));
        QVERIFY(klipy.configured());
        QCOMPARE(klipy.source, KeySourceClass::EnvFile);
        const auto giphy = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        // giphy has no env/file source here; only a compiled key could
        // configure it.
        QVERIFY(giphy.source == KeySourceClass::Absent
                || giphy.source == KeySourceClass::BuildKey);
    }

    void controllerRefreshRecoversAPickerOpenedBeforeConfiguration()
    {
        LogCapture logs;
        // Constructed with no configuration at all: both providers off —
        // the runtime failure from the screenshot.
        GifSearchController controller;
        QVERIFY(!controller.providerConfigured(QStringLiteral("giphy")));
        QVERIFY(!controller.providerConfigured(QStringLiteral("klipy")));

        // Configuration becomes available (env file appears / env set).
        const QString file = writeEnvFile(
            "LIGHTNING_GIPHY_API_KEY=" + kGiphyFixture + "\n"
            "LIGHTNING_KLIPY_API_KEY=" + kKlipyFixture + "\n");
        qputenv("LIGHTNING_GIF_ENV_FILE", file.toUtf8());

        QSignalSpy configSpy(&controller,
                             &GifSearchController::providerConfigurationChanged);
        controller.refreshProviderKeys();
        QCOMPARE(configSpy.count(), 1);
        QVERIFY(controller.providerConfigured(QStringLiteral("giphy")));
        QVERIFY(controller.providerConfigured(QStringLiteral("klipy")));

        // A second refresh with unchanged state stays quiet.
        controller.refreshProviderKeys();
        QCOMPARE(configSpy.count(), 1);

        // The test seam pins a provider; refresh must not clobber it.
        controller.setApiKey(QStringLiteral("giphy"),
                             QStringLiteral("pinned-fixture"));
        controller.refreshProviderKeys();
        QVERIFY(controller.providerConfigured(QStringLiteral("giphy")));

        // No fixture key value ever reached the log stream.
        const QString joined = logs.messages().join(QLatin1Char('\n'));
        QVERIFY(!joined.contains(QString::fromUtf8(kGiphyFixture)));
        QVERIFY(!joined.contains(QString::fromUtf8(kKlipyFixture)));
        QVERIFY(!joined.contains(QStringLiteral("pinned-fixture")));
    }
};

QTEST_GUILESS_MAIN(GifKeyConfigTest)
#include "GifKeyConfigTest.moc"
