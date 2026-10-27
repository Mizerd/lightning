#include "matrix/MatrixClient.h"

#include <QCoreApplication>

// The interface's moc code lives here, plus the one piece of shared wording
// every client needs: the text for a redaction failure category.

QString MatrixClient::redactionFailureText(const QString &category)
{
    if (category == QLatin1String("rate_limited"))
        return QCoreApplication::translate(
            "MatrixClient",
            "The server is limiting how fast messages can be deleted. Try "
            "again in a little while.");
    if (category == QLatin1String("forbidden"))
        return QCoreApplication::translate(
            "MatrixClient",
            "You do not have permission to delete that message.");
    if (category == QLatin1String("not_found"))
        return QCoreApplication::translate(
            "MatrixClient", "That message no longer exists on the server.");
    if (category == QLatin1String("not_ready"))
        return QCoreApplication::translate(
            "MatrixClient",
            "This room is still loading. Try that again in a moment.");
    if (category == QLatin1String("network"))
        return QCoreApplication::translate(
            "MatrixClient",
            "The message could not be deleted. Check your connection and try "
            "again.");
    if (category == QLatin1String("unrecognized"))
        return QCoreApplication::translate(
            "MatrixClient", "Your server does not support deleting messages.");
    return QCoreApplication::translate("MatrixClient",
                                       "The message could not be deleted.");
}
