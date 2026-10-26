//! Concurrent live accounts (Labs): the process switch, the per-handle
//! profile, the event classification and the small pure pieces the background
//! handles share (design 2026-10-15 §3.2, contract la-p1 §1 and §3).
//!
//! `ACTIVE` is set only by `mx_rust_labs_enable()` and never cleared in this
//! process: the safe direction after a runtime "off". While it is false every
//! Labs branch in the bridge is dead: no reservation, no precheck, no
//! Background handle, no profile, no sticky slot, and `enqueue` takes today's
//! path after one relaxed load (T-OFF-4).
//!
//! Nothing here reads or logs a token, a key or a message body.

use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};

use matrix_sdk::SqliteStoreConfig;

static ACTIVE: AtomicBool = AtomicBool::new(false);

/// Sets the process switch. Idempotent; there is no way to clear it.
pub(crate) fn enable() {
    ACTIVE.store(true, Ordering::SeqCst);
}

/// Whether Labs was enabled in this process. A relaxed load: the flag only
/// ever moves false -> true, and a handle created after the switch observes
/// it through the FFI call that created it.
pub(crate) fn active() -> bool {
    ACTIVE.load(Ordering::Relaxed)
}

/// The ONE gate for the store reservation and the crypto precheck (contract
/// §1, sanctioned difference 3). DECISION 5 moves both onto the flag-off path
/// in a later reviewed commit by changing this line alone.
pub(crate) fn store_guards_active() -> bool {
    active()
}

#[cfg(test)]
pub(crate) fn disable_for_test() {
    ACTIVE.store(false, Ordering::SeqCst);
}

/// Serialises tests that flip or depend on the process switch.
#[cfg(test)]
pub(crate) static LABS_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// A test's hold on the process switch: takes `LABS_TEST_LOCK` and turns the
/// switch OFF when dropped, a panicking test included, so a failed Labs test
/// cannot leave every later test in the run under Labs (review 2026-10-16,
/// finding 9). `Drop::drop` runs before the lock field is released.
#[cfg(test)]
pub(crate) struct LabsTestGuard {
    _lock: std::sync::MutexGuard<'static, ()>,
}

#[cfg(test)]
impl LabsTestGuard {
    /// The lock, with the switch on.
    pub(crate) fn enabled() -> Self {
        let guard = Self::off();
        enable();
        guard
    }

    /// The lock, with the switch off.
    pub(crate) fn off() -> Self {
        let lock = LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        disable_for_test();
        Self { _lock: lock }
    }
}

#[cfg(test)]
impl Drop for LabsTestGuard {
    fn drop(&mut self) {
        disable_for_test();
    }
}

static NEXT_SERIAL: AtomicU64 = AtomicU64::new(1);

/// A monotonic per-process handle serial, assigned at create. Never an
/// address, so a freed handle's number is never reused (no ABA).
pub(crate) fn next_handle_serial() -> u64 {
    NEXT_SERIAL.fetch_add(1, Ordering::Relaxed)
}

/// A handle's mode. Phase 1 has Foreground (every `mx_rust_create` handle)
/// and Background (every `mx_rust_create_background` handle, for its life);
/// Staging belongs to Phase 2.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Mode {
    Foreground = 0,
    #[allow(dead_code)]
    Staging = 1,
    Background = 2,
}

impl Mode {
    pub(crate) fn from_u8(value: u8) -> Self {
        match value {
            1 => Mode::Staging,
            2 => Mode::Background,
            _ => Mode::Foreground,
        }
    }
}

// ---- Profile (contract §3.1) ------------------------------------------------

/// The per-handle profile, fixed at `mx_rust_create_background`.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Profile {
    /// Thread-name suffix: `lt-{sdk,sync,boot,op}-<slot>`. `[a-z0-9]{1,4}`.
    pub slot: String,
    /// The recorded identity the crypto precheck requires.
    pub user_id: String,
    pub device_id: String,
    /// Snapshot of `STRICT_DEVICE_TRUST` for this handle.
    pub strict_trust: bool,
    /// 0 public, 1 private, 2 none.
    pub receipt_privacy: i32,
    /// Index-all hold bits applied at create.
    pub index_all_hold: u32,
    /// UNIX ms: notify-worthy events older than this are dropped at the source.
    pub floor_ms: u64,
    /// The initial local-rule room set.
    pub local_rule_rooms: Vec<String>,
    /// `max_blocking_threads` on every runtime the handle creates.
    pub max_blocking_threads: usize,
    /// The low-memory SQLite profile.
    pub low_memory_sqlite: bool,
    /// SDK `RequestConfig` bounds.
    pub retry_limit: usize,
    pub max_retry_time_ms: u64,
    /// Latest-event watch cap in Background.
    pub watch_cap: usize,
}

fn slot_is_valid(slot: &str) -> bool {
    (1..=4).contains(&slot.len())
        && slot.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
}

/// Parse `profile_json`. Unknown keys are ignored; a missing required key, a
/// wrong type or an out-of-range value is an error (the FFI returns NULL).
/// `local_rule_rooms` is the one optional key (an empty set).
pub(crate) fn parse_profile(text: &str) -> Result<Profile, String> {
    let value: serde_json::Value =
        serde_json::from_str(text).map_err(|_| "profile is not JSON".to_owned())?;
    let object = value.as_object().ok_or("profile is not an object")?;
    let get = |key: &str| object.get(key).ok_or(format!("profile lacks {key}"));
    let uint = |key: &str| -> Result<u64, String> {
        get(key)?.as_u64().ok_or(format!("profile {key} is not an unsigned integer"))
    };
    let boolean = |key: &str| -> Result<bool, String> {
        get(key)?.as_bool().ok_or(format!("profile {key} is not a bool"))
    };

    if uint("v")? != 1 {
        return Err("unsupported profile version".to_owned());
    }
    let slot = get("slot")?.as_str().ok_or("profile slot is not a string")?.to_owned();
    if !slot_is_valid(&slot) {
        return Err("profile slot must be [a-z0-9]{1,4}".to_owned());
    }
    let account = get("require_account")?
        .as_object()
        .ok_or("profile require_account is not an object")?;
    let field = |key: &str| -> Result<String, String> {
        account
            .get(key)
            .and_then(|v| v.as_str())
            .filter(|v| !v.is_empty())
            .map(str::to_owned)
            .ok_or(format!("profile require_account lacks {key}"))
    };
    let user_id = field("user_id")?;
    let device_id = field("device_id")?;
    if !user_id.starts_with('@') {
        return Err("profile require_account.user_id is not a user id".to_owned());
    }
    let receipt_privacy = uint("receipt_privacy")?;
    if receipt_privacy > 2 {
        return Err("profile receipt_privacy out of range".to_owned());
    }
    let index_all_hold = uint("index_all_hold")?;
    let index_all_hold =
        u32::try_from(index_all_hold).map_err(|_| "profile index_all_hold out of range")?;
    let max_blocking_threads = uint("max_blocking_threads")?;
    if !(1..=64).contains(&max_blocking_threads) {
        return Err("profile max_blocking_threads out of range".to_owned());
    }
    let low_memory_sqlite = match get("sqlite")?.as_str() {
        Some("low_memory") => true,
        Some("default") => false,
        _ => return Err("profile sqlite must be \"low_memory\" or \"default\"".to_owned()),
    };
    let retry_limit = uint("retry_limit")?;
    if retry_limit > 16 {
        return Err("profile retry_limit out of range".to_owned());
    }
    let max_retry_time_ms = uint("max_retry_time_ms")?;
    if max_retry_time_ms == 0 || max_retry_time_ms > 15 * 60 * 1000 {
        return Err("profile max_retry_time_ms out of range".to_owned());
    }
    let watch_cap = uint("watch_cap")?;
    if !(1..=10_000).contains(&watch_cap) {
        return Err("profile watch_cap out of range".to_owned());
    }
    let local_rule_rooms = match object.get("local_rule_rooms") {
        None => Vec::new(),
        Some(serde_json::Value::Array(rooms)) => rooms
            .iter()
            .map(|r| r.as_str().map(str::to_owned).ok_or("profile local_rule_rooms entry"))
            .collect::<Result<Vec<_>, _>>()?,
        Some(_) => return Err("profile local_rule_rooms is not an array".to_owned()),
    };
    Ok(Profile {
        slot,
        user_id,
        device_id,
        strict_trust: boolean("strict_trust")?,
        receipt_privacy: receipt_privacy as i32,
        index_all_hold,
        floor_ms: uint("floor_ms")?,
        local_rule_rooms,
        max_blocking_threads: max_blocking_threads as usize,
        low_memory_sqlite,
        retry_limit: retry_limit as usize,
        max_retry_time_ms,
        watch_cap: watch_cap as usize,
    })
}

/// `lt-<kind>-<slot>`: no PII, so `/proc/self/task` can attribute threads.
pub(crate) fn thread_name(kind: &str, slot: &str) -> String {
    format!("lt-{kind}-{slot}")
}

/// The runtime kind a `run_async` label belongs to.
pub(crate) fn runtime_kind(label: &str) -> &'static str {
    match label {
        "sync" => "sync",
        "crypto_bootstrap" => "boot",
        _ => "op",
    }
}

/// The low-memory SQLite profile for one store directory (design §3.2.11):
/// pool 4, cache 500 KB, journal 2 MB. `false` is exactly today's config.
pub(crate) fn sqlite_config(path: &Path, low_memory: bool) -> SqliteStoreConfig {
    let config = SqliteStoreConfig::new(path);
    if low_memory {
        config.pool_max_size(4).cache_size(500_000).journal_size_limit(2_000_000)
    } else {
        config
    }
}

// ---- Store-open context ------------------------------------------------------

/// What a store open must enforce for the handle that runs it. Scoped as a
/// task-local around the one future that builds the handle's client (login,
/// restore, OAuth restore), so a sibling handle of the same account can never
/// reserve under this handle's serial: a path-keyed lookup could.
#[derive(Debug, Default)]
pub(crate) struct OpenContext {
    pub serial: u64,
    /// `Some` for a Background handle: the crypto precheck.
    pub require_account: Option<(String, String)>,
    pub low_memory_sqlite: bool,
    /// `Some` for a Background handle: SDK retry bounds.
    pub request_limits: Option<(usize, u64)>,
    /// `Some` for a Background handle: its strict-trust snapshot.
    pub strict_trust: Option<bool>,
    /// The store refusal this open hit, if any (`refuse_store`). Out of band
    /// on purpose: the restore's `login_failed` is given `"stage":"store"`
    /// from HERE, never from its message text, which also carries the
    /// homeserver's own error text (review 2026-10-16, finding 5). A fresh
    /// context per restore, so nothing carries over to the next one.
    pub refusal: Mutex<Option<&'static str>>,
}

impl OpenContext {
    /// The store refusal this open recorded, if any.
    pub(crate) fn refusal(&self) -> Option<&'static str> {
        *self.refusal.lock().unwrap_or_else(|e| e.into_inner())
    }
}

tokio::task_local! {
    static OPEN_CONTEXT: Arc<OpenContext>;
}

/// The context of the build in progress, if any.
pub(crate) fn open_context() -> Option<Arc<OpenContext>> {
    OPEN_CONTEXT.try_with(Arc::clone).ok()
}

/// How long a Background handle's restore (client build with its discovery
/// fallback, `restore_session`, the unsent-request resume) may take before it
/// is abandoned as a `login_failed`. A runtime Labs-off and a quit both wait
/// for a Starting host's login result, so this is what bounds them (§18 round
/// 2, L-c). The client build alone has its own 10 s discovery budget.
pub(crate) const BACKGROUND_RESTORE_BUDGET: std::time::Duration =
    std::time::Duration::from_secs(60);

/// A restore future, bounded by [`BACKGROUND_RESTORE_BUDGET`] when
/// `background`, else returned as it is (the foreground, and every restore
/// with the flag off, unchanged). The bounded one is boxed so the `Either`
/// is no larger than the bare future (see `scope_open`).
pub(crate) fn bound_background_restore<F, T>(
    background: bool,
    future: F,
) -> impl std::future::Future<Output = Result<T, String>>
where
    F: std::future::Future<Output = Result<T, String>>,
{
    bound_restore_within(background.then_some(BACKGROUND_RESTORE_BUDGET), future)
}

fn bound_restore_within<F, T>(
    budget: Option<std::time::Duration>,
    future: F,
) -> impl std::future::Future<Output = Result<T, String>>
where
    F: std::future::Future<Output = Result<T, String>>,
{
    use futures_util::future::Either;
    match budget {
        Some(budget) => Either::Left(Box::pin(async move {
            match tokio::time::timeout(budget, future).await {
                Ok(result) => result,
                Err(_) => Err(format!(
                    "the background restore did not finish within {} seconds",
                    budget.as_secs()
                )),
            }
        })),
        None => Either::Right(future),
    }
}

/// Run `future` with `context` in scope. `None` runs it bare (today).
///
/// Not an `async fn`: that would hold `future` twice in its state (the
/// argument, then the scope wrapping it), and a restore future is large
/// enough that twice its size overflows the restore thread's stack. An
/// `Either` is the larger of the two and nothing more.
pub(crate) fn scope_open<F: std::future::Future>(
    context: Option<Arc<OpenContext>>,
    future: F,
) -> impl std::future::Future<Output = F::Output> {
    use futures_util::future::Either;
    match context {
        Some(context) => Either::Left(OPEN_CONTEXT.scope(context, future)),
        None => Either::Right(future),
    }
}

pub(crate) const REFUSAL_STORE_IN_USE: &str = "store_in_use";
pub(crate) const REFUSAL_CRYPTO_MISSING: &str = "crypto_account_missing";
pub(crate) const REFUSAL_CRYPTO_MISMATCH: &str = "crypto_account_mismatch";

/// A store refusal: recorded in the open context in scope (the restore's
/// `login_failed` reads it from there, contract §3.3), and returned as words
/// for a person. No marker in the text: a foreground handle shows the text
/// as it is, and text is never what decides the stage.
pub(crate) fn refuse_store(reason: &'static str) -> String {
    if let Some(context) = open_context() {
        *context.refusal.lock().unwrap_or_else(|e| e.into_inner()) = Some(reason);
    }
    match reason {
        REFUSAL_STORE_IN_USE => "the account's store is in use by another session",
        REFUSAL_CRYPTO_MISSING => "this device's encryption data is missing from the store",
        _ => "the store's encryption data belongs to another account or device",
    }
    .to_owned()
}

/// The store refusal of the open in scope, if any. `None` outside an open
/// context (a flag-off restore, or a test calling `admit` bare).
pub(crate) fn current_store_refusal() -> Option<&'static str> {
    open_context().and_then(|context| context.refusal())
}

// ---- Custody facts (design §3.2.7, contract §3.4) ---------------------------

/// What the token task observed, for `mx_rust_take_custody`. Facts only: the
/// tokens themselves are read live from the Client (`final_session_tokens`),
/// so nothing can be dropped and no second copy of a secret exists.
#[derive(Default, Debug)]
pub(crate) struct CustodyFacts {
    generation: AtomicU64,
    revoked: AtomicBool,
    /// 0 unknown, 1 false, 2 true.
    soft_logout: AtomicU8,
    had_refresh_token: AtomicU8,
}

fn tri(value: bool) -> u8 {
    if value { 2 } else { 1 }
}

fn untri(value: u8) -> Option<bool> {
    match value {
        1 => Some(false),
        2 => Some(true),
        _ => None,
    }
}

impl CustodyFacts {
    /// A rotation was observed.
    pub(crate) fn note_refreshed(&self) {
        self.generation.fetch_add(1, Ordering::SeqCst);
    }

    /// `UnknownToken`: sticky for the life of the handle.
    pub(crate) fn note_revoked(&self, soft_logout: bool, had_refresh_token: bool) {
        self.soft_logout.store(tri(soft_logout), Ordering::SeqCst);
        self.had_refresh_token.store(tri(had_refresh_token), Ordering::SeqCst);
        self.revoked.store(true, Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst);
    }

    pub(crate) fn generation(&self) -> u64 {
        self.generation.load(Ordering::SeqCst)
    }

    pub(crate) fn revoked(&self) -> bool {
        self.revoked.load(Ordering::SeqCst)
    }

    /// The facts as they stand, `generation` read FIRST. A rotation installs
    /// the new pair in the Client before the token task bumps the generation,
    /// so facts read before the tokens can only be OLDER than the tokens they
    /// travel with, never newer: a host that records the generation then
    /// re-takes custody when it moves, instead of pairing a consumed refresh
    /// token with a generation that will not move again (review 2026-10-16,
    /// finding 1).
    pub(crate) fn snapshot(&self) -> CustodySnapshot {
        let generation = self.generation.load(Ordering::SeqCst);
        CustodySnapshot {
            generation,
            revoked: self.revoked.load(Ordering::SeqCst),
            soft_logout: untri(self.soft_logout.load(Ordering::SeqCst)),
            had_refresh_token: untri(self.had_refresh_token.load(Ordering::SeqCst)),
        }
    }

    /// The custody JSON (SENSITIVE when tokens are returned): the facts are
    /// snapshotted, THEN `read_tokens` runs. `None` tokens: `has_session:false`
    /// and null token fields.
    pub(crate) fn custody_json(
        &self,
        read_tokens: impl FnOnce() -> Option<(String, Option<String>)>,
    ) -> serde_json::Value {
        let facts = self.snapshot();
        facts.to_json(read_tokens())
    }
}

/// `CustodyFacts` at one instant (`CustodyFacts::snapshot`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CustodySnapshot {
    pub generation: u64,
    pub revoked: bool,
    pub soft_logout: Option<bool>,
    pub had_refresh_token: Option<bool>,
}

impl CustodySnapshot {
    fn to_json(self, tokens: Option<(String, Option<String>)>) -> serde_json::Value {
        let (has_session, access, refresh) = match tokens {
            Some((access, refresh)) if !access.is_empty() => (true, Some(access), refresh),
            _ => (false, None, None),
        };
        serde_json::json!({
            "v": 1,
            "has_session": has_session,
            "access_token": access,
            "refresh_token": refresh,
            "generation": self.generation,
            "revoked": self.revoked,
            "revoked_kind": if self.revoked { "unknown_token" } else { "none" },
            "soft_logout": self.soft_logout,
            "had_refresh_token": self.had_refresh_token,
        })
    }
}

// ---- Jittered backoff (design §3.2.11, contract §3.6) -----------------------

/// `min(3000 * 2^attempt, 120000) * (0.8 + 0.4 * unit)`, `unit` in [0, 1].
/// Pure; the bridge's own retry loops use it for Background handles.
pub(crate) fn jittered_backoff_ms(attempt: u32, unit: f64) -> u64 {
    let base = 3000u64.saturating_mul(1u64 << attempt.min(16)).min(120_000);
    let unit = if unit.is_finite() { unit.clamp(0.0, 1.0) } else { 0.5 };
    (base as f64 * (0.8 + 0.4 * unit)).round() as u64
}

/// A local xorshift64* source (no new crate). Not for anything secret.
#[derive(Debug)]
pub(crate) struct Jitter(u64);

impl Jitter {
    pub(crate) fn seeded(seed: u64) -> Self {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        Self::from_seed(seed ^ nanos)
    }

    pub(crate) fn from_seed(seed: u64) -> Self {
        // Never zero: xorshift stays at zero for ever.
        Self(seed | 1)
    }

    /// A unit value in [0, 1).
    pub(crate) fn unit(&mut self) -> f64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        (x.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 11) as f64 / (1u64 << 53) as f64
    }

    pub(crate) fn backoff(&mut self, attempt: u32) -> std::time::Duration {
        std::time::Duration::from_millis(jittered_backoff_ms(attempt, self.unit()))
    }
}

// ---- Event classes (design §3.2.3, contract §3.5) ---------------------------

/// What a Background handle does with an enqueued event of a type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EventClass {
    /// Flows; the sticky types are also recorded in the session slot.
    Session,
    /// Pre-filtered (`bgsession::should_forward`).
    Notify,
    /// Op-id results of host FFIs. None flows in Phase 1 (Phase 3 adds them).
    #[allow(dead_code)]
    CommandResult,
    /// Carries tokens: custody (`mx_rust_take_custody`) is the only path.
    Custody,
    /// UI work; skipped at the source where it costs anything, and never
    /// queued.
    UiOnly,
    /// A `"type"` field that is not an FFI event (a Matrix focus or widget
    /// type, a test fixture).
    Foreign,
}

/// Every `"type"` literal in `rust/src`, classified. A literal missing here
/// fails `every_type_literal_is_classified`, which is what stops a future
/// crypto or session event from being dropped silently in Background.
const CLASSES: &[(&str, EventClass)] = {
    use EventClass::*;
    &[
        // Session (the contract's allow-list).
        ("status", Session),
        ("login_ok", Session),
        ("login_failed", Session),
        ("session_restored_offline", Session),
        ("initial_sync_done", Session),
        ("room_list_sync_state", Session),
        ("sync_error", Session),
        ("sync_stalled", Session),
        ("crypto_bootstrap", Session),
        ("crypto_health", Session),
        ("key_backup_status", Session),
        ("own_identity_key", Session),
        ("ignored_users_list", Session),
        ("ignored_users_changed", Session),
        ("session_token_revoked", Session),
        ("resumed_device_key", Session),
        ("queue_overflow", Session),
        ("logged_out", Session),
        ("verification_request_received", Session),
        ("verification_cancelled", Session),
        ("to_device_undecryptable", Session),
        ("rust_trace", Session),
        // LIGHTNING_TOKEN_TRACE only (sessiontokens.rs): words, counts and
        // milliseconds, never a token. Flows so the host can log a Background
        // rotation's wait (liveacc port onto 24a485c5).
        ("token_trace", Session),
        // Notify.
        ("timeline_event", Notify),
        ("reaction_event", Notify),
        ("event_decrypted", Notify),
        ("event_redacted", Notify),
        // Custody.
        ("session_tokens_refreshed", Custody),
        // Foreign: not FFI events.
        ("custom", Foreign),
        ("jitsi", Foreign),
        ("livekit", Foreign),
        ("noise", Foreign),
        // UiOnly: everything a Background handle neither needs nor pays for.
        ("account_data_listing", UiOnly),
        ("account_pref", UiOnly),
        ("account_pref_changed", UiOnly),
        ("account_pref_written", UiOnly),
        ("activity_seed", UiOnly),
        ("admin_room_delete_progress", UiOnly),
        ("admin_room_delete_result", UiOnly),
        ("attachment_send_result", UiOnly),
        ("auth_discovery", UiOnly),
        ("backup_action_result", UiOnly),
        ("backup_progress", UiOnly),
        ("cache_compacted", UiOnly),
        ("call_answer", UiOnly),
        ("call_candidates", UiOnly),
        ("call_hangup", UiOnly),
        ("call_invite", UiOnly),
        ("call_reject", UiOnly),
        ("call_rtc_decline", UiOnly),
        ("call_rtc_notification", UiOnly),
        ("call_select_answer", UiOnly),
        ("call_send_result", UiOnly),
        ("call_turn_servers", UiOnly),
        ("context_closed", UiOnly),
        ("context_diff", UiOnly),
        ("context_error", UiOnly),
        ("context_pagination", UiOnly),
        ("context_reset", UiOnly),
        ("cross_signing_approval", UiOnly),
        ("delayed_events_support", UiOnly),
        ("device_delete_result", UiOnly),
        ("device_list", UiOnly),
        ("device_renamed", UiOnly),
        ("direct_peer_avatar", UiOnly),
        ("dm_create_result", UiOnly),
        ("encrypted_send_failed", UiOnly),
        ("encrypted_send_ok", UiOnly),
        ("error", UiOnly),
        ("event_cache_cleared", UiOnly),
        ("event_source", UiOnly),
        ("gif_download_result", UiOnly),
        ("gif_response", UiOnly),
        ("homeserver_info", UiOnly),
        ("ignore_user_result", UiOnly),
        ("invite_policy_support", UiOnly),
        ("invite_state_update", UiOnly),
        ("knock_cancel_result", UiOnly),
        ("latest_event_watch_report", UiOnly),
        ("local_search_result", UiOnly),
        ("media_cache_cleared", UiOnly),
        ("media_failed", UiOnly),
        ("media_history_failed", UiOnly),
        ("media_history_page", UiOnly),
        ("media_ready", UiOnly),
        ("message_edit_history", UiOnly),
        ("message_edits_removed", UiOnly),
        ("message_search_result", UiOnly),
        ("moderation_plan", UiOnly),
        ("mutual_rooms_result", UiOnly),
        ("name_color", UiOnly),
        ("name_color_set", UiOnly),
        ("notification_mode_error", UiOnly),
        ("oauth_failed", UiOnly),
        ("oauth_management_url", UiOnly),
        ("oauth_ok", UiOnly),
        ("oauth_registration_retry", UiOnly),
        ("oauth_url", UiOnly),
        ("own_avatar_result", UiOnly),
        ("own_display_name_result", UiOnly),
        ("password_change_probe", UiOnly),
        ("password_change_result", UiOnly),
        ("personal_background_bytes", UiOnly),
        ("personal_background_changed", UiOnly),
        ("personal_backgrounds", UiOnly),
        ("personal_backgrounds_cleared", UiOnly),
        ("personal_background_written", UiOnly),
        ("policy_check", UiOnly),
        ("policy_rules", UiOnly),
        ("policy_rule_written", UiOnly),
        ("policy_subscriptions", UiOnly),
        ("presence_batch", UiOnly),
        ("presence_publish_failed", UiOnly),
        // musicact: the server's answer to our own presence PUT (the
        // farewell's settle); a background host publishes no presence.
        ("presence_published", UiOnly),
        // msg48 repair: reload the open room and thread after the one-time
        // classic cache repair; a background host has no views to reload.
        ("event_cache_repaired", UiOnly),
        ("profile_banner", UiOnly),
        ("profile_banner_set", UiOnly),
        ("profile_bio", UiOnly),
        ("profile_bio_set", UiOnly),
        ("public_rooms_result", UiOnly),
        ("push_rules_changed", UiOnly),
        ("push_rules_snapshot", UiOnly),
        ("qr_login_progress", UiOnly),
        ("room_closure_result", UiOnly),
        // Added by the second break-it round (main 02693a52), classified at
        // integration: the open room's posting facts answer a foreground
        // op id, and a membership lost elsewhere retires a room the
        // background counts table drops on its next full scan anyway.
        ("room_own_permissions", UiOnly),
        ("room_membership_lost", UiOnly),
        ("push_rules_write", UiOnly),
        ("read_marker_advanced", UiOnly),
        ("refused_sign_in_revoked", UiOnly),
        ("reload_timeline_done", UiOnly),
        ("reload_timeline_failed", UiOnly),
        ("report_message_result", UiOnly),
        ("room_action_error", UiOnly),
        ("room_activity", UiOnly),
        ("room_background", UiOnly),
        ("room_background_changed", UiOnly),
        ("room_background_set", UiOnly),
        ("room_banner", UiOnly),
        ("room_banner_set", UiOnly),
        ("room_bridges", UiOnly),
        ("room_closure_plan", UiOnly),
        ("room_closure_progress", UiOnly),
        ("room_create_result", UiOnly),
        ("room_directory_visibility", UiOnly),
        ("room_edit_result", UiOnly),
        ("room_export_failed", UiOnly),
        ("room_export_page", UiOnly),
        ("room_invite_done", UiOnly),
        ("room_invite_result", UiOnly),
        ("room_join_result", UiOnly),
        ("room_key_export_progress", UiOnly),
        ("room_key_export_result", UiOnly),
        ("room_key_import_done", UiOnly),
        ("room_key_import_failed", UiOnly),
        ("room_key_import_progress", UiOnly),
        ("room_key_import_started", UiOnly),
        ("room_knock_result", UiOnly),
        ("room_leave_result", UiOnly),
        ("room_list_append", UiOnly),
        ("room_list_clear", UiOnly),
        ("room_list_error", UiOnly),
        ("room_list_insert", UiOnly),
        ("room_list_mode", UiOnly),
        ("room_list_pop_back", UiOnly),
        ("room_list_pop_front", UiOnly),
        ("room_list_push_back", UiOnly),
        ("room_list_push_front", UiOnly),
        ("room_list_remove", UiOnly),
        ("room_list_reset", UiOnly),
        ("room_list_set", UiOnly),
        ("room_list_truncate", UiOnly),
        ("room_members", UiOnly),
        ("room_members_changed", UiOnly),
        ("room_moderation_result", UiOnly),
        ("room_notification_mode", UiOnly),
        ("room_pinned", UiOnly),
        ("room_pinned_changed", UiOnly),
        ("room_pin_result", UiOnly),
        ("room_power_level_result", UiOnly),
        ("room_power_matrix_result", UiOnly),
        ("room_preview_backfill", UiOnly),
        ("room_profile_result", UiOnly),
        ("room_send_result", UiOnly),
        ("room_snapshot", UiOnly),
        ("room_target_resolved", UiOnly),
        ("room_tombstone_changed", UiOnly),
        ("room_upgrade_result", UiOnly),
        ("room_versions", UiOnly),
        ("room_widgets", UiOnly),
        ("room_widget_written", UiOnly),
        ("rtc_call_reaction", UiOnly),
        ("rtc_delayed_updated", UiOnly),
        ("rtc_hand_changed", UiOnly),
        ("rtc_hand_result", UiOnly),
        ("rtc_hands", UiOnly),
        ("rtc_key_discarded", UiOnly),
        ("rtc_key_received", UiOnly),
        ("rtc_key_sent", UiOnly),
        ("rtc_membership_published", UiOnly),
        ("rtc_membership_retracted", UiOnly),
        ("rtc_send_result", UiOnly),
        ("rtc_session", UiOnly),
        ("rtc_session_changed", UiOnly),
        ("rtc_transports", UiOnly),
        ("scheduled_send_result", UiOnly),
        ("scheduled_update_result", UiOnly),
        ("search_index_all", UiOnly),
        ("search_index_deepened", UiOnly),
        ("search_index_stats", UiOnly),
        ("search_index_swept", UiOnly),
        ("send_failed", UiOnly),
        ("send_ok", UiOnly),
        ("server_admin_status", UiOnly),
        ("settings_sync_changed", UiOnly),
        ("settings_sync_read", UiOnly),
        ("settings_sync_written", UiOnly),
        ("sfu_joined", UiOnly),
        ("sfu_participants", UiOnly),
        ("sfu_quality", UiOnly),
        ("sfu_remote_candidate", UiOnly),
        ("sfu_remote_description", UiOnly),
        ("sfu_server_mute", UiOnly),
        ("sfu_speakers", UiOnly),
        ("sfu_state", UiOnly),
        ("sfu_track_published", UiOnly),
        ("space_child_removed_result", UiOnly),
        ("space_children_result", UiOnly),
        ("space_child_result", UiOnly),
        ("space_child_suggested_result", UiOnly),
        ("space_list_reset", UiOnly),
        ("sso_failed", UiOnly),
        ("sso_ok", UiOnly),
        ("sso_providers", UiOnly),
        ("sso_url", UiOnly),
        ("stale_invites_retired", UiOnly),
        ("sticker_pack_add_result", UiOnly),
        ("sticker_pack_edit_result", UiOnly),
        ("sticker_pack_migration", UiOnly),
        ("sticker_pack_rooms_set", UiOnly),
        ("sticker_packs", UiOnly),
        ("sticker_packs_changed", UiOnly),
        ("thread_closed", UiOnly),
        ("thread_diff", UiOnly),
        ("thread_error", UiOnly),
        ("thread_list_error", UiOnly),
        ("thread_list_reset", UiOnly),
        ("thread_mark_read_failed", UiOnly),
        ("thread_pagination", UiOnly),
        ("thread_participants", UiOnly),
        ("thread_read_marker_advanced", UiOnly),
        ("thread_reset", UiOnly),
        ("thread_send_failed", UiOnly),
        ("thread_subscription_result", UiOnly),
        ("thread_subscription_state", UiOnly),
        ("timeline_closed", UiOnly),
        ("timeline_diff", UiOnly),
        ("timeline_error", UiOnly),
        ("timeline_pagination", UiOnly),
        ("timeline_reset", UiOnly),
        ("timeline_retry_decryption", UiOnly),
        ("timeline_send_failed", UiOnly),
        ("timeline_shutdown", UiOnly),
        ("timestamp_event", UiOnly),
        ("typing_sent", UiOnly),
        ("typing_update", UiOnly),
        ("uia_required", UiOnly),
        ("upload_limit", UiOnly),
        ("url_preview_result", UiOnly),
        ("user_profile_result", UiOnly),
        ("user_search_result", UiOnly),
        ("verification_done", UiOnly),
        ("verification_failed", UiOnly),
        ("verification_qr_confirmed", UiOnly),
        ("verification_qr_dismissed", UiOnly),
        ("verification_qr_ready", UiOnly),
        ("verification_qr_scanned", UiOnly),
        ("verification_ready", UiOnly),
        ("verification_request_started", UiOnly),
        ("verification_sas_confirmed", UiOnly),
        ("verification_sas_ready", UiOnly),
        ("verification_sas_started", UiOnly),
        ("whoami_probe", UiOnly),
    ]
};

/// The class of an event type, `None` when unclassified. An unclassified
/// type never reaches a Background queue (the gate is an allow-list).
pub(crate) fn classify(event_type: &str) -> Option<EventClass> {
    CLASSES
        .iter()
        .find(|(name, _)| *name == event_type)
        .map(|(_, class)| *class)
}

/// Whether an event of this class may reach a Background handle's queue.
pub(crate) fn flows_in_background(class: Option<EventClass>) -> bool {
    matches!(class, Some(EventClass::Session | EventClass::Notify))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_profile() -> serde_json::Value {
        serde_json::json!({
            "v": 1, "slot": "b0",
            "require_account": {"user_id": "@u:hs", "device_id": "DEV"},
            "strict_trust": false, "receipt_privacy": 2,
            "index_all_hold": 4294967295u64, "floor_ms": 1_760_000_000_000u64,
            "local_rule_rooms": ["!r:hs"], "max_blocking_threads": 8,
            "sqlite": "low_memory", "retry_limit": 3, "max_retry_time_ms": 30000,
            "watch_cap": 200, "future_key": {"ignored": true}
        })
    }

    #[test]
    fn a_valid_profile_parses_and_unknown_keys_are_ignored() {
        let profile = parse_profile(&valid_profile().to_string()).expect("valid");
        assert_eq!(profile.slot, "b0");
        assert_eq!((profile.user_id.as_str(), profile.device_id.as_str()), ("@u:hs", "DEV"));
        assert_eq!(profile.receipt_privacy, 2);
        assert_eq!(profile.index_all_hold, u32::MAX);
        assert_eq!(profile.local_rule_rooms, vec!["!r:hs".to_owned()]);
        assert!(profile.low_memory_sqlite);
        assert_eq!(profile.watch_cap, 200);
        assert_eq!(profile.max_blocking_threads, 8);
        assert_eq!((profile.retry_limit, profile.max_retry_time_ms), (3, 30_000));
    }

    #[test]
    fn a_missing_or_mistyped_required_key_is_refused() {
        for key in [
            "v", "slot", "require_account", "strict_trust", "receipt_privacy",
            "index_all_hold", "floor_ms", "max_blocking_threads", "sqlite",
            "retry_limit", "max_retry_time_ms", "watch_cap",
        ] {
            let mut value = valid_profile();
            value.as_object_mut().unwrap().remove(key);
            assert!(parse_profile(&value.to_string()).is_err(), "missing {key} accepted");
            let mut value = valid_profile();
            value[key] = serde_json::json!([1]);
            assert!(parse_profile(&value.to_string()).is_err(), "mistyped {key} accepted");
        }
        // The optional set is optional.
        let mut value = valid_profile();
        value.as_object_mut().unwrap().remove("local_rule_rooms");
        assert!(parse_profile(&value.to_string()).unwrap().local_rule_rooms.is_empty());
        // Values out of range, and a slot that could carry PII.
        for (key, bad) in [
            ("slot", serde_json::json!("alice")),
            ("slot", serde_json::json!("B0")),
            ("slot", serde_json::json!("")),
            ("receipt_privacy", serde_json::json!(3)),
            ("max_blocking_threads", serde_json::json!(0)),
            ("sqlite", serde_json::json!("tiny")),
            ("watch_cap", serde_json::json!(0)),
            ("v", serde_json::json!(2)),
            ("require_account", serde_json::json!({"user_id": "@u:hs"})),
            ("require_account", serde_json::json!({"user_id": "u", "device_id": "D"})),
        ] {
            let mut value = valid_profile();
            value[key] = bad.clone();
            assert!(parse_profile(&value.to_string()).is_err(), "{key}={bad} accepted");
        }
        assert!(parse_profile("not json").is_err());
    }

    #[test]
    fn the_backoff_doubles_from_three_seconds_caps_at_two_minutes_and_jitters_twenty_percent() {
        assert_eq!(jittered_backoff_ms(0, 0.5), 3000);
        assert_eq!(jittered_backoff_ms(1, 0.5), 6000);
        assert_eq!(jittered_backoff_ms(5, 0.5), 96_000);
        assert_eq!(jittered_backoff_ms(6, 0.5), 120_000, "the cap");
        assert_eq!(jittered_backoff_ms(60, 0.5), 120_000, "no overflow past the cap");
        assert_eq!(jittered_backoff_ms(0, 0.0), 2400, "-20 %");
        assert_eq!(jittered_backoff_ms(0, 1.0), 3600, "+20 %");
        assert_eq!(jittered_backoff_ms(6, 1.0), 144_000);
        assert_eq!(jittered_backoff_ms(0, f64::NAN), 3000);
        let mut jitter = Jitter::from_seed(42);
        let mut seen_low = false;
        let mut seen_high = false;
        for _ in 0..1000 {
            let unit = jitter.unit();
            assert!((0.0..1.0).contains(&unit));
            seen_low |= unit < 0.1;
            seen_high |= unit > 0.9;
            let ms = jitter.backoff(2).as_millis() as u64;
            assert!((9600..=14_400).contains(&ms), "{ms}");
        }
        assert!(seen_low && seen_high, "the source must cover the unit interval");
    }

    // Review 2026-10-16, finding 5: the reason travels out of band, in the
    // open context in scope, and the text a person sees carries no marker.
    #[test]
    fn a_store_refusal_is_recorded_in_its_open_context_and_not_in_its_text() {
        let runtime = tokio::runtime::Builder::new_current_thread().build().unwrap();
        for reason in [REFUSAL_STORE_IN_USE, REFUSAL_CRYPTO_MISSING, REFUSAL_CRYPTO_MISMATCH] {
            let context = Arc::new(OpenContext { serial: 7, ..Default::default() });
            let (text, seen) = runtime.block_on(scope_open(Some(Arc::clone(&context)), async {
                let text = refuse_store(reason);
                (text, current_store_refusal())
            }));
            assert_eq!(context.refusal(), Some(reason));
            assert_eq!(seen, Some(reason));
            assert!(!text.contains(reason) && !text.contains('['), "{text}");
        }
        // Outside a context nothing is recorded, and nothing is current.
        let _ = refuse_store(REFUSAL_STORE_IN_USE);
        assert_eq!(current_store_refusal(), None);
        let fresh = Arc::new(OpenContext::default());
        let seen = runtime.block_on(scope_open(Some(Arc::clone(&fresh)), async {
            current_store_refusal()
        }));
        assert_eq!(seen, None, "a new open starts with no refusal");
    }

    #[test]
    fn custody_facts_are_sticky_and_carry_no_secret_without_a_session() {
        let facts = CustodyFacts::default();
        let none = facts.custody_json(|| None);
        assert_eq!(none["has_session"], false);
        assert!(none["access_token"].is_null() && none["refresh_token"].is_null());
        assert_eq!(none["revoked_kind"], "none");
        assert!(none["soft_logout"].is_null() && none["had_refresh_token"].is_null());
        facts.note_refreshed();
        assert_eq!(facts.generation(), 1);
        facts.note_revoked(true, false);
        let after = facts.custody_json(|| Some(("a".into(), Some("r".into()))));
        assert_eq!(after["generation"], 2);
        assert_eq!(after["revoked"], true);
        assert_eq!(after["revoked_kind"], "unknown_token");
        assert_eq!(after["soft_logout"], true);
        assert_eq!(after["had_refresh_token"], false);
        assert_eq!(after["access_token"], "a");
        assert_eq!(after["refresh_token"], "r");
        // A later rotation never clears the revocation.
        facts.note_refreshed();
        assert!(facts.revoked());
    }

    // Review 2026-10-16, finding 1 (I4): a rotation that lands between the
    // two reads must never pair the OLD tokens with the NEW generation. The
    // seam is the token read itself: it installs a rotation (new pair, then
    // the bump, in the token task's order) and returns the pair it read
    // BEFORE that, which is the worst case. The generation returned must be
    // the one from before the rotation, so the host re-takes custody.
    #[test]
    fn custody_never_stamps_old_tokens_with_a_newer_generation() {
        let facts = CustodyFacts::default();
        facts.note_refreshed();
        let before = facts.generation();
        let custody = facts.custody_json(|| {
            let old_pair = ("old-access".to_owned(), Some("old-refresh".to_owned()));
            facts.note_refreshed(); // the rotation lands mid-read
            Some(old_pair)
        });
        assert_eq!(custody["access_token"], "old-access");
        assert_eq!(custody["generation"], before, "old tokens stamped with the newer generation");
        assert!(facts.generation() > before, "the host must see the generation move next time");
        // The revocation facts are read before the tokens too.
        let facts = CustodyFacts::default();
        let custody = facts.custody_json(|| {
            facts.note_revoked(false, true);
            Some(("old-access".to_owned(), None))
        });
        assert_eq!(custody["generation"], 0);
        assert_eq!(custody["revoked"], false);
    }

    #[test]
    fn the_low_memory_profile_differs_and_the_default_is_todays_config() {
        let dir = Path::new("/nonexistent/lightning-labs");
        assert_eq!(
            format!("{:?}", sqlite_config(dir, false)),
            format!("{:?}", SqliteStoreConfig::new(dir)),
            "flag-off stores must open with exactly today's config"
        );
        let low = format!("{:?}", sqlite_config(dir, true));
        assert_ne!(low, format!("{:?}", SqliteStoreConfig::new(dir)));
        assert!(low.contains("cache_size: 500000"), "{low}");
        assert!(low.contains("journal_size_limit: 2000000"), "{low}");
        assert!(low.contains("max_size: 4"), "{low}");
    }

    /// Every `"type": "<literal>"` in `rust/src` (Matrix event types, which
    /// carry a dot, excepted) must be classified, exactly once.
    #[test]
    fn every_type_literal_is_classified() {
        let mut seen = std::collections::BTreeSet::new();
        for (name, _) in CLASSES {
            assert!(seen.insert(*name), "{name} is classified twice");
        }
        let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let mut unclassified = std::collections::BTreeSet::new();
        let mut literals = 0usize;
        let mut stack = vec![dir];
        while let Some(dir) = stack.pop() {
            for entry in std::fs::read_dir(&dir).expect("rust/src is readable") {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    stack.push(path);
                    continue;
                }
                if path.extension().and_then(|e| e.to_str()) != Some("rs") {
                    continue;
                }
                let text = std::fs::read_to_string(&path).unwrap();
                for literal in type_literals(&text) {
                    // Matrix event types carry a dot; an empty or non-identifier
                    // string is a fixture or this file's own prose.
                    if literal.contains('.')
                        || literal.is_empty()
                        || !literal.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
                        || UNCLASSIFIED_ON_PURPOSE.contains(&literal.as_str())
                    {
                        continue;
                    }
                    literals += 1;
                    if classify(&literal).is_none() {
                        unclassified.insert(literal);
                    }
                }
            }
        }
        assert!(literals > 300, "the scan found only {literals} literals: is it reading rust/src?");
        assert!(unclassified.is_empty(), "unclassified event types: {unclassified:?}");
    }

    /// Fixtures that exist to prove an unclassified type is refused.
    const UNCLASSIFIED_ON_PURPOSE: &[&str] = &[
        "a_type_from_the_future",
        "brand_new_crypto_event",
        // The scanner's own fixture.
        "a_one",
        "b_two",
        "c_three",
        "d_four",
    ];

    /// Every string literal written as an event's `"type"`, in the three
    /// shapes `rust/src` uses: a json! key followed by `:`, an index
    /// assignment `v[..] = json!(..)` or `= ..` (never `==`), and an
    /// `insert(..to_owned(), ..into())`. A tiny scanner, so the test
    /// needs no regex crate. (Review 2026-10-16, finding 11: only the first
    /// shape was read, and `qr_login_progress` went unclassified.)
    fn type_literals(text: &str) -> Vec<String> {
        fn string_at(after: &str) -> Option<String> {
            let after = after.trim_start();
            let after = after.strip_prefix("json!(").unwrap_or(after).trim_start();
            let after = after.strip_prefix('"')?;
            after.find('"').map(|end| after[..end].to_owned())
        }
        let mut out = Vec::new();
        let needle = "\"type\"";
        let mut rest = text;
        while let Some(at) = rest.find(needle) {
            rest = &rest[at + needle.len()..];
            let after = rest.trim_start();
            let found = if let Some(after) = after.strip_prefix(':') {
                string_at(after)
            } else if let Some(after) = after.strip_prefix(']') {
                let after = after.trim_start();
                match after.strip_prefix('=') {
                    Some(after) if !after.starts_with('=') => string_at(after),
                    _ => None,
                }
            } else if let Some(after) = after.strip_prefix(".to_owned(),") {
                string_at(after)
            } else {
                None
            };
            out.extend(found);
        }
        out
    }

    #[test]
    fn the_type_scanner_reads_every_shape_the_bridge_writes() {
        let text = r#"json!({ "type": "a_one" }); v["type"] = json!("b_two");
            v["type"] = "c_three"; if e["type"] == "not_me" {}
            object.insert("type".to_owned(), "d_four".into());"#;
        assert_eq!(type_literals(text), ["a_one", "b_two", "c_three", "d_four"]);
    }

    #[test]
    fn the_background_allow_list_is_session_and_notify_only() {
        assert!(flows_in_background(classify("status")));
        assert!(flows_in_background(classify("timeline_event")));
        assert!(flows_in_background(classify("verification_request_received")));
        assert!(!flows_in_background(classify("session_tokens_refreshed")), "tokens via custody only");
        assert!(!flows_in_background(classify("room_list_reset")));
        assert!(!flows_in_background(classify("typing_update")));
        assert!(!flows_in_background(classify("a_type_added_tomorrow")), "allow-list, not deny-list");
        assert!(!flows_in_background(classify("send_ok")));
    }
}

#[cfg(test)]
mod restore_bound_tests {
    use super::*;

    /// §18 round 2, L-c: a Background restore that never finishes ends as a
    /// failure within its budget; one that finishes is passed through; the
    /// foreground's (no budget) is the bare future.
    #[test]
    fn a_background_restore_that_never_finishes_fails_within_its_budget() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let started = std::time::Instant::now();
        let stuck: Result<(), String> = runtime.block_on(bound_restore_within(
            Some(std::time::Duration::from_millis(50)),
            std::future::pending::<Result<(), String>>(),
        ));
        assert!(stuck.unwrap_err().contains("did not finish"));
        assert!(started.elapsed() < std::time::Duration::from_secs(5));
        let done: Result<u8, String> = runtime.block_on(bound_restore_within(
            Some(std::time::Duration::from_secs(5)),
            async { Ok(7u8) },
        ));
        assert_eq!(done, Ok(7));
        let bare: Result<u8, String> =
            runtime.block_on(bound_background_restore(false, async { Ok(3u8) }));
        assert_eq!(bare, Ok(3));
        assert_eq!(BACKGROUND_RESTORE_BUDGET, std::time::Duration::from_secs(60));
    }
}
