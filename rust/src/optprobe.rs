//! Reads of optional items (account data, room state a client may never have
//! set). A missing one is an HTTP 404, and a server behind an anti-scanner
//! rule bans a client that makes a dozen of them in a second. So: the store
//! first, no request for an item the first sync response proves absent, no
//! repeat of a 404 already seen, and the requests that remain are spaced.

use std::collections::HashSet;
use std::future::Future;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::config::get_global_account_data,
        events::GlobalAccountDataEventType,
    },
    Client,
};
use serde_json::Value;

use crate::backdrop::classify_write_error;
use crate::stickers::{absent_answers, account_key};

const SPACING: Duration = Duration::from_millis(250);

static SETTLED: Mutex<Option<HashSet<String>>> = Mutex::new(None);
static FRESH: Mutex<Option<HashSet<(String, String)>>> = Mutex::new(None);
static NEXT_SLOT: Mutex<Option<Instant>> = Mutex::new(None);

fn with<T: Default, R>(slot: &Mutex<Option<T>>, f: impl FnOnce(&mut T) -> R) -> R {
    let mut guard = slot.lock().unwrap_or_else(|p| p.into_inner());
    f(guard.get_or_insert_with(T::default))
}

/// The account's first sync response is in the store: global account data
/// missing from it does not exist (the extension sends all of it initially).
pub(crate) fn settle(client: &Client) {
    with(&SETTLED, |s| s.insert(account_key(client)));
}

pub(crate) fn unsettle(client: &Client) {
    with(&SETTLED, |s| s.remove(&account_key(client)));
}

fn is_settled(client: &Client) -> bool {
    with(&SETTLED, |s| s.contains(&account_key(client)))
}

/// After our own write the store lags the server until the sync echo, so the
/// next read of that type goes to the server.
pub(crate) fn note_write(client: &Client, event_type: &str) {
    let account = account_key(client);
    absent_answers().forget(&account, event_type);
    with(&FRESH, |f| f.insert((account, event_type.to_owned())));
}

/// The store has no copy and the first sync response is in: nothing to ask.
pub(crate) fn proves_absent(client: &Client, event_type: &str) -> bool {
    let key = (account_key(client), event_type.to_owned());
    is_settled(client) && !with(&FRESH, |f| f.contains(&key))
}

pub(crate) fn served(client: &Client, event_type: &str) {
    with(&FRESH, |f| f.remove(&(account_key(client), event_type.to_owned())));
}

async fn pace() {
    let wait = {
        let mut next = NEXT_SLOT.lock().unwrap_or_else(|p| p.into_inner());
        let now = Instant::now();
        let slot = next.map_or(now, |n| n.max(now));
        *next = Some(slot + SPACING);
        slot - now
    };
    if !wait.is_zero() {
        tokio::time::sleep(wait).await;
    }
}

pub(crate) async fn read_optional<S, Q, QF>(
    absent: &crate::stickers::AbsentAnswers,
    account: &str,
    event_type: &str,
    settled: bool,
    fresh: bool,
    store: S,
    ask: Q,
) -> Result<Option<Value>, String>
where
    S: Future<Output = Option<Value>>,
    Q: FnOnce() -> QF,
    QF: Future<Output = Result<Option<Value>, String>>,
{
    if !fresh {
        if let Some(value) = store.await {
            absent.forget(account, event_type);
            return Ok(Some(value));
        }
        if settled || absent.is_absent(account, event_type, Instant::now()) {
            return Ok(None);
        }
    }
    pace().await;
    if !fresh && absent.is_absent(account, event_type, Instant::now()) {
        return Ok(None);
    }
    let answer = ask().await?;
    if answer.is_none() {
        absent.mark_absent(account, event_type, Instant::now());
    } else {
        absent.forget(account, event_type);
    }
    Ok(answer)
}

/// Own global account data, `Ok(None)` when it does not exist. `config`
/// `None` keeps the SDK's default request settings.
pub(crate) async fn read_global(
    client: &Client,
    event_type: &str,
    config: Option<RequestConfig>,
) -> Result<Option<Value>, String> {
    let account = account_key(client);
    let fresh = with(&FRESH, |f| f.contains(&(account.clone(), event_type.to_owned())));
    let ty = GlobalAccountDataEventType::from(event_type);
    let answer = read_optional(
        absent_answers(),
        &account,
        event_type,
        is_settled(client),
        fresh,
        async {
            let raw = client.account().account_data_raw(ty.clone()).await.ok().flatten()?;
            serde_json::from_str::<Value>(raw.json().get()).ok()
        },
        || async {
            let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
            let request = get_global_account_data::v3::Request::new(own, ty.clone());
            let sent = client.send(request);
            let result = match config {
                Some(config) => sent.with_request_config(config).await,
                None => sent.await,
            };
            match result {
                Ok(r) => Ok(Some(
                    serde_json::from_str(r.account_data.json().get()).unwrap_or(Value::Null),
                )),
                Err(err) if err.as_client_api_error().is_some_and(|e| e.status_code.as_u16() == 404) => {
                    Ok(None)
                }
                Err(err) => Err(classify_write_error(&err.to_string()).0.to_owned()),
            }
        },
    )
    .await;
    if answer.is_ok() {
        with(&FRESH, |f| f.remove(&(account, event_type.to_owned())));
    }
    answer
}

pub(crate) fn state_key(room_id: &str, event_type: &str) -> String {
    format!("state|{room_id}|{event_type}")
}

pub(crate) fn state_known_absent(client: &Client, room_id: &str, event_type: &str) -> bool {
    absent_answers().is_absent(&account_key(client), &state_key(room_id, event_type), Instant::now())
}

pub(crate) fn state_mark_absent(client: &Client, room_id: &str, event_type: &str) {
    absent_answers().mark_absent(&account_key(client), &state_key(room_id, event_type), Instant::now());
}

pub(crate) fn state_forget(client: &Client, room_id: &str, event_type: &str) {
    absent_answers().forget(&account_key(client), &state_key(room_id, event_type));
}

pub(crate) async fn state_pace() {
    pace().await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stickers::AbsentAnswers;
    use std::sync::atomic::{AtomicUsize, Ordering};

    async fn read(
        absent: &AbsentAnswers,
        settled: bool,
        fresh: bool,
        stored: Option<Value>,
        server: Option<Value>,
        asked: &AtomicUsize,
    ) -> Option<Value> {
        read_optional(absent, "@a:b", "t", settled, fresh, async { stored }, || async {
            asked.fetch_add(1, Ordering::SeqCst);
            Ok(server)
        })
        .await
        .unwrap()
    }

    #[tokio::test]
    async fn the_store_answers_without_a_request() {
        let (absent, asked) = (AbsentAnswers::new(), AtomicUsize::new(0));
        let got = read(&absent, false, false, Some(serde_json::json!({"a": 1})), None, &asked).await;
        assert_eq!(got, Some(serde_json::json!({"a": 1})));
        assert_eq!(asked.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn a_settled_account_never_asks_for_what_the_store_lacks() {
        let (absent, asked) = (AbsentAnswers::new(), AtomicUsize::new(0));
        assert_eq!(read(&absent, true, false, None, Some(serde_json::json!({})), &asked).await, None);
        assert_eq!(asked.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn an_unsettled_account_asks_once_and_remembers_the_404() {
        let (absent, asked) = (AbsentAnswers::new(), AtomicUsize::new(0));
        for _ in 0..3 {
            assert_eq!(read(&absent, false, false, None, None, &asked).await, None);
        }
        assert_eq!(asked.load(Ordering::SeqCst), 1);
    }

    #[tokio::test]
    async fn a_value_the_server_has_is_returned_and_not_remembered_as_absent() {
        let (absent, asked) = (AbsentAnswers::new(), AtomicUsize::new(0));
        let v = serde_json::json!({"k": true});
        assert_eq!(read(&absent, false, false, None, Some(v.clone()), &asked).await, Some(v.clone()));
        assert_eq!(read(&absent, false, false, None, Some(v.clone()), &asked).await, Some(v));
        assert_eq!(asked.load(Ordering::SeqCst), 2);
    }

    #[tokio::test]
    async fn a_fresh_read_after_our_own_write_bypasses_the_store_and_the_memory() {
        let (absent, asked) = (AbsentAnswers::new(), AtomicUsize::new(0));
        absent.mark_absent("@a:b", "t", Instant::now());
        let v = serde_json::json!({"new": 1});
        let got = read(&absent, true, true, Some(serde_json::json!({"old": 1})), Some(v.clone()), &asked).await;
        assert_eq!(got, Some(v));
        assert_eq!(asked.load(Ordering::SeqCst), 1);
    }

    #[tokio::test]
    async fn requests_that_remain_are_spaced() {
        let start = Instant::now();
        pace().await;
        pace().await;
        pace().await;
        assert!(start.elapsed() >= SPACING * 2);
    }
}
