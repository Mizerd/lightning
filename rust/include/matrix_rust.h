/*
 * matrix_rust.h — hand-authored C ABI for Lightning's Matrix Rust SDK bridge.
 *
 * Ownership contract: every char* returned by an mx_rust_* function was
 * heap-allocated by the Rust side (CString::into_raw). The caller must
 * release it exclusively via mx_rust_free_cstring — do NOT call free().
 *
 * String-returning command functions return "" when the command was accepted
 * for async execution, or "error: …" for immediate argument / handle errors.
 * Results arrive later through mx_rust_poll_event() as compact JSON.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

char *mx_rust_backend_name(void);
char *mx_rust_status_string(void);
char *mx_rust_version(void);

void *mx_rust_create(const char *store_path);
void  mx_rust_destroy(void *client);

/*
 * Optional smoke-only MatrixSession sidecar. This lets
 * LIGHTNING_TEST_PERSISTENT_STORE=1 restore the same SDK device without
 * writing to the interactive QSettings/SecretStore session. The file contains
 * an access token, so callers must never print it and must keep it outside
 * the repository.
 */
char *mx_rust_set_session_file(void *client, const char *session_file_path);

char *mx_rust_login(void *client,
                    const char *homeserver,
                    const char *user,
                    const char *password);
char *mx_rust_restore_from_file(void *client,
                                const char *homeserver,
                                const char *expected_user_id);
char *mx_rust_restore(void *client,
                      const char *homeserver,
                      const char *user_id,
                      const char *device_id,
                      const char *access_token);
void  mx_rust_logout(void *client);

void  mx_rust_start_sync(void *client);
void  mx_rust_stop_sync(void *client);
char *mx_rust_poll_event(void *client);

char *mx_rust_send_text(void *client,
                        const char *room_id,
                        const char *body,
                        const char *transaction_id);

/*
 * Encrypted-room test probe (v0.5.0-prep+6). Only accepts encrypted rooms;
 * refuses non-encrypted rooms with a send_failed event. Reserved for the
 * headless smoke harness while E2EE is being verified — the interactive UI
 * still goes through mx_rust_send_text and is gated by C++
 * CryptoManager::supportsE2ee(). matrix-sdk performs the encryption
 * end-to-end via its e2e-encryption feature; this FFI never sees keys or
 * ciphertext. Results arrive later as `encrypted_send_ok` /
 * `encrypted_send_failed` events on the poll queue.
 */
char *mx_rust_probe_encrypted_send(void *client,
                                   const char *room_id,
                                   const char *body,
                                   const char *transaction_id);

/*
 * Key-backup recovery probe (v0.5.0-prep+7). Calls
 * matrix-sdk's client.encryption().recovery().recover(<key>) so the SDK
 * imports backed-up room keys from server-side secret storage. The FFI
 * never logs the recovery key. Result events land on the poll queue as
 *   {"type":"key_backup_status","state":"attempted|ok|failed",...}
 */
char *mx_rust_recover_from_backup(void *client,
                                  const char *recovery_key);

/* 0 = no, 1 = yes. Reports honestly — 0 until verified encrypted read/send. */
int mx_rust_supports_e2ee(void *client);

void mx_rust_free_cstring(char *ptr);

#ifdef __cplusplus
}
#endif
