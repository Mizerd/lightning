# Voice calls — signaling backend (2026-08-18)

Status: **internal pipes only.** No UI, no media. This document records what
exists, what is deliberately absent, and the contract a future media backend
plugs into.

## What was built

Legacy 1:1 Matrix VoIP signaling (MSC2746, `m.call.*` version 1) plus a
narrow MatrixRTC lane (`m.rtc.notification` observed, `m.rtc.decline` sent),
end to end through Lightning's normal backend architecture:

- **Rust (`rust/src/calls.rs`)** — send helpers for
  `m.call.invite/answer/reject/hangup/select_answer` built from ruma's
  version-1 constructors, and `m.rtc.decline` built by the SDK itself
  (`Room::make_decline_call_event`). Inbound observation registers typed
  `Client::add_event_handler`s for the six signaling kinds plus
  `m.rtc.decline`, held behind `EventHandlerDropGuard`s bound to the sync
  loop (`run_authoritative_sync`), so handlers can never outlive their
  session. Everything crosses the FFI as sanitized poll events
  (`call_invite`, `call_answer`, `call_hangup`, `call_reject`,
  `call_select_answer`, `call_rtc_notification`, `call_rtc_decline`,
  `call_send_result`).
- **FFI** — `mx_rust_calls_*` in `rust/include/matrix_rust.h`, same
  op-id/poll-lane contract as every other subsystem.
- **C++** — `CallSignal` (`src/matrix/CallSignal.h`, structurally SDP-free),
  six `MatrixClient` virtuals (non-capable backends return 0), decode in
  `RustSdkMatrixClient::handleRoomCommandEvent`, and the state machine in
  `src/calls/CallController.*`:
  `Idle → Inviting → Connecting → Active` (outbound) /
  `Idle → Ringing` (inbound) / `Ended(reason)`, with:
  - MSC2746 glare: lexicographically smaller `call_id` survives (matches
    matrix-js-sdk); the loser is hung up with reason `replaced`.
  - Fresh random `party_id` per call (never the device id); the first
    remote answer locks the party and `m.call.select_answer` names it once.
  - Cross-device settlement: our own answer/reject/hangup/`m.rtc.decline`
    from another device ends the local ring as Answered/DeclinedElsewhere,
    sending nothing.
  - Invite lifetime timers clamped against clock skew; an already-expired
    invite (cold-start backlog) never rings and sends nothing.
  - Busy: a second invite during a live session is auto-rejected without
    touching the session — BOUNDED per session (at most 8), because the
    reject is a remotely triggered send with zero user interaction; beyond
    the cap unsolicited invites are dropped silently. Re-delivery of the
    live session's own invite is idempotent. A concurrent
    `m.rtc.notification` is deliberately NOT auto-declined — a decline
    stops the ring on every one of the user's devices, so it must remain
    a user action.
  - A bounded LRU (32) of finished call ids absorbs late events; cleared on
    sign-out so the next account starts clean.
  - Ring POLICY is separate from ring STATE: `shouldRing()` consults
    backlog suppression (defaults CLOSED until a lifecycle owner opens
    it) and a room-mute check; a muted room still produces `Ringing`
    state. An IGNORED sender is stronger than policy: their invites and
    notifications are dropped before any state or any send — an ignored
    user must not be able to elicit a wire event that confirms we are
    online.
  - The local user for targeted-invite filtering is resolved live from the
    client (`currentUserId()`), so account switches are followed without
    extra wiring.

## What is deliberately absent, and why

- **Media.** No WebRTC, no SDP generation, no ICE, no audio device. The
  SDP parameters on the send pipes are *required opaque inputs* — an empty
  SDP is refused synchronously; there is no stub value. `placeCall()`
  refuses with `no_media_backend`; `placeCallWithOffer()` is the complete
  outbound pipe a media backend feeds (tests feed it a synthetic SDP).
- **`m.call.candidates` / `m.call.negotiate` /
  `m.call.sdp_stream_metadata_changed`** — pure ICE/SDP payloads (raw host
  IPs) with no consumer; neither sent nor observed.
- **MatrixRTC membership (`m.call.member`, MSC3401).** Publishing joinable
  session membership without a media stack tells every client in the room
  to attempt an SFU connection that can never complete — a lie on the
  wire, not a stub. The notification/decline lane gives real interop with
  Element X ringing without publishing anything false.
- **`experimental-widgets` / Element Call embedding** — a webview product
  decision, off in `rust/Cargo.toml`, stays off.
- **Answering.** `mx_rust_calls_answer` exists so the signaling layer is
  complete, but `CallController` exposes no `answer()` — it cannot produce
  an answer SDP honestly.
- **Notification/ring sounds, call history, any QML.** Later rounds.
  `NotificationManager` will subscribe to `incomingCallStarted/Ended` and
  `shouldRing()`.

## Security and privacy rules

- **SDP never crosses the FFI, is never logged, never enqueued, never
  stored.** Poll events carry `has_offer`/`has_answer` booleans and a
  sanitized session type from a closed set. `CallSignal` has no SDP
  member, so QML exposure is structurally impossible.
- **Inbound `call_id`/`party_id`/`selected_party_id` are SENDER-CHOSEN
  opaque text** (ruma validates nothing about a VoipId). They are bounded
  at the Rust edge (length cap, control characters refused — a failing id
  drops the whole event) and are compared, never logged or rendered.
- Hangup reasons are a closed set both directions; `_Custom` free text
  collapses to `unknown` inbound and only `replaced` is emitted outbound.
- Call events in encrypted rooms ride SDK encryption automatically
  (`Room::send`). Note honestly: `m.call.*` metadata — that a call
  happened, when, between whom — is visible to the server regardless;
  encryption hides the SDP body, not the fact of the call.
- Every send is gated on `RoomState::Joined` in Rust. Errors cross as
  coarse categories only.
- No dependency, `Cargo.toml`, or lock-file change.

## Known gaps (recorded, not defects)

- An event handler only fires for rooms sliding sync is currently
  delivering; a call in a room outside the list window will not ring until
  push notifications carry it.
- Legacy `m.call.*` is on a deprecation path toward MatrixRTC. The state
  machine consumes transport-neutral `CallSignal`s keyed on
  `(room, call_id, party_id)`, so a MatrixRTC session lane can be attached
  under it without rebuilding the machine.

## Round 2 (same day): policy wiring, the incoming-call experience, and the media seam

- **Ring policy is wired to its real owners** (`AppController`): ignored
  senders via `ModerationController::isIgnored`, muted rooms via
  `SettingsManager::roomNotificationMode == Muted`, and backlog
  suppression from `MatrixClient::initialSyncDone` (edge-connected). The
  hooks are no longer inert defaults.
- **Incoming-call notification + ring** (`NotificationManager`): one
  freedesktop notification with a **Decline action** (routed back to
  `CallController::rejectIncoming`), re-delivered every 5 s via
  `replaces_id` with the themed `phone-incoming-call` sound while ringing
  — the closest honest "ring" the notification API offers; Lightning
  bundles no audio and plays none itself. Dismissing the card stops the
  local re-ring but declines nothing. Stopped on call end; a missed ring
  (`InviteTimeout` / `RemoteHangup` while ringing) raises a "Missed call"
  notice routed to the room, respecting mute/preview-privacy/enablement.
  The ring sound is gated by the new global setting
  `Settings → Notifications → "Ring for incoming voice calls"`
  (`SettingsManager::ringForCalls`, default ON) and the existing sound
  mode; the notification itself only by `notificationsEnabled`.
- **Incoming-call corner card** (`qml/IncomingCallPrompt.qml`, hosted
  above the passive prompts in Main.qml's corner column): shows call
  STATE (`app.calls.ringing`) regardless of sound policy, names the
  caller (localpart only), offers **Decline** (the wire action) and
  Dismiss (local hide), and honestly says answering on this device isn't
  supported yet. No answer affordance may appear until a media engine
  exists (contract-tested).
- **The media seam is complete** (`CallMediaBackend.h` + `SdpStore.h`):
  `placeCall()` runs the full outbound pipe when a backend is registered
  (offer production bounded at 15 s → invite → answer → `select_answer` →
  `setRemoteAnswer` → Connecting → `connected()` → Active), `answer()`
  exists and runs the inbound pipe (remote-offer take → answer production
  → `m.call.answer` → Connecting → Active), and media failure announces
  `user_media_failed` on the wire only when the peer could be waiting.
  **SDP transport is opt-in end to end**: only
  `mx_rust_calls_set_media_capable(true)` — called exactly when a backend
  registers, which production never does today — makes the Rust handlers
  attach `offer_sdp`/`answer_sdp` (bounded 128 KiB) to the poll payloads,
  where the bridge moves them into the bounded (8), single-shot,
  memory-only `calls::SdpStore`, wiped on sign-out/detach. CallSignal
  stays structurally SDP-free; nothing logs any of it (the `login_ok`
  access-token discipline applies).
- Review corrections folded in before commit: the ring's duration follows
  the invite's real remaining lifetime (no fixed 60 s window); missed
  classification happens at end-of-session from the PRIOR state (an
  answered call the peer hung up is completed, never "missed") and
  additionally requires the ring to have been announced; hangup() can end
  an answered inbound call; an offer still in production never produces a
  wire hangup (glare or local); the SDP store wipes on EVERY teardown
  path via clearLocalState and drops a call's unconsumed offer at end;
  media-capable mode survives handle recreation and gates the C++ insert
  side too; the backend pointer is a QPointer and a live call's job is
  closed on backend swap; ring announcements have a 30 s per-sender
  cooldown; the new notification bodies HTML-escape member-chosen text
  (the pre-existing invite/verification bodies are a recorded follow-up).
- Still absent, still deliberate: any real media engine, candidates/ICE,
  MatrixRTC membership, answering in production (the card says so).

## Validation

- Rust: `calls::tests` (closed-set sanitizers, SDP requirement, clamps,
  media-capable gating of carried SDP).
- C++: `call-controller` (state machine incl. the full outbound and
  inbound media cycles under a FakeMediaBackend, answered-inbound hangup,
  prior-state missed classification, undispatched-invite silence, media
  timeouts/failures, SdpStore bounds), `call-ring-policy` (the production
  wiring of every ring gate on a full AppController, the announced-ring
  gate on missed notices, the per-sender ring cooldown),
  `call-ui-contract` (corner-card contracts + a real offscreen
  instantiation driven through a live ring/decline), and the
  `notification-manager` call-ring cases (timer lifecycle, id-matched
  decline/closed handling, replacement, deadline, payload promotion).
- Live interoperability (Element rings Lightning, the desktop
  notification and corner card appear, Decline stops Element's ring,
  missed-call notices, the themed ring sound on a real notification
  daemon, encrypted-room call events decrypting at the peer): **NOT
  TESTED** — no live pass has been run; see
  `docs/element-interop-checklist.md` for where such a pass gets recorded.
