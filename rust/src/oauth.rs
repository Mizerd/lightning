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
/// `redirect_uri` is the loopback URI the C++ listener is bound to. It is
/// registered as the sole redirect URI, so an authorization response aimed
/// anywhere else is rejected by the authorization server rather than by us.
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
                let built = client
                    .oauth()
                    .login(redirect, None, Some(metadata.into()), None)
                    .build()
                    .await;

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
                let event = match client.as_ref() {
                    Some(client) => match client.oauth().logout().await {
                        Ok(()) => json!({ "type": "logged_out" }),
                        // A revocation failure still ends the local session:
                        // the caller has already stopped sync and is about to
                        // drop the client. Report it without the SDK detail,
                        // which can quote endpoint URLs.
                        Err(_) => json!({
                            "type": "logged_out",
                            "warning": "The server could not be told to end this session.",
                        }),
                    },
                    None => json!({ "type": "logged_out" }),
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
}
