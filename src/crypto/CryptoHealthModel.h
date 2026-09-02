#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

// v0.6.0 checkpoint 7: read-only E2EE health and readiness state.
//
// The Rust Matrix SDK owns all crypto state; this model only mirrors the
// sanitized `crypto_health` snapshots the bridge emits (booleans, enum
// names, and the public device id — never keys, signatures, secrets, or
// store paths) plus coarse lifecycle knowledge from the app layer
// (supported backend, sync state, pending verification). QML consumes
// semantic properties; nothing here can mutate crypto state.
//
// Generation isolation: AppController bumps the generation on logout and
// account switches; snapshots stamped with an older generation are
// ignored, so a stale async answer can never describe the wrong account.
class CryptoHealthModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CryptoHealthModel is exposed via app.cryptoHealth")
    Q_PROPERTY(bool cryptoSupported READ cryptoSupported NOTIFY healthChanged)
    Q_PROPERTY(bool cryptoInitializing READ cryptoInitializing NOTIFY healthChanged)
    Q_PROPERTY(bool cryptoReady READ cryptoReady NOTIFY healthChanged)
    Q_PROPERTY(bool cryptoSyncing READ cryptoSyncing NOTIFY healthChanged)
    Q_PROPERTY(bool cryptoError READ cryptoError NOTIFY healthChanged)
    Q_PROPERTY(QString currentDeviceId READ currentDeviceId NOTIFY healthChanged)
    // Yes / No / Unknown — never guessed from cached UI state.
    Q_PROPERTY(TriState currentDeviceVerified READ currentDeviceVerified NOTIFY healthChanged)
    Q_PROPERTY(bool crossSigningAvailable READ crossSigningAvailable NOTIFY healthChanged)
    Q_PROPERTY(bool crossSigningReady READ crossSigningReady NOTIFY healthChanged)
    Q_PROPERTY(TriState ownIdentityVerified READ ownIdentityVerified NOTIFY healthChanged)
    Q_PROPERTY(bool keyBackupAvailable READ keyBackupAvailable NOTIFY healthChanged)
    Q_PROPERTY(bool keyBackupUsable READ keyBackupUsable NOTIFY healthChanged)
    Q_PROPERTY(QString keyBackupState READ keyBackupState NOTIFY healthChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY healthChanged)
    Q_PROPERTY(bool recoveryRequired READ recoveryRequired NOTIFY healthChanged)
    Q_PROPERTY(QString recoveryState READ recoveryState NOTIFY healthChanged)
    Q_PROPERTY(bool secretStorageAvailable READ secretStorageAvailable NOTIFY healthChanged)
    Q_PROPERTY(int pendingVerificationCount READ pendingVerificationCount NOTIFY healthChanged)
    Q_PROPERTY(QDateTime lastRefreshed READ lastRefreshed NOTIFY healthChanged)
    Q_PROPERTY(QString statusSummary READ statusSummary NOTIFY healthChanged)
    // v0.9 (phase 9): the three cross-signing keys individually, for the
    // detail view. Booleans from the SDK's own identity state — whether
    // each key is KNOWN locally (present in the store), never the private
    // material itself.
    Q_PROPERTY(bool hasMasterKey READ hasMasterKey NOTIFY healthChanged)
    Q_PROPERTY(bool hasSelfSigningKey READ hasSelfSigningKey NOTIFY healthChanged)
    Q_PROPERTY(bool hasUserSigningKey READ hasUserSigningKey NOTIFY healthChanged)

public:
    enum TriState { Unknown = 0, No = 1, Yes = 2 };
    Q_ENUM(TriState)

    explicit CryptoHealthModel(QObject *parent = nullptr);

    // App-layer context (not part of the Rust snapshot).
    void setSupported(bool supported);
    void setSyncing(bool syncing);
    void setPendingVerificationCount(int count);
    void setError(bool error);

    // Adopt one sanitized Rust snapshot. Ignored when `generation` no
    // longer matches (stale answer from a previous account/session).
    void applySnapshot(const QVariantMap &snapshot, quint64 generation);
    quint64 generation() const { return m_generation; }
    // New account/session epoch: everything returns to Unknown.
    void resetForNewGeneration();

    bool cryptoSupported() const { return m_supported; }
    bool cryptoInitializing() const
    { return m_supported && !m_hasSnapshot && !m_error; }
    bool cryptoReady() const { return m_supported && m_hasSnapshot && !m_error; }
    bool cryptoSyncing() const { return m_supported && m_syncing; }
    bool cryptoError() const { return m_error; }
    QString currentDeviceId() const { return m_deviceId; }
    TriState currentDeviceVerified() const { return m_deviceVerified; }
    bool crossSigningAvailable() const { return m_crossSigningAvailable; }
    bool crossSigningReady() const { return m_crossSigningReady; }
    bool hasMasterKey() const { return m_hasMasterKey; }
    bool hasSelfSigningKey() const { return m_hasSelfSigningKey; }
    bool hasUserSigningKey() const { return m_hasUserSigningKey; }
    TriState ownIdentityVerified() const { return m_ownIdentityVerified; }
    bool keyBackupAvailable() const { return m_backupAvailable; }
    bool keyBackupUsable() const { return m_backupUsable; }
    QString keyBackupState() const { return m_backupState; }
    bool recoveryAvailable() const { return m_recoveryState == QLatin1String("enabled"); }
    bool recoveryRequired() const { return m_recoveryState == QLatin1String("incomplete"); }
    QString recoveryState() const { return m_recoveryState; }
    bool secretStorageAvailable() const { return m_secretStorageAvailable; }
    int pendingVerificationCount() const { return m_pendingVerifications; }
    QDateTime lastRefreshed() const { return m_lastRefreshed; }
    QString statusSummary() const;

Q_SIGNALS:
    void healthChanged();

private:
    bool m_supported = false;
    bool m_syncing = false;
    bool m_error = false;
    bool m_hasSnapshot = false;
    quint64 m_generation = 1;
    QString m_deviceId;
    TriState m_deviceVerified = Unknown;
    bool m_crossSigningAvailable = false;
    bool m_crossSigningReady = false;
    bool m_hasMasterKey = false;
    bool m_hasSelfSigningKey = false;
    bool m_hasUserSigningKey = false;
    TriState m_ownIdentityVerified = Unknown;
    bool m_backupAvailable = false;
    bool m_backupUsable = false;
    QString m_backupState = QStringLiteral("unknown");
    QString m_recoveryState = QStringLiteral("unknown");
    bool m_secretStorageAvailable = false;
    int m_pendingVerifications = 0;
    QDateTime m_lastRefreshed;
};
