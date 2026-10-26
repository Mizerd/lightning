#include "matrix/NetworkRoute.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "app/SettingsManager.h"
#include "storage/SecretStore.h"

namespace matrix::network_route {

namespace {

// rust/src/route.rs holds the same limits; it re-validates everything.
constexpr int kMaxHost = 253;
constexpr int kMaxUsername = 256;
constexpr int kMaxPassword = 1024;

bool hasControl(const QString &value)
{
    for (const QChar c : value) {
        if (c.category() == QChar::Other_Control)
            return true;
    }
    return false;
}

bool validHost(const QString &host)
{
    if (host.isEmpty() || host.size() > kMaxHost)
        return false;
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        const QString inner = host.mid(1, host.size() - 2);
        if (inner.isEmpty() || !inner.contains(QLatin1Char(':')))
            return false;
        for (const QChar c : inner) {
            if (!(c.isDigit() || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'))
                  || c == QLatin1Char(':') || c == QLatin1Char('.')))
                return false;
        }
        return true;
    }
    if (host.startsWith(QLatin1Char('-')) || host.startsWith(QLatin1Char('.')))
        return false;
    for (const QChar c : host) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                        || (u >= 'A' && u <= 'Z') || u == '.' || u == '-';
        if (!ok)
            return false;
    }
    return true;
}

// A bare IPv6 literal as typed ("::1") is bracketed, as a URL needs it.
QString normalizedHost(const QString &typed)
{
    const QString host = typed.trimmed();
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('[')))
        return QLatin1Char('[') + host + QLatin1Char(']');
    return host;
}

QByteArray stateOnly(const char *state)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("state"), QLatin1String(state)}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

Kind kindFromName(const QString &name)
{
    if (name == QLatin1String("http"))
        return Kind::Http;
    if (name == QLatin1String("socks5"))
        return Kind::Socks5;
    return Kind::None;
}

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Http:
        return QStringLiteral("http");
    case Kind::Socks5:
        return QStringLiteral("socks5");
    case Kind::None:
        break;
    }
    return QStringLiteral("none");
}

QString validate(const Route &route)
{
    if (route.kind == Kind::None)
        return {};
    if (!validHost(route.host))
        return QStringLiteral("invalid_host");
    if (route.port < 1 || route.port > 65535)
        return QStringLiteral("invalid_port");
    if (route.username.size() > kMaxUsername || hasControl(route.username))
        return QStringLiteral("invalid_username");
    return {};
}

Route fromRecord(const QVariantMap &record)
{
    Route route;
    route.kind = kindFromName(record.value(QStringLiteral("kind")).toString());
    route.host = normalizedHost(record.value(QStringLiteral("host")).toString());
    route.port = record.value(QStringLiteral("port")).toInt();
    route.username = record.value(QStringLiteral("username")).toString();
    return route;
}

QVariantMap toRecord(const Route &route)
{
    if (route.kind == Kind::None)
        return {};
    return {
        {QStringLiteral("kind"), kindName(route.kind)},
        {QStringLiteral("host"), route.host},
        {QStringLiteral("port"), route.port},
        {QStringLiteral("username"), route.username},
    };
}

Route load(const SettingsManager *settings, const QString &userId)
{
    if (!settings)
        return {};
    const QVariantMap record = settings->networkRouteRecord(userId);
    if (record.isEmpty())
        return {};
    // A stored kind this build does not know loads as None here, and
    // rustJson() still answers "unavailable" for it: the record exists.
    return fromRecord(record);
}

QString save(SettingsManager *settings, SecretStore *store, const QString &userId,
             const Route &input, PasswordChange change, const QString &password)
{
    if (!settings || !settings->hasSavedAccount(userId))
        return QStringLiteral("no_account");
    Route route = input;
    route.host = normalizedHost(route.host);
    route.username = route.username.trimmed();
    if (route.kind == Kind::None) {
        // Both halves go. The record first: a password left behind by a
        // failed delete is harmless without a route that names it.
        settings->setNetworkRouteRecord(userId, {});
        if (store)
            store->deleteSecret(userId, QLatin1String(kSecretName));
        return {};
    }
    if (const QString error = validate(route); !error.isEmpty())
        return error;
    if (route.username.isEmpty())
        change = PasswordChange::Clear;
    if (change == PasswordChange::Replace
        && (password.isEmpty() || password.size() > kMaxPassword || hasControl(password)))
        return QStringLiteral("password_required");
    if (change == PasswordChange::Keep) {
        // Keeping needs something to keep, for this same username.
        const Route stored = load(settings, userId);
        if (stored.username != route.username)
            return QStringLiteral("password_required");
        const QVariantMap now = summary(settings, store, userId);
        if (!now.value(QStringLiteral("hasPassword")).toBool())
            return QStringLiteral("password_required");
    }
    if (change != PasswordChange::Keep) {
        if (!store || !store->isAvailable())
            return QStringLiteral("keyring_unavailable");
        const bool ok = change == PasswordChange::Replace
            ? store->storeSecret(userId, QLatin1String(kSecretName), password)
            : (store->deleteSecret(userId, QLatin1String(kSecretName)), true);
        if (!ok)
            return QStringLiteral("keyring_unavailable");
    }
    if (!settings->setNetworkRouteRecord(userId, toRecord(route)))
        return QStringLiteral("no_account");
    return {};
}

QVariantMap summary(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId)
{
    const Route route = load(settings, userId);
    bool hasPassword = false;
    if (route.kind != Kind::None && !route.username.isEmpty() && store
        && store->isAvailable()) {
        QString value = store->readSecret(userId, QLatin1String(kSecretName));
        hasPassword = !store->lastReadFailed() && !value.isEmpty();
        value.fill(QLatin1Char('\0'));
    }
    return {
        {QStringLiteral("kind"), kindName(route.kind)},
        {QStringLiteral("host"), route.host},
        {QStringLiteral("port"), route.port},
        {QStringLiteral("username"), route.username},
        {QStringLiteral("hasPassword"), hasPassword},
    };
}

QByteArray rustJson(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId)
{
    if (!settings || userId.isEmpty() || !settings->networkRouteConfigured(userId))
        return stateOnly("none");
    const Route route = load(settings, userId);
    if (route.kind == Kind::None || !validate(route).isEmpty())
        return stateOnly("unavailable");
    QJsonObject out{
        {QStringLiteral("state"), QStringLiteral("configured")},
        {QStringLiteral("kind"), kindName(route.kind)},
        {QStringLiteral("host"), route.host},
        {QStringLiteral("port"), route.port},
    };
    if (!route.username.isEmpty()) {
        if (!store || !store->isAvailable())
            return stateOnly("unavailable");
        QString password = store->readSecret(userId, QLatin1String(kSecretName));
        if (store->lastReadFailed() || password.isEmpty()) {
            password.fill(QLatin1Char('\0'));
            return stateOnly("unavailable");
        }
        out.insert(QStringLiteral("username"), route.username);
        out.insert(QStringLiteral("password"), password);
        password.fill(QLatin1Char('\0'));
    }
    return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

QString serverKey(const QString &value)
{
    QString v = value.trimmed().toLower();
    const qsizetype scheme = v.indexOf(QLatin1String("://"));
    if (scheme >= 0)
        v = v.mid(scheme + 3);
    const qsizetype slash = v.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        v = v.left(slash);
    while (v.endsWith(QLatin1Char('.')))
        v.chop(1);
    return v;
}

QByteArray rustJsonForSignIn(const SettingsManager *settings, const SecretStore *store,
                             const QString &homeserver, const QString &userId)
{
    if (!settings)
        return stateOnly("none");
    if (!userId.isEmpty() && settings->hasSavedAccount(userId))
        return rustJson(settings, store, userId);
    const QString wanted = serverKey(homeserver);
    if (wanted.isEmpty())
        return stateOnly("none");
    QByteArray chosen;
    for (const QString &uid : settings->savedAccountUserIds()) {
        if (!settings->networkRouteConfigured(uid))
            continue;
        const QString recorded = serverKey(
            settings->accountRecord(uid).value(QStringLiteral("homeserver")).toString());
        const qsizetype colon = uid.indexOf(QLatin1Char(':'));
        const QString serverName = colon > 0 ? serverKey(uid.mid(colon + 1)) : QString();
        if (wanted != recorded && wanted != serverName)
            continue;
        QByteArray json = rustJson(settings, store, uid);
        if (chosen.isEmpty()) {
            chosen = json;
        } else if (chosen != json) {
            // Two routed accounts there disagree: which one is signing in is
            // not known yet, so neither route is guessed and none is skipped.
            json.fill('\0');
            chosen.fill('\0');
            return stateOnly("unavailable");
        }
        json.fill('\0');
    }
    return chosen.isEmpty() ? stateOnly("none") : chosen;
}

QString describeJson(const QByteArray &json)
{
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    const QString state = object.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("configured"))
        return object.value(QStringLiteral("kind")).toString();
    return state.isEmpty() ? QStringLiteral("unavailable") : state;
}

} // namespace matrix::network_route
