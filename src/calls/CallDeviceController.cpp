#include "calls/CallDeviceController.h"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>
#include <QVariantMap>

#include "app/SettingsManager.h"

namespace {
/// Which list a resolve is against. Kept local: the public API is three
/// explicit accessors, so this never needs to be a shared enum.
enum DeviceKind { Microphone, Speaker, Camera };

/// A pipeline description fragment. The id is already sanitized by
/// SettingsManager (bounded, no control characters, no quote/backslash/`!`),
/// which is what makes embedding it in a gst-parse description safe.
QString pulseElement(const QString &element, const QString &id)
{
    if (id.isEmpty())
        return QString();
    return QStringLiteral("%1 device=\"%2\"").arg(element, id);
}
} // namespace

CallDeviceController::CallDeviceController(QObject *parent) : QObject(parent)
{
    // Deliberately EMPTY. Touching QMediaDevices — even to read a list —
    // initialises the Qt Multimedia backend, which on a PipeWire desktop
    // costs real startup time and prints SPA parse noise for every device on
    // the system. Lightning already learned this once: the first QVideoSink
    // in a process blocks ~931 ms (see VideoPosterExtractor). So nothing is
    // enumerated until something actually asks — opening a device menu, or
    // starting a call.
}

void CallDeviceController::ensureBackend() const
{
    if (m_devices)
        return;
    auto *self = const_cast<CallDeviceController *>(this);
    self->m_devices = new QMediaDevices(self);
    // Hotplug. All three lists funnel into one handler, which re-resolves and
    // only announces an ACTIVE change when the resolved device actually
    // moved — plugging in an unrelated webcam must not restart audio.
    connect(m_devices, &QMediaDevices::audioInputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    connect(m_devices, &QMediaDevices::videoInputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    self->m_lastActiveMic = activeMicrophoneId();
    self->m_lastActiveSpeaker = activeSpeakerId();
    self->m_lastActiveCamera = activeCameraId();
}

void CallDeviceController::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::callDevicePreferenceChanged,
                this, &CallDeviceController::onDeviceListChanged);
    }
    // No enumeration here: setSettings runs at login, and warming the
    // multimedia backend then is exactly the startup cost this class avoids.
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();
}

QString CallDeviceController::resolveActive(const QString &preferred,
                                            int kind) const
{
    // An explicit "system default" (empty preference) stays empty: the
    // caller then uses the automatic element, which follows the system
    // default as it changes rather than pinning today's answer.
    if (preferred.isEmpty())
        return QString();
    const auto present = [&](const QString &id) {
        switch (kind) {
        case Microphone:
            for (const QAudioDevice &d : QMediaDevices::audioInputs()) {
                if (QString::fromUtf8(d.id()) == id)
                    return true;
            }
            return false;
        case Speaker:
            for (const QAudioDevice &d : QMediaDevices::audioOutputs()) {
                if (QString::fromUtf8(d.id()) == id)
                    return true;
            }
            return false;
        default:
            for (const QCameraDevice &d : QMediaDevices::videoInputs()) {
                if (QString::fromUtf8(d.id()) == id)
                    return true;
            }
            return false;
        }
    };
    // The preference is KEPT even when the device is absent (see the header):
    // only the resolved ACTIVE value falls back, so reconnecting a headset
    // restores the user's choice instead of having silently lost it.
    return present(preferred) ? preferred : QString();
}

QString CallDeviceController::activeMicrophoneId() const
{
    return resolveActive(
        m_settings ? m_settings->preferredMicrophoneId() : QString(),
        Microphone);
}

QString CallDeviceController::activeSpeakerId() const
{
    return resolveActive(
        m_settings ? m_settings->preferredSpeakerId() : QString(), Speaker);
}

QString CallDeviceController::activeCameraId() const
{
    return resolveActive(
        m_settings ? m_settings->preferredCameraId() : QString(), Camera);
}

bool CallDeviceController::preferredMicrophoneMissing() const
{
    if (!m_settings)
        return false;
    const QString preferred = m_settings->preferredMicrophoneId();
    // Only a NON-EMPTY preference can be missing; "system default" always
    // resolves.
    return !preferred.isEmpty() && activeMicrophoneId().isEmpty();
}

bool CallDeviceController::hasMicrophone() const
{
    ensureBackend();
    return !QMediaDevices::audioInputs().isEmpty();
}

bool CallDeviceController::hasCamera() const
{
    ensureBackend();
    return !QMediaDevices::videoInputs().isEmpty();
}

QVariantList CallDeviceController::microphones() const
{
    ensureBackend();
    QVariantList out;
    const QString active = activeMicrophoneId();
    const QString preferred =
        m_settings ? m_settings->preferredMicrophoneId() : QString();
    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        const QString id = QString::fromUtf8(device.id());
        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("description"), device.description());
        row.insert(QStringLiteral("isDefault"), device.isDefault());
        // "active" is what audio actually flows through; "chosen" is what
        // the user picked. They differ exactly when a preferred device is
        // unplugged, and the menu shows both truthfully.
        row.insert(QStringLiteral("active"),
                   active.isEmpty() ? device.isDefault() : id == active);
        row.insert(QStringLiteral("chosen"), id == preferred);
        out.append(row);
    }
    return out;
}

QVariantList CallDeviceController::speakers() const
{
    ensureBackend();
    QVariantList out;
    const QString active = activeSpeakerId();
    const QString preferred =
        m_settings ? m_settings->preferredSpeakerId() : QString();
    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        const QString id = QString::fromUtf8(device.id());
        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("description"), device.description());
        row.insert(QStringLiteral("isDefault"), device.isDefault());
        row.insert(QStringLiteral("active"),
                   active.isEmpty() ? device.isDefault() : id == active);
        row.insert(QStringLiteral("chosen"), id == preferred);
        out.append(row);
    }
    return out;
}

QVariantList CallDeviceController::cameras() const
{
    ensureBackend();
    QVariantList out;
    const QString active = activeCameraId();
    const QString preferred =
        m_settings ? m_settings->preferredCameraId() : QString();
    for (const QCameraDevice &device : QMediaDevices::videoInputs()) {
        const QString id = QString::fromUtf8(device.id());
        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("description"), device.description());
        row.insert(QStringLiteral("isDefault"), device.isDefault());
        row.insert(QStringLiteral("active"),
                   active.isEmpty() ? device.isDefault() : id == active);
        row.insert(QStringLiteral("chosen"), id == preferred);
        out.append(row);
    }
    return out;
}

void CallDeviceController::selectMicrophone(const QString &id)
{
    if (m_settings)
        m_settings->setPreferredMicrophoneId(id);
}

void CallDeviceController::selectSpeaker(const QString &id)
{
    if (m_settings)
        m_settings->setPreferredSpeakerId(id);
}

void CallDeviceController::selectCamera(const QString &id)
{
    if (m_settings)
        m_settings->setPreferredCameraId(id);
}

QString CallDeviceController::microphoneElement() const
{
    // pulsesrc rather than pipewiresrc: pipewire-pulse exposes exactly the
    // node names QMediaDevices reports, so the id needs no translation. An
    // empty result means "use autoaudiosrc" and follow the system default.
    return pulseElement(QStringLiteral("pulsesrc"), activeMicrophoneId());
}

QString CallDeviceController::speakerElement() const
{
    return pulseElement(QStringLiteral("pulsesink"), activeSpeakerId());
}

void CallDeviceController::onDeviceListChanged()
{
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();

    // Only announce an ACTIVE change when the resolved device really moved.
    // A live call re-opens its capture on this signal, so firing it for an
    // unrelated hotplug would interrupt audio for no reason.
    const QString mic = activeMicrophoneId();
    const QString speaker = activeSpeakerId();
    const QString camera = activeCameraId();
    const bool moved = mic != m_lastActiveMic
        || speaker != m_lastActiveSpeaker || camera != m_lastActiveCamera;
    m_lastActiveMic = mic;
    m_lastActiveSpeaker = speaker;
    m_lastActiveCamera = camera;
    if (moved)
        Q_EMIT activeDevicesChanged();
}
