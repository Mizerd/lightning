# Voice calls — signaling backend (2026-08-18)

> **See also `docs/matrixrtc.md`** (2026-08-23) for the MatrixRTC lane —
> membership observation, transport discovery, the room call banner, and the
> mute/deafen controls this document's engine gained. One correction to the
> claim below: the `m.rtc.notification` handler described here uses ruma's
> **stable** `m.rtc.notification` type, and current Element sends
> `org.matrix.msc4075.rtc.notification`, so it never fired for a current
> Element ring. That is fixed in the MatrixRTC module, not here.

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

## Round 3 (same day): the REAL media engine — GStreamer webrtcbin

Voice calls now actually place, ring, answer, and carry audio, on builds
that have the engine.

- **Engine**: `src/calls/GstCallMediaBackend.{h,cpp}` implements the
  `CallMediaBackend` seam over GStreamer's `webrtcbin` — full WebRTC (ICE
  via libnice, DTLS-SRTP, Opus), audio-only. One pipeline per call:
  `autoaudiosrc → opusenc → rtpopuspay → webrtcbin`, receive pads →
  `rtpopusdepay → opusdec → autoaudiosink`. Test-tone mode substitutes
  `audiotestsrc`/`fakesink` so headless CI runs REAL handshakes with no
  audio devices. GStreamer callbacks marshal onto the Qt thread behind an
  alive-registry; promise contexts pin the webrtcbin and the call id, and
  every Qt-side handler re-checks the live session, so late callbacks for
  closed calls are silent no-ops.
- **Build/runtime gating**: `LIGHTNING_ENABLE_WEBRTC` (AUTO — pkg-config
  probe for gstreamer-1.0/webrtc/sdp; `HAVE_LIGHTNING_WEBRTC`). The engine
  additionally re-probes its ~17 required element factories at RUNTIME
  (`runtimeAvailable`) before AppController ever registers it, and
  `LIGHTNING_DISABLE_WEBRTC=1` is a kill switch. A build or machine
  without the plugins keeps the honest signaling-only refusal. The dev
  shell now carries gst-plugins-base/good/bad and libnice (whose
  GStreamer plugin joins `GST_PLUGIN_SYSTEM_PATH_1_0`). **Packaging
  follow-up (lightning-deploy)**: official packages do not yet declare
  the GStreamer/libnice runtime deps, so packaged builds stay
  signaling-only until that lands — by design, not by accident.
- **ICE candidates** (`m.call.candidates`) now flow BOTH ways, media-capable
  mode only (pure ICE = host IPs; without an engine nothing crosses):
  inbound handler → bounded entries (32/event, 1024/line, control-free) →
  `callCandidatesReceived` → CallController → engine (buffered until the
  remote description applies); outbound engine candidates are batched
  (150 ms) into `m.call.candidates` sends with MSC2746's empty
  end-of-candidates marker on gathering completion.
- **TURN**: `/voip/turnServer` via ruma `get_turn_server_info`; the
  short-lived credentials cross the FFI once, cached in CallController
  (memory only, TTL-refreshed), and applied to the engine
  (`stun-server` / `add-turn-server`, credentials percent-encoded). The
  engine contacts ONLY servers the homeserver names — no third-party STUN
  that would leak the user's IP. No TURN answer = host candidates only.
- **UI**: the corner card is now the whole call surface — Calling…/
  Incoming/Connecting…/In-call forms with Accept (ONLY when the engine is
  registered), Decline, Hang up, Dismiss; states are compared
  symbolically (`CallController.Ringing` — QML_ELEMENT/UNCREATABLE, the
  PaginationController precedent) and the accessible name follows the
  visible title. The room header gains a `startVoiceCallButton` gated to
  **1:1 DMs** (a legacy invite rings every room member) plus the engine
  gate, contract-enforced.
- **Review corrections folded in before commit** (four-lens §18 pass):
  every GStreamer callback carries the EMITTING element's pointer as a
  session token checked against the live session on delivery — the
  engine is one object reused call after call, and a queued event from a
  closed call must never be attributed to the next one (offers,
  candidates, gathering-complete, connection state, bus errors alike);
  the TURN pre-fetch fires when the client/backend PAIR completes (the
  registration-order gap meant the FIRST call of a session ran without
  relay servers, masked by a test using the reverse order — both fixed);
  the TURN cache resets on client change; homeserver-supplied ICE uris
  are sanity-filtered (length, control chars, '@', '/') before being
  assembled into credential-bearing URIs; the bus sync handler DROPS
  messages after inspection (nothing drained the async queue); engine
  registration moved out of the AppController constructor into
  `enableCallMediaEngine()`, called only by main.cpp, so the offscreen
  test fleet never gst_inits or gains a media engine it didn't ask for.
- **Recheck corrections (second §18 pass, GStreamer 1.26.11 sources
  consulted)**: the ANSWER path now reuses the OFFERER's Opus payload
  number (RFC 3264) extracted from the remote offer's rtpmap — our own
  offers keep 111, the ecosystem convention; candidates the caller
  trickles while the callee is still RINGING are buffered in the
  controller and drained into the engine at Accept (previously dropped —
  the realistic inbound-call killer, since callers trickle immediately
  and humans answer slowly); local candidate bursts are chunked to the
  wire cap (32/event) so a fat burst is never rejected whole; a TURN
  fetch stranded by poll-queue overflow times out and retries; the TURN
  response is defensively bounded (16 uris, 1 KiB credentials) and the
  ttl clamped at both ends of the FFI; gst_bin_add's failure mode
  (element finalized) is handled; the in-call card (the app's only Hang
  Up) now follows the user into Settings — only the RINGING form stays
  chat-shell-scoped.

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
- **`call-media-loopback` (round 3): a REAL WebRTC call in-process** —
  two engines exchange SDP + trickled candidates exactly as
  CallController wires them over Matrix and reach CONNECTED: genuine ICE
  over loopback host candidates, genuine DTLS-SRTP, Opus RTP flowing;
  plus clean teardown/recycling and honest failure on garbage SDP. SKIPs
  (never fails) on trees without the plugins.
- Live interoperability (Element ⇄ Lightning ringing, ANSWERING a real
  call across the network with audible audio both ways, TURN traversal on
  a real homeserver, Decline stopping Element's ring, missed-call
  notices, the ring sound on a real daemon, encrypted-room call events
  decrypting at the peer): **NOT TESTED** — the loopback suite proves the
  engine and the handshake, not the network or another client; see
  `docs/element-interop-checklist.md`.
