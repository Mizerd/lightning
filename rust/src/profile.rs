//! The signed-in account's OWN profile (v0.7.4).
//!
//! Display-name writes are `Account::set_display_name`, the SDK's own wrapper
//! over the profile endpoints (`PUT .../profile/{user}/displayname`, or the
//! newer delete-profile-field request when the server advertises it and the
//! name is being cleared). Lightning implements no profile request of its own
//! here; this module contributes exactly three things the SDK does not: a
//! length bound, the session-generation guard every async command in this
//! bridge carries, and a sanitized result payload.
//!
//! The name itself is text the user typed. It is never logged, and it never
//! comes BACK across the FFI on the result: C++ already holds the string it
//! submitted, so echoing it would only widen the surface that can leak it.

use std::sync::Arc;

use matrix_sdk::ruma::api::error::ErrorBody;
use serde_json::json;

use crate::rooms::require_client;
use crate::{enqueue, RustClient};

/// The profile write rides the room-action pool, which sign-out joins.
/// Bounded so a hung request degrades to a reported failure rather than
/// stalling the account teardown behind it.
const DISPLAY_NAME_REQUEST_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(15);

/// Matrix does not specify a maximum display-name length and servers differ,
/// so 255 is a client-side ceiling, not a protocol one.
const DISPLAY_NAME_MAX_CHARS: usize = 255;

/// Bound a display name at [`DISPLAY_NAME_MAX_CHARS`] Unicode scalar values.
///
/// Deliberately `chars()`, never a byte slice: `&name[..255]` panics on a
/// multi-byte boundary and, where it does not, hands the server a string cut
/// through the middle of a UTF-8 sequence. Scalar values are also the right
/// unit for the C++ side to reason about — a UTF-16 code-unit bound would cut
/// an emoji in half between its surrogates.
///
/// This can still split a grapheme CLUSTER (a base character from its
/// combining marks, or a ZWJ sequence at the joiner). That is accepted and
/// stated rather than fixed with a segmentation crate: no such crate is in
/// this offline `--locked` build's dependency set, the result is always valid
/// UTF-8 and always a valid Matrix display name, and a 255-character name is
/// already far past anything a server or a UI will render whole.
pub(crate) fn bound_display_name(name: &str) -> String {
    name.chars().take(DISPLAY_NAME_MAX_CHARS).collect()
}

/// Collapse control characters and whitespace runs, then bound the length.
///
/// The message is server-authored, but it lands in a UI label, so it is
/// treated like any other remote string: no newlines, no control bytes, no
/// unbounded length.
pub(crate) fn sanitize_error_message(message: &str) -> String {
    let collapsed: String = message
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    collapsed
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(200)
        .collect()
}

/// The server's own human-readable sentence for a refusal, if it sent one.
///
/// Only `StandardErrorBody.message` is taken — the one field the spec defines
/// as prose meant for a person. The error's `Display` is deliberately NOT a
/// fallback: it can carry the request URL, and a URL is not an error message.
/// An empty return means "the server said nothing usable", and the
/// presentation layer supplies its own translated wording for that.
fn server_error_message(err: &matrix_sdk::Error) -> String {
    let Some(api) = err.as_client_api_error() else {
        return String::new();
    };
    let ErrorBody::Standard(body) = &api.body else {
        return String::new();
    };
    sanitize_error_message(&body.message)
}

/// Set — or CLEAR — the signed-in account's display name.
///
/// An empty `name` means CLEAR. `set_display_name` takes `Option<&str>` and
/// `Some("")` is NOT the same request: it asks the server to STORE an empty
/// name rather than to remove the field. The UI makes clearing a separate,
/// explicit action for the same reason, so an empty editor can never arrive
/// here by accident.
///
/// Result event: `own_display_name_result { op_id, lifecycle, ok, error }`.
pub(crate) fn set_own_display_name(
    bridge: &RustClient,
    name: String,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let bounded = bound_display_name(&name);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let arg = if bounded.is_empty() {
            None
        } else {
            Some(bounded.as_str())
        };
        let result = tokio::time::timeout(
            DISPLAY_NAME_REQUEST_TIMEOUT,
            client.account().set_display_name(arg),
        )
        .await;
        // A completion that outlived its session must never be reported:
        // the next account's UI would take it as ITS answer.
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, error) = match result {
            Ok(Ok(())) => (true, String::new()),
            Ok(Err(err)) => (false, server_error_message(&err)),
            // A timeout has no server body at all; the empty string is the
            // honest answer and C++ words it.
            Err(_) => (false, String::new()),
        };
        enqueue(
            &events,
            json!({
                "type": "own_display_name_result",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": ok,
                "error": error,
            }),
        );
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bound_keeps_short_names_byte_identical() {
        for name in [
            "Alice",
            "Rokas Smetonis",
            "Ąžuolas Užupis",
            "日本語の名前",
            "🦊",
            "👨‍👩‍👧‍👦",
            "e\u{0301}\u{0327}", // e + combining acute + combining cedilla
            "Ω mixed Ωmega 漢字 🦊",
        ] {
            assert_eq!(bound_display_name(name), name, "mangled {name:?}");
        }
    }

    #[test]
    fn bound_cuts_at_a_scalar_boundary_never_inside_one() {
        // 254 ASCII characters then one astral emoji: the 255th CHARACTER is
        // the emoji, so it survives whole. A UTF-16 code-unit bound of 255
        // would have cut it between its surrogates, and a byte bound would
        // have cut it after one of its four UTF-8 bytes.
        let name = format!("{}{}", "a".repeat(254), '\u{1F98A}');
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded, name);
        assert!(bounded.ends_with('\u{1F98A}'));
        // std::str is UTF-8 by construction, so the real proof that nothing
        // was split is that the astral scalar came through as ONE char.
        assert_eq!(bounded.chars().last(), Some('\u{1F98A}'));

        // Push the same emoji one character past the bound: it must be
        // dropped ENTIRELY, never half-emitted.
        let over = format!("{}{}", "a".repeat(255), '\u{1F98A}');
        let bounded_over = bound_display_name(&over);
        assert_eq!(bounded_over.chars().count(), 255);
        assert!(!bounded_over.contains('\u{1F98A}'));
        assert_eq!(bounded_over, "a".repeat(255));
    }

    #[test]
    fn bound_truncates_long_non_latin_names_at_255_scalars() {
        let name = "\u{65E5}".repeat(300); // 300 × U+65E5, 3 bytes each
        let bounded = bound_display_name(&name);
        assert_eq!(bounded.chars().count(), 255);
        assert_eq!(bounded.len(), 255 * 3); // no partial 3-byte sequence
    }

    #[test]
    fn empty_name_stays_empty_so_the_caller_can_map_it_to_none() {
        assert!(bound_display_name("").is_empty());
    }

    #[test]
    fn error_messages_are_collapsed_and_bounded() {
        assert_eq!(
            sanitize_error_message("Display name too\n\tlong   for  server"),
            "Display name too long for server"
        );
        assert_eq!(sanitize_error_message("   "), "");
        let long = "x".repeat(500);
        assert_eq!(sanitize_error_message(&long).chars().count(), 200);
    }
}
