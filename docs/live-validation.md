# Live validation: what Rokas has actually confirmed

**MOVED OUT OF `CLAUDE.md` §16 on 2026-09-15**, at 139,949 characters against
that file's 150,000 hard limit — the fifth move, and for the reason all five
happened: past roughly 140,000 the file's own TAIL heads for a cliff where it
truncates SILENTLY and §§17-19 vanish from agent context. §16 says outright
that it is a LESSON INDEX and not an inventory; a chronological record of what
has been confirmed live is an inventory, so it was the section to go.

This is the record of what has actually been exercised against real
homeservers, real hardware and real peers — newest first. Nothing in it is
inferred from a passing build or a green pipeline. Read it before promoting
anything to tested, and read `open-items.md` beside it for what has NOT been.

### Live validation: what Rokas has actually confirmed

**2026-09-15 (evening) — THE WINDOWS CAMERA DELIVERS ~29.8 fps SUSTAINED, AND
THE SELF-VIEW SHOWS IT: PASS.** On the laptop's Windows guest, released 0.9.5
portable, with the laptop's physical privacy shutter OPEN — which is the whole
point of this entry, because the same round measured a rock-steady 10.00 fps a
few hours earlier with it closed and those numbers meant nothing. Windows
itself had said so: *"Your camera is reporting that it is blocked or turned off
by a switch."*

Three runs from the `capture delivered frames count=` pad probe on `capsrc` —
what the DEVICE emits, not a negotiated caps string: **29.797 / 29.812 / 29.809
fps** over 285 s, 218 s and 151 s, flat in every 500-frame bucket
(16.76-16.79 s each). `camera chain= mjpg`, `image/jpeg 1920x1080 30/1`,
`firstCaptureMs= 514-535`. The negotiated and delivered rates AGREE, which is
the thing that had never been true before. Against 0.9.4's raw chain on the
same guest and sensor (`YUY2 1920x1080 framerate=5/1`), the MJPG work is doing
exactly what it was built to do.

The self-view tile shows the live camera image, not the crossed-camera
placeholder the previous round saw — that observation was the shuttered sensor
and is WITHDRAWN. No `SfuMediaEngine.cpp` change is indicated.

**What this does NOT cover:** it is a QEMU `usb-host` passthrough, not bare
metal, so it does not measure the host's USB 2.0 bus, and the raw-YUY2
bandwidth ceiling the original 10 fps theory named still needs physical
hardware. It is 0.9.5, so the camera-preference fix on `main` is NOT TESTED
here. And a second peer receiving those frames was not part of it.

**2026-09-15 — OFFLINE RESTORE, LIVE: PASS, and the room list, an encrypted
DM's decrypted history and the local search index all came off the disk.**
Automation-driven on the maintainer's desktop with two throwaway accounts, not
Rokas. With `HTTPS_PROXY` pointed at a closed port so every outbound request is
refused at once, the client goes Boot -> Main and **not to the login page**,
logs `session restored from the local store`, shows the complete cached room
list, opens an ENCRYPTED DM and renders its history, and reads
**"Offline — retrying"** from the first frame (the `setState` override; before
it the same session said "Loading rooms…" over a list that was already
complete). The find bar's History scope answered `Searching 12 messages
Lightning has indexed, including encrypted ones.` with three hits, with no
server at all — which was the specific ask. In the same session the three
message layouts were audited against a real account and four collision defects
found and fixed; see `docs/round-history.md`, 2026-09-15. NOT covered: a real
homeserver outage (this is a refused proxy), and the first offline start of an
account that has never signed in on this build, which still fails by design.

**2026-09-13 (night) — A SHARED WINDOW THAT NEVER REPAINTS NOW PUBLISHES:
PASS, and it published NOTHING before.** Automation-driven on the laptop, same
static window and same peer either side of the fix. A PipeWire screencast
delivers ON DAMAGE and `videorate` emits nothing until a SECOND buffer
arrives, so a window that does not repaint gave `capture delivered frames
count= 1`, no `publish first encoded frame` line at all, and a far end stuck on
"Waiting for the picture" indefinitely. §16's `videorate` block described this
as a WAIT ("~1 s to 10 s"); for a still window it is unbounded. Fixed in
`5abcc81` by handing videorate the second buffer it is waiting for — measured
after: `keep-alive … quietMs= 506` then `afterPublishMs= 595`, and the far end
RENDERS the window. A moving share is unchanged (141 ms, keep-alive fires
zero times). A GAP event joins the do-not-retry list (0 buffers out), and the
injected PTS must come from the SAMPLED BUFFER, never the pipeline clock —
`LightningWindowCaptureSrc` and `ximagesrc` are zero-based, and the
running-time version was measured in review at 2612 buffers from one
injection and a permanently dead share. Full account in
`docs/round-history.md`, 2026-09-12 (night).

**2026-09-13 (evening) — THE SNAP COULD NEVER CARRY CALL MEDIA, AND THE CAUSE
IS NSS. Sixth occurrence of "a library loads its own plugins", and the first to
reach a shipped lane.** Debian builds **`libsrtp2` against NSS, not OpenSSL**.
`ldd` names libnss3/libnspr4/libnssutil3/libplc4/libplds4, the ELF walk bundles
all five, every payload check passes — and NSS does no crypto itself: it
**dlopens `libsoftokn3.so`**, which dlopens `libfreebl3.so`, from a path
derived at RUNTIME. Nothing staged them. Unconfined the host's NSS is found and
this is invisible; under strict confinement `/usr` is core24's, which has NO
NSS, so libsrtp returns `init_fail` (err 5), `srtpenc` posts "Could not
initialize SRTP encoder", the publisher dies and the subscriber never gets a
receive pad. **The AppImage carries the identical gap** and is one NSS-less
host away from the same failure. Fixed by staging the four NSS modules beside
`libnss3.so` in `build-appimage.sh`, asserted BY NAME in both validators.
**LIVE-VALIDATED PASS on the artefact, 2026-09-13**: pipeline 214's snap,
installed over the signed-in revision, produced `received track attributed=
true`, `a receive chain is RUNNING`, and 3000 frames in the clear BOTH ways,
with ZERO srtp errors and zero pipeline errors. `received track` had never
appeared on this lane once. The snap carries call media for the first time.

FOUR HYPOTHESES WERE KILLED FIRST and must not be re-proposed: a missing
GStreamer plugin (29 staged, env points at them); the publisher's bus error
tearing the call down (`handleBusMessage` deliberately never calls `failed()`);
a Matrix-level failure (membership, media key, SDP and ICE all verified good);
and OpenSSL provider loading (`OPENSSL_CONF=/dev/null` changed nothing). What
settled it was `GST_DEBUG=dtls*:6,srtpenc:5` INSIDE the confinement: DTLS
COMPLETES and hands srtpenc a correct 30-byte aes-128-icm key, and libsrtp
fails to initialise with it.

**AND DO NOT PUSH TO `main` WHILE A PIPELINE IS RUNNING.** `resolve-source`
pins the SHA; every later job re-clones `main` and refuses a different commit
(`error: source ref moved`). Pipeline 211 lost two jobs to exactly that. That
is the guard working — trigger, then hold.

**2026-09-13 — THE FIVE-MINUTE MATRIXRTC MEMBERSHIP EXPIRY IS CLOSED: PASS,
and it is the headline of a full packaged-flatpak GUI sweep.** A two-party call
between the packaged flatpak and an AppImage was held **nineteen and a half
minutes**, ~4x `MEMBERSHIP_EXPIRY_NO_DELAYED_MS`, and at the end both clients
still read `session read room participants= 2` with both `frames in the clear
in` counters climbing and no `frames dropped: no key in` anywhere. That is
`expires_for_refresh()` working: before it, the membership died a fixed five
minutes after the JOIN however often it was refreshed, lopsidedly (still heard,
hearing nobody). Automation-driven, not Rokas.

Everything else PASS in the same sweep, all on the confined flatpak: screen
share through the portal — `remote_fd= true` on the sharer, and the RECEIVING
client's stage tile drew the sharer's desktop, captured from the AppImage's own
window on the default RHI backend, NOT the sharer's self-view and not
`QT_QUICK_BACKEND=software` (the distinction §16's WITHDRAWN 1 exists for);
share audio; **call audio FRAMES both directions** (clear-frame counters, in
and out — audibility NOT TESTED, nobody listened); **group power control** (Member -> Moderator
-> Member, real `m.room.power_levels`); threads (panel, reply, summary card,
and §8 held); the Ctrl+Shift+K command palette EXECUTING an action; a real
freedesktop notification with Reply/Mark as read; the updater (installation
type "Flatpak", check reaches the server); local message search INCLUDING an
encrypted room, whose index sits inside the flatpak's own data dir; token AND crypto-store persistence
across a restart; and §6's rule live — relaunched with no session bus, the
unreadable secret store did NOT read as a missing account.

Both of the desktop notification's ACTIONS were pressed, not merely shown:
**Mark as read** moved the window caption from `(1 unread)` to clean and sent
two real read receipts, and the toast's inline **Reply** put a message into an
ENCRYPTED room, decrypted on the peer, from a client whose window was never
focused. And the call stage was checked against a known state: two tiles with
the right names, and a crossed-microphone badge that appeared on the PEER's
tile and not the local one when the peer pressed Ctrl+Shift+U.

FOUR defects found and fixed (Activity Center rows baking raw ids in for the
session plus its silent reconcile consequence; Updates contradicting itself;
the Space Home row running off a narrow pane; an edited thread root pushing
its summary card off the bubble), plus a fifth that could NOT be fixed here —
the snap is not signed in — **one claim WITHDRAWN before it was acted on** (shortcuts are NOT dead under a menu — Lightning's menus are
in-scene popups, not `xdg_popup`s), and two non-defects recorded so nobody
"fixes" them (Lithuanian date dividers are `LC_TIME`; the three-hour timestamp
gap is the container's missing TZ). **The SNAP could not be swept — it is not
signed in**, and so is recovery/key-backup setup, which would put a generated
recovery key in a screenshot. Detail in `docs/round-history.md`, 2026-09-13
(afternoon).


**2026-09-12 (evening) — THE WINDOWS CAMERA'S 10-FPS CEILING IS CLOSED: PASS.**
On the laptop's Windows guest with its USB webcam passed through, the portable
build that carries `libgstjpeg.dll` negotiates `image/jpeg 1920x1080 @ 30/1`
where the released 0.9.4 negotiated `video/x-raw YUY2 1920x1080 @ 5/1` —
`camera chain= mjpg (jpeg elements present)`, 500 frames delivered and
climbing, a real picture on screen, and `firstCaptureMs= 424` against the
794-811 ms the raw path cost. Same guest, same camera; the only variable is
that the shipped package finally contains the plugin. The packaging half of
that is builder image v6 plus the plugin being REQUIRED again and `jpegdec`
being probed against the extracted package under Wine.


**2026-09-12 (afternoon) — THE WINDOWS "NO VIDEO" DEFECT IS CLOSED, on a
PICTURE: PASS.** Automation-driven on the laptop's Windows 11 guest, not
Rokas. With the GL probe choosing `Direct3D11` the guest RENDERS a remote
screen share — the receiving tile carries the sending machine's live desktop —
where the same guest, same share and same clear-frame counters previously
advanced past 1000 against an empty rectangle. The renderer was the only
variable, and the defect was our own probe, never packaging. In the same
session: the WINDOWS TRAY-BALLOON notification displays and its CLICK routes to
the right room, both live for the first time. And FOUR convincing "defects" on
that guest — no call banner, no Join, no room-list glyph, a `?` facepile —
were its CLOCK, seven hours ahead in UTC, which made every `m.call.member`
expiry read as past. Check `date -u` on the host against a guest log's own `Z`
stamps before believing anything about call state on a VM. Detail in
`docs/round-history.md` and `docs/open-items.md`.


**2026-09-11 (evening) — CALLS AND PER-PARTICIPANT VOLUME, LIVE ON TWO
CLIENTS: PASS — AND TWO OF THIS ENTRY'S ORIGINAL CLAIMS WERE WITHDRAWN ON
2026-09-12.** What stands: a two-party call with clear-frame counters
advancing both ways on both clients; 0% muting; 200% amplifying. Driven by
`scripts/gui-suite-calls.sh`, which is now the tracked form of those checks —
every assertion on an engine log line or a value on disk, never on a picture.
**NOT TESTED: audibility** (nobody listened; what is proven is that the value
reaches a real GStreamer `volume` element).

WITHDRAWN 1 — "a screen share the other client RENDERS". The suite asserts
`frames in the clear in`, and `SfuMediaEngine.cpp` says in its own comment
that the crypto counters climb either way, so a tile that never attached its
sink is indistinguishable from working video. It is worse than insufficient:
MEASURED 2026-09-12 on one Linux client, one call, `QT_QUICK_BACKEND=software`
the only change, that counter climbed past 500 **against an empty rectangle**.
Qt Quick's software adaptation has no node type for video at all
(`QSGSoftwareRenderableNode::NodeType` lists rectangles, glyphs, images and
nine-patches; `QSGVideoNode` is none of them, and the path is RHI-only while
the software context has no RHI). The honest claim is: frames reach the other
client's decryptor in the clear. **That a picture was drawn is NOT asserted by
any automated check we have**, and on a host with no usable GL it is false.

WITHDRAWN 2 — "the value surviving a `systemctl --user restart`". The restart
persistence was proven ON DISK ONLY. `21f4a1a`, a later commit, records that
THE STORED VOLUME NEVER REACHED THE ENGINE — "the slider read 200% and nothing
had reached the audio graph" — so the restarted client could not have applied
it at the time of that run. Fixed in `21f4a1a`; **NOT TESTED since**. The round also fixed a bell that
HID REAL UNREADS and carries the generalised lesson — a consumer of derived
state must listen to the writer of that state, not to the signal named after
the same noun. Detail in `docs/round-history.md`, 2026-09-11 (evening).

**2026-09-11 — the send path, live, with `scripts/test-netproxy.py` instead of
root.** reqwest honours `HTTPS_PROXY`, so one app's network becomes
controllable: throttle it and an upload is samplable, cut it and that app
alone goes offline. PASSES: the upload percentage advances on a determinate
bar; `cancel_too_late` says the message had already been sent; and Retry
recovers a room whose send queue matrix-sdk had disabled. STILL OPEN: a local
echo can sit at "sending…" after the server has the event, resolving only on a
timeline reload — cause NOT established, and NOT reproducible on demand (two
cut/restore cycles on 2026-09-11 evening did not trigger it). **Run the
capture with `LIGHTNING_SEND_TRACE=1`** as well as `LIGHTNING_RUST_LOG=1`: it
reconciles the timeline's in-flight items against `SendQueue::local_echoes()`
and prints `queued=0 … ORPHANED` exactly when a terminal update was lost,
which is the one thing that separates this from a slow link. Read
`docs/round-history.md`, 2026-09-11, before theorising — it records which
conclusions the evidence does and does not support.

**2026-09-11 — THE THREAD ROOT CARD FOLLOWS AN IN-PLACE CHANGE: PASS.**
Editing a thread ROOT from the room timeline with the panel open updates the
card without a reopen. It renders the root from a SNAPSHOT refreshed only on a
lifecycle change and on countChanged, and a late decryption, an edit, a
redaction and a sender-name resolution all arrive as in-place Sets that change
no row count — so an undecryptable root stayed undecryptable on screen above
replies that had decrypted fine. Driven on a LAPTOP over SSH, which is how GUI
tests run now without touching the maintainer's desktop.

**2026-09-11 — THE THREAD EDIT IS LIVE-VALIDATED: PASS.** The fourth and last
of the "address the event on the timeline that HOLDS it" family, driven
through a real thread panel against a real homeserver: the edit applies, the
row carries the `edited` marker and the room's summary card follows — and the
new body survives a restart, so it is the server's copy and not an echo.
Found in the same session: the panel's "N replies" divider read the ROW
count, so date dividers inflated it — it said 3 beside two replies where the
room card correctly said 2. The shipped fix is
`ThreadController::replyCount` and it is **LIVE-VALIDATED PASS** on the real
backend: three replies read "3 replies", and a fourth sent with the panel
open moved it to "4 replies" without a reopen. Getting there caught one more
defect the same way — preferring the SDK's `num_replies` outright showed "2
replies" above THREE visible ones and had not corrected itself 45 s later,
because a thread summary is stale LOW as readily as high. The loaded replies
are a floor now; the SDK's number covers only what lies beyond the window. Two harness facts: the message context menu
survives a `shot_pid` capture (the no-capture-mid-menu rule is about
spectacle's interactive mode) and publishes its own shortcuts, `T` and `E`.
`ydotool key` needs KEYCODES — `28:1 28:0` for Return; a key NAME types
nothing and reports success.

**2026-09-10 — the first GUI validation of anything above 0.9.4, and it was
AUTOMATION-driven on a throwaway fixture account, not Rokas.** Four PASSes,
detailed in `docs/round-history.md` under 2026-09-10 (night): the room mirror
is retired only for the room you LEFT (`rows= 107 -> 60`); local search was
driven from the GUI for the FIRST TIME (it is the find bar's History scope,
Ctrl+F) and its "load more" now walks to the last row instead of spinning; the
Appearance theme cards render with the ring on the active theme; and 70 rapid
sends all landed, drained over ~3 minutes by Synapse's `rc_message` limit
rather than by any defect. Two-account behaviour, thread-panel identities, the
sticker grid and leaving a Space remain NOT TESTED — do not promote them.

**2026-08-30 — THE GPU SCREEN-SHARE SCALE PATH WORKS ON FOUR ENVIRONMENTS,
AND TWO GPU VENDORS.** `screen share scaling on the GPU` confirmed on: NixOS
from source (NVIDIA), a packaged Windows portable build (NVIDIA), the Fedora
RPM (Intel), and the Flatpak (Intel) — the last two on a laptop, with share
audio and a real two-participant call carrying a distributed media key. It is
now the DEFAULT everywhere rather than opt-in.

The Windows numbers are the ones that justify it: a game held 225 of 240 fps
while sharing, and the capture fed the encoder 1:1 (1000 delivered, 1000
encrypted) where the CPU path at 60 fps delivered ~500 against ~1500 — i.e.
`videorate` tripling every real frame, so two thirds of the encode and
encryption was the same picture.

`gstreamer initialised bundled= false` is CORRECT for the RPM and the Flatpak
(system and runtime GStreamer respectively); only the AppImage bundles.

**AND THE APPIMAGE SHIPPED WITHOUT THE PLUGIN, which nothing could have
caught.** The 0.8.2 AppImage logged `screen share falling back to the CPU:
GStreamer element "glupload" is not available in this build` — `libgstopengl`
was simply not in `GST_REQUIRED_PLUGINS`. The engine probes for the element
and degrades, so a missing plugin can never fail a build or a call: GRACEFUL
FALLBACK AND SILENT ABSENCE ARE THE SAME OBSERVABLE unless something asserts
the payload. Third time this shape has bitten — sctp on Windows, ximagesrc,
now opengl — and `validate-appimage.sh` now names it, as it already named
those two. The same gap existed on Windows in a SECOND list:
packaging/windows/Dockerfile stages into the builder SYSROOT,
stage-windows-runtime.py stages into the shipped ZIP, and updating one is not
updating the other.

NOT COVERED: whether the GPU path survives on a machine with no usable GL —
the new `gpuShareChainUsable()` pre-flight is written for that case and has
never been observed declining. macOS is untested entirely.

**2026-08-29 — SCREEN SHARE AUDIO REACHES ELEMENT, on Linux.** Confirmed on
a real desktop into an ENCRYPTED room: Element hears what the sharing
computer is playing. First time share audio has ever left this client.

The log carries the whole path — `share audio published`, then
`negotiation needed: offering 2 track(s)` and an answer at **3 sections**
(the SFU accepting the added track), then TWO independent
`frames encrypted video=false` counters running side by side, which is the
microphone and the share audio as two separately encrypted Opus tracks.

NOT covered by that confirmation: Windows (the capture element differs and
has only been verified to EXIST and to carry a `loopback` Boolean, by
running the shipped SDK's own gst-inspect under Wine — Wine answers
metadata questions, not whether WASAPI captures); the RECEIVE direction,
i.e. whether Lightning plays share audio someone else sends; and whether a
receiver handles two audio tracks from one participant.

Seen in the same capture and NOT diagnosed: `frames dropped: no key in
video=false` climbing on the receive side, with `sfu joined others=2`
against `media key distributed targets=1`. The leading explanation is a
GHOST MEMBERSHIP from the evening's repeated Ctrl+C exits — the log says
`no MSC4140 delayed retraction armed — an unclean exit will leave this
membership until it expires` — which is the mechanism recorded under
"media key targets=0". Not established, and not attributable to the share
audio change either way.

**2026-08-27 — THE WINDOWS CAMERA AND THE WINDOWS SCREEN SHARE BOTH WORK.**
Confirmed on a real packaged Windows build (project 7 pipeline 135, a
NON-PUBLISHING snapshot from `9f829a3`): the camera sends live video instead
of one frozen frame, and screen sharing works. Earlier the same day, from the
same tester on the pipeline 134 artifact: selecting a MONITOR works with two
monitors attached, and a window share of File Explorer is correct.

CONFIRMED LATER THE SAME DAY, on the pipeline 136 build: the camera works,
and a WINDOW share is correct — right aspect ratio, and resizing the window
mid-share does not break it. So the three headline defects of this lane are
all closed on Windows.

WHAT IT STILL DOES NOT COVER: closing a shared window while sharing, the
picker's grid rework, the call-UI layout fixes, and anything at all on macOS.
Do not promote those to tested.

STILL WRONG at the time of that confirmation, reported with a screenshot and
fixed afterwards in `008ccfd` (so ITSELF not yet re-validated): with a share
running, dragging the call panel small collapsed the picture to a sliver and
the spotlight's overlay controls drew across its top edge in half.

**2026-08-26 (later still) — RAISED HANDS INTEROPERATE WITH ELEMENT.**
Confirmed working on a real desktop: a hand raised in Lightning is seen in
Element and vice versa. The wire format was read out of element-call's own
source rather than guessed at (§16), and it was right first time — which is
the whole argument for reading the reference implementation.

Two things from the same session log, neither of them the feature under test:

* **The screen-share startup hold is 77 ms**, measured rather than reported:
  `publish first encoded frame screenShare=true afterPublishMs=135
  firstCaptureMs=58 rateStageHoldMs=77`. The open defect below describes
  "5-10 s previously, 1-2 s on a restart" — this is the first NUMBER anyone
  has had for it, and it says the `videorate` hold is no longer the cost on
  this machine. It is ONE capture on ONE desktop and the cause is unchanged;
  it is evidence about that share, not a fix.
* **Switching accounts mid-call stranded the membership** (`retraction could
  not be dispatched`), because the teardown ran after the Rust client was
  released. Fixed the same day; live re-validation NOT TESTED.

**2026-08-26 (later) — screen share STOP and RESTART, against Element.**
Confirmed working on a real desktop: stopping a share genuinely stops it —
the far end's tile clears instead of freezing on a grey box — and starting a
new share afterwards works, replacing rather than landing beside the old one.
First share near-instant, restart 1-2 s. Microphone loudness (the `webrtcdsp`
AGC) and the per-participant volume curve were confirmed in the same session.

This lane took FOUR rounds and the first three each fixed something real
without fixing the report, which is the lesson worth keeping:

1. `unpublish()` deadlocked the GUI thread against its own streaming thread
   (core dump: `gst_pad_set_active` wanting the stream lock, the encoder
   thread holding it in `do_probe_callbacks`). Fixed with an IDLE pad probe
   plus `gst_element_call_async`.
2. That probe then never fired, because a pad pushing into a webrtcbin that
   is not draining never becomes idle — so the teardown did not deadlock, it
   simply never ran. A leak wearing a fix's clothes. Instrumented: "probe
   installed", silence for three seconds, "probe fired" during teardown.
3. Releasing the request pad dropped the msid and left the section
   `a=sendrecv` — the far end still told it was being sent to, with nothing
   behind it. THAT was the grey box.
4. Setting the transceiver direction to INACTIVE is what the far end obeys.
   Section count must stay stable across renegotiation (an m= section may
   never be removed), so `a=inactive` is the only correct shape, not merely
   the tidy one.

Checked rather than assumed at step 4: livekit-protocol 0.7.12 has NO
unpublish verb for media tracks — SignalRequest carries AddTrack, Mute and an
UnpublishDataTrackRequest for DATA only. Renegotiation is the mechanism, which
is why a MUTE could never do the job: a mute removes nothing, so the stopped
track stayed in the participant list and was rendered forever.

GENERALISE: Lightning's own self-view is tee'd off the CAPTURE, upstream of
encryption and of the SFU entirely. It looking correct says nothing whatever
about what any other client receives, and it looked correct through all four
rounds. When a share is reported broken remotely and fine locally, the
preview is not evidence.

**2026-08-26 — the largest live-validation event this project has had.** Rokas
tested and confirmed WORKING, on a real desktop against real homeservers:

1. **The rail's drag, including drop-to-make-a-folder.** This is the headline:
   the gesture was structurally unreachable through THREE rules and two rounds
   that each believed they had fixed it. It works. Also confirmed: the
   folder-name dialog, the auto-scroll near the rail's ends, and a drop
   BETWEEN tiles reordering rather than grouping.
2. **Space settings and the rail's Space menu — every write.** Name, topic,
   avatar, join rule, canonical alias, the full power-level matrix, Publish to
   Directory, Local Addresses, Mark as read, Mute, Invite, Copy/Share link.
   Every one of those is a real state event and none had ever been sent.
3. **Keyboard shortcuts**, including the design's load-bearing case: Ctrl+B is
   Bold inside the message box and still toggles the room list everywhere
   else, rebinding, and the conflict refusal.
4. **Multi-account against real homeservers** — switching, encrypted-room
   decryption after a switch, notification routing, restart restoration,
   scoped removal. (The switch's 3-5 s FREEZE is a separate open defect below;
   the behaviour is correct, the latency is not.)
5. **Element interoperability** per `docs/element-interop-checklist.md`:
   encrypted both directions, threads, voice messages, video posters,
   reactions, pins, edits, redactions, the key-recovery cycle, and QR and SAS
   verification against Element.
6. **Notifications** — thread replies, server push-rule modes including
   "follow account default", and the retry after reconnect.
7. **The Channels column, the 2026-08-21 UI round across all 11 themes,** and
   the smaller items: hide/show an image without moving the timeline, read
   receipts, presence dots, saving GIFs, the compact link-preview consent box,
   reduced motion, the 24-hour clock, attachment captions.

WHAT THAT CONFIRMATION IS AND IS NOT. It is Rokas exercising each feature and
reporting it works — the only evidence that has ever counted here. It is not a
claim that every FAILURE branch was reached: a write that the server REFUSES,
a power level a homeserver rejects, and a reconnect retry all need a server
that says no, and those paths remain unexercised. Do not re-list the seven
areas above as untested; do not upgrade their failure branches to tested
either.
