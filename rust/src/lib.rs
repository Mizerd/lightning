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
    encryption::verification::{SasState, SasVerification, VerificationRequest},
    room::MessagesOptions,
    ruma::{
        events::{
            key::verification::request::ToDeviceKeyVerificationRequestEvent,
            room::{
                encrypted::OriginalSyncRoomEncryptedEvent,
                message::{MessageType, OriginalSyncRoomMessageEvent, RoomMessageEventContent},
            },
        },
        uint, OwnedDeviceId, OwnedTransactionId, OwnedUserId, RoomId, UInt, UserId,
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
    // v0.5.0: SAS verification state. Single active flow at a time keeps
    // the FFI simple; if a second request arrives we cancel the first.
    // Both slots are cleared when the flow reaches Done or Cancelled.
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas) — SasVerification has no flow_id() accessor on
    // matrix-sdk 0.18, so we track it externally.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
}

impl RustClient {
    fn new(store_path: PathBuf) -> Result<Self, String> {
        Ok(Self {
            store_path,
            session_file: Arc::new(Mutex::new(None)),
            client: Arc::new(Mutex::new(None)),
            events: Arc::new(Mutex::new(VecDeque::new())),
            sync_stop: Arc::new(Mutex::new(None)),
            active_request: Arc::new(Mutex::new(None)),
            active_sas: Arc::new(Mutex::new(None)),
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
        Ok("Matrix Rust SDK backend linked. Initial E2EE support enabled via matrix-sdk (v0.5.0-prep+9): password login, session restore, joined-room sync, plain and encrypted text send, and receive of both plain and SDK-decrypted encrypted messages all go through the SDK event queue. No SAS emoji UI, no GUI recovery-key flow, no cross-signing UI yet.".to_owned())
    })
}

// v0.5.0-prep+9: initial E2EE support gate. The FFI reports 1 because
// matrix-sdk 0.18 is compiled in with the `e2e-encryption` feature and
// the encrypted receive + encrypted send paths were both verified live
// against `matrix.smetonis.net` (Element Classic ↔ Lightning marker
// round-trip). The C++ CryptoManager gate remains the source of truth
// for the UI; this only unblocks the `sendTextMessage` refusal inside
// RustSdkMatrixClient when the interactive user is sending into an
// encrypted room.
#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee(_client: *mut c_void) -> c_int {
    1
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
        let active_request = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "login", async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
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
        let active_request = Arc::clone(&bridge.active_request);
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
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
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
        let active_request = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "restore", async move {
                match restore_client(&homeserver, &store_path, &user_id, &device_id, access_token)
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
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

                // v0.5.0-prep+9: encrypted rooms are now allowed on the
                // interactive UI send path. matrix-sdk auto-encrypts via
                // its `e2e-encryption` feature when the room's
                // encryption state says so; if it can't (missing keys,
                // untrusted target device, etc.) the SDK returns an
                // error that flows back through send_failed. The C++
                // side still refuses when CryptoManager::supportsE2ee()
                // is false — see RustSdkMatrixClient::sendTextMessage.
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

/// Timeline reload for a specific room via matrix-sdk 0.18 Room::messages.
/// Emits the same `timeline_event` shape the live event handlers use, so the
/// C++ side dedupes by event_id automatically. Body plaintext is only
/// forwarded when the SDK decrypted the event (or when it was never
/// encrypted); ciphertext is never forwarded — undecryptable rows emit an
/// empty body + undecryptable=true, exactly like the live path.
///
/// Also emits a summary event on the poll queue:
///   { "type": "reload_timeline_done",
///     "room_id": "...", "events": N, "decrypted": N, "undecryptable": N }
///   { "type": "reload_timeline_failed", "room_id": "...", "message": "..." }
#[no_mangle]
pub unsafe extern "C" fn mx_rust_reload_room_timeline(
    ptr: *mut c_void,
    room_id: *const c_char,
    limit: u32,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "reload_timeline", async move {
                let Ok(room_ref) = RoomId::parse(&room_id) else {
                    enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": "Invalid Matrix room id.",
                    }));
                    return;
                };
                let Some(room) = client.get_room(&room_ref) else {
                    enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": "Rust SDK does not know that room yet.",
                    }));
                    return;
                };

                let mut opts = MessagesOptions::backward();
                // Clamp limit to a sane range; matrix-sdk's default is 10.
                let clamped: u64 = if limit == 0 { 30 } else {
                    std::cmp::min(limit as u64, 200)
                };
                opts.limit = UInt::new(clamped).unwrap_or(uint!(30));

                match room.messages(opts).await {
                    Ok(messages) => {
                        let mut total = 0u32;
                        let mut decrypted = 0u32;
                        let mut undecryptable_count = 0u32;
                        // messages.chunk is newest-first for backward();
                        // reverse so C++ inserts oldest-first, matching
                        // the live sync ordering.
                        for ev in messages.chunk.into_iter().rev() {
                            total += 1;
                            let event_id = ev
                                .event_id()
                                .map(|i| i.to_string())
                                .unwrap_or_default();
                            let sender = ev
                                .sender()
                                .map(|i| i.to_string())
                                .unwrap_or_default();
                            if event_id.is_empty() {
                                continue;
                            }
                            let is_decrypted = ev.encryption_info().is_some();

                            let raw = ev.raw();
                            let value: serde_json::Value = match serde_json::from_str(
                                raw.json().get(),
                            ) {
                                Ok(v) => v,
                                Err(_) => continue,
                            };
                            let ts_ms = value
                                .get("origin_server_ts")
                                .and_then(|v| v.as_u64())
                                .unwrap_or(0);
                            let type_str = value
                                .get("type")
                                .and_then(|v| v.as_str())
                                .unwrap_or("");
                            let content = value.get("content");

                            let (is_encrypted, undecryptable, msgtype, body) =
                                if type_str == "m.room.encrypted" {
                                    undecryptable_count += 1;
                                    (true, true, "encrypted".to_owned(), String::new())
                                } else if type_str == "m.room.message" {
                                    let mt = content
                                        .and_then(|c| c.get("msgtype"))
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("m.text");
                                    let bd = content
                                        .and_then(|c| c.get("body"))
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("")
                                        .to_owned();
                                    let kind = match mt {
                                        "m.notice" => "notice",
                                        "m.emote" => "emote",
                                        _ => "text",
                                    }
                                    .to_owned();
                                    if is_decrypted {
                                        decrypted += 1;
                                    }
                                    (is_decrypted, false, kind, bd)
                                } else {
                                    // Skip state / other event types on
                                    // this path — the live sync handles
                                    // room state separately.
                                    continue;
                                };

                            enqueue(&events, json!({
                                "type": "timeline_event",
                                "room_id": room_id.clone(),
                                "event": {
                                    "event_id": event_id,
                                    "sender": sender,
                                    "body": body,
                                    "msgtype": msgtype,
                                    "timestamp_ms": ts_ms,
                                    "is_encrypted": is_encrypted,
                                    "is_decrypted": is_decrypted,
                                    "undecryptable": undecryptable,
                                    "decrypted": is_decrypted,
                                }
                            }));
                        }
                        enqueue(&events, json!({
                            "type": "reload_timeline_done",
                            "room_id": room_id,
                            "events": total,
                            "decrypted": decrypted,
                            "undecryptable": undecryptable_count,
                        }));
                    }
                    Err(err) => enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": format_matrix_error(
                            "Matrix Rust SDK Room::messages failed", err),
                    })),
                }
            });
        });
        Ok(String::new())
    })
}

/// SAS emoji verification — accept an incoming request and drive it to
/// the KeysExchanged state (v0.5.0). Emits `verification_sas_ready` with
/// the emoji list when the SDK finishes key exchange; `verification_done`
/// / `verification_cancelled` when the flow reaches a terminal state.
///
/// Poll-based state watching (500 ms cadence, 120 s cap) is used
/// deliberately instead of a futures Stream so we don't introduce a
/// direct futures-util dep — matrix-sdk exposes state() as a snapshot.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_accept_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;

        let request = match bridge.active_request.lock() {
            Ok(g) => g.clone(),
            Err(_) => None,
        };
        let Some(request) = request else {
            return Ok("error: no active verification request.".to_owned());
        };
        if request.flow_id() != flow_id {
            return Ok("error: verification flow id mismatch.".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "verification_accept", async move {
                if let Err(err) = request.accept().await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": request.flow_id().to_string(),
                        "message": format_matrix_error(
                            "Matrix Rust SDK verification accept failed", err),
                    }));
                    return;
                }
                enqueue(&events, json!({
                    "type": "verification_ready",
                    "flow_id": request.flow_id().to_string(),
                }));

                // Wait for the request to transition into SasV1; poll
                // for up to 60s. matrix-sdk drives the handshake
                // automatically after accept().
                let sas: Option<SasVerification> = {
                    let mut sas: Option<SasVerification> = None;
                    for _ in 0..120 {
                        tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                        if let Some(v) = client
                            .encryption()
                            .get_verification(request.other_user_id(),
                                              request.flow_id())
                            .await
                        {
                            if let matrix_sdk::encryption::verification::Verification::SasV1(s) = v {
                                sas = Some(s);
                                break;
                            }
                        }
                        if request.is_cancelled() {
                            break;
                        }
                    }
                    sas
                };
                let Some(sas) = sas else {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": request.flow_id().to_string(),
                        "message": "Timed out waiting for SAS handshake.",
                    }));
                    return;
                };
                // Start the SAS flow with the SDK's default settings.
                let sas_flow_id = request.flow_id().to_string();
                if let Err(err) = sas.accept().await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": sas_flow_id,
                        "message": format_matrix_error(
                            "SAS accept failed", err),
                    }));
                    return;
                }
                if let Ok(mut g) = sas_slot.lock() {
                    *g = Some((sas_flow_id.clone(), sas.clone()));
                }
                enqueue(&events, json!({
                    "type": "verification_sas_started",
                    "flow_id": sas_flow_id,
                }));

                // Poll state for up to 120 s waiting for KeysExchanged /
                // Done / Cancelled. matrix-sdk emits emojis at
                // KeysExchanged.
                let mut emitted_emojis = false;
                for _ in 0..240 {
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    let state = sas.state();
                    match state {
                        SasState::KeysExchanged { .. } if !emitted_emojis => {
                            emitted_emojis = true;
                            let mut emoji_list: Vec<serde_json::Value> = Vec::new();
                            if let Some(emojis) = sas.emoji() {
                                for e in emojis.iter() {
                                    emoji_list.push(json!({
                                        "symbol": e.symbol,
                                        "description": e.description,
                                    }));
                                }
                            }
                            let dec = sas.decimals().map(|(a, b, c)| json!([a, b, c]))
                                .unwrap_or(json!([]));
                            enqueue(&events, json!({
                                "type": "verification_sas_ready",
                                "flow_id": sas_flow_id,
                                "emojis": emoji_list,
                                "decimals": dec,
                            }));
                        }
                        SasState::Done { .. } => {
                            enqueue(&events, json!({
                                "type": "verification_done",
                                "flow_id": sas_flow_id,
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        SasState::Cancelled(info) => {
                            enqueue(&events, json!({
                                "type": "verification_cancelled",
                                "flow_id": sas_flow_id,
                                "message": format!("{:?}", info.reason()),
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        _ => {}
                    }
                }
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": sas_flow_id,
                    "message": "Timed out waiting for SAS completion.",
                }));
                if let Ok(mut g) = sas_slot.lock() { *g = None; }
                if let Ok(mut g) = request_slot.lock() { *g = None; }
            });
        });
        Ok(String::new())
    })
}

fn ffi_sas_action(
    ptr: *mut c_void,
    flow_id: *const c_char,
    label: &'static str,
    action: fn(SasVerification)
        -> std::pin::Pin<Box<dyn std::future::Future<Output = matrix_sdk::Result<()>> + Send>>,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let entry = match bridge.active_sas.lock() {
            Ok(g) => g.clone(),
            Err(_) => None,
        };
        let Some((stored_flow, sas)) = entry else {
            return Ok("error: no active SAS verification.".to_owned());
        };
        if stored_flow != flow_id {
            return Ok("error: SAS verification flow id mismatch.".to_owned());
        }
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, label, async move {
                if let Err(err) = action(sas.clone()).await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": stored_flow,
                        "message": format_matrix_error(label, err),
                    }));
                }
            });
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_confirm_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_sas_action(ptr, flow_id, "verification_confirm", |sas| Box::pin(async move {
        sas.confirm().await
    }))
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_mismatch_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_sas_action(ptr, flow_id, "verification_mismatch", |sas| Box::pin(async move {
        sas.mismatch().await
    }))
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_cancel_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    // Try SAS-level cancel first; if no active SAS, fall back to request-level.
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let sas_entry = bridge.active_sas.lock().ok().and_then(|g| g.clone());
        let request = bridge.active_request.lock().ok().and_then(|g| g.clone());
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "verification_cancel", async move {
                if let Some((stored_flow, sas)) = sas_entry {
                    if stored_flow == flow_id {
                        let _ = sas.cancel().await;
                        enqueue(&events, json!({
                            "type": "verification_cancelled",
                            "flow_id": flow_id,
                            "message": "cancelled",
                        }));
                    }
                } else if let Some(request) = request {
                    if request.flow_id() == flow_id {
                        let _ = request.cancel().await;
                        enqueue(&events, json!({
                            "type": "verification_cancelled",
                            "flow_id": flow_id,
                            "message": "cancelled",
                        }));
                    }
                }
                if let Ok(mut g) = sas_slot.lock() { *g = None; }
                if let Ok(mut g) = request_slot.lock() { *g = None; }
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

fn install_event_handlers(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
) {
    // v0.5.0: SAS emoji verification, receive-first. matrix-sdk 0.18 does
    // NOT expose a public `recv_verification_requests` stream, so we
    // observe incoming requests via a to-device event handler and then
    // hydrate the `VerificationRequest` via
    // `client.encryption().get_verification_request(user, flow_id)`.
    //
    // The active flow (single-flow policy) is stored in
    // `active_request`; the FFI accept path drives `accept()` +
    // `start_sas()` from that stored handle. No secret material is
    // ever forwarded through the FFI — only flow id, mxid, device id,
    // is_self_verification, and SAS emojis (which are safe to display
    // by SAS design).
    let verif_events = Arc::clone(&events);
    let verif_slot = Arc::clone(&active_request);
    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: ToDeviceKeyVerificationRequestEvent| {
            let events = Arc::clone(&verif_events);
            let slot = Arc::clone(&verif_slot);
            let client = client_clone.clone();
            async move {
                let flow_id = ev.content.transaction_id.to_string();
                let Some(request) = client
                    .encryption()
                    .get_verification_request(&ev.sender, &flow_id)
                    .await
                else {
                    return;
                };
                let other_device = request
                    .their_supported_methods()
                    .map(|_| String::new())
                    .unwrap_or_default();
                enqueue(
                    &events,
                    json!({
                        "type": "verification_request_received",
                        "flow_id": request.flow_id().to_string(),
                        "other_user_id": request.other_user_id().to_string(),
                        "other_device_id": other_device,
                        "is_self_verification": request.is_self_verification(),
                        "we_started": request.we_started(),
                    }),
                );
                if let Ok(mut g) = slot.lock() {
                    *g = Some(request);
                }
            }
        },
    );
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
