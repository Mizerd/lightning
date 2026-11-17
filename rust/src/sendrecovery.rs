
use std::time::Duration;

use matrix_sdk::Client;
use tokio::sync::broadcast::error::RecvError;

const MAX_AUTO_ATTEMPTS: u32 = 8;

#[derive(Clone, Copy, Debug)]
pub(crate) struct RecoveryPolicy {
    pub base: Duration,
    pub ceiling: Duration,
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

pub(crate) fn spawn(client: Client, policy: RecoveryPolicy) {
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

pub(crate) enum QueuedSend {
    Sent(matrix_sdk::ruma::OwnedEventId),
    Failed(String),
    // Still in the queue (or cancelled by the user): the room's timeline
    // shows it, so the caller reports nothing.
    Unresolved,
}

/// Sends through the room's send queue rather than `Room::send`, so a
/// timeline opened while the request is in flight carries the local echo
/// and reconciles it, instead of the message vanishing until sync.
pub(crate) async fn send_queued(
    room: &matrix_sdk::Room,
    content: matrix_sdk::ruma::events::AnyMessageLikeEventContent,
    wait: Duration,
) -> QueuedSend {
    use matrix_sdk::send_queue::RoomSendQueueUpdate;
    let queue = room.send_queue();
    queue.set_enabled(true);
    let Ok((_, mut updates)) = queue.subscribe().await else {
        return QueuedSend::Failed("the send queue is unavailable".to_owned());
    };
    let handle = match queue.send(content).await {
        Ok(handle) => handle,
        Err(err) => return QueuedSend::Failed(err.to_string()),
    };
    let deadline = tokio::time::Instant::now() + wait;
    // SendHandle exposes no transaction id in 0.18; its NewLocalEvent clone shares created_at.
    // shortcut: two sends to one room in the same millisecond may swap outcomes; key on the txn id once the SDK exposes it.
    let mut txn = None;
    loop {
        let update = match tokio::time::timeout_at(deadline, updates.recv()).await {
            Ok(Ok(update)) => update,
            Ok(Err(RecvError::Lagged(_))) => continue,
            Ok(Err(RecvError::Closed)) | Err(_) => return QueuedSend::Unresolved,
        };
        let ours = |id: &matrix_sdk::ruma::OwnedTransactionId| txn.as_ref() == Some(id);
        match update {
            RoomSendQueueUpdate::NewLocalEvent(echo) if txn.is_none() => {
                if let matrix_sdk::send_queue::LocalEchoContent::Event { send_handle, .. } =
                    &echo.content
                {
                    if send_handle.created_at == handle.created_at {
                        txn = Some(echo.transaction_id);
                    }
                }
            }
            RoomSendQueueUpdate::SentEvent { transaction_id, event_id } if ours(&transaction_id) => {
                return QueuedSend::Sent(event_id);
            }
            RoomSendQueueUpdate::SendError { transaction_id, error, is_recoverable: false }
                if ours(&transaction_id) =>
            {
                return QueuedSend::Failed(error.to_string());
            }
            RoomSendQueueUpdate::CancelledLocalEvent { transaction_id } if ours(&transaction_id) => {
                return QueuedSend::Unresolved;
            }
            _ => {}
        }
    }
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
        Recoverable,
        Wedged,
    }

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
    async fn a_timeline_opened_mid_send_keeps_the_message_and_reconciles_it() {
        use matrix_sdk_ui::timeline::RoomExt;
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room_id = room_id!("!mid:localhost");
        let room = server.sync_joined_room(&client, room_id).await;
        server.mock_room_state_encryption().plain().mount().await;
        server
            .mock_room_send()
            .ok_with_delay(event_id!("$sent"), Duration::from_millis(800))
            .mount()
            .await;

        let sending = {
            let room = room.clone();
            tokio::spawn(async move {
                send_queued(
                    &room,
                    RoomMessageEventContent::text_plain("hello").into(),
                    Duration::from_secs(10),
                )
                .await
            })
        };
        tokio::time::sleep(Duration::from_millis(200)).await;
        let timeline = room.timeline().await.unwrap();
        let mut echoed = false;
        for _ in 0..10 {
            echoed = timeline.items().await.iter().any(|item| {
                item.as_event().is_some_and(|ev| {
                    ev.is_local_echo()
                        && ev.content().as_message().is_some_and(|m| m.body() == "hello")
                })
            });
            if echoed {
                break;
            }
            tokio::time::sleep(Duration::from_millis(30)).await;
        }
        assert!(echoed, "a timeline opened mid-send does not show the message being sent");
        match sending.await.unwrap() {
            QueuedSend::Sent(id) => assert_eq!(id, event_id!("$sent")),
            _ => panic!("the queued send did not report the server's event id"),
        }
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn a_refused_send_reports_failure() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room_id = room_id!("!refused:localhost");
        let room = server.sync_joined_room(&client, room_id).await;
        server.mock_room_state_encryption().plain().mount().await;
        server.mock_room_send().error_too_large().mount().await;
        let outcome = send_queued(
            &room,
            RoomMessageEventContent::text_plain("x").into(),
            Duration::from_secs(10),
        )
        .await;
        assert!(matches!(outcome, QueuedSend::Failed(_)));
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn a_recoverable_send_error_is_retried_without_a_new_send() {
        assert!(delivered_without_user_action(Failure::Recoverable, true).await);
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn a_message_queued_behind_a_wedged_one_is_still_sent() {
        assert!(delivered_without_user_action(Failure::Wedged, true).await);
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn without_the_listener_a_recoverable_error_parks_the_queue() {
        assert!(!delivered_without_user_action(Failure::Recoverable, false).await);
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn without_the_listener_a_wedged_message_parks_the_queue() {
        assert!(!delivered_without_user_action(Failure::Wedged, false).await);
    }
}
