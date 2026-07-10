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
/* Cancels and joins the owned sync task. 1 = stopped, 0 = already stopped. */
int   mx_rust_stop_sync(void *client);
char *mx_rust_poll_event(void *client);

/* 0.5.8 Matrix-native room state commands. Results are asynchronous events;
 * no composer text, raw events, or secret material crosses these calls. */
char *mx_rust_send_typing(void *client, const char *room_id, int typing);
char *mx_rust_send_read_receipt(void *client,
                                const char *room_id,
                                const char *event_id);
char *mx_rust_set_marked_unread(void *client, const char *room_id, int unread);
char *mx_rust_accept_invite(void *client, const char *room_id);
char *mx_rust_reject_invite(void *client, const char *room_id);
/* Re-emit a full room_list_reset from the SDK's current room set. Safety
 * net invoked by C++ only when it rejects a malformed room-list diff. */
char *mx_rust_resync_rooms(void *client);

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

/*
 * Reload a room's recent timeline via matrix-sdk's Room::messages
 * (v0.5.0-prep+11). Emits the same `timeline_event` shape live sync
 * uses, so the C++ side dedupes by event_id automatically.
 * `limit` is clamped to [1, 200]; 0 uses the default of 30.
 * Result events on the poll queue:
 *   {"type":"reload_timeline_done", "room_id":"...", "events":N,
 *    "decrypted":N, "undecryptable":N}
 *   {"type":"reload_timeline_failed", "room_id":"...", "message":"..."}
 */
char *mx_rust_reload_room_timeline(void *client,
                                   const char *room_id,
                                   unsigned int limit);

/*
 * SAS emoji verification (v0.5.0). Receive-first flow: the Rust bridge
 * installs a to-device verification-request handler at
 * install_event_handlers time and emits `verification_request_received`
 * for incoming requests. From C++:
 *   accept   -> drive request -> SAS handshake, emit `verification_sas_ready`
 *              with the 7 emojis + decimals when ready.
 *   confirm  -> user says "they match".
 *   mismatch -> user says "they do not match".
 *   cancel   -> either SAS-level or request-level cancel, depending on
 *              where the flow currently is.
 * Only one active flow at a time (single-flow policy). All flow ids are
 * safe to log; emojis are also safe (SAS design). No secrets are ever
 * forwarded through this FFI.
 */
char *mx_rust_accept_verification(void *client, const char *flow_id);
char *mx_rust_confirm_verification(void *client, const char *flow_id);
char *mx_rust_mismatch_verification(void *client, const char *flow_id);
char *mx_rust_cancel_verification(void *client, const char *flow_id);

/*
 * Lightning-initiated (outbound) SAS verification (v0.5.6). Requests
 * verification of the current session against another session belonging
 * to the SAME Matrix account. Advertises SAS as the only method. Uses
 * the SDK's `UserIdentity::request_verification_with_methods` path, which
 * for the account owner sends the request over to-device to the user's
 * other E2EE-capable devices.
 *
 * Result events on the poll queue:
 *   { "type": "verification_request_started", "flow_id": "..." }
 * followed later by the usual verification_ready / verification_sas_ready
 * / verification_done / verification_cancelled events, exactly like the
 * receive-first flow.
 *
 * Returns "error: ..." synchronously if there is already an active flow,
 * the SDK does not have the account's own user identity available, or
 * the client is not logged in.
 */
char *mx_rust_start_own_verification(void *client);

/*
 * Report the current session's cross-signing trust state (v0.5.6).
 * Returns JSON:
 *   { "device_id": "...",
 *     "own_identity_available": true|false,
 *     "own_identity_verified": true|false,
 *     "device_cross_signed": true|false,
 *     "has_master": true|false,
 *     "has_self_signing": true|false,
 *     "has_user_signing": true|false }
 *
 * "device_cross_signed" is the source of truth for the UI's "Verified"
 * label — the SDK sets it to true only after cross-signing has actually
 * signed the current device. Never contains keys or signatures.
 * Returns "error: ..." when not logged in.
 */
char *mx_rust_query_own_device_status(void *client);

/*
 * Encrypted Megolm room-key import (v0.5.6). Decrypts an
 * Element/Matrix-SDK-compatible encrypted export file with `passphrase`
 * and imports the extracted inbound room-session keys into the active
 * SDK crypto store via `Encryption::import_room_keys`.
 *
 * The C++ side passes only a local file path and the passphrase. This
 * FFI never returns decrypted key material or JSON to C++. Only
 * aggregate counts and affected room IDs are surfaced. Passphrase
 * buffers are zeroized after use (via `zeroize::Zeroizing` inside the
 * SDK).
 *
 * Poll-queue events:
 *   { "type": "room_key_import_started" }
 *   { "type": "room_key_import_progress", "imported": N, "total": N }
 *      (currently emitted once at completion; the SDK does not surface
 *      intermediate progress in v0.18.)
 *   { "type": "room_key_import_done",
 *     "imported": N, "total": N, "affected_rooms": N,
 *     "room_ids": ["!...","!..."] }
 *   { "type": "room_key_import_failed",
 *     "category": "not_signed_in|invalid_file|bad_passphrase|read_failed|
 *                  import_failed|already_running|generation_stale",
 *     "message": "..." }
 *
 * Only one import may be active per client. Attempting a second while one
 * is running returns "already_running" via room_key_import_failed rather
 * than starting a second parallel task.
 */
char *mx_rust_import_room_keys(void *client,
                               const char *file_path,
                               const char *passphrase);

/* 0 = no import active, 1 = one in progress. Used by the C++ side to
 * gate sign-out and duplicate imports. Read-only. */
int mx_rust_room_key_import_active(void *client);

/*
 * Live SDK timeline (v0.5.7). Rust owns a persistent matrix-sdk-ui Timeline
 * for the currently open room (single active subscription). All functions
 * return "" on accepted dispatch or "error: …" synchronously. Results arrive
 * on the poll queue:
 *   {"type":"timeline_reset","room_id","room_generation","lifecycle",
 *    "items":[…]}                        — initial snapshot (atomic with the
 *                                          subscription; no event gap)
 *   {"type":"timeline_diff","op":"append|push_back|push_front|insert|set|
 *    remove|pop_front|pop_back|clear|truncate|reset", "index","length",
 *    "item","items", …}                  — incremental VectorDiff updates
 *   {"type":"timeline_pagination","state":"loading|idle|failed",
 *    "reached_start":bool,"category"}    — backward pagination lifecycle
 *   {"type":"timeline_send_failed","category"}
 *   {"type":"timeline_retry_decryption","state":"started|done","sessions":N}
 *   {"type":"timeline_error","category"}
 *   {"type":"timeline_closed","room_id"}
 * Every event is stamped with room_generation + lifecycle; the C++ side must
 * reject stale generations. Item payloads carry UI-safe metadata only —
 * never ciphertext, keys, or raw event JSON.
 */
char *mx_rust_timeline_open(void *client, const char *room_id);
char *mx_rust_timeline_close(void *client);
char *mx_rust_timeline_paginate_back(void *client,
                                     const char *room_id,
                                     unsigned short count);
char *mx_rust_timeline_send_text(void *client,
                                 const char *room_id,
                                 const char *body);
char *mx_rust_timeline_send_reply(void *client,
                                  const char *room_id,
                                  const char *in_reply_to_event_id,
                                  const char *body);
char *mx_rust_timeline_edit(void *client,
                            const char *room_id,
                            const char *target_event_id,
                            const char *new_body);
char *mx_rust_timeline_toggle_reaction(void *client,
                                       const char *room_id,
                                       const char *target_event_id,
                                       const char *key);
char *mx_rust_timeline_redact(void *client,
                              const char *room_id,
                              const char *target_event_id,
                              const char *reason);
char *mx_rust_timeline_retry_send(void *client,
                                  const char *room_id,
                                  const char *transaction_id);

/*
 * Deterministic shutdown of all managed async work: timeline subscriptions
 * are cancelled and joined, an in-flight room-key import is joined (bounded
 * last-resort timeout), the sync loop is stopped. Must be called before the
 * SDK store is deleted. Returns a short safe status string for logging.
 */
char *mx_rust_shutdown_tasks(void *client);

/* 0 = no, 1 = yes. Reports honestly — 0 until verified encrypted read/send. */
int mx_rust_supports_e2ee(void *client);

void mx_rust_free_cstring(char *ptr);

#ifdef __cplusplus
}
#endif
