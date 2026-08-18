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

## Validation

- Rust: `calls::tests` (closed-set sanitizers, SDP requirement, clamps).
- C++: `call-controller` suite (20 cases: ringing, glare both directions,
  bounded busy auto-reject, idempotent re-delivery, ignored-sender drop,
  stale-op isolation, live targeted-invite filter, expiry, cross-device
  settlement, LRU absorption, refusals, logout).
- Live interoperability (Element rings Lightning, Lightning's decline
  stops it, encrypted-room call events decrypt at the peer): **NOT
  TESTED** — no live pass has been run; see
  `docs/element-interop-checklist.md` for where such a pass gets recorded.
