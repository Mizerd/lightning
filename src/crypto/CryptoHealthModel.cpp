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
    // `device_verified` IS A CONSTANT FOR OUR OWN DEVICE AND MUST NEVER
    // REACH THIS LABEL. It is `Device::is_verified()` =
    // `is_locally_trusted() || is_cross_signing_trusted()`, and
    // matrix-sdk-crypto sets the own device's local trust unconditionally
    // when it creates it (machine/mod.rs:350) — "since we are the owners of
    // the private keys of this device we can safely mark the device as
    // verified". So it is TRUE on every session from the moment the store
    // exists, carries no information, and any label bound to it is a
    // permanent green badge.
    //
    // This line used to read `device_cross_signed ? Yes : device_verified`,
    // and that second arm was the defect: a brand-new unverified session
    // showed "This session verified: Available" while the device list beside
    // it correctly said "Not verified" — the contradiction that was
    // reported. The list was right; this was wrong.
    //
    // What "verified" MEANS for our own session is that our own identity has
    // cross-signed it, i.e. some session of ours vouched for this one.
    // That is `is_cross_signed_by_owner()`, sent as `device_cross_signed`.
    // MEASURED on a real account 2026-09-20: fresh login, every cross-signing
    // key Missing, server reporting 28 of 28 devices unsigned — this reads
    // No, and reading `device_verified` instead read Yes.
    m_deviceVerified = triState(QStringLiteral("device_cross_signed"));
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
    m_hasMasterKey = hasMaster;
    m_hasSelfSigningKey = hasSelf;
    m_hasUserSigningKey = hasUser;
    m_ownIdentityVerified =
        snapshot.value(QStringLiteral("own_identity_available")).toBool()
            ? triState(QStringLiteral("own_identity_verified"))
            : Unknown;
    m_backupState = snapshot.value(QStringLiteral("backup_state")).toString();
    if (m_backupState.isEmpty())
        m_backupState = QStringLiteral("unknown");
    // An ABSENT or null field means the server could not be asked. Only an
    // explicit boolean is an answer.
    const QVariant backupExists =
        snapshot.value(QStringLiteral("backup_exists_on_server"));
    m_backupAvailable = !backupExists.isValid() || backupExists.isNull()
        ? Unknown
        : (backupExists.toBool() ? Yes : No);
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
    m_hasMasterKey = false;
    m_hasSelfSigningKey = false;
    m_hasUserSigningKey = false;
    m_ownIdentityVerified = Unknown;
    m_backupAvailable = Unknown;
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
