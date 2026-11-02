
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use matrix_sdk::{Client, SessionTokens};
use serde_json::json;

use crate::EventQueueRef;

pub(crate) const WAIT_CAP: Duration = Duration::from_millis(3000);

/// No drain for this long since the wait began: the GUI thread is not polling
/// (blocked, possibly on this very runtime), so no answer is coming soon.
/// C++ drains every 100 ms when it is free.
pub(crate) const DRAIN_STALL: Duration = Duration::from_millis(500);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum WaitOutcome {
    Saved,
    NotSaved,
    Stalled,
    TimedOut,
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
    latest: Option<SessionTokens>,
    generation: u64,
    client_base: u64,
    answered: u64,
    answered_ok: bool,
    saved: u64,
    last_rotation: Option<Instant>,
    waits: bool,
    callbacks_active: bool,
    last_drain: Option<Instant>,
    drain_thread: Option<std::thread::ThreadId>,
}

pub(crate) struct TokenLedger {
    state: Mutex<LedgerState>,
    answered: Condvar,
    wait_cap: Duration,
    drain_stall: Duration,
    custody: Mutex<Option<Arc<crate::labs::CustodyFacts>>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct LedgerStats {
    pub rotations: u64,
    pub unsaved_rotation: bool,
    pub since_rotation_ms: Option<u64>,
}

impl TokenLedger {
    pub(crate) fn new() -> Arc<Self> {
        Self::with_bounds(true, WAIT_CAP, DRAIN_STALL)
    }

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
            custody: Mutex::new(None),
        })
    }

    pub(crate) fn attach_custody(&self, custody: Arc<crate::labs::CustodyFacts>) {
        *self.custody.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(custody);
    }

    pub(crate) fn detach_custody(&self) {
        *self.custody.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = None;
    }

    pub(crate) fn generation(&self) -> u64 {
        self.lock().generation
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, LedgerState> {
        self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn begin_client(&self, waits: bool) {
        let mut state = self.lock();
        state.latest = None;
        state.client_base = state.generation;
        state.last_rotation = None;
        state.waits = waits;
        state.callbacks_active = false;
    }

    pub(crate) fn callbacks_active(&self) -> bool {
        self.lock().callbacks_active
    }

    pub(crate) fn record(&self, tokens: SessionTokens) -> u64 {
        let generation = {
            let mut state = self.lock();
            state.generation += 1;
            state.latest = Some(tokens);
            state.last_rotation = Some(Instant::now());
            state.generation
        };
        if let Some(custody) = self
            .custody
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .as_ref()
        {
            custody.note_refreshed();
        }
        generation
    }

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

    pub(crate) fn note_drain(&self) {
        let mut state = self.lock();
        state.last_drain = Some(Instant::now());
        state.drain_thread = Some(std::thread::current().id());
    }

    pub(crate) fn stop_waiting(&self) {
        self.lock().waits = false;
        self.answered.notify_all();
    }

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

    pub(crate) fn wait_for_answer(&self, generation: u64) -> WaitOutcome {
        let started = Instant::now();
        let mut state = self.lock();
        loop {
            if state.answered >= generation {
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

pub(crate) fn token_trace_enabled() -> bool {
    use std::sync::OnceLock;
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        crate::switchtrace::switch_trace_enabled_from(
            std::env::var("LIGHTNING_TOKEN_TRACE").ok().as_deref(),
        )
    })
}

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

pub(crate) struct Sink {
    pub ledger: Arc<TokenLedger>,
    pub lane: EventQueueRef,
    pub events: EventQueueRef,
}

impl Sink {
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
