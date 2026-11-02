#pragma once

#include <QCoreApplication>
#include <QString>

namespace lightning::rooms {

inline QString membershipLostNotice(const QString &loss, const QString &roomName,
                                    bool wasOpen)
{
    // Spelled out at every call: lupdate expands no macro or helper.
    const bool named = !roomName.trimmed().isEmpty();
    if (loss == QLatin1String("kicked")) {
        return named ? QCoreApplication::translate(
                         "MembershipLostNotice", "You were removed from %1.").arg(roomName)
                     : QCoreApplication::translate(
                         "MembershipLostNotice", "You were removed from a room.");
    }
    if (loss == QLatin1String("banned")) {
        return named ? QCoreApplication::translate(
                         "MembershipLostNotice", "You were banned from %1.").arg(roomName)
                     : QCoreApplication::translate(
                         "MembershipLostNotice", "You were banned from a room.");
    }
    if (!wasOpen)
        return {};
    if (loss == QLatin1String("left")) {
        return named ? QCoreApplication::translate(
                         "MembershipLostNotice", "You left %1 from another session.").arg(roomName)
                     : QCoreApplication::translate(
                         "MembershipLostNotice", "You left this room from another session.");
    }
    return named ? QCoreApplication::translate(
                         "MembershipLostNotice", "You are no longer in %1.").arg(roomName)
                 : QCoreApplication::translate(
                         "MembershipLostNotice", "You are no longer in this room.");
}

}
