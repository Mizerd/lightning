#include "crypto/CryptoManager.h"

CryptoManager::CryptoManager(QObject *parent)
    : QObject(parent)
{
}

QString CryptoManager::backendDescription() const
{
    return QStringLiteral("No crypto backend (v0.1). Real E2EE arrives via the Matrix Rust SDK in v0.4.");
}

bool CryptoManager::isDeviceVerified(const QString &, const QString &) const
{
    return false;
}

bool CryptoManager::isRoomEncrypted(const QString &) const
{
    // The room list model's `encrypted` flag is the UI-side source of truth
    // in v0.1. Real crypto state lives here from v0.4 onward.
    return false;
}
