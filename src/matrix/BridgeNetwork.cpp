#include "matrix/BridgeNetwork.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace matrix::bridge {
namespace {

// Canonical id -> display label. The id is the localpart prefix the bridge
// uses. Adding a bridge means adding one row.
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
        // Common prefix for self-hosted SMS bridges; harmless when none exists.
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
        // Not a bridge (e.g. "thomas_redstone").
        return {};
    }

    // No underscore: the only other accepted shape is the bridge bot.
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
    // The DM partner names an actual remote account; an alias can be an
    // artefact of how a portal was created.
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

// A chip's worth of attacker-chosen bridge text. Bounded here too (Rust caps
// it at 64) because non-Rust backends also reach this.
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
        // size() counts UTF-16 units, so a truncate can split a surrogate pair;
        // drop the orphan.
        if (!text.isEmpty() && text.back().isHighSurrogate())
            text.chop(1);
        text.append(QStringLiteral("\u2026"));
    }
    return text;
}

} // namespace

AdvertisedBridgeLabel labelForAdvertisedBridge(const QString &protocolId,
                                               const QString &protocolName,
                                               const QString &networkName)
{
    const QString id = protocolId.trimmed().toLower();
    // 1. A known protocol always gets our label.
    const QString curated = labelForNetworkId(id);
    if (!curated.isEmpty())
        return { id, curated };
    // 2. Otherwise the bridge may name itself, in a chip.
    QString own = chipText(protocolName);
    if (own.isEmpty())
        own = chipText(networkName);
    return { id, own };
}

namespace {

// "<network>_<remote id>" -> "<remote id>", empty when the localpart is not
// a recognised ghost (including the bare "<network>bot" shape, which has no
// remote id).
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

// A remote id that reads as a phone number ("+447791…") is worth showing:
// digits with an optional '+', at least seven. Anything else (usernames,
// UUIDs, encodings) is machine identity.
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

} // namespace

DmNamePresentation presentableDmName(const QString &computedName,
                                     const QString &directUserId)
{
    const QString network = networkIdForUserId(directUserId);
    if (network.isEmpty())
        return { computedName, {} };

    QString name = computedName.trimmed();

    // The SDK's hero name appends an English member count ("Sim, and 2
    // others"); in a bridged 1:1 the extras are the ghost and the bot, so strip
    // it. Bridged DMs only: the shape is an SDK detail (matrix-sdk 0.18,
    // exact-pinned), and in a native room the count may describe real people.
    static const QRegularExpression heroSuffix(
        QStringLiteral(",? and \\d+ others?$"));
    name.remove(heroSuffix);

    // What remains is a human name (pass through) or the ghost id naming
    // degraded to, in full "@…:server" or bare localpart form.
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

} // namespace matrix::bridge
