#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class SettingsManager;

// Settings > Account > Network: the ACTIVE account's network route
// (matrix/NetworkRoute.h, docs/network-profiles.md).
//
// QML sees kind, host, port, username and whether a password is stored. It
// never reads a password back: one goes in through save() and stays in the
// SecretStore. A saved route applies the next time the account connects
// (restart, or switching back to it); the session already open keeps the
// transport it was built with.
class NetworkRouteController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString userId READ userId NOTIFY changed)
    // False while no saved account is active (nothing to attach a route to).
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // {kind: "none"|"http"|"socks5", host, port, username, hasPassword}
    Q_PROPERTY(QVariantMap route READ route NOTIFY changed)

public:
    explicit NetworkRouteController(SettingsManager *settings, QObject *parent = nullptr);

    QString userId() const;
    bool available() const;
    QVariantMap route() const;

    // Stores the route for the active account. `kind` "none" removes it.
    // `replacePassword` false keeps the stored one (for the same username).
    // Returns "" or an error code (matrix::network_route::save()).
    Q_INVOKABLE QString save(const QString &kind, const QString &host, int port,
                             const QString &username, const QString &password,
                             bool replacePassword);
    Q_INVOKABLE QString clear();

Q_SIGNALS:
    void changed();

private:
    void refresh();

    SettingsManager *m_settings = nullptr;
    QVariantMap m_route;
    QString m_userId;
    bool m_available = false;
};
