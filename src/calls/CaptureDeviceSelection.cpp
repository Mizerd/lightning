#include "calls/CaptureDeviceSelection.h"

#include <QStringList>

namespace lightning::calls {
namespace {

struct ElementProfile {
    const char *element;
    const char *property;
    const char *valueKeys[3];
    const char *identityKeys[4];
    bool valueIsQtId;
    bool identityIgnoresCase = false;
};

constexpr ElementProfile kProfiles[] = {
    {"v4l2src", "device", {"device.path", "api.v4l2.path", nullptr},
     {"device.path", "api.v4l2.path", "object.path", nullptr}, false},
    {"pulsesrc", "device", {"device.name", nullptr, nullptr},
     {"device.name", "node.name", nullptr, nullptr}, true},
    {"pulsesink", "device", {"device.name", nullptr, nullptr},
     {"device.name", "node.name", nullptr, nullptr}, true},
    {"pipewiresrc", "target-object", {"object.serial", nullptr, nullptr},
     {"object.serial", "object.id", "node.name", nullptr}, false},
    {"pipewiresink", "target-object", {"object.serial", nullptr, nullptr},
     {"object.serial", "object.id", "node.name", nullptr}, false},
    {"ksvideosrc", "device-path", {"device.path", nullptr, nullptr},
     {"device.path", "device.strid", nullptr, nullptr}, false, true},
    {"wasapi2src", "device", {"device.id", "device.strid", nullptr},
     {"device.id", "device.strid", nullptr, nullptr}, false, true},
    {"wasapi2sink", "device", {"device.id", "device.strid", nullptr},
     {"device.id", "device.strid", nullptr, nullptr}, false, true},
    {"wasapisrc", "device", {"device.strid", "device.id", nullptr},
     {"device.strid", "device.id", nullptr, nullptr}, false, true},
    {"wasapisink", "device", {"device.strid", "device.id", nullptr},
     {"device.strid", "device.id", nullptr, nullptr}, false, true},
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
    bool ok = false;
    const int channels =
        candidate.properties.value(QStringLiteral("audio.channels")).toInt(&ok);
    return DeviceBinding{QString::fromLatin1(profile.property), value, reason,
                         (ok && channels > 0 && channels <= 64) ? channels : 0};
}

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

}

QString captureMixMatrix(int channels)
{
    if (channels <= 2 || channels > 64)
        return QString();
    QStringList row;
    row.reserve(channels);
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

    if (!qtDescription.isEmpty()) {
        const QString wanted = qtDescription.trimmed();
        DeviceBinding match;
        for (const GstDeviceCandidate &candidate : candidates) {
            if (candidate.displayName.trimmed() != wanted)
                continue;
            const DeviceBinding binding =
                bindingFrom(*profile, candidate, QStringLiteral("display-name"),
                            qtDeviceId);
            if (binding.isEmpty())
                continue;
            if (!match.isEmpty()
                && (profile->valueIsQtId
                    || !sameIdentity(*profile, match.value, binding.value)))
                return {};
            if (match.isEmpty())
                match = binding;
        }
        if (!match.isEmpty())
            return match;
    }

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
        if (!availableElements.contains(element))
            continue;
        const DeviceBinding binding = resolveDeviceBinding(
            kind, element, qtDeviceId, qtDescription, candidates);
        if (!binding.isEmpty())
            return ElementChoice{element, binding};
    }
    return {};
}

}
