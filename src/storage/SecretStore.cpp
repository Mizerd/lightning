#include "storage/SecretStore.h"

#include "storage/InsecureFallbackSecretStore.h"
#include "storage/LibSecretStore.h"
#include "storage/PortableMode.h"
#include "storage/PortableSecretStore.h"
#include "storage/WinCredStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecretStore, "matrix.secret.factory")

std::unique_ptr<SecretStore> SecretStore::createDefault(QObject *parent)
{
    // Portable first, as a complete replacement of the choice. WinCred and
    // libsecret keep secrets outside the folder, so a copied folder could not
    // carry the session; the QSettings fallback would travel but stores tokens
    // in plain text.
    //
    // No fallback: if the portable store cannot read its directory it is
    // returned anyway with isAvailable() false, so no destructive decision is
    // taken. Falling through would silently write the session outside the
    // folder again.
    if (lightning::portable::isPortable()) {
        // The directory is injected (hermetic tests, relocation testable) and
        // comes from portable::secretsDir(), the single place naming it. An
        // empty answer is a contradiction reported as a failure, not a reason
        // to fall through.
        auto portableStore = std::make_unique<PortableSecretStore>(
            lightning::portable::secretsDir(), parent);
        if (portableStore->isAvailable()) {
            qCInfo(lcSecretStore) << "using" << portableStore->backendName();
        } else {
            // No path in the message: the data root reveals the folder's
            // location.
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
#if defined(HAVE_WINCRED) || defined(HAVE_LIBSECRET)
    return std::make_unique<InsecureFallbackSecretStore>(
        parent, /*substitutedForNative=*/true);
#else
    qCInfo(lcSecretStore)
        << "no native secure store compiled in — using insecure QSettings fallback";
    return std::make_unique<InsecureFallbackSecretStore>(parent);
#endif
}
