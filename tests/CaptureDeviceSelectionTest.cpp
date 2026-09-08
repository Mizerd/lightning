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
