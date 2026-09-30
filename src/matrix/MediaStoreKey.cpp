#include "matrix/MediaStoreKey.h"

#include "storage/SecretStore.h"

#include <QRandomGenerator>

namespace matrix::media_store_key {

namespace {

// Exactly the encoding resolve() writes. Anything else is somebody else's
// value or a damaged one, and is left alone.
QByteArray decode(const QString &value)
{
    if (value.size() != kKeyBytes * 2)
        return {};
    for (const QChar c : value) {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                         || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        if (!hex)
            return {};
    }
    return QByteArray::fromHex(value.toLatin1());
}

void scrub(QByteArray &bytes)
{
    bytes.fill('\0');
    bytes.clear();
}

void scrub(QString &text)
{
    text.fill(QLatin1Char('0'));
    text.clear();
}

} // namespace

Resolution read(const SecretStore *store, const QString &userId)
{
    Resolution out;
    if (!store || userId.trimmed().isEmpty())
        return out;
    QString value = store->readSecret(userId, QLatin1String(kSecretName));
    // isAvailable() is a construction-time probe; lastReadFailed() is the
    // outcome of this read (a keyring that locked later, a dropped bus).
    if (!store->isAvailable() || store->lastReadFailed()) {
        scrub(value);
        return out;
    }
    if (value.isEmpty()) {
        // Only a store that can vouch for its misses proves there is none.
        out.found = store->missesAreInconclusive() ? Found::Unreadable
                                                   : Found::Absent;
        return out;
    }
    QByteArray key = decode(value);
    scrub(value);
    if (key.size() != kKeyBytes)
        return out; // malformed: never overwritten
    out.found = Found::Present;
    out.key = key;
    scrub(key);
    return out;
}

Resolution resolve(SecretStore *store, const QString &userId, bool mayCreate)
{
    Resolution out = read(store, userId);
    if (out.found != Found::Absent || !mayCreate)
        return out;

    // The second witness (see the header): the account's token is there.
    QString token = store->readSecret(userId, QLatin1String(kAccessTokenName));
    const bool tokenAnswered = !store->lastReadFailed() && !token.isEmpty();
    scrub(token);
    if (!tokenAnswered) {
        out.found = Found::Unreadable;
        return out;
    }

    QByteArray key(kKeyBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(key.begin(), key.end());
    QString encoded = QString::fromLatin1(key.toHex());
    const bool stored =
        store->storeSecret(userId, QLatin1String(kSecretName), encoded);
    // Used only once it is known to be there: a key that was not kept would
    // encrypt a store nothing can open at the next start.
    QString back = stored
        ? store->readSecret(userId, QLatin1String(kSecretName))
        : QString();
    const bool confirmed = stored && !store->lastReadFailed() && back == encoded;
    scrub(back);
    scrub(encoded);
    if (confirmed) {
        out.created = true;
        out.key = key;
    }
    scrub(key);
    return out;
}

const char *describe(const Resolution &resolution)
{
    if (resolution.created)
        return "created";
    switch (resolution.found) {
    case Found::Present:
        return "present";
    case Found::Absent:
        return "absent";
    case Found::Unreadable:
        break;
    }
    return "unreadable";
}

} // namespace matrix::media_store_key
