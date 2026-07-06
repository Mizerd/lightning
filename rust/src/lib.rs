//! matrix-client-rust — Matrix Rust SDK FFI bridge for Lightning.
//!
//! The C++ backend owns UI state and polls this bridge for JSON events. Rust
//! owns the Matrix SDK client, Tokio runtime, and SDK SQLite store.

use std::{
    collections::VecDeque,
    ffi::{c_char, CStr, CString},
    fs::OpenOptions,
    io::Write,
    os::raw::{c_int, c_void},
    panic::{catch_unwind, AssertUnwindSafe},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use matrix_sdk::{
    authentication::matrix::MatrixSession,
    config::SyncSettings,
    ruma::{
        events::room::{
            encrypted::OriginalSyncRoomEncryptedEvent,
            message::{MessageType, OriginalSyncRoomMessageEvent, RoomMessageEventContent},
        },
        OwnedDeviceId, OwnedTransactionId, OwnedUserId, RoomId, UserId,
    },
    store::RoomLoadSettings,
    Client, LoopCtrl, Room, SessionMeta, SessionTokens,
};
use serde::{Deserialize, Serialize};
use serde_json::json;

struct RustClient {
    store_path: PathBuf,
    session_file: Arc<Mutex<Option<PathBuf>>>,
    client: Arc<Mutex<Option<Client>>>,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_stop: Arc<Mutex<Option<Arc<AtomicBool>>>>,
}

impl RustClient {
    fn new(store_path: PathBuf) -> Result<Self, String> {
        Ok(Self {
            store_path,
            session_file: Arc::new(Mutex::new(None)),
            client: Arc::new(Mutex::new(None)),
            events: Arc::new(Mutex::new(VecDeque::new())),
            sync_stop: Arc::new(Mutex::new(None)),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    fn abort_sync(&self) {
        if let Ok(mut guard) = self.sync_stop.lock() {
            if let Some(stop) = guard.take() {
                stop.store(true, Ordering::SeqCst);
            }
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct PersistentSessionFile {
    version: u32,
    homeserver: String,
    session: MatrixSession,
}

impl Drop for RustClient {
    fn drop(&mut self) {
        self.abort_sync();
    }
}

#[no_mangle]
pub extern "C" fn mx_rust_backend_name() -> *mut c_char {
    ffi_string(|| Ok("matrix-rust-sdk".to_owned()))
}

#[no_mangle]
pub extern "C" fn mx_rust_status_string() -> *mut c_char {
    ffi_string(|| {
        Ok("Matrix Rust SDK backend linked. Login, session restore, joined-room sync, and plain text send are wired through the SDK event queue. E2EE capability remains disabled until encrypted read/send is verified end-to-end.".to_owned())
    })
}

#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee(_client: *mut c_void) -> c_int {
    0
}

#[no_mangle]
pub extern "C" fn mx_rust_version() -> *mut c_char {
    ffi_string(|| Ok(env!("CARGO_PKG_VERSION").to_owned()))
}

#[no_mangle]
pub extern "C" fn mx_rust_create(store_path: *const c_char) -> *mut c_void {
    match catch_unwind(AssertUnwindSafe(|| {
        let store_path = unsafe { cstr_arg(store_path) }?;
        let path = PathBuf::from(store_path);
        std::fs::create_dir_all(&path)
            .map_err(|err| format!("failed to create Rust SDK store directory: {err}"))?;
        let client = RustClient::new(path)?;
        Ok::<*mut c_void, String>(Box::into_raw(Box::new(client)) as *mut c_void)
    })) {
        Ok(Ok(ptr)) => ptr,
        Ok(Err(_)) | Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_session_file(
    ptr: *mut c_void,
    session_file_path: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let path = unsafe { cstr_arg(session_file_path) }?;
        let next = if path.trim().is_empty() {
            None
        } else {
            Some(PathBuf::from(path))
        };
        let mut guard = bridge
            .session_file
            .lock()
            .map_err(|_| "Rust SDK session-file lock poisoned.".to_owned())?;
        *guard = next;
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_destroy(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if ptr.is_null() {
            return;
        }
        drop(unsafe { Box::from_raw(ptr as *mut RustClient) });
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_login(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user: *const c_char,
    password: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user = unsafe { cstr_arg(user) }?;
        let password = unsafe { cstr_arg(password) }?;

        bridge.abort_sync();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let session_file = Arc::clone(&bridge.session_file);
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "login", async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
                        install_event_handlers(&client, Arc::clone(&events));
                        let login = client
                            .matrix_auth()
                            .login_username(&user, &password)
                            .initial_device_display_name("Lightning")
                            .send()
                            .await;
                        match login {
                            Ok(response) => {
                                let session = MatrixSession::from(&response);
                                if let Some(path) = configured_session_file(&session_file) {
                                    if let Err(err) =
                                        save_persistent_session(&path, &homeserver, &session)
                                    {
                                        enqueue(
                                            &events,
                                            json!({
                                                "type": "error",
                                                "message": format!(
                                                    "Failed to save Rust SDK smoke session: {err}"
                                                ),
                                            }),
                                        );
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
                                        "user_id": response.user_id.to_string(),
                                        "device_id": response.device_id.to_string(),
                                        "access_token": response.access_token,
                                    }),
                                );
                            }
                            Err(err) => enqueue(
                                &events,
                                json!({
                                    "type": "login_failed",
                                    "message": format_matrix_error("Matrix Rust SDK login failed", err),
                                }),
                            ),
                        }
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({ "type": "login_failed", "message": err }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_restore_from_file(
    ptr: *mut c_void,
    homeserver: *const c_char,
    expected_user_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let expected_user_id = unsafe { cstr_arg(expected_user_id) }?;
        let Some(session_file) = configured_session_file(&bridge.session_file) else {
            return Ok("error: Rust SDK smoke session file is not configured.".to_owned());
        };

        bridge.abort_sync();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "restore_from_file", async move {
                let stored = match read_persistent_session(&session_file) {
                    Ok(stored) => stored,
                    Err(err) => {
                        enqueue(
                            &events,
                            json!({
                                "type": "login_failed",
                                "message": format!("Rust SDK smoke session restore unavailable: {err}"),
                            }),
                        );
                        return;
                    }
                };

                if stored.homeserver != homeserver {
                    enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session homeserver does not match this run.",
                        }),
                    );
                    return;
                }

                let stored_user = stored.session.meta.user_id.to_string();
                if !expected_user_id.is_empty() && stored_user != expected_user_id {
                    enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session account does not match this run.",
                        }),
                    );
                    return;
                }

                match restore_client_with_session(&homeserver, &store_path, stored.session.clone())
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(&client, Arc::clone(&events));
                        if let Err(err) =
                            save_persistent_session(&session_file, &homeserver, &stored.session)
                        {
                            enqueue(
                                &events,
                                json!({
                                    "type": "error",
                                    "message": format!(
                                        "Failed to refresh Rust SDK smoke session: {err}"
                                    ),
                                }),
                            );
                        }
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": stored_user,
                                "device_id": stored.session.meta.device_id.to_string(),
                            }),
                        );
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": err,
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_restore(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user_id: *const c_char,
    device_id: *const c_char,
    access_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;

        bridge.abort_sync();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "restore", async move {
                match restore_client(&homeserver, &store_path, &user_id, &device_id, access_token)
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(&client, Arc::clone(&events));
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
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": err,
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_logout(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        bridge.abort_sync();
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone());
        let events = Arc::clone(&bridge.events);
        if let Some(client) = client {
            std::thread::spawn(move || {
                let runtime_events = Arc::clone(&events);
                run_async(runtime_events, "logout", async move {
                    let _ = client.matrix_auth().logout().await;
                    enqueue(&events, json!({ "type": "logged_out" }));
                });
            });
        } else {
            bridge.enqueue(json!({ "type": "logged_out" }));
        }
        if let Ok(mut guard) = bridge.client.lock() {
            *guard = None;
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_sync(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            bridge.enqueue(json!({
                "type": "error",
                "message": "Rust SDK sync requested before a session was available.",
            }));
            return;
        };

        // Atomically check-and-reserve the sync slot so two rapid
        // start_sync() calls can't both spawn a sync loop (the earlier
        // code released the lock between the check and the assignment,
        // leaving a race that leaked the first loop's `stop` flag).
        let stop = Arc::new(AtomicBool::new(false));
        {
            let mut guard = match bridge.sync_stop.lock() {
                Ok(g) => g,
                Err(_) => return,
            };
            if guard.is_some() {
                return;
            }
            *guard = Some(Arc::clone(&stop));
        }

        let events = Arc::clone(&bridge.events);
        bridge.enqueue(json!({ "type": "status", "state": "syncing" }));

        let callback_stop = Arc::clone(&stop);
        let sync_stop_slot = Arc::clone(&bridge.sync_stop);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                let first_response = Arc::new(AtomicBool::new(true));
                let settings = SyncSettings::default()
                    .ignore_timeout_on_first_sync(true)
                    .full_state(true);

                let callback_client = client.clone();
                let callback_events = Arc::clone(&events);
                let callback_first = Arc::clone(&first_response);
                let callback_stop = Arc::clone(&callback_stop);
                let result = client
                    .sync_with_callback(settings, move |_response| {
                        let client = callback_client.clone();
                        let events = Arc::clone(&callback_events);
                        let first_response = Arc::clone(&callback_first);
                        let stop = Arc::clone(&callback_stop);
                        async move {
                            if stop.load(Ordering::SeqCst) {
                                return LoopCtrl::Break;
                            }
                            enqueue_rooms(&events, &client).await;
                            if first_response.swap(false, Ordering::SeqCst) {
                                enqueue(&events, json!({ "type": "initial_sync_done" }));
                            }
                            LoopCtrl::Continue
                        }
                    })
                    .await;

                if let Err(err) = result {
                    enqueue(
                        &events,
                        json!({
                            "type": "sync_error",
                            "message": format_matrix_error("Matrix Rust SDK sync failed", err),
                        }),
                    );
                }

                if let Ok(mut guard) = sync_stop_slot.lock() {
                    *guard = None;
                }
            });
        });
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stop_sync(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        bridge.abort_sync();
        bridge.enqueue(json!({ "type": "status", "state": "disconnected" }));
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_poll_event(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event = bridge
            .events
            .lock()
            .map_err(|_| "Rust SDK event queue lock poisoned.".to_owned())?
            .pop_front()
            .unwrap_or_default();
        Ok(event)
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_send_text(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    transaction_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let transaction_id = unsafe { cstr_arg(transaction_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "send_text", async move {
                let Some(room_id_ref) = RoomId::parse(&room_id).ok() else {
                    enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Invalid Matrix room id.",
                        }),
                    );
                    return;
                };
                let Some(room) = client.get_room(&room_id_ref) else {
                    enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Rust SDK does not know that room yet.",
                        }),
                    );
                    return;
                };

                if room.encryption_state().is_encrypted() {
                    enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Cannot send to encrypted rooms yet: Rust SDK encrypted send has not been verified.",
                        }),
                    );
                    return;
                }

                let content = RoomMessageEventContent::text_plain(body);
                let txn: OwnedTransactionId = transaction_id.clone().into();
                match room.send(content).with_transaction_id(txn).await {
                    Ok(result) => enqueue(
                        &events,
                        json!({
                            "type": "send_ok",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "event_id": result.response.event_id.to_string(),
                        }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": format_matrix_error("Matrix Rust SDK send failed", err),
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

/// Encrypted-room test probe (v0.5.0-prep+6). Structurally similar to
/// mx_rust_send_text but REFUSES non-encrypted rooms — the mirror-image of
/// the safety in mx_rust_send_text. Neither entry point can be misused: the
/// UI-facing send goes through mx_rust_send_text and only reaches
/// unencrypted rooms; the smoke-only probe goes through this function and
/// only reaches encrypted rooms.
///
/// matrix-sdk performs the encryption end-to-end via its e2e-encryption +
/// sqlite features. This FFI never sees ciphertext, keys, or session
/// material. On success the SDK returns a server event id (safe to log).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_probe_encrypted_send(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    transaction_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let transaction_id = unsafe { cstr_arg(transaction_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "probe_encrypted_send", async move {
                let Some(room_id_ref) = RoomId::parse(&room_id).ok() else {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Invalid Matrix room id.",
                        }),
                    );
                    return;
                };
                let Some(room) = client.get_room(&room_id_ref) else {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Rust SDK does not know that room yet.",
                        }),
                    );
                    return;
                };
                if !room.encryption_state().is_encrypted() {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Probe refused: target room is not encrypted.",
                        }),
                    );
                    return;
                }

                let content = RoomMessageEventContent::text_plain(body);
                let txn: OwnedTransactionId = transaction_id.clone().into();
                match room.send(content).with_transaction_id(txn).await {
                    Ok(result) => enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_ok",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "event_id": result.response.event_id.to_string(),
                        }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": format_matrix_error(
                                "Matrix Rust SDK encrypted send failed", err),
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

/// Key-backup recovery probe (v0.5.0-prep+7). Calls
/// `client.encryption().recovery().recover(recovery_key)` on matrix-sdk
/// v0.18 to import backed-up room keys from the homeserver's secret
/// storage. The recovery key must arrive here as a plain string; the C++
/// side is expected to sanitise before calling. This FFI **never** logs
/// the recovery key or the imported key material. Result events on the
/// poll queue:
///   { "type": "key_backup_status", "state": "attempted" }
///   { "type": "key_backup_status", "state": "ok" }
///   { "type": "key_backup_status", "state": "failed", "message": "…" }
///
/// Recovery-passphrase support is intentionally NOT wired here — the
/// matrix-sdk API takes a recovery *key* (BASE58) on the fast path. The
/// smoke harness reports `key_backup=failed reason=passphrase_not_supported`
/// if a passphrase is provided but no key.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_recover_from_backup(
    ptr: *mut c_void,
    recovery_key: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let recovery_key = unsafe { cstr_arg(recovery_key) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "recover_backup", async move {
                enqueue(
                    &events,
                    json!({ "type": "key_backup_status", "state": "attempted" }),
                );
                let recovery = client.encryption().recovery();
                match recovery.recover(&recovery_key).await {
                    Ok(_) => enqueue(
                        &events,
                        json!({ "type": "key_backup_status", "state": "ok" }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "key_backup_status",
                            "state": "failed",
                            "message": format_matrix_error(
                                "Matrix Rust SDK key backup recover failed", err),
                        }),
                    ),
                }
            });
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_free_cstring(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe {
            drop(CString::from_raw(ptr));
        }
    }));
}

async fn build_client(homeserver: &str, store_path: &Path) -> Result<Client, String> {
    Client::builder()
        .homeserver_url(homeserver)
        .sqlite_store(store_path, None)
        .user_agent("Lightning/0.5.0")
        .build()
        .await
        .map_err(|err| format_matrix_error("failed to build Matrix Rust SDK client", err))
}

async fn restore_client(
    homeserver: &str,
    store_path: &Path,
    user_id: &str,
    device_id: &str,
    access_token: String,
) -> Result<Client, String> {
    let user_id: OwnedUserId = UserId::parse(user_id)
        .map_err(|err| format!("invalid stored Matrix user id: {err}"))?
        .to_owned();
    let device_id: OwnedDeviceId = device_id.to_owned().into();
    let session = MatrixSession {
        meta: SessionMeta { user_id, device_id },
        tokens: SessionTokens { access_token, refresh_token: None },
    };
    restore_client_with_session(homeserver, store_path, session).await
}

async fn restore_client_with_session(
    homeserver: &str,
    store_path: &Path,
    session: MatrixSession,
) -> Result<Client, String> {
    let client = build_client(homeserver, store_path).await?;
    client
        .matrix_auth()
        .restore_session(session, RoomLoadSettings::default())
        .await
        .map_err(|err| format_matrix_error("Matrix Rust SDK session restore failed", err))?;
    Ok(client)
}

fn configured_session_file(slot: &Arc<Mutex<Option<PathBuf>>>) -> Option<PathBuf> {
    slot.lock().ok().and_then(|guard| guard.clone())
}

fn read_persistent_session(path: &Path) -> Result<PersistentSessionFile, String> {
    let bytes = std::fs::read(path)
        .map_err(|err| format!("failed to read session file: {err}"))?;
    serde_json::from_slice(&bytes)
        .map_err(|err| format!("failed to parse session file: {err}"))
}

fn save_persistent_session(
    path: &Path,
    homeserver: &str,
    session: &MatrixSession,
) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|err| format!("failed to create session directory: {err}"))?;
    }

    let data = PersistentSessionFile {
        version: 1,
        homeserver: homeserver.to_owned(),
        session: session.clone(),
    };
    let bytes = serde_json::to_vec(&data)
        .map_err(|err| format!("failed to serialize session: {err}"))?;
    let tmp = path.with_extension("json.tmp");

    let mut options = OpenOptions::new();
    options.create(true).write(true).truncate(true);
    #[cfg(unix)]
    {
        options.mode(0o600);
    }
    let mut file = options
        .open(&tmp)
        .map_err(|err| format!("failed to open session file: {err}"))?;
    file.write_all(&bytes)
        .map_err(|err| format!("failed to write session file: {err}"))?;
    file.write_all(b"\n")
        .map_err(|err| format!("failed to finish session file: {err}"))?;
    file.sync_all()
        .map_err(|err| format!("failed to flush session file: {err}"))?;
    drop(file);

    std::fs::rename(&tmp, path)
        .map_err(|err| format!("failed to install session file: {err}"))?;
    #[cfg(unix)]
    {
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))
            .map_err(|err| format!("failed to restrict session file permissions: {err}"))?;
    }
    Ok(())
}

fn install_event_handlers(client: &Client, events: Arc<Mutex<VecDeque<String>>>) {
    // Decrypted (or plaintext) room messages — the SDK dispatches this handler
    // for both. `encryption_info` is Some(...) only when the SDK decrypted the
    // payload; when Some, the event on the wire was m.room.encrypted and the
    // SDK produced usable plaintext. That distinction becomes the
    // (is_encrypted, is_decrypted) pair on the C++ side.
    //
    // The body / ciphertext boundary is enforced on the Rust side: we only
    // ever forward plaintext bodies here (decrypted or already-plaintext), and
    // we never forward ciphertext at all — the encrypted handler below emits
    // an empty body + undecryptable flag instead.
    let plaintext_events = Arc::clone(&events);
    client.add_event_handler(
        move |ev: OriginalSyncRoomMessageEvent,
              room: Room,
              encryption_info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            let events = Arc::clone(&plaintext_events);
            async move {
                let (kind, body) = match &ev.content.msgtype {
                    MessageType::Text(content) => ("text", content.body.clone()),
                    MessageType::Notice(content) => ("notice", content.body.clone()),
                    MessageType::Emote(content) => ("emote", content.body.clone()),
                    _ => return,
                };

                let is_encrypted = encryption_info.is_some();
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_event",
                        "room_id": room.room_id().to_string(),
                        "event": {
                            "event_id": ev.event_id.to_string(),
                            "sender": ev.sender.to_string(),
                            "body": body,
                            "msgtype": kind,
                            "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                            "is_encrypted": is_encrypted,
                            "is_decrypted": is_encrypted,
                            "undecryptable": false,
                            // Kept for backward compat with C++ builds that
                            // still read `decrypted` — remove after prep+6.
                            "decrypted": is_encrypted,
                        },
                    }),
                );
            }
        },
    );

    // Encrypted room messages the SDK could NOT decrypt. Without this handler
    // undecryptable events silently disappear and the room looks empty even
    // though messages are arriving. Emit a placeholder timeline event tagged
    // `undecryptable = true` so C++ can render an honest
    // "[unable to decrypt yet]" bubble. We deliberately do NOT include the
    // ciphertext in the payload — the C++ side never needs it. The
    // `error_kind` is a coarse hint the UI can display later; today we always
    // emit "no_key" because the SDK doesn't expose a finer reason on this
    // path in v0.18 without more work.
    let encrypted_events = Arc::clone(&events);
    client.add_event_handler(move |ev: OriginalSyncRoomEncryptedEvent, room: Room| {
        let events = Arc::clone(&encrypted_events);
        async move {
            enqueue(
                &events,
                json!({
                    "type": "timeline_event",
                    "room_id": room.room_id().to_string(),
                    "event": {
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        // Empty body triggers the placeholder in C++.
                        "body": "",
                        "msgtype": "encrypted",
                        "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                        "is_encrypted": true,
                        "is_decrypted": false,
                        "undecryptable": true,
                        "error_kind": "no_key",
                        // Backward compat with prep+5 C++ builds.
                        "decrypted": false,
                    },
                }),
            );
        }
    });
}

async fn enqueue_rooms(events: &Arc<Mutex<VecDeque<String>>>, client: &Client) {
    let mut out = Vec::new();
    for room in client.joined_rooms() {
        let name = room_name(&room).await;
        let encrypted = room.encryption_state().is_encrypted();
        out.push(json!({
            "id": room.room_id().to_string(),
            "name": name,
            "topic": room.topic().unwrap_or_default(),
            "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
            "last_message_preview": "",
            "last_activity_ms": room
                .latest_event_timestamp()
                .map(|ts| u64::from(ts.get()))
                .unwrap_or(0),
            "unread_count": room.num_unread_notifications(),
            "encrypted": encrypted,
            "is_space": room.is_space(),
            "prev_batch": room.last_prev_batch().unwrap_or_default(),
            "child_room_ids": [],
        }));
    }

    enqueue(events, json!({ "type": "rooms", "rooms": out }));
}

async fn room_name(room: &Room) -> String {
    if let Some(name) = room.name() {
        if !name.is_empty() {
            return name.to_owned();
        }
    }

    match room.display_name().await {
        Ok(matrix_sdk::RoomDisplayName::Named(name))
        | Ok(matrix_sdk::RoomDisplayName::Aliased(name))
        | Ok(matrix_sdk::RoomDisplayName::Calculated(name))
        | Ok(matrix_sdk::RoomDisplayName::EmptyWas(name)) if !name.is_empty() => name,
        _ => room.room_id().to_string(),
    }
}

/// Upper bound on the FFI event queue so we never grow without limit if the
/// C++ poll timer stalls (e.g. UI thread stuck under load). When we hit the
/// cap we drop the OLDEST events and inject a single `queue_overflow`
/// notice so the C++ side can log/surface the loss. The cap is generous —
/// well above the burst of `rooms` + `timeline_event` we produce on
/// initial sync of a real account.
const EVENT_QUEUE_CAP: usize = 4096;

fn enqueue(events: &Arc<Mutex<VecDeque<String>>>, value: serde_json::Value) {
    let Ok(serialized) = serde_json::to_string(&value) else {
        return;
    };
    if let Ok(mut guard) = events.lock() {
        if guard.len() >= EVENT_QUEUE_CAP {
            // Drop oldest to make room. If the previous entry we just
            // dropped was itself the overflow marker, don't spam another.
            let dropped_marker = matches!(
                guard.front().map(|s| s.contains("\"queue_overflow\"")),
                Some(true),
            );
            guard.pop_front();
            if !dropped_marker {
                // Reserve one slot for the marker + the new event.
                if guard.len() >= EVENT_QUEUE_CAP {
                    guard.pop_front();
                }
                if let Ok(marker) = serde_json::to_string(&json!({
                    "type": "queue_overflow",
                    "message": "Rust SDK event queue dropped oldest events; C++ poll may have stalled.",
                })) {
                    guard.push_back(marker);
                }
            }
        }
        guard.push_back(serialized);
    }
}

fn run_async<F>(events: Arc<Mutex<VecDeque<String>>>, label: &'static str, future: F)
where
    F: std::future::Future<Output = ()>,
{
    let result = catch_unwind(AssertUnwindSafe(|| {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|err| format!("failed to create Tokio runtime for {label}: {err}"))?;
        runtime.block_on(future);
        Ok::<(), String>(())
    }));

    match result {
        Ok(Ok(())) => {}
        Ok(Err(message)) => enqueue(
            &events,
            json!({ "type": "error", "message": message }),
        ),
        Err(_) => enqueue(
            &events,
            json!({ "type": "error", "message": format!("Rust SDK {label} task panicked.") }),
        ),
    }
}

unsafe fn bridge<'a>(ptr: *mut c_void) -> Result<&'a RustClient, String> {
    if ptr.is_null() {
        return Err("Rust SDK backend handle is null.".to_owned());
    }
    Ok(unsafe { &*(ptr as *mut RustClient) })
}

unsafe fn cstr_arg(ptr: *const c_char) -> Result<String, String> {
    if ptr.is_null() {
        return Err("null string argument passed to Rust SDK FFI.".to_owned());
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map(str::to_owned)
        .map_err(|err| format!("invalid UTF-8 string passed to Rust SDK FFI: {err}"))
}

fn ffi_string(body: impl FnOnce() -> Result<String, String>) -> *mut c_char {
    let value = match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(err)) => format!("error: {err}"),
        Err(_) => "error: Rust SDK FFI panic was caught.".to_owned(),
    };
    to_c_string(&value)
}

fn to_c_string(s: &str) -> *mut c_char {
    let sanitized = s.replace('\0', "");
    match CString::new(sanitized) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

fn format_matrix_error(context: &str, err: impl std::fmt::Display) -> String {
    format!("{context}: {err}")
}
