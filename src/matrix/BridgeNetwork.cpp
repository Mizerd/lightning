#include "matrix/BridgeNetwork.h"

#include <QHash>
#include <QStringList>

namespace matrix::bridge {
namespace {

// Canonical id -> display label. The id is the localpart prefix the bridge
// actually uses; the label is what a human calls the network. Adding a
// bridge means adding one row.
const QHash<QString, QString> &networkTable()
{
    static const QHash<QString, QString> table = {
        { QStringLiteral("whatsapp"),   QStringLiteral("WhatsApp") },
        { QStringLiteral("telegram"),   QStringLiteral("Telegram") },
        { QStringLiteral("signal"),     QStringLiteral("Signal") },
        { QStringLiteral("discord"),    QStringLiteral("Discord") },
        { QStringLiteral("slack"),      QStringLiteral("Slack") },
        { QStringLiteral("instagram"),  QStringLiteral("Instagram") },
        { QStringLiteral("facebook"),   QStringLiteral("Messenger") },
        { QStringLiteral("messenger"),  QStringLiteral("Messenger") },
        { QStringLiteral("googlechat"), QStringLiteral("Google Chat") },
        { QStringLiteral("gmessages"),  QStringLiteral("Google Messages") },
        { QStringLiteral("gvoice"),     QStringLiteral("Google Voice") },
        { QStringLiteral("twitter"),    QStringLiteral("Twitter") },
        { QStringLiteral("imessage"),   QStringLiteral("iMessage") },
        { QStringLiteral("linkedin"),   QStringLiteral("LinkedIn") },
        { QStringLiteral("bluesky"),    QStringLiteral("Bluesky") },
        // Self-hosted SMS bridges commonly use this prefix; harmless when
        // no such bridge exists, since nothing will ever match it.
        { QStringLiteral("sms"),        QStringLiteral("SMS") },
    };
    return table;
}

// Pull the localpart out of "@local:server" / "#local:server". Tolerates a
// missing sigil and a missing server part.
QString localpartOf(const QString &identifier, QChar sigil)
{
    if (identifier.isEmpty())
        return {};
    QString s = identifier;
    if (s.startsWith(sigil))
        s = s.mid(1);
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon >= 0)
        s = s.left(colon);
    return s;
}

// The shared rule for both user ids and aliases.
//
// A bridge localpart is "<network>_<remote id>", optionally with a leading
// underscore (the matrix-appservice-* convention: `_discord_1234`). The bot
// account is "<network>bot" with no remote id at all, and it matters because
// the bridge bot's DM is where login and status live — the surface a user
// most needs labelled.
QString networkIdForLocalpart(const QString &localpartIn)
{
    QString localpart = localpartIn;
    if (localpart.isEmpty())
        return {};
    if (localpart.startsWith(QLatin1Char('_')))
        localpart = localpart.mid(1);

    const int underscore = localpart.indexOf(QLatin1Char('_'));
    if (underscore > 0) {
        const QString candidate = localpart.left(underscore).toLower();
        if (networkTable().contains(candidate))
            return candidate;
        // Fall through: a name like "thomas_redstone" is not a bridge, and
        // must not be reported as one.
        return {};
    }

    // No underscore: the only other shape we accept is the bridge bot.
    if (localpart.endsWith(QLatin1String("bot"), Qt::CaseInsensitive)) {
        const QString candidate =
            localpart.left(localpart.size() - 3).toLower();
        if (networkTable().contains(candidate))
            return candidate;
    }
    return {};
}

} // namespace

QString networkIdForUserId(const QString &userId)
{
    return networkIdForLocalpart(localpartOf(userId, QLatin1Char('@')));
}

QString networkIdForAlias(const QString &alias)
{
    return networkIdForLocalpart(localpartOf(alias, QLatin1Char('#')));
}

QString networkIdForRoom(const QString &directUserId,
                         const QString &canonicalAlias)
{
    // The DM partner is the stronger signal: it names an actual remote
    // account, whereas an alias can be an artefact of how a portal room was
    // created.
    const QString fromUser = networkIdForUserId(directUserId);
    if (!fromUser.isEmpty())
        return fromUser;
    return networkIdForAlias(canonicalAlias);
}

QString labelForNetworkId(const QString &networkId)
{
    return networkTable().value(networkId.toLower());
}

} // namespace matrix::bridge
