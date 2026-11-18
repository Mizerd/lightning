//! Reads of optional items (account data, room state a client may never have
//! set). A missing one is an HTTP 404, and a server behind an anti-scanner
//! rule bans a client that makes a dozen of them in a second. So: the store
//! first, no request for an item the first sync response proves absent, no
//! repeat of a 404 already seen, and the requests that remain are spaced.

use std::collections::{HashMap, HashSet};
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

const SPACING: Duration = Duration::from_millis(400);
const SETTLE_WAIT: Duration = Duration::from_secs(6);
const FRESH_TTL: Duration = Duration::from_secs(120);

static SETTLE_EVENT: tokio::sync::Notify = tokio::sync::Notify::const_new();

static SETTLED: Mutex<Option<HashSet<String>>> = Mutex::new(None);
static UNPROVEN: Mutex<Option<HashSet<String>>> = Mutex::new(None);
static PROVED: Mutex<Option<HashSet<(String, String)>>> = Mutex::new(None);
static NEXT_SLOT: Mutex<Option<HashMap<String, Instant>>> = Mutex::new(None);
static FRESH: Freshness = Freshness::new();

fn with<T: Default, R>(slot: &Mutex<Option<T>>, f: impl FnOnce(&mut T) -> R) -> R {
    let mut guard = slot.lock().unwrap_or_else(|p| p.into_inner());
    f(guard.get_or_insert_with(T::default))
}

type Key = (String, String);

/// Types we wrote whose sync echo the store has not shown yet. A read of one
/// goes to the server until the store holds what we wrote (or, for a write
/// with no known content, the TTL passes); a read finishing never clears it,
/// so a later write cannot be hidden by an earlier read.
pub(crate) struct Freshness {
    marks: Mutex<Option<HashMap<Key, (Instant, Option<Value>)>>>,
}

impl Freshness {
    const fn new() -> Self {
        Self { marks: Mutex::new(None) }
    }

    fn note(&self, account: &str, event_type: &str, written: Option<Value>, now: Instant) {
        with(&self.marks, |m| m.insert((account.to_owned(), event_type.to_owned()), (now, written)));
    }

    fn is_fresh(&self, account: &str, event_type: &str, stored: &Option<Value>, now: Instant) -> bool {
        with(&self.marks, |m| {
            let key = (account.to_owned(), event_type.to_owned());
            let Some((at, written)) = m.get(&key) else { return false };
            let echoed = written.is_some() && written == stored;
            if echoed || now.saturating_duration_since(*at) > FRESH_TTL {
                m.remove(&key);
                return false;
            }
            true
        })
    }

    fn forget_account(&self, account: &str) {
        with(&self.marks, |m| m.retain(|(a, _), _| a != account));
    }
}

/// The account's first sync response is in the store. Global account data
/// missing from it is then absent, but only when the response evidently
/// carried account data: every real account has m.push_rules.
pub(crate) async fn settle(client: &Client) {
    let delivered = client
        .account()
        .account_data_raw(GlobalAccountDataEventType::PushRules)
        .await
        .ok()
        .flatten()
        .is_some();
    let set = if delivered { &SETTLED } else { &UNPROVEN };
    with(set, |s| s.insert(account_key(client)));
    SETTLE_EVENT.notify_waiters();
}

/// An optional read is never urgent: wait (bounded) for the first sync
/// response rather than ask for what it is about to deliver.
pub(crate) async fn await_settled(client: &Client) {
    let account = account_key(client);
    wait_until(|| in_set(&SETTLED, &account) || in_set(&UNPROVEN, &account), SETTLE_WAIT).await;
}

fn in_set(set: &Mutex<Option<HashSet<String>>>, account: &str) -> bool {
    with(set, |s| s.contains(account))
}

async fn wait_until(done: impl Fn() -> bool, limit: Duration) {
    let _ = tokio::time::timeout(limit, async {
        loop {
            let mut notified = std::pin::pin!(SETTLE_EVENT.notified());
            notified.as_mut().enable();
            if done() {
                return;
            }
            notified.await;
        }
    })
    .await;
}

pub(crate) fn forget_account_id(account: &str) {
    with(&SETTLED, |s| s.remove(account));
    with(&UNPROVEN, |s| s.remove(account));
    with(&PROVED, |p| p.retain(|(a, _)| a != account));
    with(&NEXT_SLOT, |n| n.remove(account));
    FRESH.forget_account(account);
    absent_answers().forget_prefix(account, "");
}

pub(crate) fn unsettle(client: &Client) {
    forget_account_id(&account_key(client));
}

fn is_settled(client: &Client) -> bool {
    in_set(&SETTLED, &account_key(client))
}

/// After our own write the store lags the server until the sync echo.
pub(crate) fn note_write(client: &Client, event_type: &str, written: Option<&Value>) {
    let account = account_key(client);
    absent_answers().forget(&account, event_type);
    with(&PROVED, |p| p.remove(&(account.clone(), event_type.to_owned())));
    FRESH.note(&account, event_type, written.cloned(), Instant::now());
}

/// The store has no copy and the first sync response is in: nothing to ask.
pub(crate) fn proves_absent(client: &Client, event_type: &str) -> bool {
    is_settled(client)
        && !FRESH.is_fresh(&account_key(client), event_type, &None, Instant::now())
}

/// A write composed from a read that only inferred absence must not replace
/// something the server holds: check, and refuse the write if it is there.
pub(crate) async fn guard_write(client: &Client, event_type: &str) -> Result<(), String> {
    let account = account_key(client);
    let proved = with(&PROVED, |p| p.remove(&(account.clone(), event_type.to_owned())));
    let verdict = check_inferred_absence(proved, server_get(client, event_type, None)).await;
    if verdict == Err("stale_absence".to_owned()) {
        FRESH.note(&account, event_type, None, Instant::now());
        absent_answers().forget(&account, event_type);
    }
    verdict
}

async fn check_inferred_absence(
    proved: bool,
    server: impl Future<Output = Result<Option<Value>, String>>,
) -> Result<(), String> {
    if !proved {
        return Ok(());
    }
    match server.await? {
        None => Ok(()),
        Some(_) => Err("stale_absence".to_owned()),
    }
}

/// Wait for this account's turn, then say whether the probe is still needed.
/// A probe the re-check skips gives its slot back.
async fn pace(account: &str, needed: impl Fn() -> bool) -> bool {
    let (wait, slot, previous) = {
        let mut guard = NEXT_SLOT.lock().unwrap_or_else(|p| p.into_inner());
        let map = guard.get_or_insert_with(HashMap::new);
        let now = Instant::now();
        let previous = map.get(account).copied();
        let slot = previous.map_or(now, |n| n.max(now));
        map.insert(account.to_owned(), slot + SPACING);
        (slot - now, slot, previous)
    };
    if !wait.is_zero() {
        tokio::time::sleep(wait).await;
    }
    if needed() {
        return true;
    }
    with(&NEXT_SLOT, |map| {
        if map.get(account) == Some(&(slot + SPACING)) {
            match previous {
                Some(p) => map.insert(account.to_owned(), p),
                None => map.remove(account),
            };
        }
    });
    false
}

/// `Ok((value, proved))`: `proved` is true when `None` was inferred rather
/// than answered by the server.
pub(crate) async fn read_optional<S, Q, QF>(
    absent: &crate::stickers::AbsentAnswers,
    freshness: &Freshness,
    account: &str,
    event_type: &str,
    settled: bool,
    trust_store: bool,
    store: S,
    ask: Q,
) -> Result<(Option<Value>, bool), String>
where
    S: Future<Output = Option<Value>>,
    Q: FnOnce() -> QF,
    QF: Future<Output = Result<Option<Value>, String>>,
{
    let stored = store.await;
    let fresh = freshness.is_fresh(account, event_type, &stored, Instant::now());
    let skip = || {
        !fresh && stored.is_none()
            && (settled || absent.is_absent(account, event_type, Instant::now()))
    };
    if !fresh {
        if trust_store {
            if let Some(value) = &stored {
                absent.forget(account, event_type);
                return Ok((Some(value.clone()), false));
            }
        }
        if skip() {
            return Ok((None, true));
        }
    }
    if !pace(account, || !skip()).await {
        return Ok((None, true));
    }
    let answer = ask().await?;
    if answer.is_none() {
        absent.mark_absent(account, event_type, Instant::now());
    } else {
        absent.forget(account, event_type);
    }
    Ok((answer, false))
}

async fn server_get(
    client: &Client,
    event_type: &str,
    config: Option<RequestConfig>,
) -> Result<Option<Value>, String> {
    let own = client.user_id().ok_or_else(|| "signed_out".to_owned())?.to_owned();
    let request = get_global_account_data::v3::Request::new(
        own,
        GlobalAccountDataEventType::from(event_type),
    );
    let sent = client.send(request);
    let result = match config {
        Some(config) => sent.with_request_config(config).await,
        None => sent.await,
    };
    match result {
        Ok(r) => Ok(Some(serde_json::from_str(r.account_data.json().get()).unwrap_or(Value::Null))),
        Err(err) if err.as_client_api_error().is_some_and(|e| e.status_code.as_u16() == 404) => {
            Ok(None)
        }
        Err(err) => Err(classify_write_error(&err.to_string()).0.to_owned()),
    }
}

/// Own global account data, `Ok(None)` when it does not exist. `config`
/// `None` keeps the SDK's default request settings. `trust_store` false keeps
/// a server read authoritative where the store may lag: the store then only
/// proves absence, never answers.
pub(crate) async fn read_global(
    client: &Client,
    event_type: &str,
    config: Option<RequestConfig>,
    trust_store: bool,
) -> Result<Option<Value>, String> {
    let account = account_key(client);
    await_settled(client).await;
    let ty = GlobalAccountDataEventType::from(event_type);
    let (answer, proved) = read_optional(
        absent_answers(),
        &FRESH,
        &account,
        event_type,
        is_settled(client),
        trust_store,
        async {
            let raw = client.account().account_data_raw(ty.clone()).await.ok().flatten()?;
            serde_json::from_str::<Value>(raw.json().get()).ok()
        },
        || server_get(client, event_type, config),
    )
    .await?;
    let key = (account, event_type.to_owned());
    if proved && answer.is_none() {
        with(&PROVED, |p| p.insert(key));
    } else {
        with(&PROVED, |p| p.remove(&key));
    }
    Ok(answer)
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

pub(crate) async fn state_pace(client: &Client) {
    pace(&account_key(client), || true).await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stickers::AbsentAnswers;
    use serde_json::json;
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct Rig {
        absent: AbsentAnswers,
        fresh: Freshness,
        asked: AtomicUsize,
    }

    impl Rig {
        fn new() -> Self {
            Self { absent: AbsentAnswers::new(), fresh: Freshness::new(), asked: AtomicUsize::new(0) }
        }

        async fn read(
            &self,
            settled: bool,
            trust_store: bool,
            stored: Option<Value>,
            server: Option<Value>,
        ) -> (Option<Value>, bool) {
            read_optional(&self.absent, &self.fresh, "@a:b", "t", settled, trust_store, async { stored }, || async {
                self.asked.fetch_add(1, Ordering::SeqCst);
                Ok(server)
            })
            .await
            .unwrap()
        }

        fn asked(&self) -> usize {
            self.asked.load(Ordering::SeqCst)
        }
    }

    #[tokio::test]
    async fn the_store_answers_without_a_request() {
        let rig = Rig::new();
        let (got, _) = rig.read(false, true, Some(json!({"a": 1})), None).await;
        assert_eq!(got, Some(json!({"a": 1})));
        assert_eq!(rig.asked(), 0);
    }

    #[tokio::test]
    async fn a_settled_account_never_asks_for_what_the_store_lacks_and_says_it_inferred_it() {
        let rig = Rig::new();
        assert_eq!(rig.read(true, true, None, Some(json!({}))).await, (None, true));
        assert_eq!(rig.asked(), 0);
    }

    #[tokio::test]
    async fn an_unsettled_account_asks_once_and_remembers_the_404() {
        let rig = Rig::new();
        assert_eq!(rig.read(false, true, None, None).await, (None, false));
        assert_eq!(rig.read(false, true, None, None).await, (None, true));
        assert_eq!(rig.read(false, true, None, None).await, (None, true));
        assert_eq!(rig.asked(), 1);
    }

    #[tokio::test]
    async fn a_value_the_server_has_is_returned_and_not_remembered_as_absent() {
        let rig = Rig::new();
        let v = json!({"k": true});
        assert_eq!(rig.read(false, true, None, Some(v.clone())).await.0, Some(v.clone()));
        assert_eq!(rig.read(false, true, None, Some(v.clone())).await.0, Some(v));
        assert_eq!(rig.asked(), 2);
    }

    #[tokio::test]
    async fn a_store_that_may_lag_never_answers_but_does_prove_absence() {
        let rig = Rig::new();
        let got = rig.read(true, false, Some(json!({"old": 1})), None).await;
        assert_eq!(got.0, None, "the server's 404 wins over the lagging copy");
        assert_eq!(rig.asked(), 1);
        let rig = Rig::new();
        assert_eq!(rig.read(true, false, None, Some(json!({}))).await, (None, true));
        assert_eq!(rig.asked(), 0, "settled and not in the store: no request");
    }

    #[tokio::test]
    async fn a_save_between_a_write_and_its_echo_still_reads_the_server() {
        let rig = Rig::new();
        let item1 = json!({"items": [1]});
        rig.absent.mark_absent("@a:b", "t", Instant::now());
        rig.fresh.note("@a:b", "t", Some(item1.clone()), Instant::now());
        // refresh after the first save: the store has no copy yet
        assert_eq!(rig.read(true, true, None, Some(item1.clone())).await, (Some(item1.clone()), false));
        // the user saves again before the echo: the read-before-write must ask again
        assert_eq!(rig.read(true, true, None, Some(item1.clone())).await, (Some(item1.clone()), false));
        assert_eq!(rig.asked(), 2, "an earlier read must not clear the mark of the write");
        // the echo arrives: the store is trusted again
        assert_eq!(rig.read(true, true, Some(item1.clone()), None).await, (Some(item1), false));
        assert_eq!(rig.asked(), 2);
    }

    #[tokio::test]
    async fn an_unechoed_write_with_unknown_content_ages_out() {
        let fresh = Freshness::new();
        let now = Instant::now();
        fresh.note("@a:b", "t", None, now);
        assert!(fresh.is_fresh("@a:b", "t", &Some(json!({})), now));
        assert!(!fresh.is_fresh("@a:b", "t", &None, now + FRESH_TTL + Duration::from_secs(1)));
    }

    #[tokio::test]
    async fn a_write_after_an_inferred_absence_is_refused_when_the_server_has_the_item() {
        let item = Some(json!({"items": [1]}));
        assert_eq!(check_inferred_absence(true, async { Ok(item.clone()) }).await, Err("stale_absence".to_owned()));
        assert_eq!(check_inferred_absence(true, async { Ok(None) }).await, Ok(()));
        let asked = AtomicUsize::new(0);
        let skipped = check_inferred_absence(false, async {
            asked.fetch_add(1, Ordering::SeqCst);
            Ok(item.clone())
        })
        .await;
        assert_eq!((skipped, asked.load(Ordering::SeqCst)), (Ok(()), 0), "an answered read needs no check");
    }

    static SERIAL: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

    async fn synced_client(push_rules: bool) -> (matrix_sdk::test_utils::mocks::MatrixMockServer, Client) {
        use matrix_sdk::ruma::{api::client::sync::sync_events::v5, serde::Raw};
        use matrix_sdk_base::RequestedRequiredStates;
        let server = matrix_sdk::test_utils::mocks::MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let mut response = v5::Response::new("pos-1".to_owned());
        if push_rules {
            response.extensions.account_data.global = vec![Raw::from_json_string(
                json!({ "type": "m.push_rules", "content": { "global": {
                    "override": [], "content": [], "room": [], "sender": [], "underride": [] } } })
                .to_string(),
            )
            .unwrap()];
        }
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        (server, client)
    }

    #[tokio::test]
    async fn an_account_whose_first_response_carried_no_account_data_is_never_settled() {
        let _serial = SERIAL.lock().await;
        let (_server, client) = synced_client(false).await;
        settle(&client).await;
        assert!(!is_settled(&client));
        assert!(!proves_absent(&client, "anything"));
        forget_account_id(&account_key(&client));
    }

    #[tokio::test]
    async fn an_account_whose_first_response_carried_push_rules_settles_and_forgets_on_logout() {
        let _serial = SERIAL.lock().await;
        let (_server, client) = synced_client(true).await;
        settle(&client).await;
        assert!(is_settled(&client));
        assert!(proves_absent(&client, "anything"));
        note_write(&client, "anything", None);
        assert!(!proves_absent(&client, "anything"), "our own write is not yet in the store");
        forget_account_id(&account_key(&client));
        assert!(!is_settled(&client));
    }

    #[tokio::test]
    async fn forgetting_an_account_drops_its_marks() {
        let fresh = Freshness::new();
        fresh.note("@a:b", "t", None, Instant::now());
        fresh.forget_account("@a:b");
        assert!(!fresh.is_fresh("@a:b", "t", &None, Instant::now()));
    }

    #[tokio::test]
    async fn a_read_waits_for_the_first_sync_response_but_not_for_ever() {
        let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let setter = std::sync::Arc::clone(&flag);
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(50)).await;
            setter.store(true, Ordering::SeqCst);
            SETTLE_EVENT.notify_waiters();
        });
        let start = Instant::now();
        wait_until(|| flag.load(Ordering::SeqCst), Duration::from_secs(5)).await;
        assert!(flag.load(Ordering::SeqCst) && start.elapsed() < Duration::from_secs(2));
        let start = Instant::now();
        wait_until(|| false, Duration::from_millis(80)).await;
        assert!(start.elapsed() >= Duration::from_millis(80));
    }

    #[tokio::test]
    async fn requests_that_remain_are_spaced_per_account_and_a_skipped_probe_gives_its_slot_back() {
        let start = Instant::now();
        for _ in 0..3 {
            assert!(pace("@spaced:x", || true).await);
        }
        assert!(start.elapsed() >= SPACING * 2);
        let other = Instant::now();
        pace("@other:x", || true).await;
        assert!(other.elapsed() < SPACING, "another account is not delayed");
        let t = Instant::now();
        assert!(!pace("@skip:x", || false).await);
        assert!(pace("@skip:x", || true).await);
        assert!(t.elapsed() < SPACING, "the skipped probe reserved nothing");
    }
}
