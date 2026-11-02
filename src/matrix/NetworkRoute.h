#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

class SecretStore;
class SettingsManager;

namespace matrix::network_route {

inline constexpr char kSecretName[] = "networkRoutePassword";

enum class Kind { None, Http, Socks5 };

struct Route {
    Kind kind = Kind::None;
    QString host;
    int port = 0;
    QString username;
};

enum class PasswordChange { Keep, Replace, Clear };

Kind kindFromName(const QString &name);
QString kindName(Kind kind);

QString validate(const Route &route);

Route fromRecord(const QVariantMap &record);
QVariantMap toRecord(const Route &route);

Route load(const SettingsManager *settings, const QString &userId);

QString save(SettingsManager *settings, SecretStore *store, const QString &userId,
             const Route &route, PasswordChange change, const QString &password);

QVariantMap summary(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId);

// What mx_rust_set_network_route takes: {"state":"none"} for an account with
// no route (or none saved), {"state":"unavailable"} for a route that cannot
// be used now, else {"state":"configured",...}. SENSITIVE when it carries a
// password: the caller scrubs it.
QByteArray rustJson(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId);

QByteArray rustJsonForSignIn(const SettingsManager *settings, const SecretStore *store,
                             const QString &homeserver, const QString &userId = {});

QString serverKey(const QString &homeserverOrServerName);

QString describeJson(const QByteArray &json);

}
