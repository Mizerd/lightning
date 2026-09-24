#pragma once

#include "storage/SecretStore.h"

// Windows Credential Manager backend (CredWriteW / CredReadW / CredDeleteW /
// CredEnumerateW via advapi32). Blobs are DPAPI-protected per Windows user.
//
// Implemented only with HAVE_WINCRED (set for WIN32 in CMakeLists.txt); other
// platforms get a stub that reports unavailable.
//
// Credentials are CRED_TYPE_GENERIC with target "Lightning/secret/<userId>/
// <key>". The user id is public; the token lives only in the encrypted blob,
// never in the target name or a log. CredEnumerateW's trailing wildcard
// drives clearAccountSecrets().
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

    // True when the last read could not be COMPLETED (as opposed to
    // completing and finding nothing). See readSecret().
    bool lastReadFailed() const override { return m_lastReadFailed; }
    QString lastError() const override { return m_lastError; }

private:
    void setError(const QString &err) const { m_lastError = err; }

    bool m_available = false;
    mutable QString m_lastError;
    mutable bool m_lastReadFailed = false;
};
