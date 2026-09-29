#include "calls/CaptureDeviceSelection.h"

#include <QtTest/QtTest>

using lightning::calls::CaptureKind;
using lightning::calls::DeviceBinding;
using lightning::calls::GstDeviceCandidate;
using lightning::calls::chooseCaptureElement;
using lightning::calls::ElementChoice;
using lightning::calls::identityKeysForElement;
using lightning::calls::resolveDeviceBinding;

// Resolving the user's device choice into a property the capture element
// obeys. The candidate shapes are real: GStreamer may enumerate a device
// through PipeWire (`pipewiresrc target-object=68`) while Qt reports a
// PulseAudio-style name for the same device.
namespace {

GstDeviceCandidate v4l2(const QString &name, const QString &path)
{
    return GstDeviceCandidate{name, {{QStringLiteral("device.path"), path},
                                     {QStringLiteral("api.v4l2.path"), path}}};
}

// A camera as the PipeWire device provider lists it (it outranks the v4l2
// provider on a PipeWire desktop): the node path only under
// `api.v4l2.path`, no `device.path`, and " (V4L2)" on the name. Keys and
// values as `gst-device-monitor-1.0 Video/Source` printed them for
// call-matrix's v4l2loopback camera.
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

} // namespace

class CaptureDeviceSelectionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A multi-input interface is not a microphone: asking for `channels=1`
    // averages the live input with the silent ones (-12.04 dB for four
    // inputs). Take input 1 instead.
    void aFourInputInterfaceTakesItsFirstInputNotTheMeanOfFour()
    {
        using namespace lightning::calls;
        QCOMPARE(captureMixMatrix(4),
                 QStringLiteral("mix-matrix=\"<<(float)1.0,(float)0.0,"
                                "(float)0.0,(float)0.0>>\""));
        // Both halves are needed: the matrix alone lets the source negotiate
        // mono and average first, and without the explicit unpositioned mask
        // audioconvert refuses the graph with `not-negotiated`.
        QCOMPARE(captureChannelCaps(4),
                 QStringLiteral("! audio/x-raw,channels=4,"
                                "channel-mask=(bitmask)0x0 "));
    }

    // Mono and stereo keep their existing downmix: a stereo mic with signal
    // in one channel is better averaged (6 dB quieter) than absent from one
    // ear.
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

    // The channel count is device-reported data, bounded like every other
    // value handed to g_object_set.
    void anAbsurdChannelCountIsRefusedRatherThanBuiltInto()
    {
        using namespace lightning::calls;
        QVERIFY(!captureMixMatrix(8).isEmpty());
        QVERIFY(!captureMixMatrix(64).isEmpty());
        QVERIFY(captureMixMatrix(65).isEmpty());
        QVERIFY(captureMixMatrix(1000000).isEmpty());
        QVERIFY(captureMixMatrix(-4).isEmpty());
    }

    // The count survives resolution: it is read from the monitor's
    // `audio.channels`.
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

        // A device that does not publish the key means "mix normally", never
        // "assume one channel".
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

    // A pulse element binds on a PipeWire desktop: pipewiredeviceprovider's
    // candidates carry `node.name` and `object.serial` but no `device.name`,
    // so the pulse identity keys must include `node.name`.
    void aPulseElementBindsFromAPipeWireEnumeratedDevice()
    {
        GstDeviceCandidate mic;
        mic.displayName = QStringLiteral("Built-in Audio Analog Stereo");
        // What a PipeWire desktop's monitor publishes: no device.name.
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
        // The value is the Qt id: pipewire-pulse names devices as
        // QMediaDevices reports them.
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

    // The ordinary Linux camera: Qt's V4L2 id is the node path, matching the
    // monitor's `device.path`.
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

    // Qt reports "alsa_output.pci-0000_00_1f.3.analog-stereo" while GStreamer
    // knows the same device only as PipeWire node 68; no identity key
    // matches, so the display name bridges them.
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

    // Identity outranks the name: two devices may share a driver string.
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

    // Ambiguity binds nothing: with two identical names and no identity
    // match, either could be the wrong camera. For a camera that means no
    // camera at all (see aChosenCameraThatCannotBeBoundIsRefused...).
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

    // A device the monitor no longer lists binds nothing, never a stale path.
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

    // Only with no monitor at all is the id's shape trusted, and only for the
    // element whose property takes exactly that shape.
    void aDeviceNodePathIsTrustedOnlyWhenTheMonitorCannotAnswer()
    {
        const DeviceBinding binding = resolveDeviceBinding(
            CaptureKind::Camera, QStringLiteral("v4l2src"),
            QStringLiteral("/dev/video3"), QStringLiteral("Some Camera"), {});
        QCOMPARE(binding.property, QStringLiteral("device"));
        QCOMPARE(binding.value, QStringLiteral("/dev/video3"));
        QCOMPARE(binding.reason, QStringLiteral("shape"));

        // Not a device node, so not trusted.
        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("v4l2src"),
                                     QStringLiteral("camera:0"),
                                     QStringLiteral("Some Camera"), {})
                    .isEmpty());
        // An audio name is never trusted by shape: a wrong `device=` loses the
        // microphone, the default loses nothing.
        QVERIFY(resolveDeviceBinding(CaptureKind::Microphone,
                                     QStringLiteral("pulsesrc"),
                                     QStringLiteral("alsa_input.usb-Blue"),
                                     QStringLiteral("Blue Yeti"), {})
                    .isEmpty());
    }

    // An unknown element gets nothing: `autoaudiosrc` has no device property,
    // and inventing one aborts the pipeline build.
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

    // A matched value with control characters is refused.
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

    // Windows and macOS profiles use their own properties (`ksvideosrc`
    // `device-path`, `avfvideosrc` an index); a bare `device` would be ignored
    // by both. Pins the mapping only; neither platform runs here.
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

    // DEFECT 5, reported live on a PipeWire host: the choice was a
    // v4l2loopback camera, the monitor listed it the PipeWire way, nothing
    // bound, and v4l2src opened /dev/video0 — the laptop's own webcam.
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

        // An element this table does not know cannot bind a camera either.
        QVERIFY(resolveDeviceBinding(CaptureKind::Camera,
                                     QStringLiteral("autovideosrc"),
                                     QStringLiteral("/dev/video61"),
                                     QStringLiteral("cm-cam2"),
                                     onlyTheLaptopCamera)
                    .refused);

        // "System default" is not a choice, so nothing is refused.
        QVERIFY(!resolveDeviceBinding(CaptureKind::Camera,
                                      QStringLiteral("v4l2src"), QString(),
                                      QString(), onlyTheLaptopCamera)
                     .refused);

        // A microphone keeps the platform default rather than going silent.
        const DeviceBinding mic = resolveDeviceBinding(
            CaptureKind::Microphone, QStringLiteral("pulsesrc"),
            QStringLiteral("a-device-that-vanished"), QStringLiteral("Gone"),
            {pipewire(QStringLiteral("Other"), QStringLiteral("5"),
                      QStringLiteral("other"))});
        QVERIFY(mic.isEmpty());
        QVERIFY(!mic.refused);
    }

    // gst_value_serialize() quotes and backslash-escapes a string with
    // characters outside [A-Za-z0-9_+-/:.] (measured on 1.26.11): a WASAPI
    // id and a Windows device path both come back that way, and would match
    // no Qt id or be set as a garbage `device=`.
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

        // A Windows device path, spelt in another case by Qt: the ks
        // provider compares these without case, and so does this.
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

    // The avf provider publishes the camera's unique id as `avf.unique_id`
    // (gst-plugins-bad avfdeviceprovider.m); the index is copied from the
    // device's `device-index` property by the enumeration.
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

    // Review M1: an UNPLUGGED chosen camera used to resolve to "" (system
    // default) before the engine saw it, so the refusal never ran and the
    // laptop's own camera opened. The preference must reach the engine as
    // "chosen and missing". Review S7: /dev/videoN renumbers on replug, so
    // the same id with another description is another camera.
    void aMissingOrRenumberedCameraIsReportedNotDefaulted()
    {
        using lightning::calls::PresentCamera;
        using lightning::calls::resolveCameraPreference;
        const QList<PresentCamera> present{
            {QStringLiteral("/dev/video0"), QStringLiteral("Integrated Camera")},
            {QStringLiteral("/dev/video2"), QStringLiteral("HD Pro Webcam C920")}};

        // No preference: the system default, nothing missing.
        auto none = resolveCameraPreference(QString(), QString(), present);
        QVERIFY(none.id.isEmpty());
        QVERIFY(!none.preferredMissing);

        // Present, and the description matches (or was never recorded).
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

        // Unplugged.
        auto gone = resolveCameraPreference(QStringLiteral("/dev/video61"),
                                            QStringLiteral("cm-cam2"), present);
        QVERIFY(gone.id.isEmpty());
        QVERIFY2(gone.preferredMissing,
                 "an unplugged camera read as 'system default'");

        // Replugged into another node: /dev/video0 is now a different camera.
        auto moved = resolveCameraPreference(QStringLiteral("/dev/video0"),
                                             QStringLiteral("cm-cam2"), present);
        QVERIFY(moved.id.isEmpty());
        QVERIFY(moved.preferredMissing);
    }

    // Final review S5: a Windows or macOS id names one device for good, so a
    // renamed description (a driver or Qt update) must not refuse it.
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

    // Honouring a microphone choice means picking a concrete element instead
    // of `autoaudiosrc`, but only one that can bind the device.
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

    // An element this build lacks is never named: the description would fail
    // to parse and lose the microphone entirely.
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
            {QStringLiteral("pulsesrc")}, // pipewiresrc absent from this build
            QStringLiteral("alsa_input.usb-Blue_Yeti"),
            QStringLiteral("Blue Yeti"), candidates);
        QCOMPARE(choice.element, QStringLiteral("pulsesrc"));
        QCOMPARE(choice.binding.property, QStringLiteral("device"));
        QCOMPARE(choice.binding.value,
                 QStringLiteral("alsa_input.usb-Blue_Yeti"));
    }

    // If nothing binds, the caller keeps its default element.
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
        // "System default" never picks a concrete element.
        QVERIFY(chooseCaptureElement(CaptureKind::Microphone,
                                     {QStringLiteral("pulsesrc")},
                                     {QStringLiteral("pulsesrc")}, QString(),
                                     QStringLiteral("Anything"), {})
                    .isEmpty());
    }
};

QTEST_MAIN(CaptureDeviceSelectionTest)
#include "CaptureDeviceSelectionTest.moc"
