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

/// Client URI advertised during dynamic registration (shown on the consent
/// screen). Not a secret.
const CLIENT_URI: &str = "https://gitlab.smetonis.net/Mizerd/lightning";

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
                // The server rejected the token and the SDK could not renew it: report the
                // revoked-credential state instead of letting sync fail in a loop.
                Ok(SessionChange::UnknownToken(_)) => {
                    enqueue(
                        &events,
                        json!({
                            "type": "session_token_revoked",
                        }),
                    );
                }
                // Lagged only means missed notifications; the next carries current tokens.
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
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
/// hard-coded per provider. `sso` is informational only: the SDK's SSO
/// helper needs `axum`, which this build lacks.
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
                let oauth_supported = client.oauth().server_metadata().await.is_ok();

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
                        "error": serde_json::Value::Null,
                    }),
                );
            });
        });

        Ok(String::new())
    })
}

/// Begin an OAuth login: register the client if needed and build the
/// authorization URL. Enqueues `{"type": "oauth_url", "url": "..."}` or
/// `{"type": "oauth_failed", "message": "..."}`. The bootstrap Client is
/// parked in the client slot because the SDK stores this attempt's PKCE
/// verifier and CSRF state in it; `finish_login()` must use the same
/// instance. The URL (opened by C++) contains no credentials.
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

                // device_id None: the SDK generates one, encoded in the scope, and returns
                // it from finish_login(); Phase B's store must belong to it.
                let mut built = client
                    .oauth()
                    .login(redirect.clone(), None, Some(metadata.into()), None)
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

                // The SDK checks `state` and exchanges the code with the PKCE verifier; a
                // mismatch, denial or replayed callback fails here.
                if let Err(err) = client.oauth().finish_login(callback_url.into()).await {
                    // The error may quote the callback, so report a fixed message instead.
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
                // The offline-capable build, as for password restores (see
                // build_client_for_restore).
                let client = match crate::build_client_for_restore(
                    &homeserver, &store_path, &events).await {
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
                        // On the shared runtime with rooms loaded, as the send-queue respawn
                        // requires (see resume_unsent_requests in lib.rs).
                        crate::resume_unsent_requests(&client).await;
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                        );
                        // OAuth access tokens are short-lived, so rotations must be persisted.
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

/// Log out an OAuth session, revoking its tokens at the authorization
/// server. Password sessions use `mx_rust_logout`. Store deletion is left
/// to C++.
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
                enqueue(&events, event);
            });
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
