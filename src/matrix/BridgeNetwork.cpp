#include "matrix/BridgeNetwork.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace matrix::bridge {
namespace {

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
        { QStringLiteral("sms"),        QStringLiteral("SMS") },
    };
    return table;
}

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

// A bridge localpart is "<network>_<remote id>", optionally with a leading
// underscore (matrix-appservice-*: `_discord_1234`). The bridge bot is
// "<network>bot" with no remote id; its DM is where login and status live.
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
        return {};
    }

    if (localpart.endsWith(QLatin1String("bot"), Qt::CaseInsensitive)) {
        const QString candidate =
            localpart.left(localpart.size() - 3).toLower();
        if (networkTable().contains(candidate))
            return candidate;
    }
    return {};
}

}

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
    const QString fromUser = networkIdForUserId(directUserId);
    if (!fromUser.isEmpty())
        return fromUser;
    return networkIdForAlias(canonicalAlias);
}

QString labelForNetworkId(const QString &networkId)
{
    return networkTable().value(networkId.toLower());
}

namespace {

constexpr int kChipChars = 24;

QString chipText(const QString &raw)
{
    QString text;
    text.reserve(raw.size());
    for (const QChar c : raw) {
        // Defence in depth: no bidi overrides or controls in a chip, whatever
        // the backend.
        const char32_t u = c.unicode();
        if (c.isNull() || (c.category() == QChar::Other_Control))
            continue;
        if (u == 0x200E || u == 0x200F || (u >= 0x202A && u <= 0x202E)
            || (u >= 0x2066 && u <= 0x2069))
            continue;
        text.append(c);
    }
    text = text.simplified();
    if (text.size() > kChipChars) {
        text.truncate(kChipChars - 1);
        if (!text.isEmpty() && text.back().isHighSurrogate())
            text.chop(1);
        text.append(QStringLiteral("\u2026"));
    }
    return text;
}

}

AdvertisedBridgeLabel labelForAdvertisedBridge(const QString &protocolId,
                                               const QString &protocolName,
                                               const QString &networkName)
{
    const QString id = protocolId.trimmed().toLower();
    const QString curated = labelForNetworkId(id);
    if (!curated.isEmpty())
        return { id, curated };
    QString own = chipText(protocolName);
    if (own.isEmpty())
        own = chipText(networkName);
    return { id, own };
}

namespace {

QString ghostRemoteId(const QString &localpartIn)
{
    QString localpart = localpartIn;
    if (localpart.startsWith(QLatin1Char('_')))
        localpart = localpart.mid(1);
    const int underscore = localpart.indexOf(QLatin1Char('_'));
    if (underscore <= 0)
        return {};
    if (!networkTable().contains(localpart.left(underscore).toLower()))
        return {};
    return localpart.mid(underscore + 1);
}

bool readsAsPhoneNumber(const QString &remoteId)
{
    QString digits = remoteId;
    if (digits.startsWith(QLatin1Char('+')))
        digits = digits.mid(1);
    if (digits.size() < 7)
        return false;
    for (const QChar c : digits) {
        if (!c.isDigit())
            return false;
    }
    return true;
}

}

DmNamePresentation presentableDmName(const QString &computedName,
                                     const QString &directUserId)
{
    const QString network = networkIdForUserId(directUserId);
    if (network.isEmpty())
        return { computedName, {} };

    QString name = computedName.trimmed();

    static const QRegularExpression heroSuffix(
        QStringLiteral(",? and \\d+ others?$"));
    name.remove(heroSuffix);

    const QString asGhost = networkIdForUserId(name);
    if (!asGhost.isEmpty()) {
        const QString remote = ghostRemoteId(localpartOf(name, QLatin1Char('@')));
        if (readsAsPhoneNumber(remote))
            return { remote, {} };
        return { {}, labelForNetworkId(network) };
    }
    if (name.isEmpty())
        return { {}, labelForNetworkId(network) };
    return { name, {} };
}

}
