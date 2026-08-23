# MatrixRTC (modern Matrix calling) — phase 1

Status: **observation and discovery are live; joining is not.** This document
records the exact wire Lightning speaks, what a homeserver has to provide,
what is deliberately absent, and what has and has not been validated.

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
