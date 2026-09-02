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

#include <stdbool.h>
#include <stddef.h>

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
/* refresh_token may be "" when the account has none (the usual case for a
 * password session on a server that does not issue them). It is a CREDENTIAL:
 * never log it and never pass it to QML. */
char *mx_rust_restore(void *client,
                      const char *homeserver,
                      const char *user_id,
                      const char *device_id,
                      const char *access_token,
                      const char *refresh_token);
void  mx_rust_logout(void *client);

/* ---------------------------------------------------------------------------
 * OAuth 2.0 / OIDC (matrix-sdk 0.18 `Client::oauth()`); see rust/src/oauth.rs.
 *
 * Two-phase store lifecycle. Phase A runs on a BOOTSTRAP handle created by
 * mx_rust_oauth_bootstrap_create(), which has NO persistent store — the SDK's
 * in-memory default — because the Matrix user ID is unknown until the code
 * exchange completes, so no account store can be chosen yet. Phase B happens
 * on an ordinary account-scoped handle from mx_rust_create(), via
 * mx_rust_oauth_restore(). The bootstrap handle must never sync and must be
 * released with mx_rust_destroy() once oauth_ok has been consumed.
 *
 * PKCE, the CSRF `state` and the token exchange are SDK-owned. Lightning only
 * opens the browser and receives the loopback redirect.
 *
 * Legacy Matrix SSO is deliberately NOT implemented: the SDK's helper needs
 * the sso-login/local-server features, whose axum dependency is not vendored
 * in this offline --locked build. Discovery still reports whether a server
 * offers it so the UI can say so honestly.
 * ------------------------------------------------------------------------- */
void *mx_rust_oauth_bootstrap_create(void);
/* Enqueues one `auth_discovery` event: which methods this server really
 * offers. Never hard-codes behaviour for a particular homeserver. */
char *mx_rust_oauth_discover(void *client, const char *homeserver);
/* Enqueues `oauth_url` (open it in the system browser) or `oauth_failed`.
 * redirect_uri must be a loopback address; anything else is refused. */
char *mx_rust_oauth_begin(void *client,
                          const char *homeserver,
                          const char *redirect_uri);
/* `callback` is the full redirect URI the loopback listener received. It
 * carries the authorization code — never log it. Enqueues `oauth_ok` (with
 * the canonical user/device and the session material) or `oauth_failed`. */
char *mx_rust_oauth_finish(void *client, const char *callback);
/* Cancellation/timeout: drops the SDK's stored authorization data so a late
 * or replayed callback cannot complete the sign-in. */
char *mx_rust_oauth_abort(void *client);
/* Phase B. client_id is the dynamic-registration id from oauth_ok; both
 * tokens are CREDENTIALS. refresh_token may be "". */
char *mx_rust_oauth_restore(void *client,
                            const char *homeserver,
                            const char *user_id,
                            const char *device_id,
                            const char *client_id,
                            const char *access_token,
                            const char *refresh_token);
/* Revokes the tokens at the authorization server. Store deletion stays in
 * C++, which is the only layer that knows which store belongs to whom. */
char *mx_rust_oauth_logout(void *client);

/* --- Legacy Matrix SSO (m.login.sso) -------------------------------------
 * A DIFFERENT flow from OAuth above, deliberately kept apart: the homeserver
 * redirects the browser back with a single-use `loginToken`, exchanged through
 * /login with type m.login.token. Uses the SDK's ungated
 * MatrixAuth::get_sso_login_url() / login_token(), so no `sso-login` feature
 * and no new dependency. It reuses the OAuth BOOTSTRAP handle and the same
 * two-phase store lifecycle, for the same reason: the user id is only known
 * after the exchange, so Phase B (C++) opens the account store and restores
 * through mx_rust_restore(). */

/* Lists the identity providers the server advertises. Enqueues sso_providers
 * with {id, name, icon}; an SSO server with NO providers is normal and means
 * one unnamed flow. `icon` is only ever an mxc: URI. */
char *mx_rust_sso_providers(void *client, const char *homeserver);
/* Asks the server for its SSO redirect URL. idp_id may be "" for the default
 * flow. redirect_uri must be loopback (enforced Rust-side too). Enqueues
 * sso_url or sso_failed. */
char *mx_rust_sso_begin(void *client,
                        const char *homeserver,
                        const char *redirect_uri,
                        const char *idp_id);
/* Exchanges the loginToken for a session. THE TOKEN IS A CREDENTIAL: it is
 * never logged, never echoed into an error, and never returned. Enqueues
 * sso_ok (user_id, device_id, access_token, refresh_token) or sso_failed. */
char *mx_rust_sso_finish(void *client, const char *login_token);
/* Cancellation/timeout: releases the bootstrap client so a late callback
 * cannot complete an abandoned sign-in. */
char *mx_rust_sso_abort(void *client);

void  mx_rust_start_sync(void *client);
/* Cancels and joins the owned sync task. 1 = stopped, 0 = already stopped. */
int   mx_rust_stop_sync(void *client);
char *mx_rust_poll_event(void *client);
/* v0.7 defense-in-depth: dedicated TERMINAL command lane (media ready /
 * failed, GIF results). C++ drains this completely before every bulk
 * mx_rust_poll_event batch so terminal results are never starved. */
char *mx_rust_poll_command_event(void *client);

/* 0.5.8 Matrix-native room state commands. Results are asynchronous events;
 * no composer text, raw events, or secret material crosses these calls. */
char *mx_rust_send_typing(void *client, const char *room_id, int typing);
char *mx_rust_send_read_receipt(void *client,
                                const char *room_id,
                                const char *event_id);
char *mx_rust_set_marked_unread(void *client, const char *room_id, int unread);
/* Mark a room read WITHOUT opening it. The target event comes from the SDK's
 * own Room::latest_event(), so no timeline needs to be loaded. Sends the
 * public read receipt AND m.fully_read together, and always clears the
 * manual unread flag. Answers on read_marker_advanced, or room_action_error
 * with action="mark_read". */
char *mx_rust_mark_room_read(void *client, const char *room_id);
/* Add or remove a room's m.favourite tag through Room::set_is_favourite,
 * which also drops a conflicting m.lowpriority tag. NOT optimistic: success
 * re-emits the room list carrying the SDK's own is_favourite, and a refusal
 * answers room_action_error with action="favourite" and changes nothing. */
char *mx_rust_set_room_favourite(void *client, const char *room_id,
                                 int favourite);
/* Send attachment bytes to a room whose live timeline is NOT open — the
 * forwarding path. Routes through Room::send_attachment; the SDK still
 * encrypts for the target room when that room is encrypted. Answers on
 * attachment_send_result with the given op_id. */
char *mx_rust_room_send_attachment_bytes(void *client, const char *room_id,
                                         const unsigned char *data, size_t len,
                                         const char *filename, const char *mime,
                                         uint64_t width, uint64_t height,
                                         uint64_t op_id);
/* Server-synchronized per-room notification mode (SDK push rules).
 * mode: 0 = all messages, 1 = mentions & keywords only, 2 = mute. Set is
 * label-faithful (mode 0 writes an explicit AllMessages rule); every rule
 * construction and cleanup stays inside matrix-sdk. Get resolves the
 * user-defined room rule first, else the account default for the room's
 * shape; a get issued while a write for the room is still in flight is
 * silently skipped — the write's own report is authoritative. Result
 * event on the poll queue:
 *   {"type":"room_notification_mode","room_id","mode":0|1|2,
 *    "user_defined":bool}
 * A failed write enqueues the dedicated
 *   {"type":"notification_mode_error","room_id"}
 * — room id only, never rule JSON or SDK error text. */
char *mx_rust_set_room_notification_mode(void *client,
                                         const char *room_id,
                                         int mode);
/* v0.7 "follow account default": remove the room's user-defined push rules
 * so the account's own rules apply again. Matrix has no follow-default rule
 * — it has the ABSENCE of a room override — so this deletes rather than
 * writes. Reports mode 3 with "user_defined":false on success, which is
 * exactly the room's resulting state. Shares the set path's serialization,
 * so a clear and a set cannot land out of order. */
char *mx_rust_clear_room_notification_mode(void *client, const char *room_id);
/* v0.7: real thread participants for the summary-card facepile. Answers
 * asynchronously with
 *   {"type":"thread_participants","room_id","root_event_id","ok":bool,
 *    "participants":[{"user_id","display_name","avatar_url"}],
 *    "distinct":n,"truncated":bool}
 * Cache-first (Room::load_or_fetch_event_with_relations); only
 * presentation-safe fields cross, never event content. "distinct" is the
 * number of DISTINCT senders found, not the reply count. */
char *mx_rust_thread_participants(void *client,
                                  const char *room_id,
                                  const char *root_event_id);
/* 2026-08-18: redact this message's OWN m.replace edits, returning it to its
 * original text (the "edited" marker is derived from those events, so it goes
 * with them). Cache-first relation lookup, own edits only, bounded at 50 per
 * pass. Answers asynchronously with
 *   {"type":"message_edits_removed","room_id","event_id","ok":bool,
 *    "removed":n,"failed":n,"truncated":bool}
 * Counts only — no event content crosses. */
char *mx_rust_remove_message_edits(void *client,
                                   const char *room_id,
                                   const char *event_id);
char *mx_rust_get_room_notification_mode(void *client, const char *room_id);
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
 * Interactive device verification (v0.5.0; QR added post-0.6.5).
 * Receive-first flow: the Rust bridge installs a to-device
 * verification-request handler at install_event_handlers time and emits
 * `verification_request_received` for incoming requests. From C++:
 *   accept    -> drive request -> show-QR leg, then the SAS handshake,
 *               emitting `verification_sas_ready` with the 7 emojis +
 *               decimals when SAS is reached.
 *   confirm   -> user says "they match" (SAS).
 *   confirmQr -> user says the OTHER device reported a successful scan.
 *   mismatch  -> user says "they do not match" (SAS).
 *   cancel    -> SAS-, QR- and request-level cancel; whichever levels
 *               belong to this flow id are cancelled on the wire.
 *
 * Method advertisement is [m.sas.v1, m.qr_code.show.v1, m.reciprocate.v1]
 * in BOTH directions. `m.qr_code.scan.v1` is never advertised: Lightning
 * has no camera and cannot scan a code the peer displays.
 *
 * Show-QR events on the poll queue:
 *   { "type": "verification_qr_ready", "flow_id", "size", "bits_b64" }
 *       The QR module GRID only: `size` modules per side, and `bits_b64` a
 *       base64 row-major bitmap, MSB-first, each row starting on a fresh
 *       byte (stride = (size + 7) / 8, 1 = dark). The QR PAYLOAD — which
 *       encodes cross-signing key material and the flow's shared secret —
 *       never crosses this FFI, is never logged, and is never persisted.
 *   { "type": "verification_qr_scanned",   "flow_id" }
 *       The peer scanned. The UI must ask the USER whether the other
 *       device reported success; nothing is auto-confirmed.
 *   { "type": "verification_qr_confirmed", "flow_id" }
 *   { "type": "verification_qr_dismissed", "flow_id", "reason" }
 *       The code is no longer usable and the flow continues on SAS.
 *       `reason` is a sanitized category: "peer_started_sas" (the peer
 *       chose emoji) or "not_scanned" (the display window elapsed).
 * Completion still arrives as verification_done / _cancelled / _failed.
 *
 * Only one active flow at a time (single-flow policy). All flow ids are
 * safe to log; emojis are also safe (SAS design). No secrets are ever
 * forwarded through this FFI.
 */
char *mx_rust_accept_verification(void *client, const char *flow_id);
char *mx_rust_confirm_verification(void *client, const char *flow_id);
char *mx_rust_mismatch_verification(void *client, const char *flow_id);
char *mx_rust_cancel_verification(void *client, const char *flow_id);

/*
 * Confirm that the OTHER device reported a successful scan of the QR code
 * Lightning displayed. This is the human check that gives showing a code
 * its security value, so it is never issued automatically. Returns
 * "error: ..." synchronously when there is no active QR flow, the flow id
 * does not match, or the peer has not scanned yet (the SDK would silently
 * drop a confirm sent outside its Scanned state).
 */
char *mx_rust_confirm_qr_verification(void *client, const char *flow_id);

/*
 * Lightning-initiated (outbound) verification (v0.5.6). Requests
 * verification of the current session against another session belonging
 * to the SAME Matrix account. Advertises the same method set as the
 * inbound path. Uses the SDK's
 * `UserIdentity::request_verification_with_methods` path, which for the
 * account owner sends the request over to-device to the user's other
 * E2EE-capable devices.
 *
 * Result events on the poll queue:
 *   { "type": "verification_request_started", "flow_id": "..." }
 * followed later by the usual verification_ready / verification_qr_* /
 * verification_sas_ready / verification_done / verification_cancelled
 * events, exactly like the receive-first flow.
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
 * v0.6.0 checkpoint 7: async E2EE health snapshot from official SDK state
 * APIs (device trust, cross-signing keys, key backup, recovery, secret
 * storage). Result arrives as a `crypto_health` poll event carrying only
 * booleans, enum names, and the public device id — never key material.
 */
char *mx_rust_query_crypto_health(void *client);

/*
 * v0.7.2: user-initiated "Request keys again". Nudges the per-account
 * recovery coordinator to run one immediate standards-based
 * m.secret.request round through the SDK's gossip machinery (fresh
 * request IDs, deduplicated, full SDK trust validation on answers) and
 * re-arm its bounded follow-up ladder. Progress arrives as sanitized
 * `crypto_bootstrap` poll events. Returns "" on dispatch, "error: ..."
 * when not logged in or the encryption sync is not running.
 */
char *mx_rust_request_missing_secrets(void *client);

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
/* 2026-08-19: re-open the live timeline after letting the SDK event cache
 * release the paginated backlog (Element's jump-to-live-rebuilds behaviour).
 * Explicit user-initiated jump-to-latest ONLY — never scrolling/pagination.
 * Emits the ordinary timeline_reset, plus a `trimmed_from` count. */
char *mx_rust_timeline_reload_at_live(void *client, const char *room_id);
char *mx_rust_timeline_close(void *client);
char *mx_rust_timeline_paginate_back(void *client,
                                     const char *room_id,
                                     unsigned short count);
/* `mention_user_ids` (v0.7) is a nullable newline-separated list of full MXIDs
 * placed in m.mentions; NULL/empty attaches no mentions. The body already
 * carries the matrix.to markdown links for those users.
 *
 * `body_spec` (v0.9 formatted sends) is a nullable JSON object selecting how
 * the body is interpreted:
 *   {"format":"markdown"|"plain"|"html","html":"…","msgtype":"text"|"emote"}
 * NULL/empty is the historical markdown path exactly. "plain" sends the body
 * verbatim (no markdown parsing); "html" sends the caller's plain body plus
 * the given Matrix-subset formatted body (strict-sanitized again in Rust).
 * Unknown values are refused with an error, never defaulted. */
char *mx_rust_timeline_send_text(void *client,
                                 const char *room_id,
                                 const char *body,
                                 const char *mention_user_ids,
                                 const char *body_spec);
char *mx_rust_timeline_send_reply(void *client,
                                  const char *room_id,
                                  const char *in_reply_to_event_id,
                                  const char *body,
                                  const char *mention_user_ids,
                                  const char *body_spec);
/* v0.6.0: SDK-backed thread timelines. One thread panel at a time; it
 * belongs to the open room and is closed automatically by room switches. */
char *mx_rust_thread_open(void *client,
                          const char *room_id,
                          const char *root_event_id);
char *mx_rust_thread_close(void *client);
char *mx_rust_thread_paginate_back(void *client,
                                   const char *room_id,
                                   const char *root_event_id,
                                   unsigned short count);
/* in_reply_to may be NULL/empty (plain thread message) or an event id in
 * the same thread (rich reply within the thread). */
char *mx_rust_thread_send_text(void *client,
                               const char *room_id,
                               const char *root_event_id,
                               const char *body,
                               const char *in_reply_to,
                               const char *mention_user_ids,
                               const char *body_spec);
/* v0.6.0 checkpoint 9: list the account's devices/sessions (server list
 * merged with SDK crypto trust). Result: `device_list` poll event with
 * presentation-safe fields only — never device keys or tokens. */
char *mx_rust_list_devices(void *client);

/* v0.6.0 checkpoint 8: manual decryption retry for the open room's visible
 * unable-to-decrypt events (incl. the open thread panel). Never resets or
 * touches the crypto store. */
char *mx_rust_timeline_retry_decryption(void *client, const char *room_id);

/* v0.6.0 checkpoint 5: room thread list (Threads view), MSC4306 follow
 * state, and threaded read receipts for the open thread panel. */
char *mx_rust_thread_list_open(void *client, const char *room_id);
char *mx_rust_thread_list_close(void *client);
char *mx_rust_thread_list_paginate(void *client, const char *room_id);
char *mx_rust_thread_mark_read(void *client,
                               const char *room_id,
                               const char *root_event_id);
char *mx_rust_thread_subscription_query(void *client,
                                        const char *room_id,
                                        const char *root_event_id);
char *mx_rust_thread_set_subscribed(void *client,
                                    const char *room_id,
                                    const char *root_event_id,
                                    int subscribed);
char *mx_rust_timeline_edit(void *client,
                            const char *room_id,
                            const char *target_event_id,
                            const char *new_body,
                            const char *mention_user_ids,
                            const char *body_spec);
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
/* Cancel a local echo that has not reached the server yet, via the SDK's
 * SendHandle::abort — which also aborts an in-flight media upload. A
 * successful abort emits nothing: the SDK's CancelledLocalEvent removes the
 * row through the ordinary timeline diff. A refusal answers
 * timeline_send_failed with category cancel_too_late (already on the
 * server), cancel_target_missing or cancel_failed. */
char *mx_rust_timeline_cancel_send(void *client,
                                   const char *room_id,
                                   const char *transaction_id);

/*
 * v0.7 — MSC3381 polls. thread_root_id NULL/empty targets the room's live
 * timeline; a real root event id targets that thread (the SDK owns the
 * m.thread relation for poll starts). answer_ids/answers are newline-
 * separated lists; an empty answer_ids list retracts the caller's vote.
 */
char *mx_rust_timeline_poll_response(void *client,
                                     const char *room_id,
                                     const char *thread_root_id,
                                     const char *poll_start_event_id,
                                     const char *answer_ids);
char *mx_rust_timeline_poll_end(void *client,
                                const char *room_id,
                                const char *thread_root_id,
                                const char *poll_start_event_id);
char *mx_rust_timeline_poll_create(void *client,
                                   const char *room_id,
                                   const char *thread_root_id,
                                   const char *question,
                                   const char *answers,
                                   int undisclosed,
                                   unsigned int max_selections);

/*
 * v0.5.9 — room management, user search, attachments and the media bridge.
 *
 * Every command takes a C++-generated op_id that is echoed on the async
 * result event together with the Rust lifecycle generation. C++ must ignore
 * results whose op_id it does not recognise or whose lifecycle is stale.
 */
char *mx_rust_search_users(void *client,
                           const char *query,
                           unsigned long long limit,
                           unsigned long long op_id);
/*
 * v0.5.11: exact profile lookup (GET /profile/{userId}) confirming a
 * user id the directory may not list (bare-localpart invite search).
 * Result event: {"type":"user_profile_result","op_id",…,"ok",
 * "user_id","display_name","avatar_url"} or ok=false with a coarse
 * "category" ("not_found" when the homeserver does not know the user).
 */
char *mx_rust_get_user_profile(void *client,
                               const char *user_id,
                               unsigned long long op_id);
/*
 * v0.7.4: set — or CLEAR — the signed-in account's own display name via the
 * SDK's Account::set_display_name. An EMPTY `name` means clear (the SDK
 * chooses the MSC4133 delete-profile-field endpoint where the server
 * advertises it); a non-empty one is bounded at 255 Unicode scalar values
 * in Rust. The name is never logged and never echoed back.
 * Result event: {"type":"own_display_name_result","op_id",…,"ok",
 * "error"} — `error` carries only the server's own sanitized sentence, and
 * is empty when the server said nothing usable (including a timeout).
 */
/*
 * Rooms this account and `user_id` are BOTH joined to. Reads only cached
 * membership and issues NO request, so a room whose members were never
 * synced is not listed — deliberate under-reporting, because the obvious
 * implementation costs one /state per room every time a profile opens.
 *
 * Result: {"type":"mutual_rooms_result","op_id",…,"user_id",
 *          "rooms":[{"room_id","name","avatar_url","is_direct"}]}
 */
char *mx_rust_mutual_rooms(void *client,
                           const char *user_id,
                           uint64_t op_id);

/*
 * Upload and set the signed-in account's OWN avatar from a local file path.
 * The MIME type is sniffed from the bytes, never from the file name, and SVG
 * is refused. Bounded at the same ceiling as the room-avatar path.
 *
 * Result event: {"type":"own_avatar_result","op_id",…,"ok","error"}.
 */
char *mx_rust_set_own_avatar(void *client,
                             const char *local_path,
                             uint64_t op_id);

/* Clear the account's own avatar. Same result event as the setter. */
char *mx_rust_clear_own_avatar(void *client, uint64_t op_id);

char *mx_rust_set_display_name(void *client,
                               const char *name,
                               unsigned long long op_id);
/*
 * v0.7.x Matrix presence: one bounded polling round. `user_ids_json` is a
 * JSON array of user-id strings (invalid entries dropped, batch capped at
 * 40 in Rust). Sliding Sync carries no presence, so C++ polls exactly the
 * users on screen. Answers as ONE event:
 *   {"type":"presence_batch","op_id",…,"entries":[
 *     {"user_id","ok":true,"state":"online|unavailable|offline|unknown",
 *      "currently_active":bool,"last_active_ago_ms":n}
 *   | {"user_id","ok":false,"category":"forbidden|not_found|…"}]}
 * last_active_ago_ms is OMITTED (never null) when the server sent none;
 * C++ must decode absent as -1 = unknown, never as 0.
 * Only presentation-safe fields cross; status messages do not.
 */
char *mx_rust_get_presence(void *client,
                           const char *user_ids_json,
                           unsigned long long op_id);
/*
 * v0.7.x Matrix presence: publish the local user's own state
 * (0 online, 1 unavailable, 2 offline). Fire-and-forget: success emits
 * nothing, failure emits {"type":"presence_publish_failed","category"}.
 */
char *mx_rust_set_presence(void *client, unsigned int state);
/*
 * Profile banners (MSC4427 over MSC4133 extended profile fields).
 *
 * READ prefers the stable "m.banner_url" and falls back to
 * "chat.commet.profile_banner" — the key Commet already ships and Sable and
 * Haven read — so a banner set in any of them is visible here. Answers as
 *   {"type":"profile_banner","op_id",…,"user_id","mxc","supported":bool}
 * `supported:false` means the homeserver does not implement extended profile
 * fields AT ALL, which is a different fact from "this user has no banner" and
 * must render as nothing rather than as an absence.
 *
 * The value is always an mxc:// URI; anything else is dropped in Rust. A
 * profile field is remote text, and an http URL in one would make every
 * viewer who opens the card fetch it from a host its owner controls.
 */
char *mx_rust_fetch_profile_banner(void *client,
                                   const char *user_id,
                                   unsigned long long op_id);
/*
 * Upload a local image and set it as this account's banner, under BOTH field
 * names. An EMPTY path clears both. Content decides the type (magic bytes),
 * never the file name. Answers as
 *   {"type":"profile_banner_set","op_id",…,"ok","mxc","category"}.
 */
char *mx_rust_set_profile_banner(void *client,
                                 const char *local_path,
                                 unsigned long long op_id);
/*
 * Profile biographies (MSC4440 over MSC4133 extended profile fields).
 *
 * READ prefers the stable "m.biography" and falls back to
 * "gay.fomx.biography" — MSC4440's own unstable prefix, and the key Sable
 * writes today — so a bio written in either is visible here. Answers as
 *   {"type":"profile_bio","op_id",…,"user_id","bio","supported":bool}
 * `supported:false` means the homeserver does not implement extended profile
 * fields AT ALL, which is a different fact from "this user has not written a
 * bio" and must render as nothing rather than as an error.
 *
 * The value is always PLAIN TEXT, bounded and stripped of control characters
 * in Rust. MSC4440 permits an HTML representation and Lightning deliberately
 * neither renders nor authors one: a bio is remote free text, and the MSC's
 * own example embeds an <img src="mxc://…"> that would be fetched by every
 * viewer of the card.
 */
char *mx_rust_fetch_profile_bio(void *client,
                                const char *user_id,
                                unsigned long long op_id);
/*
 * Set this account's bio, under BOTH field names. EMPTY (or whitespace-only)
 * text clears both. Answers as
 *   {"type":"profile_bio_set","op_id",…,"ok","bio","category"}.
 */
char *mx_rust_set_profile_bio(void *client,
                              const char *text,
                              unsigned long long op_id);
/*
 * Read a room's (or Space's) banner and whether this account may change it.
 * Answers as
 *   {"type":"room_banner","op_id",…,"room_id","mxc","can_set":bool}
 *
 * Matrix specifies NO room banner — MSC4427 covers user profiles only — so
 * this is Lightning's own state event, org.lightning_matrix.room_banner, and
 * a client that does not know it simply renders no banner. `can_set` is the
 * room's own required power level for that event type, asked of the SDK, not
 * a role label. As with a profile banner, only an mxc:// URI is accepted.
 */
char *mx_rust_fetch_room_banner(void *client,
                                const char *room_id,
                                unsigned long long op_id);
/*
 * Upload a local image and set it as the room's banner. An EMPTY path clears
 * it. Content decides the type (magic bytes), never the file name. Answers as
 *   {"type":"room_banner_set","op_id",…,"room_id","ok","mxc","category"}.
 */
char *mx_rust_set_room_banner(void *client,
                              const char *room_id,
                              const char *local_path,
                              unsigned long long op_id);
/*
 * Stickers and custom emoji — MSC2545 image packs (`im.ponies.*`).
 *
 * Read every pack this account can use and answer with ONE snapshot:
 *   {"type":"sticker_packs","op_id",…,"room_id",
 *    "packs":[{"id","display_name","avatar_url","attribution",
 *              "source":"user"|"room","room_id","state_key",
 *              "images":[{"shortcode","url","body","mimetype",
 *                         "width","height","size",
 *                         "is_emoticon","is_sticker"}]}]}
 *
 * `room_id` may be EMPTY. When set, that room's own `im.ponies.room_emotes`
 * packs are included: MSC2545 makes a room's packs available inside that room
 * with no opt-in, and `im.ponies.emote_rooms` is what makes them available
 * elsewhere.
 *
 * Everything in a pack is remote, author-chosen content and is validated in
 * Rust before it crosses: a non-`mxc://` url is DROPPED (an https url on a
 * picker tile is a tracking beacon that fires once per listing), a DECLARED
 * mimetype outside the five raster types is refused (image/svg+xml above all
 * — CLAUDE.md §6), and shortcodes/bodies/names are bounded and stripped of
 * control characters. They are LABELS on the C++ side, never rich text.
 */
char *mx_rust_stickers_fetch_packs(void *client,
                                   const char *room_id,
                                   unsigned long long op_id);
/*
 * Send one `m.sticker`. An EMPTY `thread_root_id` targets the room timeline;
 * otherwise the SDK's thread-aware send attaches the `m.thread` relation
 * itself — Lightning builds no relation by hand (CLAUDE.md §8).
 *
 * `url` must be a plain `mxc://`: a pack image is already Matrix media, so
 * there is nothing to upload. Refused otherwise, so no caller can put an http
 * URL on the wire. Reports failure on the ordinary timeline send path.
 */
char *mx_rust_stickers_send(void *client,
                            const char *room_id,
                            const char *thread_root_id,
                            const char *url,
                            const char *body,
                            const char *mimetype,
                            unsigned long long width,
                            unsigned long long height,
                            unsigned long long size);
/*
 * Add one image to a ROOM's `im.ponies.room_emotes` pack. Answers on the SAME
 * "sticker_pack_add_result" event as the user-pack path.
 *
 * ROOM STATE, so POWER-LEVEL GATED on the room's own required level for that
 * event type, asked of the SDK — never a role label. `category` is
 * "forbidden" when this account may not write it.
 */
char *mx_rust_stickers_add_to_room_pack(void *client,
                                        const char *room_id,
                                        const char *state_key,
                                        const char *shortcode,
                                        const char *url,
                                        const char *body,
                                        const char *mimetype,
                                        unsigned long long width,
                                        unsigned long long height,
                                        unsigned long long size,
                                        unsigned long long op_id);
/*
 * Turn one ROOM pack on or off in `im.ponies.emote_rooms` — "use this room's
 * stickers everywhere". Answers as
 *   {"type":"sticker_pack_rooms_set","op_id",…,"ok","category",
 *    "room_id","state_key","enabled"}
 *
 * ACCOUNT DATA, so no power level is involved: it records the reader's own
 * choice. A room's packs are always usable INSIDE that room whatever this
 * holds, which is why turning one off does not remove it from its own room.
 */
char *mx_rust_stickers_set_room_pack_enabled(void *client,
                                             const char *room_id,
                                             const char *state_key,
                                             bool enabled,
                                             unsigned long long op_id);
/*
 * Add one image to this account's own `im.ponies.user_emotes` pack ("save
 * this sticker"). Read-modify-write against the SERVER copy, never the store,
 * so a concurrent edit from another device is not clobbered. Answers as
 *   {"type":"sticker_pack_add_result","op_id",…,"ok","category","shortcode"}
 * where `category` is "duplicate" (that exact mxc is already in the pack),
 * "pack_full", or a coarse room-error class.
 */
/*
 * Upload a LOCAL image file and add it to this account's own pack. This is
 * the only way to create a pack from nothing — every other route needs an
 * mxc that already exists. Bounded at 4 MiB, MIME sniffed from the bytes,
 * SVG refused. Same result event as the save path.
 */
char *mx_rust_stickers_upload_to_user_pack(void *client,
                                           const char *shortcode,
                                           const char *body,
                                           const char *local_path,
                                           uint64_t op_id);

char *mx_rust_stickers_add_to_user_pack(void *client,
                                        const char *shortcode,
                                        const char *url,
                                        const char *body,
                                        const char *mimetype,
                                        unsigned long long width,
                                        unsigned long long height,
                                        unsigned long long size,
                                        unsigned long long op_id);
/*
 * v0.5.12: secure client-side HTTPS preview. Rust validates DNS and every
 * redirect, blocks local/private destinations, bounds responses and parses
 * metadata without executing page content. The URL is never logged:
 *   {"type":"url_preview_result","op_id",…,"ok",
 *    "fields":{"preview_kind","title","description","site_name",
 *              "image_source","image_mime","image_width","image_height",
 *              "image_size"}}
 * or ok=false with a coarse "category".
 */
char *mx_rust_get_url_preview(void *client,
                              const char *url,
                              unsigned long long op_id);
/*
 * v0.6.1: bounded, redirect-validated HTTPS GET for an external GIF provider
 * (GIPHY / KLIPY). `url` is built C++-side and carries the provider API key —
 * it is secret and is never logged by the bridge. Result arrives as a
 * `gif_response` poll event:
 *   {"type":"gif_response","op_id",…,"ok",bool,"status":int,
 *    "category":"ok|rate_limited|provider_error|timeout|network|too_large|
 *                blocked","body":"<bounded JSON, empty on error>"}
 * No Matrix identifiers are ever sent to the provider.
 */
char *mx_rust_gif_get(void *client, const char *url, unsigned long long op_id);

/*
 * v0.6.1: download + validate a provider GIF for sending. Accepts only https
 * provider-CDN URLs (*.giphy.com / *.klipy.com); the response must be a real
 * GIF (GIF87a/GIF89a magic, bounded canvas) — HTML/JSON/mp4/webp are rejected.
 * On success the bytes are parked for mx_rust_media_take(op_id) (never the JSON
 * queue) and a `gif_download_result` event is emitted:
 *   {"type":"gif_download_result","op_id",…,"ok":true,"mime":"image/gif",
 *    "width":N,"height":N,"size":N}
 * or ok=false with "category":"blocked|not_a_gif|too_large|invalid_media|
 *    timeout|network|provider_error".
 */
char *mx_rust_gif_download(void *client, const char *url,
                           unsigned long long op_id);

/* Synchronous m.direct projection: {"rooms":[{"room_id","name"}]}. */
char *mx_rust_get_dm_rooms(void *client, const char *user_id);
char *mx_rust_create_dm(void *client,
                        const char *user_id,
                        unsigned long long op_id);
/* options_json: {"name","topic","public","encrypted","alias","invites":[],"space_id"} */
char *mx_rust_create_room(void *client,
                          const char *options_json,
                          unsigned long long op_id);
char *mx_rust_invite_users(void *client,
                           const char *room_id,
                           const char *users_json,
                           unsigned long long op_id);
char *mx_rust_room_members(void *client,
                           const char *room_id,
                           unsigned long long op_id);
char *mx_rust_set_room_name(void *client,
                            const char *room_id,
                            const char *name,
                            unsigned long long op_id);
char *mx_rust_set_room_topic(void *client,
                             const char *room_id,
                             const char *topic,
                             unsigned long long op_id);
char *mx_rust_set_room_avatar(void *client,
                              const char *room_id,
                              const char *local_path,
                              unsigned long long op_id);
char *mx_rust_remove_room_avatar(void *client,
                                 const char *room_id,
                                 unsigned long long op_id);
char *mx_rust_leave_room(void *client,
                         const char *room_id,
                         unsigned long long op_id);
/* Moderation: kick (op = 0), ban (op = 1) or unban (op = 2) one user
 * through the SDK's own moderation calls; `reason` may be empty. Result:
 * room_moderation_result { op_id, room_id, user_id, op, ok, category }. */
char *mx_rust_moderate_user(void *client,
                            const char *room_id,
                            const char *user_id,
                            const char *reason,
                            unsigned char op,
                            unsigned long long op_id);
/* v0.7.x room administration. Set one member's power level; every other
 * user's level (including arbitrary custom numbers) is preserved by the
 * SDK. Result: room_power_level_result
 * { op_id, room_id, user_id, level, ok, category }. */
char *mx_rust_set_member_power_level(void *client,
                                     const char *room_id,
                                     const char *user_id,
                                     long long level,
                                     unsigned long long op_id);
/* 2026-08-26 Space settings: set ONE threshold in m.room.power_levels.
 * `key` must be one of the fixed allowlist in
 * rooms::set_room_power_level_key ("ban", "invite", "kick", "redact",
 * "events_default", "state_default", "users_default", "m.space.child",
 * "m.room.name", "m.room.avatar", "m.room.topic", "m.room.join_rules",
 * "m.room.canonical_alias", "m.room.power_levels", "m.room.tombstone");
 * anything else is refused at the Rust edge, so this never becomes a
 * generic arbitrary-event-type power writer. Result:
 * room_power_matrix_result { op_id, room_id, key, level, ok, category }. */
char *mx_rust_set_room_power_level_key(void *client,
                                       const char *room_id,
                                       const char *key,
                                       long long level,
                                       unsigned long long op_id);

/* Join rule: "invite", "public" or "knock". Rules carrying an allow-rule
 * list (restricted / knock_restricted) are refused here on purpose.
 * Result: room_edit_result with field "join_rule". */
/* v0.9: `allowed_room_ids` (nullable newline-separated) is the allow list
 * for "restricted" / "knock_restricted"; ignored for the other rules. */
char *mx_rust_set_room_join_rule(void *client,
                                 const char *room_id,
                                 const char *rule,
                                 const char *allowed_room_ids,
                                 unsigned long long op_id);
/* v0.9 room access. History visibility: invited | joined | shared |
 * world_readable. Guest access: can_join | forbidden. Directory visibility
 * READ answers on the room_directory_visibility poll event; the writer sets
 * published (true) or private (false). Alt aliases: the whole list, newline
 * separated; each field answers on room_edit_result. */
char *mx_rust_set_room_history_visibility(void *client,
                                          const char *room_id,
                                          const char *visibility,
                                          unsigned long long op_id);
char *mx_rust_set_room_guest_access(void *client,
                                    const char *room_id,
                                    const char *access,
                                    unsigned long long op_id);
char *mx_rust_request_room_directory_visibility(void *client,
                                                const char *room_id);
char *mx_rust_set_room_directory_visibility(void *client,
                                            const char *room_id,
                                            bool published,
                                            unsigned long long op_id);
char *mx_rust_set_room_alt_aliases(void *client,
                                   const char *room_id,
                                   const char *aliases,
                                   unsigned long long op_id);
/* v0.9 room upgrade. Versions from the homeserver's /capabilities
 * (poll event room_versions {ok, default, available:[{version, stable}]});
 * the upgrade is the standard /upgrade endpoint (poll event
 * room_upgrade_result {op_id, room_id, ok, replacement_room_id, category}). */
char *mx_rust_request_room_versions(void *client);
char *mx_rust_upgrade_room(void *client,
                           const char *room_id,
                           const char *new_version,
                           unsigned long long op_id);
/* Canonical alias; an empty alias clears it. Publishes the directory
 * mapping first when the alias does not already resolve to this room.
 * Result: room_edit_result with field "canonical_alias". */
char *mx_rust_set_room_canonical_alias(void *client,
                                       const char *room_id,
                                       const char *alias,
                                       unsigned long long op_id);
/* v0.7.x pinned messages. Read m.room.pinned_events and resolve each id
 * into a displayable row (bounded; cache-first, one /event per miss).
 * `allow_remote` permits the /state fallback used only when the room
 * carries no pinned-events state at all. Result: room_pinned
 * { op_id, room_id, ok, can_pin, total, truncated, entries[] }. */
char *mx_rust_room_pinned(void *client,
                          const char *room_id,
                          unsigned char allow_remote,
                          unsigned long long op_id);
/* Pin (pin = 1) or unpin (pin = 0) one event; the SDK performs the
 * read-modify-send of the state event. Result: room_pin_result
 * { op_id, room_id, event_id, pin, ok, changed, category }. */
char *mx_rust_set_room_pinned(void *client,
                              const char *room_id,
                              const char *event_id,
                              unsigned char pin,
                              unsigned long long op_id);
/* v0.7.x room discovery / join / knock. One op-id per call; results arrive
 * as room_target_resolved / public_rooms_result / room_join_result /
 * room_knock_result / knock_cancel_result / space_children_result events,
 * each stamped with op_id + lifecycle. */
/* Resolve #alias / !roomid / matrix: URI / matrix.to permalink into a
 * normalized join target and preview it where the server allows. ok=false
 * only for non-room input; a refused preview still resolves. */
char *mx_rust_resolve_room_target(void *client,
                                  const char *input,
                                  unsigned long long op_id);
/* One page of the public room directory. server: optional remote directory
 * host; since: pagination token from the previous page ("" for the first). */
char *mx_rust_search_public_rooms(void *client,
                                  const char *query,
                                  const char *server,
                                  const char *since,
                                  unsigned long long limit,
                                  unsigned long long op_id);
/* Join by id or alias; via is a newline-separated server list (nullable). */
char *mx_rust_join_room(void *client,
                        const char *target,
                        const char *via,
                        unsigned long long op_id);
/* Knock by id or alias with an optional reason (nullable). */
char *mx_rust_knock_room(void *client,
                         const char *target,
                         const char *via,
                         const char *reason,
                         unsigned long long op_id);
/* Withdraw a pending knock (leave the Knocked room). */
char *mx_rust_cancel_knock(void *client,
                           const char *room_id,
                           unsigned long long op_id);
/* v0.7.x ignored users + reporting (SDK account-data / reporting APIs).
 * Results: ignore_user_result / ignored_users_list / report_message_result;
 * remote (and local) list changes push ignored_users_changed from sync. */
char *mx_rust_set_user_ignored(void *client,
                               const char *user_id,
                               unsigned char ignored,
                               unsigned long long op_id);
char *mx_rust_list_ignored_users(void *client, unsigned long long op_id);
char *mx_rust_report_message(void *client,
                             const char *room_id,
                             const char *event_id,
                             const char *reason,
                             unsigned long long op_id);
/* 2026-08-18 voice-call signaling pipes (MSC2746 m.call.* v1 + the
 * m.rtc.notification/decline lane). Signaling only — no media stack exists;
 * SDP parameters are opaque required inputs and NEVER appear in poll
 * events, which carry only has_offer/has_answer booleans. Results arrive
 * later through mx_rust_poll_event() as call_send_result {op_id, ok,
 * category, call_id, event_id}; inbound observations arrive as
 * call_invite / call_answer / call_hangup / call_reject /
 * call_select_answer / call_rtc_notification / call_rtc_decline. */
char *mx_rust_calls_invite(void *client,
                           const char *room_id,
                           const char *call_id,
                           const char *party_id,
                           const char *offer_type,
                           const char *offer_sdp,
                           unsigned long long lifetime_ms,
                           const char *invitee_or_empty,
                           unsigned long long op_id);
char *mx_rust_calls_answer(void *client,
                           const char *room_id,
                           const char *call_id,
                           const char *party_id,
                           const char *answer_type,
                           const char *answer_sdp,
                           unsigned long long op_id);
char *mx_rust_calls_reject(void *client,
                           const char *room_id,
                           const char *call_id,
                           const char *party_id,
                           unsigned long long op_id);
char *mx_rust_calls_hangup(void *client,
                           const char *room_id,
                           const char *call_id,
                           const char *party_id,
                           const char *reason,
                           unsigned long long op_id);
char *mx_rust_calls_select_answer(void *client,
                                  const char *room_id,
                                  const char *call_id,
                                  const char *party_id,
                                  const char *selected_party_id,
                                  unsigned long long op_id);
char *mx_rust_calls_rtc_decline(void *client,
                                const char *room_id,
                                const char *notification_event_id,
                                unsigned long long op_id);
/* Media-capable mode: only when enabled do call_invite/call_answer poll
 * events carry offer_sdp/answer_sdp (bounded; C++ stores them memory-only
 * and single-shot, never logs, never exposes to QML). Off by default. */
char *mx_rust_calls_set_media_capable(void *client, unsigned char capable);
/* ICE candidates (media-capable mode only in BOTH directions; pure ICE —
 * host IPs — so nothing crosses without an engine to consume it). */
char *mx_rust_calls_candidates(void *client,
                               const char *room_id,
                               const char *call_id,
                               const char *party_id,
                               const char *candidates_json,
                               unsigned long long op_id);
/* Homeserver TURN servers (/voip/turnServer). Result event
 * call_turn_servers {op_id, ok, username, password, uris[], ttl_seconds}
 * carries short-lived credentials: engine-only, never logged. */
char *mx_rust_calls_turn_servers(void *client, unsigned long long op_id);
/* MatrixRTC (MSC4143) — the Matrix half of modern calling. Observation and
 * discovery only in this round: nothing here publishes membership, because
 * advertising a joinable session without a media stack tells every other
 * client in the room to attempt an SFU connection that cannot complete.
 *
 * mx_rust_rtc_session   -> rtc_session {op_id, room_id, member_count,
 *                          slot_present, slot_closed, focus, members[]}
 * mx_rust_rtc_transports-> rtc_transports {op_id, room_id, server_answered,
 *                          category, server_transports[], participant_focus}
 * mx_rust_rtc_notify    -> rtc_send_result {op_id, ok, category, event_id}
 *
 * Membership changes are announced as a payload-free rtc_session_changed
 * {room_id} poke; answer it by re-reading the session, so remote and local
 * changes converge on one parse path. Inbound MSC4075 notifications arrive
 * on the existing call_rtc_notification lane with rtc=true. */
char *mx_rust_rtc_session(void *client,
                          const char *room_id,
                          unsigned long long op_id);
char *mx_rust_rtc_transports(void *client,
                             const char *room_id_or_empty,
                             unsigned long long op_id);
char *mx_rust_rtc_notify(void *client,
                         const char *room_id,
                         const char *notification_type,
                         const char *intent,
                         unsigned long long lifetime_ms,
                         const char *membership_event_id_or_empty,
                         unsigned long long op_id);
/* Raised hands, in element-call's own wire format (read out of
 * src/reactions/useReactionsSender.tsx and ReactionsReader.ts, not chosen
 * here): raising sends an m.reaction annotating the sender's OWN
 * m.call.member state event with U+1F590 U+FE0F, and lowering REDACTS that
 * reaction. membership_event_id is required to raise, reaction_event_id to
 * lower.
 *
 * mx_rust_rtc_set_hand   -> rtc_hand_result {op_id, ok, raised, category,
 *                           event_id}  — on a successful raise event_id is
 *                           the reaction the lower must redact.
 * mx_rust_rtc_read_hands -> rtc_hands {op_id, room_id, hands[]} — the
 *                           join-time sweep, since a hand raised before we
 *                           arrived produces no sync event for us.
 *
 * Live changes arrive as rtc_hand_changed {room_id, sender, raised,
 * membership_event_id (raise only), reaction_event_id}. */
char *mx_rust_rtc_set_hand(void *client,
                           const char *room_id,
                           const char *membership_event_id_or_empty,
                           const char *reaction_event_id_or_empty,
                           unsigned char raised,
                           unsigned long long op_id);
char *mx_rust_rtc_read_hands(void *client,
                             const char *room_id,
                             unsigned long long op_id);
/* MatrixRTC phase 2 — publishing our own membership. Publish arms an
 * MSC4140 DELAYED retraction that the client restarts periodically, so a
 * crash removes the membership instead of leaving a phantom participant
 * until `expires`. An empty delay_id in the answer means the server has no
 * MSC4140 and cleanup falls back to `expires` alone.
 * Answers: rtc_membership_published / rtc_membership_retracted /
 * rtc_delayed_updated. */
char *mx_rust_rtc_publish_membership(void *client,
                                     const char *room_id,
                                     const char *focus_url_or_empty,
                                     const char *intent,
                                     unsigned long long op_id);
char *mx_rust_rtc_restart_delayed_leave(void *client,
                                        const char *delay_id,
                                        unsigned long long op_id);
char *mx_rust_rtc_retract_membership(void *client,
                                     const char *room_id,
                                     const char *delay_id_or_empty,
                                     unsigned long long op_id);
/* MatrixRTC phase 2 — call media E2EE key distribution.
 * The key is generated on the C++ side (OpenSSL) and handed here ONLY to be
 * Olm-encrypted per device; it is never logged, never enqueued back, and
 * never reaches QML. Inbound keys arrive as rtc_key_received {room_id,
 * sender, claimed_device_id, key_index, key} — `sender` is what the SDK
 * vouches for after Olm decryption; the claimed device id is a CLAIM. */
char *mx_rust_rtc_send_media_key(void *client,
                                 const char *room_id,
                                 const char *key_base64,
                                 unsigned char key_index,
                                 const char *targets_json,
                                 unsigned long long op_id);
/* MatrixRTC phase 2 — LiveKit SFU signalling. Media stays on the C++
 * GStreamer engine; this is signalling only. Authorization uses a Matrix
 * OpenID token, so the access token never reaches the SFU, and the SFU's
 * JWT never crosses this boundary.
 *
 * Poll events: sfu_state {generation, state, category}, sfu_joined
 * {identity, participants[], ice_servers[]}, sfu_participants,
 * sfu_track_published, sfu_speakers, sfu_quality, sfu_server_mute.
 * sfu_remote_description {kind, target, sdp} and sfu_remote_candidate
 * {target, candidate_init} cross ONLY in media-capable mode.
 * `target` is "publisher" (our own tracks) or "subscriber" (everyone
 * else's) — LiveKit runs two peer connections and confusing them wires
 * audio the wrong way. */
char *mx_rust_sfu_connect(void *client,
                          const char *service_url,
                          const char *room_id,
                          unsigned long long op_id);
char *mx_rust_sfu_local_description(void *client,
                                    const char *kind,
                                    const char *target,
                                    const char *sdp);
char *mx_rust_sfu_local_candidate(void *client,
                                  const char *target,
                                  const char *candidate_init_json);
char *mx_rust_sfu_add_track(void *client,
                            const char *cid,
                            const char *name,
                            int kind_audio0_video1,
                            /* Video only (0 for audio). A video track declared
                             * with no size and no layer leaves the SFU to
                             * infer the track's shape, and it infers
                             * three-layer simulcast. */
                            unsigned int width,
                            unsigned int height,
                            unsigned char screen_share,
                            /* Declares LiveKit Encryption::GCM on the track,
                             * which is how a receiving client (Element Call
                             * included) knows to run its frame decryptor. */
                            unsigned char encrypted);
char *mx_rust_sfu_mute_track(void *client,
                             const char *sid,
                             unsigned char muted);
char *mx_rust_sfu_disconnect(void *client);
/* v0.7.x UIA + device sign-out. delete may raise a uia_required challenge
 * event ({op_id, flows, completed, has_password_stage, wrong_password});
 * answer with mx_rust_uia_submit_password (the password transit buffer is
 * scrubbed inside Rust) or abandon with mx_rust_uia_cancel. Terminal
 * result: device_delete_result { op_id, ok, category }. */
char *mx_rust_delete_devices(void *client,
                             const char *device_ids,
                             unsigned long long op_id);
char *mx_rust_uia_submit_password(void *client,
                                  unsigned long long uia_id,
                                  const char *password);
char *mx_rust_uia_cancel(void *client, unsigned long long uia_id);
/* MAS/OAuth accounts manage sessions in the account console instead of
 * password UIA. device_id "" = sessions list, else that device's delete
 * page. Result: oauth_management_url { op_id, ok, url }. */
char *mx_rust_oauth_management_url(void *client,
                                   const char *device_id,
                                   unsigned long long op_id);
/* v0.7.x server-side message search (POST /_matrix/client/v3/search).
 * Unencrypted rooms ONLY — the server cannot search ciphertext, and the UI
 * must disclose that. room_id empty = all rooms; next_batch pages.
 * Result: message_search_result. */
char *mx_rust_search_messages(void *client,
                              const char *term,
                              const char *room_id,
                              const char *next_batch,
                              const char *filters_json,
                              unsigned long long limit,
                              unsigned long long op_id);
/* List a Space's children, joined and unjoined, via the SDK's
 * /hierarchy-backed SpaceRoomList. Bounded; reports truncated. */
char *mx_rust_space_children(void *client,
                             const char *space_id,
                             unsigned long long op_id);
char *mx_rust_add_room_to_space(void *client,
                                const char *space_id,
                                const char *room_id,
                                unsigned long long op_id);
/* 2026-08-19: toggles the MSC1772 `suggested` flag on an EXISTING child
 * (via list and order key preserved; a non-child is refused). Result:
 * space_child_suggested_result { op_id, space_id, room_id, suggested, ok }. */
char *mx_rust_set_space_child_suggested(void *client,
                                        const char *space_id,
                                        const char *room_id,
                                        int suggested,
                                        unsigned long long op_id);
/* v0.7: MSC1772 child removal (empty-via m.space.child); never leaves or
 * deletes the child room. Result: space_child_removed_result. */
char *mx_rust_remove_room_from_space(void *client,
                                     const char *space_id,
                                     const char *room_id,
                                     unsigned long long op_id);

/*
 * Attachment sending through the SDK timeline send queue. The SDK creates
 * a local echo, drives sending → sent/failed through the normal timeline
 * diff stream, encrypts transparently in encrypted rooms, and supports
 * retry via the existing unwedge path. width/height carry image metadata
 * (0 when unknown); `animated` marks animated GIF images.
 */
char *mx_rust_timeline_send_attachment(void *client,
                                       const char *room_id,
                                       const char *local_path,
                                       const char *mime,
                                       const char *caption,
                                       unsigned long long width,
                                       unsigned long long height,
                                       int animated,
                                       unsigned long long op_id);
/* v0.7: send a video WITH a poster thumbnail Lightning extracted from the
 * outgoing file itself. thumb_data/thumb_len may be NULL/0 — the video then
 * sends with no poster, exactly like the plain attachment path. The bytes
 * are re-validated by magic sniffing on the Rust side; a poster that fails
 * validation is dropped and the video still sends. The SDK uploads the
 * poster as its own media request and encrypts it alongside the payload in
 * an encrypted room, then fills thumbnail_url / thumbnail_file and
 * thumbnail_info on the outgoing m.video event. duration_ms may be 0. */
char *mx_rust_timeline_send_video(void *client,
                                  const char *room_id,
                                  const char *local_path,
                                  const char *mime,
                                  const char *caption,
                                  unsigned long long width,
                                  unsigned long long height,
                                  unsigned long long duration_ms,
                                  const unsigned char *thumb_data,
                                  size_t thumb_len,
                                  unsigned long long thumb_width,
                                  unsigned long long thumb_height,
                                  unsigned long long op_id);
/* v0.7: MSC3245 voice message. waveform: 0..=100 amplitudes (may be NULL /
 * empty; at most 1024 entries). The SDK adds the voice marker + duration/
 * waveform block and sends via the normal (encrypting) attachment path. */
char *mx_rust_timeline_send_voice(void *client,
                                  const char *room_id,
                                  const char *local_path,
                                  const char *mime,
                                  unsigned long long duration_ms,
                                  const unsigned char *waveform,
                                  size_t waveform_len,
                                  unsigned long long op_id);
/* v0.7 thread parity: the thread twin of mx_rust_timeline_send_voice. Same
 * MSC3245 metadata and the same 1024-entry waveform bound, routed through
 * the thread-focused SDK timeline so the event carries a real m.thread
 * relation. Never falls back to an ordinary room send. */
char *mx_rust_thread_send_voice(void *client,
                                const char *room_id,
                                const char *root_event_id,
                                const char *local_path,
                                const char *mime,
                                unsigned long long duration_ms,
                                const unsigned char *waveform,
                                size_t waveform_len,
                                unsigned long long op_id);
/* Clipboard image path: one bounded byte copy, no temporary file on disk. */
char *mx_rust_timeline_send_attachment_bytes(void *client,
                                             const char *room_id,
                                             const unsigned char *data,
                                             size_t len,
                                             const char *filename,
                                             const char *mime,
                                             unsigned long long width,
                                             unsigned long long height,
                                             unsigned long long op_id);
/* v0.6.1: send an attachment INTO a thread. Routed through the SDK's
 * thread-focused timeline, so the SDK attaches the m.thread relation (and
 * reply-to fallback) and encrypts for encrypted rooms — never an ordinary
 * room message, never hand-built relation JSON. Result echoes on the
 * attachment_send_result poll event by op_id, exactly like the room path. */
char *mx_rust_thread_send_attachment(void *client,
                                     const char *room_id,
                                     const char *root_event_id,
                                     const char *local_path,
                                     const char *mime,
                                     const char *caption,
                                     unsigned long long width,
                                     unsigned long long height,
                                     int animated,
                                     unsigned long long op_id);
/* v0.7: the thread twin of mx_rust_timeline_send_video — same poster
 * handling, routed through the SDK's thread-focused timeline. */
char *mx_rust_thread_send_video(void *client,
                                const char *room_id,
                                const char *root_event_id,
                                const char *local_path,
                                const char *mime,
                                const char *caption,
                                unsigned long long width,
                                unsigned long long height,
                                unsigned long long duration_ms,
                                const unsigned char *thumb_data,
                                size_t thumb_len,
                                unsigned long long thumb_width,
                                unsigned long long thumb_height,
                                unsigned long long op_id);
char *mx_rust_thread_send_attachment_bytes(void *client,
                                           const char *room_id,
                                           const char *root_event_id,
                                           const unsigned char *data,
                                           size_t len,
                                           const char *filename,
                                           const char *mime,
                                           unsigned long long width,
                                           unsigned long long height,
                                           unsigned long long op_id);

/*
 * Media retrieval. `key` is the item's media_key from timeline payloads
 * (event id, or SDK unique id for local echoes). kind: 0 = full file,
 * 1 = thumbnail (falls back to full when none exists). Encrypted media is
 * decrypted inside the SDK; bytes are parked in Rust and handed over via
 * mx_rust_media_take after the matching media_ready event — never through
 * the JSON queue.
 */
char *mx_rust_media_fetch(void *client,
                          const char *key,
                          unsigned int kind,
                          unsigned long long op_id,
                          /* 0 standard(40s) 1 playable(90s) 2 save(270s) —
                           * each strictly below its C++ watchdog class. */
                          unsigned int timeout_class);
/* Server-side thumbnail of a PLAIN mxc URI (avatars). kind is 2 on events. */
char *mx_rust_media_fetch_mxc(void *client,
                              const char *mxc,
                              unsigned long long width,
                              unsigned long long height,
                              unsigned long long op_id);
/*
 * Cancel an in-flight media fetch by op id: aborts the download task and
 * drops any parked bytes. Idempotent; no terminal event is emitted for a
 * cancelled op (the caller already released its slot).
 */
void mx_rust_media_cancel(void *client, unsigned long long op_id);
/*
 * Move a parked media payload out of the bridge. Returns NULL for unknown
 * (stale / already-taken) op ids; otherwise a buffer of *out_len bytes that
 * MUST be released with mx_rust_media_free (never free()).
 */
unsigned char *mx_rust_media_take(void *client,
                                  unsigned long long op_id,
                                  size_t *out_len);
void mx_rust_media_free(unsigned char *data, size_t len);

/* Emits one upload_limit event with the server's m.upload.size. */
char *mx_rust_fetch_upload_limit(void *client);

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
