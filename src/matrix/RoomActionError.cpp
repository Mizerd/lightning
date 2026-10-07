#include "matrix/RoomActionError.h"

#include <QCoreApplication>

namespace matrix::room_action {

QString userFacingError(const QString &action)
{
    if (action == QLatin1String("favourite")) {
        return QCoreApplication::translate(
            "matrix::room_action",
            "Could not change this room's favourite status.");
    }
    if (action == QLatin1String("mark_read")) {
        return QCoreApplication::translate(
            "matrix::room_action", "Could not mark this room as read.");
    }
    if (action == QLatin1String("marked_unread")) {
        return QCoreApplication::translate(
            "matrix::room_action", "Could not change this room's unread mark.");
    }
    // read_receipt, and anything the table does not know yet: saying nothing
    // beats saying the wrong thing (see the header).
    return {};
}

QString failureHint(const QString &reason)
{
    if (reason == QLatin1String("timeout") || reason == QLatin1String("connect")
        || reason == QLatin1String("network")) {
        return QCoreApplication::translate(
            "matrix::room_action",
            "The server could not be reached. Check your connection and try again.");
    }
    // http_<status>_<errcode>. A 429 or 5xx reaches here only after the SDK's
    // own backoff gave up, so "busy" is the honest word.
    if (reason.startsWith(QLatin1String("http_429_"))
        || reason.startsWith(QLatin1String("http_5"))) {
        return QCoreApplication::translate(
            "matrix::room_action",
            "The server is busy or having trouble. Try again in a moment.");
    }
    if (reason == QLatin1String("refresh_failed")
        || reason.startsWith(QLatin1String("http_401_"))) {
        return QCoreApplication::translate(
            "matrix::room_action",
            "The server did not accept this session. Try again, and sign in "
            "again if it keeps happening.");
    }
    if (reason.startsWith(QLatin1String("http_403_"))) {
        return QCoreApplication::translate(
            "matrix::room_action", "The server refused the change.");
    }
    return {};
}

QString userFacingError(const QString &action, const QString &reason)
{
    const QString sentence = userFacingError(action);
    if (sentence.isEmpty())
        return {};
    const QString hint = failureHint(reason);
    return hint.isEmpty() ? sentence : sentence + QLatin1Char(' ') + hint;
}

} // namespace matrix::room_action
