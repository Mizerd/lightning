# MatrixRTC (modern Matrix calling)

Status: **phase 1 (observation and discovery) and phase 2 (SFU signalling,
membership publishing, media transport and call E2EE) are implemented; no
call has been completed against another client.** This document records the
exact wire Lightning speaks, what a homeserver has to provide, what is
deliberately absent, and what has and has not been validated.

Read the validation section before believing any of it works end to end.

For the *legacy* 1:1 `m.call.*` lane (MSC2746, GStreamer `webrtcbin`, real
audio), see `docs/voice-calls.md`. The two lanes coexist by design; §"Two
lanes, one ring" below is the contract between them.

## Why the wire is pinned to an implementation, not an MSC

The MatrixRTC MSCs (4143 membership, 4075 notifications, 4310 declines, 4354
sticky events) are **unmerged pull requests**, and the deployed behaviour is
ahead of the drafts. Everything here was therefore read out of the reference
implementation:

* `matrix-js-sdk` `src/matrixrtc` @ **84fb28a** (2026-08-19)
* `element-call` @ **b51a33c** (2026-08-21)

When those move, re-read them before changing anything here. Do not "fix"
this module against an MSC document.

## The events Lightning speaks

| Purpose | Event type | Direction |
|---|---|---|
| Membership (deployed form) | `org.matrix.msc3401.call.member` (state) | read |
| Membership (next form) | `org.matrix.msc4143.rtc.member` (sticky) | parsed, not observable |
| Session slot | `org.matrix.msc4143.rtc.slot` (state) | read |
| Ring / announce | `org.matrix.msc4075.rtc.notification` | read (send pipe built, no caller yet) |
| Decline | `org.matrix.msc4310.rtc.decline` | send (existing lane) |

### The notification type mattered, and ruma gets it wrong for interop

ruma 0.34 types the notification event as the **stable** `m.rtc.notification`
with no unstable alias. Current Element sends
**`org.matrix.msc4075.rtc.notification`**. A ruma-typed handler is therefore
deaf to every ring a current Element client sends — which is exactly what
`calls.rs` had, so the previously documented "real interop with Element X
ringing" did not hold for current Element.

`rust/src/rtc.rs` defines its own event content with the MSC4075 type string
and observes that. The stable ruma handler is kept as well, so a client that
sends the stable type still rings. Pinned by
`the_notification_event_type_is_the_one_element_sends`.

### Membership: two formats, one of them reachable

The deployed form is a **state event** whose content is
`SessionMembershipData`. The state key Element writes is
`_{userId}_{deviceId}_{application}{slotId}` (leading underscore except on
`org.matrix.msc3757`/`msc3779` room versions). Lightning **reads any state
key** and trusts the event's `sender` instead, which is both more robust and
the only safe choice: nothing defines what power level would let one user
publish another's membership, so `member.user_id` must equal `sender` or the
membership is dropped.

Two normalisations are load-bearing:

* `call_id: ""` is the room-wide call, and the newer vocabulary spells the
  same thing `"ROOM"`. Both map to slot id `m.call#ROOM`, or the same call
  read through the two formats looks like two calls.
* The SFU participant identity is **derived, never invented**:
  `"{user_id}:{device_id}"` for session memberships (what the SFU assigns for
  that format), and unpadded-base64(sha256(canonical JSON
  `[user_id, device_id, member_id]`)) for the sticky format. A different
  encoding silently mismatches Lightning's idea of a participant against
  Element's.

The sticky (MSC4354) form is **parsed and tested but not observable**:
matrix-sdk 0.18 has no sticky-event support at all. That is a recorded SDK
gap, not a defect — the parser exists so the format cannot rot, and its dead
code is marked with the reason.

### Expiry is what stops phantom participants

A membership carries `expires` (default **4 hours**, matching the reference
implementation's `DEFAULT_EXPIRE_DURATION`) relative to `created_ts`. A
client that dies leaves its state event behind, so **expired memberships are
dropped** before anything is counted. Addition saturates: a hostile
`expires` of `u64::MAX` must not wrap into the past.

Dedup is per **`(user_id, device_id)`**, keeping the newest `created_ts`. The
same person on a laptop and a phone is two real participants; collapsing by
user would hide one. Facepiles dedup again, per *person*, because one human
should show one face.

## Transport discovery — discovered, never assumed

There is **no hardcoded SFU anywhere in Lightning**, and no dependency on
`call.element.io` or any other hosted instance. Resolution order matches
element-call's own:

1. The homeserver's authenticated MSC4143 endpoint,
   `GET /_matrix/client/unstable/org.matrix.msc4143/rtc/transports`, which
   answers `{"rtc_transports": [...]}`.
2. Otherwise the focus the **existing participants advertise** in their own
   membership `foci_preferred`. This is what `focus_selection:
   "oldest_membership"` means, and it is how every pre-endpoint deployment
   works — it is what would let Lightning join a call Element started on a
   server with no MSC4143 endpoint.
3. Otherwise nothing. A user-configured URL is the intended third step and
   is **not implemented yet**.

An earlier MatrixRTC draft advertised foci through `.well-known/matrix/client`
as `org.matrix.msc4143.rtc_foci`. Current Element does not read it, so
Lightning deliberately does **not** implement it — it would be a dead path.

Only `type: "livekit"` transports are understood, and a transport URL must be
**`https:`** with a host; an `http:` SFU would downgrade the channel that
carries call authorization. Unknown transport types are skipped rather than
guessed at.

The endpoint is called over the SDK's own HTTP client
(`Client::http_client()`), because ruma-client-api 0.24 has no MSC4143
endpoint and defining one requires ruma's endpoint macros, which need `ruma`
as a *direct* dependency. Same trade `banner.rs` already made for MSC4133.
The access token is read from the SDK, used for one request, and never
logged, stored, enqueued, or returned.

### "No calling here" and "we could not check" are different facts

`rtc_transports` reports `server_answered` separately from a closed-set
`category`, and `RtcController::joinBlock()` keeps four causes apart:

| Reason token | Meaning |
|---|---|
| `unsupported` | this backend has no MatrixRTC (mock/HTTP) |
| `undiscovered` | discovery has not answered yet |
| `no_transport` | the server answered and offers nothing |
| `discovery_failed` | the check itself failed |
| `session_closed` | a slot state event says the session ended |
| `no_media_transport` | Matrix side is fine; this build has no SFU client |

The UI maps those tokens to wording. It never renders a raw server string.

### The slot is read fail-closed, and the focus does not wander

Two rules that diverged from the reference during review and were corrected:

* **Focus selection reads the OLDEST membership's own `foci_preferred[0]`
  and stops.** It does not walk on to a younger member that advertises one.
  The reference (`getOldestMembership()` + `getTransport()`) yields
  `undefined` in that case, and walking would put Lightning on a different
  SFU than Element in exactly the situation the oldest-membership rule
  exists to prevent.
* **The slot state key must match `m.call#ROOM` exactly, and the session is
  open only if `status == "open"` AND `application.type == "m.call"`.** An
  earlier revision matched an EMPTY state key as the room call's slot and
  closed only on the literal string `"closed"`, which meant anyone able to
  send that state type could suppress a room's entire call display, and a
  slot for a different application read as an open `m.call` one.

Absence of a slot is still deliberately **not** "closed": almost no
deployment publishes one, so treating a missing slot as closed would hide
every real call.

## What is deliberately absent, and why

**Lightning does not publish membership.** Advertising a joinable session
without a media transport tells every other client in the room to open an SFU
connection that can never complete — a lie on the wire, not a stub, and the
same reason the legacy lane refuses to invite without an engine. There is no
join/publish pipe in the FFI at all, so it cannot be reached by accident.
`joinBlockReason()` therefore always has something to say today, and the Join
button is disabled with that reason rather than being offered dead.

Consequently absent in this phase: media of any kind over an SFU, media
E2EE (`io.element.call.encryption_keys` key distribution), screen sharing,
video, raise hand, call reactions, per-participant volume, and PiP. Those
belong to the SFU transport, which is phase 2.

The **notification send pipe is built but has no caller**
(`rtc::send_notification` → `mx_rust_rtc_notify` → `MatrixClient::rtcNotify`
→ `rtcSendFinished`). It is deliberately unwired: a ring announces a session
a peer is expected to join, and Lightning publishes no membership to join.
It is kept, tested at the content level, and documented as a phase-2 seam
rather than deleted, because the ring is the first thing phase 2 needs.

## Two lanes, one ring

The legacy `m.call.*` lane and the MatrixRTC notification lane both reach the
same `CallController`. Whichever announces first owns the ring; a second
announcement while a session is live is ignored rather than rung again.

One case needed a fix. A dual-stack caller can announce **one** call on both
lanes, and the ids can never match (an RTC session is keyed on the
notification event id), so the re-delivery guard missed it and the legacy
invite fell into the busy branch and sent `m.call.reject`. That told the
caller "declined" while Lightning was in fact ringing the user for exactly
that person in exactly that room, and the user might then answer a call the
caller had already abandoned. A legacy invite from the **same sender in the
same room** while an RTC ring is live is now treated as the same conversation
and answered with silence — no second ring and no wire event. Pinned by
`aDualStackCallerDoesNotGetAFalseDecline`, with
`aDifferentCallerWhileRingingIsStillRejectedAsBusy` keeping the guard narrow.

Adopting the legacy leg instead (which would make an otherwise unanswerable
RTC ring answerable, since the legacy lane has media) is a deliberate
follow-up, not done here: it would change session identity mid-ring.

## Mute and deafen are real

`CallMediaBackend` gained `setMicrophoneMuted`/`setOutputMuted` and
`supportsMuteControl()`. In `GstCallMediaBackend`:

* **Mute** is a named `valve` between the audio source and the encoder.
  `drop=true` discards buffers *before* encoding, so no RTP is produced and
  the peer receives nothing. Lowering gain would still send audio and is not
  mute.
* **Deafen** is a named `volume` element in each receive bin. That
  legitimately *is* a local volume operation — there is no upstream to stop —
  and it touches only call audio, never media playback elsewhere. Every
  receive bin is muted (a group call has one per remote track), matched on
  the element **name** we gave it (`outvol`) and deliberately **not** on the
  `volume` factory: `autoaudiosrc`/`autoaudiosink` are bins that may contain
  a volume element of their own, so a recursive factory match would reach
  into the SEND chain.
* A remote track that arrives *after* the user deafened comes up already
  silenced: the desired state is published through an atomic that the
  GStreamer streaming thread reads in `pad-added`, because marshalling to the
  Qt thread first would let the track be briefly audible.

The user's intent lives in `CallController`, not the engine: engine state is
per session and resets. It is pushed down **at session start**, immediately
after `createOffer`/`createAnswer` build the pipeline — *not* when the call
connects. `connected` arrives through a queued marshal, i.e. at least one
event-loop turn after RTP is already flowing, so applying it there published
a muted user live (and let a deafened user hear) for the opening window of
every call. Pinned by `aStandingMuteIsAppliedBeforeMediaCanFlow` and
`aStandingMuteIsAppliedBeforeAnsweringToo`, both verified to fail without the
fix.

Deafening remembers the prior microphone state and **undeafening restores
it**, so someone who was muted first does not come back live. The intent
survives call-to-call (the familiar convention) but is cleared on sign-out
and on an account change, because everything else on those paths is cleared
too and a deafened state carried silently into the next account is
unhearable with no visible cause.

`supportsMuteControl()` defaults to false on the seam, so an engine that does
not implement mute cannot light up a working-looking control.

## What the UI shows

* **Room call banner** (`qml/RoomCallBanner.qml`) — "N people in call" with a
  per-person facepile, in the timeline above the room-upgrade banner, as an
  ordinary layout child so it reflows rather than occludes. It produces **no
  timeline rows**: call membership churn as timeline spam is exactly what is
  to be avoided. Collapses to zero height with no call.
* **In-call audio controls** — mute and deafen, as circular
  `CallControlButton`s on the existing in-call card. Layout and interaction
  follow Discord; every colour, radius and type value comes from `AppTheme`,
  so all eleven themes and the text scale apply. No Discord artwork or
  colour is used. They appear only when the engine genuinely implements
  mute (`muteControlAvailable`), so they can never be dead controls, and
  that gate is contract-tested.
* A participant grid and a full control bar were written and then
  **removed before commit**: nothing hosts them until an SFU transport
  exists, and unreachable UI is how this codebase previously shipped a
  permanent no-op unnoticed. They belong to phase 2, with the media that
  gives them content.
* A control whose backend does not exist is **not shown**, rather than shown
  disabled with a tooltip: a disabled `AbstractButton` receives no hover
  events in Qt Quick, so a tooltip cannot explain it. Where a reason must be
  visible anyway — the room banner's Join — it is rendered as an **inline
  label**, not a tooltip.

Two presentation rules worth keeping:

* A tile shows a muted badge only when something authoritative said so
  (`micKnown`). An observed membership says a device *joined*, never whether
  its microphone is live — only an SFU can say that, and "unknown" must
  render nothing rather than a confident wrong "not muted".
* Every binding that calls into `RtcController` reads a `refreshTick`
  counter. Qt cannot track a C++ function call as a dependency, so a binding
  without it evaluates once and never again — for `participantCount` that
  means `visible` stays false and the banner never appears at all.

## Server requirements

For Lightning to report that calling is available, a homeserver needs:

* **A MatrixRTC transport**, advertised either through
  `/_matrix/client/unstable/org.matrix.msc4143/rtc/transports` or by the
  existing participants' `foci_preferred`. In practice that means a
  **LiveKit SFU** plus the **LiveKit JWT service** (`livekit-jwt-service`)
  reachable over HTTPS.
* The JWT service authorizes with a **Matrix OpenID token**
  (`POST {livekit_service_url}/sfu/get` with `{room, openid_token,
  device_id}` answering `{url, jwt}`), so the user's Matrix **access token
  never leaves Lightning**. Phase 1 does not perform this exchange; it is
  recorded here because it constrains the deployment.
* Federation: the JWT service must be able to resolve the calling user's
  homeserver to validate the OpenID token, so cross-server calls need the
  usual federation reachability.
* Media: LiveKit needs its WebRTC UDP range reachable (plus TCP/TLS fallback
  if deployed), and `wss://` for its signalling.
* MSC4140 delayed events are supported by ruma and are how a client's
  membership is cleaned up if it dies; Lightning does not publish membership
  yet and therefore does not use them yet.

Nothing here is specific to any one deployment, and no vendor's server is a
default.

## Diagnostics and privacy

* Nothing in `rust/src/rtc.rs` logs a sender-chosen string, a member id, a
  transport URL, or an access token. Ids are compared, never rendered raw.
* Every inbound string is bounded and rejected if it carries control
  characters; every collection is capped (128 members, 8 transports). A
  membership that fails validation is dropped whole, never partially trusted.
* The wire format a membership came from (`session`/`rtc`) crosses as a
  diagnostic field and is not shown in normal UI.
* No new dependency beyond `sha2` (`=0.10.9`), which the pinned matrix-sdk
  already pulls in transitively — declared directly for the MSC4143 identity
  hash, following the `mime`/`emojis` precedent.

## Validation

**Automated (run, exact results):**

* Rust: `rtc::tests` — **28 passed**, covering real Element membership
  content, the `""`→`ROOM` slot normalisation, identity defaults, the
  forgery guard, expiry saturation and the 4-hour fallback, `http:` refusal,
  unknown transport types, oldest-membership focus selection, per-device
  dedup, and the notification's on-wire shape and type string.
* C++: `rtc-session` — **19 passed** (17 cases plus init/cleanup): op-id/room matching, cross-account isolation
  across an account switch, sign-out clearing, poke coalescing, a poke that
  lands during an in-flight read, unchanged-session suppression,
  slot-closed suppression, own-device vs own-user, one face per person, the
  join-block causes staying distinct, an unanswered read releasing its room
  (verified to fail without the reap), a replaced client's late reply, and
  one room's focus not deciding another's.
* C++: `call-controller` — **49 passed**, including new regression cases for
  the cross-lane false decline (with the room and sender clauses pinned
  separately), the mute/deafen restore semantics, the start-of-session mute
  application, and the sign-out clear. The false-decline case and both
  start-of-session cases were **verified to fail on the unfixed code**.
* C++: `call-ui-contract` — **8 passed**, including a contract that the
  audio controls cannot appear without an engine that really implements
  mute; verified to fail on a weakened gate.

**NOT TESTED (live), all of it:** any real MatrixRTC interoperability with
Element in either direction; any media over an SFU (there is none in this
phase); the transport-discovery endpoint against a real homeserver that
implements it; membership observation against a real Element-started call;
the banner, tiles or control bar on a real desktop; mute and deafen against
a real peer with audible audio; federation.

Compilation and passing offscreen suites are not interoperability. Nothing in
this document should be read as "calling works" — phase 1 makes Element's
calls *visible* and makes Lightning's existing 1:1 audio calls properly
controllable, and says honestly why it cannot yet join.

---

# Phase 2 — SFU signalling, membership, media and E2EE

Phase 1 made Element's calls *visible*. Phase 2 adds everything needed to
*join* one: SFU authorization, membership publishing, the LiveKit signalling
protocol, a media transport, and end-to-end encrypted media.

## Authorization: the access token never leaves Lightning

The SFU is authorized with a **Matrix OpenID token**, not the Matrix access
token:

```
POST {livekit_service_url}/sfu/get
  { room, openid_token: {access_token, token_type, matrix_server_name,
                         expires_in}, device_id }
  -> { url, jwt }
```

The OpenID token is minted by the homeserver for exactly this purpose and
lets the JWT service verify who the user is *by asking their homeserver*. So
the SFU never sees a Matrix credential, and the SFU's JWT never reaches
Matrix. Neither is logged, neither crosses the FFI toward QML, and neither is
persisted — the JWT lives in the signalling task for one connection.

The returned SFU URL is re-validated as `wss:` with a host before use.

## Signalling: `livekit-protocol`, not the `livekit` client

`rust/src/sfu.rs` speaks LiveKit's WebSocket/protobuf protocol directly.
**Deliberately not the official `livekit` crate**: it depends on
`webrtc-sys`, which downloads a prebuilt libwebrtc during the build. That
breaks this crate's `--offline --locked` contract outright, and measured at
+318 crates and ~1.7 GB of build artifacts on a tree that already links a
2.1 GB debug staticlib into ~150 test binaries. `livekit-protocol` is pure
message definitions — no media, no download — so Lightning speaks the same
wire with the GStreamer engine it already ships. Net cost: **+21 crates, no
libwebrtc**, and `--offline --locked` still builds.

**Two peer connections, and confusing them wires audio the wrong way.** The
client offers on PUBLISHER (its own tracks); the server offers on SUBSCRIBER
(everyone else's). The target rides every description and every candidate,
and an unrecognised target degrades to *subscriber* — never to publisher,
which would attach a remote description to our own outgoing connection.

## Membership: the format Element reads

Published as the **legacy session state event**
(`org.matrix.msc3401.call.member`), because that is what every deployed
server and every current Element understands; matrix-sdk 0.18 cannot send a
sticky event at all.

* State key `_{user}_{device}_m.call`, dropping the leading underscore only
  on `org.matrix.msc3757`/`msc3779` room versions. Wrong here means either
  the server refuses the write, or a refresh fails to replace our own
  previous membership and we appear **twice** in the call.
* `created_ts` is preserved across refreshes. It orders oldest-membership
  focus selection, so resetting it would reshuffle everyone's chosen SFU
  every few minutes.
* Cleanup is an **MSC4140 delayed retraction**, armed at publish and
  restarted while we live. If Lightning dies, the server retracts our
  membership for us. A server without MSC4140 refuses that call and cleanup
  falls back to `expires` — reported as `delayed_category`, never as a
  failure of the call itself.
* Leaving retracts explicitly *and* cancels the pending delayed event, in
  that order: a redundant no-op retraction is harmless, a window with
  neither is not.

## Media: `SfuMediaEngine`

A separate class from the 1:1 `GstCallMediaBackend`, because LiveKit differs
architecturally in three ways that would have turned that class into a mess
of conditionals: two peer connections, N remote streams appearing and
leaving at any time, and tracks that are *declared* (AddTrack) before they
are negotiated.

* Audio: Opus, published behind a named `valve` — real mute stops buffers
  before the encoder, so no RTP is produced at all.
* Video: VP8. Screen share and camera use different encoder settings, since
  screen content is text-heavy and wants readability over motion smoothness.
* Screen capture: **`pipewiresrc` with a node id from an
  xdg-desktop-portal ScreenCast session** — never direct framebuffer
  access, so the user's own portal dialog decides what is shared. A
  negative node id is refused rather than defaulted, because "whatever
  PipeWire feels like" is exactly how you publish the wrong monitor.
* Per-participant local volume, deafen, and the same
  arrives-after-you-deafened protection as the 1:1 engine.

## Call E2EE: real, and cross-checked against an independent implementation

`src/calls/CallFrameCryptor.*` implements LiveKit's frame encryption, so the
SFU forwards media it cannot read. **The format is not invented here** —
every constant and byte position was read out of `livekit-client` 2.22.0
(`src/e2ee/`), which is the same format libwebrtc's native FrameCryptor
implements and therefore the same one Element Call speaks.

```
key      : HKDF-SHA256(ikm = raw 32-byte key,
                       salt = "LKFrameEncryptionKey",
                       info = 128 zero bytes) -> 16 bytes (AES-128-GCM)
frame    : [ cleartext header ][ ciphertext + 16-byte tag ]
           [ IV: 12 bytes ][ trailer: 2 bytes = {12, keyIndex} ]
header   : audio (Opus TOC) = 1, VP8 keyframe = 10, VP8 delta = 3
IV       : [0..3] ssrc, [4..7] rtp timestamp,
           [8..11] timestamp - (sendCount % 0xffff)     (all big endian)
```

Three properties worth stating explicitly:

* **The cleartext header is authenticated as AAD.** The SFU can still route
  on it and detect keyframes, but cannot alter it undetected.
* **IV reuse is the whole ballgame.** AES-GCM leaks its authentication key
  on a repeated (key, IV) pair — a total break, not a lost frame. The
  counter is per-SSRC, monotonic, and seeded at a random offset exactly as
  the reference does. A test encrypts 512 frames on one SSRC at an
  unchanging timestamp (the worst case) and requires 512 distinct IVs.
* **No key means no output.** Not a passthrough. A cleartext fallback would
  silently un-encrypt an encrypted room's call, so `encryptFrame` returns
  nothing and the caller must drop the frame. Likewise an unknown key index
  is dropped rather than decrypted with "some key we have", which would
  defeat rotation.

Key derivation is pinned by a **known-answer test cross-checked against a
from-scratch RFC 5869 HKDF** (`262178a9e5dabf73df9342ed5bae9fe1` for
`ikm = 32 * 'k'`), not against this implementation. That is what proves we
derive the *same* key Element does: a self-consistent round-trip test would
pass just as happily with the wrong salt, the wrong info length, or PBKDF2.

Keys are distributed as `io.element.call.encryption_keys`, **Olm-encrypted
per device** through `Encryption::encrypt_and_send_raw_to_device`, so the
homeserver never sees one. Only devices that have declared themselves
present in the call are sent the key; a device that cannot be resolved is
skipped, never substituted. The SDK answers with the devices it could *not*
reach, so a partial delivery is visible rather than silently successful.
Inbound, what is trusted is the **Olm-decrypted sender** — the `member`
block in the content is a claim and is used only to fill in an id.

This needs matrix-sdk's `experimental-send-custom-to-device` feature: it
gates the only public API for sending a custom event type Olm-encrypted per
device, and there is no stable equivalent in 0.18.

## Packaging (lightning-deploy)

GStreamer **plugins are `dlopen`ed from a plugin path**, so nothing that
inspects ELF NEEDED entries can find them — `dpkg-shlibdeps`, rpm's
automatic generator and `linuxdeploy` all miss them, because the binary
links only gstreamer core/webrtc/sdp. Every format therefore names them
explicitly, and each fails identically if it stops: the package installs and
launches perfectly, then refuses every call because the engine's runtime
element probe finds nothing.

* **deb** — `CALL_DEPENDS` beside the existing `QML_DEPENDS`, which exists
  for the same reason.
* **rpm** — explicit `Requires:` lines.
* **AppImage** — the plugins are *staged into the AppDir* before
  `linuxdeploy` runs (so it also bundles their own dependencies and rewrites
  their RPATHs), **plus an AppRun hook** setting
  `GST_PLUGIN_SYSTEM_PATH_1_0`. Staging without the hook bundles files
  nothing ever loads.
* **Flatpak** — `--filesystem=xdg-run/pipewire-0` only. The portal is
  reachable from a sandbox by default and *it* decides what may be
  captured; the socket is merely how the negotiated stream is read.
  Deliberately **not** `--filesystem=host` and **not** `--device=all`.

All of this is pinned by `tests/test-pipeline-config.py`.

## Validation

**Live, against a real LiveKit 1.13.5 server** (nixpkgs, `--dev` mode,
running locally):

* The signalling handshake this module implements **completes**: WebSocket
  connect to `/rtc`, `JOIN_OK`, our identity echoed back, ICE servers
  delivered, `subscriber_primary=true`. The server's own log shows a real
  room, a real participant, and both PUBLISHER and SUBSCRIBER transports,
  with our exact client info (`sdk: CPP, version 0.7.6, protocol 15`).

**Automated:** Rust `cargo test` 174 passed / 0 failed / 4 ignored, including
32 `rtc::` and 5 `sfu::` cases. C++ `call-frame-cryptor` 19 passed, including
the known-answer derivation, byte-exact IV layout, AAD coverage, tamper
detection, rotation, and the no-key-no-output property.

**NOT TESTED — and this is the honest headline:**

* **No call has ever been completed between two clients.** Not
  Lightning↔Element, not Lightning↔Lightning. No audio or video has been
  exchanged over an SFU by this code.
* The frame cryptor is **not yet wired into the GStreamer pipeline** — it is
  a correct, tested unit with no pad probes attached, so media currently
  publishes unencrypted. Encrypted rooms must therefore not be offered a
  call until that wiring lands; see "Remaining work".
* No screen-share **source picker** exists: the engine accepts a PipeWire
  node id, but nothing yet opens an xdg-desktop-portal ScreenCast session to
  obtain one.
* The packaging changes are **unbuilt** — no pipeline has run with them.
* Federation, TURN traversal, reconnection, and every platform other than
  Linux: untested.

## The call lifecycle

`SfuCallController` binds the three halves that must agree, and the ORDER is
the main thing it exists to get right:

1. Discover a focus (phase 1's `RtcController`).
2. **Publish membership first**, carrying that focus. Other clients pick
   their SFU from the oldest membership, so ours has to be on the wire before
   we expect anyone to meet us there.
3. Connect to the SFU and negotiate both peer connections.
4. Only then publish tracks.

Leaving runs in reverse, and every step is idempotent, because the leave path
is also the failure path — anything that goes wrong mid-join has to unwind
from wherever it got to. Teardown releases **media first**, so no device
stays live because a network call hung.

`join()` refuses, with plain wording rather than a category, when: the room is
encrypted and media E2EE is unavailable; no focus is known; there is no media
engine; or the build has no WebRTC. One call at a time globally — a second
join tears the first down explicitly rather than leaving two engines holding
the microphone.

## The encrypted-room gate, and why it fails closed

`RtcController::joinBlock()` returns `media_encryption_unavailable` for an
encrypted room whenever media E2EE is not active, and `SfuCallController`
refuses the join outright. §6 requires failing safely and saying so, never
silently weakening encryption.

The default is deliberately **encrypted**: a room the controller has not been
told about is treated as encrypted, because a boolean cannot say "unknown"
and the safe answer to "might this be encrypted?" is yes. `AppController`
supplies the real answer from the room's own `encrypted`/`encryptionKnown`
pair — and an unknown `encryptionKnown` also fails closed, so the tri-state
survives the whole way down.

## What the UI is

* **`CallStage`** — the call surface, hosted in the timeline column and
  *replacing* the timeline while the call's room is open. A call is the thing
  the user is doing; half-covering the room gives neither surface room.
  Layout follows the participant count automatically (grid, or spotlight when
  someone shares or a participant is pinned) with a manual override.
* **`CallParticipantTile`** — avatar, speaking ring, name strip, state
  badges. A badge appears only when the SFU actually reported that track's
  state; unknown renders nothing rather than a confident "not muted".
* **`CallControlBar`** — mic, deafen, camera, raise hand, layout,
  participants, leave. Every control reaches something real; nothing is shown
  disabled with a tooltip, because a disabled control receives no hover in Qt
  Quick and so cannot explain itself.
* **`VoiceConnectedBar`** — the persistent footer in the room list. The call
  does not end because the user opened another room, and this is how they get
  back to it.

Layout and interaction follow Discord; every colour, radius and type value
comes from `AppTheme`, so all eleven themes and the text scale apply. No
Discord artwork or colour is used.

## Validation

**Live, against a real LiveKit 1.13.5 server** (nixpkgs, `--dev`, local):
the signalling handshake this module implements **completes** — WebSocket
connect to `/rtc`, `JOIN_OK`, our identity echoed back, ICE servers
delivered, `subscriber_primary=true`. The server's own log shows a real room,
a real participant, and both PUBLISHER and SUBSCRIBER transports, with our
exact client info (`sdk: CPP, version 0.7.6, protocol 15`).

**Automated:** see the table in the completion report; the frame cryptor's
key derivation is pinned by a known-answer test cross-checked against a
from-scratch RFC 5869 HKDF, not against itself.

**NOT TESTED — the honest headline:**

* **No call has ever been completed between two clients.** Not
  Lightning↔Element, not Lightning↔Lightning. No audio or video has been
  exchanged over an SFU by this code.
* The frame cryptor is **not attached to the pipeline** — it is a correct,
  tested unit with no pad probes wired. That is exactly why encrypted rooms
  refuse to join rather than publishing in the clear.
* No screen-share **source picker**: the engine and controller accept a
  PipeWire node id and refuse a negative one, but nothing yet opens an
  xdg-desktop-portal ScreenCast session to obtain one.
* Raise hand and call reactions are **local state only** — no
  MatrixRTC-compatible event is sent, so other clients cannot see them.
* The packaging changes are **unbuilt**: no pipeline has run with them.
* Federation, TURN traversal, reconnection, and every platform other than
  Linux: untested.

## Remaining work, stated plainly

1. **Wire the frame cryptor into the pipeline** (pad probes on the RTP
   payloader/depayloader), then flip `mediaEncrypted` and let encrypted rooms
   join. Until then the gate above is what keeps this honest.
2. **Portal integration** for screen-share and camera source selection.
3. **Video rendering**: remote tracks are received and decoded, but the
   spotlight shows who holds the stage rather than their picture — there is
   no `QVideoSink` bridge yet.
4. **Raise hand and reactions on the wire**, using whatever MatrixRTC
   defines rather than a Lightning-only event.
5. Live interoperability with Element, which is the only thing that turns any
   of the above from "implemented" into "works".
