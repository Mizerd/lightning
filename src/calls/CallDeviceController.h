// Audio and camera device selection for calls.
//
// Enumerates devices through QMediaDevices, exposes them to QML, and hands
// the choice to the media engine. On Linux the QAudioDevice id is the
// PipeWire/PulseAudio node name, which `pulsesrc device=` accepts directly.
//
// A stored preferred id is kept even while that device is absent: the active
// device falls back to the system default, and reconnecting restores the
// choice. `preferredMissing` reports the difference.
//
// Device ids and descriptions are never logged (hardware fingerprinting).
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QMediaDevices;
class SettingsManager;

class CallDeviceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallDeviceController is exposed via app.callDevices")

    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    /// True inside a Flatpak, where Qt lists no camera (no /dev/video*) yet
    /// the camera works through the xdg Camera portal, which picks the
    /// device. Lets Settings say so instead of "No camera was found". Not
    /// Snap, whose camera plug exposes the real device.
    Q_PROPERTY(bool camerasChosenByDesktop READ camerasChosenByDesktop
                   CONSTANT)
    /// The device actually in use. Falls back to the system default when the
    /// preferred one is absent.
    Q_PROPERTY(QString activeMicrophoneId READ activeMicrophoneId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeSpeakerId READ activeSpeakerId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeCameraId READ activeCameraId
                   NOTIFY selectionChanged)
    /// True when the chosen device is not connected, so the UI can say so
    /// instead of implying the fallback was chosen.
    Q_PROPERTY(bool preferredMicrophoneMissing READ preferredMicrophoneMissing
                   NOTIFY selectionChanged)
    /// The chosen camera is not usable now (unplugged, or its node belongs to
    /// another camera). Unlike the microphone there is no fallback: no
    /// camera opens, so the UI must not show "System default" as selected.
    Q_PROPERTY(bool preferredCameraMissing READ preferredCameraMissing
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool hasMicrophone READ hasMicrophone NOTIFY devicesChanged)
    Q_PROPERTY(bool hasCamera READ hasCamera NOTIFY devicesChanged)

public:
    explicit CallDeviceController(QObject *parent = nullptr);

    void setSettings(SettingsManager *settings);

    /// Each entry: {id, description, isDefault, active}.
    QVariantList microphones() const;
    QVariantList speakers() const;
    QVariantList cameras() const;
    bool camerasChosenByDesktop() const;

    QString activeMicrophoneId() const;
    QString activeSpeakerId() const;
    QString activeCameraId() const;
    bool preferredMicrophoneMissing() const;
    bool preferredCameraMissing() const;
    /// Without a microphone the app still joins, receive-only.
    bool hasMicrophone() const;
    bool hasCamera() const;
    /// At least one audio output exists. Answered from Qt's device list,
    /// which the backend keeps current; for audioOutputsChanged() listeners.
    bool hasSpeaker() const;

    /// Empty selects "system default", stored as such rather than as the
    /// currently resolved id.
    Q_INVOKABLE void selectMicrophone(const QString &id);
    Q_INVOKABLE void selectSpeaker(const QString &id);
    Q_INVOKABLE void selectCamera(const QString &id);

    /// The active device's id and description. Qt and GStreamer ids differ,
    /// and the driver-supplied name is what both agree on
    /// (CaptureDeviceSelection.h). An empty id means "system default".
    struct Selection {
        QString id;
        QString description;
        /// Camera only: a camera was chosen and is not usable now (unplugged,
        /// or its node renumbered to another camera). The engine opens no
        /// camera then: the default is a different device.
        bool preferredMissing = false;
    };
    Selection cameraSelection() const;
    Selection microphoneSelection() const;
    Selection speakerSelection() const;

Q_SIGNALS:
    /// The device LIST changed (hotplug).
    void devicesChanged();
    /// The active or preferred selection changed.
    void selectionChanged();
    /// The active device changed in a way a live call must follow.
    void activeDevicesChanged();
    /// The list of audio OUTPUTS changed: one was added or removed (a
    /// headset, a Bluetooth sink, a remote-desktop audio endpoint). Emitted
    /// even when the default did not move, because a call whose output
    /// failed must try again when one comes back.
    void audioOutputsChanged();

private Q_SLOTS:
    void onDeviceListChanged();

private:
    /// Create QMediaDevices and prime the cached actives on first use; lazy
    /// because touching Qt Multimedia initialises its backend.
    void ensureBackend() const;
    QString resolveActive(const QString &preferred, int kind) const;

    mutable QMediaDevices *m_devices = nullptr;
    QPointer<SettingsManager> m_settings;
    // Cached so a hotplug can report what actually changed.
    QString m_lastActiveMic;
    QString m_lastActiveSpeaker;
    QString m_lastActiveCamera;
    // A missing chosen camera and "system default" share an empty id, and the
    // engine opens a camera for one and not the other.
    bool m_lastCameraMissing = false;
};
