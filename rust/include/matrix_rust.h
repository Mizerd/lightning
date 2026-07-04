/*
 * matrix_rust.h — hand-authored C ABI for the v0.4 Rust scaffold.
 *
 * When the Rust surface grows (matrix-sdk login/sync/timeline/send/crypto),
 * replace this file with cbindgen-generated output. For v0.4 the surface is
 * small enough that hand-authoring is clearer than an extra build tool.
 *
 * Ownership contract: every char* returned by an mx_rust_* function was
 * heap-allocated by the Rust side (CString::into_raw). The caller must
 * release it exclusively via mx_rust_free_cstring — do NOT call free().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Backend identifier, e.g. "matrix-rust-sdk (scaffold)". */
char *mx_rust_backend_name(void);

/* Human-readable status string. */
char *mx_rust_status_string(void);

/* 0 = no, 1 = yes. Reports honestly — 0 until real SDK crypto is wired. */
int mx_rust_supports_e2ee(void);

/* Semver of the Rust crate. */
char *mx_rust_version(void);

/* Release any char* returned by the accessors above. Safe to call with NULL. */
void mx_rust_free_cstring(char *ptr);

#ifdef __cplusplus
}
#endif
