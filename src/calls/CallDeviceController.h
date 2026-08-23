// Audio and camera device selection for calls.
//
// Enumerates microphones, speakers and cameras through QMediaDevices (the
// same source VoiceRecorder already uses), exposes them to QML, and hands the
// chosen device to the media engine as a GStreamer device name.
//
// On Linux the QAudioDevice id IS the PipeWire/PulseAudio node name — e.g.
// `alsa_input.usb-Roland_Rubix44-00.analog-surround-40` — which is exactly
// what `pulsesrc device=` / `pulsesink device=` take. That is why selection
// can be applied without a translation table.
//
// PERSISTENCE RULE, and the reason it is not the obvious one: the preferred
// device is stored by ID, but a stored ID that is not currently present is
// NOT discarded. Unplugging a headset must not silently and permanently
// rewrite the user's choice to "whatever was default that day" — plugging it
// back in has to restore it. So the stored value persists, the ACTIVE value
// falls back to the system default, and `preferredMissing` reports the
// difference honestly rather than hiding it.
//
// Nothing here logs a device id or description: on a shared machine a device
// list is a small amount of hardware fingerprinting, and it buys nothing.
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
    /// The device actually in use. Falls back to the system default when the
    /// preferred one is absent.
    Q_PROPERTY(QString activeMicrophoneId READ activeMicrophoneId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeSpeakerId READ activeSpeakerId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeCameraId READ activeCameraId
                   NOTIFY selectionChanged)
    /// True when the user picked a device that is not currently connected, so
    /// the UI can say "your choice is unavailable" instead of pretending the
    /// fallback was chosen.
    Q_PROPERTY(bool preferredMicrophoneMissing READ preferredMicrophoneMissing
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

    QString activeMicrophoneId() const;
    QString activeSpeakerId() const;
    QString activeCameraId() const;
    bool preferredMicrophoneMissing() const;
    /// A machine with no microphone must not crash or refuse to start — it
    /// joins receive-only, so the UI needs to know.
    bool hasMicrophone() const;
    bool hasCamera() const;

    /// Empty selects "system default", which is a real choice and is stored
    /// as such rather than as the resolved id of the day.
    Q_INVOKABLE void selectMicrophone(const QString &id);
    Q_INVOKABLE void selectSpeaker(const QString &id);
    Q_INVOKABLE void selectCamera(const QString &id);

    /// GStreamer source/sink descriptions for the active devices. Empty means
    /// "use the automatic element", so a caller never has to special-case the
    /// default.
    QString microphoneElement() const;
    QString speakerElement() const;

Q_SIGNALS:
    /// The device LIST changed (hotplug).
    void devicesChanged();
    /// The active or preferred selection changed.
    void selectionChanged();
    /// The active device changed in a way a live call must follow.
    void activeDevicesChanged();

private Q_SLOTS:
    void onDeviceListChanged();

private:
    /// Create the QMediaDevices instance and prime the cached actives on
    /// FIRST USE. Kept lazy because touching Qt Multimedia at all initialises
    /// its backend — real startup cost plus SPA log noise on PipeWire — and
    /// most sessions never open a call.
    void ensureBackend() const;
    QString resolveActive(const QString &preferred, int kind) const;

    mutable QMediaDevices *m_devices = nullptr;
    QPointer<SettingsManager> m_settings;
    // Cached so a hotplug can report what actually changed rather than
    // making every consumer re-resolve.
    QString m_lastActiveMic;
    QString m_lastActiveSpeaker;
    QString m_lastActiveCamera;
};
