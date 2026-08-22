//! Legacy Matrix SSO (`m.login.sso`) for Lightning, on matrix-sdk 0.18's
//! `Client::matrix_auth()` API.
//!
//! # This is NOT OAuth
//!
//! Lightning supports two browser sign-in flows and they are deliberately kept
//! apart. OAuth 2.0/OIDC (`crate::oauth`) talks to an authorization server,
//! does PKCE and a code exchange, and yields an OAuth session with a client id
//! and a rotating refresh token. Legacy Matrix SSO predates all of that: the
//! homeserver redirects the browser back with a single-use `loginToken`, which
//! is exchanged through the ordinary `/login` endpoint with
//! `type: m.login.token` for a normal Matrix session. A server can offer
//! either, both, or neither.
//!
//! # No new dependency, and no second HTTP server
//!
//! matrix-sdk 0.18 has a convenience wrapper, `MatrixAuth::login_sso()`, that
//! runs its own local web server — and it is behind the `sso-login` feature,
//! whose `axum` dependency is not vendored in this offline `--locked` build.
//! It is not needed. The two primitives underneath it are NOT feature-gated:
//!
//!   * `MatrixAuth::get_sso_login_url(redirect_url, idp_id)` builds the
//!     server's SSO redirect URL (and handles identity-provider selection);
//!   * `MatrixAuth::login_token(token)` exchanges the returned login token.
//!
//! So this module uses those directly and Lightning's EXISTING hardened
//! loopback listener (`src/auth/OAuthCallbackServer`) receives the redirect —
//! the same listener OAuth uses, rather than a second unrelated local server.
//! Every protocol primitive stays SDK-owned; Lightning contributes only the
//! browser launch and the loopback endpoint.
//!
//! # The two-phase store lifecycle applies here too
//!
//! Exactly as for OAuth, and for the same reason: the Matrix user id is not
//! known until the token exchange returns it, so Phase A runs on a bootstrap
//! client with an **in-memory store only** and must never sync (a sync would
//! upload device keys from a throwaway crypto store that Phase B would then
//! contradict). Phase B — in C++ — derives the account identity, applies the
//! session policy, opens that account's sqlite store and restores the session
//! through the ordinary `mx_rust_restore_client` path. A device the server
//! just issued must never adopt a store belonging to a different device.
//!
//! # The login token is a credential
//!
//! `loginToken` is single-use and short-lived, but it IS a credential: anyone
//! holding it can complete this sign-in. It is therefore never logged, never
//! placed in an error message (errors from the exchange are reported as fixed
//! text rather than formatted, because SDK errors can quote the request), and
//! never returned across the FFI. It enters this module and is consumed here.

use std::ffi::{c_char, c_void};
use std::path::PathBuf;
use std::sync::Arc;

use matrix_sdk::ruma::api::client::session::get_login_types::v3::LoginType;
use serde_json::json;
use url::Url;

use crate::{bridge, build_client, cstr_arg, enqueue, ffi_string, run_async_on};

/// Shown to the homeserver as this session's device name, matching the other
/// login paths so a user's device list reads consistently.
const DEVICE_DISPLAY_NAME: &str = "Lightning";

/// Reject any redirect that is not loopback.
///
/// Defence in depth: the C++ listener only ever binds 127.0.0.1, but we also
/// refuse to ASK a homeserver to send a login token anywhere else, so a caller
/// passing something odd cannot turn the server into a token courier for a
/// remote host.
fn require_loopback(redirect: &Url) -> Result<(), String> {
    match redirect.host_str() {
        Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => Ok(()),
        _ => Err("SSO redirect URI must be a loopback address.".to_owned()),
    }
}

/// List the identity providers a homeserver advertises for `m.login.sso`.
///
/// Enqueues:
///
/// ```json
/// { "type": "sso_providers", "homeserver": "...", "sso": true,
///   "providers": [ { "id": "oidc-google", "name": "Google", "icon": "mxc://…" } ] }
/// ```
///
/// An SSO server that advertises NO providers is normal and common — it means
/// "one unnamed flow", and the UI offers a single generic action. Provider
/// names come from the server's own answer; nothing is hard-coded per vendor.
///
/// `icon` is passed through only when it is an `mxc:` URI. A provider icon is
/// remote text from the homeserver, and an `http(s)` value there would make
/// the login screen fetch an image from a host chosen by that server before
/// the user has signed in to anything.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sso_providers(
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
            run_async_on(shared_runtime, runtime_events, "sso_providers", async move {
                let client = match build_client(&homeserver, &PathBuf::new()).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_providers",
                                "homeserver": homeserver,
                                "sso": false,
                                "providers": [],
                                "error": err,
                            }),
                        );
                        return;
                    }
                };

                let mut sso = false;
                let mut providers = Vec::new();
                if let Ok(response) = client.matrix_auth().get_login_types().await {
                    for flow in &response.flows {
                        if let LoginType::Sso(details) = flow {
                            sso = true;
                            for idp in &details.identity_providers {
                                let icon = idp
                                    .icon
                                    .as_ref()
                                    .map(|uri| uri.to_string())
                                    .filter(|uri| uri.starts_with("mxc://"))
                                    .unwrap_or_default();
                                providers.push(json!({
                                    "id": idp.id,
                                    "name": idp.name,
                                    "icon": icon,
                                }));
                            }
                        }
                    }
                }

                enqueue(
                    &events,
                    json!({
                        "type": "sso_providers",
                        "homeserver": homeserver,
                        "sso": sso,
                        "providers": providers,
                        "error": serde_json::Value::Null,
                    }),
                );
            });
        });

        Ok(String::new())
    })
}

/// Begin an SSO login: ask the homeserver for its SSO redirect URL.
///
/// `idp_id` selects one advertised identity provider; empty means the server's
/// default single flow. The bootstrap `Client` is parked in the bridge's client
/// slot because the session produced by `mx_rust_sso_finish` must land on the
/// same instance.
///
/// Enqueues `{"type": "sso_url", "url": "..."}` or
/// `{"type": "sso_failed", "message": "..."}`. The URL carries no credentials:
/// it is the server's SSO endpoint plus our loopback redirect.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sso_begin(
    ptr: *mut c_void,
    homeserver: *const c_char,
    redirect_uri: *const c_char,
    idp_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let redirect_uri = unsafe { cstr_arg(redirect_uri) }?;
        let idp_id = unsafe { cstr_arg(idp_id) }?;

        let redirect = Url::parse(&redirect_uri)
            .map_err(|err| format!("invalid SSO redirect URI: {err}"))?;
        require_loopback(&redirect)?;

        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "sso_begin", async move {
                let client = match build_client(&homeserver, &PathBuf::new()).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(&events, json!({ "type": "sso_failed", "message": err }));
                        return;
                    }
                };

                let idp = if idp_id.trim().is_empty() { None } else { Some(idp_id.as_str()) };
                match client.matrix_auth().get_sso_login_url(redirect.as_str(), idp).await {
                    Ok(url) => {
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(&events, json!({ "type": "sso_url", "url": url }));
                    }
                    Err(_) => {
                        // Not formatted: an SDK error here can quote the
                        // request, and the request contains our redirect. The
                        // failure mode is also singular enough to name plainly.
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_failed",
                                "message": "This homeserver did not provide an \
                                            SSO sign-in address.",
                            }),
                        );
                    }
                }
            });
        });

        Ok(String::new())
    })
}

/// Complete an SSO login by exchanging the `loginToken` the browser returned.
///
/// The token is a CREDENTIAL: it is never logged, never echoed into an error,
/// and never leaves this module. On success this enqueues the same identity and
/// session material `oauth_ok` carries, so C++ Phase B opens the account store
/// through one shared path:
///
/// ```json
/// { "type": "sso_ok", "user_id": "@u:s", "device_id": "ABC",
///   "access_token": "…", "refresh_token": "…" }
/// ```
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sso_finish(
    ptr: *mut c_void,
    login_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let login_token = unsafe { cstr_arg(login_token) }?;
        if login_token.trim().is_empty() {
            // Deliberately does not quote the input.
            return Err("empty SSO login token".to_owned());
        }

        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "sso_finish", async move {
                let client = match client_slot.lock().ok().and_then(|mut g| g.take()) {
                    Some(client) => client,
                    None => {
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_failed",
                                "message": "No SSO sign-in is in progress.",
                            }),
                        );
                        return;
                    }
                };

                let outcome = client
                    .matrix_auth()
                    .login_token(&login_token)
                    .initial_device_display_name(DEVICE_DISPLAY_NAME)
                    .await;
                if outcome.is_err() {
                    // A used, expired or forged token all land here. The SDK
                    // error is NOT formatted in: it can quote the request body,
                    // which is the token.
                    drop(client);
                    enqueue(
                        &events,
                        json!({
                            "type": "sso_failed",
                            "message": "The sign-in could not be completed. \
                                        The sign-in may have been cancelled or \
                                        it expired. Please try again.",
                        }),
                    );
                    return;
                }

                let session = match client.matrix_auth().session() {
                    Some(session) => session,
                    None => {
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_failed",
                                "message": "The server completed sign-in without \
                                            returning a session.",
                            }),
                        );
                        return;
                    }
                };

                enqueue(
                    &events,
                    json!({
                        "type": "sso_ok",
                        "user_id": session.meta.user_id.to_string(),
                        "device_id": session.meta.device_id.to_string(),
                        "access_token": session.tokens.access_token,
                        "refresh_token": session.tokens.refresh_token,
                    }),
                );

                // Phase A is over: drop the bootstrap client so its in-memory
                // store, and the tokens held in it, go away. Phase B builds the
                // real account-scoped client from the event above.
                drop(client);
            });
        });

        Ok(String::new())
    })
}

/// Abort an SSO login in progress (cancelled, browser closed, listener timed
/// out). Releases the bootstrap client so a late callback cannot complete a
/// sign-in the user has already abandoned, and so the next attempt starts
/// clean.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sso_abort(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if let Ok(mut guard) = bridge.client.lock() {
            let _ = guard.take();
        }
        Ok(String::new())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_loopback_redirects_are_accepted() {
        for good in [
            "http://127.0.0.1:1234/callback",
            "http://localhost:1/x",
            "http://[::1]:9/x",
        ] {
            assert!(
                require_loopback(&Url::parse(good).unwrap()).is_ok(),
                "should accept {good}"
            );
        }
        // A homeserver must never be asked to deliver a login token off-box.
        for bad in [
            "http://example.org/callback",
            "https://127.0.0.1.evil.example/x",
            "http://10.0.0.5/x",
        ] {
            assert!(
                require_loopback(&Url::parse(bad).unwrap()).is_err(),
                "should refuse {bad}"
            );
        }
    }
}
