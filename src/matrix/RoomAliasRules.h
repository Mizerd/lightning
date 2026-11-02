#pragma once

#include <QChar>
#include <QCoreApplication>
#include <QList>
#include <QString>

namespace matrix::room_alias {

enum class Problem {
    None,
    Empty,
    Whitespace,
    Reserved,
    Invisible,
    MissingServer,
    TooLong,
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
}

inline Problem localpartProblem(const QString &localpart,
                                const QString &serverName = QString())
{
    if (localpart.isEmpty())
        return Problem::Empty;
    const Problem p = detail::scan(localpart, true);
    if (p != Problem::None)
        return p;
    if (!serverName.isEmpty()
        && 2 + localpart.toUtf8().size() + serverName.toUtf8().size() > 255)
        return Problem::TooLong;
    return Problem::None;
}

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
    p = detail::scan(alias.mid(colon + 1), false);
    if (p != Problem::None)
        return p;
    if (alias.toUtf8().size() > 255)
        return Problem::TooLong;
    return Problem::None;
}

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

}
