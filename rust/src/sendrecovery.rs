//! Automatic recovery of a send queue that matrix-sdk disabled after a
//! RECOVERABLE send error (network failure, 429, 5xx, timeout).
//!
//! matrix-sdk sets a room's queue `locally_enabled = false` on ANY send error
//! and never turns it back on by itself. Lightning re-enabled it in exactly
//! three places: when a NEW send is composed in a room that has a live
//! timeline (`timeline::unwedge_send_queue`), on a sync Running/recovery edge,
//! and on a manual Retry. A send-only failure (sync healthy, `/send` rejected
//! or timed out) in a room the user then stopped typing in therefore left the
//! echo parked until the next restart, where
//! `respawn_tasks_for_rooms_with_unsent_requests` finally picked it up.
//!
//! This task listens to `SendQueue::subscribe_errors` and re-enables the
//! failing room after a bounded backoff. Unrecoverable errors are wedged by
//! the SDK (skipped, not resent) and are left for the user's Retry/Cancel.
//! Logging carries a coarse category and a redacted room id, never a body,
//! never the SDK error text.

use std::time::Duration;

use matrix_sdk::Client;
use tokio::sync::broadcast::error::RecvError;

/// Attempts per room before the automatic path gives up and leaves the
/// visible Retry as the only way forward.
const MAX_AUTO_ATTEMPTS: u32 = 8;

#[derive(Clone, Copy, Debug)]
pub(crate) struct RecoveryPolicy {
    pub base: Duration,
    pub ceiling: Duration,
    /// An error this long after the previous one starts the count afresh.
    pub forget_after: Duration,
}

impl RecoveryPolicy {
    pub(crate) const fn production() -> Self {
        Self {
            base: Duration::from_secs(2),
            ceiling: Duration::from_secs(60),
            forget_after: Duration::from_secs(300),
        }
    }

    pub(crate) fn delay(&self, attempt: u32) -> Duration {
        let factor = 1u32 << attempt.min(10);
        self.base.saturating_mul(factor).min(self.ceiling)
    }
}

/// Sigil plus 8 hex of SHA-256: stable for correlation, not reversible.
fn redact(id: &str) -> String {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(id.as_bytes());
    let hex: String = digest.iter().take(4).map(|b| format!("{b:02x}")).collect();
    format!("{}{hex}", id.chars().next().unwrap_or('?'))
}

fn category(error: &matrix_sdk::Error) -> &'static str {
    match error {
        matrix_sdk::Error::Http(_) => "http",
        matrix_sdk::Error::ConcurrentRequestFailed => "concurrent_request",
        _ => "other",
    }
}

/// Start the listener. The task ends when the client is no longer active
/// (logout) or the runtime shuts down.
pub(crate) fn spawn(client: Client, policy: RecoveryPolicy) {
    // Subscribed before the task starts, so an error cannot slip in between.
    let mut errors = client.send_queue().subscribe_errors();
    tokio::spawn(async move {
        let mut state: std::collections::HashMap<
            matrix_sdk::ruma::OwnedRoomId,
            (u32, std::time::Instant),
        > = std::collections::HashMap::new();
        loop {
            let error = match tokio::time::timeout(Duration::from_secs(60), errors.recv()).await {
                Err(_) => {
                    if !client.is_active() {
                        return;
                    }
                    continue;
                }
                Ok(Ok(error)) => error,
                // A lagged receiver is a gap, not an end.
                Ok(Err(RecvError::Lagged(_))) => continue,
                Ok(Err(RecvError::Closed)) => return,
            };
            let room = redact(error.room_id.as_str());
            if !error.is_recoverable {
                // The SDK wedged the failing item (skipped for good) but ALSO
                // disabled the room's queue, which strands every message
                // queued behind it. Re-enabling resends nothing that was
                // rejected; the wedged one stays for the user's Retry/Cancel.
                eprintln!(
                    "lightning.send_queue: room={room} unrecoverable category={} \
(item wedged for Retry/Cancel); re-enabling the queue for those behind it",
                    category(&error.error)
                );
                let client = client.clone();
                let room_id = error.room_id.clone();
                tokio::spawn(async move {
                    tokio::time::sleep(policy.base.min(Duration::from_secs(1))).await;
                    if let Some(room) = client.get_room(&room_id) {
                        room.send_queue().set_enabled(true);
                    }
                });
                continue;
            }
            let now = std::time::Instant::now();
            let entry = state.entry(error.room_id.clone()).or_insert((0, now));
            if now.duration_since(entry.1) > policy.forget_after {
                entry.0 = 0;
            }
            entry.1 = now;
            let attempt = entry.0;
            if attempt >= MAX_AUTO_ATTEMPTS {
                eprintln!(
                    "lightning.send_queue: room={room} recoverable category={} \
giving up automatic re-enable after {attempt} attempts; Retry is offered",
                    category(&error.error)
                );
                continue;
            }
            entry.0 += 1;
            let delay = policy.delay(attempt);
            eprintln!(
                "lightning.send_queue: room={room} recoverable category={} \
queue disabled; re-enabling in {} ms (attempt {})",
                category(&error.error),
                delay.as_millis(),
                attempt + 1
            );
            let client = client.clone();
            let room_id = error.room_id.clone();
            tokio::spawn(async move {
                tokio::time::sleep(delay).await;
                if let Some(room) = client.get_room(&room_id) {
                    room.send_queue().set_enabled(true);
                }
            });
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::{
        ruma::{event_id, events::room::message::RoomMessageEventContent, room_id},
        send_queue::RoomSendQueueUpdate,
        test_utils::mocks::MatrixMockServer,
    };

    #[test]
    fn the_delay_doubles_and_is_capped() {
        let p = RecoveryPolicy::production();
        assert_eq!(p.delay(0), Duration::from_secs(2));
        assert_eq!(p.delay(2), Duration::from_secs(8));
        assert_eq!(p.delay(20), Duration::from_secs(60));
    }

    #[derive(Clone, Copy, PartialEq)]
    enum Failure {
        /// 500s that outlast the SDK's own short retry: recoverable, the
        /// queue disabled, the item kept.
        Recoverable,
        /// 413: permanent, the item wedged, and the queue disabled anyway.
        Wedged,
    }

    /// Fail the first /send(s) of message A, queue message B behind it, and
    /// report whether the LAST message reached the server within the window
    /// with no further user action (no new send, no Retry, no restart).
    async fn delivered_without_user_action(failure: Failure, recover: bool) -> bool {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room_id = room_id!("!stuck:localhost");
        let room = server.sync_joined_room(&client, room_id).await;
        if recover {
            spawn(
                client.clone(),
                RecoveryPolicy {
                    base: Duration::from_millis(100),
                    ceiling: Duration::from_millis(400),
                    forget_after: Duration::from_secs(60),
                },
            );
        }
        server.mock_room_state_encryption().plain().mount().await;
        match failure {
            Failure::Recoverable => {
                server.mock_room_send().error500().up_to_n_times(8).mount().await;
            }
            Failure::Wedged => {
                server.mock_room_send().error_too_large().up_to_n_times(1).mount().await;
            }
        }

        // Mounted second: wiremock tries mocks in mount order, so this answers
        // once the failures above are spent.
        server.mock_room_send().ok(event_id!("$sent")).mount().await;
        let queue = room.send_queue();
        let (_, mut updates) = queue.subscribe().await.unwrap();
        queue.send(RoomMessageEventContent::text_plain("a").into()).await.unwrap();
        queue.send(RoomMessageEventContent::text_plain("b").into()).await.unwrap();

        let window = Duration::from_secs(if recover { 20 } else { 6 });
        let deadline = tokio::time::Instant::now() + window;
        let mut sent = 0;
        let mut errors = 0;
        while let Ok(Ok(update)) = tokio::time::timeout_at(deadline, updates.recv()).await {
            match update {
                RoomSendQueueUpdate::SendError { is_recoverable, .. } => {
                    errors += 1;
                    assert_eq!(is_recoverable, failure == Failure::Recoverable);
                }
                RoomSendQueueUpdate::SentEvent { .. } => {
                    sent += 1;
                    // A wedged A is never resent, so only B arrives there.
                    let want = if failure == Failure::Wedged { 1 } else { 2 };
                    if sent == want {
                        assert!(errors >= 1, "the mock should have failed at least once");
                        return true;
                    }
                }
                _ => {}
            }
        }
        false
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn a_recoverable_send_error_is_retried_without_a_new_send() {
        assert!(delivered_without_user_action(Failure::Recoverable, true).await);
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn a_message_queued_behind_a_wedged_one_is_still_sent() {
        assert!(delivered_without_user_action(Failure::Wedged, true).await);
    }

    /// Controls: with no listener the SDK leaves the queue disabled, which is
    /// what the tests above would hit on the old code.
    #[tokio::test(flavor = "multi_thread")]
    async fn without_the_listener_a_recoverable_error_parks_the_queue() {
        assert!(!delivered_without_user_action(Failure::Recoverable, false).await);
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn without_the_listener_a_wedged_message_parks_the_queue() {
        assert!(!delivered_without_user_action(Failure::Wedged, false).await);
    }
}
