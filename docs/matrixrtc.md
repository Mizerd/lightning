# MatrixRTC (modern Matrix calling)

Status: **phase 1 (observation and discovery) and phase 2 (SFU signalling,
membership publishing, media transport and call E2EE) are implemented, and
calls carry audio, camera and screen share in both directions against
Element** — live-confirmed on Linux 2026-08-24/25, and from a packaged
**Windows** build 2026-08-27. This document records the exact wire Lightning
speaks, what a homeserver has to provide, what is deliberately absent, and
what has and has not been validated.

**Every "Validation" block below is a SNAPSHOT of the round that wrote it,
kept because the refuted theories in it are expensive to re-derive.** The two
phase-2 blocks still open with "no call has ever been completed between two
clients", which was true when they were written and has not been true since
2026-08-25; each carries a superseding note. The current position is this
status line plus the last two sections of the document, and nothing else.

Read the validation sections before believing any of it works end to end.

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

## LiveKit wire facts that each cost a working call

Every one of these was read out of the reference (livekit-client 2.22.0 and
the LiveKit server's own `pkg/rtc`), and every one was wrong here first. They
are listed together because they share a failure mode: the call CONNECTS, the
UI says it is fine, and no media is usable.

* **`ParticipantUpdate` is a DELTA, not the room.** It names only the
  participants whose state changed, including ourselves. Assigning it over
  the participant list erases everyone it does not mention — so publishing
  our own audio produced an update about us that deleted the person we were
  talking to, and their next update deleted us. That is a permanent "1 person
  in call", and it takes the media key with it, because a key can only be
  installed against a participant still held. Merge by `identity`; remove on
  `state == DISCONNECTED`. Reference: `Room.handleParticipantUpdates`.
* **`JoinResponse.participant` is US**, and `other_participants` is everyone
  else. Both belong in the participant list, or the local tile cannot be
  drawn from the join and nothing can find our own track sids — which is what
  a mute has to name.
* **The `msid` stream id is PACKED.** The server writes
  `PackStreamID(publisherID, trackID)` — `PA_…|TR_…`, separator `|` — for
  every client above protocol 0 (`SupportsPackedStreamId()` is literally
  `v > 0`). Taking the token whole yields a name matching nothing: media keys
  are installed per participant and video sinks attached per participant, so
  a remote participant is silent AND invisible at once. Split it, exactly as
  `unpackStreamId()` does; a LEADING separator names no participant and must
  resolve to empty rather than to a confident wrong id.
* **`SessionDescription.id` is the offer correlation id, not the peer-
  connection target.** The server seeds its own offer ids randomly
  (`rand.Intn(1<<8)+1`) and compares an incoming answer against
  `localOfferId`. Sending the target there matched only by accident. Echo the
  id of the offer being answered; number our own offers with a counter.
  Reference: `toProtoSessionDescription(offer, offerId)`.
* **`AddTrackRequest.disable_red` must be TRUE whenever frames are
  encrypted.** RED (RFC 2198) wraps the Opus payload, so a receiver hands its
  frame decryptor a packet whose first byte is not the Opus TOC the format
  leaves in the clear, and every frame fails its authentication tag.
  livekit-client says it in one line: `disableRed: this.isE2EEEnabled || …`.
* **A media key names a DEVICE; a frame names a sid.** Nothing orders the
  to-device key, the SFU participant list and the MatrixRTC membership
  against each other, so resolving the sid first and dropping the key when it
  is not known yet loses that key permanently — nothing re-sends it. Store
  the ring under a name derived from the key itself (the Olm-decrypted sender
  plus the claimed device id) and BIND the sid to that ring whenever the
  participant list makes it possible, re-running the binding on every update.
  The binding makes the two names one shared ring rather than a redirection,
  because a redirection strands whichever ring was created first.

* **A media key is 16 OR 32 raw bytes.** element-call mints **16**
  (`new Uint8Array(16)` in matrix-js-sdk's `RTCEncryptionManager`);
  livekit-client's own `createE2EEKey()` mints 32. HKDF accepts either and
  derives the same 16-byte AES-128 key. Requiring 32 meant every key an
  Element peer sent was rejected for its LENGTH — the key arrived, was
  discarded, and every frame from that participant dropped for want of a key,
  so an Element user could never be heard or seen while our own media reached
  them normally. Still a closed set: a length nobody uses must not derive
  cleanly and light up `encryptionActive()` under material no peer can have.
* **`rtpvp8pay` cannot payload an encrypted frame, so Lightning ships its
  own.** GStreamer's payloader READS the VP8 bitstream to build its descriptor
  — partition0's size from the frame tag, a keyframe's `0x9d 0x01 0x2a` start
  code, then segmentation and loop-filter fields bool-decoded out of the
  compressed partition (`gst_rtp_vp8_pay_parse_frame`). Frame encryption
  leaves only 10 header bytes of a keyframe (3 of a delta) in the clear, so it
  is handed ciphertext, fails, and posts STREAM/ENCODE "Failed to parse VP8
  frame": a screen share publishes exactly ONE frame and stops, a camera
  publishes none, and the far end shows a grey rectangle. libwebrtc — and so
  Chrome and Element Call — never hits this, because `RtpPacketizerVp8` takes
  the keyframe flag and picture id from the encoder as METADATA and never
  reads the payload. `src/calls/RtpVp8Payloader.*` reproduces that: the
  RFC 7741 descriptor in Chrome's aggregate form (X=0, N=0, PID=0, S=1 on the
  first packet of a frame, marker bit on the last) and MTU fragmentation,
  reading nothing. The DEPAYLOADER needs no equivalent — it only touches the
  first 10 header bytes, which are cleartext by construction.
* **The VP8 PICTURE ID is not optional in practice.** libwebrtc always emits
  one, and LiveKit's SFU **rewrites** the descriptor when it forwards —
  `pkg/sfu/codecmunger/vp8.go` unwraps `vp8.PictureID`, munges it, and
  marshals a new header whose size it computes from that field. Handed packets
  with `X=0` and no picture id, its idea of the header no longer matches the
  sender's and everything after the first frame is corrupted: the far end
  renders one frame and then nothing. Lightning's payloader therefore sends
  Chrome's shape — `X=1`, `I=1`, a 15-bit `M=1` picture id incremented once
  per FRAME and repeated on every packet of it, `S=1` only on the first packet,
  marker bit on the last.
* **A received track is identified by its TRACK SID, never by a `mid`.**
  `a=msid:PA_<participant>|TR_<track> TR_<track>` — the track sid is the half
  after the separator, exactly as livekit-client's `extractTrackSid()` reads
  it. The `mid` LiveKit states on a `TrackInfo` belongs to the PUBLISHER's
  connection; the mid our subscriber transceiver is given is assigned
  independently on ours, and the two agree only by coincidence. Keying on it
  meant a remote screen share ARRIVED AND DECRYPTED — 500+ frames, measured —
  with no surface waiting for it, so it was never drawn.
* **A video track must declare its size and one explicit layer.**
  livekit-client always sets `width`/`height` on `AddTrackRequest` (it waits on
  `waitForDimensions()` to do it), and the proto says of `VideoLayer.quality`:
  *"for tracks with a single layer, this should be HIGH"*. Declaring neither
  leaves the SFU to infer the track's shape — it logs the three-layer simulcast
  default `sdpRids ["q","h","f"]` — while we publish one untagged stream, so
  what it forwards and what we send do not describe the same track. With them
  declared the SFU records a single `{quality: HIGH, width, height, ssrc}`
  layer, which is what Chrome produces.
* **A desktop capture is VARIABLE RATE, and the encoder must not inherit that.**
  Measured straight off the portal on KDE/Wayland:
  `video/x-raw, format=BGRA, width=3840, height=2160, framerate=(fraction)0/1,
  max-framerate=(fraction)59/1` — PipeWire delivers a buffer when the screen
  CHANGES, not on a clock, and the source is the full 4K panel. The publish
  caps used a framerate RANGE `[0/1,30/1]`, so 0/1 negotiated all the way
  through: `videorate` had no target to convert to and `vp8enc` no rate to
  plan its bitrate against. Every WebRTC sender encodes at a steady cadence
  and every receiver's jitter buffer expects one. The framerate is now FIXED
  (`30/1`), which is what videorate is for — it turns the on-damage source
  into an even stream, repeating the last picture while the screen is still.
  The SIZE stays a range, because those are ceilings — but a range alone does
  NOT keep the picture's shape, and the bullet below is the whole of why. Note
  also: no `memory:DMABuf` feature appears, so the usual DMA-BUF stall is NOT
  what happens here.
* **A SIZE CEILING DOES NOT KEEP THE SHAPE. `videoscale` answers one with a
  non-square PIXEL ASPECT RATIO, and neither VP8 nor the RTP payload carries
  one.** This document and the source both said the opposite for as long as
  the ceiling has existed — "videoscale picks the largest size inside them
  that keeps the display aspect ratio, so an ultrawide stays ultrawide" — and
  it was false. videoscale does preserve the DISPLAY aspect ratio; it
  preserves it by clamping BOTH axes to the ceiling and signalling the
  difference as a PAR, and a libwebrtc receiver draws the frame at its literal
  width by height. Measured, `videoscale ! <the publish ceiling> ! fakesink`:

  | source | negotiated | PAR | what the far end draws |
  |---|---|---|---|
  | 3840x2160 | 1920x1080 | 1/1 | correct — 16:9, which is why nobody saw this |
  | 3840x2100 | 1920x1080 | 36/35 | 2.9% stretch |
  | 1920x1200 | 1920x1080 | 9/10 | 11% stretch |
  | 3440x1440 | 1920x1080 | 43/32 | 34% squash |

  Real elements, not only fixtures: `ximagesrc` on the maintainer's desktop
  negotiates `pixel-aspect-ratio=2/1` unpinned — a 2x squash at the far end,
  live the whole time and never reported, because the distortion only shows on
  a source that is not already 16:9.
  **Pinning `pixel-aspect-ratio=(fraction)1/1` into the ceiling caps** makes
  videoscale satisfy the ratio by choosing a SIZE, which is the only thing
  that survives to the far end: 3840x2100 -> 1920x1050, 3440x1440 ->
  1920x804, 1920x1200 -> 1728x1080, 800x600 unchanged (never upscaled).

  **SECOND-ORDER HAZARD, and it is worse than the defect it fixes.** Pinning
  the PAR downstream is exactly what makes `videoconvertscale` offer the
  SOURCE an OPEN PAR RANGE upstream, and a source that does not fixate PAR
  itself falls through to `gst_caps_fixate`, which takes a range's MINIMUM —
  `1/2147483647`. videoscale then dies converting it back:

  ```
  3840x2100 -> ERROR negotiation problem (integer overflow)
  3840x2160 -> ERROR negotiation problem
  1557x1213 -> "succeeds" carrying pixel-aspect-ratio=1/2147483647
  ```

  That is every window over the ceiling publishing NOTHING. Caught by
  independent review before it shipped, and the measurement that MISSED it was
  taken with `videotestsrc`, which fixates PAR in its own vfunc and therefore
  can never meet the open range. So there are TWO pins, not one: Lightning's
  own capture element fixates PAR 1/1 in its own `fixate`, AND a fixed
  `capsfilter caps="video/x-raw,pixel-aspect-ratio=(fraction)1/1"` sits
  immediately after the source. A fixed value is not a range, so nothing can
  fixate it to a minimum. Read out of the shipped plugin sources rather than
  assumed: `gstgdiscreencapsrc.c` builds its caps with a fixed 1/1 and
  `ksvideohelpers.c` sets a real `par_width`/`par_height`, so both Windows
  elements were already safe — and the maintainer's Windows log shows both
  negotiating `pixel-aspect-ratio=(fraction)1/1` while Lightning's own element
  negotiated no PAR field at all. `avfvideosrc` ends its fixate in a bare
  `gst_caps_fixate` and declares no PAR anywhere in the file, and
  `pipewiresrc` cannot be tested outside a portal session, which is why the
  belt-and-braces filter in front of the source is not optional.
* **The screen capture can deliver ONE frame and stop, and it is not the pool
  size.** Measured as `capture delivered frames count= 1` with no second
  report, while `videorate` repeated that frame into two thousand encoded ones
  — so every counter downstream looked healthy and both ends showed the screen
  frozen at the instant the portal picker closed. It is a RACE: another run of
  the same build reached hundreds of frames. pipewiresrc's `min-buffers`
  (default 1) was the obvious suspect, because this pipeline holds several
  buffers downstream; raising it to 8 was tried and made things STRICTLY WORSE
  — not one frame arrived. So the pool size is refuted, and the cause is still
  open. `capture negotiated caps=` is logged once per share for the next
  attempt: a `memory:DMABuf` feature there is the other classic reason a
  PipeWire capture stalls silently.
* **`videorate` also HOLDS the first picture, and that is the ~1 s freeze at
  the start of a share.** Measured in this repo's dev shell (GStreamer
  1.26.11) against exactly this caps shape — input `framerate=(fraction)0/1`,
  output pinned `30/1`: videorate emits **nothing at all** for the first
  buffer. It holds it until a SECOND buffer arrives, and then back-fills the
  whole gap in one sub-millisecond burst of duplicates whose RTP timestamps
  span it. A PipeWire desktop capture delivers ON DAMAGE, so that gap is
  "how long until something on the screen moves" — and a libwebrtc receiver
  renders on the frame timeline, so Element paints the opening picture and
  sits on it for exactly that long. Reported as "when i screen share from
  linux about for 1 sec the screen share is frozen then frames start going
  good and normal".
  **`pipewiresrc keepalive-time=100` was tried as the fix and made it
  STRICTLY WORSE.** The reasoning was good — re-push the buffer the element
  already holds, touching no caps, no pool and no negotiation, unlike the
  `min-buffers` experiment below. It was shipped on that reasoning without a
  live measurement, and the share then froze on its FIRST frame and never
  recovered. The LOCAL SELF-VIEW sat on "Waiting for the picture", which is
  what proves it: the self-view is tee'd off the capture, so it indicts the
  capture and not the network. Reverted in the same day.
  **That is the SECOND property tried here on reasoning alone and the second
  to kill the capture.** The rule this lane keeps re-learning: a GStreamer
  property change on the publish path is not a code review question, it is a
  measurement question, and the measurement is one live share.
  Two further alternatives measured and refuted before either was shipped:
  `videorate skip-to-first=true` changes nothing on this input shape — it
  cannot manufacture the second buffer videorate needs before it knows the
  interval — and `videorate max-duplication-time` removes the burst but keeps
  the hold AND starves the encoder below the pinned 30 fps, which is the
  condition that made Element refuse to render in the first place.
  **`skip-to-first=true` IS SET NOW ANYWAY, and that is not a reversal of this
  entry.** It was refuted AGAINST THE HOLD, on a fresh pipeline where the call
  age is ~0 and the property is a no-op by construction; it is the entire fix
  for a DIFFERENT defect, the back-fill in the next bullet. Both facts have to
  stay: delete the refutation and someone re-proposes it against the hold,
  delete the fix and someone removes the property.
  **The ~1 s hold is therefore ACCEPTED for now**, and the measurement that
  should precede the next attempt is: start a share, keep the desktop still
  for two seconds, and compare the timestamps of `capture delivered frames
  count= 1` against the first `frames encrypted video=` line. That comparison
  is now ONE line — `publish first encoded frame screenShare= afterPublishMs=
  firstCaptureMs= rateStageHoldMs=`, emitted once per publish by a
  self-removing probe on the encoder's src pad. Its first live reading, on the
  maintainer's desktop 2026-08-26, was `afterPublishMs=135 firstCaptureMs=58
  rateStageHoldMs=77`: 77 ms of hold on that share, on that machine. One
  capture BOUNDS the problem; it does not close it, because a genuinely still
  desktop still has nothing to deliver.
* **`videorate` starts its output clock at SEGMENT START, not at the first
  buffer's PTS — and THAT is a camera that sends one frame and freezes.**
  A publish bin is added to a publisher pipeline that has been PLAYING since
  the call was joined, and `ksvideosrc` and `gdiscreencapsrc` both stamp their
  buffers with the pipeline's RUNNING TIME. So a camera switched on three
  minutes into a call hands videorate a first PTS of three minutes, and
  `gst_video_rate_compute_next_ts` has it owing thirty duplicates for every
  second of that age. It emits them as fast as the encoder will take them: a
  full-rate stream of ONE picture, with every counter — captured, encoded,
  encrypted, sent — healthy. Reported as "camera didnt work, it sent one frame
  out and froze".
  MEASURED, not reasoned. `appsrc` pushing ten buffers at 10 fps into
  `videorate ! 30/1 ! fakesink`, GStreamer 1.26.11:

  ```
  first PTS      0 s, skip-to-first=false ->    27 buffers out
  first PTS     10 s, skip-to-first=false ->   327
  first PTS     10 s, skip-to-first=true  ->    27
  first PTS    174 s, skip-to-first=false ->  5247
  first PTS    174 s, skip-to-first=true  ->    27
  ```

  174 s is the real call age at which the camera was switched on in the
  Windows log, and that log's corroboration is exact: the monitor share was
  published 8.73 s into the call and encoded 262 frames more than a 30 fps
  steady state accounts for, against 30 x 8.73 = 262 predicted.
  **The negative control is Lightning's own `WindowCaptureSrc`**, which stamps
  from ZERO and therefore never back-filled — which is why a WINDOW share
  worked on Windows while the camera on the same build did not, and why that
  element must keep its zero-based timeline. A source on this path must not
  stamp pipeline running time; pace it on a clock anchor and keep the PTS
  zero-based.
  Pinned by `theRateStageDoesNotBackFillFromSegmentStart`, which drives the
  real rate stage and fails on the unfixed tree with "5247 buffers against 27".
* **`videorate` masks a dead capture.** It repeats the last picture to hold
  the output rate, so a screen capture that stalls still produces a full-rate
  stream of identical frames: every counter downstream — encoded, encrypted,
  sent — looks healthy while both ends show one frozen image. The capture's own
  buffers are therefore counted separately (`capture delivered frames count=`),
  and the capture queue is `leaky=downstream` so a software encoder falling
  behind at 1080p drops frames instead of reaching back and stalling the
  PipeWire source.
* **The local camera needs a self-view tee.** Our own camera is published,
  never received, so without one there is no local camera video anywhere and
  a local tile can only show an avatar while the capture light is on. The
  screen share already had this; both now route under `local:camera` /
  `local:screen`.

## The 2026-08-24/25 interop round: what was tried, and what actually worked

Calls went from "connects and carries nothing" to audio, camera and screen
share working in both directions against Element. It took a long chain of
separate defects, and roughly as many refuted theories. Both halves are
recorded, because the refuted ones are expensive to re-derive.

### What was actually wrong (in the order it was found)

| # | Defect | How it presented |
|---|---|---|
| 1 | `ParticipantUpdate` treated as the whole room, not a delta | permanent "1 person in call"; publishing our own audio deleted the peer |
| 2 | `JoinResponse.participant` (ourselves) dropped from the list | no local tile; mute could not find its own track sid |
| 3 | msid stream id taken whole, not unpacked at `\|` | remote peer silent AND invisible: keys and sinks keyed on a name matching nothing |
| 4 | Media key dropped when the sid was not yet known | that sender stayed undecryptable for the whole call; nothing re-sends a key |
| 5 | `SessionDescription.id` carried the target, not the offer id | answers never matched the server's offer |
| 6 | RED left enabled on encrypted tracks | RED wraps Opus, so the frame decryptor never sees the TOC byte |
| 7 | Call stage never replaced the timeline (both `fillHeight`) | the call UI squashed into a strip, message rows under the dock |
| 8 | Media keys addressed by membership alone | went to devices with a lingering membership rather than those in the call |
| 9 | `get_device` is a store lookup that fetches nothing | `no_devices`; falls back to a real `/keys/query` now |
| 10 | **Media key length required exactly 32 bytes** | element-call mints **16**; every key Element sent was rejected for its LENGTH — could never hear them |
| 11 | Received tracks attributed by a pad-name index | LiveKit's subscriber offer has a data channel in section 0, so `src_0` read the wrong section: frames decrypted into an empty ring |
| 12 | **`rtpvp8pay` parses the VP8 bitstream** | cannot payload an encrypted frame; share published exactly one frame then STREAM/ENCODE. Replaced by `RtpVp8Payloader` |
| 13 | Video track declared with no size and no layer | the SFU inferred three-layer simulcast for a single untagged stream |
| 14 | Local camera had no self-view tee | capture light on, tile showing an avatar |
| 15 | Portal request could wedge `m_busy` forever | "sometimes it won't let me share" |
| 16 | **Publish caps allowed `framerate=(fraction)[0/1,30/1]`** | a desktop capture negotiates `0/1`, which propagated to `vp8enc`; fixed at `30/1` — this is what finally made Element render |

### Theories that were tested and REFUTED — do not re-propose

* **The frame crypto is wrong.** It is not. An independent implementation of
  LiveKit's format (WebCrypto, transcribed from `livekit-client`) decrypts our
  audio, VP8 keyframes AND VP8 delta frames byte-for-byte.
* **RTP timestamps stall.** They advance per frame and are identical across a
  frame's packets.
* **The keyframe/delta flag disagrees with the bitstream.** It agrees on every
  frame, so both ends pick the same cleartext header length.
* **Keyframe requests (PLI) never reach the encoder.** They do, through our
  own payloader.
* **Our packetization is malformed.** Every frame survives strict
  libwebrtc-style reassembly, and an independent pion subscriber
  (`livekit-cli`) receives camera *and* screen share at ~1.1 Mbps, 0% loss.
* **The `encryption` sliding-sync connection never starts.** It does. That
  reading came from a harness that never synced.
* **The capture stalls because pipewiresrc's pool is one buffer.** Raising
  `min-buffers` made it strictly WORSE — no frame at all. Reverted and pinned.
* **DMA-BUF.** The negotiated caps carry no `memory:DMABuf` feature.

### Harness traps that produced false findings

Every one of these produced a confident wrong conclusion at least once:

* `startSync()` returns silently unless the session is logged in, and restore
  is ASYNCHRONOUS — a harness that calls it too early never syncs while still
  sending, reading state and connecting to the SFU.
* Publishing a screen share before the call reaches `Connected` reaches
  `ensurePeer()` with an inactive engine: it returns `true` and puts no track
  on the wire.
* Sampling the SFU before the share starts shows a call with no screen share
  and looks exactly like a forwarding failure.
* Test-source mode used to end video receive in a `fakesink`, so the whole
  route-to-a-surface path was exercised only on a real desktop.
* `videorate` repeats the last picture, so a DEAD capture still produces
  full-rate encoded frames and healthy-looking counters everywhere downstream.

### The diagnostics that made it findable

Counters and one-line facts, never content. `capture delivered frames count=`
(before videorate) against `frames encrypted` is the single most valuable
pair: in step means healthy, diverging means a dead capture being repeated.
Then `capture negotiated caps=`, `received track attributed=`, `media key
sent/received`, `video frames decrypted but NOT rendered`, and the peer
connection's ICE/DTLS transitions.

`tests/CallLiveDiagnostic.cpp` drives a real call against a real homeserver
and SFU, headless, and SKIPS unless `LIGHTNING_LIVE_*` is set.

### Getting a log out of a PACKAGED build

A tester on Windows or macOS has no terminal. `--console` opens one on
Windows but reopens stdout ONTO it, so a shell redirect captures nothing,
and a macOS bundle started from Finder has no console at all. Use
`--log-file`:

```powershell
# Windows (PowerShell). cmd users: "%LOCALAPPDATA%\Programs\Lightning\Lightning.exe" --log-file "%USERPROFILE%\Desktop\lightning.log"
& "$env:LOCALAPPDATA\Programs\Lightning\Lightning.exe" --log-file "$env:USERPROFILE\Desktop\lightning.log"
```

```sh
# macOS
/Applications/Lightning.app/Contents/MacOS/Lightning --log-file ~/Desktop/lightning.log
```

It APPENDS and flushes every line, so a crash keeps the lines that explain
it, and several runs accumulate in one file. It is a mirror of the same
stream the console gets — no tokens, no passwords, no recovery keys, no
message bodies.

When calls do not start at all, `--call-media-status` answers why without
needing a call, and names the GStreamer version it loaded.

### Reading `received track attributed=`

One line settles which of four things went wrong, and they have nothing in
common:

```
received track attributed= true trackKey= "TR_…" fromPadMsid= true
    sdpSections= 3 padSenderUnknown= false
```

* `fromPadMsid= false` — webrtcbin's pad `msid` property gave nothing and
  the ids came from our own SDP parse. Expected on a runtime whose
  webrtcbin populates it differently; **this is the packaged Windows and
  macOS case** (1.28.x) and not the dev shell's (1.26.x).
* `padSenderUnknown= true` — the property gave a sender that appears in NO
  section of the description we were sent. Worse than empty: a well-formed
  id nobody will ever send a key for. Re-derived from the SDP.
* `sdpSections= 0` — our own scan of the subscriber description recorded no
  media sections. Nothing to match against, so look at
  `applyRemoteDescription`, not at the pad.
* `attributed= false` with `sdpSections` non-zero — the sections are there
  and the transceiver mid matched none of them.

`trackKey` must be a `TR_…`. A small integer there is a media-section mid
standing in for a track sid; that key routes video to no surface, and it is
what shipped before 2026-08-26.

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
* Screen capture, per platform, and never direct framebuffer access on the
  one platform that has a broker:
  * **Linux — `pipewiresrc` with a node id from an xdg-desktop-portal
    ScreenCast session**, so the user's own portal dialog decides what is
    shared. A negative node id is refused rather than defaulted, because
    "whatever PipeWire feels like" is exactly how you publish the wrong
    monitor.
  * **Windows — `gdiscreencapsrc` for a display, `lightningwindowcapturesrc`
    for a single window.** A window carries an HWND and NO node id, so every
    refusal on this path has to accept exactly one of the two names; see the
    Windows round below for the guard that did not.
  * **macOS — `avfvideosrc capture-screen=true`**, displays only.
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

**NOT TESTED — and this is the honest headline** *(SUPERSEDED 2026-08-25:
calls now carry audio, camera and screen share both ways against Element, and
the frame cryptor is attached. Kept as the record of what this round could and
could not claim — see "The 2026-08-24/25 interop round" and the sections after
it for the current position)*:

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

Rebuilt 2026-08-26 as a PRESENTATION-only round: nothing below reaches the
wire. No publish cap, `vp8enc` property, `RtpVp8Payloader`, frame cryptor,
ssrc/msid/`TrackSource`, `AddTrackRequest` or negotiation ordering changed.

* **`CallStage`** — a PANEL AT THE TOP of the conversation column, with the
  messages still visible and scrolling below it. It used to REPLACE the
  timeline; Discord's DM call is the arrangement copied here, and the
  maintainer asked for it in those words. 40% of the column voice-only, 70%
  once anything sends video, clamped to [220px, 75%], collapsible to a 64px
  strip, resizable by a divider whose value is committed on the FALLING EDGE
  of the drag — a release moves nothing and emits no `heightChanged` (§16).
  The reader's message does not move when a call starts: the timeline is a
  rotated Flickable pinned to the BOTTOM edge, this changes HEIGHT only, and
  nothing writes `contentY`, so the space comes off the top.
* **A SHARE IS A TILE, NOT A MODE.** `CallTileGrid` builds the grid over
  SURFACES, not people: one cell per camera and one per screen share, so a
  sharer with their camera on is TWO cells and two sharers are two cells.
  This replaced `sharingPerson`, which looped participants and returned the
  FIRST match — a second simultaneous share was structurally unrepresentable.
* **A dismissed share can always be reached again.** The old "Back to grid"
  wrote `layoutMode = "grid"` and NOTHING anywhere wrote it back, so the
  spotlight was unreachable for the component's lifetime and only a room
  switch (which destroys the Loader) recovered it. Reported as "now if share
  is closed no way to get it back". `CallStageState` (C++, call-scoped, so it
  survives that Loader) holds the dismissal, and it applies to the SPOTLIGHT
  only — `CallShareModel` has no `dismissed` role and does not know the class
  exists, so a dismissed share is still a row, still a grid tile, still
  routable. A NEW share re-arms `auto`.
* **The speaking ring reads AMPLITUDE, and Discord's cannot.** LiveKit's
  `SpeakerInfo` carries `level` 0..1; `rust/src/sfu.rs` had been sending it
  and `SfuCallController` was discarding everything but `active`. The ring's
  gap is `3 + 6 * level` with a ~60 ms attack and ~220 ms release, drawn as a
  free child of a fixed-size holder so it contributes no implicit size and
  reflows nothing. **No level is fabricated from the boolean** — an SFU that
  reports only `active` degrades to the fixed 3 px ring. Discord's own voice
  gateway speaking payload is a bitmask with no amplitude field at all.
* **`CallParticipantModel` is a real `QAbstractListModel`.** It was a
  `Q_INVOKABLE QVariantList` re-invoked behind a hand-bumped tick and bound
  into views — a MODEL RESET on every change, fired continuously while anyone
  talked, destroying every tile and its `VideoOutput` on every syllable. The
  same defect the Spaces rail hit. Membership changes are
  `begin{Insert,Remove,Move}Rows`; value changes are per-row `dataChanged`
  naming only the changed roles.
* **A VIDEO ROUTE IS OWNED BY THE SINK THAT CLAIMED IT** (2026-08-27).
  `SfuVideoRouter` holds one `QVideoSink` per routing key, and a release
  now names the SINK: `releaseSink(sink)` gives up every key that sink owns
  and nothing else. The four key-named detaches on `SfuCallController` are
  gone, replaced by one `detachSink(QObject *videoSink)`.

  Without that rule the surface above could not work at all. Qt destroys a
  deactivated Loader's content and a regenerated Repeater's delegates with
  `deleteLater()` while creating the replacements SYNCHRONOUSLY, so the
  order on every grid↔spotlight swap and on every participant reorder is:
  new tile attaches, THEN old tile detaches. A key-named detach removed
  whatever was there, so the dying tile unhooked the living one — and since
  a tile attaches only on creation and on a routing-key change, nothing
  ever put it back. The video was gone for the rest of the call.

  Both of the maintainer's reports are this one defect: "camera no longer
  works" (a `beginMoveRows` on the participant model, which
  `QQuickRepeater` answers by regenerating every delegate) and "when i full
  screen it it stop shwoing video" (the grid→spotlight Loader swap, which
  fires by itself the moment any share appears). It was masked before this
  round by accident: the stage bound a JS array rebuilt on every update, so
  every tile was destroyed and re-created several times a second and
  re-attached itself. Removing that churn — which is what made an
  amplitude-driven speaking ring possible — exposed a defect that had been
  there since the router was written.

  **CORRECTION, 2026-08-27: "camera no longer works" was NOT one defect.**
  The router fault above is real and is fixed, and the camera report SURVIVED
  it — on Windows the remaining cause was `videorate` back-filling from
  segment start (see "LiveKit wire facts"), which produces a camera that sends
  one frame and freezes with every counter healthy. Two independent faults
  wearing one report, and the first fix looked plausible enough to close it.
  Ask what the CAPTURE delivered before concluding anything about the route.

  Two smaller rules fall out of the same reasoning and are pinned:
  `attachSink(key, nullptr)` is a NO-OP rather than an eviction, and
  `clear()` on teardown stays UNCONDITIONAL (there is no surviving owner to
  protect, and honouring ownership there would leave exactly the stale
  entries it exists to remove).

  The older claim that "the grid and the spotlight are mutually exclusive
  Loaders" was the stated safety property, and it is true of their `active`
  and NOT of their object LIFETIME. That gap is where the defect lived; the
  comment in `qml/CallShareTile.qml` has been corrected.

* **FULL SCREEN is a separate top-level `Window`** (2026-08-27), because
  "full monitor" is what was asked for and an overlay can only ever fill
  the application window. `CallStageState::fullScreen` owns the flag —
  call-scoped, so it survives the room-switch Loader — and REFUSES to enter
  with nothing focused, dropping itself whenever the spotlight empties: a
  black monitor with no obvious way out is the one state this feature must
  never reach. The stage's grid and spotlight both stand down while it is
  up, so there is still exactly one surface per routing key.

  Escape is a `Keys` handler on the window's focused item, never a
  `Shortcut`: Esc is in `ShortcutRegistry`'s RESERVED list and two enabled
  Shortcuts on one sequence fire NEITHER. The window's visibility is driven
  imperatively (`showFullScreen()`/`hide()`) rather than bound, because a
  binding on `visibility` is a binding on the property a window manager
  writes when the user closes the window. `onClosing` ACCEPTS the close and
  writes the flag back — refusing it would veto Ctrl+Q.
* **`CallParticipantTile`** — avatar, speaking ring, name strip, state
  badges. A badge appears only when the SFU actually reported that track's
  state; unknown renders nothing rather than a confident "not muted".
* **`VoiceConnectedBar`** — the persistent footer in the room list. The call
  does not end because the user opened another room, and this is how they get
  back to it.
* **`CallControlBar` was DELETED** — nothing instantiated it and the contract
  test banned it from the stage. `CallHeaderBar` carries the controls in both
  of its placements. Every control still reaches something real; nothing is
  shown disabled with a tooltip, because a disabled control receives no hover
  in Qt Quick and so cannot explain itself. The screen-share control has no
  device chevron for a related reason: on Linux the PORTAL is the picker, and
  on Windows and macOS pressing the control opens Lightning's own picker
  (below), so a chevron would be a second route to the same dialog.
* **The timeline's call row** is its own kind now, not a room-state row. It
  used to arrive as `state_kind: "m.call"` with the literal body "call
  event", which the activity grouper drew as "1 room update" — reported as
  "also room event look bleak". It now says who started the call and whether
  it was video, and carries a Join button ONLY while the room's session is
  live. The BUTTON is the only join target: Discord had a period where the
  whole row was clickable and people joined by accident.

Layout and interaction follow Discord; every colour, radius and type value
comes from `AppTheme`, so all eleven themes and the text scale apply. No
Discord artwork or colour is used. One honest caveat written into the source:
CURRENT desktop Discord draws tiles even for a voice-only call — circular
"bubbles" are its mobile and older DM presentation. Lightning draws the
circles because that is what was asked for, not because Discord does it
today.

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

**NOT TESTED — the honest headline** *(SUPERSEDED 2026-08-25 and after, on
every bullet in this list except federation, TURN traversal and reconnection.
Kept for the record; the current position is the status line at the top of the
document and the last two sections)*:

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

## Stopping a published track: `a=inactive`, not a mute, not a pad release

Live-confirmed 2026-08-26. Getting here took four rounds, and each of the
first three fixed something real without fixing the report — so all four are
recorded, because the near-misses are the reusable part.

**There is no unpublish verb on this wire.** Checked in
livekit-protocol 0.7.12 rather than assumed: `SignalRequest` carries
`AddTrack`, `Mute`, and an `UnpublishDataTrackRequest` for DATA tracks only.
A media track is withdrawn by RENEGOTIATION — the publisher offers without
it — which is what makes the SDP shape the entire problem.

| Round | What was wrong | What it actually cost |
|---|---|---|
| 1 | `unpublish()` set a bin to NULL while it sat in the PLAYING pipeline mid-push | Deadlocked the GUI thread. Core dump: this thread in `gst_pad_set_active` wanting the pad's stream lock, `queue1:src` holding it parked in `do_probe_callbacks` → `g_cond_wait` |
| 2 | Fixed with a `GST_PAD_PROBE_TYPE_IDLE` probe — which never fired | A pad pushing into a webrtcbin that is not draining never becomes idle. Instrumented: "probe installed", nothing for 3 s, "probe fired" during teardown. Not a deadlock any more; simply never ran |
| 3 | Released the webrtcbin request pad synchronously (which also unblocks the pusher, so round 2's probe now fires) | Dropped our msid and left the section `a=sendrecv`. The far end is told we are still sending on a section with nothing behind it |
| 4 | Set the transceiver `direction` to INACTIVE before releasing | Works |

Measured, on the renegotiated offer:

```
before unpublish   m=video ... | a=sendrecv
after  (round 3)   m=video ... | a=sendrecv     <- msid gone, still sending
after  (round 4)   m=video ... | a=inactive
```

Two invariants, both asserted by
`SfuMediaEngineTest::theOfferAfterUnpublishNoLongerAdvertisesTheTrack`:

* the section goes **inactive**, and
* the section **COUNT does not change**. An m= section may never be removed
  from an SDP; a shrinking offer is its own protocol fault, not a fix.

**A MUTE CANNOT DO THIS AND NEVER COULD.** `applyVideoState()` expressed a
stop as `sfuMuteTrack` because it was the only removal-shaped verb available,
and a mute removes nothing — the stopped track stays in the participant's
list for the rest of the session, which is precisely the grey tile that never
cleared. The mute is retained as belt-and-braces (one signal, covers the
window before renegotiation lands) and must never again be the only mechanism.

**The self-view is not evidence.** It is tee'd off the CAPTURE, upstream of
encryption and of the SFU entirely, so it looked perfectly correct through all
four rounds while nothing usable reached anyone else. When a share is reported
broken remotely and fine locally, that gap is the diagnosis, not a puzzle.

## Microphone loudness: `webrtcdsp`, probed and optional

Lightning was audibly quieter than Element on the same microphone because
element-call captures through the browser's WebRTC audio path, which runs
automatic gain control, and Lightning ran none. `webrtcdsp` is that same
processing module. Measured through the real chain shape on a -29 dBFS sine,
sampled after convergence:

```
without   rms 1158.0   -29.0 dBFS
with      rms 3935.2   -18.4 dBFS      (+10.6 dB, ~3.4x)
```

It is **probed at runtime and optional**: it lives in gst-plugins-bad, and a
`gst_parse` description naming an element that does not exist fails to PARSE —
which would remove the microphone entirely rather than leave it quiet. With it
absent the chain is byte-identical to the pre-existing one, which is also why
it is deliberately NOT in the engine's required-element list.
`echo-cancel=false`: real echo cancellation needs a `webrtcechoprobe` in the
PLAYBACK path, and claiming it without one cancels against nothing.

**The volume curve.** Sliders read 0-200; 200 MEANS 1000% of audio.
`SfuMediaEngine::audioFactorPercent()` keeps 0-100 literal — a curve there
would make every setting below unity mean something other than it says, and 0
must be exactly silence — and expands 100-200 onto 100-1000. A straight 0-200
slider tops out at +6 dB, reported as "above 100% barely any difference"; a
straight 0-1000 slider puts every useful setting in its first tenth. 1000 is
the GStreamer `volume` element's own factor ceiling (range 0-10), not a number
chosen here. Stored and displayed values are always the user scale.

## Raised hands: element-call's own reaction, read out of its source

**Landed 2026-08-26.** Live Element interop: **NOT TESTED**.

The control existed and was honestly labelled "only shown on this device":
`setHandRaised()` reached no SFU, no membership and no to-device message.
The note against it said inventing a wire representation was "a protocol
decision to be checked against a real element-call client, not guessed at
here", so this round went and read one.

### The format

From `element-call/src/reactions/useReactionsSender.tsx` and
`ReactionsReader.ts`:

| | |
|---|---|
| **Raise** | an `m.reaction` whose `m.relates_to` is `{ rel_type: "m.annotation", event_id: <the sender's OWN m.call.member state event>, key: "🖐️" }` |
| **Lower** | a **redaction** of that reaction |
| **Read** | annotations of each membership event, keeping only those whose SENDER owns that membership |

Three things about it are load-bearing and none of them is obvious.

**The target is the sender's own MEMBERSHIP STATE EVENT, not a timeline
message.** That is what scopes a hand to one call rather than to the room's
history: rejoining or refreshing publishes a new membership event, so an old
hand cannot follow the user into the next call. It is also why
`RtcMember` had to start carrying `event_id` — `parse_session_membership`
sees content alone, so `read_session` fills it from the envelope, and a
membership read from a source with no envelope keeps it EMPTY (which reads as
"no hand can be matched", never as a wrong match).

**The key is two code points.** U+1F590 RAISED HAND WITH FINGERS SPLAYED
followed by U+FE0F VARIATION SELECTOR-16. element-call compares the whole
string, so the same emoji without the selector is a hand no Element client
will ever see — and the two are visually identical in every editor, which is
why `the_raised_hand_key_is_element_calls_own_bytes` asserts the seven UTF-8
bytes rather than the literal.

**The sender must own the membership they annotate.** Anyone may react to
anyone's state event. Without that check one user could raise everybody's
hand, so `RtcController::identityForMembership` refuses a sender who is not
the membership's own user, and the Rust join-time sweep applies the same rule.

### Three lanes, and why there are three

* **Ours.** `mx_rust_rtc_set_hand` sends or redacts. The answer carries the
  reaction's event id, and keeping it is not optional: a hand can only be
  lowered by redacting the specific event that raised it, so a client that
  forgets the id has raised a hand it can never lower.
* **Live.** Two sync handlers, deliberately UNFILTERED by room: a reaction is
  cheap to inspect and almost all of them are rejected by the key comparison,
  where filtering on "the room we are in a call in" would need the handler to
  hold call state it has no business holding — and would drop a hand raised
  in the window between joining and that state being written.
* **The backlog.** `mx_rust_rtc_read_hands`, spent ONCE per join. A hand
  raised before this client arrived produces no sync event for us, so without
  it an early raiser is invisible for the whole call. Bounded at
  `MAX_HAND_PROBES` memberships and cache-first, because it is one relations
  load per membership in the worst case.

**A redaction names only what it removed.** The reaction is gone by the time
we see it, so nothing on the wire can say whose hand it was:
`SfuCallController` holds `reaction event id -> identity` and answers from
that. It is also what makes forwarding every redaction in every room cheap —
one hash lookup rejects the ones that are not ours.

**Only our own row is optimistic.** The toggle is a control the user is
watching and the round trip is a second or more, so the local badge lights
immediately and a REFUSAL puts it back. Nothing else is: a remote hand is
only ever drawn from an event that actually arrived, and a failed read
contributes nothing rather than a lowered hand — absence of evidence is not
evidence that a hand is down.

## Remaining work, stated plainly

The first three items on this list were **DONE** by the 2026-08-24/25 interop
round and the 2026-08-26 surface round, and were left standing here long
enough to mislead a reader. Recorded rather than silently deleted, because a
list that quietly loses entries cannot be trusted either:

* ~~Wire the frame cryptor into the pipeline~~ — done; see "Call E2EE: real,
  and cross-checked against an independent implementation" above.
* ~~Portal integration for screen-share source selection~~ — done
  (`src/calls/ScreenCastPortal.cpp`), and the fd it returns is load-bearing.
  Windows and macOS have no portal and now get Lightning's own picker
  (2026-08-26/27; `qml/ScreenSharePicker.qml`), which on Windows also offers a
  single WINDOW. What survives as item 1 below is the CAMERA: a device list
  exists, and the SFU publish path does not read it.
* ~~Video rendering / no `QVideoSink` bridge yet~~ — done
  (`src/calls/SfuVideoRouter`), and its one-sink-per-key rule is now an
  ownership rule; see "What the UI is".

## Windows and macOS: the capture elements, and what differs

**Landed 2026-08-26**, pipeline 121, both platforms green. Each artifact
proves itself: `matrix-client --call-media-status` runs from inside the
packaged build and reports

```
call media engine built in: yes
bundled plugin directory: .../Lightning/gstreamer-1.0
gstreamer: initialised
1:1 call engine: available
group call (SFU) engine: available
RESULT: calls can be placed and answered.
```

Both validators fail the build without that line AND without the bundled
directory being the app's own — falling back to a system GStreamer would pass
on both runners and fail on every user's machine.

**Windows: PASS, 2026-08-27.** The maintainer placed a real call from a
packaged Windows build (pipeline 135, artifact from `9f829a3`) and confirmed
the CAMERA works and SCREEN SHARE works. Calls were already being placed from
the pipeline 134 package the day before — that build could share a monitor and
list windows, it simply sheared the wide ones — so what changed here is that
the camera produces motion and a share is correct, not that a package can call
at all. macOS remains NOT TESTED.
**macOS: NOT TESTED.** Nothing has ever been placed from that package; it
needs a person on the machine, and everything recorded for it here is the
packaging and the engine probe.

A packaged Windows or macOS build had NO media engine at all. CMake sets
`HAVE_LIGHTNING_WEBRTC` from a pkg-config probe, the Windows cross-builder had
no mingw GStreamer and the Mac mini had none installed, so both shipped a
client whose every call answered "Joining isn't available". The refusal was
correct; the cause was packaging, not code.

Both now bundle GStreamer. The engine's capture sources are per-platform:

| | Linux | Windows | macOS |
|---|---|---|---|
| camera | `v4l2src` | `ksvideosrc` | `avfvideosrc` |
| screen | `pipewiresrc` | `gdiscreencapsrc` | `avfvideosrc capture-screen=true` |
| ONE WINDOW | `pipewiresrc` (the portal picks it) | `lightningwindowcapturesrc` — ours | not available |
| audio | `autoaudiosrc`/`autoaudiosink` → Pulse/PipeWire | → WASAPI | → CoreAudio |

Audio needs no branch: `autoaudiosrc` is an autodetect bin that instantiates
the highest-ranked native element on each platform. Everything from the encoder
onward — `webrtcbin`, opus, VP8, nice, dtls/srtp, Lightning's own payloader and
frame cryptor — is identical everywhere, and must be: it is what Element and
the SFU see on the wire.

### The traps, each one measured

**Fedora's mingw GStreamer ships the webrtc LIBRARY and not the `webrtcbin`
PLUGIN** — plus no nice, srtp, opus or vpx. A `.pc` file and a DLL of the right
name are not the element. The official upstream MinGW SDK is the route.

**Property names differ between capture families.** `gdiscreencapsrc` takes
`monitor` and `cursor`; `d3d11screencapturesrc` takes `monitor-index` and
`show-cursor`. `gst_parse_launch` fails outright on an unknown property, so
using one family's names with the other's element is a screen share that can
never start.

**A UCRT/msvcrt CRT split, invisible to the Wine probe.** The upstream SDK is
a UCRT build; the toolchain is msvcrt. mingw-w64's `wchar.h` makes `mbstate_t`
a struct under `_UCRT` and an `int` otherwise, so `libgstd3d11.dll` and
`libgstmediafoundation.dll` import `_ZNSt7codecvtIwc9_MbstatetEC2Ey`, which the
staged libstdc++ does not export. Windows fails a missing NORMAL import at
LoadLibrary with `ERROR_PROC_NOT_FOUND` — **and Wine loads it anyway**, so the
element probe passes while the feature is dead on the target platform. Those
two are not shipped, which costs Windows the cheaper Desktop Duplication
capture; a symbol-level walk over the staged closure is the only check that
sees this class of defect and now runs in validation. It is a CRT CHOICE, not
version drift: a GStreamer bump will not fix it.

**macOS codesign refuses a plain directory of dylibs in the bundle.** The
working shape is `Contents/MacOS/gstreamer-1.0` as a SYMLINK to
`../PlugIns/gstreamer-plugins`. And `macdeployqt` rewrites the app's GStreamer
glib/gobject dependencies into `Contents/Frameworks` out of Homebrew, splitting
the GObject type system between Qt's GLib and the plugins' — silently, past
every existing check. Validation now asserts every GStreamer library the
executable loads is the staged copy.

### Sharing ONE WINDOW on Windows: Lightning ships its own capture element

**Landed 2026-08-26** (`src/calls/WindowCaptureSrc.{h,cpp}`), and it is the
SECOND GStreamer element this repository compiles into the binary and
registers at init — `lightningrtpvp8pay` is the precedent.

The reason is not design. `gdiscreencapsrc`, the only capture element in the
shipped Windows set, takes a `monitor` index and a crop rectangle and has no
window property at all. The element that does — `d3d11screencapturesrc`, with
`window-handle` — lives in `libgstd3d11.dll`, which the UCRT/msvcrt split
above forbids shipping. So the capture is ours, and it costs no new
dependency: plain Win32 GDI, with `DwmGetWindowAttribute` resolved through
`GetProcAddress` rather than linked, so the validated packaging closure gains
no new edge.

Decisions in it that are decisions, not defaults:

* **IT ASKS THE WINDOW TO DRAW ITSELF** (`PrintWindow` with
  `PW_RENDERFULLCONTENT`) rather than reading the screen where the window
  sits. Cropping the screen to the window's rectangle is far less code and was
  refused: anything stacked on top — another app, a password prompt, a
  notification — would be shared too. A share that can leak a window the user
  did not choose is not a feature. The honest cost of that choice is that a
  window rendering through its own swapchain may print BLANK; it is reported
  as a black frame, not as a failure to start, and the picker's preview uses
  the same path so a blank tile tells the truth in advance.
* **Geometry is the WINDOW rect cropped by `DWMWA_EXTENDED_FRAME_BOUNDS`**,
  from ONE helper shared by the capture, the picker's row and the picker's
  preview — so a row can never advertise a shape the share does not send.
  PrintWindow draws the window rect, not the client rect: sizing the surface
  to the client rect (what the element did first) is the wrong rectangle
  before anything else goes wrong.
* **Caps are negotiated ONCE.** A window resized mid-share is letterboxed into
  the agreed rectangle rather than renegotiating under an encoder and an SFU
  that have already settled a resolution.
* **A window CLOSING is EOS, not an error.** Closing what you were sharing is
  an ordinary thing to do and must not tear down the call — see the round
  below for what has to listen for it.
* **Its own 30 fps clock and a ZERO-BASED PTS.** Unlike a damage-driven
  desktop capture, a perfectly still window still streams, so there is no
  `videorate` first-buffer hold to wait out; and because it stamps from zero
  rather than from pipeline running time, it never triggered the back-fill
  that froze the camera. Both properties are load-bearing and must survive any
  rewrite of the element.
* **Our own windows are never offered.** Sharing the call into the call is a
  hall of mirrors, and a picker that lists it invites the mistake.
* **The log says `window=` as a BOOLEAN.** An HWND and a window title belong
  to the user's desktop, not to a log they may be asked to send.

WHAT IS ACTUALLY KNOWN ABOUT IT RUNNING, which is not the same as what a
Linux build can check. None of this file compiles off Windows, so the local
gate is a cross-compile syntax check (`x86_64-w64-mingw32-g++-posix
-fsyntax-only` inside `debian:13.6-slim` against Linux Qt/GStreamer headers,
with a stub for `QtGui/qwindowdefs_win.h`, and a probe TU proving the parse
reached the end of the file). Everything beyond that comes from the packaged
artifact and the tester:

* **It runs and it captures.** On the pipeline 134 build the picker listed
  real windows and a File Explorer share arrived correctly at the far end.
* **It was WRONG in a way only a real desktop showed**, which is the round
  below: windows wider than the publish ceiling arrived sheared.
* **The fix is NOT confirmed against those windows.** Pipeline 135 confirmed
  the camera and a screen share; nobody has since shared Brave or G Hub.

There is no Wine-based check of this element. The pipeline's element probe
covers PLUGINS, and this one is compiled into the executable rather than
loaded from the plugin directory, so it is invisible to that probe by
construction — the same blind spot that hid the missing sctp plugin, inverted.

### The 2026-08-27 Windows round: what was tried, and what actually worked

The first packaged Windows build that could share a window did it badly, and a
real Windows log settled every question in this section. Live validation of
the outcome — **PASS**, on a packaged Windows build (pipeline 135, artifact
from `9f829a3`): the CAMERA works and SCREEN SHARE works. Everything else this
round touched is **NOT TESTED**.

**THE GARBLING: the element published one size and produced another.**
`WindowCaptureSrc` implemented no `set_caps` vfunc. It advertises a
width/height RANGE, so fixation landed wherever downstream allowed — the
Windows log records its own src pad settling at BGRA 1920x1080 for a 3840x2100
window and BGRA 1556x1080 for a 1557x1213 one — while `createFrame` went on
allocating and copying a buffer of the WINDOW's size.
**`gst_video_frame_map` only refuses a buffer SMALLER than the caps imply.**
An oversized one passes in silence and is then read at the CAPS' stride, so
every displayed row was half a source row and only the top quarter of the
window was ever consumed. A window under 1920 wide escaped the shear because
its stride matched, losing only its bottom rows — which is exactly why File
Explorer "worked perfect" while Brave was "a garbled mess, just the top right
corner".

**FIXATING STRUCTURE 0 IS NOT FIXATING AGAINST THE PEER.**
`videoconvertscale` answers a caps query with the DOWNSTREAM-RESTRICTED
structure FIRST — passthrough is cheaper, so it is offered first — and
appends the size-opened one after it.
Fixating `gst_caps_get_structure(caps, 0)` blind therefore lands inside the
1920x1080 publish ceiling and leaves the element scaling in GDI, one halftone
`StretchBlt` per frame. `gdiscreencapsrc` never meets this because it reports
FIXED caps: the restricted structure intersects to nothing and drops out. That
one difference is the whole reason a MONITOR share negotiated the full
3840x2160 and a WINDOW share did not, on the same build, in the same call.

The element now: learns its negotiated size in `set_caps`; keeps the PRINT
surface separate from the OUTPUT surface and rebuilds it only on a resize (it
was building a ~32 MB DIB per frame, and leaking it — `DeleteObject` on a
bitmap still selected into its DC does nothing); prefers a caps structure that
admits the window's native size; fits rather than clamping each axis
independently (`fitInto`, even edges for VP8's 16x16 macroblocks over
subsampled chroma, and it NEVER upscales); attaches a `GstVideoMeta` so no
consumer takes the stride on trust; paces itself on the clock instead of
running as fast as PrintWindow returns (which paced a 4K window at 32-35 fps,
burning a core on frames `videorate` then discarded); and derives its
timestamps from a clock-anchored frame index, so a capture that cannot keep up
drops frames instead of slowing the stream's own clock.

**A CAPTURE THAT ENDS ITSELF, and nothing was listening.** Closing the shared
window answers with EOS — correctly — and the consequence was: the encoder
stopped, the track stayed DECLARED, the far end kept the last frame
indefinitely, and this end still showed "Your screen" with Stop armed. An EOS
probe on the capture's src pad now retires the publish through the same path
the Stop button uses, which is the one that sets the transceiver INACTIVE and
actually clears the far end's tile. It reports as its own category
(`screen_share_source_closed` / `camera_source_closed`) so the wording is
"The window you were sharing was closed", not "Screen sharing couldn't
start" — and it rides `publishFailed`, which turns the button back off, NOT
`failed`, which would end the call over one closed window.

**THE CAMERA FREEZE WAS NOT THE CAMERA.** It is `videorate` back-filling from
segment start; the measurement, the corroboration and the negative control are
in "LiveKit wire facts" above. The reason it is worth restating here: the
symptom named the wrong subsystem twice over. A camera that sends one frame
and freezes, on a build whose window share works, looks like a camera element
or a video route — and the 2026-08-27 video-router ownership fix had already
been made against exactly that report without curing it.

**THE PICKER became a tabbed grid** (`qml/ScreenSharePicker.qml`): a list of
64 px strips beside a caption could not answer the question it exists to
answer. Two general defects fell out of wiring it up, both with teeth:

* **`QObject::findChild` cannot reach a `Repeater`'s delegates.** They belong
  to the delegate model, not to the item they are laid out inside. Established
  by giving a segment a CONSTANT `objectName` and watching it stay absent from
  a full `findChildren` dump — which rules out a binding that failed to
  evaluate. Walk `childItems()` instead.
* **A change handler can run BEFORE the bindings that depend on the same
  property.** `onTabChanged` read `shownRows`, a binding on `tab`, and got the
  tab being LEFT: the rescue that keeps a tile highlighted moved the selection
  into the old tab, so switching to Applications with a display selected
  showed window tiles with nothing highlighted while Share still pointed at
  the display. Every imperative reader calls the function directly now; only
  the GridView's model — the thing being updated — reads the binding.

And one rule the filtered view must keep: **a row carries the index it has in
the UNFILTERED array**, because that is what the controller indexes into.
Getting it wrong shares the wrong source silently, which is the worst outcome
this dialog has. Pinned by a test built on interleaved rows, where a naive
view-index implementation picks a window when the user pressed a screen.

**A GUARD WITH A TWIN.** `startScreenShare` refused on `pipewireNodeId < 0`
alone, and a window share carries a handle and no node id — so choosing a
window returned false BEFORE the first log line ("select a window, press
share, nothing happens"). Its twin in `SfuMediaEngine::publishVideo` had been
taught about windows in the same commit that added them; this one had not.
GENERALISE: when a new way of naming a source is added, every refusal that
reads the OLD name has to learn it, and they are not next to each other.

**THE CALL UI: a run of layout defects, every one of them worst where the
logical viewport is smallest** — which is why they surfaced on Windows at 125-150% scaling and not
on Linux. The one that produced "buttons sitting on buttons": `CallHeaderBar`
declared no `implicitWidth`, so the two placements that read its implicit size
laid the compact control row out at WIDTH 0 and put the collapse button
immediately after a zero-width point, while the row itself went on drawing a
hundred pixels each side of it; `implicitHeight` was already doing that job on
the other axis. Also: the spotlight strip's fixed 96 px height made it the
bigger half of a short stage (a 335 px panel spent 96 on the strip and left
the shared screen 61, so a 4K desktop arrived as a postage stamp); nameplate
rows that never elided because the row inside the pill used `anchors.centerIn`
and so took its full implicit width; `RowLayout` children squeezed to a smear
by a wrapping paragraph reporting its whole unwrapped sentence; a card with a
hardcoded width; a speaking ring larger than the cell that clipped it; and a
per-participant volume control that was a bare transparent icon painted on
video, now the same filled ground every other call control uses.

**AND THE PANEL'S FLOOR ASKS THE STAGE, not a fraction.** The divider's clamp
took 45% of the pane whatever the call was doing, and a call panel spends a
header row, a dock and its own margins before any picture starts — so on a
short window 45% bought the chrome and about ten pixels of video. It now asks
the stage for its own `minimumUsefulHeight` (the stage is where those bands
are declared; a second copy of the arithmetic would drift the first time one
changed), capped at 60% of the pane so a very short window keeps its timeline,
and a voice-only panel is untouched at 45% because it has no picture to
protect. Measured: dragging the divider to nothing with a share running now
leaves 136 px of picture where it left none. The spotlight's overlay controls
(Full screen, Back to grid) are anchored in a CLIPPED rectangle at a fixed
height, so once the tile was shorter than they are they drew across its top
edge cut in half; they are ABSENT below that height rather than squeezed,
which costs no route out — the header's "Show screen share" and the share's
own grid tile both lead back.

### What is NOT the same as Linux

The xdg portal owns Linux's picker and offers monitors AND windows. Windows and
macOS have no broker, so **Lightning draws its own picker** there
(`qml/ScreenSharePicker.qml` over `SfuCallController::screenShareSources`) —
a tabbed grid of live previews, Applications and Screens, on Discord's shape.
What each platform can actually put in it differs, and the difference is the
platform's, not a design choice:

* **Windows** — every display, plus every ordinary top-level window, captured
  by Lightning's own `lightningwindowcapturesrc` (below).
* **macOS** — displays only. There is no window enumeration at all, so the
  Applications tab is permanently EMPTY, and the picker says which of the two
  facts it is: "Lightning cannot list windows on this platform" is not the
  same sentence as "you have no windows open", and the second one is simply
  false in front of a Mac user with three apps running. Share is gated on the
  selection being IN THE VISIBLE TAB for the same reason — an empty tab leaves
  the selection pointing into the other one, and a bounds check alone let a
  Mac user press Share on an empty Applications grid and put their whole
  display out. (Caught in review, never shipped.)

Three rules the picker's rows obey, each one a defect that was reported:

* **Screens report PHYSICAL pixels.** `QScreen::geometry()` is
  device-independent, so a 4K display at 125% scaling was offered as
  "3072 x 1728" — a number that appears nowhere else on the machine, while the
  capture element grabs the real framebuffer.
* **The display index is resolved by DEVICE NAME** (`\\.\DISPLAY1`), never by
  position. Qt's screen order, the order `EnumDisplayMonitors` walks and
  whatever `gdiscreencapsrc`'s `monitor` property counts are three unrelated
  enumerations, so a positional index can name one display, preview a second
  and capture a third.
* **A window carries its owning APPLICATION**, read from the executable's
  VERSIONINFO, ahead of its caption. A Chromium window's caption is the TAB's
  title and names no browser at all.

The previews come from the same `PrintWindow` path the capture uses, so a tile
that comes up blank is telling the truth about what would be broadcast rather
than flattering it. Nothing is cached and nothing touches disk: a still of the
user's screen must not outlive the dialog that asked for it.

What is actually left:

1. **A CAMERA source picker that reaches the wire.** The screen picker is done
   on all three platforms (the portal's on Linux, Lightning's on Windows and
   macOS). The camera is not: `CallDeviceController` lists cameras and
   `qml/CallDeviceSettings.qml` offers them, but the SFU publish path
   instantiates `SfuMediaEngine::cameraSource()` — a bare `v4l2src` /
   `ksvideosrc` / `avfvideosrc` with no `device=` — and only the AUDIO
   selection is pushed into the engine (`setAudioDevices`). So on a machine
   with two cameras the choice is displayed and ignored. A node id resolved
   from a `GstDeviceMonitor` on `Video/Source` is the shape that answers it.
2. **Emoji reactions on the wire.** The RAISED HAND now interoperates (see
   "Raised hands" below); the transient emoji reactions element-call sends
   beside it — `io.element.call.reaction` with a `m.reference` to the
   sender's membership — do not, and there is no control for them.
3. **The screen-share startup hold**, on a DAMAGE-DRIVEN capture only.
   Live-confirmed 2026-08-26 as near-instant on a first share and 1-2 s on a
   restart — much better than the 5-10 s originally reported — and measured on
   that same desktop as `rateStageHoldMs=77` for one share. Still not fixed:
   `videorate` emits nothing until a SECOND input buffer arrives and a desktop
   capture delivers ON DAMAGE, so the wait is "how long until something on
   screen changes". THREE properties have been shipped against this without
   measurement and all three made it worse: `min-buffers=8` and
   `keepalive-time=100` each killed the capture, and `compositor` as the rate
   stage cropped a 4K desktop to its top-left quarter. The open lead, measured
   but NOT shipped: putting the SIZE ceiling BEFORE the rate stage makes
   `compositor` usable without the crop (`sink 1920 / src 1920` against a 4K
   input, where caps-after gave `sink 3840 / src 1920`). The unmeasured half is
   whether it keeps the instant first frame, and that has to go through
   `framesFromASingleCaptureBuffer` before anything ships.
   It does NOT apply to Lightning's window capture, which runs on its own
   clock and therefore always has a second buffer coming — a perfectly still
   window still streams.
4. Live interoperability with Element, which is the only thing that turns any
   of the above from "implemented" into "works". Confirmed so far: audio,
   camera and screen share in both directions on Linux (2026-08-24/25), screen
   share stop and restart (2026-08-26), raised hands (2026-08-26), and the
   camera plus screen share from a packaged **Windows** build (2026-08-27).
   NOT TESTED: anything at all from the macOS package; every failure branch of
   the above — a refused publish, a renegotiation Element rejects, a
   reconnect; and, precisely, the single-WINDOW branch on Windows. The
   2026-08-27 confirmation names "the camera" and "screen share"; whether the
   run that produced it shared a WINDOW or a MONITOR is not recorded, and the
   two negotiate differently by construction (a monitor source reports FIXED
   caps and a window source does not — that difference is the whole of the
   garbling defect). Treat the window branch as unconfirmed until someone
   says otherwise.
