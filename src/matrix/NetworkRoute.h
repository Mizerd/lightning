#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

class SecretStore;
class SettingsManager;

// One account's network route: an explicit HTTP (CONNECT) or SOCKS5 proxy for
// everything that account's Matrix client sends (docs/network-profiles.md,
// rust/src/route.rs).
//
// Custody (CLAUDE.md §6):
//   * kind, host, port and username are device-local settings in the
//     account's record (SettingsManager::networkRouteRecord); never synced,
//     never in diagnostics.
//   * the password is a secret in the account's SecretStore beside its
//     tokens, so clearAccountSecrets() (sign-out, removal) takes it with
//     them. It is never logged, never handed to QML (summary() reports only
//     whether one is stored), and leaves C++ only inside rustJson()'s bytes,
//     which the caller scrubs.
//
// Fail closed: rustJson() answers "unavailable" (and the Rust side then
// refuses to build the client) whenever a configured route cannot be used:
// a password that cannot be read, a record that does not validate. It never
// answers "none" for an account that has a route.
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

Kind kindFromName(const QString &name);       // "http", "socks5"; else None
QString kindName(Kind kind);                  // "none", "http", "socks5"

// Empty when valid, else an error code: "invalid_host", "invalid_port",
// "invalid_username". A None route is always valid.
QString validate(const Route &route);

Route fromRecord(const QVariantMap &record);
QVariantMap toRecord(const Route &route);

// The stored route; None when the account has none (or no record).
Route load(const SettingsManager *settings, const QString &userId);

// Validates and stores. The password is written first, then the record, so a
// failed keyring write stores nothing new. Error codes: those of validate(),
// "no_account", "password_required" (a username with no password to keep or
// set), "keyring_unavailable". Kind None clears both halves.
QString save(SettingsManager *settings, SecretStore *store, const QString &userId,
             const Route &route, PasswordChange change, const QString &password);

// For Settings: {kind, host, port, username, hasPassword}. hasPassword is
// read, never the value; an unreadable keyring reports false.
QVariantMap summary(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId);

// What mx_rust_set_network_route takes: {"state":"none"} for an account with
// no route (or none saved), {"state":"unavailable"} for a route that cannot
// be used now, else {"state":"configured",...}. SENSITIVE when it carries a
// password: the caller scrubs it.
QByteArray rustJson(const SettingsManager *settings, const SecretStore *store,
                    const QString &userId);

// The state rustJson() chose, for a log line: "none", "unavailable",
// "http" or "socks5". Never the host, never a credential.
QString describeJson(const QByteArray &json);

} // namespace matrix::network_route
