#include "storage/WinCredStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcWinCred, "matrix.secret.wincred")

#ifdef HAVE_WINCRED

#include <QByteArray>

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

namespace {

// Deterministic target name: "Lightning/secret/<userId>/<key>". The Matrix
// user id is a public identifier; the secret itself lives only in the
// encrypted CredentialBlob. Never place a token in the target name.
std::wstring targetName(const QString &userId, const QString &key)
{
    return QStringLiteral("Lightning/secret/%1/%2")
        .arg(userId, key).toStdWString();
}

// Trailing-'*' prefix filter matching every key stored under one account.
std::wstring accountPrefixFilter(const QString &userId)
{
    return QStringLiteral("Lightning/secret/%1/*").arg(userId).toStdWString();
}

QString winErr(DWORD code)
{
    return QStringLiteral("Win32 error %1").arg(static_cast<qulonglong>(code));
}

} // namespace

WinCredStore::WinCredStore(QObject *parent) : SecretStore(parent)
{
    // Credential Manager is always present for an interactive Windows user.
    m_available = true;
    qCInfo(lcWinCred) << "Windows secure credential store active";
}

WinCredStore::~WinCredStore() = default;

bool WinCredStore::isAvailable() const { return m_available; }

QString WinCredStore::backendName() const
{
    return QStringLiteral("Windows Credential Manager");
}

bool WinCredStore::storeSecret(const QString &userId,
                               const QString &key,
                               const QString &value)
{
    m_lastError.clear();
    std::wstring target = targetName(userId, key);
    const QByteArray blob = value.toUtf8();   // token bytes — never logged

    CREDENTIALW cred;
    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target.data();
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!CredWriteW(&cred, 0)) {
        setError(winErr(GetLastError()));
        qCWarning(lcWinCred) << "CredWriteW failed:" << m_lastError;
        return false;
    }
    return true;
}

QString WinCredStore::readSecret(const QString &userId, const QString &key) const
{
    m_lastError.clear();
    std::wstring target = targetName(userId, key);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        const DWORD e = GetLastError();
        if (e != ERROR_NOT_FOUND)
            setError(winErr(e));
        return {};
    }
    QString out;
    if (cred && cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        out = QString::fromUtf8(
            reinterpret_cast<const char *>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize));
    }
    if (cred)
        CredFree(cred);
    return out;
}

bool WinCredStore::deleteSecret(const QString &userId, const QString &key)
{
    m_lastError.clear();
    std::wstring target = targetName(userId, key);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD e = GetLastError();
        if (e == ERROR_NOT_FOUND)
            return true;   // nothing to remove == success (matches libsecret)
        setError(winErr(e));
        return false;
    }
    return true;
}

bool WinCredStore::clearAccountSecrets(const QString &userId)
{
    m_lastError.clear();
    std::wstring filter = accountPrefixFilter(userId);
    DWORD count = 0;
    PCREDENTIALW *creds = nullptr;
    if (!CredEnumerateW(filter.c_str(), 0, &count, &creds)) {
        const DWORD e = GetLastError();
        if (e == ERROR_NOT_FOUND)
            return true;   // no secrets for this account
        setError(winErr(e));
        return false;
    }
    bool ok = true;
    for (DWORD i = 0; i < count; ++i) {
        if (creds[i] && creds[i]->TargetName
            && !CredDeleteW(creds[i]->TargetName, CRED_TYPE_GENERIC, 0)) {
            const DWORD e = GetLastError();
            if (e != ERROR_NOT_FOUND) {
                setError(winErr(e));
                ok = false;
            }
        }
    }
    if (creds)
        CredFree(creds);
    return ok;
}

#else // HAVE_WINCRED

WinCredStore::WinCredStore(QObject *parent) : SecretStore(parent) {}
WinCredStore::~WinCredStore() = default;

bool WinCredStore::isAvailable() const { return false; }
QString WinCredStore::backendName() const
{
    return QStringLiteral("Windows Credential Manager (not compiled in)");
}
bool WinCredStore::storeSecret(const QString &, const QString &, const QString &) { return false; }
QString WinCredStore::readSecret(const QString &, const QString &) const { return {}; }
bool WinCredStore::deleteSecret(const QString &, const QString &) { return false; }
bool WinCredStore::clearAccountSecrets(const QString &) { return false; }

#endif // HAVE_WINCRED
