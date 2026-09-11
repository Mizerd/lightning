//! OAuth 2.0 / OIDC authentication for Lightning, on matrix-sdk 0.18's
//! `Client::oauth()` API.
//!
//! # Why this module exists separately
//!
//! Every OAuth protocol primitive is SDK-owned. This module does NOT implement
//! PKCE, CSRF state generation/validation, the token exchange, or refresh:
//! `OAuth::login()` builds the authorization URL with a PKCE challenge and a
//! `state`, `OAuth::finish_login()` parses the redirect, validates the state
//! against the stored `AuthorizationValidationData` and performs the code
//! exchange, and the SDK refreshes access tokens internally. Lightning
//! contributes exactly two things the SDK cannot do for a desktop app: opening
//! the system browser, and receiving the loopback redirect (matrix-sdk's own
//! `local-server` helper is behind the `sso-login`/`local-server` features,
//! which pull in `axum` — not vendored in this offline, `--locked` build).
//!
//! # The two-phase store lifecycle
//!
//! Password login knows the account before it contacts the server, so
//! `RustSdkMatrixClient::login()` can open the account's sqlite store first.
//! OAuth cannot: the Matrix user ID is only known after `finish_login()` runs
//! `whoami`. Opening a persistent store before that would mean guessing which
//! account's crypto store to attach a not-yet-identified device to — precisely
//! the store/device ownership bug class Lightning already guards against for
//! password login (`RustSessionPolicy::passwordLoginBlockReason`).
//!
//! So authentication runs in two phases:
//!
//!   Phase A (this module, `mx_rust_oauth_bootstrap_create`):
//!     A bootstrap client with an **in-memory store only** — the builder's
//!     default when `.sqlite_store()` is never called. Discovery, dynamic
//!     client registration, the authorization URL and the code exchange all
//!     happen here. Nothing is written to disk, so there is no persistent
//!     account store to collide, orphan or clean up. **This client must never
//!     sync**: a sync would upload device keys generated in the throwaway
//!     in-memory crypto store, and Phase B would then upload a second,
//!     different set of keys for the same device ID. Nothing in this module
//!     starts a sync loop.
//!
//!   Phase B (C++ `RustSdkMatrixClient`, using `mx_rust_oauth_restore`):
//!     Only once the canonical user ID and device ID are known does C++ derive
//!     the normal `AccountIdentity`/store slug, apply the store-ownership
//!     policy, create the real account-scoped handle, and restore the OAuth
//!     session into the account's own sqlite store. Sync and E2EE start after
//!     that, exactly as they do for a restored password session.
//!
//! # Secrets
//!
//! Access tokens, refresh tokens and the dynamic-registration client ID cross
//! this boundary once, into the C++ SecretStore. They are never logged here,
//! never placed in an error string, and never reach QML. The authorization
//! callback URL carries a `code` and is likewise never logged.

use std::collections::VecDeque;
use std::ffi::{c_char, c_void};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use matrix_sdk::authentication::oauth::error::{
    ClientRegistrationErrorResponseType, OAuthClientRegistrationError, OAuthError,
    RequestTokenError,
};
use matrix_sdk::authentication::oauth::registration::{
    ApplicationType, ClientMetadata, Localized, OAuthGrantType,
};
use matrix_sdk::authentication::oauth::{ClientId, OAuthSession, UserSession};
use matrix_sdk::authentication::SessionTokens;
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

/// Client URI advertised during dynamic client registration. Identifies
/// Lightning to the authorization server's consent screen. Not a secret.
const CLIENT_URI: &str = "https://gitlab.smetonis.net/Mizerd/lightning";

/// Build Lightning's OAuth client metadata for dynamic client registration.
///
/// `redirect_uri` is registered as the sole redirect URI, so an authorization
/// response aimed anywhere else is rejected by the authorization server rather
/// than by us. It is normally the exact loopback URI the C++ listener is bound
/// to; on a server that enforces RFC 8252 §7.3 strictly it is that URI with
/// its ephemeral port removed — see `portless_registration_retry`.
fn client_metadata(redirect_uri: Url) -> Result<Raw<ClientMetadata>, String> {
    let client_uri = Url::parse(CLIENT_URI)
        .map_err(|err| format!("invalid Lightning client URI: {err}"))?;

    let mut metadata = ClientMetadata::new(
        // A desktop application, not a web client: this is what tells the
        // authorization server to expect a loopback redirect rather than a
        // hosted callback endpoint.
        ApplicationType::Native,
        vec![OAuthGrantType::AuthorizationCode { redirect_uris: vec![redirect_uri] }],
        Localized::new(client_uri, []),
    );
    metadata.client_name = Some(Localized::new("Lightning".to_owned(), []));

    Raw::new(&metadata)
        .map_err(|err| format!("failed to serialize OAuth client metadata: {err}"))
}

/// The registration-time form of a loopback redirect URI: the same URI with
/// its ephemeral port removed.
///
/// RFC 8252 §7.3 requires an authorization server to accept ANY port at
/// request time for a loopback redirect URI, precisely because a native client
/// takes the port from the operating system when the attempt starts. Some
/// servers enforce the corollary: registering a PINNED port is invalid client
/// metadata, because it claims a constraint the server will not honour.
/// Continuwuity refuses registration outright with `invalid_client_metadata`
/// ("HTTP redirect URIs for native applications do not need to specify a
/// port"), which blocks OAuth sign-in on those homeservers entirely.
///
/// Returns `None` when there is nothing to do or nothing safe to do: a URI
/// with no explicit port, a non-`http` scheme, or a host that is not loopback.
/// The loopback list mirrors `mx_rust_oauth_begin`'s own guard — `host_str()`
/// returns an IPv6 literal in its bracketed form, so `[::1]` is the spelling
/// that actually occurs.
///
/// Only the PORT is removed. The path still carries the per-attempt 128-bit
/// nonce the C++ listener generates, so the widening is bounded to "any port
/// on loopback, this exact path" rather than "any loopback URI".
fn portless_loopback_redirect(redirect: &Url) -> Option<Url> {
    if redirect.scheme() != "http" {
        return None;
    }
    match redirect.host_str() {
        Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => {}
        _ => return None,
    }
    // `Url::port()` is None both when no port was given and when it equals the
    // scheme default, and either way there is nothing to strip.
    redirect.port()?;
    let mut portless = redirect.clone();
    portless.set_port(None).ok()?;
    Some(portless)
}

/// Whether an `OAuth::login().build()` failure is the authorization server
/// REFUSING our client metadata, as opposed to a network error, a missing
/// registration endpoint, or a failure later in the flow.
///
/// Matched structurally on RFC 7591 §3.2.2's error code rather than on the
/// server's prose: the wording is implementation-specific and localizable,
/// the code is not.
/// Returns WHICH code was matched, not merely that one was: the retry reports
/// it, and a report that conflates the two tells the reader less than it
/// appears to. Raised in review.
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

/// Decide how to retry a registration the authorization server refused.
///
/// Returns the metadata to register (the loopback redirect URI WITHOUT its
/// port) paired with the redirect URI to put in the authorization REQUEST
/// (the live listener URI, port and all) — which is exactly the split RFC 8252
/// §7.3 contemplates, and which matrix-sdk 0.18 permits: `OAuth::login()`
/// takes the request URI as its own argument and never derives it from the
/// registration metadata, and `finish_login()` sends that same request URI in
/// the token exchange (RFC 6749 §4.1.3), so the two stay consistent.
///
/// `None` means "do not retry": the failure was not a metadata refusal, or
/// there is no port to remove, or the URI is not a loopback `http` URI.
///
/// Retrying rather than registering portless unconditionally is deliberate.
/// MAS accepts the pinned port today and is the only configuration this flow
/// has ever been live-validated against; changing what every sign-in sends it
/// would risk a regression nothing here can catch. This path is reached only
/// when a server has already said no, so a server that works today is
/// untouched — and the portless form is what BOTH implementations then match
/// against: continuwuity strips the port from the authorization request before
/// comparing it to the registered set (gated on `application_type: native`),
/// and MAS does the same in `Client::resolve_redirect_uri`.
fn portless_registration_retry(
    redirect: &Url,
    err: &OAuthError,
) -> Option<(Raw<ClientMetadata>, Url, &'static str)> {
    let reason = registration_refused_metadata(err)?;
    let portless = portless_loopback_redirect(redirect)?;
    let metadata = client_metadata(portless).ok()?;
    Some((metadata, redirect.clone(), reason))
}

/// Persist rotated session tokens for the lifetime of this client.
///
/// `ClientBuilder::handle_refresh_tokens()` makes the SDK renew an expired
/// access token automatically, but the SDK does not persist the result — the
/// application must. Without this, a refresh rotates the tokens in memory
/// only, the store keeps the CONSUMED refresh token, and the next start
/// presents it; an OAuth 2.1 / MAS authorization server treats a replayed
/// refresh token as compromise and can revoke the whole token family.
///
/// Applies to password sessions too: servers that issue refreshable password
/// sessions rotate them the same way.
///
/// The emitted event carries CREDENTIALS. C++ writes them straight to the
/// SecretStore; nothing logs them.
/// The returned handle MUST be stored in `RustClient::token_task` so shutdown
/// can abort it. The task holds a strong `Client`, and the broadcast sender it
/// waits on lives inside that same `Client`, so `recv()` never returns
/// `Closed` by itself — an unowned task here would keep the account's crypto
/// store open past `mx_rust_destroy`, and sign-out deletes that store.
#[must_use]
pub(crate) fn spawn_token_persistence(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
) -> tokio::task::JoinHandle<()> {
    let mut changes = client.subscribe_to_session_changes();
    let client = client.clone();
    tokio::spawn(async move {
        loop {
            match changes.recv().await {
                Ok(SessionChange::TokensRefreshed) => {
                    let Some(tokens) = client.session_tokens() else { continue };
                    enqueue(
                        &events,
                        json!({
                            "type": "session_tokens_refreshed",
                            "access_token": tokens.access_token,
                            "refresh_token": tokens.refresh_token,
                        }),
                    );
                }
                // The server rejected the token and the SDK could not renew
                // it. Report it as the existing revoked-credential state
                // rather than letting sync fail in a loop.
                Ok(SessionChange::UnknownToken(_)) => {
                    enqueue(
                        &events,
                        json!({
                            "type": "session_token_revoked",
                        }),
                    );
                }
                // Lagged just means we missed intermediate notifications; the
                // next one still carries the current tokens.
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                Err(_) => break,
            }
        }
    })
}

/// Phase A bootstrap handle: a `RustClient` whose store path is empty.
///
/// `build_client()` skips `.sqlite_store()` for an empty path, leaving the
/// SDK's in-memory default. Destroy it with the ordinary `mx_rust_destroy`
/// once the session has been handed to the real account handle.
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

/// Discover which authentication methods a homeserver actually offers.
///
/// Enqueues one `auth_discovery` event:
///
/// ```json
/// { "type": "auth_discovery", "homeserver": "...", "oauth": true,
///   "password": true, "sso": false, "error": null }
/// ```
///
/// `oauth` is true when the server publishes OAuth 2.0 authorization server
/// metadata. `password`/`sso` come from the legacy `/login` flow list. Nothing
/// is hard-coded per provider — the server's own answer decides, and a server
/// that advertises neither yields all-false rather than a guess.
///
/// `sso` is reported for honesty in the UI ("this server offers SSO, Lightning
/// cannot use it") and is never presented as a usable method: the SDK's SSO
/// login helper needs the `sso-login`/`local-server` features, whose `axum`
/// dependency is not vendored in this build.
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
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_discover", async move {
                // In-memory client: discovery must not create a store either.
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

                // OAuth support: the server either publishes authorization
                // server metadata or it does not. An error here is a normal
                // "this server is not an OAuth server" answer, not a failure.
                let oauth_supported = client.oauth().server_metadata().await.is_ok();

                // Legacy flows. A failure to read them is not fatal — an OAuth
                // -only server may not answer /login at all.
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
                        "error": serde_json::Value::Null,
                    }),
                );
            });
        });

        Ok(String::new())
    })
}

/// Begin an OAuth login: register the client if needed and build the
/// authorization URL.
///
/// Enqueues `{"type": "oauth_url", "url": "..."}` on success, or
/// `{"type": "oauth_failed", "message": "..."}`. The bootstrap `Client` is
/// parked in the bridge's client slot because the SDK stores this attempt's
/// PKCE verifier and CSRF state inside it — `finish_login()` must run on the
/// same instance.
///
/// The URL is opened in the system browser by C++. It contains no credentials:
/// it is the authorization endpoint plus this attempt's public parameters.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_begin(
    ptr: *mut c_void,
    homeserver: *const c_char,
    redirect_uri: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let redirect_uri = unsafe { cstr_arg(redirect_uri) }?;

        let redirect = Url::parse(&redirect_uri)
            .map_err(|err| format!("invalid OAuth redirect URI: {err}"))?;
        // Defence in depth: the listener binds loopback, and we refuse to ask
        // an authorization server to redirect anywhere else even if a caller
        // passes something odd.
        match redirect.host_str() {
            Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => {}
            _ => return Err("OAuth redirect URI must be a loopback address.".to_owned()),
        }

        let metadata = client_metadata(redirect.clone())?;

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_begin", async move {
                let client = match build_client(&homeserver, &PathBuf::new()).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(&events, json!({ "type": "oauth_failed", "message": err }));
                        return;
                    }
                };

                // device_id None: the SDK generates one and encodes it in the
                // requested scope. It comes back from finish_login() and is
                // the device the account store in Phase B must belong to.
                let mut built = client
                    .oauth()
                    .login(redirect.clone(), None, Some(metadata.into()), None)
                    .build()
                    .await;

                // A server that enforces RFC 8252 §7.3 strictly refuses to
                // register a loopback redirect URI that pins a port. Retry
                // ONCE with the port removed from the METADATA only; the
                // authorization request keeps the live listener port. Safe to
                // reuse this client: a failed registration leaves the SDK's
                // client_id unset, so the retry re-registers rather than
                // panicking on already-set authentication data.
                let retry = built
                    .as_ref()
                    .err()
                    .and_then(|err| portless_registration_retry(&redirect, err));
                if let Some((retry_metadata, request_redirect, reason)) = retry {
                    // SAY THAT IT HAPPENED. This ships to users on servers
                    // nobody here can reproduce, and without a line the three
                    // outcomes — the retry never fired, it fired and worked,
                    // it fired and was refused again — are indistinguishable
                    // in a report. Carries no URI, no port, no nonce and no
                    // token: the fact alone is the whole diagnostic.
                    enqueue(&events, json!({
                        "type": "oauth_registration_retry",
                        "reason": reason,
                    }));
                    built = client
                        .oauth()
                        .login(request_redirect, None, Some(retry_metadata.into()), None)
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

/// Complete an OAuth login from the callback the loopback listener received.
///
/// `callback` is the full redirect URI (or just its query string). It carries
/// the authorization `code` and `state`, so it is never logged, and it is
/// never echoed back into an error message.
///
/// The SDK validates `state` against this attempt's stored validation data and
/// performs the PKCE code exchange. On success this enqueues `oauth_ok` with
/// the identity and session material C++ needs to open the real account store:
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
        // Parsed here, synchronously, so a malformed redirect fails fast. The
        // error deliberately does NOT quote the input: it carries the
        // authorization code.
        let callback_url = Url::parse(&callback)
            .map_err(|_| "malformed OAuth callback".to_owned())?;

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_finish", async move {
                let client = match client_slot.lock().ok().and_then(|mut g| g.take()) {
                    Some(client) => client,
                    None => {
                        enqueue(
                            &events,
                            json!({
                                "type": "oauth_failed",
                                "message": "No OAuth sign-in is in progress.",
                            }),
                        );
                        return;
                    }
                };

                // The SDK parses the redirect, checks `state` against the
                // validation data stored by build(), and exchanges the code
                // with the PKCE verifier. A state mismatch, a denied
                // authorization or a replayed callback all fail here.
                if let Err(err) = client.oauth().finish_login(callback_url.into()).await {
                    // The error may quote the callback; format_matrix_error
                    // is not applied to it for that reason. Report a fixed,
                    // safe message and keep the detail out of the event.
                    let _ = err;
                    drop(client);
                    if let Ok(mut guard) = state_slot.lock() {
                        *guard = None;
                    }
                    enqueue(
                        &events,
                        json!({
                            "type": "oauth_failed",
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

                // Phase A is over. Drop the bootstrap client so its in-memory
                // store — and the tokens held in it — go away; Phase B builds
                // the real account-scoped client from the event above.
                drop(client);
                if let Ok(mut guard) = state_slot.lock() {
                    *guard = None;
                }
            });
        });

        Ok(String::new())
    })
}

/// Abort an OAuth login in progress (the user cancelled, closed the browser,
/// or the listener timed out).
///
/// Clears the SDK's stored authorization data for this attempt so a late or
/// replayed callback cannot complete it, and releases the bootstrap client.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_abort(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };

        let client_slot = Arc::clone(&bridge.client);
        let state_slot = Arc::clone(&bridge.oauth_state);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
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

/// Phase B: restore an OAuth session into the real, account-scoped store.
///
/// Mirrors `mx_rust_restore` for password sessions, but dispatches through
/// `oauth().restore_session()` so the SDK owns the OAuth session state and its
/// token refresh. `refresh_token` may be empty when the server issued none.
///
/// The caller must already have decided — from the recorded account identity
/// and the store-ownership policy — that `store_path` belongs to `user_id`
/// with `device_id`. This function does not and cannot make that judgement.
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
        // Server-issued opaque string, not a freshly generated device id.
        let parsed_device: OwnedDeviceId = device_id.clone().into();

        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let token_task = Arc::clone(&bridge.token_task);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_restore", async move {
                let client = match build_client(&homeserver, &store_path).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(&events, json!({ "type": "login_failed", "message": err }));
                        return;
                    }
                };

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

                match client
                    .oauth()
                    .restore_session(session, RoomLoadSettings::default())
                    .await
                {
                    Ok(()) => {
                        // The rooms exist now, and this runs on the SHARED
                        // runtime — both conditions the send-queue respawn
                        // needs. See resume_unsent_requests in lib.rs.
                        crate::resume_unsent_requests(&client).await;
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                        );
                        // OAuth access tokens are short-lived by design, so
                        // rotated tokens MUST be written back.
                        if let Ok(mut guard) = token_task.lock() {
                            if let Some(previous) = guard.replace(
                                spawn_token_persistence(&client, Arc::clone(&events)))
                            {
                                previous.abort();
                            }
                        }
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": user_id,
                                "device_id": device_id,
                            }),
                        );
                    }
                    Err(err) => {
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "login_failed",
                                "message": format_matrix_error(
                                    "Matrix OAuth session restore failed", err),
                            }),
                        );
                    }
                }
            });
        });

        Ok(String::new())
    })
}

/// Log out an OAuth session, revoking the tokens at the authorization server.
///
/// Password sessions continue to use `mx_rust_logout`. Both leave store
/// deletion to C++, which is the only layer that knows which account's store
/// is which.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_oauth_logout(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };

        bridge.stop_sync_and_wait();

        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "oauth_logout", async move {
                let client = client_slot.lock().ok().and_then(|mut g| g.take());
                // THE SAME SHAPE mx_rust_logout USES, because the C++ side
                // reads "result" and nothing has ever read "warning". With
                // only a warning field, the dispatcher's default of "ok"
                // applied and a failed REVOCATION was indistinguishable from
                // a successful one — on the branch whose entire purpose is
                // revoking the token.
                let event = match client.as_ref() {
                    Some(client) => match client.oauth().logout().await {
                        Ok(()) => json!({ "type": "logged_out", "result": "ok" }),
                        // A revocation failure still ends the local session:
                        // the caller has already stopped sync and is about to
                        // drop the client. Reported WITHOUT the SDK detail,
                        // which can quote endpoint URLs.
                        Err(_) => json!({
                            "type": "logged_out",
                            "result": "failed",
                            "category": "revocation_failed",
                        }),
                    },
                    None => json!({ "type": "logged_out", "result": "no_session" }),
                };
                drop(client);
                enqueue(&events, event);
            });
        });

        Ok(String::new())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // ClientMetadata is serialize-only in matrix-sdk 0.18, so these assert on
    // the JSON actually sent to the registration endpoint — which is the thing
    // that matters anyway.
    fn metadata_json(redirect: &str) -> serde_json::Value {
        let raw = client_metadata(Url::parse(redirect).unwrap()).expect("metadata builds");
        serde_json::from_str(raw.json().get()).expect("metadata is JSON")
    }

    #[test]
    fn client_metadata_registers_only_the_loopback_redirect() {
        let redirect = "http://127.0.0.1:51234/callback";
        let json = metadata_json(redirect);

        // A desktop app, so the authorization server expects a loopback
        // redirect rather than a hosted callback.
        assert_eq!(json["application_type"], "native");
        // Exactly ONE redirect URI is registered: the ephemeral loopback
        // endpoint this attempt is listening on. Registering anything wider
        // would let an authorization response be aimed elsewhere.
        assert_eq!(json["redirect_uris"], serde_json::json!([redirect]));
        // The SDK adds `refresh_token` to the requested grants itself. That is
        // required, not incidental: without it the authorization server issues
        // no refresh token and the SDK cannot renew an expired access token,
        // which is the failure this whole round exists to prevent.
        assert_eq!(json["grant_types"],
                   serde_json::json!(["authorization_code", "refresh_token"]));
    }

    #[test]
    fn client_metadata_names_the_application() {
        let json = metadata_json("http://127.0.0.1:1/cb");
        assert_eq!(json["client_name"], "Lightning");
    }

    // The device-code grant is what login_with_qr_code uses. Lightning does
    // not implement QR login, so it must not ask an authorization server for
    // that capability.
    #[test]
    fn client_metadata_does_not_request_the_device_code_grant() {
        let json = metadata_json("http://127.0.0.1:1/cb");
        let grants = json["grant_types"].as_array().expect("grant_types is a list");
        assert!(!grants.iter().any(|g| g == "urn:ietf:params:oauth:grant-type:device_code"),
                "device_code grant must not be requested: {grants:?}");
    }

    // ---------------------------------------------------------------------
    // RFC 8252 §7.3: a strict authorization server refuses to register a
    // loopback redirect URI that pins a port.
    // ---------------------------------------------------------------------

    /// The listener URI the C++ callback server produces: loopback, an
    /// ephemeral port, and a per-attempt 128-bit nonce in the path.
    const LISTENER: &str = "http://127.0.0.1:51234/lightning-oauth/0a1b2c3d4e5f60718293a4b5c6d7e8f9";
    const LISTENER_PORTLESS: &str =
        "http://127.0.0.1/lightning-oauth/0a1b2c3d4e5f60718293a4b5c6d7e8f9";

    fn url(raw: &str) -> Url {
        Url::parse(raw).expect("test URL parses")
    }

    /// The refusal continuwuity actually sends, reduced to what the wire
    /// carries: RFC 7591 §3.2.2's `invalid_client_metadata` code plus prose.
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

    // Nothing may re-attach a port on the way into the metadata: the whole
    // point of the retry is that the REGISTERED URI carries none.
    #[test]
    fn client_metadata_registers_a_portless_loopback_uri_verbatim() {
        let json = metadata_json(LISTENER_PORTLESS);
        assert_eq!(json["redirect_uris"], serde_json::json!([LISTENER_PORTLESS]));
        assert_eq!(json["application_type"], "native");
    }

    #[test]
    fn portless_loopback_redirect_strips_the_ephemeral_port_and_keeps_the_path() {
        let stripped = portless_loopback_redirect(&url(LISTENER)).expect("a port to strip");
        // The per-attempt nonce path survives, so the registration is widened
        // to "any port on loopback, THIS path" and no further.
        assert_eq!(stripped.as_str(), LISTENER_PORTLESS);
    }

    // mx_rust_oauth_begin's own guard accepts the bracketed IPv6 literal, so
    // the port stripper has to handle it too or a `[::1]` listener would fall
    // back to no retry at all.
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

    // Nothing to strip is not an error, but it must not produce a retry
    // either: re-registering the identical metadata would just fail again.
    #[test]
    fn portless_loopback_redirect_declines_when_there_is_no_port() {
        assert!(portless_loopback_redirect(&url(LISTENER_PORTLESS)).is_none());
    }

    // Defence in depth. mx_rust_oauth_begin refuses a non-loopback redirect
    // before it ever gets here, and the retry must not become a second,
    // laxer door into registering one.
    #[test]
    fn portless_loopback_redirect_refuses_a_non_loopback_host() {
        assert!(portless_loopback_redirect(&url("http://evil.example:8080/cb")).is_none());
        assert!(portless_loopback_redirect(&url("http://127.0.0.2:8080/cb")).is_none());
        assert!(portless_loopback_redirect(&url("http://127.0.0.1.evil.example:8080/cb")).is_none());
    }

    // The port rule is about http loopback URIs specifically; anything else
    // keeps whatever it was given.
    #[test]
    fn portless_loopback_redirect_refuses_a_non_http_scheme() {
        assert!(portless_loopback_redirect(&url("https://127.0.0.1:8443/cb")).is_none());
        assert!(portless_loopback_redirect(&url("org.lightning:/cb")).is_none());
    }

    #[test]
    fn registration_refused_metadata_recognises_a_metadata_refusal() {
        // The code is REPORTED, not merely detected: the retry's diagnostic
        // names which of the two the server actually sent.
        assert_eq!(
            registration_refused_metadata(&continuwuity_refusal()),
            Some("invalid_client_metadata")
        );
        // The sibling code RFC 7591 defines for the same class of complaint.
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

    // A retry is only ever correct for a refusal of what we SENT. A transport
    // failure or a server with no registration endpoint would fail the second
    // attempt identically, and a later-stage failure has already registered.
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

    // THE RULE THIS ROUND EXISTS FOR: register without the port, request WITH
    // it. matrix-sdk 0.18 keeps the two separate — OAuth::login() takes the
    // request URI directly and never reads it back out of the metadata — so
    // both halves come out of this one function and are asserted together.
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

        // The authorization request keeps the live port: that is the URI the
        // browser is sent to, the one the listener is actually bound to, and
        // the one finish_login() replays in the token exchange.
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

    // A metadata refusal is not a licence to register a redirect URI pointing
    // off the machine.
    #[test]
    fn portless_registration_retry_declines_a_non_loopback_redirect() {
        assert!(portless_registration_retry(
            &url("http://evil.example:8080/cb"),
            &continuwuity_refusal(),
        )
        .is_none());
    }

    // A server that refuses metadata for some OTHER reason must not put us in
    // a loop: with no port to strip there is nothing new to send.
    #[test]
    fn portless_registration_retry_declines_when_the_uri_is_already_portless() {
        assert!(
            portless_registration_retry(&url(LISTENER_PORTLESS), &continuwuity_refusal()).is_none()
        );
    }
}

/// v0.7.x session management for MAS/OAuth accounts. Password UIA does not
/// exist for them — device sign-out happens in the account-management web
/// console. Answers with the console URL for one action:
/// `device_id` empty → the sessions list, otherwise the delete page for
/// that device. Result event: `oauth_management_url { op_id, ok, url }`.
/// The URL is the user's own account console; it carries no secret, and it
/// is never logged here.
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
