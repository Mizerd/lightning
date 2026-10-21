//! OAuth 2.0 / OIDC authentication on matrix-sdk 0.18's `Client::oauth()`.
//!
//! All protocol primitives are SDK-owned: `OAuth::login()` builds the
//! authorization URL with PKCE and `state`, `OAuth::finish_login()` validates
//! the redirect and exchanges the code, and the SDK refreshes tokens. This
//! module only opens the system browser and receives the loopback redirect
//! (the SDK's `local-server` helper needs `axum`, which is not vendored in
//! this offline build).
//!
//! Two phases, because the account is unknown until `whoami` after
//! `finish_login()`, and opening a store before that would risk attaching a
//! device to the wrong account's crypto store:
//!
//!   Phase A (`mx_rust_oauth_bootstrap_create`): a bootstrap client with an
//!   in-memory store only. Discovery, registration, the authorization URL and
//!   the code exchange happen here; nothing is written to disk. It must
//!   never sync, or it would upload throwaway device keys for the device id
//!   Phase B then uploads again.
//!
//!   Phase B (C++, via `mx_rust_oauth_restore`): once user and device ids are
//!   known, C++ applies the store-ownership policy, creates the real
//!   account-scoped handle and restores the session into its sqlite store.
//!
//! Access tokens, refresh tokens and the registered client id cross once,
//! into the C++ SecretStore; they are never logged, put in error strings or
//! sent to QML. The callback URL carries a `code` and is never logged.

use std::collections::VecDeque;
use std::ffi::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use matrix_sdk::authentication::oauth::error::{
    BasicErrorResponseType, ClientRegistrationErrorResponseType, HttpClientError,
    OAuthClientRegistrationError, OAuthDiscoveryError, OAuthError,
    OAuthRequestError, RequestTokenError,
};
use matrix_sdk::authentication::oauth::registration::{
    ApplicationType, ClientMetadata, Localized, OAuthGrantType,
};
use matrix_sdk::authentication::oauth::{ClientId, OAuthSession, UserSession};
use matrix_sdk::authentication::SessionTokens;
use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::Prompt;
use matrix_sdk::ruma::api::client::session::get_login_types::v3::LoginType;
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::ruma::{OwnedDeviceId, OwnedUserId, UserId};
use matrix_sdk::store::RoomLoadSettings;
use matrix_sdk::{Client, SessionChange, SessionMeta};
use serde_json::json;
use url::Url;

use crate::{
    bridge, build_client, cstr_arg, enqueue, ffi_string, format_matrix_error,
    install_event_handlers, run_async_on,
};

/// Client URI advertised during dynamic registration (shown on the consent
/// screen). Not a secret.
const CLIENT_URI: &str = "https://www.lightning-matrix.org";

/// Lightning's OAuth client metadata for dynamic registration.
/// `redirect_uri` is the sole registered redirect, so the server rejects
/// responses aimed elsewhere. Normally the exact loopback URI the C++
/// listener uses; for servers enforcing RFC 8252 §7.3 strictly, the same
/// URI without its port (see `portless_registration_retry`).
fn client_metadata(redirect_uri: Url) -> Result<Raw<ClientMetadata>, String> {
    let client_uri = Url::parse(CLIENT_URI)
        .map_err(|err| format!("invalid Lightning client URI: {err}"))?;

    let mut metadata = ClientMetadata::new(
        // A native app, so the server expects a loopback redirect.
        ApplicationType::Native,
        vec![OAuthGrantType::AuthorizationCode { redirect_uris: vec![redirect_uri] }],
        Localized::new(client_uri, []),
    );
    metadata.client_name = Some(Localized::new("Lightning".to_owned(), []));

    Raw::new(&metadata)
        .map_err(|err| format!("failed to serialize OAuth client metadata: {err}"))
}

/// The registration-time form of a loopback redirect URI: the same URI
/// without its ephemeral port.
///
/// RFC 8252 §7.3 makes servers accept any port at request time for loopback
/// redirects, and some (Continuwuity) refuse to register a pinned port with
/// `invalid_client_metadata`, blocking OAuth sign-in entirely.
///
/// `None` when there is no explicit port, the scheme is not `http`, or the
/// host is not loopback (`[::1]` is how `host_str()` spells IPv6). Only the
/// port is removed; the path keeps the per-attempt nonce, so the widening
/// is "any port on loopback, this path".
fn portless_loopback_redirect(redirect: &Url) -> Option<Url> {
    if redirect.scheme() != "http" {
        return None;
    }
    match redirect.host_str() {
        Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => {}
        _ => return None,
    }
    // `port()` is None for no port or the default port; nothing to strip.
    redirect.port()?;
    let mut portless = redirect.clone();
    portless.set_port(None).ok()?;
    Some(portless)
}

/// Whether an `OAuth::login().build()` failure is the server refusing our
/// client metadata (RFC 7591 §3.2.2 error code, matched structurally rather
/// than on prose). Returns which code matched, for the retry's report.
fn registration_refused_metadata(err: &OAuthError) -> Option<&'static str> {
    let OAuthError::ClientRegistration(OAuthClientRegistrationError::OAuth(
        RequestTokenError::ServerResponse(response),
    )) = err
    else {
        return None;
    };
    match response.error() {
        ClientRegistrationErrorResponseType::InvalidClientMetadata => {
            Some("invalid_client_metadata")
        }
        ClientRegistrationErrorResponseType::InvalidRedirectUri => {
            Some("invalid_redirect_uri")
        }
        _ => None,
    }
}

/// How to retry a refused registration: metadata with the loopback redirect
/// URI minus its port, plus the redirect URI for the authorization request
/// (the live listener URI, with port), the split RFC 8252 §7.3 intends.
/// matrix-sdk 0.18 allows it: `OAuth::login()` takes the request URI
/// directly, and `finish_login()` sends that same URI in the token exchange.
///
/// `None` when the failure was not a metadata refusal, there is no port, or
/// the URI is not loopback `http`.
///
/// Retry rather than always registering portless: MAS accepts the pinned
/// port and is the only server this flow was live-validated against. Both
/// continuwuity (for `application_type: native`) and MAS
/// (`Client::resolve_redirect_uri`) strip the port when matching.
fn portless_registration_retry(
    redirect: &Url,
    err: &OAuthError,
) -> Option<(Raw<ClientMetadata>, Url, &'static str)> {
    let reason = registration_refused_metadata(err)?;
    let portless = portless_loopback_redirect(redirect)?;
    let metadata = client_metadata(portless).ok()?;
    Some((metadata, redirect.clone(), reason))
}

/// A server-chosen code, reduced to what a log may carry: ASCII letters,
/// digits and `_ . -`, at most 48 characters, or `unrecognised`.
pub(crate) fn wire_token(code: &str) -> String {
    let loggable = !code.is_empty()
        && code.len() <= 48
        && code.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-'));
    if loggable { code.to_owned() } else { "unrecognised".to_owned() }
}

fn transport_reason(timeout: bool, connect: bool) -> String {
    if timeout {
        "timeout".to_owned()
    } else if connect {
        "connect".to_owned()
    } else {
        "network".to_owned()
    }
}

fn registration_request_reason(
    err: &OAuthRequestError<ClientRegistrationErrorResponseType>,
) -> String {
    match err {
        RequestTokenError::ServerResponse(response) => wire_token(response.error().as_str()),
        RequestTokenError::Request(HttpClientError::Reqwest(inner)) => {
            transport_reason(inner.is_timeout(), inner.is_connect())
        }
        RequestTokenError::Request(_) => "network".to_owned(),
        RequestTokenError::Parse(..) => "bad_response".to_owned(),
        RequestTokenError::Other(_) => "other".to_owned(),
    }
}

fn token_request_reason(err: &OAuthRequestError<BasicErrorResponseType>) -> String {
    match err {
        RequestTokenError::ServerResponse(response) => {
            let code: &str = response.error().as_ref();
            wire_token(code)
        }
        RequestTokenError::Request(HttpClientError::Reqwest(inner)) => {
            transport_reason(inner.is_timeout(), inner.is_connect())
        }
        RequestTokenError::Request(_) => "network".to_owned(),
        RequestTokenError::Parse(..) => "bad_response".to_owned(),
        RequestTokenError::Other(_) => "other".to_owned(),
    }
}

/// Why an OAuth step failed, as a fixed token for the log. Never carries a
/// URL, code, state, token or server prose; the UI keeps its own wording.
pub(crate) fn oauth_failure_reason(err: &OAuthError) -> String {
    use matrix_sdk::authentication::oauth::error::OAuthAuthorizationCodeError as Code;
    match err {
        OAuthError::Discovery(OAuthDiscoveryError::NotSupported) => {
            "discovery_not_supported".to_owned()
        }
        OAuthError::Discovery(_) => "discovery_failed".to_owned(),
        OAuthError::ClientRegistration(OAuthClientRegistrationError::NotSupported) => {
            "registration_not_supported".to_owned()
        }
        OAuthError::ClientRegistration(OAuthClientRegistrationError::OAuth(err)) => {
            format!("registration_{}", registration_request_reason(err))
        }
        OAuthError::ClientRegistration(_) => "registration_bad_json".to_owned(),
        OAuthError::NotRegistered => "not_registered".to_owned(),
        OAuthError::AuthorizationCode(Code::InvalidState) => "state_mismatch".to_owned(),
        OAuthError::AuthorizationCode(Code::Cancelled) => "cancelled".to_owned(),
        OAuthError::AuthorizationCode(Code::RedirectUri(_)) => "callback_unparsable".to_owned(),
        OAuthError::AuthorizationCode(Code::Authorization(response)) => {
            format!("authorization_{}", wire_token(response.error().as_str()))
        }
        OAuthError::AuthorizationCode(Code::RequestToken(err)) => {
            format!("token_{}", token_request_reason(err))
        }
        OAuthError::SessionMismatch => "session_mismatch".to_owned(),
        _ => "other".to_owned(),
    }
}

/// Why a Matrix request failed, as a fixed token for the log: the HTTP status
/// and errcode for a server answer, the transport class otherwise.
pub(crate) fn http_failure_reason(err: &matrix_sdk::HttpError) -> String {
    if let Some(api) = err.as_client_api_error() {
        let code = api
            .error_kind()
            .map(|kind| wire_token(kind.errcode().as_str()))
            .unwrap_or_else(|| "no_errcode".to_owned());
        return format!("http_{}_{}", api.status_code.as_u16(), code);
    }
    match err {
        matrix_sdk::HttpError::Reqwest(inner) => {
            transport_reason(inner.is_timeout(), inner.is_connect())
        }
        _ => "http_other".to_owned(),
    }
}

/// `oauth_failure_reason` / `http_failure_reason` for the SDK's own error.
pub(crate) fn sdk_failure_reason(err: &matrix_sdk::Error) -> String {
    match err {
        matrix_sdk::Error::OAuth(err) => oauth_failure_reason(err),
        matrix_sdk::Error::Http(err) => http_failure_reason(err),
        _ => "other".to_owned(),
    }
}

/// Persist rotated session tokens for the lifetime of this client.
///
/// With `handle_refresh_tokens()` the SDK renews expired access tokens but
/// does not persist them. Without this, the store keeps the consumed refresh
/// token, and an OAuth 2.1 / MAS server treats its replay as compromise and
/// may revoke the whole token family. Refreshable password sessions rotate
/// the same way.
///
/// The emitted event carries credentials; C++ writes them to the
/// SecretStore and nothing logs them. Store the returned handle in
/// `RustClient::token_task` so shutdown can abort it: it holds a strong
/// Client whose own broadcast sender keeps `recv()` from ever closing.
#[must_use]
pub(crate) fn spawn_token_persistence(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
    ledger: Option<Arc<crate::sessiontokens::TokenLedger>>,
) -> tokio::task::JoinHandle<()> {
    spawn_token_persistence_with(client, client.subscribe_to_session_changes(), events, ledger)
}

/// `spawn_token_persistence` over a receiver subscribed EARLIER. A broadcast
/// receiver sees only what is sent after it exists, and a restore can rotate
/// before it returns: `restore_session()` spawns matrix-sdk's E2EE
/// initialization task, whose requests (backup, recovery) carry the restored
/// access token, usually an expired one for OAuth, and so refresh. A receiver
/// made after that misses the rotation, and the keyring keeps the consumed
/// refresh token until the next one. Subscribe before restoring.
///
/// With `ledger` whose save callback is installed on this client
/// (sessiontokens.rs), rotations are announced there, inside the refresh, and
/// this watcher only reports revocations; it still announces them itself when
/// the callback could not be installed, or for a client without a ledger.
#[must_use]
pub(crate) fn spawn_token_persistence_with(
    client: &Client,
    changes: tokio::sync::broadcast::Receiver<SessionChange>,
    events: Arc<Mutex<VecDeque<String>>>,
    ledger: Option<Arc<crate::sessiontokens::TokenLedger>>,
) -> tokio::task::JoinHandle<()> {
    spawn_token_persistence_custody(
        client,
        changes,
        events,
        ledger,
        Arc::new(crate::labs::CustodyFacts::default()),
    )
}

/// `spawn_token_persistence_with`, also recording what it sees in the
/// handle's custody facts (design 2026-10-15 §3.2.7): a rotation bumps the
/// generation, `UnknownToken` sets the sticky revoked flag. The queue output
/// is unchanged; a Background handle's gate keeps `session_tokens_refreshed`
/// off its queue, because custody is its only token path.
///
/// A rotation announced by the ledger's save callback has ALREADY bumped the
/// custody generation inside the refresh (`sessiontokens::Sink::custody`), so
/// a Background host takes custody and answers while the SDK waits; this
/// watcher does not bump it a second time for the same rotation.
#[must_use]
pub(crate) fn spawn_token_persistence_custody(
    client: &Client,
    mut changes: tokio::sync::broadcast::Receiver<SessionChange>,
    events: Arc<Mutex<VecDeque<String>>>,
    ledger: Option<Arc<crate::sessiontokens::TokenLedger>>,
    custody: Arc<crate::labs::CustodyFacts>,
) -> tokio::task::JoinHandle<()> {
    let client = client.clone();
    tokio::spawn(async move {
        loop {
            match changes.recv().await {
                Ok(SessionChange::TokensRefreshed) => {
                    if ledger.as_ref().is_some_and(|l| l.callbacks_active()) {
                        continue;
                    }
                    let Some(tokens) = client.session_tokens() else { continue };
                    custody.note_refreshed();
                    enqueue(
                        &events,
                        json!({
                            "type": "session_tokens_refreshed",
                            "access_token": tokens.access_token,
                            "refresh_token": tokens.refresh_token,
                        }),
                    );
                }
                // The server rejected the token and the SDK could not renew it: report the
                // revoked-credential state instead of letting sync fail in a loop. A soft
                // logout keeps the device on the server; only a hard one proves it gone,
                // which is what lets a new sign-in move this device's store aside.
                Ok(SessionChange::UnknownToken(data)) => {
                    // Whether the session had a refresh token to try: without
                    // one matrix-sdk reports this without asking the server
                    // (`RefreshTokenRequired`), with one the server refused
                    // it. A boolean only, never the token.
                    let had_refresh_token = client
                        .session_tokens()
                        .is_some_and(|tokens| tokens.refresh_token.is_some());
                    custody.note_revoked(data.soft_logout, had_refresh_token);
                    let mut event = json!({
                        "type": "session_token_revoked",
                        "soft_logout": data.soft_logout,
                        "had_refresh_token": had_refresh_token,
                    });
                    // Why the server may have refused: counts and a boolean,
                    // never a token. `rotations` 0 means the refused refresh
                    // token is the one this session was restored with (from
                    // the keyring); `unsaved_rotation` that the session had
                    // rotated past the last pair C++ confirmed in the keyring.
                    if let Some(stats) = ledger.as_ref().map(|l| l.stats()) {
                        if let Some(map) = event.as_object_mut() {
                            map.insert("rotations".to_owned(), json!(stats.rotations));
                            map.insert("unsaved_rotation".to_owned(),
                                       json!(stats.unsaved_rotation));
                            map.insert("since_rotation_ms".to_owned(),
                                       json!(stats.since_rotation_ms));
                        }
                    }
                    enqueue(&events, event);
                }
                // Lagged only means missed notifications; the next carries current tokens.
                // A missed one may have been a rotation, so the custody generation
                // moves anyway: a host re-taking custody for nothing costs one read,
                // one that never re-takes keeps a consumed refresh token.
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                    custody.note_refreshed();
                    continue;
                }
                Err(_) => break,
            }
        }
    })
}

/// Phase A bootstrap handle: a `RustClient` with an empty store path, so
/// `build_client()` uses the in-memory store. Destroy with `mx_rust_destroy`
/// once the session is handed to the real account handle.
#[no_mangle]
pub extern "C" fn mx_rust_oauth_bootstrap_create() -> *mut c_void {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let client = crate::RustClient::new(PathBuf::new())?;
        Ok::<*mut c_void, String>(Box::into_raw(Box::new(client)) as *mut c_void)
    })) {
        Ok(Ok(ptr)) => ptr,
        Ok(Err(_)) | Err(_) => std::ptr::null_mut(),
    }
}

/// Discover which authentication methods a homeserver offers. Enqueues one
/// `auth_discovery` event:
///
/// ```json
/// { "type": "auth_discovery", "homeserver": "...", "oauth": true,
///   "password": true, "sso": false, "error": null }
/// ```
///
/// `oauth` is true when the server publishes OAuth authorization server
/// metadata; `password`/`sso` come from `/login` flows. Nothing is
/// hard-coded per provider.
///
/// Also, on success: `resolved_homeserver`, the client API base URL the SDK
/// resolved (the login screen warns when it is plain http off loopback);
/// `oauth_can_create`, whether the metadata lists `prompt=create` (sign-up
/// through the server's own page, as Element does); `account_management_uri`,
/// the metadata's account page, https only. `error` is set only when the
/// client could not be built, i.e. the server could not be reached.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_discover(
    ptr: *mut c_void,
    homeserver: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;

        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_discover", async move {
                // In-memory client: discovery must not create a store.
                let client = match build_client(&homeserver, &PathBuf::new()).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(
                            &events,
                            json!({
                                "type": "auth_discovery",
                                "homeserver": homeserver,
                                "oauth": false,
                                "password": false,
                                "sso": false,
                                "error": err,
                            }),
                        );
                        return;
                    }
                };

                // An error here means "not an OAuth server", not a failure.
                let metadata = client.oauth().server_metadata().await.ok();
                let oauth_supported = metadata.is_some();
                // `Prompt` has no PartialEq; compare its wire string.
                let oauth_can_create = metadata.as_ref().is_some_and(|m| {
                    m.prompt_values_supported
                        .iter()
                        .any(|p| p.as_str() == Prompt::Create.as_str())
                });
                // Opened in the browser from the login screen: https only.
                let account_management_uri = metadata
                    .as_ref()
                    .and_then(|m| m.account_management_uri.as_ref())
                    .filter(|uri| uri.scheme() == "https" && uri.host().is_some())
                    .map(|uri| uri.to_string())
                    .unwrap_or_default();

                // Not fatal: an OAuth-only server may not answer /login.
                let (password, sso) = match client.matrix_auth().get_login_types().await {
                    Ok(response) => {
                        let mut password = false;
                        let mut sso = false;
                        for flow in &response.flows {
                            match flow {
                                LoginType::Password(_) => password = true,
                                LoginType::Sso(_) => sso = true,
                                _ => {}
                            }
                        }
                        (password, sso)
                    }
                    Err(_) => (false, false),
                };

                enqueue(
                    &events,
                    json!({
                        "type": "auth_discovery",
                        "homeserver": homeserver,
                        "oauth": oauth_supported,
                        "password": password,
                        "sso": sso,
                        "resolved_homeserver": client.homeserver().to_string(),
                        "oauth_can_create": oauth_can_create,
                        "account_management_uri": account_management_uri,
                        "error": serde_json::Value::Null,
                    }),
                );
            });
        });

        Ok(String::new())
    })
}

/// Begin an OAuth login: register the client if needed and build the
/// authorization URL. `prompt_create` non-zero asks the server's page to
/// create an account (`prompt=create`); the rest of the flow is identical.
/// Enqueues `{"type": "oauth_url", "url": "..."}` or
/// `{"type": "oauth_failed", "message": "..."}`. The bootstrap Client is
/// parked in the client slot because the SDK stores this attempt's PKCE
/// verifier and CSRF state in it; `finish_login()` must use the same
/// instance. The URL (opened by C++) contains no credentials.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_begin(
    ptr: *mut c_void,
    homeserver: *const c_char,
    redirect_uri: *const c_char,
    prompt_create: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let redirect_uri = unsafe { cstr_arg(redirect_uri) }?;

        let redirect = Url::parse(&redirect_uri)
            .map_err(|err| format!("invalid OAuth redirect URI: {err}"))?;
        // Defence in depth: never ask a server to redirect anywhere but loopback.
        match redirect.host_str() {
            Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => {}
            _ => return Err("OAuth redirect URI must be a loopback address.".to_owned()),
        }

        let metadata = client_metadata(redirect.clone())?;

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_begin", async move {
                let client = match build_client(&homeserver, &PathBuf::new()).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "oauth_failed",
                            "stage": "begin",
                            "reason": "client_build_failed",
                            "message": err,
                        }));
                        return;
                    }
                };

                // device_id None: the SDK generates one, encoded in the scope, and returns
                // it from finish_login(); Phase B's store must belong to it.
                // A sign-up only where the server's metadata lists it; C++ asks
                // only then, and this keeps the rule where the request is built.
                if prompt_create != 0 {
                    let supported = client.oauth().server_metadata().await.is_ok_and(|m| {
                        m.prompt_values_supported
                            .iter()
                            .any(|p| p.as_str() == Prompt::Create.as_str())
                    });
                    if !supported {
                        drop(client);
                        enqueue(&events, json!({
                            "type": "oauth_failed",
                            "stage": "begin",
                            "reason": "prompt_create_unsupported",
                            "message": "This server does not create accounts from Lightning.",
                        }));
                        return;
                    }
                }
                // Only a sign-up sets `prompt`; a sign-in leaves it out entirely.
                let with_prompt = |request: matrix_sdk::authentication::oauth::OAuthAuthCodeUrlBuilder| {
                    if prompt_create != 0 {
                        request.prompt(vec![Prompt::Create])
                    } else {
                        request
                    }
                };
                let mut built = with_prompt(client.oauth().login(
                    redirect.clone(),
                    None,
                    Some(metadata.into()),
                    None,
                ))
                .build()
                .await;

                // A server enforcing RFC 8252 §7.3 refuses a pinned loopback port: retry
                // once with the port removed from the metadata only. Reusing this client is
                // safe: a failed registration leaves client_id unset.
                let retry = built
                    .as_ref()
                    .err()
                    .and_then(|err| portless_registration_retry(&redirect, err));
                if let Some((retry_metadata, request_redirect, reason)) = retry {
                    // Log that the retry happened (no URI, port, nonce or token), so reports
                    // can tell "never fired", "worked" and "refused again" apart.
                    enqueue(&events, json!({
                        "type": "oauth_registration_retry",
                        "reason": reason,
                    }));
                    built = with_prompt(client.oauth().login(
                        request_redirect,
                        None,
                        Some(retry_metadata.into()),
                        None,
                    ))
                    .build()
                    .await;
                }

                match built {
                    Ok(data) => {
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        if let Ok(mut guard) = state_slot.lock() {
                            *guard = Some(data.state.clone());
                        }
                        enqueue(
                            &events,
                            json!({ "type": "oauth_url", "url": data.url.to_string() }),
                        );
                    }
                    Err(err) => {
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "oauth_failed",
                                "stage": "begin",
                                "reason": oauth_failure_reason(&err),
                                "message": format_matrix_error(
                                    "Matrix OAuth authorization request failed", err),
                            }),
                        );
                    }
                }
            });
        });

        Ok(String::new())
    })
}

/// Complete an OAuth login from the callback the loopback listener got.
/// `callback` (full redirect URI or query) carries `code` and `state`, so it
/// is never logged or echoed in errors.
///
/// The SDK validates `state` and does the PKCE code exchange. On success
/// this enqueues `oauth_ok` with what C++ needs to open the account store:
///
/// ```json
/// { "type": "oauth_ok", "homeserver": "...", "user_id": "@u:s",
///   "device_id": "ABC", "client_id": "...", "access_token": "...",
///   "refresh_token": "..." }
/// ```
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_finish(
    ptr: *mut c_void,
    callback: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let callback = unsafe { cstr_arg(callback) }?;
        if callback.trim().is_empty() {
            return Err("empty OAuth callback".to_owned());
        }
        // Parsed synchronously to fail fast; the error never quotes the input (it
        // carries the authorization code).
        let callback_url = Url::parse(&callback)
            .map_err(|_| "malformed OAuth callback".to_owned())?;

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_finish", async move {
                let client = match client_slot.lock().ok().and_then(|mut g| g.take()) {
                    Some(client) => client,
                    None => {
                        enqueue(
                            &events,
                            json!({
                                "type": "oauth_failed",
                                "stage": "finish",
                                "reason": "no_attempt",
                                "message": "No OAuth sign-in is in progress.",
                            }),
                        );
                        return;
                    }
                };

                // The SDK checks `state` and exchanges the code with the PKCE verifier; a
                // mismatch, denial or replayed callback fails here.
                if let Err(err) = client.oauth().finish_login(callback_url.into()).await {
                    // The error may quote the callback, so only its category travels.
                    let reason = sdk_failure_reason(&err);
                    drop(client);
                    if let Ok(mut guard) = state_slot.lock() {
                        *guard = None;
                    }
                    enqueue(
                        &events,
                        json!({
                            "type": "oauth_failed",
                            "stage": "finish",
                            "reason": reason,
                            "message": "The sign-in could not be completed. \
                                        The authorization may have been denied, \
                                        cancelled, or it expired. Please try again.",
                        }),
                    );
                    return;
                }

                let session = match client.oauth().full_session() {
                    Some(session) => session,
                    None => {
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "oauth_failed",
                                "stage": "finish",
                                "reason": "no_session",
                                "message": "The server completed sign-in without \
                                            returning a session.",
                            }),
                        );
                        return;
                    }
                };

                let OAuthSession { client_id, user: UserSession { meta, tokens } } = session;
                enqueue(
                    &events,
                    json!({
                        "type": "oauth_ok",
                        "user_id": meta.user_id.to_string(),
                        "device_id": meta.device_id.to_string(),
                        "client_id": client_id.as_str(),
                        "access_token": tokens.access_token,
                        "refresh_token": tokens.refresh_token,
                    }),
                );

                // Phase A is over: drop the bootstrap client and its in-memory tokens.
                drop(client);
                if let Ok(mut guard) = state_slot.lock() {
                    *guard = None;
                }
            });
        });

        Ok(String::new())
    })
}

/// Abort an OAuth login in progress. Clears the SDK's stored authorization
/// data so a late or replayed callback cannot complete it, and releases the
/// bootstrap client.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_abort(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_abort", async move {
                let client = client_slot.lock().ok().and_then(|mut g| g.take());
                let state = state_slot.lock().ok().and_then(|mut g| g.take());
                if let (Some(client), Some(state)) = (client.as_ref(), state.as_ref()) {
                    client.oauth().abort_login(state).await;
                }
                drop(client);
            });
        });

        Ok(String::new())
    })
}

/// Build the account's client and restore an OAuth session into it. Returns
/// the client and a session-change receiver subscribed BEFORE
/// `restore_session()`: that call starts matrix-sdk's E2EE initialization
/// task, whose requests carry the restored (usually expired) access token and
/// so can rotate the tokens before this returns. A receiver made after it
/// misses that rotation and the keyring keeps the consumed refresh token.
pub(crate) async fn restore_oauth_client(
    homeserver: &str,
    store_path: &std::path::Path,
    session: OAuthSession,
    events: &Arc<Mutex<VecDeque<String>>>,
    token_sink: Option<&crate::sessiontokens::Sink>,
) -> Result<(Client, tokio::sync::broadcast::Receiver<SessionChange>), String> {
    restore_oauth_client_then(homeserver, store_path, session, events, token_sink, |_| async {})
        .await
}

/// `restore_oauth_client` with `after_restore` run right after
/// `restore_session()`: it stands for what that call set running, so a test
/// can rotate the tokens at exactly that point. Production passes nothing.
pub(crate) async fn restore_oauth_client_then<F, Fut>(
    homeserver: &str,
    store_path: &std::path::Path,
    session: OAuthSession,
    events: &Arc<Mutex<VecDeque<String>>>,
    token_sink: Option<&crate::sessiontokens::Sink>,
    after_restore: F,
) -> Result<(Client, tokio::sync::broadcast::Receiver<SessionChange>), String>
where
    F: FnOnce(Client) -> Fut,
    Fut: std::future::Future<Output = ()>,
{
    // The offline-capable build, as for password restores (see
    // build_client_for_restore).
    let client = crate::build_client_for_restore(homeserver, store_path, events).await?;
    // The SDK's save callback, before restore_session(): every rotation this
    // client makes, the restore's own included, is recorded and announced
    // inside the refresh, and held until C++ has written it.
    if let Some(sink) = token_sink {
        sink.install_on(&client);
    }
    let changes = client.subscribe_to_session_changes();
    if let Err(err) = crate::switchtrace::timed(
        Some(crate::switchtrace::RestoreStep::Restore),
        client.oauth().restore_session(session, RoomLoadSettings::default()),
    )
    .await
    {
        drop(client);
        return Err(format_matrix_error("Matrix OAuth session restore failed", err));
    }
    after_restore(client.clone()).await;
    // On the shared runtime with rooms loaded, as the send-queue respawn
    // requires (see resume_unsent_requests in lib.rs).
    crate::switchtrace::timed(
        Some(crate::switchtrace::RestoreStep::ResumeUnsent),
        crate::resume_unsent_requests(&client),
    )
    .await;
    Ok((client, changes))
}

/// Phase B: restore an OAuth session into the real account-scoped store,
/// like `mx_rust_restore` but via `oauth().restore_session()` so the SDK
/// owns token refresh. `refresh_token` may be empty.
///
/// The caller must already have decided, from the recorded identity and the
/// store-ownership policy, that `store_path` belongs to `user_id` /
/// `device_id`; this function cannot judge that.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_restore(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user_id: *const c_char,
    device_id: *const c_char,
    client_id: *const c_char,
    access_token: *const c_char,
    refresh_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;
        let client_id = unsafe { cstr_arg(client_id) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;
        let refresh_token = unsafe { cstr_arg(refresh_token) }?;

        if client_id.trim().is_empty() {
            return Err("missing OAuth client registration".to_owned());
        }

        let parsed_user: OwnedUserId = UserId::parse(&user_id)
            .map_err(|err| format!("invalid stored Matrix user id: {err}"))?
            .to_owned();
        // A server-issued opaque string.
        let parsed_device: OwnedDeviceId = device_id.clone().into();

        bridge.abandon_uia();
        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let token_task = Arc::clone(&bridge.token_task);
        let token_sink = bridge.token_sink();
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        let shared_runtime = Arc::clone(&bridge.runtime);
        let custody = Arc::clone(&bridge.custody);
        // Labs: this handle's reservation and precheck (none with the flag off).
        let open_context = bridge.open_context();
        crate::spawn_session_thread(bridge.op_thread_name(), move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_restore",
                crate::labs::scope_open(open_context, crate::switchtrace::restore_scope(
                    crate::switchtrace::switch_trace_enabled(), async move {
                let session = OAuthSession {
                    client_id: ClientId::new(client_id),
                    user: UserSession {
                        meta: SessionMeta {
                            user_id: parsed_user,
                            device_id: parsed_device,
                        },
                        tokens: SessionTokens {
                            access_token,
                            refresh_token: if refresh_token.is_empty() {
                                None
                            } else {
                                Some(refresh_token)
                            },
                        },
                    },
                };
                match restore_oauth_client(
                    &homeserver, &store_path, session, &events, Some(&token_sink)).await {
                    Ok((client, changes)) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                            Arc::clone(&sync_notify),
                        );
                        // OAuth access tokens are short-lived, so rotations must be persisted.
                        if let Ok(mut guard) = token_task.lock() {
                            if let Some(previous) = guard.replace(
                                spawn_token_persistence_custody(
                                    &client, changes, Arc::clone(&events),
                                    Some(Arc::clone(&token_sink.ledger)), custody))
                            {
                                previous.abort();
                            }
                        }
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            crate::switchtrace::with_restore_trace(json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": user_id,
                                "device_id": device_id,
                            })),
                        );
                    }
                    Err(message) => {
                        enqueue(&events, crate::switchtrace::with_restore_trace(
                            json!({ "type": "login_failed", "message": message })));
                    }
                }
            })));
        });

        Ok(String::new())
    })
}

/// End, at the server, a session a browser sign-in created and Phase B then
/// refused to keep on this device (an existing store for another device, a
/// storage-name collision, ...). Without this the server keeps a device and a
/// live sign-in nobody holds. `flow` is "oauth" (revoke at the authorization
/// server) or "sso" (`/logout`). Best effort and bounded
/// (`REFUSED_SIGN_IN_REVOKE_BUDGET`): an in-memory client, no store, no sync.
/// Enqueues `{"type":"refused_sign_in_revoked","category":...}` on this
/// handle, a fixed word only. SENSITIVE: the arguments carry credentials and
/// are never logged.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_revoke_refused_sign_in(
    ptr: *mut c_void,
    homeserver: *const c_char,
    flow: *const c_char,
    client_id: *const c_char,
    user_id: *const c_char,
    device_id: *const c_char,
    access_token: *const c_char,
    refresh_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let flow = unsafe { cstr_arg(flow) }?;
        let client_id = unsafe { cstr_arg(client_id) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;
        let refresh_token = unsafe { cstr_arg(refresh_token) }?;
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "revoke_refused_sign_in", async move {
                let category = match tokio::time::timeout(
                    REFUSED_SIGN_IN_REVOKE_BUDGET,
                    revoke_refused_sign_in(
                        &homeserver, &flow, client_id, &user_id, device_id,
                        access_token, refresh_token),
                )
                .await
                {
                    Ok(Ok(())) => "revoked",
                    Ok(Err(category)) => category,
                    Err(_) => "timed_out",
                };
                enqueue(
                    &events,
                    json!({ "type": "refused_sign_in_revoked", "category": category }),
                );
            });
        });
        Ok(String::new())
    })
}

/// Phase 2 of adopting a sign-in saved before install scoping (password
/// sessions): ask the server whose device the stored access token belongs
/// to, BEFORE anything is restored. A plain `GET /account/whoami` with no
/// client session at all, so no crypto, no sync and no refresh can run on a
/// token another install may be using. Enqueues
/// `{"type":"whoami_probe","result":"device","device_id":"..."}`, or a result
/// of "unauthorized" (401), "unreachable" or "other". Bounded.
/// SENSITIVE: `access_token` is a credential and is never logged.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_whoami_probe(
    ptr: *mut c_void,
    homeserver: *const c_char,
    access_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "whoami_probe", async move {
                let (result, device_id) = match tokio::time::timeout(
                    WHOAMI_PROBE_BUDGET,
                    whoami_probe(&homeserver, &access_token),
                )
                .await
                {
                    Ok(answer) => answer,
                    Err(_) => ("unreachable", None),
                };
                enqueue(
                    &events,
                    json!({ "type": "whoami_probe", "result": result, "device_id": device_id }),
                );
            });
        });
        Ok(String::new())
    })
}

/// How long the adoption check may take; past it the restore goes on.
const WHOAMI_PROBE_BUDGET: std::time::Duration = std::time::Duration::from_secs(15);

/// The check itself: a fixed result word and, for a 200, the device id.
pub(crate) async fn whoami_probe(
    homeserver: &str,
    access_token: &str,
) -> (&'static str, Option<String>) {
    if access_token.is_empty() {
        return ("other", None);
    }
    // In memory and without a session: only its resolved URL and HTTP client
    // are used.
    let Ok(client) = build_client(homeserver, &PathBuf::new()).await else {
        return ("unreachable", None);
    };
    let mut url = client.homeserver();
    let Ok(mut segments) = url.path_segments_mut() else {
        return ("other", None);
    };
    segments.pop_if_empty().extend(["_matrix", "client", "v3", "account", "whoami"]);
    drop(segments);
    let response = match client
        .http_client()
        .get(url)
        .header(reqwest::header::AUTHORIZATION, format!("Bearer {access_token}"))
        .timeout(std::time::Duration::from_secs(10))
        .send()
        .await
    {
        Ok(response) => response,
        Err(_) => return ("unreachable", None),
    };
    match response.status().as_u16() {
        200 => {
            let device = response
                .bytes()
                .await
                .ok()
                .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok())
                .and_then(|body| body["device_id"].as_str().map(str::to_owned));
            match device {
                Some(device) => ("device", Some(device)),
                None => ("other", None),
            }
        }
        401 => ("unauthorized", None),
        _ => ("other", None),
    }
}

/// How long ending a refused sign-in may take before it is given up.
const REFUSED_SIGN_IN_REVOKE_BUDGET: std::time::Duration = std::time::Duration::from_secs(20);

/// The revocation itself. A fixed category on failure, never server text.
pub(crate) async fn revoke_refused_sign_in(
    homeserver: &str,
    flow: &str,
    client_id: String,
    user_id: &str,
    device_id: String,
    access_token: String,
    refresh_token: String,
) -> Result<(), &'static str> {
    if access_token.is_empty() {
        return Err("no_session");
    }
    let parsed_user: OwnedUserId =
        UserId::parse(user_id).map_err(|_| "bad_identity")?.to_owned();
    let meta = SessionMeta { user_id: parsed_user, device_id: device_id.into() };
    let tokens = SessionTokens {
        access_token,
        refresh_token: if refresh_token.is_empty() { None } else { Some(refresh_token) },
    };
    // In memory: nothing of this session may reach a store on this device.
    let client = build_client(homeserver, &PathBuf::new()).await.map_err(|_| "unreachable")?;
    match flow {
        "oauth" => {
            if client_id.is_empty() {
                return Err("no_client_id");
            }
            client
                .oauth()
                .restore_session(
                    OAuthSession {
                        client_id: ClientId::new(client_id),
                        user: UserSession { meta, tokens },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .map_err(|_| "restore")?;
            client.oauth().logout().await.map_err(|_| "refused")
        }
        "sso" => {
            client
                .matrix_auth()
                .restore_session(
                    matrix_sdk::authentication::matrix::MatrixSession { meta, tokens },
                    RoomLoadSettings::default(),
                )
                .await
                .map_err(|_| "restore")?;
            client.matrix_auth().logout().await.map(|_| ()).map_err(|_| "refused")
        }
        _ => Err("unknown_flow"),
    }
}

/// Log out an OAuth session, revoking its tokens at the authorization
/// server. Password sessions use `mx_rust_logout`. Store deletion is left
/// to C++.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_logout(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };

        bridge.abandon_uia();
        bridge.stop_sync_and_wait();

        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(bridge.op_thread_name(), move || {
            let runtime_events = Arc::clone(&events);
            // Held and announced only after `run_async_on` has returned, i.e.
            // after this thread's Client is gone, as `mx_rust_logout` does:
            // C++ starts deleting the store when it sees `logged_out`.
            let outcome: Arc<Mutex<Option<serde_json::Value>>> = Arc::new(Mutex::new(None));
            let outcome_slot = Arc::clone(&outcome);
            run_async_on(shared_runtime, runtime_events, "oauth_logout", async move {
                let client = client_slot.lock().ok().and_then(|mut g| g.take());
                // Same shape as mx_rust_logout: C++ reads "result", so a failed revocation
                // must not default to "ok".
                let event = match client.as_ref() {
                    Some(client) => match client.oauth().logout().await {
                        Ok(()) => json!({ "type": "logged_out", "result": "ok" }),
                        // A revocation failure still ends the local session. The SDK detail is
                        // omitted since it may quote endpoint URLs.
                        Err(_) => json!({
                            "type": "logged_out",
                            "result": "failed",
                            "category": "revocation_failed",
                        }),
                    },
                    None => json!({ "type": "logged_out", "result": "no_session" }),
                };
                drop(client);
                if let Ok(mut slot) = outcome_slot.lock() {
                    *slot = Some(event);
                }
            });
            let event = outcome
                .lock()
                .ok()
                .and_then(|mut slot| slot.take())
                .unwrap_or_else(|| json!({
                    "type": "logged_out",
                    "result": "failed",
                    "category": "logout_task_failed",
                }));
            enqueue(&events, event);
        });

        Ok(String::new())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // ClientMetadata is serialize-only in matrix-sdk 0.18, so assert on the JSON
    // sent to the registration endpoint.
    fn metadata_json(redirect: &str) -> serde_json::Value {
        let raw = client_metadata(Url::parse(redirect).unwrap()).expect("metadata builds");
        serde_json::from_str(raw.json().get()).expect("metadata is JSON")
    }

    #[test]
    fn client_metadata_registers_only_the_loopback_redirect() {
        let redirect = "http://127.0.0.1:51234/callback";
        let json = metadata_json(redirect);

        // A native app: the server expects a loopback redirect.
        assert_eq!(json["application_type"], "native");
        // Exactly one redirect URI: this attempt's loopback endpoint.
        assert_eq!(json["redirect_uris"], serde_json::json!([redirect]));
        // The SDK adds `refresh_token` to the grants; without it no refresh token
        // is issued and expired access tokens cannot be renewed.
        assert_eq!(json["grant_types"],
                   serde_json::json!(["authorization_code", "refresh_token"]));
    }

    #[test]
    fn client_metadata_names_the_application() {
        let json = metadata_json("http://127.0.0.1:1/cb");
        assert_eq!(json["client_name"], "Lightning");
    }

    // The device-code grant (login_with_qr_code) is not used, so it must not be
    // requested.
    #[test]
    fn client_metadata_does_not_request_the_device_code_grant() {
        let json = metadata_json("http://127.0.0.1:1/cb");
        let grants = json["grant_types"].as_array().expect("grant_types is a list");
        assert!(!grants.iter().any(|g| g == "urn:ietf:params:oauth:grant-type:device_code"),
                "device_code grant must not be requested: {grants:?}");
    }

    // ---------------------------------------------------------------------
    // RFC 8252 §7.3: strict servers refuse to register a pinned loopback port.
    // ---------------------------------------------------------------------

    /// The C++ listener URI: loopback, an ephemeral port, and a per-attempt
    /// 128-bit nonce path.
    const LISTENER: &str = "http://127.0.0.1:51234/lightning-oauth/0a1b2c3d4e5f60718293a4b5c6d7e8f9";
    const LISTENER_PORTLESS: &str =
        "http://127.0.0.1/lightning-oauth/0a1b2c3d4e5f60718293a4b5c6d7e8f9";

    fn url(raw: &str) -> Url {
        Url::parse(raw).expect("test URL parses")
    }

    /// continuwuity's refusal, reduced to the wire: RFC 7591 §3.2.2's
    /// `invalid_client_metadata` code plus prose.
    fn metadata_refusal(description: &str) -> OAuthError {
        use matrix_sdk::authentication::oauth::error::StandardErrorResponse;
        OAuthError::ClientRegistration(OAuthClientRegistrationError::OAuth(
            RequestTokenError::ServerResponse(StandardErrorResponse::new(
                ClientRegistrationErrorResponseType::InvalidClientMetadata,
                Some(description.to_owned()),
                None,
            )),
        ))
    }

    fn continuwuity_refusal() -> OAuthError {
        metadata_refusal(
            "HTTP redirect URIs for native applications do not need to specify a port. \
             All ports will be accepted during authorization.",
        )
    }

    // The registered URI must carry no port.
    #[test]
    fn client_metadata_registers_a_portless_loopback_uri_verbatim() {
        let json = metadata_json(LISTENER_PORTLESS);
        assert_eq!(json["redirect_uris"], serde_json::json!([LISTENER_PORTLESS]));
        assert_eq!(json["application_type"], "native");
    }

    #[test]
    fn portless_loopback_redirect_strips_the_ephemeral_port_and_keeps_the_path() {
        let stripped = portless_loopback_redirect(&url(LISTENER)).expect("a port to strip");
        // The nonce path survives: widened to any port on loopback, this path.
        assert_eq!(stripped.as_str(), LISTENER_PORTLESS);
    }

    // The oauth_begin guard accepts `[::1]`, so this must handle it too.
    #[test]
    fn portless_loopback_redirect_handles_ipv6_loopback() {
        let stripped =
            portless_loopback_redirect(&url("http://[::1]:51234/cb/abc")).expect("a port to strip");
        assert_eq!(stripped.as_str(), "http://[::1]/cb/abc");
    }

    #[test]
    fn portless_loopback_redirect_handles_localhost() {
        let stripped =
            portless_loopback_redirect(&url("http://localhost:51234/cb")).expect("a port to strip");
        assert_eq!(stripped.as_str(), "http://localhost/cb");
    }

    // Nothing to strip: no retry (identical metadata would fail again).
    #[test]
    fn portless_loopback_redirect_declines_when_there_is_no_port() {
        assert!(portless_loopback_redirect(&url(LISTENER_PORTLESS)).is_none());
    }

    // Defence in depth: the retry must not register a non-loopback redirect.
    #[test]
    fn portless_loopback_redirect_refuses_a_non_loopback_host() {
        assert!(portless_loopback_redirect(&url("http://evil.example:8080/cb")).is_none());
        assert!(portless_loopback_redirect(&url("http://127.0.0.2:8080/cb")).is_none());
        assert!(portless_loopback_redirect(&url("http://127.0.0.1.evil.example:8080/cb")).is_none());
    }

    // The port rule applies to http loopback URIs only.
    #[test]
    fn portless_loopback_redirect_refuses_a_non_http_scheme() {
        assert!(portless_loopback_redirect(&url("https://127.0.0.1:8443/cb")).is_none());
        assert!(portless_loopback_redirect(&url("org.lightning:/cb")).is_none());
    }

    #[test]
    fn registration_refused_metadata_recognises_a_metadata_refusal() {
        // The code is reported, not just detected.
        assert_eq!(
            registration_refused_metadata(&continuwuity_refusal()),
            Some("invalid_client_metadata")
        );
        // The sibling RFC 7591 code for the same complaint.
        assert_eq!(
            registration_refused_metadata(&OAuthError::ClientRegistration(
                OAuthClientRegistrationError::OAuth(RequestTokenError::ServerResponse(
                    matrix_sdk::authentication::oauth::error::StandardErrorResponse::new(
                        ClientRegistrationErrorResponseType::InvalidRedirectUri,
                        None,
                        None,
                    ),
                )),
            )),
            Some("invalid_redirect_uri")
        );
    }

    // Retry only a refusal of what we sent: transport failures and missing
    // endpoints would fail again, and later-stage failures already registered.
    #[test]
    fn registration_refused_metadata_ignores_everything_else() {
        assert!(registration_refused_metadata(&OAuthError::ClientRegistration(
            OAuthClientRegistrationError::NotSupported
        ))
        .is_none());
        assert!(registration_refused_metadata(&OAuthError::ClientRegistration(
            OAuthClientRegistrationError::OAuth(RequestTokenError::Other(
                "connection reset".to_owned()
            )),
        ))
        .is_none());
        assert!(registration_refused_metadata(&OAuthError::NotRegistered).is_none());
    }

    // Register without the port, request with it; both come from this one
    // function (matrix-sdk 0.18 keeps them separate).
    #[test]
    fn portless_registration_retry_registers_without_the_port_and_requests_with_it() {
        let listener = url(LISTENER);
        let (metadata, request_redirect, reason) =
            portless_registration_retry(&listener, &continuwuity_refusal())
                .expect("a metadata refusal on a ported loopback URI retries");
        assert_eq!(reason, "invalid_client_metadata");

        let json: serde_json::Value =
            serde_json::from_str(metadata.json().get()).expect("metadata is JSON");
        assert_eq!(json["redirect_uris"], serde_json::json!([LISTENER_PORTLESS]));
        assert_eq!(json["application_type"], "native");

        // The request keeps the live port: the browser URI, the listener's port,
        // and what finish_login() replays in the token exchange.
        assert_eq!(request_redirect.as_str(), LISTENER);
        assert_eq!(request_redirect.port(), Some(51234));
        // And the input is untouched.
        assert_eq!(listener.as_str(), LISTENER);
    }

    #[test]
    fn portless_registration_retry_handles_an_ipv6_listener() {
        let listener = url("http://[::1]:51234/cb/abc");
        let (metadata, request_redirect, _reason) =
            portless_registration_retry(&listener, &continuwuity_refusal()).expect("retries");
        let json: serde_json::Value =
            serde_json::from_str(metadata.json().get()).expect("metadata is JSON");
        assert_eq!(json["redirect_uris"], serde_json::json!(["http://[::1]/cb/abc"]));
        assert_eq!(request_redirect.as_str(), "http://[::1]:51234/cb/abc");
    }

    #[test]
    fn portless_registration_retry_declines_an_unrelated_failure() {
        assert!(portless_registration_retry(&url(LISTENER), &OAuthError::NotRegistered).is_none());
        assert!(portless_registration_retry(
            &url(LISTENER),
            &OAuthError::ClientRegistration(OAuthClientRegistrationError::NotSupported),
        )
        .is_none());
    }

    // A metadata refusal must not lead to registering an off-machine redirect.
    #[test]
    fn portless_registration_retry_declines_a_non_loopback_redirect() {
        assert!(portless_registration_retry(
            &url("http://evil.example:8080/cb"),
            &continuwuity_refusal(),
        )
        .is_none());
    }

    // A refusal with no port to strip must not loop.
    #[test]
    fn portless_registration_retry_declines_when_the_uri_is_already_portless() {
        assert!(
            portless_registration_retry(&url(LISTENER_PORTLESS), &continuwuity_refusal()).is_none()
        );
    }

    // ---------------------------------------------------------------------
    // Failure reasons: what the log may say about a failed browser sign-in.
    // Before these, a failed code exchange left no trace anywhere.
    // ---------------------------------------------------------------------

    fn token_refusal(code: BasicErrorResponseType, description: &str) -> OAuthError {
        use matrix_sdk::authentication::oauth::error::{
            OAuthAuthorizationCodeError, StandardErrorResponse,
        };
        OAuthError::AuthorizationCode(OAuthAuthorizationCodeError::RequestToken(
            RequestTokenError::ServerResponse(StandardErrorResponse::new(
                code,
                Some(description.to_owned()),
                None,
            )),
        ))
    }

    #[test]
    fn a_state_mismatch_is_named_as_one() {
        use matrix_sdk::authentication::oauth::error::OAuthAuthorizationCodeError;
        let err = OAuthError::AuthorizationCode(OAuthAuthorizationCodeError::InvalidState);
        assert_eq!(oauth_failure_reason(&err), "state_mismatch");
    }

    // The protocol code is the whole report: the server's prose can quote
    // the code or the redirect URI.
    #[test]
    fn a_refused_code_exchange_names_the_protocol_code_and_nothing_else() {
        let err = token_refusal(
            BasicErrorResponseType::InvalidGrant,
            "code abc123 for http://127.0.0.1:1/callback/x was already used",
        );
        assert_eq!(oauth_failure_reason(&err), "token_invalid_grant");
        // The SDK's own wrapper reaches the same answer.
        assert_eq!(
            sdk_failure_reason(&matrix_sdk::Error::OAuth(Box::new(err))),
            "token_invalid_grant"
        );
    }

    #[test]
    fn a_server_chosen_code_is_logged_only_when_token_shaped() {
        let extension =
            |code: &str| token_refusal(BasicErrorResponseType::Extension(code.to_owned()), "");
        assert_eq!(
            oauth_failure_reason(&extension("consent_required")),
            "token_consent_required"
        );
        assert_eq!(
            oauth_failure_reason(&extension("bad\ncode https://x/?code=1")),
            "token_unrecognised"
        );
        assert_eq!(oauth_failure_reason(&extension(&"a".repeat(49))), "token_unrecognised");
    }

    #[test]
    fn a_refused_or_missing_registration_is_named() {
        assert_eq!(
            oauth_failure_reason(&continuwuity_refusal()),
            "registration_invalid_client_metadata"
        );
        assert_eq!(
            oauth_failure_reason(&OAuthError::ClientRegistration(
                OAuthClientRegistrationError::NotSupported
            )),
            "registration_not_supported"
        );
        assert_eq!(
            oauth_failure_reason(&OAuthError::Discovery(OAuthDiscoveryError::NotSupported)),
            "discovery_not_supported"
        );
        assert_eq!(oauth_failure_reason(&OAuthError::NotRegistered), "not_registered");
    }

    #[test]
    fn wire_token_passes_codes_and_refuses_everything_else() {
        assert_eq!(wire_token("M_FORBIDDEN"), "M_FORBIDDEN");
        assert_eq!(wire_token("invalid_grant"), "invalid_grant");
        assert_eq!(wire_token(""), "unrecognised");
        assert_eq!(wire_token("M_FORBIDDEN token=syl_abc"), "unrecognised");
        assert_eq!(wire_token(&"A".repeat(48)), "A".repeat(48));
        assert_eq!(wire_token(&"A".repeat(49)), "unrecognised");
    }
}

/// Session management for MAS/OAuth accounts, which have no password UIA:
/// device sign-out happens in the account console. Answers with the console
/// URL (empty `device_id`: the sessions list; otherwise that device's delete
/// page). Result event: `oauth_management_url { op_id, ok, url }`. Not
/// logged.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_management_url(
    ptr: *mut c_void,
    device_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        let device = unsafe { cstr_arg(device_id) }?.trim().to_owned();
        let Some(client) = handle.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("no active Matrix session".to_owned());
        };
        let events = Arc::clone(&handle.events);
        let timelines = Arc::clone(&handle.timelines);
        let lifecycle = timelines.lifecycle();
        handle.spawn_room_action(async move {
            use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::{
                AccountManagementActionData, DeviceDeleteData,
            };
            let result = client.oauth().server_metadata().await;
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let url = match result {
                Ok(metadata) => {
                    if device.is_empty() {
                        metadata.account_management_url_with_action(
                            AccountManagementActionData::DevicesList,
                        )
                    } else {
                        let owned = OwnedDeviceId::from(device.as_str());
                        metadata.account_management_url_with_action(
                            AccountManagementActionData::DeviceDelete(
                                DeviceDeleteData::new(&owned),
                            ),
                        )
                    }
                }
                Err(_) => None,
            };
            enqueue(&events, json!({
                "type": "oauth_management_url",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "ok": url.is_some(),
                "url": url.map(|u| u.to_string()).unwrap_or_default(),
            }));
        });
        Ok(String::new())
    })
}

#[cfg(test)]
mod token_persistence_tests {
    use super::{
        restore_oauth_client_then, revoke_refused_sign_in, spawn_token_persistence,
        spawn_token_persistence_custody, spawn_token_persistence_with,
    };
    use crate::modern_sync_auth_tests::respond;
    use std::io::Read;
    use std::collections::VecDeque;
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::authentication::oauth::{ClientId, OAuthSession, UserSession};
    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };

    /// The request line and headers; any body is read and dropped.
    fn read_head(stream: &mut std::net::TcpStream) -> String {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        let head_end = loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                return String::new();
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
        head
    }

    fn head_has_bearer(head: &str, token: &str) -> bool {
        head.lines().any(|line| {
            line.to_ascii_lowercase().starts_with("authorization:")
                && line.trim_end().ends_with(&format!("Bearer {token}"))
        })
    }

    #[derive(Default)]
    struct Seen {
        issued: AtomicUsize,
        revocations: AtomicUsize,
        logouts: AtomicUsize,
    }

    /// A homeserver with its own authorization server: `/refresh` (password
    /// sessions) and the OAuth token endpoint hand out `access-N` /
    /// `refresh-N`; revocation and `/logout` are counted; everything else is
    /// an empty success (the E2EE initialization task's requests).
    fn serve() -> (String, Arc<Seen>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let base = format!("http://{}", listener.local_addr().expect("addr"));
        let seen = Arc::new(Seen::default());
        let metadata = format!(
            r#"{{"issuer":"{base}/","authorization_endpoint":"{base}/oauth2/authorize","token_endpoint":"{base}/oauth2/token","registration_endpoint":"{base}/oauth2/registration","revocation_endpoint":"{base}/oauth2/revoke","response_types_supported":["code"],"response_modes_supported":["query","fragment"],"grant_types_supported":["authorization_code","refresh_token"],"code_challenge_methods_supported":["S256"]}}"#
        );
        let served = Arc::clone(&seen);
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&served);
                let metadata = metadata.clone();
                thread::spawn(move || {
                    let line_and_head = read_head(&mut stream);
                    let path = line_and_head
                        .lines()
                        .next()
                        .and_then(|line| line.split_whitespace().nth(1))
                        .unwrap_or("")
                        .to_owned();
                    let pair = |seen: &Seen| {
                        let n = seen.issued.fetch_add(1, Ordering::SeqCst) + 1;
                        (format!("access-{n}"), format!("refresh-{n}"))
                    };
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else if path.starts_with("/.well-known") {
                        respond(&mut stream, "404 Not Found",
                                r#"{"errcode":"M_NOT_FOUND","error":"none"}"#);
                    } else if path.contains("auth_metadata") {
                        respond(&mut stream, "200 OK", &metadata);
                    } else if path.starts_with("/_matrix/client/v3/refresh") {
                        let (access, refresh) = pair(&seen);
                        respond(&mut stream, "200 OK", &format!(
                            r#"{{"access_token":"{access}","refresh_token":"{refresh}","expires_in_ms":300000}}"#));
                    } else if path.starts_with("/oauth2/token") {
                        let (access, refresh) = pair(&seen);
                        respond(&mut stream, "200 OK", &format!(
                            r#"{{"access_token":"{access}","token_type":"Bearer","expires_in":300,"refresh_token":"{refresh}"}}"#));
                    } else if path.starts_with("/oauth2/revoke") {
                        seen.revocations.fetch_add(1, Ordering::SeqCst);
                        respond(&mut stream, "200 OK", "{}");
                    } else if path.starts_with("/_matrix/client/v3/account/whoami") {
                        if head_has_bearer(&line_and_head, "good") {
                            respond(&mut stream, "200 OK",
                                    r#"{"user_id":"@me:example.org","device_id":"THEIRDEVICE"}"#);
                        } else {
                            respond(&mut stream, "401 Unauthorized",
                                    r#"{"errcode":"M_UNKNOWN_TOKEN","error":"expired","soft_logout":false}"#);
                        }
                    } else if path.starts_with("/_matrix/client/v3/logout") {
                        seen.logouts.fetch_add(1, Ordering::SeqCst);
                        respond(&mut stream, "200 OK", "{}");
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        (base, seen)
    }

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_multi_thread().enable_all().build().expect("runtime")
    }

    fn store_dir(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "lightning-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos())
                .unwrap_or_default()
        ))
    }

    fn meta() -> SessionMeta {
        SessionMeta {
            user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
            device_id: OwnedDeviceId::from("TESTDEVICE"),
        }
    }

    fn tokens() -> SessionTokens {
        SessionTokens {
            access_token: "access-0".to_owned(),
            refresh_token: Some("refresh-0".to_owned()),
        }
    }

    /// Labs custody (design §3.2.7): the token task records a rotation and a
    /// revocation in the handle's facts, and its queue output is unchanged.
    #[test]
    fn the_token_task_records_rotation_and_revocation_in_custody() {
        let (homeserver, _) = serve();
        runtime().block_on(async {
            // A rotation.
            let client = matrix_sdk::Client::builder()
                .homeserver_url(&homeserver)
                .handle_refresh_tokens()
                .build()
                .await
                .expect("client");
            let changes = client.subscribe_to_session_changes();
            client
                .matrix_auth()
                .restore_session(MatrixSession { meta: meta(), tokens: tokens() },
                                 RoomLoadSettings::default())
                .await
                .expect("restore");
            let custody = Arc::new(crate::labs::CustodyFacts::default());
            let events = Arc::new(Mutex::new(VecDeque::new()));
            let watcher = spawn_token_persistence_custody(
                &client, changes, Arc::clone(&events), None, Arc::clone(&custody));
            client.refresh_access_token().await.expect("refresh");
            let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
            while custody.generation() == 0 && tokio::time::Instant::now() < deadline {
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            watcher.abort();
            assert!(custody.generation() >= 1, "a rotation did not bump the generation");
            assert!(!custody.revoked());
            assert_eq!(refreshed(&events).len(), 1, "the queue output must be unchanged");

            // A revocation: no refresh token, and the server refuses the token.
            let client = matrix_sdk::Client::builder()
                .homeserver_url(&homeserver)
                .build()
                .await
                .expect("client");
            let changes = client.subscribe_to_session_changes();
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: meta(),
                        tokens: SessionTokens { access_token: "bad".to_owned(), refresh_token: None },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");
            let custody = Arc::new(crate::labs::CustodyFacts::default());
            let events = Arc::new(Mutex::new(VecDeque::new()));
            let watcher = spawn_token_persistence_custody(
                &client, changes, Arc::clone(&events), None, Arc::clone(&custody));
            let _ = client.whoami().await;
            let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
            while !custody.revoked() && tokio::time::Instant::now() < deadline {
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            watcher.abort();
            assert!(custody.revoked(), "UnknownToken did not set the revoked flag");
            let facts = custody.custody_json(|| None);
            assert_eq!(facts["revoked_kind"], "unknown_token");
            assert_eq!(facts["soft_logout"], false);
            assert_eq!(facts["had_refresh_token"], false);
            let revoked: Vec<serde_json::Value> = events
                .lock()
                .unwrap()
                .iter()
                .map(|e| serde_json::from_str::<serde_json::Value>(e).unwrap())
                .filter(|e| e["type"] == "session_token_revoked")
                .collect();
            assert_eq!(revoked.len(), 1, "the queue output must be unchanged");
        });
    }

    fn refreshed(events: &Arc<Mutex<VecDeque<String>>>) -> Vec<serde_json::Value> {
        events
            .lock()
            .unwrap()
            .iter()
            .map(|e| serde_json::from_str::<serde_json::Value>(e).unwrap())
            .filter(|e| e["type"] == "session_tokens_refreshed")
            .collect()
    }

    /// Watches `changes` and returns what it announced within a few seconds.
    async fn announced(
        client: &matrix_sdk::Client,
        changes: tokio::sync::broadcast::Receiver<matrix_sdk::SessionChange>,
    ) -> Vec<serde_json::Value> {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        let watcher = spawn_token_persistence_with(client, changes, Arc::clone(&events), None);
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        while refreshed(&events).is_empty() && tokio::time::Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        // Room for a second announcement the restore's own task may make.
        tokio::time::sleep(Duration::from_millis(200)).await;
        watcher.abort();
        refreshed(&events)
    }

    /// PROPERTY test of matrix-sdk's broadcast, which the restore paths rely
    /// on: a receiver sees only what is sent after it exists. It tests no
    /// Lightning code path; the two restore tests below do.
    #[test]
    fn property_a_session_change_reaches_only_receivers_that_existed_before_it() {
        let (homeserver, _) = serve();
        runtime().block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(&homeserver)
                .handle_refresh_tokens()
                .build()
                .await
                .expect("client");
            let early = client.subscribe_to_session_changes();
            client
                .matrix_auth()
                .restore_session(MatrixSession { meta: meta(), tokens: tokens() },
                                 RoomLoadSettings::default())
                .await
                .expect("restore");
            client.refresh_access_token().await.expect("refresh");

            let late_events = Arc::new(Mutex::new(VecDeque::new()));
            let late = spawn_token_persistence(&client, Arc::clone(&late_events), None);
            let early_seen = announced(&client, early).await;
            late.abort();
            assert_eq!(early_seen.len(), 1, "the early receiver missed the rotation");
            assert!(
                refreshed(&late_events).is_empty(),
                "a receiver taken after the rotation saw it; the SDK now replays \
                 session changes and the restore ordering no longer matters"
            );
        });
    }

    /// The password and SSO restore (`mx_rust_restore`): a rotation made by
    /// whatever `restore_session()` set running is announced on the receiver
    /// the restore hands back. The hook rotates at exactly that point; moving
    /// the subscription below `restore_session()` fails this.
    #[test]
    fn a_rotation_during_the_password_restore_is_announced() {
        let (homeserver, _) = serve();
        let store = store_dir("restore-order-password");
        runtime().block_on(async {
            let events = Arc::new(Mutex::new(VecDeque::new()));
            let (client, changes) = crate::restore_client_with_session_then(
                &homeserver,
                &store,
                MatrixSession { meta: meta(), tokens: tokens() },
                &events,
                None,
                |client| async move {
                    client.refresh_access_token().await.expect("refresh");
                },
            )
            .await
            .expect("restore");
            let rotated = client.session_tokens().and_then(|t| t.refresh_token);
            let seen = announced(&client, changes).await;
            assert!(!seen.is_empty(), "the rotation during the restore was never announced");
            assert_eq!(seen.last().unwrap()["refresh_token"].as_str(), rotated.as_deref());
        });
        let _ = std::fs::remove_dir_all(&store);
    }

    /// The OAuth restore (`mx_rust_oauth_restore`), the matrix.org case: same
    /// property through `restore_oauth_client_then`, refreshing at the OAuth
    /// token endpoint.
    #[test]
    fn a_rotation_during_the_oauth_restore_is_announced() {
        let (homeserver, seen) = serve();
        let store = store_dir("restore-order-oauth");
        runtime().block_on(async {
            let events = Arc::new(Mutex::new(VecDeque::new()));
            let session = OAuthSession {
                client_id: ClientId::new("client".to_owned()),
                user: UserSession { meta: meta(), tokens: tokens() },
            };
            let (client, changes) = restore_oauth_client_then(
                &homeserver,
                &store,
                session,
                &events,
                None,
                |client| async move {
                    client.refresh_access_token().await.expect("oauth refresh");
                },
            )
            .await
            .expect("restore");
            assert!(seen.issued.load(Ordering::SeqCst) >= 1);
            let rotated = client.session_tokens().and_then(|t| t.refresh_token);
            let announced = announced(&client, changes).await;
            assert!(!announced.is_empty(),
                    "the rotation during the OAuth restore was never announced");
            assert_eq!(announced.last().unwrap()["refresh_token"].as_str(),
                       rotated.as_deref());
        });
        let _ = std::fs::remove_dir_all(&store);
    }

    /// The adoption check's three answers, from a plain whoami with no client
    /// session.
    #[test]
    fn the_adoption_check_names_the_device_or_says_it_cannot_tell() {
        let (homeserver, _) = serve();
        runtime().block_on(async {
            assert_eq!(super::whoami_probe(&homeserver, "good").await,
                       ("device", Some("THEIRDEVICE".to_owned())));
            assert_eq!(super::whoami_probe(&homeserver, "expired").await,
                       ("unauthorized", None));
            assert_eq!(super::whoami_probe("http://127.0.0.1:9", "good").await.0,
                       "unreachable");
        });
    }

    /// A browser sign-in Phase B refused is ended at the server: an SSO
    /// session logs out; OAuth tokens are revoked at the authorization server
    /// (only up to the request here, see below).
    #[test]
    fn a_refused_browser_sign_in_is_ended_at_the_server() {
        let (homeserver, seen) = serve();
        runtime().block_on(async {
            // OAuth gets as far as the revocation request and no further over
            // plain http: the oauth2 crate refuses a revocation endpoint that
            // is not https (a real authorization server's always is), so on
            // this loopback fixture it reports "refused" before sending.
            assert_eq!(
                revoke_refused_sign_in(&homeserver, "oauth", "client".to_owned(),
                                       "@me:example.org", "NEWDEVICE".to_owned(),
                                       "access-x".to_owned(), "refresh-x".to_owned())
                    .await,
                Err("refused"));
            assert_eq!(seen.revocations.load(Ordering::SeqCst), 0);

            revoke_refused_sign_in(&homeserver, "sso", String::new(),
                                   "@me:example.org", "NEWDEVICE".to_owned(),
                                   "access-y".to_owned(), String::new())
                .await
                .expect("sso logout");
            assert_eq!(seen.logouts.load(Ordering::SeqCst), 1);

            // Nothing to end, and nothing that could be: a word, no request.
            assert_eq!(
                revoke_refused_sign_in(&homeserver, "oauth", String::new(),
                                       "@me:example.org", "D".to_owned(),
                                       String::new(), String::new()).await,
                Err("no_session"));
        });
    }
}
