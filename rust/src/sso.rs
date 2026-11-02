
use std::ffi::{c_char, c_void};
use std::sync::Arc;

use matrix_sdk::ruma::api::client::session::get_login_types::v3::LoginType;
use serde_json::json;
use url::Url;

use crate::oauth::sdk_failure_reason;
use crate::{bridge, cstr_arg, enqueue, ffi_string, run_async_on};

const DEVICE_DISPLAY_NAME: &str = "Lightning";

/// Reject any non-loopback redirect. The C++ listener binds 127.0.0.1 only;
/// this also stops us asking a homeserver to send a login token elsewhere.
fn require_loopback(redirect: &Url) -> Result<(), String> {
    match redirect.host_str() {
        Some("127.0.0.1") | Some("localhost") | Some("[::1]") | Some("::1") => Ok(()),
        _ => Err("SSO redirect URI must be a loopback address.".to_owned()),
    }
}

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
        let route_of = crate::route::key_for(ptr as usize, &bridge.store_path);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "sso_providers", async move {
                let client = match crate::build_memory_client(&homeserver, &route_of).await {
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
                                let brand = idp
                                    .brand
                                    .as_ref()
                                    .map(|brand| brand.as_str().to_ascii_lowercase())
                                    .filter(|brand| {
                                        brand.len() <= 32
                                            && brand.bytes().all(|b| {
                                                b.is_ascii_lowercase()
                                                    || b.is_ascii_digit()
                                                    || b == b'.'
                                                    || b == b'_'
                                                    || b == b'-'
                                            })
                                    })
                                    .unwrap_or_default();
                                providers.push(json!({
                                    "id": idp.id,
                                    "name": idp.name,
                                    "icon": icon,
                                    "brand": brand,
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
        let route_of = crate::route::key_for(ptr as usize, &bridge.store_path);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "sso_begin", async move {
                let client = match crate::build_memory_client(&homeserver, &route_of).await {
                    Ok(client) => client,
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "sso_failed",
                            "stage": "begin",
                            "reason": "client_build_failed",
                            "message": err,
                        }));
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
                    Err(err) => {
                        drop(client);
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_failed",
                                "stage": "begin",
                                "reason": sdk_failure_reason(&err),
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sso_finish(
    ptr: *mut c_void,
    login_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let login_token = unsafe { cstr_arg(login_token) }?;
        if login_token.trim().is_empty() {
            return Err("empty SSO login token".to_owned());
        }

        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let shared_runtime = Arc::clone(&bridge.runtime);
        crate::spawn_session_thread(None, move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "sso_finish", async move {
                let client = match client_slot.lock().ok().and_then(|mut g| g.take()) {
                    Some(client) => client,
                    None => {
                        enqueue(
                            &events,
                            json!({
                                "type": "sso_failed",
                                "stage": "finish",
                                "reason": "no_attempt",
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
                if let Err(err) = outcome {
                    let reason = sdk_failure_reason(&err);
                    drop(client);
                    enqueue(
                        &events,
                        json!({
                            "type": "sso_failed",
                            "stage": "finish",
                            "reason": reason,
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
                                "stage": "finish",
                                "reason": "no_session",
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

                drop(client);
            });
        });

        Ok(String::new())
    })
}

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
