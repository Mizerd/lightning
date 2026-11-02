#include "calls/CaptureDeviceSelection.h"

#include <QtTest/QtTest>

using lightning::calls::CaptureKind;
using lightning::calls::DeviceBinding;
using lightning::calls::GstDeviceCandidate;
using lightning::calls::chooseCaptureElement;
using lightning::calls::ElementChoice;
using lightning::calls::identityKeysForElement;
using lightning::calls::resolveDeviceBinding;

namespace {

GstDeviceCandidate v4l2(const QString &name, const QString &path)
{
    return GstDeviceCandidate{name, {{QStringLiteral("device.path"), path},
                                     {QStringLiteral("api.v4l2.path"), path}}};
}

GstDeviceCandidate pipewireCamera(const QString &name, const QString &path,
                                  const QString &serial)
{
    return GstDeviceCandidate{
        name + QStringLiteral(" (V4L2)"),
        {{QStringLiteral("api.v4l2.path"), path},
         {QStringLiteral("object.path"), QStringLiteral("v4l2:") + path},
         {QStringLiteral("object.serial"), serial},
         {QStringLiteral("device.api"), QStringLiteral("v4l2")},
         {QStringLiteral("media.class"), QStringLiteral("Video/Source")}}};
}

GstDeviceCandidate pipewire(const QString &name, const QString &serial,
                            const QString &nodeName)
{
    return GstDeviceCandidate{name,
                              {{QStringLiteral("object.serial"), serial},
                               {QStringLiteral("node.name"), nodeName}}};
}

}

class CaptureDeviceSelectionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aFourInputInterfaceTakesItsFirstInputNotTheMeanOfFour()
    {
        using namespace lightning::calls;
        QCOMPARE(captureMixMatrix(4),
                 QStringLiteral("mix-matrix=\"<<(float)1.0,(float)0.0,"
                                "(float)0.0,(float)0.0>>\""));
        QCOMPARE(captureChannelCaps(4),
                 QStringLiteral("! audio/x-raw,channels=4,"
                                "channel-mask=(bitmask)0x0 "));
    }

    void oneAndTwoChannelDevicesAreMixedExactlyAsBefore()
    {
        using namespace lightning::calls;
        for (const int channels : {0, 1, 2}) {
            QVERIFY2(captureMixMatrix(channels).isEmpty(),
                     qPrintable(QStringLiteral("channels=%1").arg(channels)));
            QVERIFY2(captureChannelCaps(channels).isEmpty(),
                     qPrintable(QStringLiteral("channels=%1").arg(channels)));
        }
    }

    void anAbsurdChannelCountIsRefusedRatherThanBuiltInto()
    {
        using namespace lightning::calls;
        QVERIFY(!captureMixMatrix(8).isEmpty());
        QVERIFY(!captureMixMatrix(64).isEmpty());
        QVERIFY(captureMixMatrix(65).isEmpty());
        QVERIFY(captureMixMatrix(1000000).isEmpty());
        QVERIFY(captureMixMatrix(-4).isEmpty());
    }

    void theBindingCarriesTheDevicesChannelCount()
    {
        GstDeviceCandidate rubix;
        rubix.displayName = QStringLiteral("Rubix44 Analog Surround 4.0");
        rubix.properties.insert(
            QStringLiteral("node.name"),
            QStringLiteral("alsa_input.usb-Roland_Rubix44-00.analog-surround-40"));
        rubix.properties.insert(QStringLiteral("object.serial"),
                                QStringLiteral("8732"));
        rubix.properties.insert(QStringLiteral("audio.channels"),
                                QStringLiteral("4"));
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("pipewiresrc"),
            QStringLiteral(
                "alsa_input.usb-Roland_Rubix44-00.analog-surround-40"),
            QStringLiteral("Rubix44 Analog Surround 4.0"), {rubix});
        QCOMPARE(binding.value, QStringLiteral("8732"));
        QCOMPARE(binding.reason, QStringLiteral("identity"));
        QCOMPARE(binding.channels, 4);

        GstDeviceCandidate plain = rubix;
        plain.properties.remove(QStringLiteral("audio.channels"));
        QCOMPARE(resolveDeviceBinding(
                     CaptureKind::Microphone, QStringLiteral("pipewiresrc"),
                     QStringLiteral("alsa_input.usb-Roland_Rubix44-00."
                                    "analog-surround-40"),
                     QStringLiteral("Rubix44 Analog Surround 4.0"), {plain})
                     .channels,
                 0);
    }

    void aPulseElementBindsFromAPipeWireEnumeratedDevice()
    {
        GstDeviceCandidate mic;
        mic.displayName = QStringLiteral("Built-in Audio Analog Stereo");
        mic.properties.insert(QStringLiteral("node.name"),
                              QStringLiteral("alsa_input.pci-0000_00_1f.3"));
        mic.properties.insert(QStringLiteral("object.serial"),
                              QStringLiteral("59"));
        const DeviceBinding b = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("pulsesrc"),
            QStringLiteral("alsa_input.pci-0000_00_1f.3"),
            QStringLiteral("Built-in Audio Analog Stereo"), {mic});
        QVERIFY2(!b.isEmpty(), "pulsesrc did not bind at all");
        QCOMPARE(b.property, QStringLiteral("device"));
        QCOMPARE(b.value, QStringLiteral("alsa_input.pci-0000_00_1f.3"));
        QCOMPARE(b.reason, QStringLiteral("identity"));
    }

    void anEmptyPreferenceBindsNothing()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            v4l2(QStringLiteral("Integrated Camera"),
                 QStringLiteral("/dev/video0"))};
        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("v4l2src"), QString(),
                                     QStringLiteral("Integrated Camera"),
                                     candidates)
                    .isEmpty());
    }

    void aCameraIsBoundByIdentityWhenTheMonitorAgrees()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            v4l2(QStringLiteral("Integrated Camera"),
                 QStringLiteral("/dev/video0")),
            v4l2(QStringLiteral("HD Pro Webcam C920"),
                 QStringLiteral("/dev/video2"))};
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video2"), QStringLiteral("HD Pro Webcam C920"),
            candidates);
        QCOMPARE(binding.property, QStringLiteral("device"));
        QCOMPARE(binding.value, QStringLiteral("/dev/video2"));
        QCOMPARE(binding.reason, QStringLiteral("identity"));
    }

    void aPipeWireAudioNodeIsBoundByItsDisplayName()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            pipewire(QStringLiteral("Built-in Audio Analog Stereo"),
                     QStringLiteral("68"),
                     QStringLiteral("alsa_input.pci-0000_00_1f.3.analog-stereo")),
            pipewire(QStringLiteral("Rubix44 Analog Surround 4.0"),
                     QStringLiteral("5134"), QStringLiteral("alsa_input.usb-1"))};
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("pipewiresrc"),
            QStringLiteral("alsa_input.pci-0000_00_1f.3.analog-stereo-qt-id"),
            QStringLiteral("Rubix44 Analog Surround 4.0"), candidates);
        QCOMPARE(binding.property, QStringLiteral("target-object"));
        QCOMPARE(binding.value, QStringLiteral("5134"));
        QCOMPARE(binding.reason, QStringLiteral("display-name"));
    }

    void identityWinsOverAMatchingDisplayName()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            v4l2(QStringLiteral("USB Camera"), QStringLiteral("/dev/video0")),
            v4l2(QStringLiteral("USB Camera"), QStringLiteral("/dev/video4"))};
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video4"), QStringLiteral("USB Camera"),
            candidates);
        QCOMPARE(binding.value, QStringLiteral("/dev/video4"));
        QCOMPARE(binding.reason, QStringLiteral("identity"));
    }

    void twoDevicesSharingANameBindNothing()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            v4l2(QStringLiteral("USB Camera"), QStringLiteral("/dev/video0")),
            v4l2(QStringLiteral("USB Camera"), QStringLiteral("/dev/video4"))};
        QVERIFY2(resolveDeviceBinding(CaptureKind::Camera,
                                      QStringLiteral("v4l2src"),
                                      QStringLiteral("some-other-qt-id"),
                                      QStringLiteral("USB Camera"), candidates)
                     .isEmpty(),
                 "an ambiguous display name was resolved to one of the two "
                 "devices");
    }

    void aDeviceThatIsNoLongerPresentBindsNothing()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            v4l2(QStringLiteral("Integrated Camera"),
                 QStringLiteral("/dev/video0"))};
        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("v4l2src"),
                                     QStringLiteral("/dev/video9"),
                                     QStringLiteral("Unplugged Camera"),
                                     candidates)
                    .isEmpty());
    }

    void aDeviceNodePathIsTrustedOnlyWhenTheMonitorCannotAnswer()
    {
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video3"), QStringLiteral("Some Camera"), {});
        QCOMPARE(binding.property, QStringLiteral("device"));
        QCOMPARE(binding.value, QStringLiteral("/dev/video3"));
        QCOMPARE(binding.reason, QStringLiteral("shape"));

        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("v4l2src"),
                                     QStringLiteral("camera:0"),
                                     QStringLiteral("Some Camera"), {})
                    .isEmpty());
        QVERIFY(resolveDeviceBinding(CaptureKind::Microphone,
                                     QStringLiteral("pulsesrc"),
                                     QStringLiteral("alsa_input.usb-Blue"),
                                     QStringLiteral("Blue Yeti"), {})
                    .isEmpty());
    }

    void anUnknownElementBindsNothing()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            pipewire(QStringLiteral("Built-in Audio Analog Stereo"),
                     QStringLiteral("68"), QStringLiteral("alsa_input.pci"))};
        QVERIFY(resolveDeviceBinding(CaptureKind::Microphone,
                                     QStringLiteral("autoaudiosrc"),
                                     QStringLiteral("some-id"),
                                     QStringLiteral("Built-in Audio Analog "
                                                    "Stereo"),
                                     candidates)
                    .isEmpty());
        QVERIFY(identityKeysForElement(QStringLiteral("autoaudiosrc")).isEmpty());
        QVERIFY(!identityKeysForElement(QStringLiteral("v4l2src")).isEmpty());
    }

    void aValueCarryingControlCharactersIsRefused()
    {
        const auto candidates = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("Odd Camera"),
            {{QStringLiteral("device.path"),
              QStringLiteral("/dev/video0\nExec=/bin/sh")}}}};
        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("v4l2src"),
                                     QStringLiteral("wanted"),
                                     QStringLiteral("Odd Camera"), candidates)
                    .isEmpty());
    }

    void theWindowsAndMacProfilesUseTheirOwnPropertyNames()
    {
        const auto ks = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("Integrated Webcam"),
            {{QStringLiteral("device.path"), QStringLiteral("\\\\?\\usb#vid_0c45")}}}};
        const DeviceBinding win = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("ksvideosrc"),
            QStringLiteral("\\\\?\\usb#vid_0c45"),
            QStringLiteral("Integrated Webcam"), ks);
        QCOMPARE(win.property, QStringLiteral("device-path"));
        QCOMPARE(win.reason, QStringLiteral("identity"));

        const auto avf = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("FaceTime HD Camera"),
            {{QStringLiteral("device.unique-id"), QStringLiteral("0x8020000005ac8514")},
             {QStringLiteral("device.index"), QStringLiteral("0")}}}};
        const DeviceBinding mac = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("avfvideosrc"),
            QStringLiteral("0x8020000005ac8514"),
            QStringLiteral("FaceTime HD Camera"), avf);
        QCOMPARE(mac.property, QStringLiteral("device-index"));
        QCOMPARE(mac.value, QStringLiteral("0"));
    }

    void aPipeWireListedCameraBindsByItsV4l2Path()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            pipewireCamera(QStringLiteral("Integrated Camera"),
                           QStringLiteral("/dev/video0"),
                           QStringLiteral("51")),
            pipewireCamera(QStringLiteral("cm-cam2"),
                           QStringLiteral("/dev/video61"),
                           QStringLiteral("88"))};
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video61"), QStringLiteral("cm-cam2"),
            candidates);
        QCOMPARE(binding.property, QStringLiteral("device"));
        QCOMPARE(binding.value, QStringLiteral("/dev/video61"));
        QCOMPARE(binding.reason, QStringLiteral("identity"));
        QVERIFY(!binding.refused);
    }

    // When a chosen camera cannot be bound, the camera is refused: "the
    // default" is another camera, and opening it is a privacy failure. Audio
    // keeps the default, deliberately.
    void aChosenCameraThatCannotBeBoundIsRefusedNotReplaced()
    {
        const auto onlyTheLaptopCamera = QList<GstDeviceCandidate>{
            pipewireCamera(QStringLiteral("Integrated Camera"),
                           QStringLiteral("/dev/video0"),
                           QStringLiteral("51"))};
        const DeviceBinding gone = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video61"), QStringLiteral("cm-cam2"),
            onlyTheLaptopCamera);
        QVERIFY(gone.isEmpty());
        QVERIFY2(gone.refused,
                 "an unbindable camera choice would open the default camera");

        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("autovideosrc"),
                                     QStringLiteral("/dev/video61"),
                                     QStringLiteral("cm-cam2"),
                                     onlyTheLaptopCamera)
                    .refused);

        QVERIFY(!resolveDeviceBinding(CaptureKind::Camera,
                                      QStringLiteral("v4l2src"), QString(),
                                      QString(), onlyTheLaptopCamera)
                     .refused);

        const DeviceBinding mic = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("pulsesrc"),
            QStringLiteral("a-device-that-vanished"), QStringLiteral("Gone"),
            {pipewire(QStringLiteral("Other"), QStringLiteral("5"),
                      QStringLiteral("other"))});
        QVERIFY(mic.isEmpty());
        QVERIFY(!mic.refused);
    }

    void aSerialisedWindowsIdIsUnwrappedBeforeItIsCompared()
    {
        const auto wasapi = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("Microphone (USB Audio)"),
            {{QStringLiteral("device.strid"),
              QStringLiteral("\"\\{0.0.1.00000000\\}.\\{a1b2\\}\"")}}}};
        const DeviceBinding mic = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("wasapisrc"),
            QStringLiteral("{0.0.1.00000000}.{a1b2}"),
            QStringLiteral("Microphone (USB Audio)"), wasapi);
        QCOMPARE(mic.value, QStringLiteral("{0.0.1.00000000}.{a1b2}"));
        QCOMPARE(mic.reason, QStringLiteral("identity"));

        const auto ks = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("Integrated Webcam"),
            {{QStringLiteral("device.path"),
              QStringLiteral("\"\\\\\\\\\\?\\\\usb\\#vid_0c45\\&pid_6a10\"")}}}};
        const DeviceBinding cam = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("ksvideosrc"),
            QStringLiteral("\\\\?\\USB#VID_0C45&PID_6A10"),
            QStringLiteral("Integrated Webcam"), ks);
        QCOMPARE(cam.property, QStringLiteral("device-path"));
        QCOMPARE(cam.value, QStringLiteral("\\\\?\\usb#vid_0c45&pid_6a10"));
        QCOMPARE(cam.reason, QStringLiteral("identity"));
    }

    void theMacProviderUniqueIdBindsTheIndex()
    {
        const auto avf = QList<GstDeviceCandidate>{
            GstDeviceCandidate{QStringLiteral("FaceTime HD Camera"),
                               {{QStringLiteral("avf.unique_id"),
                                 QStringLiteral("0x8020000005ac8514")},
                                {QStringLiteral("device.index"),
                                 QStringLiteral("0")}}},
            GstDeviceCandidate{QStringLiteral("Continuity Camera"),
                               {{QStringLiteral("avf.unique_id"),
                                 QStringLiteral("A1B2-C3")},
                                {QStringLiteral("device.index"),
                                 QStringLiteral("1")}}}};
        const DeviceBinding mac = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("avfvideosrc"),
            QStringLiteral("A1B2-C3"), QStringLiteral("Continuity Camera"),
            avf);
        QCOMPARE(mac.property, QStringLiteral("device-index"));
        QCOMPARE(mac.value, QStringLiteral("1"));
        QCOMPARE(mac.reason, QStringLiteral("identity"));
    }

    void aMissingOrRenumberedCameraIsReportedNotDefaulted()
    {
        using lightning::calls::PresentCamera;
        using lightning::calls::resolveCameraPreference;
        const QList<PresentCamera> present{
            {QStringLiteral("/dev/video0"), QStringLiteral("Integrated Camera")},
            {QStringLiteral("/dev/video2"), QStringLiteral("HD Pro Webcam C920")}};

        auto none = resolveCameraPreference(QString(), QString(), present);
        QVERIFY(none.id.isEmpty());
        QVERIFY(!none.preferredMissing);

        auto here = resolveCameraPreference(QStringLiteral("/dev/video2"),
                                            QStringLiteral("HD Pro Webcam C920"),
                                            present);
        QCOMPARE(here.id, QStringLiteral("/dev/video2"));
        QCOMPARE(here.description, QStringLiteral("HD Pro Webcam C920"));
        QVERIFY(!here.preferredMissing);
        QCOMPARE(resolveCameraPreference(QStringLiteral("/dev/video2"),
                                         QString(), present)
                     .id,
                 QStringLiteral("/dev/video2"));

        auto gone = resolveCameraPreference(QStringLiteral("/dev/video61"),
                                            QStringLiteral("cm-cam2"), present);
        QVERIFY(gone.id.isEmpty());
        QVERIFY2(gone.preferredMissing,
                 "an unplugged camera read as 'system default'");

        auto moved = resolveCameraPreference(QStringLiteral("/dev/video0"),
                                             QStringLiteral("cm-cam2"), present);
        QVERIFY(moved.id.isEmpty());
        QVERIFY(moved.preferredMissing);
    }

    void aStableIdIsNotRefusedForARenamedDescription()
    {
        using lightning::calls::PresentCamera;
        using lightning::calls::resolveCameraPreference;
        const QString windowsId =
            QStringLiteral("\\\\?\\usb#vid_0c45&pid_6a10");
        const QString macId = QStringLiteral("0x8020000005ac8514");
        const QList<PresentCamera> present{
            {windowsId, QStringLiteral("Integrated Webcam (2)")},
            {macId, QStringLiteral("FaceTime HD Camera (Built-in)")}};
        auto win = resolveCameraPreference(
            windowsId, QStringLiteral("Integrated Webcam"), present);
        QCOMPARE(win.id, windowsId);
        QVERIFY(!win.preferredMissing);
        auto mac = resolveCameraPreference(
            macId, QStringLiteral("FaceTime HD Camera"), present);
        QCOMPARE(mac.id, macId);
        QVERIFY(!mac.preferredMissing);
    }

    void windowsAudioProviders_data()
    {
        QTest::addColumn<bool>("speaker");
        QTest::addColumn<bool>("identity");
        QTest::addColumn<bool>("modernAvailable");
        QTest::addColumn<bool>("legacyListed");
        for (bool speaker : {false, true}) {
            for (bool identity : {false, true}) {
                for (bool modern : {false, true}) {
                    for (bool legacy : {false, true}) {
                        const QByteArray row = QByteArray::number(speaker) + "-"
                            + QByteArray::number(identity) + "-"
                            + QByteArray::number(modern) + "-"
                            + QByteArray::number(legacy);
                        QTest::newRow(row.constData())
                            << speaker << identity << modern << legacy;
                    }
                }
            }
        }
    }

    void windowsAudioProviders()
    {
        QFETCH(bool, speaker);
        QFETCH(bool, identity);
        QFETCH(bool, modernAvailable);
        QFETCH(bool, legacyListed);
        const QString name = QStringLiteral("USB Audio");
        const QString id = QStringLiteral("{0.0.1.00000000}.{A1B2}");
        QList<GstDeviceCandidate> devices{
            {name, {{QStringLiteral("device.api"), QStringLiteral("wasapi2")},
                    {QStringLiteral("device.id"), id},
                    {QStringLiteral("device.default"), QStringLiteral("false")},
                    {QStringLiteral("wasapi2.device.loopback"), QStringLiteral("false")}}}};
        if (legacyListed) {
            devices.append({name,
                {{QStringLiteral("device.api"), QStringLiteral("wasapi")},
                 {QStringLiteral("device.strid"), id}}});
        }
        const QString modern = speaker ? QStringLiteral("wasapi2sink")
                                       : QStringLiteral("wasapi2src");
        const QString legacy = speaker ? QStringLiteral("wasapisink")
                                       : QStringLiteral("wasapisrc");
        const auto choice = chooseCaptureElement(
            speaker ? CaptureKind::Speaker : CaptureKind::Microphone,
            {modern, legacy}, modernAvailable ? QStringList{modern, legacy}
                                             : QStringList{legacy},
            identity ? id.toLower() : QStringLiteral("qt-other-namespace"),
            name, devices);
        QCOMPARE(choice.element, modernAvailable ? modern : legacy);
        QCOMPARE(choice.binding.property, QStringLiteral("device"));
        QCOMPARE(choice.binding.value, id);
        QCOMPARE(choice.binding.reason, identity ? QStringLiteral("identity")
                                                : QStringLiteral("display-name"));
    }

    void aLegacyOnlyProviderCanBindEitherWindowsGeneration()
    {
        const QList<GstDeviceCandidate> devices{
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("device.api"), QStringLiteral("wasapi")},
              {QStringLiteral("device.strid"),
               QStringLiteral("{0.0.1.00000000}.{A1B2}")}}}};
        for (const QString &element : {QStringLiteral("wasapi2src"),
                                      QStringLiteral("wasapi2sink"),
                                      QStringLiteral("wasapisrc"),
                                      QStringLiteral("wasapisink")}) {
            const auto binding = resolveDeviceBinding(
                element.endsWith(QLatin1String("sink")) ? CaptureKind::Speaker
                                                      : CaptureKind::Microphone,
                element, QStringLiteral("qt-other-namespace"),
                QStringLiteral("USB Audio"), devices);
            QCOMPARE(binding.property, QStringLiteral("device"));
            QCOMPARE(binding.value, QStringLiteral("{0.0.1.00000000}.{A1B2}"));
        }
    }

    void aPulseIdDoesNotCollapseDifferentDevicesWithOneName()
    {
        const QList<GstDeviceCandidate> devices{
            pipewire(QStringLiteral("USB Audio"), QStringLiteral("1"),
                     QStringLiteral("node-one")),
            pipewire(QStringLiteral("USB Audio"), QStringLiteral("2"),
                     QStringLiteral("node-two"))};
        QVERIFY(resolveDeviceBinding(CaptureKind::Microphone,
            QStringLiteral("pulsesrc"), QStringLiteral("qt-other-namespace"),
            QStringLiteral("USB Audio"), devices).isEmpty());
    }

    void windowsAudioNamesRemainAmbiguousForDifferentEndpoints()
    {
        const QList<GstDeviceCandidate> devices{
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("device.api"), QStringLiteral("wasapi2")},
              {QStringLiteral("device.id"), QStringLiteral("endpoint-one")}}},
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("device.api"), QStringLiteral("wasapi")},
              {QStringLiteral("device.strid"), QStringLiteral("endpoint-two")}}}};
        for (const QString &element : {QStringLiteral("wasapi2src"),
                                      QStringLiteral("wasapisrc")}) {
            QVERIFY(resolveDeviceBinding(CaptureKind::Microphone, element,
                QStringLiteral("qt-other-namespace"), QStringLiteral("USB Audio"),
                devices).isEmpty());
            QCOMPARE(resolveDeviceBinding(CaptureKind::Microphone, element,
                QStringLiteral("endpoint-two"), QStringLiteral("USB Audio"),
                devices).value, QStringLiteral("endpoint-two"));
        }
    }

    void windowsMicrophoneNeverBindsLoopbackOrDefaultAlias()
    {
        const QString id = QStringLiteral("endpoint-one");
        const QList<GstDeviceCandidate> devices{
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("device.api"), QStringLiteral("wasapi2")},
              {QStringLiteral("device.id"), id},
              {QStringLiteral("wasapi2.device.loopback"), QStringLiteral("true")}}},
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("device.api"), QStringLiteral("wasapi2")},
              {QStringLiteral("device.id"), QStringLiteral("default-alias")},
              {QStringLiteral("device.actual-id"), id},
              {QStringLiteral("device.default"), QStringLiteral("true")}}}};
        for (const QString &element : {QStringLiteral("wasapi2src"),
                                      QStringLiteral("wasapisrc")}) {
            QVERIFY(resolveDeviceBinding(CaptureKind::Microphone, element, id,
                QStringLiteral("USB Audio"), devices).isEmpty());
        }
    }

    void macAudioUsesTheProvidersUniqueId()
    {
        const QList<GstDeviceCandidate> devices{
            {QStringLiteral("USB Audio"),
             {{QStringLiteral("unique-id"), QStringLiteral("CoreAudio-UID")},
              {QStringLiteral("is-default"), QStringLiteral("false")},
              {QStringLiteral("transport"), QStringLiteral("usb ")}}}};
        for (bool speaker : {false, true}) {
            const auto binding = resolveDeviceBinding(
                speaker ? CaptureKind::Speaker : CaptureKind::Microphone,
                speaker ? QStringLiteral("osxaudiosink") : QStringLiteral("osxaudiosrc"),
                QStringLiteral("CoreAudio-UID"), QStringLiteral("USB Audio"), devices);
            QCOMPARE(binding.property, QStringLiteral("unique-id"));
            QCOMPARE(binding.value, QStringLiteral("CoreAudio-UID"));
            QCOMPARE(binding.reason, QStringLiteral("identity"));
        }
    }

    void aConcreteElementIsChosenOnlyWhenItCanBindTheDevice()
    {
        const auto candidates = QList<GstDeviceCandidate>{
            pipewire(QStringLiteral("Blue Yeti"), QStringLiteral("77"),
                     QStringLiteral("alsa_input.usb-Blue_Yeti"))};
        const ElementChoice choice = chooseCaptureElement(
            CaptureKind::Microphone,
            {QStringLiteral("pipewiresrc"), QStringLiteral("pulsesrc")},
            {QStringLiteral("pipewiresrc"), QStringLiteral("pulsesrc")},
            QStringLiteral("some-qt-id"), QStringLiteral("Blue Yeti"),
            candidates);
        QCOMPARE(choice.element, QStringLiteral("pipewiresrc"));
        QCOMPARE(choice.binding.property, QStringLiteral("target-object"));
        QCOMPARE(choice.binding.value, QStringLiteral("77"));
    }

    void anUnavailableElementIsSkippedForTheNextCandidate()
    {
        const auto candidates = QList<GstDeviceCandidate>{GstDeviceCandidate{
            QStringLiteral("Blue Yeti"),
            {{QStringLiteral("object.serial"), QStringLiteral("77")},
             {QStringLiteral("device.name"),
              QStringLiteral("alsa_input.usb-Blue_Yeti")}}}};
        const ElementChoice choice = chooseCaptureElement(
            CaptureKind::Microphone,
            {QStringLiteral("pipewiresrc"), QStringLiteral("pulsesrc")},
            {QStringLiteral("pulsesrc")},
            QStringLiteral("alsa_input.usb-Blue_Yeti"),
            QStringLiteral("Blue Yeti"), candidates);
        QCOMPARE(choice.element, QStringLiteral("pulsesrc"));
        QCOMPARE(choice.binding.property, QStringLiteral("device"));
        QCOMPARE(choice.binding.value,
                 QStringLiteral("alsa_input.usb-Blue_Yeti"));
    }

    void anUnresolvableChoiceLeavesTheDefaultElementAlone()
    {
        QVERIFY(chooseCaptureElement(CaptureKind::Microphone,
                                     {QStringLiteral("pulsesrc")},
                                     {QStringLiteral("pulsesrc")},
                                     QStringLiteral("a-device-that-vanished"),
                                     QStringLiteral("Gone"),
                                     {GstDeviceCandidate{
                                         QStringLiteral("Something Else"),
                                         {{QStringLiteral("device.name"),
                                           QStringLiteral("other")}}}})
                    .isEmpty());
        QVERIFY(chooseCaptureElement(CaptureKind::Microphone,
                                     {QStringLiteral("pulsesrc")},
                                     {QStringLiteral("pulsesrc")}, QString(),
                                     QStringLiteral("Anything"), {})
                    .isEmpty());
    }
};

QTEST_MAIN(CaptureDeviceSelectionTest)
#include "CaptureDeviceSelectionTest.moc"
