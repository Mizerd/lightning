#include "calls/CaptureDeviceSelection.h"

#include <QStringList>

namespace lightning::calls {
namespace {

// The property each capture element uses to select a device, and the device
// properties that identify one. Every entry is the element's own documented
// spelling: they disagree with each other on purpose (`device` vs
// `device-path` vs `target-object`), which is precisely why a caller must not
// guess.
struct ElementProfile {
    const char *element;
    const char *property;
    // Identity keys, MOST SPECIFIC FIRST. A device may publish several; the
    // first that matches wins, and the value that is set is the value of the
    // key the property expects, not the key that matched.
    const char *valueKey;         // the key holding what `property` wants
    const char *identityKeys[4];  // keys whose value may equal the Qt id
};

constexpr ElementProfile kProfiles[] = {
    // Linux video. Qt's V4L2 camera id IS the node path, so identity usually
    // matches on the first key.
    {"v4l2src", "device", "device.path",
     {"device.path", "api.v4l2.path", "object.path", nullptr}},
    // Linux audio through PulseAudio (or pipewire-pulse).
    {"pulsesrc", "device", "device.name", {"device.name", nullptr, nullptr, nullptr}},
    {"pulsesink", "device", "device.name", {"device.name", nullptr, nullptr, nullptr}},
    // Linux audio and video through PipeWire directly. The handle is a
    // numeric node id that only this element understands, so a Qt id can
    // never match it by identity -- the display name is the bridge.
    {"pipewiresrc", "target-object", "object.serial",
     {"object.serial", "object.id", "node.name", nullptr}},
    {"pipewiresink", "target-object", "object.serial",
     {"object.serial", "object.id", "node.name", nullptr}},
    // Windows.
    {"ksvideosrc", "device-path", "device.path",
     {"device.path", "device.strid", nullptr, nullptr}},
    {"wasapisrc", "device", "device.strid",
     {"device.strid", "device.id", nullptr, nullptr}},
    {"wasapisink", "device", "device.strid",
     {"device.strid", "device.id", nullptr, nullptr}},
    // macOS. `device-index` is an integer the platform assigns, and Qt's id
    // is an AVFoundation unique id string, so identity cannot match: the
    // display name is the only bridge, and the value comes from the device's
    // own index property.
    {"avfvideosrc", "device-index", "device.index",
     {"device.unique-id", "device.index", nullptr, nullptr}},
    {"osxaudiosrc", "device", "device.id",
     {"device.uid", "device.id", nullptr, nullptr}},
    {"osxaudiosink", "device", "device.id",
     {"device.uid", "device.id", nullptr, nullptr}},
};

const ElementProfile *profileFor(const QString &element)
{
    for (const ElementProfile &profile : kProfiles) {
        if (element == QLatin1String(profile.element))
            return &profile;
    }
    return nullptr;
}

// A value that reaches g_object_set is not parsed by anything, so quoting is
// not the hazard -- a control character or an absurd length is, because it
// means we matched garbage and are about to hand it to a driver.
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
                          const QString &reason)
{
    const QString value =
        candidate.properties.value(QString::fromLatin1(profile.valueKey));
    if (!valueIsSane(value))
        return {};
    return DeviceBinding{QString::fromLatin1(profile.property), value, reason};
}

} // namespace

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
    // "System default" is a real choice and it is the one that follows the
    // platform as it changes. Never pin it to today's answer.
    if (qtDeviceId.isEmpty())
        return {};
    const ElementProfile *profile = profileFor(element);
    if (!profile)
        return {};

    // 1. IDENTITY. An exact match on a key both subsystems publish is proof
    //    they mean the same device.
    for (const GstDeviceCandidate &candidate : candidates) {
        for (const char *key : profile->identityKeys) {
            if (!key)
                continue;
            if (candidate.properties.value(QString::fromLatin1(key))
                != qtDeviceId) {
                continue;
            }
            const DeviceBinding binding =
                bindingFrom(*profile, candidate, QStringLiteral("identity"));
            if (!binding.isEmpty())
                return binding;
        }
    }

    // 2. DISPLAY NAME, which is where a Pulse-shaped Qt id meets a PipeWire
    //    node. AMBIGUITY IS REFUSED: two devices with one name means we
    //    cannot tell which the user picked, and opening the wrong one is
    //    worse than opening the default.
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
                bindingFrom(*profile, *match, QStringLiteral("display-name"));
            if (!binding.isEmpty())
                return binding;
        }
    }

    // 3. SHAPE, and ONLY with no candidates at all -- i.e. the monitor could
    //    not answer. With candidates present, "not found" is an answer: the
    //    device is gone, and the default is the right outcome.
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
        // An element the build does not carry cannot be named in a
        // description: `gst_parse_bin_from_description` fails to PARSE, which
        // would take the microphone out entirely rather than leaving it on
        // the default device.
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
