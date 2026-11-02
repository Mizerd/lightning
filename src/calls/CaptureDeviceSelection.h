#pragma once

#include <QList>
#include <QMap>
#include <QString>

namespace lightning::calls {

enum class CaptureKind {
    Camera,
    Microphone,
    Speaker,
};

struct GstDeviceCandidate {
    QString displayName;
    QMap<QString, QString> properties;
};

struct DeviceBinding {
    QString property;
    QString value;
    QString reason;
    int channels = 0;
    bool refused = false;

    bool isEmpty() const { return property.isEmpty(); }
};

QString captureMixMatrix(int channels);

QString captureChannelCaps(int channels);

/// The property binding for `element`, or an empty binding: "let the platform
/// choose" for audio, "refused" (see `refused`) for a chosen camera.
///
/// `qtDeviceId` and `qtDescription` come from QMediaDevices; an empty id
/// ("system default") always yields an empty binding that is not refused.
/// `candidates` is what GStreamer's device monitor reported for the device
/// class; an empty list means the monitor was unavailable, enabling the
/// shape rule. Unknown elements yield an empty binding.
DeviceBinding resolveDeviceBinding(CaptureKind kind, const QString &element,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

struct ElementChoice {
    QString element;
    DeviceBinding binding;

    bool isEmpty() const { return element.isEmpty(); }
};

ElementChoice chooseCaptureElement(CaptureKind kind,
                                   const QStringList &preferenceOrder,
                                   const QStringList &availableElements,
                                   const QString &qtDeviceId,
                                   const QString &qtDescription,
                                   const QList<GstDeviceCandidate> &candidates);

struct PresentCamera {
    QString id;
    QString description;
};

struct CameraSelection {
    QString id;
    QString description;
    /// A camera was chosen and is not usable now: unplugged, or its node now
    /// belongs to another camera. The engine must open NO camera; the default
    /// would be a different device.
    bool preferredMissing = false;
};

inline CameraSelection resolveCameraPreference(
    const QString &preferredId, const QString &storedDescription,
    const QList<PresentCamera> &present)
{
    if (preferredId.isEmpty())
        return {};
    for (const PresentCamera &camera : present) {
        if (camera.id != preferredId)
            continue;
        const QString stored = storedDescription.simplified();
        if (preferredId.startsWith(QLatin1String("/dev/video"))
            && !stored.isEmpty() && camera.description.simplified() != stored)
            return CameraSelection{QString(), QString(), true};
        return CameraSelection{camera.id, camera.description, false};
    }
    return CameraSelection{QString(), QString(), true};
}

QStringList identityKeysForElement(const QString &element);

QString devicePropertyForElement(const QString &element);

}
