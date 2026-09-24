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

} // namespace matrix::room_action
