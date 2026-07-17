#include "crypto/CryptoHealthModel.h"

CryptoHealthModel::CryptoHealthModel(QObject *parent)
    : QObject(parent)
{
}

void CryptoHealthModel::setSupported(bool supported)
{
    if (m_supported == supported)
        return;
    m_supported = supported;
    Q_EMIT healthChanged();
}

void CryptoHealthModel::setSyncing(bool syncing)
{
    if (m_syncing == syncing)
        return;
    m_syncing = syncing;
    Q_EMIT healthChanged();
}

void CryptoHealthModel::setPendingVerificationCount(int count)
{
    count = qMax(0, count);
    if (m_pendingVerifications == count)
        return;
    m_pendingVerifications = count;
    Q_EMIT healthChanged();
}

void CryptoHealthModel::setError(bool error)
{
    if (m_error == error)
        return;
    m_error = error;
    Q_EMIT healthChanged();
}

void CryptoHealthModel::applySnapshot(const QVariantMap &snapshot,
                                      quint64 generation)
{
    if (generation != m_generation)
        return;   // stale: answered for a previous account/session epoch

    auto triState = [&](const QString &key) {
        return snapshot.value(key).toBool() ? Yes : No;
    };
    m_hasSnapshot = true;
    m_error = false;
    m_deviceId = snapshot.value(QStringLiteral("device_id")).toString();
    // "Verified" means cross-signed by the account owner — SDK truth, never
    // an inference from local session restoration.
    m_deviceVerified =
        snapshot.value(QStringLiteral("device_cross_signed")).toBool()
            ? Yes
            : triState(QStringLiteral("device_verified"));
    const bool hasMaster =
        snapshot.value(QStringLiteral("has_master")).toBool();
    const bool hasSelf =
        snapshot.value(QStringLiteral("has_self_signing")).toBool();
    const bool hasUser =
        snapshot.value(QStringLiteral("has_user_signing")).toBool();
    m_crossSigningAvailable =
        snapshot.value(QStringLiteral("own_identity_available")).toBool()
        || hasMaster || hasSelf || hasUser;
    m_crossSigningReady = hasMaster && hasSelf && hasUser;
    m_ownIdentityVerified =
        snapshot.value(QStringLiteral("own_identity_available")).toBool()
            ? triState(QStringLiteral("own_identity_verified"))
            : Unknown;
    m_backupState = snapshot.value(QStringLiteral("backup_state")).toString();
    if (m_backupState.isEmpty())
        m_backupState = QStringLiteral("unknown");
    m_backupAvailable =
        snapshot.value(QStringLiteral("backup_exists_on_server")).toBool();
    // Usable = the SDK actively backs up / reads with this backup. An
    // existing-but-unconnected backup is "available", not "usable".
    m_backupUsable = m_backupState == QLatin1String("enabled");
    m_recoveryState =
        snapshot.value(QStringLiteral("recovery_state")).toString();
    if (m_recoveryState.isEmpty())
        m_recoveryState = QStringLiteral("unknown");
    m_secretStorageAvailable =
        snapshot.value(QStringLiteral("secret_storage_enabled")).toBool();
    m_lastRefreshed = QDateTime::currentDateTimeUtc();
    Q_EMIT healthChanged();
}

void CryptoHealthModel::resetForNewGeneration()
{
    ++m_generation;
    m_syncing = false;
    m_error = false;
    m_hasSnapshot = false;
    m_deviceId.clear();
    m_deviceVerified = Unknown;
    m_crossSigningAvailable = false;
    m_crossSigningReady = false;
    m_ownIdentityVerified = Unknown;
    m_backupAvailable = false;
    m_backupUsable = false;
    m_backupState = QStringLiteral("unknown");
    m_recoveryState = QStringLiteral("unknown");
    m_secretStorageAvailable = false;
    m_pendingVerifications = 0;
    m_lastRefreshed = QDateTime{};
    Q_EMIT healthChanged();
}

QString CryptoHealthModel::statusSummary() const
{
    if (!m_supported)
        return tr("This backend does not support end-to-end encryption.");
    if (m_error)
        return tr("Encryption state could not be read.");
    if (!m_hasSnapshot)
        return tr("Checking encryption state…");
    if (m_deviceVerified == Yes && m_backupUsable)
        return tr("This session is verified and key backup is active.");
    if (m_deviceVerified == Yes)
        return tr("This session is verified.");
    if (m_crossSigningReady || m_crossSigningAvailable)
        return tr("This session is not verified yet. Verify it from another "
                  "session or with a recovery key.");
    return tr("Cross-signing is not set up for this account.");
}
