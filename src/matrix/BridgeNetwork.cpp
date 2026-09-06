#include "matrix/BridgeNetwork.h"

#include <QHash>
#include <QRegularExpression>
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

namespace {

// A chip's worth of a bridge's own text. Attacker-chosen, so bounded here
// too even though Rust already bounded it at 64: this function is also
// reachable from a backend that is not the Rust one, and the badge is a
// single line beside a room name.
constexpr int kChipChars = 24;

QString chipText(const QString &raw)
{
    QString text;
    text.reserve(raw.size());
    for (const QChar c : raw) {
        // Defence in depth. Rust strips these already; a second backend or a
        // future caller must not be able to put a bidi override in a chip.
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
        // QString::size() counts UTF-16 units, so a blind truncate can cut a
        // surrogate pair in half and leave an invalid string. Drop the
        // orphan; an emoji lost from an over-long chip costs nothing.
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
    // 1. A known protocol gets OUR label, always.
    const QString curated = labelForNetworkId(id);
    if (!curated.isEmpty())
        return { id, curated };
    // 2. Otherwise the bridge may name itself, once, in a chip.
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

// A remote id that reads as a phone number is worth showing: "+447791…"
// beats "WhatsApp contact". Digits with an optional leading '+', at least
// seven of them — anything shorter or mixed (usernames, UUIDs, base64-ish
// encodings) is machine identity and is not.
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

    // The SDK's hero rendering appends the membership count in English
    // ("Sim, and 2 others"). For a bridged 1:1 the extras are the ghost and
    // the bridge bot — plumbing, not people — so the suffix is noise by
    // construction. Only a bridged DM gets this surgery: the string shape is
    // an SDK implementation detail (exact-pinned matrix-sdk 0.18), and a
    // native room's "and 2 others" may be describing actual people.
    static const QRegularExpression heroSuffix(
        QStringLiteral(",? and \\d+ others?$"));
    name.remove(heroSuffix);

    // Whatever remains is either a human name (pass it through) or the
    // ghost id the algorithm degraded to. Both the full "@…:server" form
    // and the bare localpart appear in practice.
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
