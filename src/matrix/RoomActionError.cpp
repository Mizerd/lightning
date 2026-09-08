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
    // read_receipt, and anything a future Rust change adds before this table
    // learns about it. Saying nothing is better than saying the wrong thing,
    // and read_receipt is silent on purpose (see the header).
    return {};
}

} // namespace matrix::room_action
