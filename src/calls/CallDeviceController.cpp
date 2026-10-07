#include "calls/CallDeviceController.h"

#include <QFileInfo>
#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>
#include <QVariantMap>

#include "app/SettingsManager.h"
#include "calls/CaptureDeviceSelection.h"

namespace {
/// Which device list a resolve checks.
enum DeviceKind { Microphone, Speaker, Camera };

/// Builds a `<element> device="<id>"` fragment, or empty for the system
/// default.
///
/// Linux only: these are PulseAudio element names, and QAudioDevice ids on
/// WASAPI or CoreAudio are not what those GStreamer elements accept, so a
/// foreign id would stop the call from starting. Empty falls back to
/// `autoaudiosrc`/`autoaudiosink`. Real parity needs GstDeviceMonitor ids.
QString platformDeviceElement(const QString &element, const QString &id)
{
    if (id.isEmpty())
        return QString();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Q_UNUSED(element);
    return QString();
#else
    // The result is parsed by gst_parse_bin_from_description, where a quote
    // ends the value and `!` starts another element. Refuse ids that cannot
    // be represented literally; the caller then uses the automatic element.
    // (The SFU engine sets the property on the parsed element instead; see
    // CaptureDeviceSelection.h.)
    for (const QChar c : id) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')
            || c == QLatin1Char('!') || c.category() == QChar::Other_Control) {
            return QString();
        }
    }
    return QStringLiteral("%1 device=\"%2\"").arg(element, id);
#endif
}
} // namespace

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
    // Hotplug for all three lists; an active change is announced only when
    // the resolved device actually moved.
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
    // No enumeration here: this runs at login.
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();
}

QString CallDeviceController::resolveActive(const QString &preferred,
                                            int kind) const
{
    // An empty preference ("system default") stays empty, so the automatic
    // element keeps following the default.
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
    // The preference is kept even when absent; only the active value falls
    // back, so reconnecting a device restores the choice.
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
    // Only a non-empty preference can be missing.
    return !preferred.isEmpty() && activeMicrophoneId().isEmpty();
}

bool CallDeviceController::preferredCameraMissing() const
{
    // In a sandbox Qt lists no camera and the desktop's portal chooses one
    // when it is turned on; the UI says that instead.
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
    return !qEnvironmentVariableIsEmpty("FLATPAK_ID")
        || QFileInfo::exists(QStringLiteral("/.flatpak-info"));
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
        // "active" is what audio flows through, "chosen" what the user
        // picked; they differ when a preferred device is unplugged.
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
        // A missing camera is chosen by nobody: the device under its old
        // /dev/videoN is another camera, and "chosen" draws a radio.
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
    // The description goes first: the id setter announces the change, and a
    // listener must read the pair. Recorded so a /dev/videoN that later
    // belongs to another camera is refused rather than opened.
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
    // Re-picking the same id announces nothing through the settings (the id
    // did not change), yet it can end "missing" by recording the camera now
    // under that node. Announce it here.
    if (sameId)
        onDeviceListChanged();
}

namespace {

// The description of the device with this id, or empty when gone. Looked up,
// not stored: driver strings can change on hotplug.
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

} // namespace

CallDeviceController::Selection CallDeviceController::cameraSelection() const
{
    ensureBackend();
    if (!m_settings)
        return {};
    QList<lightning::calls::PresentCamera> present;
    for (const QCameraDevice &device : QMediaDevices::videoInputs())
        present.append({QString::fromUtf8(device.id()), device.description()});
    // Unlike audio, a missing camera is reported, never resolved to the
    // default (CaptureDeviceSelection.h).
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

QString CallDeviceController::microphoneElement() const
{
    // pulsesrc rather than pipewiresrc: pipewire-pulse exposes the node names
    // QMediaDevices reports, so no translation is needed. Empty means
    // autoaudiosrc (system default).
    return platformDeviceElement(QStringLiteral("pulsesrc"),
                                 activeMicrophoneId());
}

QString CallDeviceController::speakerElement() const
{
    return platformDeviceElement(QStringLiteral("pulsesink"),
                                 activeSpeakerId());
}

void CallDeviceController::onDeviceListChanged()
{
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();

    // Announce an active change only when the resolved device moved: a live
    // call re-opens its capture on this signal.
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
