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

char *mx_rust_login(void *client,
                    const char *homeserver,
                    const char *user,
                    const char *password);
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

/* 0 = no, 1 = yes. Reports honestly — 0 until verified encrypted read/send. */
int mx_rust_supports_e2ee(void *client);

void mx_rust_free_cstring(char *ptr);

#ifdef __cplusplus
}
#endif
