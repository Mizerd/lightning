
#include <QtTest/QtTest>

#include <QTemporaryDir>

#include "gif/GifKeyConfig.h"
#include "gif/GifSearchController.h"

using gif::KeySourceClass;
using gif::parseEnvAssignments;
using gif::resolveProviderKeyDetailed;

namespace {

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

}

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
            "TRAILING_NO_NEWLINE=last";
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

        auto resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QVERIFY(resolved.configured());
        QCOMPARE(resolved.source, KeySourceClass::EnvFile);
        QCOMPARE(resolved.key, QString::fromUtf8(kGiphyFixture));

        qputenv("LIGHTNING_GIPHY_API_KEY", "env-fixture-value");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QCOMPARE(resolved.source, KeySourceClass::Environment);
        QCOMPARE(resolved.key, QStringLiteral("env-fixture-value"));

        qputenv("LIGHTNING_GIPHY_API_KEY", "   ");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
        QCOMPARE(resolved.source, KeySourceClass::EnvFile);
        QCOMPARE(resolved.key, QString::fromUtf8(kGiphyFixture));

        const QString emptyFile =
            writeEnvFile("LIGHTNING_GIPHY_API_KEY=\"\"\n");
        qputenv("LIGHTNING_GIF_ENV_FILE", emptyFile.toUtf8());
        qunsetenv("LIGHTNING_GIPHY_API_KEY");
        resolved = resolveProviderKeyDetailed(QStringLiteral("giphy"));
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
        QVERIFY(giphy.source == KeySourceClass::Absent
                || giphy.source == KeySourceClass::BuildKey);
    }

    void controllerRefreshRecoversAPickerOpenedBeforeConfiguration()
    {
        LogCapture logs;
        GifSearchController controller;
        QVERIFY(!controller.providerConfigured(QStringLiteral("giphy")));
        QVERIFY(!controller.providerConfigured(QStringLiteral("klipy")));

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

        controller.refreshProviderKeys();
        QCOMPARE(configSpy.count(), 1);

        controller.setApiKey(QStringLiteral("giphy"),
                             QStringLiteral("pinned-fixture"));
        controller.refreshProviderKeys();
        QVERIFY(controller.providerConfigured(QStringLiteral("giphy")));

        const QString joined = logs.messages().join(QLatin1Char('\n'));
        QVERIFY(!joined.contains(QString::fromUtf8(kGiphyFixture)));
        QVERIFY(!joined.contains(QString::fromUtf8(kKlipyFixture)));
        QVERIFY(!joined.contains(QStringLiteral("pinned-fixture")));
    }
};

QTEST_GUILESS_MAIN(GifKeyConfigTest)
#include "GifKeyConfigTest.moc"
