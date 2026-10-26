#include "app/NetworkRouteController.h"

#include "app/SettingsManager.h"
#include "matrix/NetworkRoute.h"

namespace route = matrix::network_route;

NetworkRouteController::NetworkRouteController(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    if (m_settings) {
        connect(m_settings, &SettingsManager::networkRouteChanged, this,
                &NetworkRouteController::refresh);
        connect(m_settings, &SettingsManager::sessionChanged, this,
                &NetworkRouteController::refresh);
        connect(m_settings, &SettingsManager::accountsChanged, this,
                &NetworkRouteController::refresh);
    }
    refresh();
}

QString NetworkRouteController::userId() const
{
    return m_settings ? m_settings->activeAccountUserId() : QString();
}

bool NetworkRouteController::available() const
{
    const QString uid = userId();
    return m_settings && !uid.isEmpty() && m_settings->hasSavedAccount(uid);
}

QVariantMap NetworkRouteController::route() const
{
    return m_route;
}

QString NetworkRouteController::save(const QString &kind, const QString &host, int port,
                                     const QString &username, const QString &password,
                                     bool replacePassword)
{
    if (!available())
        return QStringLiteral("no_account");
    route::Route r;
    r.kind = route::kindFromName(kind);
    r.host = host;
    r.port = port;
    r.username = username;
    const auto change = replacePassword ? route::PasswordChange::Replace
                                        : route::PasswordChange::Keep;
    const QString error = route::save(m_settings, m_settings->secretStore(), userId(), r,
                                      change, password);
    refresh();
    return error;
}

QString NetworkRouteController::clear()
{
    return save(QStringLiteral("none"), {}, 0, {}, {}, false);
}

void NetworkRouteController::refresh()
{
    const QVariantMap next = available()
        ? route::summary(m_settings, m_settings->secretStore(), userId())
        : QVariantMap{{QStringLiteral("kind"), QStringLiteral("none")}};
    const QString uid = userId();
    const bool isAvailable = available();
    if (next == m_route && uid == m_userId && isAvailable == m_available)
        return;
    m_route = next;
    m_userId = uid;
    m_available = isAvailable;
    Q_EMIT changed();
}
