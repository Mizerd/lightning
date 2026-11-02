
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};

use matrix_sdk::{
    encryption::{
        secret_storage::{SecretStorageError, SecretStore},
        CrossSigningResetAuthType, CrossSigningResetHandle,
    },
    ruma::{
        api::client::uiaa::{self, AuthData, AuthType, UiaaInfo},
        OwnedDeviceId,
    },
    Client,
};
use serde::Deserialize;
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

pub(crate) enum UiaOperation {
    DeleteDevices(Vec<OwnedDeviceId>),
    CrossSigning(CrossSigningJob),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CrossSigningKind {
    Setup,
    Reset,
}

#[derive(Clone, Default)]
pub(crate) struct CancelToken {
    inner: Arc<CancelInner>,
}

#[derive(Default)]
struct CancelInner {
    cancelled: AtomicBool,
    reset_handle: std::sync::Mutex<Option<Arc<CrossSigningResetHandle>>>,
}

impl CancelToken {
    pub(crate) fn is_cancelled(&self) -> bool {
        self.inner.cancelled.load(Ordering::SeqCst)
    }

    pub(crate) fn request(&self) -> Option<Arc<CrossSigningResetHandle>> {
        self.inner.cancelled.store(true, Ordering::SeqCst);
        self.inner
            .reset_handle
            .lock()
            .ok()
            .and_then(|guard| (*guard).clone())
    }

    async fn attach(&self, handle: &Arc<CrossSigningResetHandle>) {
        if let Ok(mut guard) = self.inner.reset_handle.lock() {
            *guard = Some(Arc::clone(handle));
        }
        if self.is_cancelled() {
            handle.cancel().await;
        }
    }
}

#[derive(Default)]
pub(crate) struct StorageConsent {
    pub recovery_key: Option<String>,
    pub replace_confirmed: bool,
}

impl Drop for StorageConsent {
    // However the job ends (dropped while parked, aborted at teardown), the
    // key the user typed does not outlive it.
    fn drop(&mut self) {
        if let Some(key) = self.recovery_key.as_mut() {
            scrub_string(key);
        }
    }
}

pub(crate) enum StoragePlan {
    EnableRecovery,
    ExportInto(SecretStore),
    ReplaceRecoveryKey,
    Nothing,
}

pub(crate) struct CrossSigningJob {
    pub kind: CrossSigningKind,
    pub action: String,
    consent: StorageConsent,
    setup_plan: Option<SetupPlan>,
    storage: Option<StoragePlan>,
    bootstrap_started: bool,
    /// Reset: the SDK's handle. Once it exists the LOCAL identity has already
    /// been replaced (`bootstrap_cross_signing(true)` saves before uploading).
    handle: Option<Arc<CrossSigningResetHandle>>,
    reset_started: bool,
    cancel: CancelToken,
}

impl CrossSigningJob {
    pub(crate) fn new(
        kind: CrossSigningKind,
        action: &str,
        consent: StorageConsent,
        cancel: CancelToken,
    ) -> Self {
        Self {
            kind,
            action: action.to_owned(),
            consent,
            setup_plan: None,
            storage: None,
            bootstrap_started: false,
            handle: None,
            reset_started: false,
            cancel,
        }
    }
}

pub(crate) const ACTION_SETUP_CROSS_SIGNING: &str = "setup_cross_signing";
pub(crate) const ACTION_RESET_CROSS_SIGNING: &str = "reset_cross_signing";

pub(crate) fn cross_signing_kind_for_action(action: &str) -> Option<CrossSigningKind> {
    match action {
        ACTION_SETUP_CROSS_SIGNING => Some(CrossSigningKind::Setup),
        ACTION_RESET_CROSS_SIGNING => Some(CrossSigningKind::Reset),
        _ => None,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ServerIdentity {
    Absent,
    MatchesLocalKeys,
    Other,
}

/// Pure: classify the server's master key against the local one. Local keys
/// alone never count: after an interrupted setup they exist with nothing on
/// the server.
pub(crate) fn server_identity_state(
    server_master: Option<&str>,
    local_master: Option<&str>,
    local_keys_complete: bool,
) -> ServerIdentity {
    fn unpadded(value: &str) -> &str {
        value.trim_end_matches('=')
    }
    match server_master {
        None => ServerIdentity::Absent,
        Some(server)
            if local_keys_complete
                && !unpadded(server).is_empty()
                && local_master.is_some_and(|local| unpadded(local) == unpadded(server)) =>
        {
            ServerIdentity::MatchesLocalKeys
        }
        Some(_) => ServerIdentity::Other,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SetupPlan {
    AlreadyComplete,
    Bootstrap,
    IdentityWithoutKeys,
}

pub(crate) fn plan_setup(server: ServerIdentity) -> SetupPlan {
    match server {
        ServerIdentity::Absent => SetupPlan::Bootstrap,
        ServerIdentity::MatchesLocalKeys => SetupPlan::AlreadyComplete,
        ServerIdentity::Other => SetupPlan::IdentityWithoutKeys,
    }
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum StorageDecision {
    EnableRecovery,
    ExportIntoExisting,
    ReplaceRecoveryKey,
    Nothing,
}

/// `secret_storage_enabled` is a FRESH read; a failed read never gets here.
/// With secret storage present and the keys about to change, the job needs
/// either the current recovery key or the user's explicit consent to replace
/// it; without either it refuses before anything is created.
pub(crate) fn decide_storage(
    secret_storage_enabled: bool,
    keys_will_change: bool,
    have_recovery_key: bool,
    replace_confirmed: bool,
) -> Result<StorageDecision, &'static str> {
    if !secret_storage_enabled {
        Ok(StorageDecision::EnableRecovery)
    } else if have_recovery_key {
        Ok(StorageDecision::ExportIntoExisting)
    } else if !keys_will_change {
        Ok(StorageDecision::Nothing)
    } else if replace_confirmed {
        Ok(StorageDecision::ReplaceRecoveryKey)
    } else {
        Err("recovery_key_required")
    }
}

pub(crate) enum Drive {
    Done(Option<String>),
    Challenge { info: UiaaInfo, wrong_password: bool },
    Cancelled,
    Failed(&'static str),
}

enum Step {
    Ready,
    Challenge { info: UiaaInfo, wrong_password: bool },
    Cancelled,
    Failed(&'static str),
}

#[derive(Deserialize)]
struct OAuthParams {
    url: String,
}

/// The one UIA slot. A cross-signing job holds it from start to result:
/// RUNNING (`op` None, `cancel` set) blocks a second job and lets Cancel reach
/// it; PARKED (`op` Some) can be answered or cancelled. Device deletion holds
/// it only while parked.
pub(crate) struct UiaPending {
    pub uia_id: u64,
    pub session: Option<String>,
    pub op: Option<UiaOperation>,
    pub cancel: Option<CancelToken>,
}

fn release_slot(slot: &std::sync::Mutex<Option<UiaPending>>, uia_id: u64) {
    if let Ok(mut guard) = slot.lock() {
        if guard
            .as_ref()
            .is_some_and(|p| p.uia_id == uia_id && p.op.is_none())
        {
            guard.take();
        }
    }
}

struct SlotGuard {
    slot: Arc<std::sync::Mutex<Option<UiaPending>>>,
    uia_id: u64,
}

impl Drop for SlotGuard {
    fn drop(&mut self) {
        release_slot(&self.slot, self.uia_id);
    }
}

fn scrub_string(secret: &mut String) {
    // SAFETY: writing 0x00 into every byte keeps the buffer valid UTF-8.
    unsafe {
        for byte in secret.as_mut_vec().iter_mut() {
            std::ptr::write_volatile(byte, 0);
        }
    }
    std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
    secret.clear();
}

fn enqueue_challenge(
    events: &crate::EventQueueRef,
    uia_id: u64,
    lifecycle: u64,
    info: &UiaaInfo,
    wrong_password: bool,
) {
    let flows: Vec<Vec<String>> = info
        .flows
        .iter()
        .map(|flow| flow.stages.iter().map(|s| s.to_string()).collect())
        .collect();
    let completed: Vec<String> =
        info.completed.iter().map(|s| s.to_string()).collect();
    let has_password_stage = info.flows.iter().any(|flow| {
        flow.stages
            .iter()
            .filter(|stage| !info.completed.contains(stage))
            .all(|stage| {
                matches!(stage, AuthType::Password | AuthType::Dummy)
            })
            && flow.stages.iter().any(|s| matches!(s, AuthType::Password))
    });
    enqueue(events, json!({
        "type": "uia_required",
        "op_id": uia_id,
        "lifecycle": lifecycle,
        "flows": flows,
        "completed": completed,
        "has_password_stage": has_password_stage,
        "wrong_password": wrong_password,
    }));
}

pub(crate) fn delete_devices(
    bridge: &RustClient,
    device_ids: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let ids: Vec<OwnedDeviceId> = device_ids
        .iter()
        .map(|id| OwnedDeviceId::from(id.as_str()))
        .collect();
    if ids.is_empty() {
        return Err("no devices given".to_owned());
    }
    if bridge
        .uia_pending
        .lock()
        .ok()
        .map(|guard| guard.is_some())
        .unwrap_or(false)
    {
        return Err("another operation is awaiting authentication".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.delete_devices(&ids, None).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(_) => {
                enqueue(&events, json!({
                    "type": "device_delete_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                }));
            }
            Err(err) => {
                if let Some(info) = err.as_uiaa_response() {
                    if let Ok(mut guard) = pending_slot.lock() {
                        *guard = Some(UiaPending {
                            uia_id: op_id,
                            session: info.session.clone(),
                            op: Some(UiaOperation::DeleteDevices(ids)),
                            cancel: None,
                        });
                    }
                    enqueue_challenge(&events, op_id, lifecycle, info, false);
                } else {
                    enqueue(&events, json!({
                        "type": "device_delete_result",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
            }
        }
    });
    Ok(())
}

pub(crate) fn uia_submit_password(
    bridge: &RustClient,
    uia_id: u64,
    mut password: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let Some(own_user) = client.user_id().map(ToOwned::to_owned) else {
        scrub_string(&mut password);
        return Err("no authenticated user".to_owned());
    };
    // Take the parked op only if the reply matches, so a stale reply cannot
    // consume a newer challenge. A cross-signing job keeps the slot (RUNNING)
    // while its retry is in flight; device deletion frees it as before.
    let pending = {
        let mut guard = bridge
            .uia_pending
            .lock()
            .map_err(|_| "internal state unavailable".to_owned())?;
        let taken = match guard.as_mut() {
            Some(p) if p.uia_id == uia_id => {
                let session = p.session.clone();
                p.op.take().map(|op| (op, session))
            }
            _ => None,
        };
        if matches!(taken, Some((UiaOperation::DeleteDevices(_), _))) {
            guard.take();
        }
        taken
    };
    let Some((op, session)) = pending else {
        scrub_string(&mut password);
        return Err("no matching authentication challenge".to_owned());
    };

    let mut auth = uiaa::Password::new(
        uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(
            own_user.to_string(),
        )),
        std::mem::take(&mut password),
    );
    auth.session = session;

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let auth_data = AuthData::Password(auth);
        let (result, op) = match op {
            UiaOperation::DeleteDevices(ids) => (
                client.delete_devices(&ids, Some(auth_data)).await,
                UiaOperation::DeleteDevices(ids),
            ),
            UiaOperation::CrossSigning(job) => {
                run_cross_signing_pass(
                    client,
                    job,
                    Some(auth_data),
                    uia_id,
                    lifecycle,
                    events,
                    timelines,
                    pending_slot,
                )
                .await;
                return;
            }
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(_) => {
                enqueue(&events, json!({
                    "type": "device_delete_result",
                    "op_id": uia_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                }));
            }
            Err(err) => {
                if let Some(info) = err.as_uiaa_response() {
                    if let Ok(mut guard) = pending_slot.lock() {
                        *guard = Some(UiaPending {
                            uia_id,
                            session: info.session.clone(),
                            op: Some(op),
                            cancel: None,
                        });
                    }
                    enqueue_challenge(&events, uia_id, lifecycle, info, true);
                } else {
                    enqueue(&events, json!({
                        "type": "device_delete_result",
                        "op_id": uia_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
            }
        }
    });
    Ok(())
}

pub(crate) fn uia_cancel(bridge: &RustClient, uia_id: u64) {
    let mut parked_job = None;
    let mut polling_handle = None;
    if let Ok(mut guard) = bridge.uia_pending.lock() {
        let parked = match guard.as_mut() {
            Some(pending) if pending.uia_id == uia_id => match pending.op.take() {
                Some(op) => Some(op),
                None => {
                    polling_handle =
                        pending.cancel.as_ref().and_then(CancelToken::request);
                    None
                }
            },
            _ => None,
        };
        match parked {
            Some(UiaOperation::DeleteDevices(_)) => {
                guard.take();
            }
            Some(UiaOperation::CrossSigning(job)) => parked_job = Some(job),
            None => {}
        }
    }
    if let Some(handle) = polling_handle {
        bridge.spawn_room_action(async move {
            handle.cancel().await;
        });
    }
    let Some(job) = parked_job else {
        return;
    };
    let Ok(client) = require_client(bridge) else {
        release_slot(&bridge.uia_pending, uia_id);
        return;
    };
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let _slot = SlotGuard { slot: Arc::clone(&pending_slot), uia_id };
        let category = cancelled_category(&client, &job).await;
        release_slot(&pending_slot, uia_id);
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "backup_action_result", "op_id": uia_id,
            "lifecycle": lifecycle, "action": job.action, "ok": false,
            "recovery_key": "", "category": category,
        }));
    });
}

async fn server_master_key(client: &Client) -> Result<Option<String>, &'static str> {
    use matrix_sdk::ruma::api::client::keys::get_keys;

    let own = client.user_id().map(ToOwned::to_owned).ok_or("network")?;
    let mut request = get_keys::v3::Request::new();
    request.device_keys.insert(own.clone(), Vec::new());
    let response = client
        .send(request)
        .await
        .map_err(|err| classify_room_error(&err.to_string()))?;
    let Some(raw) = response.master_keys.get(&own) else {
        return Ok(None);
    };
    let key = raw.deserialize().map_err(|_| "invalid")?;
    key.keys.values().next().cloned().map(Some).ok_or("invalid")
}

/// This session's master public key (from the identity it stores) and
/// whether it holds every private cross-signing key.
async fn local_identity(client: &Client) -> (Option<String>, bool) {
    let enc = client.encryption();
    let complete = enc
        .cross_signing_status()
        .await
        .map(|status| status.is_complete())
        .unwrap_or(false);
    let master = match client.user_id() {
        Some(own) => enc
            .get_user_identity(own)
            .await
            .ok()
            .flatten()
            .and_then(|identity| identity.master_key().get_first_key())
            .map(|key| key.to_base64()),
        None => None,
    };
    (master, complete)
}

async fn server_identity(client: &Client) -> Result<ServerIdentity, &'static str> {
    let server = server_master_key(client).await?;
    let (local, complete) = local_identity(client).await;
    Ok(server_identity_state(server.as_deref(), local.as_deref(), complete))
}

async fn precheck(client: &Client, job: &mut CrossSigningJob) -> Result<(), &'static str> {
    let mut recovery_key = job.consent.recovery_key.take();
    let result = precheck_inner(client, job, recovery_key.as_deref()).await;
    if let Some(key) = recovery_key.as_mut() {
        scrub_string(key);
    }
    result
}

async fn precheck_inner(
    client: &Client,
    job: &mut CrossSigningJob,
    recovery_key: Option<&str>,
) -> Result<(), &'static str> {
    let enc = client.encryption();
    if job.kind == CrossSigningKind::Setup {
        let plan = plan_setup(server_identity(client).await?);
        job.setup_plan = Some(plan);
        // Never auto-reset: an identity we cannot sign with stays as it is
        // until the user explicitly chooses to replace it. Keys an interrupted
        // setup or reset left here that do not match it are dropped, so the
        // session reads "keys missing" (where the explicit reset is offered)
        // rather than "unconfirmed" (whose setup could only fail again).
        if plan == SetupPlan::IdentityWithoutKeys {
            let _ = realign_own_identity(client).await;
            return Err("keys_unavailable");
        }
    }
    let keys_will_change = true;
    let enabled = enc
        .secret_storage()
        .is_enabled()
        .await
        .map_err(|err| classify_room_error(&err.to_string()))?;
    if !enabled {
        let backups = enc.backups();
        if !backups.are_enabled().await
            && backups
                .fetch_exists_on_server()
                .await
                .map_err(|err| classify_room_error(&err.to_string()))?
        {
            return Err("backup_without_recovery");
        }
    }
    let have_key = recovery_key.is_some_and(|key| !key.trim().is_empty());
    let decision =
        decide_storage(enabled, keys_will_change, have_key, job.consent.replace_confirmed)?;
    job.storage = Some(match decision {
        StorageDecision::EnableRecovery => StoragePlan::EnableRecovery,
        StorageDecision::Nothing => StoragePlan::Nothing,
        StorageDecision::ReplaceRecoveryKey => StoragePlan::ReplaceRecoveryKey,
        StorageDecision::ExportIntoExisting => {
            // Opening checks the key against the store's own MAC, so a wrong
            // key is refused here, before anything changes.
            match enc
                .secret_storage()
                .open_secret_store(recovery_key.unwrap_or_default().trim())
                .await
            {
                Ok(store) => StoragePlan::ExportInto(store),
                Err(SecretStorageError::SecretStorageKey(_)) => {
                    return Err("recovery_key_wrong")
                }
                Err(err) => return Err(classify_room_error(&err.to_string())),
            }
        }
    });
    Ok(())
}

/// Ask the server for our own keys so the SDK drops local cross-signing keys
/// that do not match what the server holds (matrix-sdk-crypto
/// identities/manager.rs, `check_private_identity`). True when it answered.
async fn realign_own_identity(client: &Client) -> bool {
    match client.user_id().map(ToOwned::to_owned) {
        Some(own) => client.encryption().request_user_identity(&own).await.is_ok(),
        None => false,
    }
}

pub(crate) fn health_may_realign(slot: &std::sync::Mutex<Option<UiaPending>>) -> bool {
    slot.lock().map(|guard| guard.is_none()).unwrap_or(false)
}

async fn recheck_before_upload(client: &Client) -> Option<Step> {
    match server_identity(client).await {
        Ok(ServerIdentity::Absent) => None,
        Ok(ServerIdentity::MatchesLocalKeys) => Some(Step::Ready),
        Ok(ServerIdentity::Other) => {
            let _ = realign_own_identity(client).await;
            Some(Step::Failed("keys_unavailable"))
        }
        Err(category) => Some(Step::Failed(category)),
    }
}

async fn reset_failure_category(client: &Client) -> &'static str {
    let realigned = realign_own_identity(client).await;
    match server_identity(client).await {
        Ok(ServerIdentity::MatchesLocalKeys) => "reset_incomplete",
        Ok(_) if realigned => "reset_failed",
        _ => "reset_failed_unconfirmed",
    }
}

async fn ensure_own_device_signed(client: &Client) -> Result<(), &'static str> {
    async fn refresh(client: &Client) -> Result<(), &'static str> {
        let own = client.user_id().map(ToOwned::to_owned).ok_or("network")?;
        let enc = client.encryption();
        if enc.request_user_identity(&own).await.is_ok() {
            return Ok(());
        }
        enc.request_user_identity(&own)
            .await
            .map(|_| ())
            .map_err(|err| classify_room_error(&err.to_string()))
    }
    async fn signed(client: &Client) -> bool {
        matches!(
            client.encryption().get_own_device().await,
            Ok(Some(device)) if device.is_cross_signed_by_owner()
        )
    }
    refresh(client).await?;
    if signed(client).await {
        return Ok(());
    }
    let device = client
        .encryption()
        .get_own_device()
        .await
        .map_err(|err| classify_room_error(&err.to_string()))?
        .ok_or("device_signature_failed")?;
    device
        .verify()
        .await
        .map_err(|err| classify_room_error(&err.to_string()))?;
    refresh(client).await?;
    if signed(client).await {
        Ok(())
    } else {
        Err("device_signature_failed")
    }
}

const APPROVAL_POLL_INTERVAL: std::time::Duration = std::time::Duration::from_millis(500);
const APPROVAL_POLLS: u32 = 240;

async fn identity_step(
    client: &Client,
    job: &mut CrossSigningJob,
    auth: Option<AuthData>,
    on_approval: &(dyn Fn(String) + Send + Sync),
) -> Step {
    let enc = client.encryption();
    let wrong_password = auth.is_some();
    match job.kind {
        CrossSigningKind::Setup => {
            if job.bootstrap_started {
                if let Some(step) = recheck_before_upload(client).await {
                    return step;
                }
            } else {
                match job.setup_plan {
                    Some(SetupPlan::Bootstrap) => {}
                    Some(SetupPlan::AlreadyComplete) => return Step::Ready,
                    Some(SetupPlan::IdentityWithoutKeys) => {
                        return Step::Failed("keys_unavailable")
                    }
                    None => return Step::Failed("network"),
                }
            }
            job.bootstrap_started = true;
            let err = match enc.bootstrap_cross_signing(auth).await {
                Ok(()) => return Step::Ready,
                Err(err) => err,
            };
            let Some(info) = err.as_uiaa_response() else {
                return Step::Failed(classify_room_error(&err.to_string()));
            };
            let Ok(Some(params)) = info.params::<OAuthParams>(&AuthType::OAuth) else {
                return Step::Challenge { info: info.clone(), wrong_password };
            };
            on_approval(params.url);
            let mut oauth = uiaa::OAuth::new();
            oauth.session = info.session.clone();
            for _ in 0..APPROVAL_POLLS {
                if job.cancel.is_cancelled() {
                    return Step::Cancelled;
                }
                tokio::time::sleep(APPROVAL_POLL_INTERVAL).await;
                if job.cancel.is_cancelled() {
                    return Step::Cancelled;
                }
                if let Some(step) = recheck_before_upload(client).await {
                    return step;
                }
                match enc
                    .bootstrap_cross_signing(Some(AuthData::OAuth(oauth.clone())))
                    .await
                {
                    Ok(()) => return Step::Ready,
                    Err(e) if e.as_uiaa_response().is_some() => continue,
                    Err(e) => return Step::Failed(classify_room_error(&e.to_string())),
                }
            }
            Step::Failed("timed_out")
        }
        CrossSigningKind::Reset => {
            let handle = match job.handle.clone() {
                Some(handle) => handle,
                None => match {
                    job.reset_started = true;
                    enc.reset_cross_signing().await
                } {
                    Ok(None) => return Step::Ready,
                    Ok(Some(handle)) => {
                        let handle = Arc::new(handle);
                        job.handle = Some(Arc::clone(&handle));
                        match handle.auth_type().clone() {
                            CrossSigningResetAuthType::Uiaa(info) => {
                                return Step::Challenge { info, wrong_password: false }
                            }
                            CrossSigningResetAuthType::OAuth(oauth_info) => {
                                on_approval(oauth_info.approval_url.to_string());
                                job.cancel.attach(&handle).await;
                                let mut oauth = uiaa::OAuth::new();
                                oauth.session = oauth_info.session;
                                let polled = handle.auth(Some(AuthData::OAuth(oauth))).await;
                                return reset_oauth_outcome(client, &job.cancel, polled).await;
                            }
                        }
                    }
                    Err(err) => return Step::Failed(classify_room_error(&err.to_string())),
                },
            };
            match handle.auth(auth).await {
                Ok(()) => Step::Ready,
                Err(err) => match err.as_uiaa_response() {
                    Some(info) => Step::Challenge { info: info.clone(), wrong_password: true },
                    None => Step::Failed(classify_room_error(&err.to_string())),
                },
            }
        }
    }
}

async fn reset_oauth_outcome(
    client: &Client,
    cancel: &CancelToken,
    polled: matrix_sdk::Result<()>,
) -> Step {
    match polled {
        Err(matrix_sdk::Error::Timeout) => Step::Failed("timed_out"),
        Err(err) => Step::Failed(classify_room_error(&err.to_string())),
        Ok(()) if !cancel.is_cancelled() => Step::Ready,
        Ok(()) => match server_identity(client).await {
            Ok(ServerIdentity::MatchesLocalKeys) => Step::Ready,
            Ok(_) => Step::Cancelled,
            Err(_) => Step::Failed("cancel_unconfirmed"),
        },
    }
}

async fn storage_step(
    client: &Client,
    job: &mut CrossSigningJob,
) -> Result<Option<String>, &'static str> {
    let recovery = client.encryption().recovery();
    match job.storage.take() {
        None => Err("network"),
        Some(StoragePlan::Nothing) => Ok(None),
        // Not `wait_for_backups_to_upload()`: the room-key upload carries on
        // in the SDK's backup task, and waiting for it kept the new recovery key
        // from the user for as long as the upload took, exposed to the teardown
        // abort (see `mx_rust_backup_action`).
        Some(StoragePlan::EnableRecovery) => recovery
            .enable()
            .await
            .map(Some)
            .map_err(|err| classify_room_error(&err.to_string())),
        Some(StoragePlan::ExportInto(store)) => store
            .export_secrets()
            .await
            .map(|_| None)
            .map_err(|err| classify_room_error(&err.to_string())),
        Some(StoragePlan::ReplaceRecoveryKey) => recovery
            .reset_key()
            .await
            .map(Some)
            .map_err(|err| classify_room_error(&err.to_string())),
    }
}

async fn drive_cross_signing(
    client: &Client,
    job: &mut CrossSigningJob,
    auth: Option<AuthData>,
    on_approval: &(dyn Fn(String) + Send + Sync),
) -> Drive {
    if job.storage.is_none() {
        if let Err(category) = precheck(client, job).await {
            return Drive::Failed(category);
        }
    }
    match identity_step(client, job, auth, on_approval).await {
        Step::Ready => {}
        Step::Challenge { info, wrong_password } => {
            return Drive::Challenge { info, wrong_password }
        }
        Step::Cancelled => return Drive::Cancelled,
        Step::Failed(_) if job.kind == CrossSigningKind::Reset && job.reset_started => {
            return Drive::Failed(reset_failure_category(client).await)
        }
        Step::Failed(category) => return Drive::Failed(category),
    }
    match server_identity(client).await {
        Ok(ServerIdentity::MatchesLocalKeys) => {}
        Ok(_) => {
            let _ = realign_own_identity(client).await;
            return Drive::Failed(if job.kind == CrossSigningKind::Reset {
                "reset_keys_lost"
            } else {
                "keys_unavailable"
            });
        }
        Err(category) => return Drive::Failed(category),
    }
    if let Err(category) = ensure_own_device_signed(client).await {
        return Drive::Failed(category);
    }
    match storage_step(client, job).await {
        Ok(key) => Drive::Done(key),
        Err(category) => Drive::Failed(category),
    }
}

async fn cancelled_category(client: &Client, job: &CrossSigningJob) -> &'static str {
    if job.kind == CrossSigningKind::Reset && job.reset_started {
        if realign_own_identity(client).await {
            "cancelled_reset"
        } else {
            "cancelled_reset_unconfirmed"
        }
    } else {
        "cancelled"
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) async fn run_cross_signing_pass(
    client: Client,
    mut job: CrossSigningJob,
    auth: Option<AuthData>,
    op_id: u64,
    lifecycle: u64,
    events: crate::EventQueueRef,
    timelines: Arc<crate::timeline::TimelineRegistry>,
    pending_slot: Arc<std::sync::Mutex<Option<UiaPending>>>,
) {
    let _slot = SlotGuard { slot: Arc::clone(&pending_slot), uia_id: op_id };
    let approval_events = Arc::clone(&events);
    let on_approval = move |url: String| {
        enqueue(&approval_events, json!({
            "type": "cross_signing_approval",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "url": url,
        }));
    };
    let mut drive = drive_cross_signing(&client, &mut job, auth, &on_approval).await;
    if matches!(drive, Drive::Challenge { .. }) && job.cancel.is_cancelled() {
        drive = Drive::Cancelled;
    }
    if !timelines.lifecycle_current(lifecycle) {
        return;
    }
    let action = job.action.clone();
    let result = move |ok: bool, key: Option<String>, category: &str| {
        json!({
            "type": "backup_action_result", "op_id": op_id,
            "lifecycle": lifecycle, "action": action, "ok": ok,
            "recovery_key": key.unwrap_or_default(),
            "category": category,
        })
    };
    match drive {
        Drive::Done(key) => {
            release_slot(&pending_slot, op_id);
            enqueue(&events, result(true, key, ""));
        }
        Drive::Failed(category) => {
            release_slot(&pending_slot, op_id);
            enqueue(&events, result(false, None, category));
        }
        Drive::Cancelled => {
            let category = cancelled_category(&client, &job).await;
            release_slot(&pending_slot, op_id);
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, result(false, None, category));
            }
        }
        Drive::Challenge { info, wrong_password } => {
            let cancel = job.cancel.clone();
            if let Ok(mut guard) = pending_slot.lock() {
                *guard = Some(UiaPending {
                    uia_id: op_id,
                    session: info.session.clone(),
                    op: Some(UiaOperation::CrossSigning(job)),
                    cancel: Some(cancel),
                });
            }
            enqueue_challenge(&events, op_id, lifecycle, &info, wrong_password);
        }
    }
}

fn refuse_start(recovery_key: &mut String, message: &str) -> Result<(), String> {
    scrub_string(recovery_key);
    Err(message.to_owned())
}

pub(crate) fn start_cross_signing(
    bridge: &RustClient,
    action: &str,
    mut recovery_key: String,
    replace_recovery_key: bool,
    op_id: u64,
) -> Result<(), String> {
    let Some(kind) = cross_signing_kind_for_action(action) else {
        return refuse_start(&mut recovery_key, "unknown cross-signing action");
    };
    let client = match require_client(bridge) {
        Ok(client) => client,
        Err(err) => return refuse_start(&mut recovery_key, &err),
    };
    let cancel = CancelToken::default();
    {
        let Ok(mut guard) = bridge.uia_pending.lock() else {
            return refuse_start(&mut recovery_key, "internal state unavailable");
        };
        if guard.is_some() {
            return refuse_start(
                &mut recovery_key,
                "another operation is awaiting authentication",
            );
        }
        *guard = Some(UiaPending {
            uia_id: op_id,
            session: None,
            op: None,
            cancel: Some(cancel.clone()),
        });
    }
    let consent = StorageConsent {
        recovery_key: if recovery_key.is_empty() { None } else { Some(recovery_key) },
        replace_confirmed: replace_recovery_key,
    };
    let job = CrossSigningJob::new(kind, action, consent, cancel);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    let reservation = SlotGuard { slot: Arc::clone(&pending_slot), uia_id: op_id };
    bridge.spawn_reported_action("cross_signing", async move {
        let _reservation = reservation;
        run_cross_signing_pass(
            client, job, None, op_id, lifecycle, events, timelines, pending_slot,
        )
        .await;
    });
    Ok(())
}

pub(crate) fn abandon_on_teardown(
    slot: &std::sync::Mutex<Option<UiaPending>>,
) -> Option<Arc<CrossSigningResetHandle>> {
    let pending = slot.lock().ok().and_then(|mut guard| guard.take())?;
    if let Some(UiaOperation::CrossSigning(job)) = &pending.op {
        let _ = job.cancel.request();
    }
    pending.cancel.as_ref().and_then(CancelToken::request)
}

#[cfg(test)]
mod cross_signing_tests {
    use super::*;

    #[test]
    fn setup_never_replaces_an_identity_it_cannot_sign_with() {
        assert_eq!(plan_setup(ServerIdentity::Absent), SetupPlan::Bootstrap);
        assert_eq!(
            plan_setup(ServerIdentity::MatchesLocalKeys),
            SetupPlan::AlreadyComplete
        );
        assert_eq!(
            plan_setup(ServerIdentity::Other),
            SetupPlan::IdentityWithoutKeys
        );
    }

    #[test]
    fn local_keys_without_the_server_identity_are_not_set_up() {
        let local = Some("MASTERKEYbase64");
        assert_eq!(server_identity_state(None, local, true), ServerIdentity::Absent);
        assert_eq!(
            plan_setup(server_identity_state(None, local, true)),
            SetupPlan::Bootstrap
        );
        assert_eq!(
            server_identity_state(Some("MASTERKEYbase64"), local, true),
            ServerIdentity::MatchesLocalKeys
        );
        assert_eq!(
            server_identity_state(Some("MASTERKEYbase64="), local, true),
            ServerIdentity::MatchesLocalKeys
        );
        assert_eq!(
            server_identity_state(Some("SOMEONEELSE"), local, true),
            ServerIdentity::Other
        );
        assert_eq!(
            server_identity_state(Some("MASTERKEYbase64"), local, false),
            ServerIdentity::Other
        );
        assert_eq!(server_identity_state(Some("X"), None, true), ServerIdentity::Other);
        assert_eq!(
            server_identity_state(Some(""), Some(""), true),
            ServerIdentity::Other
        );
    }

    #[test]
    fn an_existing_recovery_key_is_kept_or_replaced_only_with_consent() {
        use StorageDecision::*;
        assert_eq!(decide_storage(false, true, false, false), Ok(EnableRecovery));
        assert_eq!(decide_storage(false, false, false, false), Ok(EnableRecovery));
        assert_eq!(decide_storage(true, true, true, false), Ok(ExportIntoExisting));
        assert_eq!(decide_storage(true, true, true, true), Ok(ExportIntoExisting));
        assert_eq!(decide_storage(true, false, true, false), Ok(ExportIntoExisting));
        assert_eq!(decide_storage(true, false, false, false), Ok(Nothing));
        assert_eq!(decide_storage(true, true, false, true), Ok(ReplaceRecoveryKey));
        assert_eq!(
            decide_storage(true, true, false, false),
            Err("recovery_key_required")
        );
    }

    #[test]
    fn only_the_two_named_actions_run_a_cross_signing_job() {
        assert_eq!(
            cross_signing_kind_for_action("setup_cross_signing"),
            Some(CrossSigningKind::Setup)
        );
        assert_eq!(
            cross_signing_kind_for_action("reset_cross_signing"),
            Some(CrossSigningKind::Reset)
        );
        for other in ["enable", "reset_key", "disable_recovery", "", "setup"] {
            assert_eq!(cross_signing_kind_for_action(other), None, "{other}");
        }
    }

    #[test]
    fn only_the_reset_job_calls_reset_cross_signing() {
        let src = include_str!("uia.rs");
        let step = src
            .split("async fn identity_step")
            .nth(1)
            .expect("identity step present")
            .split("async fn reset_oauth_outcome")
            .next()
            .expect("identity step end");
        let setup = step
            .split("CrossSigningKind::Reset =>")
            .next()
            .expect("setup arm");
        assert!(setup.contains("bootstrap_cross_signing("));
        assert!(!setup.contains("reset_cross_signing"));
        assert_eq!(step.matches("reset_cross_signing()").count(), 1);
        let drive = src
            .split("async fn drive_cross_signing")
            .nth(1)
            .expect("driver present")
            .split("async fn cancelled_category")
            .next()
            .expect("driver end");
        let precheck_at = drive.find("precheck(client, job)").expect("precheck runs");
        let identity_at = drive.find("identity_step(").expect("identity step runs");
        assert!(
            precheck_at < identity_at,
            "secret storage must be decided before the keys change"
        );
        let code = src.split("#[cfg(test)]").next().expect("code");
        assert!(!code.contains("tracing::") && !code.contains("println!"));
    }

    #[test]
    fn the_cross_signing_ffi_is_separate_and_carries_explicit_consent() {
        let lib = include_str!("lib.rs");
        assert!(lib.contains("uia::cross_signing_kind_for_action(&action)"));
        assert!(lib.contains(
            "uia::start_cross_signing(bridge, &action, recovery_key, replace_recovery_key != 0, op_id)"
        ));
    }
}

#[cfg(test)]
mod cross_signing_server_tests {
    use super::*;
    use matrix_sdk::test_utils::mocks::MatrixMockServer;

    fn job_with(kind: CrossSigningKind) -> CrossSigningJob {
        let action = match kind {
            CrossSigningKind::Setup => ACTION_SETUP_CROSS_SIGNING,
            CrossSigningKind::Reset => ACTION_RESET_CROSS_SIGNING,
        };
        let mut job =
            CrossSigningJob::new(kind, action, StorageConsent::default(), CancelToken::default());
        if kind == CrossSigningKind::Setup {
            job.setup_plan = Some(SetupPlan::Bootstrap);
        }
        job.storage = Some(StoragePlan::Nothing);
        job
    }

    fn no_approval() -> impl Fn(String) + Send + Sync {
        |_url: String| {}
    }

    // MAJOR 1. The first upload is refused with a password challenge AFTER the
    // SDK saved the new private identity locally, so this session now "has
    // every key" while the server has none. The retry must send that identity
    // WITH the password. The previous driver re-planned instead, read the
    // local keys as complete, skipped the upload and reported success: the
    // authenticated mock below then never matches, and `verify()` fails.
    #[tokio::test]
    async fn the_password_retry_uploads_the_identity_the_first_attempt_saved() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let own = client.user_id().expect("logged in").to_owned();

        server.mock_upload_keys().ok().mount().await;
        server.mock_query_keys().ok().mount().await;
        server.mock_upload_cross_signing_signatures().ok().mount().await;

        let mut job = job_with(CrossSigningKind::Setup);
        {
            let _challenge = server
                .mock_upload_cross_signing_keys()
                .uiaa()
                .expect(1)
                .mount_as_scoped()
                .await;
            let first = identity_step(&client, &mut job, None, &no_approval()).await;
            let Step::Challenge { info, wrong_password } = first else {
                panic!("the first upload must stop at the password challenge");
            };
            assert!(!wrong_password);
            assert!(info.session.is_some());
        }
        assert!(job.bootstrap_started);
        assert!(
            client.encryption().cross_signing_status().await.unwrap().is_complete(),
            "matrix-sdk saves the identity before the upload; if this changes, \
             revisit CrossSigningJob::bootstrap_started"
        );
        assert_eq!(
            server_identity(&client).await,
            Ok(ServerIdentity::Absent),
            "local keys alone must not read as an identity the server holds"
        );
        assert_eq!(
            plan_setup(server_identity(&client).await.unwrap()),
            SetupPlan::Bootstrap
        );

        let mut password = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(own.to_string())),
            "correct horse".to_owned(),
        );
        password.session = Some("oFIJVvtEOCKmRUTYKTYIIPHL".to_owned());
        let auth = AuthData::Password(password);
        server
            .mock_upload_cross_signing_keys()
            .expect_uiaa_auth_data(&auth)
            .ok()
            .expect(1)
            .named("the retry carries the password")
            .mount()
            .await;
        let retry = identity_step(&client, &mut job, Some(auth), &no_approval()).await;
        assert!(matches!(retry, Step::Ready), "the authenticated retry must upload");
        server.verify().await;
    }

    #[tokio::test]
    async fn cancel_stops_the_setup_approval_wait() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().logged_in_with_oauth().build().await;
        server.mock_upload_keys().ok().mount().await;
        server
            .mock_upload_cross_signing_keys()
            .uiaa_stable_oauth("oauth_session", None)
            .mount()
            .await;

        let mut job = job_with(CrossSigningKind::Setup);
        let cancel = job.cancel.clone();
        let approvals = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let seen = Arc::clone(&approvals);
        let on_approval = move |_url: String| {
            seen.fetch_add(1, Ordering::SeqCst);
            let _ = cancel.request();
        };
        let step = tokio::time::timeout(
            std::time::Duration::from_secs(10),
            identity_step(&client, &mut job, None, &on_approval),
        )
        .await
        .expect("a cancelled approval wait must end promptly");
        assert!(matches!(step, Step::Cancelled));
        assert_eq!(approvals.load(Ordering::SeqCst), 1);
        let uploads = server
            .received_requests()
            .await
            .unwrap_or_default()
            .iter()
            .filter(|request| request.url.path().ends_with("/keys/device_signing/upload"))
            .count();
        assert_eq!(uploads, 1, "no upload may follow the cancel");
    }

    #[tokio::test]
    async fn a_cancelled_reset_approval_is_not_a_finished_reset() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().logged_in_with_oauth().build().await;
        server.mock_upload_keys().ok().mount().await;
        server.mock_query_keys().ok().mount().await;
        server
            .mock_upload_cross_signing_keys()
            .uiaa_stable_oauth("oauth_session", None)
            .mount()
            .await;

        let mut job = job_with(CrossSigningKind::Reset);
        let cancel = job.cancel.clone();
        let on_approval = move |_url: String| {
            let _ = cancel.request();
        };
        let step = tokio::time::timeout(
            std::time::Duration::from_secs(10),
            identity_step(&client, &mut job, None, &on_approval),
        )
        .await
        .expect("a cancelled reset must end promptly");
        assert!(
            matches!(step, Step::Cancelled),
            "a cancel must not read as a finished reset"
        );
        assert!(job.handle.is_some());
        assert_eq!(cancelled_category(&client, &job).await, "cancelled_reset");
    }

    async fn two_sessions(
        server: &MatrixMockServer,
    ) -> (Client, Client) {
        let user = matrix_sdk::ruma::user_id!("@alice:example.org");
        server.mock_crypto_endpoints_preset().await;
        let owner = server
            .client_builder_for_crypto_end_to_end(user, matrix_sdk::ruma::device_id!("OWNER"))
            .build()
            .await;
        let victim = server
            .client_builder_for_crypto_end_to_end(user, matrix_sdk::ruma::device_id!("VICTIM"))
            .build()
            .await;
        (owner, victim)
    }

    async fn local_master(client: &Client) -> Option<String> {
        local_identity(client).await.0
    }

    async fn keys_complete(client: &Client) -> bool {
        local_identity(client).await.1
    }

    #[tokio::test]
    async fn a_failed_reset_puts_this_session_back_on_the_servers_identity() {
        let server = MatrixMockServer::new().await;
        server
            .mock_upload_cross_signing_keys()
            .expect_access_token("TOKEN_1")
            .error500()
            .mount()
            .await;
        let (owner, victim) = two_sessions(&server).await;
        owner.encryption().bootstrap_cross_signing(None).await.expect("owner identity");
        let owners = local_master(&owner).await.expect("owner master");

        let mut job = job_with(CrossSigningKind::Reset);
        let drive = drive_cross_signing(&victim, &mut job, None, &no_approval()).await;
        let Drive::Failed(category) = drive else {
            panic!("an upload error must fail the reset");
        };
        assert_eq!(category, "reset_failed");
        assert!(
            !keys_complete(&victim).await,
            "the replacement keys the server refused must be gone"
        );
        assert_eq!(local_master(&victim).await, Some(owners.clone()));
        assert_eq!(server_master_key(&owner).await, Ok(Some(owners)));
    }

    #[tokio::test]
    async fn keys_the_server_does_not_hold_are_dropped_before_keys_unavailable() {
        let server = MatrixMockServer::new().await;
        server
            .mock_upload_cross_signing_keys()
            .expect_access_token("TOKEN_1")
            .error500()
            .mount()
            .await;
        let (owner, victim) = two_sessions(&server).await;
        owner.encryption().bootstrap_cross_signing(None).await.expect("owner identity");
        assert!(victim.encryption().reset_cross_signing().await.is_err());
        assert!(keys_complete(&victim).await);

        let mut job = CrossSigningJob::new(
            CrossSigningKind::Setup,
            ACTION_SETUP_CROSS_SIGNING,
            StorageConsent::default(),
            CancelToken::default(),
        );
        assert_eq!(precheck_inner(&victim, &mut job, None).await, Err("keys_unavailable"));
        assert!(!keys_complete(&victim).await, "the stale keys must be dropped");
    }

    #[tokio::test]
    async fn finishing_setup_signs_a_device_the_signature_upload_missed() {
        let server = MatrixMockServer::new().await;
        server
            .mock_upload_cross_signing_signatures()
            .expect_any_access_token()
            .error500()
            .up_to_n_times(1)
            .mount()
            .await;
        let (owner, _victim) = two_sessions(&server).await;
        server.mock_sync().ok_and_run(&owner, |_| {}).await;
        assert!(owner.encryption().bootstrap_cross_signing(None).await.is_err());
        assert_eq!(server_identity(&owner).await, Ok(ServerIdentity::MatchesLocalKeys));
        let own_device_signed = |client: Client| async move {
            client
                .encryption()
                .get_own_device()
                .await
                .ok()
                .flatten()
                .is_some_and(|device| device.is_cross_signed_by_owner())
        };

        let mut job = job_with(CrossSigningKind::Setup);
        let plan = plan_setup(server_identity(&owner).await.expect("server answered"));
        assert_eq!(plan, SetupPlan::AlreadyComplete);
        job.setup_plan = Some(plan);
        let drive = drive_cross_signing(&owner, &mut job, None, &no_approval()).await;
        assert!(matches!(drive, Drive::Done(None)));
        assert!(
            own_device_signed(owner.clone()).await,
            "finishing setup must leave this device cross-signed by its owner"
        );
    }

    #[tokio::test]
    async fn a_password_retry_never_replaces_an_identity_that_appeared_meanwhile() {
        let server = MatrixMockServer::new().await;
        server
            .mock_upload_cross_signing_keys()
            .expect_access_token("TOKEN_1")
            .uiaa()
            .up_to_n_times(1)
            .mount()
            .await;
        let (owner, victim) = two_sessions(&server).await;

        let mut job = job_with(CrossSigningKind::Setup);
        let first = identity_step(&victim, &mut job, None, &no_approval()).await;
        assert!(matches!(first, Step::Challenge { .. }));
        owner.encryption().bootstrap_cross_signing(None).await.expect("owner identity");
        let owners = local_master(&owner).await.expect("owner master");

        let mut password = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(
                "@alice:example.org".to_owned(),
            )),
            "correct horse".to_owned(),
        );
        password.session = Some("oFIJVvtEOCKmRUTYKTYIIPHL".to_owned());
        let retry =
            identity_step(&victim, &mut job, Some(AuthData::Password(password)), &no_approval())
                .await;
        assert!(matches!(retry, Step::Failed("keys_unavailable")));
        assert_eq!(
            server_master_key(&owner).await,
            Ok(Some(owners)),
            "the identity another session created must still be the server's"
        );
        assert!(!keys_complete(&victim).await);
    }

    #[tokio::test]
    async fn a_reset_whose_keys_were_lost_while_parked_never_reports_success() {
        let server = MatrixMockServer::new().await;
        server
            .mock_upload_cross_signing_keys()
            .expect_access_token("TOKEN_1")
            .uiaa()
            .up_to_n_times(1)
            .mount()
            .await;
        let (owner, victim) = two_sessions(&server).await;
        owner.encryption().bootstrap_cross_signing(None).await.expect("owner identity");

        let mut job = job_with(CrossSigningKind::Reset);
        let first = drive_cross_signing(&victim, &mut job, None, &no_approval()).await;
        assert!(matches!(first, Drive::Challenge { .. }));
        assert!(keys_complete(&victim).await, "the parked reset holds its new keys");

        let slot = std::sync::Mutex::new(Some(UiaPending {
            uia_id: 1,
            session: None,
            op: None,
            cancel: None,
        }));
        assert!(!health_may_realign(&slot));
        assert!(health_may_realign(&std::sync::Mutex::new(None)));

        assert!(realign_own_identity(&victim).await);
        assert!(!keys_complete(&victim).await);

        let mut password = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(
                "@alice:example.org".to_owned(),
            )),
            "correct horse".to_owned(),
        );
        password.session = Some("oFIJVvtEOCKmRUTYKTYIIPHL".to_owned());
        let retry = drive_cross_signing(
            &victim,
            &mut job,
            Some(AuthData::Password(password)),
            &no_approval(),
        )
        .await;
        match retry {
            Drive::Done(_) => panic!("a reset whose keys are gone must not report success"),
            Drive::Failed(category) => assert_eq!(category, "reset_keys_lost"),
            _ => panic!("expected a failure"),
        }
    }

    #[test]
    fn a_sync_stop_keeps_a_parked_job_and_its_one_result() {
        let dir = std::env::temp_dir().join(format!(
            "lightning-uia-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos())
                .unwrap_or_default()
        ));
        let bridge = RustClient::new(dir.clone()).expect("bridge");
        let (server, client) = bridge.runtime.block_on(async {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            (server, client)
        });
        *bridge.client.lock().unwrap() = Some(client);
        let park = |id: u64| {
            *bridge.uia_pending.lock().unwrap() = Some(UiaPending {
                uia_id: id,
                session: Some("s".to_owned()),
                op: Some(UiaOperation::CrossSigning(job_with(CrossSigningKind::Setup))),
                cancel: Some(CancelToken::default()),
            });
        };
        let parked = |bridge: &RustClient| {
            bridge.uia_pending.lock().unwrap().as_ref().is_some_and(|p| {
                matches!(p.op, Some(UiaOperation::CrossSigning(_)))
            })
        };

        park(77);
        bridge.stop_sync_and_wait();
        assert!(parked(&bridge), "a sync stop must not drop the password prompt's job");

        uia_cancel(&bridge, 77);
        let results = |bridge: &RustClient| -> Vec<serde_json::Value> {
            bridge
                .events
                .lock()
                .unwrap()
                .iter()
                .filter_map(|e| serde_json::from_str::<serde_json::Value>(e).ok())
                .filter(|v| v["type"] == "backup_action_result" && v["op_id"] == 77)
                .collect()
        };
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        while results(&bridge).is_empty() && std::time::Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        let seen = results(&bridge);
        assert_eq!(seen.len(), 1, "exactly one result: {seen:?}");
        assert_eq!(seen[0]["category"], "cancelled");
        assert!(bridge.uia_pending.lock().unwrap().is_none());

        park(78);
        bridge.abandon_uia();
        assert!(bridge.uia_pending.lock().unwrap().is_none());

        bridge.runtime.block_on(async move { drop(server) });
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn the_slot_is_held_while_running_and_freed_at_the_end() {
        let slot = Arc::new(std::sync::Mutex::new(None));
        let cancel = CancelToken::default();
        *slot.lock().unwrap() = Some(UiaPending {
            uia_id: 7,
            session: None,
            op: None,
            cancel: Some(cancel.clone()),
        });
        release_slot(&slot, 8);
        assert!(slot.lock().unwrap().is_some());
        {
            let _guard = SlotGuard { slot: Arc::clone(&slot), uia_id: 7 };
        }
        assert!(slot.lock().unwrap().is_none(), "a finished job frees the slot");

        *slot.lock().unwrap() = Some(UiaPending {
            uia_id: 9,
            session: None,
            op: Some(UiaOperation::DeleteDevices(Vec::new())),
            cancel: Some(cancel.clone()),
        });
        release_slot(&slot, 9);
        assert!(slot.lock().unwrap().is_some(), "a parked job stays answerable");
        assert!(abandon_on_teardown(&slot).is_none());
        assert!(slot.lock().unwrap().is_none());
        assert!(cancel.is_cancelled(), "teardown stops a polling job");
    }
}
