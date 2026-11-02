#include "calls/CallDeviceController.h"

#include <QFileInfo>
#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>
#include <QVariantMap>

#include "app/SandboxEnvironment.h"
#include "app/SettingsManager.h"
#include "calls/CaptureDeviceSelection.h"

namespace {
enum DeviceKind { Microphone, Speaker, Camera };

}

CallDeviceController::CallDeviceController(QObject *parent) : QObject(parent)
{
    // Empty on purpose: touching QMediaDevices initialises the Qt Multimedia
    // backend, which is slow and noisy on PipeWire. Nothing is enumerated
    // until a device menu opens or a call starts.
}

void CallDeviceController::ensureBackend() const
{
    if (m_devices)
        return;
    auto *self = const_cast<CallDeviceController *>(this);
    self->m_devices = new QMediaDevices(self);
    connect(m_devices, &QMediaDevices::audioInputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, self,
            &CallDeviceController::audioOutputsChanged);
    connect(m_devices, &QMediaDevices::videoInputsChanged, self,
            &CallDeviceController::onDeviceListChanged);
    self->m_lastActiveMic = activeMicrophoneId();
    self->m_lastActiveSpeaker = activeSpeakerId();
    self->m_lastActiveCamera = activeCameraId();
    self->m_lastCameraMissing = preferredCameraMissing();
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
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();
}

QString CallDeviceController::resolveActive(const QString &preferred,
                                            int kind) const
{
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
    return cameraSelection().id;
}

bool CallDeviceController::preferredMicrophoneMissing() const
{
    if (!m_settings)
        return false;
    const QString preferred = m_settings->preferredMicrophoneId();
    return !preferred.isEmpty() && activeMicrophoneId().isEmpty();
}

bool CallDeviceController::preferredCameraMissing() const
{
    return !camerasChosenByDesktop() && cameraSelection().preferredMissing;
}

bool CallDeviceController::hasMicrophone() const
{
    ensureBackend();
    return !QMediaDevices::audioInputs().isEmpty();
}

bool CallDeviceController::hasSpeaker() const
{
    ensureBackend();
    return !QMediaDevices::audioOutputs().isEmpty();
}

bool CallDeviceController::camerasChosenByDesktop() const
{
#if defined(Q_OS_LINUX)
    return sandboxenv::isFlatpak();
#else
    return false;
#endif
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
    const Selection selection = cameraSelection();
    const QString active = selection.id;
    const bool missing = selection.preferredMissing;
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
        row.insert(QStringLiteral("chosen"), id == preferred && !missing);
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
    if (!m_settings)
        return;
    ensureBackend();
    QString description;
    for (const QCameraDevice &device : QMediaDevices::videoInputs()) {
        if (QString::fromUtf8(device.id()) == id) {
            description = device.description();
            break;
        }
    }
    const bool sameId = m_settings->preferredCameraId() == id;
    m_settings->setPreferredCameraDescription(id.isEmpty() ? QString()
                                                           : description);
    m_settings->setPreferredCameraId(id);
    if (sameId)
        onDeviceListChanged();
}

namespace {

template <typename ListT>
QString describe(const ListT &devices, const QString &id)
{
    if (id.isEmpty())
        return {};
    for (const auto &device : devices) {
        if (QString::fromUtf8(device.id()) == id)
            return device.description();
    }
    return {};
}

}

CallDeviceController::Selection CallDeviceController::cameraSelection() const
{
    ensureBackend();
    if (!m_settings)
        return {};
    QList<lightning::calls::PresentCamera> present;
    for (const QCameraDevice &device : QMediaDevices::videoInputs())
        present.append({QString::fromUtf8(device.id()), device.description()});
    const lightning::calls::CameraSelection chosen =
        lightning::calls::resolveCameraPreference(
            m_settings->preferredCameraId(),
            m_settings->preferredCameraDescription(), present);
    return {chosen.id, chosen.description, chosen.preferredMissing};
}

CallDeviceController::Selection CallDeviceController::microphoneSelection() const
{
    ensureBackend();
    const QString id = activeMicrophoneId();
    return {id, describe(QMediaDevices::audioInputs(), id)};
}

CallDeviceController::Selection CallDeviceController::speakerSelection() const
{
    ensureBackend();
    const QString id = activeSpeakerId();
    return {id, describe(QMediaDevices::audioOutputs(), id)};
}

void CallDeviceController::onDeviceListChanged()
{
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();

    const QString mic = activeMicrophoneId();
    const QString speaker = activeSpeakerId();
    const Selection cameraNow = cameraSelection();
    const QString camera = cameraNow.id;
    const bool moved = mic != m_lastActiveMic
        || speaker != m_lastActiveSpeaker || camera != m_lastActiveCamera
        || cameraNow.preferredMissing != m_lastCameraMissing;
    m_lastActiveMic = mic;
    m_lastActiveSpeaker = speaker;
    m_lastActiveCamera = camera;
    m_lastCameraMissing = cameraNow.preferredMissing;
    if (moved)
        Q_EMIT activeDevicesChanged();
}
