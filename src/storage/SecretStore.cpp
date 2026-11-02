#include "storage/SecretStore.h"

#include "storage/InsecureFallbackSecretStore.h"
#include "storage/LibSecretStore.h"
#include "storage/MacKeychainStore.h"
#include "storage/PortableMode.h"
#include "storage/PortableSecretStore.h"
#include "storage/WinCredStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecretStore, "matrix.secret.factory")

std::unique_ptr<SecretStore> SecretStore::createDefault(QObject *parent)
{
    if (lightning::portable::isPortable()) {
        auto portableStore = std::make_unique<PortableSecretStore>(
            lightning::portable::secretsDir(), parent);
        if (portableStore->isAvailable()) {
            qCInfo(lcSecretStore) << "using" << portableStore->backendName();
        } else {
            qCWarning(lcSecretStore)
                << "portable secret store unavailable:"
                << portableStore->lastError()
                << "-- refusing to fall back to a machine-bound store; the "
                   "saved sign-in is left untouched";
        }
        return portableStore;
    }

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
#ifdef HAVE_MAC_KEYCHAIN
    auto mackeychain = std::make_unique<MacKeychainStore>(parent);
    if (mackeychain->isAvailable()) {
        qCInfo(lcSecretStore) << "using" << mackeychain->backendName();
        return mackeychain;
    }
    qCWarning(lcSecretStore)
        << "macOS Keychain unavailable:"
        << mackeychain->lastError()
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
    // Substituted mode only when a native backend exists and failed: the tokens
    // may still be in libsecret or Credential Manager, so an empty read must
    // not read as "no saved sign-in" and arm the local reset (CLAUDE.md §6).
    // The fallback still reads and writes; it only stops claiming its misses
    // are authoritative.
#if defined(HAVE_WINCRED) || defined(HAVE_LIBSECRET) || defined(HAVE_MAC_KEYCHAIN)
    return std::make_unique<InsecureFallbackSecretStore>(
        parent,true);
#else
    qCInfo(lcSecretStore)
        << "no native secure store compiled in — using insecure QSettings fallback";
    return std::make_unique<InsecureFallbackSecretStore>(parent);
#endif
}
