#include "models/UserLookup.h"

#include <QRegularExpression>

namespace matrix::user_lookup {

QString localpartOrUserId(const QString &userId)
{
    if (!userId.startsWith(QLatin1Char('@')))
        return userId;
    const QString localpart = userId.mid(1).section(QLatin1Char(':'), 0, 0);
    return localpart.isEmpty() ? userId : localpart;
}

QString serverNameFromUserId(const QString &userId)
{
    const int colon = userId.indexOf(QLatin1Char(':'));
    if (!userId.startsWith(QLatin1Char('@')) || colon < 0)
        return {};
    return userId.mid(colon + 1);
}

bool isValidLocalpart(const QString &localpart)
{
    static const QRegularExpression re(
        QStringLiteral("^[a-zA-Z0-9._=/+\\-]+$"));
    return !localpart.isEmpty() && re.match(localpart).hasMatch();
}

bool isValidServerName(const QString &serverName)
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9.\\-]+(?::\\d{1,5})?$"));
    return !serverName.isEmpty() && re.match(serverName).hasMatch();
}

QString exactCandidate(const QString &rawQuery, const QString &ownServerName)
{
    QString query = rawQuery.trimmed();
    if (query.startsWith(QLatin1Char('@')))
        query.remove(0, 1);
    if (query.isEmpty())
        return {};

    const int colon = query.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        const QString localpart = query.left(colon);
        const QString server = query.mid(colon + 1);
        if (!isValidLocalpart(localpart) || !isValidServerName(server))
            return {};
        return QLatin1Char('@') + localpart + QLatin1Char(':') + server;
    }

    if (!isValidLocalpart(query) || !isValidServerName(ownServerName))
        return {};
    return QLatin1Char('@') + query + QLatin1Char(':') + ownServerName;
}

bool queryNamesServer(const QString &rawQuery)
{
    QString query = rawQuery.trimmed();
    if (query.startsWith(QLatin1Char('@')))
        query.remove(0, 1);
    return query.contains(QLatin1Char(':'));
}

} // namespace matrix::user_lookup
