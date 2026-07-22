#include "storage/SecretStore.h"

#include "storage/InsecureFallbackSecretStore.h"
#include "storage/LibSecretStore.h"
#include "storage/WinCredStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecretStore, "matrix.secret.factory")

std::unique_ptr<SecretStore> SecretStore::createDefault(QObject *parent)
{
#ifdef HAVE_WINCRED
    auto wincred = std::make_unique<WinCredStore>(parent);
    if (wincred->isAvailable()) {
        qCInfo(lcSecretStore) << "using" << wincred->backendName();
        return wincred;
    }
    qCWarning(lcSecretStore)
        << "Windows Credential Manager unavailable:"
        << wincred->lastError()
        << "-- falling back to insecure QSettings store";
#endif
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
#endif
#if !defined(HAVE_WINCRED) && !defined(HAVE_LIBSECRET)
    qCInfo(lcSecretStore)
        << "no native secure store compiled in — using insecure QSettings fallback";
#endif
    return std::make_unique<InsecureFallbackSecretStore>(parent);
}
