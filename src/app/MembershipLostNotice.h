#pragma once

// The status-strip line for a room the server confirmed we are no longer in
// (MatrixClient::roomMembershipLost, rust/src/membership_lost.rs). Header-only
// and pure so the wording is decided in one place.
//
// A kick or a ban is always said: the room leaves the list and the user did
// not ask for that. Our own leave from another session, or an exit whose
// cause the server did not show, is said only when the room was open, where
// the pane closing under the reader needs a reason; a list row that goes
// because the user left it elsewhere is what they did.

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

} // namespace lightning::rooms
