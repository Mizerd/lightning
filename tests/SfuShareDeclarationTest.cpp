#include "calls/SfuCallController.h"
#include "calls/SfuMediaEngine.h"
#include "matrix/MockMatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <gst/gst.h>

namespace {
class DeclaringClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    void sfuAddTrackFor(quint64, int, const QString &cid, const QString &name,
                        int, int, int, bool, bool) override
    {
        if (name == QLatin1String("screen"))
            calls << QStringLiteral("add:") + cid;
    }
    void sfuAddShareWithBackupFor(quint64, const QString &cid,
                                  const QString &backupCid, int, int,
                                  bool) override
    {
        calls << QStringLiteral("backup:") + cid + QLatin1Char('+') + backupCid;
    }
    void sfuLocalDescriptionFor(quint64, int, const QString &kind,
                                const QString &target, const QString &) override
    {
        if (kind == QLatin1String("offer")
            && target == QLatin1String("publisher"))
            calls << QStringLiteral("offer");
    }
    QStringList calls;
};
}

class SfuShareDeclarationTest : public QObject
{
    Q_OBJECT

private:
    static bool standInAvailable()
    {
        return gst_init_check(nullptr, nullptr, nullptr)
            && SfuMediaEngine::firstMissingElement(
                   {QByteArrayLiteral("openh264enc"),
                    QByteArrayLiteral("h264parse")}).isEmpty();
    }
    static void useHardware(bool on)
    {
        SfuMediaEngine::clearShareHardwareEncoderFailedForTest();
        if (on) {
            qputenv("LIGHTNING_SHARE_ENCODER", "hardware");
            SfuMediaEngine::setShareHardwareEncoderForTest(
                SfuMediaEngine::ShareHardwareEncoder{
                    {"openh264enc", "bitrate=%1 gop-size=%2",
                     "constrained-baseline", false},
                    false});
        } else {
            qunsetenv("LIGHTNING_SHARE_ENCODER");
            SfuMediaEngine::setShareHardwareEncoderForTest(std::nullopt);
        }
    }

private slots:
    void initTestCase()
    {
        qputenv("LIGHTNING_SHARE_GPU", "0");
        QString whyNot;
        if (!SfuMediaEngine::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(whyNot));
    }
    void cleanup() { useHardware(false); }

    void aSoftwareShareDeclaresAPlainTrackBeforeItsOffer()
    {
        useHardware(false);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        QCOMPARE(client.calls.size() >= 2, true);
        QVERIFY2(client.calls.first().startsWith(QLatin1String("add:")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        engine.stop();
    }

    void aBackedUpShareDeclaresBothCodecsBeforeItsOffer()
    {
        if (!standInAvailable())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        useHardware(true);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        const QString first = client.calls.first();
        QVERIFY2(first.startsWith(QLatin1String("backup:"))
                     && first.endsWith(QLatin1String("-vp8")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        for (const QString &entry : client.calls)
            QVERIFY2(!entry.startsWith(QLatin1String("add:")), qPrintable(entry));
        engine.stop();
    }

    // A failure inside publishVideo() must not re-enter startScreenShare().
    void aBackupThatCannotStartDeclaresNothingAndEndsTheShareAfterwards()
    {
        if (!standInAvailable())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        useHardware(true);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        engine.failNextShareBackupForTest();
        QVERIFY(call.startScreenShare(0));
        QVERIFY2(call.screenSharing(),
                 "the failure was handled inside startScreenShare()");
        QTRY_VERIFY_WITH_TIMEOUT(!call.screenSharing(), 5000);
        QVERIFY(client.calls.isEmpty());
        QVERIFY(SfuMediaEngine::shareHardwareEncoderFailedForTest());

        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        QVERIFY2(client.calls.first().startsWith(QLatin1String("add:")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        engine.stop();
    }
};

QTEST_MAIN(SfuShareDeclarationTest)
#include "SfuShareDeclarationTest.moc"
