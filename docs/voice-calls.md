# Voice calls — signaling backend (2026-08-18)

> **See also `docs/matrixrtc.md`** (2026-08-23) for the MatrixRTC lane —
> membership observation, transport discovery, the room call banner, and the
> mute/deafen controls this document's engine gained. One correction to the
> claim below: the `m.rtc.notification` handler described here uses ruma's
> **stable** `m.rtc.notification` type, and current Element sends
> `org.matrix.msc4075.rtc.notification`, so it never fired for a current
> Element ring. That is fixed in the MatrixRTC module, not here.
>
> **The two lanes have diverged sharply since, and this matters when reading
> any status line below.** The MatrixRTC/SFU lane is the one that carries real
> media against Element today; the legacy 1:1 `m.call.*` lane described here
> has a real engine and has never been live-validated against another client.
> Anything in this document about capture elements, publish caps, videoscale
> or screen sharing belongs to the SFU engine, and `docs/matrixrtc.md` is
> authoritative for it.

Status, by round rather than as one word — this document is written in
ROUNDS, and each round's own status line was true when it was written:

* **Signalling (rounds 1-2):** implemented, plus the incoming-call surface.
* **Media (round 3):** a real GStreamer `webrtcbin` engine — audio-only, 1:1.
  Builds without the plugins keep the honest signalling-only refusal.
* **Live validation of the legacy lane: NOT TESTED**, still. No answered 1:1
  call has ever been placed across a network, and the incoming-call prompt's
  Accept is a currently OPEN defect (CLAUDE.md §16). The loopback suite proves
  the engine and the handshake, not the network and not another client.
* **Packaging:** Windows and macOS packages BUNDLE GStreamer as of
  2026-08-26, and the Linux formats declare or stage the plugins. The
  "packaged builds stay signalling-only" note in round 3 below is superseded;
  see `docs/matrixrtc.md` §Packaging.

The rest of this document records what exists, what is deliberately absent,
and the contract the media backend plugs into.

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
  follow-up (lightning-deploy) — SUPERSEDED 2026-08-26**: this said official
  packages did not declare the GStreamer/libnice runtime deps, so packaged
  builds stayed signaling-only. They now do: deb `CALL_DEPENDS`, rpm
  `Requires:`, AppImage staging plus an AppRun hook, and Windows and macOS
  BUNDLE GStreamer beside the executable with `--call-media-status` run from
  inside the package to prove the engine finds it. See
  `docs/matrixrtc.md` §Packaging and §"Windows and macOS".
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

## Round 6: frame encryption actually attached, and LiveKit wire corrections

Until this round the frame cryptor was a tested library nothing called, and
an encrypted room could not be joined at all. That was honest but useless.
The cryptor is now attached to the pipeline, so an encrypted room joins and
screen sharing works there.

### Where encryption happens, and why exactly there

Send side: a pad probe on the **encoder's src pad** — after encoding, before
RTP payloading. Receive side: a probe on the **depayloader's src pad** —
after the frame is whole again, before decoding.

That placement is the whole interoperability story. LiveKit and Element Call
encrypt one **encoded frame**; a frame spans many RTP packets, so encrypting
per packet would be a different scheme that interoperates with nobody while
looking perfectly reasonable in a capture. The cleartext header (VP8 keyframe
10 bytes, VP8 delta 3, Opus 1) is what lets the SFU keep routing and
detecting keyframes without holding a key.

Keyframe detection comes from `GST_BUFFER_FLAG_DELTA_UNIT` rather than from
parsing the VP8 bitstream: the encoder already told us, and re-deriving it
would be a second source of truth for the header size.

### The gate is real, not a label

`setEncryptionRequired(true)` is armed at join from the room's encryption
state, **before any media exists**. With it set and no key installed, the
probe returns `GST_PAD_PROBE_DROP`. There is no cleartext fallback anywhere
in the path — a decryption that fails its authentication tag drops too.
`encryptionActive()` reads whether a key is actually installed, so
`mediaEncrypted` cannot claim encryption that is not happening.

Room encryption is captured **once per call**, and UNKNOWN fails closed to
encrypted. A room-state change mid-call cannot quietly relax what the user
was already told.

### Key lifecycle

Keys are 32 bytes from `QRandomGenerator::system()` (getrandom(2)) — never
the generic generator, which is a PRNG. Distribution is Olm-encrypted
to-device, addressed **per device**: our own device is excluded, but our own
account on a second device is not, because that is a separate Olm session
that would otherwise be deafened.

Distribution happens **before** installation. The other order encrypts our
frames under a key nobody has yet, and every receiver drops them until the
to-device message lands. The first key is minted inside `publishTracks()`,
before the first frame can exist. Any change in the participant set rotates,
so a leaver stops being able to decrypt. Keys are cleared on teardown: they
must not outlive the call that used them.

### Two IV mistakes that were in my own first wiring

Both fixed here, and both are worth stating because neither is visible in a
working call:

1. **Both probes passed `ssrc=1`.** The cryptor keeps its send counter per
   SSRC, so audio and video shared one. Two frames with the same timestamp
   and counter under the same key produce the **same IV**, which for AES-GCM
   is a total break of both frames, not a weakening. Each encrypting track
   now takes a distinct IV stream id. The value need not be the real RTP
   SSRC: the IV travels inside the frame and livekit-client uses it verbatim
   without checking that field, so local uniqueness is the entire
   requirement.
2. **The cryptor had no lock.** One cryptor serves every track in a
   direction and GStreamer runs each track on its own streaming thread, so
   the counter `QHash` was a data race. Now a recursive mutex held across
   the whole frame, so the key ring cannot rotate out from under a frame
   between choosing the index and using the key.

### LiveKit wire corrections (interoperability)

Two things Element Call reads that we were getting wrong:

* **`TrackSource` was off by one in both directions.** Hand-written
  literals had camera=2, microphone=3, screen_share=4 against LiveKit's
  actual `CAMERA=1, MICROPHONE=2, SCREEN_SHARE=3, SCREEN_SHARE_AUDIO=4`.
  Our screen share therefore arrived at Element as `SCREEN_SHARE_AUDIO` — a
  track it treats as audio and never renders — and Element's camera arrived
  here as `unknown`. Now read through the generated enum, with the raw wire
  numbers pinned in a test: an assertion written in terms of the same enum
  would have passed against the defect.
* **`AddTrackRequest.encryption` was never set.** LiveKit carries E2EE
  per track and a receiving client decides whether to run its frame
  decryptor from it. Encrypting the bytes while declaring `NONE` renders as
  garbage at the far end. Now `GCM` whenever the room is encrypted, which
  is the same value Element Call publishes.

### Screen share, matched to livekit-client's own presets

Screen share is `ScreenSharePresets.h1080fps30` — 1920x1080, 30 fps,
3 Mbit/s — and camera is the h720 default, 1280x720 at 1.7 Mbit/s. Those are
ceilings expressed as caps **ranges** on the SIZE (the framerate is FIXED at
`30/1`, and the range was a bug — see `docs/matrixrtc.md`).

**CORRECTED 2026-08-27. The sentence that stood here — "so videoscale picks
the largest size inside them that keeps the display aspect ratio: an
ultrawide stays ultrawide instead of being stretched to 16:9" — was FALSE,
and it was false in the source comment too.** videoscale answers a size
ceiling by clamping BOTH axes and signalling the shape as a non-square
PIXEL ASPECT RATIO; VP8 carries no PAR and neither does the RTP payload, so a
libwebrtc receiver draws the frame at its literal width by height and the
ratio videoscale carefully preserved is thrown away. Measured: 3840x2160 ->
PAR 1/1 (16:9, which is why nobody ever saw this), 3840x2100 -> 36/35,
1920x1200 -> 9/10 (11% stretch), 3440x1440 -> 43/32 (34% squash); and
`ximagesrc` on a real desktop negotiates PAR 2/1 unpinned, a 2x squash at the
far end.

The fix is to pin `pixel-aspect-ratio=(fraction)1/1` into the ceiling, which
makes videoscale satisfy the ratio by choosing a SIZE (3440x1440 ->
1920x804, 1920x1200 -> 1728x1080, 800x600 unchanged — it never upscales). It
has a second-order hazard that must not be forgotten: pinning downstream
hands the SOURCE an open PAR range, and a source that does not fixate PAR
falls through to `gst_caps_fixate`, which takes a range's MINIMUM
(1/2147483647), after which videoscale dies of integer overflow. Hence a
fixed 1/1 capsfilter in front of the source as well. The full measurement,
the elements that are and are not safe, and why a `videotestsrc` probe cannot
see the hazard are in `docs/matrixrtc.md`.

The ceiling is the point. A screencast source is whatever the monitor is; on
a 4K display an uncapped pipeline asks VP8 to encode 3840x2160 in real time,
which costs far more CPU than the frame is worth and overruns the bitrate
anyway.

Encoder settings differ from the camera's on every axis because screen
content is text-heavy and mostly static: `static-threshold=100` skips
macroblocks that did not change (most of a desktop, most of the time; on a
camera it would smear real motion, so it stays 0 there), a longer keyframe
distance spends the budget on legible text, and `cpu-used=4` buys the
headroom 1080p needs.

The share is published as an **additional** track, so the camera keeps
running — the same as Element.

### Per-sender key rings, and the pad-to-participant mapping

LiveKit's key index is **per participant**: two senders may both use index 0
with entirely different material. One shared ring would decrypt at most one
of them, so there is one ring per sender, exactly as livekit-client keeps one
decryptor per participant.

Attributing a received pad to its sender is the mapping that makes that
possible. The subscriber offer carries one `a=msid:<stream-id> <track-id>`
per media section and LiveKit's stream id **is** the sending participant's
sid — the same attribute livekit-client reads. webrtcbin names a received pad
`src_<index>` where the index is the media-section index, so the pad, the
SDP section and the sender line up without guessing.

A section we cannot attribute gets its **own** ring keyed by the media-section
index rather than being folded into a shared one. A ring nobody has keyed
drops; decrypting one participant's frames with another's key would be silent
corruption.

Keys reach the right ring by two hops, and both are needed: the MatrixRTC
membership gives a device's SFU **identity** (derived in Rust — a sha256 in
the sticky format, so it cannot be recomputed in C++), and the SFU's own
participant list gives the **sid** that appears in the `msid`. An
unresolvable sender installs nothing.

### Received video is rendered

It previously went to a `fakesink`: the pipeline decoded every frame
correctly and then discarded it, so a video call showed nothing at all.

The pinned toolchain ships neither `qml6glsink` nor `qmlglsink` (checked with
`gst-inspect-1.0`), and a package cannot depend on a plugin that may be
absent — so frames come out through `appsink`, which is in
gst-plugins-base and always present, and are pushed into the `QVideoSink`
that a QML `VideoOutput` already exposes. No new plugin dependency, and the
declarative surface is the one the rest of the app uses.

Decisions worth keeping:

* `max-buffers=1 drop=true` on the video appsink. A late video frame is
  worthless, and queueing them turns a slow consumer into growing latency.
  Audio is never dropped this way.
* The frame is **copied**, because a `QVideoFrame` cannot borrow a
  `GstBuffer`'s memory: the buffer is unreffed when the callback returns
  while the frame lives until the GUI thread has rendered it. Copying is why
  the router is asked `watching()` first — an unwatched participant costs a
  hash lookup instead of a full-frame memcpy at frame rate.
* Row-by-row copy, not one block: GStreamer and Qt need not agree on stride,
  and copying the whole thing when they disagree shears the image.
* `SfuVideoRouter` holds `QPointer`s and takes its own mutex. A VideoOutput
  can be destroyed between a frame being queued on the streaming thread and
  delivered on the GUI thread — a window that opens on every grid relayout —
  and `watching()` is called from a streaming thread while the GUI thread
  inserts, where an unguarded QHash rehash is a crash rather than a wrong
  answer.
* Tiles route on **identity**, never on userId+deviceId. The participant
  rows derive those two by splitting the identity on `:`, which is right for
  the legacy `@user:server:DEVICE` form and garbage for the sticky form —
  so a modern Element participant would have resolved to nothing and simply
  never shown video.
* Teardown clears every sink. A sink attached for the call that just ended is
  a live destination for the next call's frames, whose stream ids the SFU
  assigns afresh.

### Screen-share audio

Not captured on any platform, and this is parity rather than a gap:
xdg-desktop-portal's ScreenCast interface does not offer audio, which is the
same position Element Call is in on Wayland, and neither `gdiscreencapsrc`
nor Lightning's own Windows window capture produces audio either. Capturing
the default sink's monitor instead would share everything the computer plays
— including the other participants' voices back to them — so it is
deliberately not done.

### NOT TESTED

*(As written, and SUPERSEDED from 2026-08-25 onward. Kept because it is the
honest record of what this round could claim.)*

No call has completed between two clients, so nothing here has decrypted a
frame another implementation encrypted, and no remote video frame has been
rendered from a real sender. What *is* verified: the cryptor's format against
a known answer and an independent HKDF, the LiveKit handshake against a real
`nixpkgs#livekit` SFU, and the router's lifetime discipline in
`sfu-video-router`.

**Where the SFU lane actually stands now** — the authoritative account is
`docs/matrixrtc.md`; this is the index:

* **PASS** — audio, camera and screen share both ways against Element on
  Linux (2026-08-24/25); screen-share stop and restart (2026-08-26); raised
  hands both ways (2026-08-26); the camera and screen share from a packaged
  **Windows** build (2026-08-27, pipeline 135).
* **NOT TESTED** — anything from the macOS package; federation, TURN
  traversal and reconnection; the failure branches of everything above; and
  the single-WINDOW branch on Windows specifically, since the confirmation
  names "the camera" and "screen share" without recording which kind of
  source the share used.

None of that transfers to the legacy 1:1 lane this document is mostly about.
That lane's live status is unchanged and is stated at the top.

## After round 6: two publish-path facts that will be re-derived wrongly here

Rounds 7 and later happened in the SFU lane and are recorded in
`docs/matrixrtc.md`, not here. Two of their findings are properties of
GStreamer rather than of LiveKit, so they will bite anything on a publish
path — including this document's engine, if it ever gains video:

* **`videorate` starts its output clock at SEGMENT START, not at the first
  buffer's PTS.** Add a publish bin to a pipeline that has been PLAYING for
  three minutes, with a source that stamps pipeline RUNNING TIME
  (`ksvideosrc` and `gdiscreencapsrc` both do), and videorate owes thirty
  duplicates per second of call age and emits them as fast as the encoder
  takes them: one picture at full rate, every counter healthy. Measured, at
  30/1: first PTS 0 s -> 27 buffers, 10 s -> 327, 174 s -> 5247;
  `skip-to-first=true` -> 27 in every case. A source of our own must keep its
  PTS ZERO-BASED and pace on a clock anchor.
  Note carefully: `skip-to-first` is ALSO recorded as refuted — against the
  first-buffer HOLD, which is a different defect and on which it genuinely
  does nothing. Both entries stand.
* **A size ceiling does not keep a picture's shape.** `videoscale` answers one
  with a non-square pixel aspect ratio, which VP8 and RTP both drop. Pin
  `pixel-aspect-ratio=(fraction)1/1` at the ceiling AND in front of the
  source; see the corrected section above.

Live validation of that lane, so it is not read off this document by mistake:
audio, camera and screen share are **PASS** against Element on Linux, and the
camera and screen share are **PASS** from a packaged Windows build
(2026-08-27). The legacy 1:1 lane described in this document remains
**NOT TESTED** end to end.
