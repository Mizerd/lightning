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
    // PORTABLE FIRST, and it is a hard branch — not a preference among
    // several backends but a complete replacement of the choice.
    //
    // Why ahead of HAVE_WINCRED: the Windows Credential Manager is exactly
    // what makes the "portable" ZIP not portable. Its entries are bound to
    // the Windows user account and the machine, they are not inside the
    // extracted folder, and copying the folder therefore cannot carry the
    // session — which is the reported defect this whole round exists to fix.
    // The same argument disqualifies libsecret (the login keyring is not in
    // the folder either) and the insecure QSettings fallback (in portable
    // mode QSettings is redirected to an INI inside the folder, so it would
    // technically travel, but it would put access tokens on disk in plain
    // text, which is strictly worse than the sealed file below).
    //
    // Note the deliberate absence of a fallback. If the portable store cannot
    // read its own directory it is returned ANYWAY, reporting isAvailable()
    // false, so every existing surface says "the secret backend cannot
    // answer" and no destructive decision is taken. Falling through to
    // WinCred/libsecret here would be the worst possible outcome: the app
    // would quietly start writing the session outside the folder again, which
    // looks like it works and silently un-does portability. Preserve data,
    // fail clearly, never leave portable mode by accident.
    if (lightning::portable::isPortable()) {
        // The directory is INJECTED rather than resolved inside the store, so
        // the store stays hermetically testable and the relocation property
        // (written under root A, read under root B) can be exercised
        // directly. It comes from portable::secretsDir() rather than being
        // concatenated here, so the one place that names the "secrets"
        // literal is the one place that also names it for cleanup.
        //
        // An empty answer here would mean isPortable() and secretsDir()
        // disagree, which is a contradiction rather than a reason to look
        // elsewhere: the store reports it as a failure and we still do not
        // fall through.
        auto portableStore = std::make_unique<PortableSecretStore>(
            lightning::portable::secretsDir(), parent);
        if (portableStore->isAvailable()) {
            qCInfo(lcSecretStore) << "using" << portableStore->backendName();
        } else {
            // No path in the message: a resolved data root contains the
            // extracted folder's location, and this category is not a place
            // to start printing them.
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
    // SUBSTITUTED ONLY WHEN A NATIVE BACKEND EXISTED AND FAILED, and this
    // distinction is the whole point. The portable branch above already
    // states the rule and follows it: a store that cannot answer is returned
    // ANYWAY, reporting that it cannot, so no destructive decision is taken.
    // The native branches did the opposite — they handed back a DIFFERENT
    // store that reports isAvailable() true and lastReadFailed() false. The
    // user's tokens were still in libsecret or Credential Manager, so every
    // read came back empty and the app concluded the account had no saved
    // sign-in, which arms `requireLocalReset`. §6: never treat "no readable
    // access token" as "no account".
    //
    // The fallback still reads and writes, so a machine with no keyring at
    // all keeps working; in substituted mode it only stops claiming its
    // misses are authoritative.
#if defined(HAVE_WINCRED) || defined(HAVE_LIBSECRET)
    return std::make_unique<InsecureFallbackSecretStore>(
        parent, /*substitutedForNative=*/true);
#else
    qCInfo(lcSecretStore)
        << "no native secure store compiled in — using insecure QSettings fallback";
    return std::make_unique<InsecureFallbackSecretStore>(parent);
#endif
}
