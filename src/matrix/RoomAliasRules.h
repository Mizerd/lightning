#pragma once

#include <QChar>
#include <QCoreApplication>
#include <QList>
#include <QString>

// What this client accepts as a room address (alias) before sending one to the
// server: at room creation (the localpart only) and in Room information's
// published and alternative addresses (the full "#localpart:server").
//
// The spec's grammar is permissive (any code point but ':' and NUL in the
// localpart), and a homeserver may accept what it allows: Continuwuity
// registered "#bad name:localhs.test" and it resolves (break-it s03). The rule
// here is the one the reference clients enforce instead, because an address is
// something people type, paste and linkify: Element's RoomAliasField refuses
// whitespace, '#', ':' and ',' in the localpart, and Synapse refuses
// whitespace at /createRoom. Control and format characters are refused too:
// an address with a zero-width space or an RLO in it looks like a different
// one. The 255-byte limit is the spec's, over the whole alias.
//
// Header-only: the controllers that use it and their test targets compile it
// without another source.
namespace matrix::room_alias {

enum class Problem {
    None,
    Empty,          // nothing before the server (or nothing at all)
    Whitespace,     // a space, tab or line break anywhere
    Reserved,       // '#', ':' or ',' inside the localpart
    Invisible,      // a control or format character (zero-width, bidi, ...)
    MissingServer,  // a full alias without ":server"
    TooLong,        // the whole alias over 255 bytes of UTF-8
};

namespace detail {
inline Problem scan(const QString &text, bool localpart)
{
    for (const uint point : text.toUcs4()) {
        const char32_t c = char32_t(point);
        if (QChar::isSpace(c))
            return Problem::Whitespace;
        const auto category = QChar::category(c);
        if (category == QChar::Other_Control || category == QChar::Other_Format)
            return Problem::Invisible;
        if (localpart && (c == U'#' || c == U':' || c == U','))
            return Problem::Reserved;
    }
    return Problem::None;
}
} // namespace detail

// The localpart typed at room creation, where the server appends its own
// name. `serverName` (without ':') only sizes the length check; empty skips it.
inline Problem localpartProblem(const QString &localpart,
                                const QString &serverName = QString())
{
    if (localpart.isEmpty())
        return Problem::Empty;
    const Problem p = detail::scan(localpart, true);
    if (p != Problem::None)
        return p;
    // '#' + localpart + ':' + server.
    if (!serverName.isEmpty()
        && 2 + localpart.toUtf8().size() + serverName.toUtf8().size() > 255)
        return Problem::TooLong;
    return Problem::None;
}

// A complete "#localpart:server", as Room information sends it.
inline Problem aliasProblem(const QString &alias)
{
    if (!alias.startsWith(QLatin1Char('#')) || alias.size() < 2)
        return Problem::Empty;
    const qsizetype colon = alias.indexOf(QLatin1Char(':'));
    if (colon < 0 || colon + 1 >= alias.size())
        return Problem::MissingServer;
    const QString local = alias.mid(1, colon - 1);
    if (local.isEmpty())
        return Problem::Empty;
    Problem p = detail::scan(local, true);
    if (p != Problem::None)
        return p;
    // The server part may carry a port (":8448"), so only whitespace and
    // invisible characters are refused there.
    p = detail::scan(alias.mid(colon + 1), false);
    if (p != Problem::None)
        return p;
    if (alias.toUtf8().size() > 255)
        return Problem::TooLong;
    return Problem::None;
}

// The sentence to show for a problem; empty for None. One wording for both
// places an address is typed.
inline QString describe(Problem problem)
{
    switch (problem) {
    case Problem::None:
        return QString();
    case Problem::Empty:
        return QCoreApplication::translate(
            "RoomAlias", "The address needs a name before the server.");
    case Problem::Whitespace:
        return QCoreApplication::translate(
            "RoomAlias", "A room address cannot contain spaces.");
    case Problem::Reserved:
        return QCoreApplication::translate(
            "RoomAlias",
            "A room address cannot contain “#”, “:” or "
            "“,” in its name.");
    case Problem::Invisible:
        return QCoreApplication::translate(
            "RoomAlias",
            "A room address cannot contain invisible or control characters.");
    case Problem::MissingServer:
        return QCoreApplication::translate(
            "RoomAlias", "The address needs a server name after “:”.");
    case Problem::TooLong:
        return QCoreApplication::translate(
            "RoomAlias", "That room address is too long.");
    }
    return QString();
}

} // namespace matrix::room_alias
