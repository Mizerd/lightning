#pragma once

#include <QString>

// What to tell the user when the server refuses a room action they asked for.
//
// # Why this exists
//
// `rust/src/lib.rs` enqueues `room_action_error` for four actions, and the
// C++ handler swallowed all four with a `qCWarning` and returned. Three of
// them are rows in the room's context menu — Favourite, Mark as read, Mark as
// unread — so a refused write left the menu looking as though it had worked:
// the list simply did not change, which is indistinguishable from a slow
// sync. The notification flyout in that same menu has always reported its
// failures honestly, and that contrast is what makes this a defect rather
// than a deliberate policy.
//
// Found by the 2026-09-08 settings and menus audit (BUGS.md B021).
//
// # read_receipt is deliberately still silent
//
// Nobody asks for a read receipt. It is sent automatically as the reader
// moves through a room, the next one supersedes it, and a status strip
// saying "could not send a read receipt" would be noise about something that
// fixes itself. Silence there is a decision, not an oversight, and the empty
// string is how this function says so.
//
// # Pure, so the mapping is testable without a Rust handle
//
// The whole `RustSdkMatrixClient` translation unit needs the FFI, a tokio
// runtime and a live client to construct. This is a string mapping; it lives
// on its own so it can be tested for a few milliseconds instead.
namespace matrix::room_action {

/// The sentence to show for a refused room action, or an empty string for an
/// action the user never asked for (and for an action this does not know).
///
/// The three sentences are deliberately distinct: "could not change this
/// room's favourite status" and "could not mark this room as read" are
/// different failures and a reader who sees one should not have to guess
/// which control refused.
QString userFacingError(const QString &action);

} // namespace matrix::room_action
