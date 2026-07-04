#include "storage/SecretStore.h"

#include "storage/InsecureFallbackSecretStore.h"
#include "storage/LibSecretStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecretStore, "matrix.secret.factory")

std::unique_ptr<SecretStore> SecretStore::createDefault(QObject *parent)
{
#ifdef HAVE_LIBSECRET
    auto libsecret = std::make_unique<LibSecretStore>(parent);
    if (libsecret->isAvailable()) {
        qCInfo(lcSecretStore) << "using" << libsecret->backendName();
        return libsecret;
    }
    qCWarning(lcSecretStore)
        << "libsecret compiled in but unavailable at runtime:"
        << libsecret->lastError()
        << "-- falling back to insecure QSettings store";
#else
    qCInfo(lcSecretStore)
        << "libsecret not compiled in — using insecure QSettings fallback";
#endif
    return std::make_unique<InsecureFallbackSecretStore>(parent);
}
