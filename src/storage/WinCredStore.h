#pragma once

#include "storage/SecretStore.h"

// Windows Credential Manager backend for SecretStore, using the Win32
// Credential Management API (CredWriteW / CredReadW / CredDeleteW /
// CredEnumerateW via advapi32). The blob is DPAPI-protected per Windows user
// by the OS, so access tokens are no longer written to QSettings in plaintext.
//
// Only compiled with a real implementation when HAVE_WINCRED is defined
// (CMakeLists.txt sets it for WIN32 targets and links advapi32). On every
// other platform this compiles as a no-op stub reporting unavailable, so the
// factory falls through to libsecret / the insecure fallback exactly as before.
//
// Credentials are stored as CRED_TYPE_GENERIC under a deterministic target
// name "Lightning/secret/<userId>/<key>" (the Matrix user id is a public
// identifier; the token lives only in the encrypted CredentialBlob, never in
// the target name and never in a log). CredEnumerateW's trailing-wildcard
// filter powers clearAccountSecrets() for a single account on logout.
class WinCredStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit WinCredStore(QObject *parent = nullptr);
    ~WinCredStore() override;

    bool isSecure() const override { return true; }
    bool isAvailable() const override;
    QString backendName() const override;

    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override;
    QString readSecret(const QString &userId,
                       const QString &key) const override;
    bool deleteSecret(const QString &userId, const QString &key) override;
    bool clearAccountSecrets(const QString &userId) override;

    QString lastError() const override { return m_lastError; }

private:
    void setError(const QString &err) const { m_lastError = err; }

    bool m_available = false;
    mutable QString m_lastError;
};
