#include "calls/CaptureDeviceSelection.h"

#include <QStringList>

namespace lightning::calls {
namespace {

// Per element: the property that selects a device and the device properties
// that identify one. Each uses the element's own documented spelling, which
// differs between elements, so callers must not guess.
struct ElementProfile {
    const char *element;
    const char *property;
    // Identity keys, most specific first. The value set is the one under
    // `valueKey`, not the key that matched.
    const char *valueKey;         // the key holding what `property` wants
    const char *identityKeys[4];  // keys whose value may equal the Qt id
    /// The value to set is the Qt id itself. True for the PulseAudio
    /// elements: pipewire-pulse names devices exactly as QMediaDevices does,
    /// while the PipeWire device provider's candidates carry no `device.name`,
    /// so a candidate-derived value would never bind.
    bool valueIsQtId;
};

constexpr ElementProfile kProfiles[] = {
    // Linux video. Qt's V4L2 camera id is the node path.
    {"v4l2src", "device", "device.path",
     {"device.path", "api.v4l2.path", "object.path", nullptr}, false},
    // Linux audio through PulseAudio or pipewire-pulse. `node.name` is what a
    // PipeWire desktop's monitor publishes, matching the Pulse device name.
    {"pulsesrc", "device", "device.name",
     {"device.name", "node.name", nullptr, nullptr}, true},
    {"pulsesink", "device", "device.name",
     {"device.name", "node.name", nullptr, nullptr}, true},
    // Linux through PipeWire directly. The handle is a numeric node id that no
    // Qt id matches, so the display name is the bridge.
    {"pipewiresrc", "target-object", "object.serial",
     {"object.serial", "object.id", "node.name", nullptr}, false},
    {"pipewiresink", "target-object", "object.serial",
     {"object.serial", "object.id", "node.name", nullptr}, false},
    // Windows.
    {"ksvideosrc", "device-path", "device.path",
     {"device.path", "device.strid", nullptr, nullptr}, false},
    {"wasapisrc", "device", "device.strid",
     {"device.strid", "device.id", nullptr, nullptr}, false},
    {"wasapisink", "device", "device.strid",
     {"device.strid", "device.id", nullptr, nullptr}, false},
    // macOS. `device-index` is platform-assigned and Qt's id is an
    // AVFoundation unique id, so the display name bridges them and the value
    // comes from the device's index property.
    {"avfvideosrc", "device-index", "device.index",
     {"device.unique-id", "device.index", nullptr, nullptr}, false},
    {"osxaudiosrc", "device", "device.id",
     {"device.uid", "device.id", nullptr, nullptr}, false},
    {"osxaudiosink", "device", "device.id",
     {"device.uid", "device.id", nullptr, nullptr}, false},
};

const ElementProfile *profileFor(const QString &element)
{
    for (const ElementProfile &profile : kProfiles) {
        if (element == QLatin1String(profile.element))
            return &profile;
    }
    return nullptr;
}

// Values go to g_object_set, not a parser, so quoting is not the hazard; a
// control character or absurd length means we matched garbage.
bool valueIsSane(const QString &value)
{
    if (value.isEmpty() || value.size() > 512)
        return false;
    for (const QChar c : value) {
        if (c.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

DeviceBinding bindingFrom(const ElementProfile &profile,
                          const GstDeviceCandidate &candidate,
                          const QString &reason,
                          const QString &qtDeviceId)
{
    // See ElementProfile::valueIsQtId.
    const QString value =
        profile.valueIsQtId
            ? qtDeviceId
            : candidate.properties.value(QString::fromLatin1(profile.valueKey));
    if (!valueIsSane(value))
        return {};
    // `audio.channels` is what the device captures, per the monitor; 0 when
    // unpublished, meaning "mix normally".
    bool ok = false;
    const int channels =
        candidate.properties.value(QStringLiteral("audio.channels")).toInt(&ok);
    return DeviceBinding{QString::fromLatin1(profile.property), value, reason,
                         (ok && channels > 0 && channels <= 64) ? channels : 0};
}

} // namespace

QString captureMixMatrix(int channels)
{
    // Two channels or fewer is a microphone; leave the downmix alone.
    if (channels <= 2 || channels > 64)
        return QString();
    QStringList row;
    row.reserve(channels);
    // Unity on the first input, zero on the rest. Explicit `(float)` entries:
    // gst_parse infers int from a bare 1.
    row << QStringLiteral("(float)1.0");
    for (int i = 1; i < channels; ++i)
        row << QStringLiteral("(float)0.0");
    return QStringLiteral("mix-matrix=\"<<%1>>\"")
        .arg(row.join(QStringLiteral(",")));
}

QString captureChannelCaps(int channels)
{
    if (channels <= 2 || channels > 64)
        return QString();
    // channel-mask=0 declares the channels unpositioned; audioconvert refuses
    // more than two channels it cannot position.
    return QStringLiteral(
               "! audio/x-raw,channels=%1,channel-mask=(bitmask)0x0 ")
        .arg(channels);
}

QStringList identityKeysForElement(const QString &element)
{
    QStringList keys;
    if (const ElementProfile *profile = profileFor(element)) {
        for (const char *key : profile->identityKeys) {
            if (key)
                keys << QString::fromLatin1(key);
        }
    }
    return keys;
}

QString devicePropertyForElement(const QString &element)
{
    const ElementProfile *profile = profileFor(element);
    return profile ? QString::fromLatin1(profile->property) : QString();
}

DeviceBinding resolveDeviceBinding(CaptureKind kind, const QString &element,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates)
{
    Q_UNUSED(kind);
    // "System default" follows the platform; never pin it.
    if (qtDeviceId.isEmpty())
        return {};
    const ElementProfile *profile = profileFor(element);
    if (!profile)
        return {};

    // 1. Identity: an exact match on a key both subsystems publish.
    for (const GstDeviceCandidate &candidate : candidates) {
        for (const char *key : profile->identityKeys) {
            if (!key)
                continue;
            if (candidate.properties.value(QString::fromLatin1(key))
                != qtDeviceId) {
                continue;
            }
            const DeviceBinding binding =
                bindingFrom(*profile, candidate, QStringLiteral("identity"),
                            qtDeviceId);
            if (!binding.isEmpty())
                return binding;
        }
    }

    // 2. Display name, where a Pulse-shaped Qt id meets a PipeWire node.
    //    Ambiguity is refused: opening the wrong device is worse than the
    //    default.
    if (!qtDescription.isEmpty()) {
        const QString wanted = qtDescription.trimmed();
        const GstDeviceCandidate *match = nullptr;
        for (const GstDeviceCandidate &candidate : candidates) {
            if (candidate.displayName.trimmed() != wanted)
                continue;
            if (match)
                return {}; // ambiguous
            match = &candidate;
        }
        if (match) {
            const DeviceBinding binding =
                bindingFrom(*profile, *match, QStringLiteral("display-name"),
                            qtDeviceId);
            if (!binding.isEmpty())
                return binding;
        }
    }

    // 3. Shape, only when the monitor returned no candidates at all. With
    //    candidates, "not found" means the device is gone.
    if (!candidates.isEmpty())
        return {};
    if (element == QLatin1String("v4l2src")
        && qtDeviceId.startsWith(QLatin1String("/dev/video"))
        && valueIsSane(qtDeviceId)) {
        return DeviceBinding{QStringLiteral("device"), qtDeviceId,
                             QStringLiteral("shape")};
    }
    return {};
}

ElementChoice chooseCaptureElement(CaptureKind kind,
                                   const QStringList &preferenceOrder,
                                   const QStringList &availableElements,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates)
{
    if (qtDeviceId.isEmpty())
        return {};
    for (const QString &element : preferenceOrder) {
        // A missing element cannot be named in a description: the parse would
        // fail and the microphone would be lost entirely.
        if (!availableElements.contains(element))
            continue;
        const DeviceBinding binding = resolveDeviceBinding(
            kind, element, qtDeviceId, qtDescription, candidates);
        if (!binding.isEmpty())
            return ElementChoice{element, binding};
    }
    return {};
}

} // namespace lightning::calls
