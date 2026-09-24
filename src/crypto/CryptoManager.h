#pragma once

#include <QObject>
#include <QString>

// Capability and status surface for the active Matrix backend. Implements no
// cryptography; E2EE lives in the active MatrixClient (only the Rust SDK
// backend can decrypt; see docs/threat-model.md). Wired by AppController once
// the backend is known.
class CryptoManager : public QObject
{
    Q_OBJECT

    // Which backend is currently in use ("mock" / "http" / "rust").
    Q_PROPERTY(QString backendName READ backendName NOTIFY backendChanged)

    // Human-readable description of the current crypto capability.
    Q_PROPERTY(QString backendDescription READ backendDescription NOTIFY backendChanged)

    // True only when the active backend actually implements E2EE at the
    // protocol level. HTTP and Mock always report false.
    Q_PROPERTY(bool supportsE2ee READ supportsE2ee NOTIFY backendChanged)

    // Reported false until wired through the Rust SDK.
    Q_PROPERTY(bool supportsEncryptedMedia READ supportsEncryptedMedia NOTIFY backendChanged)
    Q_PROPERTY(bool supportsDeviceVerification READ supportsDeviceVerification NOTIFY backendChanged)

    Q_PROPERTY(QString statusString READ statusString NOTIFY backendChanged)

public:
    explicit CryptoManager(QObject *parent = nullptr);

    // Called by AppController once the active backend is known.
    void setBackendName(const QString &backendName);

    QString backendName() const { return m_backendName; }
    bool supportsE2ee() const;
    bool supportsEncryptedMedia() const;
    bool supportsDeviceVerification() const;
    QString backendDescription() const;
    QString statusString() const;

    // Currently always false. For UX badges only; QML must never gate crypto
    // decisions on these.
    Q_INVOKABLE bool isDeviceVerified(const QString &userId, const QString &deviceId) const;
    Q_INVOKABLE bool isRoomEncrypted(const QString &roomId) const;

Q_SIGNALS:
    void backendChanged();

private:
    QString m_backendName;
};
