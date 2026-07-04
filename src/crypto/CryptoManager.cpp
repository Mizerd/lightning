#include "crypto/CryptoManager.h"

CryptoManager::CryptoManager(QObject *parent)
    : QObject(parent)
{
}

void CryptoManager::setBackendName(const QString &backendName)
{
    if (m_backendName == backendName)
        return;
    m_backendName = backendName;
    Q_EMIT backendChanged();
}

bool CryptoManager::supportsE2ee() const
{
    // Only the Rust SDK backend implements E2EE. Even when compiled in, the
    // v0.4 scaffold does not yet route real Matrix crypto — so report false
    // until the scaffold graduates. This function is the single source of
    // truth for the UI: no other layer may claim E2EE support.
#ifdef ENABLE_RUST_SDK_BACKEND
#  ifdef RUST_SDK_E2EE_WIRED
    return m_backendName == QLatin1String("rust");
#  else
    return false;
#  endif
#else
    return false;
#endif
}

bool CryptoManager::supportsEncryptedMedia() const
{
    // Follows supportsE2ee(); documented v0.5+ target.
    return supportsE2ee();
}

bool CryptoManager::supportsDeviceVerification() const
{
    // Rust SDK will drive SAS verification. Not wired in v0.4.
    return false;
}

QString CryptoManager::backendDescription() const
{
    if (m_backendName == QLatin1String("mock")) {
        return QStringLiteral(
            "Mock backend. Encrypted room UI is simulated only — no real "
            "cryptography is performed. Do not use for real conversations.");
    }
    if (m_backendName == QLatin1String("http")) {
        return QStringLiteral(
            "C++ HTTP backend. Talks plain Matrix Client-Server API. Cannot "
            "encrypt or decrypt room messages; encrypted rooms are read-only "
            "placeholders. Use the Rust SDK backend for E2EE (v0.4+).");
    }
    if (m_backendName == QLatin1String("rust")) {
#ifdef ENABLE_RUST_SDK_BACKEND
#  ifdef RUST_SDK_E2EE_WIRED
        return QStringLiteral(
            "Matrix Rust SDK backend. Handles Olm/Megolm sessions, device "
            "keys, and E2EE.");
#  else
        return QStringLiteral(
            "Matrix Rust SDK backend (scaffold). Compiled in but not yet "
            "feature-complete — login/sync/crypto are not wired. Encrypted "
            "sends are blocked and encrypted messages show placeholders.");
#  endif
#else
        return QStringLiteral(
            "Rust SDK backend requested but not compiled in. Configure with "
            "-DENABLE_RUST_SDK_BACKEND=ON.");
#endif
    }
    return QStringLiteral("No backend selected.");
}

QString CryptoManager::statusString() const
{
    if (supportsE2ee())
        return QStringLiteral("E2EE active");
    if (m_backendName == QLatin1String("rust"))
        return QStringLiteral("Rust backend scaffold (E2EE not yet wired)");
    return QStringLiteral("E2EE not available on this backend");
}

bool CryptoManager::isDeviceVerified(const QString &, const QString &) const
{
    return false;
}

bool CryptoManager::isRoomEncrypted(const QString &) const
{
    // The room list model's `encrypted` flag is the UI-side source of truth
    // until Rust SDK exposes its own crypto state per room.
    return false;
}
