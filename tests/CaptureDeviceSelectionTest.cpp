#include "calls/CaptureDeviceSelection.h"

#include <QtTest/QtTest>

using lightning::calls::CaptureKind;
using lightning::calls::DeviceBinding;
using lightning::calls::GstDeviceCandidate;
using lightning::calls::chooseCaptureElement;
using lightning::calls::ElementChoice;
using lightning::calls::identityKeysForElement;
using lightning::calls::resolveDeviceBinding;

// Resolving the user's device choice into something an element obeys.
//
// Every case here fails on the code that shipped in 0.9.3, where the capture
// element was built from a bare name with no device property and the whole
// picker was decoration.
//
// The candidate shapes are the real ones. The audio device on the maintainer's
// machine is enumerated by GStreamer through PipeWire
// (`gst-device-monitor-1.0 Audio/Source` answers
// `pipewiresrc target-object=68` for "Built-in Audio Analog Stereo"), while Qt
// reports a PulseAudio-style name for the same device: that mismatch is the
// entire reason this file exists.
namespace {

GstDeviceCandidate v4l2(const QString &name, const QString &path)
{
    return GstDeviceCandidate{name, {{QStringLiteral("device.path"), path},
                                     {QStringLiteral("api.v4l2.path"), path}}};
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
    // SYSTEM DEFAULT STAYS SYSTEM DEFAULT. An empty preference must never be
    // pinned to whatever is default today, or unplugging a headset would
    // strand the user on a device they never chose.
    // A MULTI-INPUT INTERFACE IS NOT A MICROPHONE.
    //
    // Measured on a Roland Rubix44, 2026-09-16, while the maintainer spoke:
    // input 1 peaked -21 dBFS and inputs 2-4 sat at -86, -67 and -92, while
    // the chain asking for `channels=1` produced -33 dBFS. That is
    // 20*log10(1/4) = -12.04 dB, the mean of one voice and three silences,
    // sent to Element as a perfectly encrypted whisper.
    //
    // FAIL-ON-OLD: return QString() unconditionally from captureMixMatrix()
    // and the three matrix cases fail; from captureChannelCaps() and the
    // caps case fails. Both measured.
    void aFourInputInterfaceTakesItsFirstInputNotTheMeanOfFour()
    {
        using namespace lightning::calls;
        QCOMPARE(captureMixMatrix(4),
                 QStringLiteral("mix-matrix=\"<<(float)1.0,(float)0.0,"
                                "(float)0.0,(float)0.0>>\""));
        // Both halves are needed: the matrix alone leaves the source free to
        // negotiate mono and average before we see a sample, and without the
        // explicit UNPOSITIONED mask audioconvert refuses the graph outright
        // with `not-negotiated`.
        QCOMPARE(captureChannelCaps(4),
                 QStringLiteral("! audio/x-raw,channels=4,"
                                "channel-mask=(bitmask)0x0 "));
    }

    // STEREO IS A MICROPHONE AND MUST KEEP THE DOWNMIX IT HAS. The chain's
    // own comment records a Windows mic exposed as stereo with signal in one
    // channel only, where averaging costs 6 dB and being quiet in both ears
    // beats being absent from one ear. Changing that is a regression, not a
    // fix, so the boundary is asserted rather than described.
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

    // A channel count is device-reported data, so it is bounded like every
    // other value this file hands to g_object_set.
    void anAbsurdChannelCountIsRefusedRatherThanBuiltInto()
    {
        using namespace lightning::calls;
        QVERIFY(!captureMixMatrix(8).isEmpty());
        QVERIFY(!captureMixMatrix(64).isEmpty());
        QVERIFY(captureMixMatrix(65).isEmpty());
        QVERIFY(captureMixMatrix(1000000).isEmpty());
        QVERIFY(captureMixMatrix(-4).isEmpty());
    }

    // And the count has to SURVIVE resolution, or none of the above is ever
    // reached: it is read off the monitor's own `audio.channels`.
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

        // A device that does not publish the key says nothing, and nothing
        // means "mix it the ordinary way" — never "assume one channel".
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

    // A PULSE ELEMENT MUST BIND ON A PIPEWIRE DESKTOP, and it could not.
    //
    // The device monitor there enumerates through `pipewiredeviceprovider`,
    // whose candidates carry `node.name` and `object.serial` and NO
    // `device.name` — the only key the pulse entries matched on, and the only
    // key they read their value from. So `pulsesrc` never resolved, every
    // Linux capture fell through to `pipewiresrc`, and reordering the
    // preference to put pulsesrc first (which it now is, for the AppImage
    // target-object defect) changed NOTHING until this was fixed too.
    //
    // FAIL-ON-OLD: drop "node.name" from the pulse identity keys, or set
    // valueIsQtId false for them, and this fails.
    void aPulseElementBindsFromAPipeWireEnumeratedDevice()
    {
        GstDeviceCandidate mic;
        mic.displayName = QStringLiteral("Built-in Audio Analog Stereo");
        // EXACTLY what a PipeWire desktop's monitor publishes: no device.name.
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
        // The VALUE is the Qt id: pipewire-pulse names devices exactly as
        // QMediaDevices reports them, and the candidate never published it.
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

    // THE ORDINARY LINUX CAMERA CASE. Qt's V4L2 id is the node path, so the
    // monitor's own `device.path` matches it exactly.
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

    // THE CASE THAT MOTIVATED THE DISPLAY-NAME RULE. Qt says
    // "alsa_output.pci-0000_00_1f.3.analog-stereo"; GStreamer enumerated the
    // same device through PipeWire and knows it only as node 68. No identity
    // key can match, and the name is the bridge.
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

    // IDENTITY OUTRANKS THE NAME, because two devices may legitimately share
    // a driver string while only one is the device the user picked.
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

    // AMBIGUITY IS REFUSED. Two identical names and no identity match means
    // we cannot tell which one was chosen; opening the wrong camera is worse
    // than opening the default one.
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
                 "devices instead of falling back to the platform default");
    }

    // THE DEVICE WENT AWAY. The monitor answered and this device is not in
    // it, so the honest outcome is the default -- not a stale path handed to
    // a driver that will fail to open it.
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

    // NO MONITOR AT ALL is the only state in which the id's shape is trusted,
    // and only for the one element whose property takes exactly that shape.
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
        // An audio name is never trusted by shape: a wrong `device=` costs
        // the user their microphone, where the default costs them nothing.
        QVERIFY(resolveDeviceBinding(CaptureKind::Microphone,
                                     QStringLiteral("pulsesrc"),
                                     QStringLiteral("alsa_input.usb-Blue"),
                                     QStringLiteral("Blue Yeti"), {})
                    .isEmpty());
    }

    // AN ELEMENT THIS FILE DOES NOT KNOW GETS NOTHING. `autoaudiosrc` is a
    // bin with no device property at all: inventing one would abort the
    // pipeline build.
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

    // GARBAGE IS NOT PASSED ON. A control character in a matched value means
    // we matched something we do not understand; a driver should never see it.
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

    // THE WINDOWS AND macOS PROFILES EXIST AND SPELL THEIR OWN PROPERTY.
    // Neither platform can be exercised from here, so this pins the mapping
    // rather than claiming it was run: `ksvideosrc` takes `device-path`,
    // `avfvideosrc` takes an INDEX, and a bare `device` would be silently
    // ignored by both.
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

    // THE ENGINE'S ACTUAL DECISION. `autoaudiosrc` has no device property, so
    // honouring a microphone choice means picking a concrete element -- and
    // only one that can really bind the device, or a working default would be
    // swapped for a broken pin.
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

    // AN ELEMENT THIS BUILD DOES NOT CARRY IS NEVER NAMED. A description
    // naming a missing element fails to PARSE, which loses the microphone
    // entirely rather than leaving it on the default device.
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

    // NOTHING BINDS -> NOTHING CHANGES. The caller keeps the element it has
    // always used, which is the behaviour that shipped.
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
        // And "system default" never picks a concrete element at all.
        QVERIFY(chooseCaptureElement(CaptureKind::Microphone,
                                     {QStringLiteral("pulsesrc")},
                                     {QStringLiteral("pulsesrc")}, QString(),
                                     QStringLiteral("Anything"), {})
                    .isEmpty());
    }
};

QTEST_MAIN(CaptureDeviceSelectionTest)
#include "CaptureDeviceSelectionTest.moc"
