#include "storage/PortableSecretStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QLoggingCategory>
#include <QSaveFile>

// Only libcrypto, exactly as SignatureVerifier.cpp uses it. crypto.h is named
// explicitly for OPENSSL_cleanse rather than relied upon transitively through
// evp.h — the packaged builds compile against a different OpenSSL 3 minor than
// the dev shell, and an implicit include is precisely the kind of difference
// that shows up only in a CI job.
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#include <optional>

// Counts and reasons only. A key, a nonce, a token, a user id's secret value
// or any part of the plaintext document must never reach this category.
Q_LOGGING_CATEGORY(lcPortableSecret, "matrix.secret.portable")

namespace {

constexpr int kKeyBytes = 32;   // AES-256
constexpr int kNonceBytes = 12; // 96-bit GCM nonce, the only size that avoids
                                // the extra GHASH derivation step
constexpr int kTagBytes = 16;

constexpr int kFormatVersion = 1;

// Bounded reads. A truncated or garbage file must be refused, not slurped:
// these are our own small documents, so anything larger is already wrong.
constexpr qint64 kMaxKeyFileBytes = 4 * 1024;
constexpr qint64 kMaxDataFileBytes = 4 * 1024 * 1024;

// Additional authenticated data. Fixed, versioned, and deliberately free of
// any path or machine identifier: binding to either would defeat the entire
// point of a portable folder, which is that it can be renamed, moved to a
// different drive letter, and opened on a different PC. It exists only so the
// sealed blob cannot be substituted for some other Lightning file.
constexpr char kAad[] = "lightning-portable-secret-store-v1";

struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX *ctx) const noexcept
    {
        if (ctx)
            EVP_CIPHER_CTX_free(ctx);
    }
};
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;

// Best-effort hygiene, honestly labelled: it wipes the buffer we own. It says
// nothing about copies Qt's implicit sharing, the JSON parser, or the
// allocator may already have made. The same honesty applies here as to the
// UIA password scrubbing — transit hygiene, never a guarantee.
void cleanse(QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
}

std::optional<QByteArray> decodeBase64Strict(const QByteArray &text)
{
    const QByteArray::FromBase64Result result =
        QByteArray::fromBase64Encoding(text, QByteArray::Base64Encoding
                                           | QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return std::nullopt;
    return *result;
}

std::optional<QByteArray> randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    ERR_clear_error();
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), count) != 1) {
        // No fallback to a non-cryptographic generator. A nonce or key from a
        // predictable source is worse than refusing to write at all.
        ERR_clear_error();
        cleanse(out);
        return std::nullopt;
    }
    return out;
}

// Reads a whole small file, refusing anything over `limit`.
//
// THREE outcomes, and keeping them apart is the whole point of the signature:
//   * nullopt          — the file is there and could not be read. A failure.
//   * *absent == true  — no such file. An ordinary answer (a freshly extracted
//                        portable folder has neither of our two files).
//   * a QByteArray     — the bytes, which MAY be empty when the file itself is
//                        zero bytes long. That is NOT the same as absent: a
//                        zero-byte secrets.dat or secrets.key is a truncation,
//                        and the callers must treat it as damage. Collapsing
//                        the two — which the first draft of this function did
//                        by returning an empty QByteArray for a missing file —
//                        made a truncated key file look like "no key yet", and
//                        the mint path would then have written a NEW key over
//                        the only one that could open an existing secrets.dat.
//                        That is irreversible loss of the saved session and
//                        device, so the distinction is load-bearing, not tidy.
std::optional<QByteArray> readBoundedFile(const QString &path, qint64 limit,
                                          bool *absent, QString *error)
{
    if (absent)
        *absent = false;
    QFile file(path);
    if (!file.exists()) {
        if (absent)
            *absent = true;
        return QByteArray{};
    }
    if (file.size() > limit) {
        if (error)
            *error = QStringLiteral("%1 is larger than the allowed %2 bytes")
                         .arg(QFileInfo(path).fileName())
                         .arg(limit);
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1 could not be opened: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return std::nullopt;
    }
    return file.readAll();
}

// Owner-only where the filesystem has the concept at all. Deliberately not
// checked for success: FAT32/exFAT removable media — a completely normal home
// for a portable install — have no permission bits, and failing a sign-in
// because a USB stick cannot express file ownership would be absurd. The
// header states plainly that this is not enforceable everywhere.
void restrictToOwner(const QString &path)
{
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

bool writeFileAtomically(const QString &path, const QByteArray &bytes,
                         QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("%1 could not be written: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("%1 could not be written in full")
                         .arg(QFileInfo(path).fileName());
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("%1 could not be committed: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    // After commit, because QSaveFile writes through a temporary name and
    // renames on top. The window between rename and chmod is real but tiny,
    // and the containing directory is itself owner-only.
    restrictToOwner(path);
    return true;
}

// AES-256-GCM seal. `nonce` must be fresh for every call under one key.
std::optional<QByteArray> sealGcm(const QByteArray &key, const QByteArray &nonce,
                                  const QByteArray &plaintext, QByteArray *tagOut)
{
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes || !tagOut)
        return std::nullopt;

    ERR_clear_error();
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        ERR_clear_error();
        return std::nullopt;
    }

    const auto fail = []() -> std::optional<QByteArray> {
        ERR_clear_error();
        return std::nullopt;
    };

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail();
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) != 1)
        return fail();
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
        return fail();

    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char *>(kAad),
                          static_cast<int>(sizeof(kAad) - 1)) != 1)
        return fail();

    QByteArray out(plaintext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int written = 0;
    if (EVP_EncryptUpdate(ctx.get(),
                          reinterpret_cast<unsigned char *>(out.data()), &written,
                          reinterpret_cast<const unsigned char *>(plaintext.constData()),
                          static_cast<int>(plaintext.size())) != 1)
        return fail();
    int total = written;
    if (EVP_EncryptFinal_ex(ctx.get(),
                            reinterpret_cast<unsigned char *>(out.data()) + total,
                            &written) != 1)
        return fail();
    total += written;
    out.resize(total);

    QByteArray tag(kTagBytes, Qt::Uninitialized);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagBytes,
                            tag.data()) != 1)
        return fail();

    ERR_clear_error();
    *tagOut = tag;
    return out;
}

// AES-256-GCM open. Returns nullopt when the tag does not authenticate, which
// is the case that matters: a single flipped byte anywhere in the ciphertext,
// the nonce, or the AAD lands here rather than producing plausible garbage.
std::optional<QByteArray> openGcm(const QByteArray &key, const QByteArray &nonce,
                                  const QByteArray &ciphertext, const QByteArray &tag)
{
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes
        || tag.size() != kTagBytes)
        return std::nullopt;

    ERR_clear_error();
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        ERR_clear_error();
        return std::nullopt;
    }

    const auto fail = []() -> std::optional<QByteArray> {
        ERR_clear_error();
        return std::nullopt;
    };

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return fail();
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) != 1)
        return fail();
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
        return fail();

    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char *>(kAad),
                          static_cast<int>(sizeof(kAad) - 1)) != 1)
        return fail();

    QByteArray out(ciphertext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int written = 0;
    if (EVP_DecryptUpdate(ctx.get(),
                          reinterpret_cast<unsigned char *>(out.data()), &written,
                          reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                          static_cast<int>(ciphertext.size())) != 1) {
        cleanse(out);
        return fail();
    }
    int total = written;

    QByteArray mutableTag = tag;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagBytes,
                            mutableTag.data()) != 1) {
        cleanse(out);
        return fail();
    }

    const int result = EVP_DecryptFinal_ex(
        ctx.get(), reinterpret_cast<unsigned char *>(out.data()) + total, &written);
    ERR_clear_error();
    if (result != 1) {
        // Tag mismatch: the document is corrupt, truncated, or was sealed
        // with a different key. Never return the partial plaintext.
        cleanse(out);
        return std::nullopt;
    }
    total += written;
    out.resize(total);
    return out;
}

} // namespace

QString PortableSecretStore::dataFileName()
{
    return QStringLiteral("secrets.dat");
}

QString PortableSecretStore::keyFileName()
{
    return QStringLiteral("secrets.key");
}

QString PortableSecretStore::mapKey(const QString &userId, const QString &key)
{
    // Same separator as InMemorySecretStore: US (0x1f) cannot occur in a
    // Matrix user id or in any key this project stores.
    return userId + QLatin1Char('\x1f') + key;
}

PortableSecretStore::PortableSecretStore(const QString &secretsDir, QObject *parent)
    : SecretStore(parent)
    , m_dir(secretsDir)
{
    load();
    if (m_state == State::Ok) {
        qCInfo(lcPortableSecret)
            << "portable secret store ready —" << m_secrets.size()
            << "secret(s) loaded. The key travels with the data: anyone with "
               "this folder has the saved session.";
    } else {
        qCWarning(lcPortableSecret)
            << "portable secret store UNAVAILABLE:" << m_lastError
            << "— the existing file is left untouched; nothing will be "
               "written over it.";
    }
}

PortableSecretStore::~PortableSecretStore()
{
    cleanse(m_key);
}

QString PortableSecretStore::backendName() const
{
    // Shown verbatim in Settings → Privacy & security. It must not read as a
    // reassurance: the key being in the folder is the headline fact.
    return QStringLiteral(
        "portable file (AES-256-GCM, key stored in the same folder — "
        "anyone with the folder has the session)");
}

void PortableSecretStore::load()
{
    m_state = State::Ok;
    m_lastError.clear();
    m_secrets.clear();

    if (m_dir.isEmpty()) {
        m_state = State::Failed;
        setError(QStringLiteral("no portable secrets directory was resolved"));
        return;
    }

    QDir dir(m_dir);
    if (!dir.exists() && !QDir().mkpath(m_dir)) {
        m_state = State::Failed;
        setError(QStringLiteral("the secrets directory could not be created"));
        return;
    }
    // Owner-only plus traverse. Same caveat as the files: FAT/exFAT ignores it.
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner);

    const QString dataPath = dir.filePath(dataFileName());
    const QString keyPath = dir.filePath(keyFileName());

    QString readError;
    bool dataAbsent = false;
    const std::optional<QByteArray> dataBytes =
        readBoundedFile(dataPath, kMaxDataFileBytes, &dataAbsent, &readError);
    if (!dataBytes) {
        m_state = State::Failed;
        setError(readError);
        return;
    }
    if (dataAbsent) {
        // No document yet. A freshly extracted portable folder looks exactly
        // like this, and it is a real answer, not a failure. The key file is
        // read (if one exists) so a later write reuses it rather than minting
        // a second one; it is NOT minted here, so merely constructing the
        // store on a read-only medium does not fail.
        if (QFile::exists(keyPath) && !ensureKey()) {
            m_state = State::Failed;
            // ensureKey() already set the reason.
            return;
        }
        return;
    }
    if (dataBytes->isEmpty()) {
        // Present and zero bytes. Something truncated it — an interrupted copy
        // of the folder, a full disk, a filesystem that lost the tail. It is
        // NOT "nothing stored yet", and treating it as such would let the next
        // sign-in write a fresh document over the wreckage and destroy any
        // chance of recovering the old device from a backup of the folder.
        m_state = State::Failed;
        setError(QStringLiteral(
            "%1 is empty — it was truncated. It has been left untouched; "
            "restore the whole folder from a backup, or delete %1 and %2 "
            "together to start over with a new sign-in.")
                     .arg(dataFileName(), keyFileName()));
        return;
    }

    if (!QFile::exists(keyPath)) {
        // The one case that must never be mistaken for "no account": there IS
        // a saved session here, and its key is gone. Refusing keeps the
        // ciphertext intact for whoever can restore the key file; starting
        // over would overwrite the only copy on the next sign-in.
        m_state = State::Failed;
        setError(QStringLiteral(
            "the saved sign-in exists but its key file (%1) is missing — "
            "restore it from the same folder; nothing will be overwritten")
                     .arg(keyFileName()));
        return;
    }
    if (!ensureKey()) {
        m_state = State::Failed;
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument envelope = QJsonDocument::fromJson(*dataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !envelope.isObject()) {
        m_state = State::Failed;
        setError(QStringLiteral("%1 is not a valid sealed document")
                     .arg(dataFileName()));
        return;
    }
    const QJsonObject object = envelope.object();
    if (object.value(QLatin1String("version")).toInt() != kFormatVersion
        || object.value(QLatin1String("alg")).toString()
               != QLatin1String("aes-256-gcm")) {
        // No format agility: exactly one version and one algorithm are
        // accepted, so a rewritten header cannot negotiate anything weaker.
        m_state = State::Failed;
        setError(QStringLiteral("%1 was written by an unsupported version")
                     .arg(dataFileName()));
        return;
    }

    // The three sealed fields must be present, be strings, decode strictly,
    // and carry EXACTLY the sizes this format defines. Checking the sizes here
    // rather than leaving them to openGcm() is what makes a mangled envelope
    // report itself as malformed instead of as an authentication failure —
    // both refuse, but only one of the two messages tells the user the truth
    // about what happened to their folder.
    const QJsonValue nonceValue = object.value(QLatin1String("nonce"));
    const QJsonValue ctValue = object.value(QLatin1String("ct"));
    const QJsonValue tagValue = object.value(QLatin1String("tag"));
    const std::optional<QByteArray> nonce =
        nonceValue.isString() ? decodeBase64Strict(nonceValue.toString().toLatin1())
                              : std::nullopt;
    const std::optional<QByteArray> ciphertext =
        ctValue.isString() ? decodeBase64Strict(ctValue.toString().toLatin1())
                           : std::nullopt;
    const std::optional<QByteArray> tag =
        tagValue.isString() ? decodeBase64Strict(tagValue.toString().toLatin1())
                            : std::nullopt;
    if (!nonce || !ciphertext || !tag || nonce->size() != kNonceBytes
        || tag->size() != kTagBytes || ciphertext->isEmpty()) {
        m_state = State::Failed;
        setError(QStringLiteral(
            "%1 is malformed — it is damaged or was not written by Lightning. "
            "It has been left untouched.")
                     .arg(dataFileName()));
        return;
    }

    std::optional<QByteArray> plaintext = openGcm(m_key, *nonce, *ciphertext, *tag);
    if (!plaintext) {
        m_state = State::Failed;
        setError(QStringLiteral(
            "the saved sign-in in %1 could not be opened — the file is "
            "damaged or does not belong to %2. It has been left untouched.")
                     .arg(dataFileName(), keyFileName()));
        return;
    }

    QJsonParseError innerError{};
    const QJsonDocument inner = QJsonDocument::fromJson(*plaintext, &innerError);
    cleanse(*plaintext);
    if (innerError.error != QJsonParseError::NoError || !inner.isObject()) {
        m_state = State::Failed;
        setError(QStringLiteral("the saved sign-in in %1 is not readable")
                     .arg(dataFileName()));
        return;
    }

    const QJsonObject accounts =
        inner.object().value(QLatin1String("accounts")).toObject();
    for (auto account = accounts.constBegin(); account != accounts.constEnd();
         ++account) {
        const QJsonObject entries = account.value().toObject();
        for (auto entry = entries.constBegin(); entry != entries.constEnd();
             ++entry) {
            if (!entry.value().isString())
                continue;
            m_secrets.insert(mapKey(account.key(), entry.key()),
                             entry.value().toString());
        }
    }
}

bool PortableSecretStore::ensureKey()
{
    if (m_key.size() == kKeyBytes)
        return true;

    const QString keyPath = QDir(m_dir).filePath(keyFileName());

    QString readError;
    bool keyAbsent = false;
    const std::optional<QByteArray> raw =
        readBoundedFile(keyPath, kMaxKeyFileBytes, &keyAbsent, &readError);
    if (!raw) {
        setError(readError);
        return false;
    }

    if (!keyAbsent) {
        if (raw->isEmpty()) {
            // Present and zero bytes. Refuse — see readBoundedFile. Minting
            // here would overwrite the only key that can open an existing
            // secrets.dat, which is unrecoverable.
            setError(QStringLiteral(
                "%1 is empty — it was truncated. It has been left untouched; "
                "restore it from a backup of this folder.")
                         .arg(keyFileName()));
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(*raw, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setError(QStringLiteral("%1 is not a valid key file")
                         .arg(keyFileName()));
            return false;
        }
        const QJsonObject object = document.object();
        if (object.value(QLatin1String("version")).toInt() != kFormatVersion
            || object.value(QLatin1String("alg")).toString()
                   != QLatin1String("aes-256-gcm")) {
            setError(QStringLiteral("%1 was written by an unsupported version")
                         .arg(keyFileName()));
            return false;
        }
        std::optional<QByteArray> key = decodeBase64Strict(
            object.value(QLatin1String("key")).toString().toLatin1());
        if (!key || key->size() != kKeyBytes) {
            setError(QStringLiteral("%1 does not contain a usable key")
                         .arg(keyFileName()));
            return false;
        }
        m_key = *key;
        return true;
    }

    // No key FILE AT ALL — mint one. Reachable only when the key file is
    // genuinely absent (a present-but-unreadable one returned false above),
    // and load() refuses outright when a sealed document exists without its
    // key, so this can never write over a key an existing session depends on.
    std::optional<QByteArray> minted = randomBytes(kKeyBytes);
    if (!minted) {
        setError(QStringLiteral(
            "the system random number generator refused to produce a key"));
        return false;
    }

    QJsonObject object;
    object.insert(QLatin1String("version"), kFormatVersion);
    object.insert(QLatin1String("alg"), QLatin1String("aes-256-gcm"));
    object.insert(QLatin1String("key"),
                  QString::fromLatin1(minted->toBase64()));
    // A note to whoever opens this file wondering what it is. It says what it
    // protects and what it does not; it must never claim more.
    object.insert(QLatin1String("note"),
                  QLatin1String("Lightning portable secret key. It decrypts "
                                "secrets.dat in this same folder: anyone with "
                                "this folder has the saved sign-in. Keep the "
                                "folder private; do not share or publish it."));

    QByteArray serialized = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QString writeError;
    const bool written = writeFileAtomically(QDir(m_dir).filePath(keyFileName()),
                                             serialized, &writeError);
    cleanse(serialized);
    if (!written) {
        cleanse(*minted);
        setError(writeError);
        return false;
    }

    m_key = *minted;
    return true;
}

bool PortableSecretStore::persist()
{
    if (m_state != State::Ok) {
        // Refuse rather than start a fresh document over a damaged one. See
        // the failure semantics in the header: this is data preservation, and
        // it is why a corrupt store surfaces as a hard error instead of a
        // silent new login.
        return false;
    }
    if (!ensureKey())
        return false;

    QJsonObject accounts;
    for (auto it = m_secrets.constBegin(); it != m_secrets.constEnd(); ++it) {
        const int separator = it.key().indexOf(QLatin1Char('\x1f'));
        if (separator < 0)
            continue;
        const QString userId = it.key().left(separator);
        const QString key = it.key().mid(separator + 1);
        QJsonObject entries = accounts.value(userId).toObject();
        entries.insert(key, it.value());
        accounts.insert(userId, entries);
    }
    QJsonObject inner;
    inner.insert(QLatin1String("version"), kFormatVersion);
    inner.insert(QLatin1String("accounts"), accounts);

    QByteArray plaintext = QJsonDocument(inner).toJson(QJsonDocument::Compact);

    // A FRESH nonce on every single write. Reusing one under the same key is
    // the one mistake that breaks GCM outright, so it is generated here and
    // never derived from a counter, a timestamp, or the file contents.
    const std::optional<QByteArray> nonce = randomBytes(kNonceBytes);
    if (!nonce) {
        cleanse(plaintext);
        setError(QStringLiteral(
            "the system random number generator refused to produce a nonce"));
        return false;
    }

    QByteArray tag;
    const std::optional<QByteArray> ciphertext =
        sealGcm(m_key, *nonce, plaintext, &tag);
    cleanse(plaintext);
    if (!ciphertext) {
        setError(QStringLiteral("the saved sign-in could not be encrypted"));
        return false;
    }

    QJsonObject envelope;
    envelope.insert(QLatin1String("version"), kFormatVersion);
    envelope.insert(QLatin1String("alg"), QLatin1String("aes-256-gcm"));
    envelope.insert(QLatin1String("nonce"),
                    QString::fromLatin1(nonce->toBase64()));
    envelope.insert(QLatin1String("ct"),
                    QString::fromLatin1(ciphertext->toBase64()));
    envelope.insert(QLatin1String("tag"), QString::fromLatin1(tag.toBase64()));

    QString writeError;
    if (!writeFileAtomically(QDir(m_dir).filePath(dataFileName()),
                             QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                             &writeError)) {
        setError(writeError);
        return false;
    }
    m_lastError.clear();
    return true;
}

bool PortableSecretStore::storeSecret(const QString &userId, const QString &key,
                                      const QString &value)
{
    if (m_state != State::Ok) {
        qCWarning(lcPortableSecret)
            << "refusing to write: the portable secret store is unreadable —"
            << m_lastError;
        return false;
    }
    const QString mapped = mapKey(userId, key);
    const QString previous = m_secrets.value(mapped);
    const bool existed = m_secrets.contains(mapped);
    m_secrets.insert(mapped, value);
    if (!persist()) {
        // Roll the in-memory view back so it never claims something the disk
        // does not hold. A caller that saw true from a failed write would
        // believe the session survives a restart when it does not.
        if (existed)
            m_secrets.insert(mapped, previous);
        else
            m_secrets.remove(mapped);
        return false;
    }
    return true;
}

QString PortableSecretStore::readSecret(const QString &userId,
                                        const QString &key) const
{
    if (m_state != State::Ok) {
        // Empty AND flagged. §6: an unreadable secret is not an absent one,
        // and this flag is what stops SettingsManager reading the empty
        // string as "this account has no saved sign-in".
        m_lastReadFailed = true;
        return {};
    }
    m_lastReadFailed = false;
    return m_secrets.value(mapKey(userId, key));
}

bool PortableSecretStore::deleteSecret(const QString &userId, const QString &key)
{
    if (m_state != State::Ok) {
        qCWarning(lcPortableSecret)
            << "refusing to delete: the portable secret store is unreadable —"
            << m_lastError;
        return false;
    }
    const QString mapped = mapKey(userId, key);
    if (!m_secrets.contains(mapped)) {
        // Absent target. Report honestly rather than claiming a removal that
        // did not happen — "target absent" and "removed" are different
        // outcomes, and conflating them hides a no-op behind a success. The
        // reason is stated too, so a caller logging lastError() on the false
        // does not print an empty string and read it as an unexplained
        // backend failure. Matches InMemorySecretStore, which likewise
        // returns whether anything was actually removed.
        setError(QStringLiteral(
            "no secret was stored under that key — nothing was deleted"));
        return false;
    }
    const QString previous = m_secrets.take(mapped);
    if (!persist()) {
        m_secrets.insert(mapped, previous);
        return false;
    }
    return true;
}

bool PortableSecretStore::clearAccountSecrets(const QString &userId)
{
    if (m_state != State::Ok) {
        qCWarning(lcPortableSecret)
            << "refusing to clear: the portable secret store is unreadable —"
            << m_lastError;
        return false;
    }
    const QString prefix = userId + QLatin1Char('\x1f');
    const QHash<QString, QString> snapshot = m_secrets;
    for (auto it = m_secrets.begin(); it != m_secrets.end();) {
        if (it.key().startsWith(prefix))
            it = m_secrets.erase(it);
        else
            ++it;
    }
    if (m_secrets.size() == snapshot.size()) {
        // Nothing was bound to this account. Sign-out still succeeds, but the
        // rewrite is pointless, so skip it and say so honestly.
        m_lastError.clear();
        return true;
    }
    if (!persist()) {
        m_secrets = snapshot;
        return false;
    }
    return true;
}
