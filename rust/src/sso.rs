//! Legacy Matrix SSO (`m.login.sso`) on matrix-sdk 0.18's
//! `Client::matrix_auth()`.
//!
//! Not OAuth (`crate::oauth`): the homeserver redirects back with a
//! single-use `loginToken`, exchanged at `/login` (`m.login.token`) for a
//! normal Matrix session.
//!
//! `MatrixAuth::login_sso()` needs the `sso-login` feature (`axum`, not
//! vendored), but its primitives are ungated:
//! `MatrixAuth::get_sso_login_url(redirect_url, idp_id)` and
//! `MatrixAuth::login_token(token)`. The redirect is received by the same
//! loopback listener OAuth uses (`src/auth/OAuthCallbackServer`).
//!
//! Same two phases as OAuth: the user id is unknown until the token
//! exchange, so Phase A uses an in-memory bootstrap client that must never
//! sync, and Phase B (C++) opens the account's store and restores through
//! `mx_rust_restore_client`.
//!
//! The login token is a credential: never logged, never put in an error
//! message (SDK errors can quote the request), never returned across the FFI.

use std::ffi::{c_char, c_void};
use std::path::PathBuf;
use std::sync::Arc;

use matrix_sdk::ruma::api::client::session::get_login_types::v3::LoginType;
use serde_json::json;
use url::Url;

use crate::{bridge, build_client, cstr_arg, enqueue, ffi_string, run_async_on};

/// Device name shown to the homeserver, as for the other login paths.
const DEVICE_DISPLAY_NAME: &str = "Lightning";

/// Reject any non-loopback redirect. The C++ listener binds 127.0.0.1 only;
/// this also stops us asking a homeserver to send a login token elsewhere.
fn require_loopback(redirect: &Url) -> Result<(), String> {
    match redirect.host_str() {
        Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => Ok(()),
        _ => Err("SSO redirect URI must be a loopback address.".to_owned()),
    }
}

/// List the identity providers a homeserver advertises for `m.login.sso`.
/// Enqueues:
///
/// ```json
/// { "type": "sso_providers", "homeserver": "...", "sso": true,
///   "providers": [ { "id": "oidc-google", "name": "Google", "icon": "mxc://…" } ] }
/// ```
///
/// No providers is common (one unnamed flow). Names come from the server.
/// `icon` passes only as an `mxc:` URI, so the login screen never fetches
/// from a host the server chose.
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

/// Begin an SSO login by asking the homeserver for its redirect URL. Empty
/// `idp_id` means the default flow. The bootstrap Client is parked in the
/// client slot because `mx_rust_sso_finish` must use the same instance.
/// Enqueues `{"type": "sso_url", "url": "..."}` or
/// `{"type": "sso_failed", "message": "..."}`; the URL holds no credentials.
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
                        // Not formatted: the SDK error can quote the request.
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

/// Complete an SSO login by exchanging the `loginToken` from the browser.
/// The token never leaves this module. On success enqueues the same
/// identity and session material as `oauth_ok`, so C++ Phase B shares one
/// path:
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
            // Does not quote the input.
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
                    // Used, expired or forged tokens land here. The SDK error is not included:
                    // it can quote the request body, which is the token.
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

                // Phase A is over: drop the bootstrap client and its in-memory tokens.
                drop(client);
            });
        });

        Ok(String::new())
    })
}

/// Abort an SSO login in progress, releasing the bootstrap client so a late
/// callback cannot complete an abandoned sign-in.
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
        // A login token must never be delivered off-box.
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
