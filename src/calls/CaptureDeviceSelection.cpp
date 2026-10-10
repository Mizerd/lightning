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
    // Identity keys, most specific first. The value set is the one under the
    // first present `valueKeys` entry, not the key that matched.
    const char *valueKeys[3];     // keys holding what `property` wants
    const char *identityKeys[4];  // keys whose value may equal the Qt id
    /// The value to set is the Qt id itself. True for the PulseAudio
    /// elements: pipewire-pulse names devices exactly as QMediaDevices does,
    /// while the PipeWire device provider's candidates carry no `device.name`,
    /// so a candidate-derived value would never bind.
    bool valueIsQtId;
    /// Identity compares without case: Windows device paths are
    /// case-insensitive and Qt and the provider may spell them differently.
    bool identityIgnoresCase = false;
};

constexpr ElementProfile kProfiles[] = {
    // Linux video. Qt's V4L2 camera id is the node path. The v4l2 device
    // provider publishes it as `device.path`; the PipeWire provider, which
    // wins on a PipeWire desktop, only as `api.v4l2.path`.
    {"v4l2src", "device", {"device.path", "api.v4l2.path", nullptr},
     {"device.path", "api.v4l2.path", "object.path", nullptr}, false},
    // Linux audio through PulseAudio or pipewire-pulse. `node.name` is what a
    // PipeWire desktop's monitor publishes, matching the Pulse device name.
    {"pulsesrc", "device", {"device.name", nullptr, nullptr},
     {"device.name", "node.name", nullptr, nullptr}, true},
    {"pulsesink", "device", {"device.name", nullptr, nullptr},
     {"device.name", "node.name", nullptr, nullptr}, true},
    // Linux through PipeWire directly. The handle is a numeric node id that no
    // Qt id matches, so the display name is the bridge.
    {"pipewiresrc", "target-object", {"object.serial", nullptr, nullptr},
     {"object.serial", "object.id", "node.name", nullptr}, false},
    {"pipewiresink", "target-object", {"object.serial", nullptr, nullptr},
     {"object.serial", "object.id", "node.name", nullptr}, false},
    // Windows. The ks provider publishes no property structure; the path is a
    // GObject property of the device, which the enumeration copies into
    // `device.path`.
    {"ksvideosrc", "device-path", {"device.path", nullptr, nullptr},
     {"device.path", "device.strid", nullptr, nullptr}, false, true},
    // Both WASAPI generations take the same MMDevice endpoint id. The
    // monitor may enumerate only one generation even when both elements exist.
    {"wasapi2src", "device", {"device.id", "device.strid", nullptr},
     {"device.id", "device.strid", nullptr, nullptr}, false, true},
    {"wasapi2sink", "device", {"device.id", "device.strid", nullptr},
     {"device.id", "device.strid", nullptr, nullptr}, false, true},
    {"wasapisrc", "device", {"device.strid", "device.id", nullptr},
     {"device.strid", "device.id", nullptr, nullptr}, false, true},
    {"wasapisink", "device", {"device.strid", "device.id", nullptr},
     {"device.strid", "device.id", nullptr, nullptr}, false, true},
    // macOS. Qt's id is the AVFoundation unique id, which the avf provider
    // publishes as `avf.unique_id`; `device-index` is a GObject property of
    // the device, copied into `device.index` by the enumeration.
    {"avfvideosrc", "device-index", {"device.index", nullptr, nullptr},
     {"avf.unique_id", "device.unique-id", "device.index", nullptr}, false},
    // osxaudio publishes `unique-id`, not device.id/device.uid. Since 1.26
    // both elements select with that stable CoreAudio UID (as the provider's
    // own create_element does), rather than a transient numeric device id.
    {"osxaudiosrc", "unique-id", {"unique-id", nullptr, nullptr},
     {"unique-id", nullptr, nullptr, nullptr}, false},
    {"osxaudiosink", "unique-id", {"unique-id", nullptr, nullptr},
     {"unique-id", nullptr, nullptr, nullptr}, false},
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
    const QString element = QString::fromLatin1(profile.element);
    if (element.startsWith(QLatin1String("wasapi"))) {
        const QString api = candidate.properties.value(QStringLiteral("device.api"));
        if (!api.isEmpty() && api != QLatin1String("wasapi")
            && api != QLatin1String("wasapi2"))
            return {};
        // wasapi2's default pseudo-device follows Windows' default even if
        // its actual-id currently matches the chosen endpoint. Its speaker
        // loopback candidates are Audio/Source too, but are not microphones.
        if (candidate.properties.value(QStringLiteral("device.default"))
                == QLatin1String("true")
            || (element.endsWith(QLatin1String("src"))
                && candidate.properties.value(QStringLiteral("wasapi2.device.loopback"))
                    == QLatin1String("true")))
            return {};
    }
    // See ElementProfile::valueIsQtId.
    QString value;
    if (profile.valueIsQtId) {
        value = qtDeviceId;
    } else {
        for (const char *key : profile.valueKeys) {
            if (!key)
                break;
            value = candidate.properties.value(QString::fromLatin1(key));
            if (!value.isEmpty())
                break;
        }
    }
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

// A value as gst_value_serialize() wrote it: a string holding anything but
// [A-Za-z0-9_+-/:.] comes back quoted with backslash escapes (a WASAPI id
// `{0.0.1.00000000}.{guid}` or a Windows device path), and would then match
// no Qt id and make a garbage `device=`. Unwrapped here; anything else is
// returned as it is.
QString unserialized(const QString &value)
{
    if (value.size() < 2 || !value.startsWith(QLatin1Char('"'))
        || !value.endsWith(QLatin1Char('"'))) {
        return value;
    }
    QString out;
    out.reserve(value.size());
    const QStringView body = QStringView(value).mid(1, value.size() - 2);
    for (qsizetype i = 0; i < body.size(); ++i) {
        if (body.at(i) == QLatin1Char('\\') && i + 1 < body.size())
            ++i;
        out.append(body.at(i));
    }
    return out;
}

QList<GstDeviceCandidate> unserializedCandidates(
    const QList<GstDeviceCandidate> &candidates)
{
    QList<GstDeviceCandidate> out = candidates;
    for (GstDeviceCandidate &candidate : out) {
        for (auto it = candidate.properties.begin();
             it != candidate.properties.end(); ++it) {
            it.value() = unserialized(it.value());
        }
    }
    return out;
}

bool sameIdentity(const ElementProfile &profile, const QString &a,
                  const QString &b)
{
    return QString::compare(a, b,
                            profile.identityIgnoresCase ? Qt::CaseInsensitive
                                                        : Qt::CaseSensitive)
        == 0;
}

DeviceBinding resolveOnce(const ElementProfile &profile,
                          const QString &element, const QString &qtDeviceId,
                          const QString &qtDescription,
                          const QList<GstDeviceCandidate> &candidates);

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
    // "System default" follows the platform; never pin it.
    if (qtDeviceId.isEmpty())
        return {};
    const ElementProfile *profile = profileFor(element);
    DeviceBinding binding = profile
        ? resolveOnce(*profile, element, qtDeviceId, qtDescription,
                      unserializedCandidates(candidates))
        : DeviceBinding{};
    // A chosen camera that cannot be bound is refused, not replaced: the
    // default is a different camera, and opening a camera the user did not
    // pick is a privacy failure. A microphone or speaker deliberately keeps
    // the platform default instead: a wrong `device=` would lose the call's
    // audio, and the default device is the one the user already hears.
    if (binding.isEmpty() && kind == CaptureKind::Camera)
        binding.refused = true;
    return binding;
}

namespace {

DeviceBinding resolveOnce(const ElementProfile &profileRef,
                          const QString &element, const QString &qtDeviceId,
                          const QString &qtDescription,
                          const QList<GstDeviceCandidate> &candidates)
{
    const ElementProfile *profile = &profileRef;

    // 1. Identity: an exact match on a key both subsystems publish.
    for (const GstDeviceCandidate &candidate : candidates) {
        for (const char *key : profile->identityKeys) {
            if (!key)
                continue;
            if (!sameIdentity(*profile,
                              candidate.properties.value(
                                  QString::fromLatin1(key)),
                              qtDeviceId)) {
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
        DeviceBinding match;
        for (const GstDeviceCandidate &candidate : candidates) {
            if (candidate.displayName.trimmed() != wanted)
                continue;
            const DeviceBinding binding =
                bindingFrom(*profile, candidate, QStringLiteral("display-name"),
                            qtDeviceId);
            // A candidate from another subsystem that cannot select this
            // element does not make a usable candidate ambiguous. Providers
            // listing the same endpoint twice are also one choice.
            if (binding.isEmpty())
                continue;
            if (!match.isEmpty()
                && (profile->valueIsQtId
                    || !sameIdentity(*profile, match.value, binding.value)))
                return {}; // genuinely different devices with the same name
            if (match.isEmpty())
                match = binding;
        }
        if (!match.isEmpty())
            return match;
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

} // namespace

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
