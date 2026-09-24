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

/// The sentence for a refused room action, or empty for an action the user
/// never asked for (or one this does not know). The sentences are distinct so
/// the reader knows which control refused.
QString userFacingError(const QString &action);

} // namespace matrix::room_action
