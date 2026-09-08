#pragma once

#include <QList>
#include <QMap>
#include <QString>

// Turning "the user picked this camera" into something a GStreamer element
// will actually obey.
//
// THE DEFECT THIS EXISTS FOR. Settings enumerates devices through Qt
// (QMediaDevices) and persists the chosen QCameraDevice/QAudioDevice `id`.
// The call pipeline builds its capture from a bare element name --
// `v4l2src`, `ksvideosrc`, `avfvideosrc`, `autoaudiosrc` -- with no device
// property at all, so every one of those choices was decoration: the element
// opened whatever the platform considered default. The picker showed the
// chosen device as "chosen" and the call used another one.
//
// WHY IT IS NOT A ONE-LINE FIX. Qt and GStreamer enumerate devices through
// DIFFERENT subsystems, so their identifiers are not the same namespace and
// must not be assumed interchangeable. Qt's V4L2 id happens to be the device
// node path, which `v4l2src device=` wants; Qt's audio id is a PulseAudio
// device name, while GStreamer on the same machine may enumerate the very
// same device through PipeWire, where the handle is a numeric node id that
// `pulsesrc` cannot use. Guessing wrong is worse than doing nothing: a bad
// `device=` makes the element fail to open, which costs the user their
// camera or microphone entirely, where ignoring the preference merely gives
// them the default one.
//
// SO THE RESOLUTION IS EVIDENCE-BASED, in this order:
//
//   1. IDENTITY. Ask GStreamer's own device monitor what it can see, and
//      match the Qt id against the identifying properties each candidate
//      publishes (`device.path`, `device.name`, `object.serial` ...). An
//      exact match is proof that both subsystems mean the same device.
//   2. DISPLAY NAME. Failing that, match Qt's description against the
//      candidate's display name. Both come from the same underlying driver
//      string, so this is reliable in practice and is how a PipeWire audio
//      node gets matched to a Pulse-shaped Qt id.
//   3. SHAPE, and only when the monitor offered nothing at all. `v4l2src`
//      with an id that is a `/dev/video*` path is safe to take at face
//      value. Anything else yields NO binding.
//
// AMBIGUITY IS REFUSED, never guessed: two candidates sharing a display name
// produce no binding, so the pipeline keeps the platform default rather than
// opening a device the user did not choose.
//
// The result is one property name and one value to `g_object_set` on the
// already-parsed element, which is why this file needs no GStreamer headers
// and is tested by calling it.
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
    /// The device's own property structure, flattened. Only the identifying
    /// keys matter here; extra entries are ignored.
    QMap<QString, QString> properties;
};

/// What to set on the capture element once the bin is parsed.
struct DeviceBinding {
    /// The element property to set, empty when nothing should be set.
    QString property;
    QString value;
    /// How it was resolved -- "identity", "display-name" or "shape". Logged,
    /// so a support report says WHY a device was chosen.
    QString reason;

    bool isEmpty() const { return property.isEmpty(); }
};

/// The property binding for `element`, or an empty binding meaning "leave the
/// element alone and let the platform choose".
///
/// `qtDeviceId` and `qtDescription` come from QMediaDevices (empty id means
/// the user chose "system default", which always yields an empty binding).
/// `candidates` is what GStreamer's device monitor reported for the matching
/// device class; an empty list means the monitor was unavailable, which
/// enables the shape rule above.
///
/// An unknown element yields an empty binding: this file never invents a
/// property name for an element it does not know.
DeviceBinding resolveDeviceBinding(CaptureKind kind, const QString &element,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

/// The element to instantiate and the property to set on it.
struct ElementChoice {
    /// Empty means "keep the caller's own default element and set nothing",
    /// which is exactly the behaviour that shipped before device selection
    /// existed.
    QString element;
    DeviceBinding binding;

    bool isEmpty() const { return element.isEmpty(); }
};

/// The first element in `preferenceOrder` that is BOTH available in this
/// build and able to bind the chosen device.
///
/// This is the whole decision the engine makes, kept here so it can be tested
/// by calling it: `autoaudiosrc` is a bin with no device property, so
/// honouring a preference means choosing a CONCRETE element instead -- and
/// choosing one that cannot bind the device would swap a working default for
/// a broken pin. When nothing binds, the result is empty and the caller keeps
/// what it always used.
ElementChoice chooseCaptureElement(CaptureKind kind,
                                   const QStringList &preferenceOrder,
                                   const QStringList &availableElements,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

/// The identifying property keys consulted for `element`, most specific
/// first. Exposed for tests and for the diagnostics report.
QStringList identityKeysForElement(const QString &element);

/// The element property that selects a device for `element`, empty when this
/// file does not know the element.
QString devicePropertyForElement(const QString &element);

} // namespace lightning::calls
