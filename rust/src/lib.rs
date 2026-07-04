//! matrix-client-rust — v0.4 FFI scaffold.
//!
//! Exposes a minimal C ABI that the C++ RustSdkMatrixClient calls to prove
//! the link, report backend identity, and (later) drive login/sync via the
//! Matrix Rust SDK. Kept intentionally tiny in v0.4:
//!
//! * `mx_rust_backend_name`   -> "matrix-rust-sdk (scaffold)".
//! * `mx_rust_status_string`  -> "compiled but not feature complete".
//! * `mx_rust_supports_e2ee`  -> 0 (until real SDK is wired).
//! * `mx_rust_free_cstring`   -> deallocate strings returned across FFI.
//!
//! The hand-written C header is at rust/include/matrix_rust.h. When we
//! start adding calls (login, sync, timeline, send) we will introduce
//! cbindgen or cxx to keep the surface consistent.

use std::ffi::{c_char, CString};
use std::os::raw::c_int;

/// Return a heap-allocated null-terminated UTF-8 string that names the
/// backend. Caller MUST release it via `mx_rust_free_cstring`.
///
/// # Safety
/// The returned pointer is either null (allocation failure) or a valid
/// pointer into memory that only `mx_rust_free_cstring` may release.
#[no_mangle]
pub extern "C" fn mx_rust_backend_name() -> *mut c_char {
    to_c_string("matrix-rust-sdk (scaffold)")
}

/// Return a heap-allocated status string. Caller frees with
/// `mx_rust_free_cstring`.
///
/// # Safety
/// Same ownership rules as `mx_rust_backend_name`.
#[no_mangle]
pub extern "C" fn mx_rust_status_string() -> *mut c_char {
    to_c_string(
        "Matrix Rust SDK backend scaffold compiled and linked. Login, sync, \
         and E2EE are not wired in v0.4 — this reports capability honestly \
         so the UI does not claim features that do not exist.",
    )
}

/// 0 = no, 1 = yes. Always 0 in v0.4 — real crypto is a follow-up.
#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee() -> c_int {
    0
}

/// Semver of this scaffold crate. Owned by the callee.
#[no_mangle]
pub extern "C" fn mx_rust_version() -> *mut c_char {
    to_c_string(env!("CARGO_PKG_VERSION"))
}

/// Deallocate a C string previously handed out by any of the accessors
/// above.
///
/// # Safety
/// `ptr` must either be null or a pointer previously returned by one of
/// this crate's FFI functions. Passing anything else is undefined
/// behaviour.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_free_cstring(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    // SAFETY: pointer was produced by CString::into_raw on this same
    // allocator, per the contract above.
    unsafe { drop(CString::from_raw(ptr)); }
}

fn to_c_string(s: &str) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}
