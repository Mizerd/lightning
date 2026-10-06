#include "storage/MacKeychainStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcMacKeychain, "matrix.secret.mackeychain")

#ifdef HAVE_MAC_KEYCHAIN

#include <QByteArray>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <utility>

namespace {

// Owns one CF object; releases it on scope exit.
template <typename T>
class CfRef
{
public:
    explicit CfRef(T ref = nullptr) : m_ref(ref) {}
    ~CfRef() { if (m_ref) CFRelease(m_ref); }
    CfRef(const CfRef &) = delete;
    CfRef &operator=(const CfRef &) = delete;
    CfRef(CfRef &&other) noexcept : m_ref(std::exchange(other.m_ref, nullptr)) {}
    CfRef &operator=(CfRef &&other) noexcept
    {
        if (this != &other) {
            if (m_ref) CFRelease(m_ref);
            m_ref = std::exchange(other.m_ref, nullptr);
        }
        return *this;
    }
    T get() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }
private:
    T m_ref;
};

CFStringRef makeCfString(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8 *>(utf8.constData()),
                                   static_cast<CFIndex>(utf8.size()),
                                   kCFStringEncodingUTF8, false);
}

QString serviceFor(const QString &userId)
{
    return QStringLiteral("Lightning/secret/%1").arg(userId);
}

QString osErr(OSStatus status)
{
    return QStringLiteral("Keychain error %1").arg(static_cast<int>(status));
}

// The query selecting one item, or every item of the account when `key` is
// empty.
CfRef<CFMutableDictionaryRef> baseQuery(const QString &userId, const QString &key)
{
    CfRef<CFMutableDictionaryRef> q(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    if (!q)
        return q;
    CfRef<CFStringRef> service(makeCfString(serviceFor(userId)));
    CFDictionarySetValue(q.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q.get(), kSecAttrService, service.get());
    if (!key.isEmpty()) {
        CfRef<CFStringRef> account(makeCfString(key));
        CFDictionarySetValue(q.get(), kSecAttrAccount, account.get());
    }
    return q;
}

} // namespace

MacKeychainStore::MacKeychainStore(QObject *parent) : SecretStore(parent)
{
    // The login keychain exists for any interactive macOS user. A locked or
    // unreachable one shows up per call, as a failed read, not here.
    m_available = true;
    qCInfo(lcMacKeychain) << "macOS Keychain secret store active";
}

MacKeychainStore::~MacKeychainStore() = default;

bool MacKeychainStore::isAvailable() const { return m_available; }

QString MacKeychainStore::backendName() const
{
    return QStringLiteral("macOS Keychain");
}

bool MacKeychainStore::storeSecret(const QString &userId,
                                   const QString &key,
                                   const QString &value)
{
    m_lastError.clear();
    const QByteArray blob = value.toUtf8();   // token bytes: never logged
    CfRef<CFDataRef> data(CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(blob.constData()),
        static_cast<CFIndex>(blob.size())));
    if (!data) {
        m_lastError = QStringLiteral("Keychain: out of memory");
        return false;
    }

    // Update in place first: a delete-then-add would lose the existing token
    // whenever the add fails (a denied prompt, a locked keychain). Only an item
    // that does not exist yet is added.
    {
        auto match = baseQuery(userId, key);
        CfRef<CFMutableDictionaryRef> changes(CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        if (!match || !changes) {
            m_lastError = QStringLiteral("Keychain: out of memory");
            return false;
        }
        CFDictionarySetValue(changes.get(), kSecValueData, data.get());
        const OSStatus up = SecItemUpdate(match.get(), changes.get());
        if (up == errSecSuccess)
            return true;
        if (up != errSecItemNotFound) {
            m_lastError = osErr(up);
            qCWarning(lcMacKeychain) << "SecItemUpdate failed:" << m_lastError;
            return false;
        }
    }

    auto attrs = baseQuery(userId, key);
    if (!attrs) {
        m_lastError = QStringLiteral("Keychain: out of memory");
        return false;
    }
    CFDictionarySetValue(attrs.get(), kSecValueData, data.get());
    const OSStatus st = SecItemAdd(attrs.get(), nullptr);
    if (st != errSecSuccess) {
        m_lastError = osErr(st);
        qCWarning(lcMacKeychain) << "SecItemAdd failed:" << m_lastError;
        return false;
    }
    return true;
}

QString MacKeychainStore::readSecret(const QString &userId, const QString &key) const
{
    m_lastError.clear();
    // Only a completed lookup, hit or miss, clears this. A locked keychain or a
    // denied access prompt must read as "cannot tell", never as "no saved
    // sign-in" (CLAUDE.md §6), which would arm the destructive local reset.
    m_lastReadFailed = false;

    auto q = baseQuery(userId, key);
    if (!q) {
        m_lastReadFailed = true;
        m_lastError = QStringLiteral("Keychain: out of memory");
        return {};
    }
    CFDictionarySetValue(q.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus st = SecItemCopyMatching(q.get(), &result);
    CfRef<CFTypeRef> owned(result);
    if (st == errSecItemNotFound)
        return {};
    if (st != errSecSuccess) {
        m_lastReadFailed = true;
        m_lastError = osErr(st);
        return {};
    }
    // A success that is not data is a keychain answer we do not understand, not
    // proof the item is absent: inconclusive, so no destructive decision may key
    // on it (CLAUDE.md section 6).
    if (!result || CFGetTypeID(result) != CFDataGetTypeID()) {
        m_lastReadFailed = true;
        m_lastError = QStringLiteral("Keychain: unexpected result type");
        return {};
    }
    const auto data = static_cast<CFDataRef>(result);
    return QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        static_cast<qsizetype>(CFDataGetLength(data)));
}

bool MacKeychainStore::deleteSecret(const QString &userId, const QString &key)
{
    m_lastError.clear();
    auto q = baseQuery(userId, key);
    if (!q) {
        m_lastError = QStringLiteral("Keychain: out of memory");
        return false;
    }
    const OSStatus st = SecItemDelete(q.get());
    if (st == errSecSuccess || st == errSecItemNotFound)
        return true;   // nothing to remove == success (matches libsecret)
    m_lastError = osErr(st);
    return false;
}

bool MacKeychainStore::clearAccountSecrets(const QString &userId)
{
    m_lastError.clear();
    // The service names the account, so leaving out the account attribute
    // selects every key of this user. kSecMatchLimitAll makes macOS delete all
    // of them rather than the first match.
    auto q = baseQuery(userId, QString());
    if (!q) {
        m_lastError = QStringLiteral("Keychain: out of memory");
        return false;
    }
    CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitAll);
    const OSStatus st = SecItemDelete(q.get());
    if (st == errSecSuccess || st == errSecItemNotFound)
        return true;
    m_lastError = osErr(st);
    return false;
}

#else // HAVE_MAC_KEYCHAIN

MacKeychainStore::MacKeychainStore(QObject *parent) : SecretStore(parent) {}
MacKeychainStore::~MacKeychainStore() = default;

bool MacKeychainStore::isAvailable() const { return false; }
QString MacKeychainStore::backendName() const
{
    return QStringLiteral("macOS Keychain (not compiled in)");
}
bool MacKeychainStore::storeSecret(const QString &, const QString &, const QString &) { return false; }
QString MacKeychainStore::readSecret(const QString &, const QString &) const { return {}; }
bool MacKeychainStore::deleteSecret(const QString &, const QString &) { return false; }
bool MacKeychainStore::clearAccountSecrets(const QString &) { return false; }

#endif // HAVE_MAC_KEYCHAIN
