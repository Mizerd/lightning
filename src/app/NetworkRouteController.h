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
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QVariantMap route READ route NOTIFY changed)

public:
    explicit NetworkRouteController(SettingsManager *settings, QObject *parent = nullptr);

    QString userId() const;
    bool available() const;
    QVariantMap route() const;

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
