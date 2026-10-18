//! The rotation contract against a homeserver whose authorization server
//! follows MAS's refresh-token rules, plus the ledger's own bounds.
//!
//! The fixture implements `refresh_token_grant` from MAS
//! (`crates/handlers/src/oauth2/token.rs`): a refresh rotates both tokens and
//! revokes the previous access token; a consumed refresh token is accepted
//! again only while its replacement is unconsumed AND the access token issued
//! with that replacement has never been used, which then revokes the
//! replacement pair; anything else is `invalid_grant`. "Used" here means a
//! `whoami` carried it: the fixture answers every other client API request
//! without looking at the token, so matrix-sdk's own E2EE initialization
//! requests neither refresh nor count, and the order under test is the one
//! between our probe's retry and the keyring write.
//!
//! A std thread stands in for C++: every `drain_every` it notes a drain,
//! empties the command lane, writes each `session_tokens_refreshed` pair to a
//! fake keyring and answers the ledger, as `RustSdkMatrixClient` does.

use super::*;

use std::collections::{HashMap, VecDeque};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread;

use matrix_sdk::authentication::oauth::{ClientId, OAuthSession, UserSession};
use matrix_sdk::ruma::{OwnedDeviceId, OwnedUserId};
use matrix_sdk::SessionMeta;

// ---- the MAS-rule fixture -------------------------------------------------

struct RefreshRec {
    access: String,
    next: Option<String>,
    consumed: bool,
    revoked: bool,
}

#[derive(Default)]
struct AccessRec {
    used_at: Option<u64>,
    revoked: bool,
}

#[derive(Default)]
struct Mas {
    refresh: HashMap<String, RefreshRec>,
    access: HashMap<String, AccessRec>,
    issued: u64,
    presented: Vec<String>,
    invalid_grants: u64,
}

struct Fixture {
    base: String,
    mas: Arc<Mutex<Mas>>,
    /// One clock for the server's "first used" and the keyring's "written".
    seq: Arc<AtomicU64>,
}

impl Fixture {
    fn first_use(&self, access: &str) -> Option<u64> {
        self.mas.lock().unwrap().access.get(access).and_then(|a| a.used_at)
    }
    fn invalid_grants(&self) -> u64 {
        self.mas.lock().unwrap().invalid_grants
    }
    fn presented(&self) -> Vec<String> {
        self.mas.lock().unwrap().presented.clone()
    }
}

/// Request line + headers, and the body.
fn read_request(stream: &mut TcpStream) -> (String, String) {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    let head_end = loop {
        let n = stream.read(&mut chunk).unwrap_or(0);
        if n == 0 {
            return (String::new(), String::new());
        }
        buf.extend_from_slice(&chunk[..n]);
        if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break pos + 4;
        }
    };
    let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
    let length = head
        .lines()
        .find_map(|line| {
            line.to_ascii_lowercase()
                .strip_prefix("content-length:")
                .map(|v| v.trim().parse::<usize>().unwrap_or(0))
        })
        .unwrap_or(0);
    while buf.len() < head_end + length {
        let n = stream.read(&mut chunk).unwrap_or(0);
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
    let body = String::from_utf8_lossy(&buf[head_end..]).to_string();
    (head, body)
}

fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
    let response = format!(
        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
         Connection: close\r\nContent-Length: {}\r\n\r\n{}",
        payload.len(),
        payload
    );
    let _ = stream.write_all(response.as_bytes());
}

fn bearer(head: &str) -> Option<String> {
    head.lines().find_map(|line| {
        let lower = line.to_ascii_lowercase();
        lower.starts_with("authorization:").then(|| {
            line.split_whitespace().last().unwrap_or("").to_owned()
        })
    })
}

fn form_value(body: &str, key: &str) -> Option<String> {
    body.split('&').find_map(|pair| {
        let (k, v) = pair.split_once('=')?;
        (k == key).then(|| v.replace("%2D", "-").replace('+', " "))
    })
}

/// MAS's `refresh_token_grant`, reduced to the token states.
fn refresh_grant(mas: &mut Mas, presented: &str) -> Result<(String, String), &'static str> {
    mas.presented.push(presented.to_owned());
    let Some(rec) = mas.refresh.get(presented) else { return Err("not found") };
    if rec.revoked {
        return Err("revoked");
    }
    if rec.consumed {
        let next = rec.next.clone().expect("a consumed token names its successor");
        let next_rec = mas.refresh.get(&next).expect("successor exists");
        if next_rec.consumed || next_rec.revoked {
            return Err("replay: the successor was used");
        }
        let next_access = next_rec.access.clone();
        if mas.access.get(&next_access).is_some_and(|a| a.used_at.is_some()) {
            return Err("replay: the successor's access token was used");
        }
        // A lost response: revoke the unused pair and reissue.
        if let Some(a) = mas.access.get_mut(&next_access) {
            a.revoked = true;
        }
        if let Some(r) = mas.refresh.get_mut(&next) {
            r.revoked = true;
        }
    }
    mas.issued += 1;
    let n = mas.issued;
    let (access, refresh) = (format!("access-{n}"), format!("refresh-{n}"));
    mas.access.insert(access.clone(), AccessRec::default());
    mas.refresh.insert(
        refresh.clone(),
        RefreshRec { access: access.clone(), next: None, consumed: false, revoked: false },
    );
    let rec = mas.refresh.get_mut(presented).expect("checked above");
    rec.consumed = true;
    rec.next = Some(refresh.clone());
    let old_access = rec.access.clone();
    if let Some(a) = mas.access.get_mut(&old_access) {
        a.revoked = true;
    }
    Ok((access, refresh))
}

fn serve() -> Fixture {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
    let base = format!("http://{}", listener.local_addr().expect("addr"));
    let mut mas = Mas::default();
    // The pair in the keyring at the start: its access token has expired.
    mas.access.insert("access-0".to_owned(), AccessRec { used_at: None, revoked: true });
    mas.refresh.insert(
        "refresh-0".to_owned(),
        RefreshRec { access: "access-0".to_owned(), next: None, consumed: false, revoked: false },
    );
    let mas = Arc::new(Mutex::new(mas));
    let seq = Arc::new(AtomicU64::new(1));
    let metadata = format!(
        r#"{{"issuer":"{base}/","authorization_endpoint":"{base}/oauth2/authorize","token_endpoint":"{base}/oauth2/token","registration_endpoint":"{base}/oauth2/registration","revocation_endpoint":"{base}/oauth2/revoke","response_types_supported":["code"],"response_modes_supported":["query","fragment"],"grant_types_supported":["authorization_code","refresh_token"],"code_challenge_methods_supported":["S256"]}}"#
    );
    let (served, clock) = (Arc::clone(&mas), Arc::clone(&seq));
    thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(mut stream) = stream else { break };
            let (mas, clock, metadata) = (Arc::clone(&served), Arc::clone(&clock), metadata.clone());
            thread::spawn(move || {
                let (head, body) = read_request(&mut stream);
                let path = head
                    .lines()
                    .next()
                    .and_then(|line| line.split_whitespace().nth(1))
                    .unwrap_or("")
                    .to_owned();
                if path.starts_with("/_matrix/client/versions") {
                    respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                } else if path.starts_with("/.well-known") {
                    respond(&mut stream, "404 Not Found", r#"{"errcode":"M_NOT_FOUND","error":"none"}"#);
                } else if path.contains("auth_metadata") {
                    respond(&mut stream, "200 OK", &metadata);
                } else if path.starts_with("/oauth2/token") {
                    let presented = form_value(&body, "refresh_token").unwrap_or_default();
                    let mut mas = mas.lock().unwrap();
                    match refresh_grant(&mut mas, &presented) {
                        Ok((access, refresh)) => respond(&mut stream, "200 OK", &format!(
                            r#"{{"access_token":"{access}","token_type":"Bearer","expires_in":300,"refresh_token":"{refresh}"}}"#)),
                        Err(why) => {
                            mas.invalid_grants += 1;
                            respond(&mut stream, "400 Bad Request", &format!(
                                r#"{{"error":"invalid_grant","error_description":"{why}"}}"#));
                        }
                    }
                } else if path.starts_with("/_matrix/client/v3/account/whoami") {
                    let token = bearer(&head).unwrap_or_default();
                    let mut mas = mas.lock().unwrap();
                    let live = mas.access.get_mut(&token).filter(|a| !a.revoked);
                    if let Some(rec) = live {
                        if rec.used_at.is_none() {
                            rec.used_at = Some(clock.fetch_add(1, Ordering::SeqCst));
                        }
                        respond(&mut stream, "200 OK",
                                r#"{"user_id":"@me:example.org","device_id":"TESTDEVICE"}"#);
                    } else {
                        respond(&mut stream, "401 Unauthorized",
                                r#"{"errcode":"M_UNKNOWN_TOKEN","error":"Token is not active","soft_logout":false}"#);
                    }
                } else {
                    respond(&mut stream, "200 OK", "{}");
                }
            });
        }
    });
    Fixture { base, mas, seq }
}

// ---- the stand-in for C++ -------------------------------------------------

#[derive(Default)]
struct Keyring {
    access: String,
    refresh: String,
    /// Fixture clock value when each refresh token was written.
    written_at: HashMap<String, u64>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Gui {
    /// Writes each rotation and answers, like RustSdkMatrixClient.
    Persist,
    /// Drains (so no stall is seen) and never answers: the process dies in
    /// the middle of the keyring write.
    DiesBeforeTheWrite,
}

struct GuiThread {
    keyring: Arc<Mutex<Keyring>>,
    seen: Arc<AtomicU64>,
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl GuiThread {
    fn start(sink: &Sink, fixture: &Fixture, mode: Gui, drain_every: Duration) -> Self {
        let keyring = Arc::new(Mutex::new(Keyring {
            access: "access-0".to_owned(),
            refresh: "refresh-0".to_owned(),
            ..Keyring::default()
        }));
        let seen = Arc::new(AtomicU64::new(0));
        let stop = Arc::new(AtomicBool::new(false));
        let (ledger, lane, clock) =
            (Arc::clone(&sink.ledger), Arc::clone(&sink.lane), Arc::clone(&fixture.seq));
        let (ring, seen2, stop2) = (Arc::clone(&keyring), Arc::clone(&seen), Arc::clone(&stop));
        let handle = thread::spawn(move || {
            while !stop2.load(Ordering::SeqCst) {
                thread::sleep(drain_every);
                ledger.note_drain();
                let drained: Vec<String> = lane.lock().unwrap().drain(..).collect();
                for raw in drained {
                    let event: serde_json::Value = serde_json::from_str(&raw).unwrap();
                    if event["type"] != "session_tokens_refreshed" {
                        continue;
                    }
                    let generation = event["generation"].as_u64().expect("generation");
                    seen2.store(generation, Ordering::SeqCst);
                    if mode == Gui::DiesBeforeTheWrite {
                        continue;
                    }
                    let mut ring = ring.lock().unwrap();
                    ring.access = event["access_token"].as_str().unwrap().to_owned();
                    ring.refresh = event["refresh_token"].as_str().unwrap().to_owned();
                    let refresh = ring.refresh.clone();
                    ring.written_at.insert(refresh, clock.fetch_add(1, Ordering::SeqCst));
                    drop(ring);
                    ledger.answer(generation, true);
                }
            }
        });
        Self { keyring, seen, stop, handle: Some(handle) }
    }

    fn wait_seen(&self, generation: u64, budget: Duration) -> bool {
        let deadline = Instant::now() + budget;
        while Instant::now() < deadline {
            if self.seen.load(Ordering::SeqCst) >= generation {
                return true;
            }
            thread::sleep(Duration::from_millis(5));
        }
        false
    }
}

impl Drop for GuiThread {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

// ---- helpers ----------------------------------------------------------------

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .expect("runtime")
}

fn store_dir(name: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(format!(
        "lightning-tokens-{name}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or_default()
    ))
}

fn queue() -> EventQueueRef {
    Arc::new(Mutex::new(VecDeque::new()))
}

fn new_sink(ledger: Arc<TokenLedger>) -> Sink {
    Sink { ledger, lane: queue(), events: queue() }
}

fn session(access: &str, refresh: &str) -> OAuthSession {
    OAuthSession {
        client_id: ClientId::new("client".to_owned()),
        user: UserSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                device_id: OwnedDeviceId::from("TESTDEVICE"),
            },
            tokens: SessionTokens {
                access_token: access.to_owned(),
                refresh_token: Some(refresh.to_owned()),
            },
        },
    }
}

/// `mx_rust_oauth_restore`'s client: the production restore with the handle's
/// sink installed before `restore_session()`.
async fn restore(fixture: &Fixture, store: &std::path::Path, sink: &Sink, access: &str, refresh: &str)
    -> Client
{
    let (client, _changes) = crate::oauth::restore_oauth_client(
        &fixture.base, store, session(access, refresh), &sink.events, Some(sink),
    )
    .await
    .expect("restore");
    client
}

// ---- the ledger's bounds ------------------------------------------------------

fn tokens(n: u32) -> SessionTokens {
    SessionTokens { access_token: format!("a{n}"), refresh_token: Some(format!("r{n}")) }
}

#[test]
fn an_answer_ends_the_wait_and_says_whether_the_keyring_took_it() {
    let ledger = TokenLedger::with_bounds(true, Duration::from_secs(5), Duration::from_secs(5));
    let generation = ledger.record(tokens(1));
    let answering = Arc::clone(&ledger);
    let t = thread::spawn(move || {
        thread::sleep(Duration::from_millis(50));
        answering.answer(generation, true);
    });
    assert_eq!(ledger.wait_for_answer(generation), WaitOutcome::Saved);
    t.join().unwrap();

    let refused = ledger.record(tokens(2));
    ledger.answer(refused, false);
    assert_eq!(ledger.wait_for_answer(refused), WaitOutcome::NotSaved);
    // An answer for a later pair answers an earlier one: the keyring then
    // holds something at least as new.
    let older = ledger.record(tokens(3));
    let newer = ledger.record(tokens(4));
    ledger.answer(newer, true);
    assert_eq!(ledger.wait_for_answer(older), WaitOutcome::Saved);
    assert_eq!(ledger.latest(), Some(tokens(4)));
}

#[test]
fn a_gui_that_stops_draining_releases_the_refresh() {
    let ledger = TokenLedger::with_bounds(true, Duration::from_secs(10), Duration::from_millis(150));
    let generation = ledger.record(tokens(1));
    let started = Instant::now();
    assert_eq!(ledger.wait_for_answer(generation), WaitOutcome::Stalled);
    let waited = started.elapsed();
    assert!(waited >= Duration::from_millis(150) && waited < Duration::from_secs(2), "{waited:?}");
}

#[test]
fn a_draining_gui_that_never_answers_is_bounded_by_the_cap() {
    let ledger = TokenLedger::with_bounds(true, Duration::from_millis(400), Duration::from_millis(150));
    let generation = ledger.record(tokens(1));
    let stop = Arc::new(AtomicBool::new(false));
    let (draining, stop2) = (Arc::clone(&ledger), Arc::clone(&stop));
    let t = thread::spawn(move || {
        while !stop2.load(Ordering::SeqCst) {
            draining.note_drain();
            thread::sleep(Duration::from_millis(20));
        }
    });
    let started = Instant::now();
    assert_eq!(ledger.wait_for_answer(generation), WaitOutcome::TimedOut);
    assert!(started.elapsed() < Duration::from_secs(3));
    stop.store(true, Ordering::SeqCst);
    t.join().unwrap();
}

#[test]
fn shutdown_and_detached_ledgers_never_hold_a_refresh() {
    let ledger = TokenLedger::with_bounds(true, Duration::from_secs(10), Duration::from_secs(10));
    let generation = ledger.record(tokens(1));
    let stopping = Arc::clone(&ledger);
    let t = thread::spawn(move || {
        thread::sleep(Duration::from_millis(50));
        stopping.stop_waiting();
    });
    let started = Instant::now();
    assert_eq!(ledger.wait_for_answer(generation), WaitOutcome::NoWait);
    assert!(started.elapsed() < Duration::from_secs(2));
    t.join().unwrap();

    let detached = TokenLedger::detached();
    let generation = detached.record(tokens(1));
    assert_eq!(detached.wait_for_answer(generation), WaitOutcome::NoWait);
}

#[test]
fn a_current_thread_runtime_waits_for_the_answer_like_any_other() {
    // The sync loop's runtime is current-thread (lib.rs run_async), and the
    // answer needs nothing from it.
    let ledger = TokenLedger::with_bounds(true, Duration::from_secs(10), Duration::from_secs(10));
    let generation = ledger.record(tokens(1));
    let answering = Arc::clone(&ledger);
    let t = thread::spawn(move || {
        thread::sleep(Duration::from_millis(100));
        answering.answer(generation, true);
    });
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    let started = Instant::now();
    let outcome = rt.block_on(async { wait_off_runtime(&ledger, generation) });
    assert_eq!(outcome, WaitOutcome::Saved);
    assert!(started.elapsed() >= Duration::from_millis(100));
    t.join().unwrap();
}

#[test]
fn the_draining_thread_never_waits_for_itself() {
    let ledger = TokenLedger::with_bounds(true, Duration::from_secs(10), Duration::from_secs(10));
    ledger.note_drain();
    let generation = ledger.record(tokens(1));
    let started = Instant::now();
    assert_eq!(wait_off_runtime(&ledger, generation), WaitOutcome::NoWait);
    assert!(started.elapsed() < Duration::from_secs(1));
}

#[test]
fn a_new_client_starts_with_no_pair_and_counts_its_own_rotations() {
    let ledger = TokenLedger::detached();
    ledger.record(tokens(1));
    ledger.answer(1, true);
    ledger.record(tokens(2));
    let stats = ledger.stats();
    assert_eq!((stats.rotations, stats.unsaved_rotation), (2, true));
    ledger.begin_client(false);
    assert_eq!(ledger.latest(), None);
    let stats = ledger.stats();
    assert_eq!((stats.rotations, stats.unsaved_rotation, stats.since_rotation_ms), (0, false, None));
    // Generations keep counting, so an answer about the previous client can
    // never satisfy this one.
    assert_eq!(ledger.record(tokens(3)), 3);
}

// ---- against the MAS-rule server -----------------------------------------------

/// The ordering MAS needs: the request whose 401 caused a refresh is retried
/// with the new access token only after the new pair is in the keyring.
/// Without the save callback's wait the retry goes out at once and the stand-in
/// GUI writes up to 100 ms later, so "first use" precedes "written".
#[test]
fn the_retried_request_waits_until_the_rotated_pair_is_in_the_keyring() {
    let fixture = serve();
    let store = store_dir("order");
    let sink = new_sink(TokenLedger::new());
    let gui = GuiThread::start(&sink, &fixture, Gui::Persist, Duration::from_millis(100));
    runtime().block_on(async {
        let client = restore(&fixture, &store, &sink, "access-0", "refresh-0").await;
        client.whoami().await.expect("whoami after one refresh");
    });
    let written = gui.keyring.lock().unwrap().written_at.get("refresh-1").copied();
    let used = fixture.first_use("access-1");
    assert!(written.is_some(), "the rotation never reached the keyring");
    assert!(used.is_some(), "the retried whoami never carried the new access token");
    assert!(written < used,
            "access-1 was used (t={used:?}) before refresh-1 was written (t={written:?})");
    // Announced on the command lane, not behind the bulk lane's diffs.
    let bulk: Vec<String> = sink.events.lock().unwrap().iter().cloned().collect();
    assert!(bulk.iter().all(|e| !e.contains("session_tokens_refreshed")), "{bulk:?}");
    drop(gui);
    let _ = std::fs::remove_dir_all(&store);
}

/// The same ordering when the 401 comes from a request made on a
/// current-thread runtime, as the sync loop's are (lib.rs `run_async`), on a
/// client built on the handle's multi-thread runtime.
#[test]
fn the_sync_loops_runtime_also_waits_for_the_keyring() {
    let fixture = serve();
    let store = store_dir("order-sync");
    let sink = new_sink(TokenLedger::new());
    let gui = GuiThread::start(&sink, &fixture, Gui::Persist, Duration::from_millis(100));
    let shared = runtime();
    let client = shared.block_on(restore(&fixture, &store, &sink, "access-0", "refresh-0"));
    let sync_client = client.clone();
    thread::spawn(move || {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
            .block_on(async move { sync_client.whoami().await.map(|_| ()) })
    })
    .join()
    .unwrap()
    .expect("whoami from the sync-shaped runtime");
    let written = gui.keyring.lock().unwrap().written_at.get("refresh-1").copied();
    let used = fixture.first_use("access-1");
    assert!(written.is_some() && used.is_some(), "written={written:?} used={used:?}");
    assert!(written < used,
            "access-1 was used (t={used:?}) before refresh-1 was written (t={written:?})");
    drop(client);
    drop(gui);
    shared.shutdown_timeout(Duration::from_secs(5));
    let _ = std::fs::remove_dir_all(&store);
}

/// A crash (or kill) after the SDK rotated and before the keyring write: the
/// next start presents the old refresh token, and MAS must still accept it.
/// It does only while the new access token is unused, which the wait
/// guarantees for the retried request. Without the wait the retry has used
/// access-1 long before the GUI sees the rotation, and the restart's refresh
/// is `invalid_grant`: "This session was signed out remotely".
#[test]
fn a_crash_before_the_keyring_write_leaves_the_old_refresh_token_usable() {
    let fixture = serve();
    let (store, restart_store) = (store_dir("crash"), store_dir("crash-restart"));
    let doomed = new_sink(TokenLedger::with_bounds(true, Duration::from_secs(10), DRAIN_STALL));
    let gui = GuiThread::start(&doomed, &fixture, Gui::DiesBeforeTheWrite, Duration::from_millis(100));
    let old = runtime();
    let old_ledger = Arc::clone(&doomed.ledger);
    let (base, mas, seq) = (fixture.base.clone(), Arc::clone(&fixture.mas), Arc::clone(&fixture.seq));
    let doomed_store = store.clone();
    let doomed_sink = Sink {
        ledger: Arc::clone(&doomed.ledger),
        lane: Arc::clone(&doomed.lane),
        events: Arc::clone(&doomed.events),
    };
    old.spawn(async move {
        let fixture = Fixture { base, mas, seq };
        let client = restore(&fixture, &doomed_store, &doomed_sink, "access-0", "refresh-0").await;
        let _ = client.whoami().await;
    });
    assert!(gui.wait_seen(1, Duration::from_secs(20)), "the rotation was never announced");
    // The process dies here. What the keyring holds is all the next start has.
    let ring = gui.keyring.lock().unwrap();
    let (access, refresh) = (ring.access.clone(), ring.refresh.clone());
    drop(ring);
    assert_eq!(refresh, "refresh-0", "the stand-in wrote although it was told not to");
    assert_eq!(fixture.first_use("access-1"), None,
               "the rotated access token was used before the keyring held its pair");

    let restart = new_sink(TokenLedger::detached());
    let restarted = runtime().block_on(async {
        let client = restore(&fixture, &restart_store, &restart, &access, &refresh).await;
        client.whoami().await.map(|r| r.user_id.to_string())
    });
    assert_eq!(restarted.ok().as_deref(), Some("@me:example.org"),
               "the restart's refresh was refused: invalid_grant={}", fixture.invalid_grants());
    assert_eq!(fixture.invalid_grants(), 0);

    old_ledger.stop_waiting();
    drop(gui);
    old.shutdown_timeout(Duration::from_secs(5));
    let _ = std::fs::remove_dir_all(&store);
    let _ = std::fs::remove_dir_all(&restart_store);
}

/// The normal path: rotations are written, the next start uses the newest
/// pair, and no refresh token is ever presented twice.
#[test]
fn a_persisted_rotation_restarts_without_presenting_a_used_refresh_token() {
    let fixture = serve();
    let (store, restart_store) = (store_dir("persist"), store_dir("persist-restart"));
    let sink = new_sink(TokenLedger::new());
    let gui = GuiThread::start(&sink, &fixture, Gui::Persist, Duration::from_millis(100));
    runtime().block_on(async {
        let client = restore(&fixture, &store, &sink, "access-0", "refresh-0").await;
        client.whoami().await.expect("whoami");
        // A second rotation in the same session (the access token expired
        // again), driven directly.
        client.refresh_access_token().await.expect("second refresh");
    });
    let deadline = Instant::now() + Duration::from_secs(5);
    while gui.keyring.lock().unwrap().refresh != "refresh-2" && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(20));
    }
    let (access, refresh) = {
        let ring = gui.keyring.lock().unwrap();
        (ring.access.clone(), ring.refresh.clone())
    };
    assert_eq!(refresh, "refresh-2", "the keyring kept an older pair");
    drop(gui);

    let restart = new_sink(TokenLedger::detached());
    runtime().block_on(async {
        let client = restore(&fixture, &restart_store, &restart, &access, &refresh).await;
        // access-2 is live: no refresh is needed at all.
        client.whoami().await.expect("restart");
        client.refresh_access_token().await.expect("restart refresh");
    });
    let presented = fixture.presented();
    let mut unique = presented.clone();
    unique.sort();
    unique.dedup();
    assert_eq!(presented.len(), unique.len(), "a refresh token was presented twice: {presented:?}");
    assert_eq!(fixture.invalid_grants(), 0);
    let _ = std::fs::remove_dir_all(&store);
    let _ = std::fs::remove_dir_all(&restart_store);
}

/// The retirement hole: matrix-sdk's own tasks can refresh after Lightning's
/// broadcast watcher is aborted and after the final tokens were read. The
/// save callback records every rotation in the ledger, which outlives the
/// client, so the worker's read after the runtime is gone has the newest pair.
#[test]
fn a_rotation_nobody_watches_is_still_in_the_ledger_after_the_client_is_gone() {
    let fixture = serve();
    let store = store_dir("retire");
    let sink = new_sink(TokenLedger::new());
    // Nothing drains or answers: shutdown has begun.
    let rt = runtime();
    let latest_seen_by_client = rt.block_on(async {
        let client = restore(&fixture, &store, &sink, "access-0", "refresh-0").await;
        sink.ledger.stop_waiting();
        client.refresh_access_token().await.expect("refresh during shutdown");
        client.session_tokens().and_then(|t| t.refresh_token)
    });
    rt.shutdown_timeout(Duration::from_secs(5));
    assert_eq!(latest_seen_by_client.as_deref(), Some("refresh-1"));
    assert_eq!(sink.ledger.latest().and_then(|t| t.refresh_token).as_deref(), Some("refresh-1"));
    let _ = std::fs::remove_dir_all(&store);
}
