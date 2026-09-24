#pragma once

#include <QList>
#include <QMap>
#include <QString>

// Turning "the user picked this device" into something a GStreamer element
// will obey.
//
// Settings stores the Qt (QMediaDevices) device id, but Qt and GStreamer
// enumerate through different subsystems and their ids are not one
// namespace: Qt's V4L2 id is the node path `v4l2src device=` wants, while
// Qt's audio id is a PulseAudio name and GStreamer may see the same device via
// PipeWire as a numeric node id. A wrong `device=` makes the element fail to
// open, which is worse than using the default, so resolution is
// evidence-based:
//
//   1. Identity: match the Qt id against the identifying properties each
//      candidate from GStreamer's device monitor publishes (`device.path`,
//      `device.name`, `object.serial`, ...).
//   2. Display name: match Qt's description against the candidate's display
//      name (both come from the same driver string).
//   3. Shape, only when the monitor offered nothing: a `/dev/video*` id for
//      `v4l2src` is taken at face value. Otherwise no binding.
//
// Ambiguity (two candidates with one display name) yields no binding, so the
// platform default is kept.
//
// The result is a property name and value to g_object_set on the parsed
// element, so this file needs no GStreamer headers and is tested directly.
namespace lightning::calls {

enum class CaptureKind {
    Camera,
    Microphone,
    Speaker,
};

/// One device as GStreamer's monitor describes it.
struct GstDeviceCandidate {
    /// `gst_device_get_display_name()`.
    QString displayName;
    /// The device's property structure, flattened; only identifying keys
    /// matter.
    QMap<QString, QString> properties;
};

/// What to set on the capture element once the bin is parsed.
struct DeviceBinding {
    /// The element property to set; empty when nothing should be set.
    QString property;
    QString value;
    /// How it was resolved ("identity", "display-name" or "shape"); logged.
    QString reason;
    /// Channels the device captures (the monitor's `audio.channels`), or 0.
    /// See captureMixMatrix().
    int channels = 0;

    bool isEmpty() const { return property.isEmpty(); }
};

/// The `audioconvert` mix-matrix that takes a multi-input device's first
/// input, or empty when the device should be mixed normally.
///
/// A four-channel interface with a microphone on input 1 and nothing on the
/// others, downmixed to mono, averages in three silent inputs and loses
/// 12 dB (20*log10(1/4)).
///
/// Stereo is untouched: a two-channel microphone is averaged on purpose (a
/// Windows mic often has signal in only one channel). Three or more channels
/// is an interface, not a microphone.
///
/// Limit: input 1 is a convention. A microphone on another input yields
/// silence, which the capture level meter then reports.
QString captureMixMatrix(int channels);

/// The caps a multi-input device must be pinned to for that matrix to
/// negotiate: its own channel count, explicitly unpositioned. Without the
/// count the chain negotiates mono at the source (PipeWire averages before we
/// see a sample); without `channel-mask=0` audioconvert fails with
/// `not-negotiated`.
QString captureChannelCaps(int channels);

/// The property binding for `element`, or an empty binding ("let the platform
/// choose").
///
/// `qtDeviceId` and `qtDescription` come from QMediaDevices; an empty id
/// ("system default") always yields an empty binding. `candidates` is what
/// GStreamer's device monitor reported for the device class; an empty list
/// means the monitor was unavailable, enabling the shape rule. Unknown
/// elements yield an empty binding.
DeviceBinding resolveDeviceBinding(CaptureKind kind, const QString &element,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

/// The element to instantiate and the property to set on it.
struct ElementChoice {
    /// Empty means "keep the caller's default element and set nothing".
    QString element;
    DeviceBinding binding;

    bool isEmpty() const { return element.isEmpty(); }
};

/// The first element in `preferenceOrder` that is both available in this
/// build and able to bind the chosen device. `autoaudiosrc` has no device
/// property, so honouring a preference means choosing a concrete element,
/// but never one that cannot bind the device. Empty when nothing binds.
ElementChoice chooseCaptureElement(CaptureKind kind,
                                   const QStringList &preferenceOrder,
                                   const QStringList &availableElements,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

/// The identifying property keys for `element`, most specific first.
QStringList identityKeysForElement(const QString &element);

/// The element property that selects a device, or empty for an unknown
/// element.
QString devicePropertyForElement(const QString &element);

} // namespace lightning::calls
