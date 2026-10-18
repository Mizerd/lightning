//! Rotated session tokens, captured inside matrix-sdk's own refresh.
//!
//! matrix.org's authorization server (MAS) rotates the refresh token on every
//! refresh and accepts a consumed one again ONLY while the access token issued
//! with its replacement has never been used (`refresh_token_grant` in MAS's
//! `oauth2/token.rs`). matrix-sdk retries the request whose 401 caused the
//! refresh straight after it, with the new access token. So a rotation that
//! never reaches the keyring is not a harmless lag: the next start presents
//! the consumed refresh token, MAS answers `invalid_grant`, the SDK broadcasts
//! `UnknownToken`, and the account reads "This session was signed out
//! remotely" while its device still exists on the server.
//!
//! The SDK's intended hook is `Client::set_session_callbacks`: its save
//! callback runs synchronously inside the refresh, after the new pair is
//! adopted and before the refresh returns (so before that retry), with the
//! refresh lock held (matrix-sdk 0.18 `refresh_access_token_inner`, and
//! `MatrixAuth::refresh_access_token` for refreshable password sessions).
//! Lightning's keyring is written by C++ on the GUI thread, and Rust never
//! calls into C++, so the callback here:
//!
//! 1. records the pair and a generation in this handle's [`TokenLedger`],
//!    which outlives the `Client` (the retiring worker reads it after the
//!    runtime is shut down, when no task can rotate any more);
//! 2. announces it as `session_tokens_refreshed` on the COMMAND lane, which
//!    C++ drains completely before every bulk batch, instead of behind a sync
//!    diff flood on the bulk lane (≤256 per 100 ms tick, oldest dropped past
//!    4096);
//! 3. waits, bounded, for C++ to say the keyring write finished
//!    (`mx_rust_session_tokens_persisted`). Only then does the SDK retry with
//!    the new access token, so a crash or kill before the write leaves that
//!    token unused and MAS accepts the old refresh token again.
//!
//! The wait is bounded three ways so it can never wedge sync: C++ answering
//! (saved or not), C++ not draining at all for [`DRAIN_STALL`] (the GUI thread
//! is blocked, possibly on this runtime), and [`WAIT_CAP`] overall. It never
//! waits on the GUI thread itself, nor once shutdown has begun.
//! Requests the SDK starts from OTHER tasks while the callback waits read the
//! new access token already (the SDK adopts it before calling back), so the
//! guarantee covers the retried request, not those.
//!
//! The reload callback exists because the SDK takes both. It is only called
//! by the cross-process refresh lock, which Lightning does not enable: one
//! process owns a profile (SingleInstanceGuard, `storelocking.rs`), and
//! another install has its own store, device and MAS session.
//!
//! SENSITIVE: the ledger holds tokens. Nothing here logs them, nor a digest of
//! them; the trace says generations, counts and milliseconds only.

use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use matrix_sdk::{Client, SessionTokens};
use serde_json::json;

use crate::EventQueueRef;

/// The longest the save callback holds the refresh open for C++.
pub(crate) const WAIT_CAP: Duration = Duration::from_millis(3000);

/// No drain for this long since the wait began: the GUI thread is not polling
/// (blocked, possibly on this very runtime), so no answer is coming soon.
/// C++ drains every 100 ms when it is free.
pub(crate) const DRAIN_STALL: Duration = Duration::from_millis(500);

/// How a save callback's wait ended. A word for the trace.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum WaitOutcome {
    /// C++ wrote the pair to the keyring.
    Saved,
    /// C++ answered that the keyring refused the write (or no account owns it).
    NotSaved,
    /// C++ stopped draining; the SDK goes on.
    Stalled,
    /// `WAIT_CAP` passed.
    TimedOut,
    /// Not waited: shutdown, a ledger without a C++ sink, or a refresh on the
    /// draining (GUI) thread.
    NoWait,
}

impl WaitOutcome {
    pub(crate) fn word(self) -> &'static str {
        match self {
            WaitOutcome::Saved => "saved",
            WaitOutcome::NotSaved => "not_saved",
            WaitOutcome::Stalled => "drain_stalled",
            WaitOutcome::TimedOut => "timed_out",
            WaitOutcome::NoWait => "no_wait",
        }
    }
}

#[derive(Default)]
struct LedgerState {
    /// The pair the current client last rotated to; `None` until it rotates.
    latest: Option<SessionTokens>,
    /// Rotations recorded on this handle, across its clients. Never reset, so
    /// an answer about an earlier client can never satisfy a later one.
    generation: u64,
    /// Generation of the current client's first rotation minus one.
    client_base: u64,
    /// Highest generation C++ answered for, and whether that write succeeded.
    answered: u64,
    answered_ok: bool,
    /// Highest generation C++ confirmed in the keyring.
    saved: u64,
    /// When the last rotation was recorded.
    last_rotation: Option<Instant>,
    /// False: never wait (no C++ sink, or shutdown has begun).
    waits: bool,
    /// The current client's save callback is installed, so it announces
    /// rotations and the broadcast watcher must not announce them again.
    callbacks_active: bool,
    /// When C++ last drained the command lane, and from which thread (the
    /// GUI thread): a refresh running ON that thread can never be answered.
    last_drain: Option<Instant>,
    drain_thread: Option<std::thread::ThreadId>,
}

/// One handle's rotations. Shared by the handle, its client's save callback,
/// and (after retirement) the C++ worker that writes the final pair back.
pub(crate) struct TokenLedger {
    state: Mutex<LedgerState>,
    answered: Condvar,
    wait_cap: Duration,
    drain_stall: Duration,
}

/// Counts for the "signed out" log line and the trace. No secret.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct LedgerStats {
    /// Rotations of the current client.
    pub rotations: u64,
    /// The current client rotated past the last pair C++ confirmed saved.
    pub unsaved_rotation: bool,
    /// Milliseconds since the last rotation; `None` if it never rotated.
    pub since_rotation_ms: Option<u64>,
}

impl TokenLedger {
    /// A handle's ledger: C++ drains its lane and answers, so rotations wait.
    pub(crate) fn new() -> Arc<Self> {
        Self::with_bounds(true, WAIT_CAP, DRAIN_STALL)
    }

    /// A ledger nobody answers (in-memory helper clients, tests that only
    /// read what was recorded): rotations are recorded and never wait.
    #[cfg(test)]
    pub(crate) fn detached() -> Arc<Self> {
        Self::with_bounds(false, WAIT_CAP, DRAIN_STALL)
    }

    pub(crate) fn with_bounds(waits: bool, wait_cap: Duration, drain_stall: Duration) -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(LedgerState { waits, ..LedgerState::default() }),
            answered: Condvar::new(),
            wait_cap,
            drain_stall,
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, LedgerState> {
        // A poisoned ledger still holds the newest pair; keep using it.
        self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// A new client takes over this handle: what the previous one rotated to
    /// is not this one's. Waiting is armed again for a ledger with a sink.
    fn begin_client(&self, waits: bool) {
        let mut state = self.lock();
        state.latest = None;
        state.client_base = state.generation;
        state.last_rotation = None;
        state.waits = waits;
        state.callbacks_active = false;
    }

    /// Whether the current client's rotations are announced by its save
    /// callback (`install` succeeded).
    pub(crate) fn callbacks_active(&self) -> bool {
        self.lock().callbacks_active
    }

    /// Record a rotation; returns its generation.
    pub(crate) fn record(&self, tokens: SessionTokens) -> u64 {
        let mut state = self.lock();
        state.generation += 1;
        state.latest = Some(tokens);
        state.last_rotation = Some(Instant::now());
        state.generation
    }

    /// C++'s answer for `generation`: written to the keyring (`saved`), or not.
    pub(crate) fn answer(&self, generation: u64, saved: bool) {
        let mut state = self.lock();
        if generation >= state.answered {
            state.answered = generation;
            state.answered_ok = saved;
        }
        if saved && generation > state.saved {
            state.saved = generation;
        }
        drop(state);
        self.answered.notify_all();
    }

    /// C++ drained the command lane (called on every poll of it).
    pub(crate) fn note_drain(&self) {
        let mut state = self.lock();
        state.last_drain = Some(Instant::now());
        state.drain_thread = Some(std::thread::current().id());
    }

    /// Shutdown has begun: nothing will answer; never hold a refresh again
    /// until a new client is installed.
    pub(crate) fn stop_waiting(&self) {
        self.lock().waits = false;
        self.answered.notify_all();
    }

    /// The pair the current client last rotated to. SENSITIVE.
    pub(crate) fn latest(&self) -> Option<SessionTokens> {
        self.lock().latest.clone()
    }

    pub(crate) fn stats(&self) -> LedgerStats {
        let state = self.lock();
        LedgerStats {
            rotations: state.generation - state.client_base,
            unsaved_rotation: state.generation > state.client_base
                && state.saved < state.generation,
            since_rotation_ms: state
                .last_rotation
                .map(|at| at.elapsed().as_millis().min(u128::from(u64::MAX)) as u64),
        }
    }

    /// Block until C++ answers for `generation`, or a bound ends the wait.
    /// Runs on whatever thread the SDK's refresh runs on.
    pub(crate) fn wait_for_answer(&self, generation: u64) -> WaitOutcome {
        let started = Instant::now();
        let mut state = self.lock();
        loop {
            if state.answered >= generation {
                // An answer for a later generation answers this one too: the
                // keyring then holds a pair at least as new.
                return if state.saved >= generation || state.answered_ok {
                    WaitOutcome::Saved
                } else {
                    WaitOutcome::NotSaved
                };
            }
            if !state.waits {
                return WaitOutcome::NoWait;
            }
            let now = Instant::now();
            if now.duration_since(started) >= self.wait_cap {
                return WaitOutcome::TimedOut;
            }
            let drained_since = state.last_drain.map_or(started, |at| at.max(started));
            if now.duration_since(drained_since) >= self.drain_stall {
                return WaitOutcome::Stalled;
            }
            let (next, _) = self
                .answered
                .wait_timeout(state, Duration::from_millis(25))
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            state = next;
        }
    }

    /// Whether a rotation on this thread may wait: never on the thread that
    /// drains (it would be waiting for itself), never with waiting off.
    fn may_wait_here(&self) -> bool {
        let state = self.lock();
        state.waits && state.drain_thread != Some(std::thread::current().id())
    }
}

/// Whether the opt-in token trace is on: LIGHTNING_TOKEN_TRACE, the same rule
/// as the other traces (set and not "", "0", "false", "off", "no"). Read once.
pub(crate) fn token_trace_enabled() -> bool {
    use std::sync::OnceLock;
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        crate::switchtrace::switch_trace_enabled_from(
            std::env::var("LIGHTNING_TOKEN_TRACE").ok().as_deref(),
        )
    })
}

/// Enqueue a `token_trace` event on the bulk lane when the trace is on.
/// `fields` must hold no secret: words, counts, milliseconds.
pub(crate) fn trace(events: &EventQueueRef, stage: &str, mut fields: serde_json::Value) {
    if !token_trace_enabled() {
        return;
    }
    if let Some(map) = fields.as_object_mut() {
        map.insert("type".to_owned(), json!("token_trace"));
        map.insert("stage".to_owned(), json!(stage));
    }
    crate::enqueue(events, fields);
}

/// Wait for C++ from inside the SDK's refresh. The answer depends on no Rust
/// runtime: the GUI thread drains the command lane, writes the keyring and
/// answers through a std mutex. So the wait may block whichever thread runs
/// the refresh, a current-thread runtime's included, which is where most
/// refreshes happen (the sync loop's own runtime, `run_async`); on a
/// multi-thread runtime it goes through `block_in_place` so the worker's
/// other tasks move on. The one thread that must not wait is the one that
/// drains: a refresh run from the GUI thread would be waiting for itself.
/// A GUI thread blocked on this very sync (stop_sync_and_wait) stops
/// draining, and `DRAIN_STALL` releases the wait.
fn wait_off_runtime(ledger: &TokenLedger, generation: u64) -> WaitOutcome {
    if !ledger.may_wait_here() {
        return WaitOutcome::NoWait;
    }
    match tokio::runtime::Handle::try_current() {
        Ok(handle) if handle.runtime_flavor() == tokio::runtime::RuntimeFlavor::MultiThread => {
            tokio::task::block_in_place(|| ledger.wait_for_answer(generation))
        }
        _ => ledger.wait_for_answer(generation),
    }
}

type SaveCallback =
    Box<dyn Fn(Client) -> Result<(), Box<dyn std::error::Error + Send + Sync>> + Send + Sync>;
type ReloadCallback = Box<
    dyn Fn(Client) -> Result<SessionTokens, Box<dyn std::error::Error + Send + Sync>>
        + Send
        + Sync,
>;

/// Install the session callbacks on a freshly built client, BEFORE its
/// session is restored or logged in, so every rotation it ever makes passes
/// through `ledger`. `lane` is the handle's command lane; `events` its bulk
/// lane (trace only). Each client is installed once (the SDK refuses a
/// second); an error is reported to the caller, which goes on: the broadcast
/// watcher (`oauth::spawn_token_persistence_with`) still announces rotations.
pub(crate) fn install(
    client: &Client,
    ledger: &Arc<TokenLedger>,
    lane: &EventQueueRef,
    events: &EventQueueRef,
    waits: bool,
) -> Result<(), String> {
    ledger.begin_client(waits);
    let save_ledger = Arc::clone(ledger);
    let save_lane = Arc::clone(lane);
    let save_events = Arc::clone(events);
    let save: SaveCallback = Box::new(move |client: Client| {
        // The SDK has just adopted the pair (its documented invariant).
        let Some(tokens) = client.session_tokens() else {
            return Err("no session tokens after a refresh".into());
        };
        let generation = save_ledger.record(tokens.clone());
        // SENSITIVE: carries the rotated pair; C++ writes it and logs neither.
        crate::enqueue_terminal_routed(
            &save_lane,
            None,
            None,
            json!({
                "type": "session_tokens_refreshed",
                "access_token": tokens.access_token,
                "refresh_token": tokens.refresh_token,
                "generation": generation,
            }),
        );
        let started = Instant::now();
        let outcome = wait_off_runtime(&save_ledger, generation);
        trace(
            &save_events,
            "rotated",
            json!({
                "generation": generation,
                "has_refresh_token": tokens.refresh_token.is_some(),
                "wait": outcome.word(),
                "wait_ms": started.elapsed().as_millis() as u64,
            }),
        );
        Ok(())
    });
    let reload: ReloadCallback = Box::new(|_client: Client| {
        Err("Lightning keeps no cross-process refresh lock; nothing to reload".into())
    });
    client
        .set_session_callbacks(reload, save)
        .map_err(|err| format!("session callbacks not installed: {err}"))?;
    ledger.lock().callbacks_active = true;
    Ok(())
}

/// Where a handle's client sends its rotations: the handle's ledger and its
/// command lane, plus the bulk lane for the trace. Passed to the restore and
/// login paths, which install it on the client they build.
pub(crate) struct Sink {
    pub ledger: Arc<TokenLedger>,
    pub lane: EventQueueRef,
    pub events: EventQueueRef,
}

impl Sink {
    /// Install on a freshly built client. A failure is reported on the trace
    /// and otherwise ignored: the broadcast watcher still announces rotations
    /// (`TokenLedger::callbacks_active` stays false), as before this existed.
    pub(crate) fn install_on(&self, client: &Client) {
        let installed = install(client, &self.ledger, &self.lane, &self.events, true);
        trace(
            &self.events,
            "installed",
            json!({ "ok": installed.is_ok() }),
        );
    }
}

#[cfg(test)]
mod tests;
