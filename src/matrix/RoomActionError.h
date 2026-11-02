#pragma once

#include <QString>

// What to tell the user when the server refuses a room action they asked
// for.
//
// rust/src/lib.rs enqueues `room_action_error` for four actions. Three are
// context-menu rows (Favourite, Mark as read, Mark as unread), and a silent
// refusal leaves the list unchanged, indistinguishable from a slow sync.
//
// read_receipt is deliberately silent: nobody asks for one, it is sent
// automatically and superseded by the next, so an error would be noise about
// something that fixes itself. The empty string says so.
//
// Pure so the mapping is testable without constructing RustSdkMatrixClient.
namespace matrix::room_action {

QString userFacingError(const QString &action);

QString userFacingError(const QString &action, const QString &reason);

QString failureHint(const QString &reason);

}
