//! A retired bridge must leave nothing of its account store open.
//!
//! GitHub #2 (Windows, 2026-10-07): after a restart, Sign out ended in "The
//! repair didn't finish". `mx_rust_destroy` returned in 1 ms, C++ waited for
//! it and then could not delete one file of the store: every SQLite database
//! (state, event cache, crypto, media, with their -wal/-shm) stayed open until
//! the PROCESS exited. Linux hides that, because an open file can be unlinked,
//! so these tests ask the kernel directly: once `mx_rust_destroy` has
//! returned, no descriptor of this process may point into the store.
//!
//! They drive the bridge through the same FFI calls, in the same order, as
//! `RustSdkMatrixClient` does: create, set the media-store key, restore (which installs the event
//! handlers), optionally sync, then sign-out (`mx_rust_logout`, then the
//! retirement worker's `mx_rust_shutdown_tasks` and `mx_rust_destroy`) or a
//! plain account switch (no logout: the Client is still in the bridge's slot
//! when it is destroyed).
//!
//! Linux only: the evidence is `/proc/self/fd`.

use std::ffi::{c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

unsafe fn take(ptr: *mut std::ffi::c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
    unsafe { super::mx_rust_free_cstring(ptr) };
    s
}

/// Drain bridge events until `pred` matches; panics after `timeout`.
unsafe fn wait_for(
    handle: *mut c_void,
    what: &str,
    timeout: Duration,
    mut pred: impl FnMut(&serde_json::Value) -> bool,
) -> serde_json::Value {
    let deadline = Instant::now() + timeout;
    let mut seen = Vec::new();
    loop {
        loop {
            let raw = unsafe { take(super::mx_rust_poll_event(handle)) };
            if raw.is_empty() {
                break;
            }
            let Ok(ev) = serde_json::from_str::<serde_json::Value>(&raw) else {
                continue;
            };
            if pred(&ev) {
                return ev;
            }
            seen.push(ev["type"].as_str().unwrap_or("?").to_owned());
        }
        assert!(
            Instant::now() < deadline,
            "timed out waiting for {what}; saw event types {seen:?}"
        );
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// A homeserver that accepts the client (`/versions`) and refuses every other
/// call with a Matrix 404, so sign-in, crypto setup and sync all run and all
/// fail fast, which is what a server that has just forgotten us looks like.
fn serve() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
    let addr = listener.local_addr().expect("addr");
    std::thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(mut stream) = stream else { break };
            std::thread::spawn(move || {
                let mut buf = [0u8; 4096];
                let read = stream.read(&mut buf).unwrap_or(0);
                let request = String::from_utf8_lossy(&buf[..read]);
                let (status, body) = if request.starts_with("GET /_matrix/client/versions") {
                    (
                        "200 OK",
                        r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#,
                    )
                } else {
                    (
                        "404 Not Found",
                        r#"{"errcode":"M_UNRECOGNIZED","error":"Unrecognized request"}"#,
                    )
                };
                let response = format!(
                    "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
                     Connection: close\r\nContent-Length: {}\r\n\r\n{body}",
                    body.len()
                );
                let _ = stream.write_all(response.as_bytes());
                let _ = stream.flush();
            });
        }
    });
    format!("http://{}:{}", addr.ip(), addr.port())
}

/// A per-test store directory, removed on drop.
struct StoreDir(PathBuf);

impl StoreDir {
    fn new(name: &str) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "lightning-storeclose-{name}-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).expect("store dir");
        Self(std::fs::canonicalize(&path).unwrap_or(path))
    }
}

impl Drop for StoreDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// What this process still has open inside `dir`, by file name only.
fn open_files_under(dir: &Path) -> Vec<String> {
    let mut open = Vec::new();
    let Ok(entries) = std::fs::read_dir("/proc/self/fd") else {
        panic!("/proc/self/fd is not readable; this test cannot see anything");
    };
    for entry in entries.flatten() {
        if let Ok(target) = std::fs::read_link(entry.path()) {
            if target.starts_with(dir) {
                open.push(
                    target
                        .strip_prefix(dir)
                        .map(|p| p.display().to_string())
                        .unwrap_or_default(),
                );
            }
        }
    }
    open.sort();
    open
}

/// A bridge restored into `store` against `homeserver`, exactly as
/// `RustSdkMatrixClient` restores a saved account at start-up.
unsafe fn restored_bridge(store: &StoreDir, homeserver: &str) -> *mut c_void {
    let path = CString::new(store.0.to_string_lossy().as_bytes()).unwrap();
    let handle = super::mx_rust_create(path.as_ptr());
    assert!(!handle.is_null(), "bridge handle");
    // A media-store key, as C++ sets one from the keyring before every sign-in,
    // so the encrypted media store (`lightning-media-store/`, the fourth set of
    // files the Windows report found open) is opened too. Fixture bytes.
    let key = [0x42u8; 32];
    let set = unsafe {
        take(super::mx_rust_set_media_store_key(handle, key.as_ptr(), 32, 1))
    };
    assert!(set.is_empty(), "media key refused: {set}");
    let hs = CString::new(homeserver).unwrap();
    let user = CString::new("@storeclose:localhost").unwrap();
    let device = CString::new("STORECLOSE").unwrap();
    let token = CString::new("fixture-access-token").unwrap();
    let refresh = CString::new("").unwrap();
    let started = unsafe {
        take(super::mx_rust_restore(
            handle,
            hs.as_ptr(),
            user.as_ptr(),
            device.as_ptr(),
            token.as_ptr(),
            refresh.as_ptr(),
        ))
    };
    assert!(started.is_empty(), "restore refused to start: {started}");
    unsafe {
        wait_for(handle, "login_ok", Duration::from_secs(30), |ev| {
            assert_ne!(ev["type"], "login_failed", "restore failed: {ev}");
            ev["type"] == "login_ok"
        })
    };
    let open = open_files_under(&store.0);
    for expected in [
        "matrix-sdk-state.sqlite3",
        "matrix-sdk-event-cache.sqlite3",
        "matrix-sdk-crypto.sqlite3",
        "lightning-media-store/matrix-sdk-media.sqlite3",
    ] {
        assert!(
            open.iter().any(|f| f == expected),
            "the restored session does not hold {expected} open, so the test \
             cannot see it being released: {open:?}"
        );
    }
    handle
}

/// The retirement worker's half (`retireRustHandleAsync`), then the check C++
/// cannot make on Windows until it tries to delete.
unsafe fn retire_and_assert_closed(handle: *mut c_void, store: &StoreDir, what: &str) {
    let _ = unsafe { take(super::mx_rust_shutdown_tasks(handle)) };
    let _ = unsafe { take(super::mx_rust_final_session_tokens(handle)) };
    // What the C++ retirement worker calls.
    let report = unsafe { take(super::mx_rust_destroy_and_report(handle)) };
    let open = open_files_under(&store.0);
    assert!(
        // `builds=0` is fine too: after a logout the last reference can be
        // gone before retirement even starts (the logout thread dropped it).
        report.starts_with("store_closed=true "),
        "{what}: the retirement report does not say the store was released: {report}"
    );
    assert!(
        open.is_empty(),
        "{what}: mx_rust_destroy returned with the account store still open \
         (on Windows the store cannot be deleted): {open:?}"
    );
}

/// Sign-out: the server logout, then retirement.
#[test]
fn sign_out_after_a_restore_leaves_no_store_file_open() {
    let homeserver = serve();
    let store = StoreDir::new("signout");
    unsafe {
        let handle = restored_bridge(&store, &homeserver);
        super::mx_rust_logout(handle);
        wait_for(handle, "logged_out", Duration::from_secs(30), |ev| {
            ev["type"] == "logged_out"
        });
        retire_and_assert_closed(handle, &store, "sign-out");
    }
}

/// An account switch: no logout, so the bridge still holds its Client when it
/// is destroyed. Dropping that last reference outside a Tokio runtime would
/// also make the SQLite pool's own close panic (deadpool spawns it as a
/// blocking task), so this proves the close runs, and runs in time.
#[test]
fn an_account_switch_after_a_restore_leaves_no_store_file_open() {
    let homeserver = serve();
    let store = StoreDir::new("switch");
    unsafe {
        let handle = restored_bridge(&store, &homeserver);
        retire_and_assert_closed(handle, &store, "account switch");
    }
}

/// The same with the sync loop running, as it always is in the app: its
/// thread, the room-list service and the sync-scoped handlers must all be gone
/// too.
#[test]
fn sign_out_while_syncing_leaves_no_store_file_open() {
    let homeserver = serve();
    let store = StoreDir::new("syncing");
    unsafe {
        let handle = restored_bridge(&store, &homeserver);
        super::mx_rust_start_sync(handle);
        // Long enough for the sync thread to build its services and fail at
        // least one request against the 404 server.
        std::thread::sleep(Duration::from_millis(1500));
        super::mx_rust_logout(handle);
        wait_for(handle, "logged_out", Duration::from_secs(30), |ev| {
            ev["type"] == "logged_out"
        });
        retire_and_assert_closed(handle, &store, "sign-out while syncing");
    }
}

/// A handle that never built a client (the GUI thread destroys one when its
/// session file is refused) has nothing to wait for, even while another
/// handle for the same account directory holds a live session there.
#[test]
fn a_handle_that_never_signed_in_does_not_wait_for_another_handles_stores() {
    let homeserver = serve();
    let store = StoreDir::new("never-signed-in");
    unsafe {
        let live = restored_bridge(&store, &homeserver);
        let path = CString::new(store.0.to_string_lossy().as_bytes()).unwrap();
        let idle = super::mx_rust_create(path.as_ptr());
        assert!(!idle.is_null(), "second handle");
        let started = Instant::now();
        let report = take(super::mx_rust_destroy_and_report(idle));
        let took = started.elapsed();
        assert!(
            report.starts_with("store_closed=true ") && report.contains(" builds=0 "),
            "the idle handle waited for someone else's stores: {report}"
        );
        assert!(
            took < Duration::from_millis(900),
            "the idle handle's retirement waited {took:?}"
        );
        // The live session's stores were not touched by that.
        assert!(!open_files_under(&store.0).is_empty());
        retire_and_assert_closed(live, &store, "the live handle after the idle one");
    }
}
