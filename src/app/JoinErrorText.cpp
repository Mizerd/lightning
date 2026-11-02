#include "app/JoinErrorText.h"

#include <QCoreApplication>

QString joinerror::describe(const QString &category)
{
    if (category == QLatin1String("banned")) {
        return QCoreApplication::translate("RoomDiscoveryController",
                                           "You are banned from this room.");
    }
    if (category == QLatin1String("forbidden")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "This room is invite-only, or you do not have permission to "
            "join it.");
    }
    if (category == QLatin1String("restricted_denied")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "This room is restricted: joining requires membership of "
            "another room or space you are not in.");
    }
    if (category == QLatin1String("not_found")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "No room with that address could be found.");
    }
    if (category == QLatin1String("rate_limited")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "The server is rate limiting this action. Try again shortly.");
    }
    if (category == QLatin1String("invalid")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "That is not a valid room address or link.");
    }
    return QCoreApplication::translate(
        "RoomDiscoveryController",
        "Could not reach the server. Try again.");
}
