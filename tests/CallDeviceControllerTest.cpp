// The camera choice as it leaves CallDeviceController for the media engine
// (review M1). An unplugged chosen camera used to come out as "" ("system
// default"), so the engine's refusal never ran and the default camera opened.
#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "app/SettingsManager.h"
#include "calls/CallDeviceController.h"

class CallDeviceControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-device-controller-test"));
    }

    void aChosenCameraThatIsNotConnectedIsReportedMissing()
    {
        SettingsManager settings;
        CallDeviceController devices;
        devices.setSettings(&settings);

        // No choice: the system default, nothing missing.
        settings.setPreferredCameraId(QString());
        auto selection = devices.cameraSelection();
        QVERIFY(selection.id.isEmpty());
        QVERIFY(!selection.preferredMissing);

        // A camera no machine has: chosen, and not connected.
        settings.setPreferredCameraId(
            QStringLiteral("/dev/video-lightning-test-missing"));
        selection = devices.cameraSelection();
        QVERIFY(selection.id.isEmpty());
        QVERIFY2(selection.preferredMissing,
                 "an unplugged chosen camera left as 'system default'; the "
                 "engine would open the default camera");
        QVERIFY(devices.activeCameraId().isEmpty());

        // Choosing "system default" again clears it.
        devices.selectCamera(QString());
        QVERIFY(!devices.cameraSelection().preferredMissing);
        QVERIFY(settings.preferredCameraDescription().isEmpty());
    }

    // Final review S5: re-picking the same camera announced nothing (the id
    // did not change), and leaving "missing" for "system default" changed no
    // id either, so the engine kept refusing a camera nobody had chosen.
    void aRePickAndTheEndOfAMissingCameraAreAnnounced()
    {
        SettingsManager settings;
        CallDeviceController devices;
        devices.setSettings(&settings);
        const QString gone = QStringLiteral("/dev/video-lightning-test-gone");
        devices.selectCamera(gone);
        QVERIFY(devices.preferredCameraMissing());

        QSignalSpy selection(&devices, &CallDeviceController::selectionChanged);
        QSignalSpy active(&devices,
                          &CallDeviceController::activeDevicesChanged);
        devices.selectCamera(gone); // the same id again
        QVERIFY2(!selection.isEmpty(),
                 "re-picking the same camera announced nothing");

        selection.clear();
        active.clear();
        devices.selectCamera(QString()); // back to "system default"
        QVERIFY(!devices.preferredCameraMissing());
        QVERIFY(!selection.isEmpty());
        QVERIFY2(!active.isEmpty(),
                 "missing -> system default keeps the empty id, and the "
                 "engine was never told the camera may open again");
    }

    // The same two surfaces, as source: a missing camera selects no row in
    // either (never "System default" beside the chosen one), and each says
    // why. Loading them needs the whole app; the rows' `chosen` comes from
    // cameras(), which the case above exercises through the same selection.
    void aMissingCameraSelectsNoRowAndSaysSo()
    {
        const auto read = [](const char *name) {
            QFile file(QStringLiteral(QML_DIR "/") + QLatin1String(name));
            return file.open(QIODevice::ReadOnly)
                ? QString::fromUtf8(file.readAll()).simplified()
                : QString();
        };
        const QString menu = read("CallDeviceMenu.qml");
        QVERIFY(!menu.isEmpty());
        QVERIFY2(QRegularExpression(QStringLiteral(
                     "root\\.kind === \"camera\"\\) return "
                     "app\\.callDevices\\.activeCameraId === \"\" && "
                     "!app\\.callDevices\\.preferredCameraMissing"))
                     .match(menu)
                     .hasMatch(),
                 "the menu's 'System default' radio is selected while the "
                 "chosen camera is missing");
        QVERIFY(menu.contains(QStringLiteral("\"callMenuCameraMissing\"")));

        const QString settingsUi = read("CallDeviceSettings.qml");
        QVERIFY(!settingsUi.isEmpty());
        QVERIFY2(settingsUi.contains(QStringLiteral(
                     "if (picker.missing) return -1;")),
                 "Settings selects 'System default' while the chosen camera "
                 "is missing");
        QVERIFY(settingsUi.contains(QStringLiteral(
            "return root.activated && app.callDevices.preferredCameraMissing;")));
        QVERIFY(settingsUi.contains(
            QStringLiteral("\"callDeviceCameraMissing\"")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(CallDeviceControllerTest)
#include "CallDeviceControllerTest.moc"
