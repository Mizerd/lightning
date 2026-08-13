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
char *mx_rust_timeline_close(void *client);
char *mx_rust_timeline_paginate_back(void *client,
                                     const char *room_id,
                                     unsigned short count);
/* `mention_user_ids` (v0.7) is a nullable newline-separated list of full MXIDs
 * placed in m.mentions; NULL/empty attaches no mentions. The body already
 * carries the matrix.to markdown links for those users. */
char *mx_rust_timeline_send_text(void *client,
                                 const char *room_id,
                                 const char *body,
                                 const char *mention_user_ids);
char *mx_rust_timeline_send_reply(void *client,
                                  const char *room_id,
                                  const char *in_reply_to_event_id,
                                  const char *body,
                                  const char *mention_user_ids);
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
                               const char *mention_user_ids);
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
                            const char *mention_user_ids);
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
char *mx_rust_add_room_to_space(void *client,
                                const char *space_id,
                                const char *room_id,
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
