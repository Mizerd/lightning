# Open items and the NOT TESTED inventory

Moved out of `CLAUDE.md` §16 on 2026-09-11: that file had reached 140,752
characters against its own 140,000 rewrite threshold (and a hard 150,000 limit
past which it truncates SILENTLY, dropping its own tail — §§17-19 — from agent
context). This is the FOURTH such move: §7 to `docs/feature-contracts.md` on
2026-08-28, §16's round history to `docs/round-history.md` on 2026-09-03, §2's
release inventory to `docs/release-operations.md` on 2026-09-10, and this.
Nothing was deleted; the whole block is below unchanged.

This one was chosen because §16 says outright that it is "a LESSON INDEX, not
an inventory" — and an inventory is exactly what this block is. The lessons,
refutations and standing warnings stay in `CLAUDE.md`; the list of what is
open and what has never been tested lives here.

**READ THIS BEFORE claiming anything is fixed, before promoting anything to
tested, and before proposing a fix in an area it covers.** An item here that
says NOT TESTED has not been tested, however plausible the code reads.

### Open items and NOT TESTED inventory

OPEN DEFECTS, reported live and not yet confirmed fixed. These are the list.

- ~~**The camera does not work at all**~~ — **FIXED and LIVE-CONFIRMED on
  Windows 2026-08-27** (`31e6048`), as is the window share's aspect ratio and
  resizing a shared window mid-share. It was never the camera, and "screen share
  works on the same publish path" was the clue rather than the puzzle: the
  window share worked because Lightning's OWN capture element stamps PTS from
  ZERO, and every other source stamps the pipeline's RUNNING TIME.
  `videorate` starts its output clock at SEGMENT START, so a camera switched
  on three minutes into a call handed it a first PTS of three minutes and it
  owed thirty duplicate frames for every second of that — emitted as fast as
  the encoder would take them. ONE picture at full rate, every counter
  healthy. See `videorate skip-to-first` below and §16's rate-stage note.
  **The 10 fps ceiling is a SEPARATE and still-open item**: `ksvideosrc`
  negotiated YUY2 1280x720@10 because `libgstjpeg.dll` is not staged, so an
  MJPG mode cannot negotiate and raw YUY2 at 720p saturates USB 2.0. Fixing
  the freeze did not raise the rate. The control lag is also unclosed
  (`firstCaptureMs` 794-811 ms, the KS device open running on the GUI thread
  inside `gst_element_sync_state_with_parent`); it needs a
  `LIGHTNING_GUI_STALL_TRACE` capture, not a theory.
- **An account switch FREEZES the UI for 3-5 seconds** — **the WAIT IS NOW OFF
  THE GUI THREAD (2026-09-04, Task A §2), and the defect is not yet closed.**
  `releaseRustHandle()` used to run `mx_rust_shutdown_tasks` and
  `mx_rust_destroy` on the thread that draws the window; the second drops the
  tokio runtime and therefore blocks until every in-flight spawn_blocking
  finishes, SQLite closes included. Both now run on a two-thread retirement
  pool that takes ownership of the handle. Safe because **Rust never calls
  back into C++** — events are PULLED by `pollRustEvents()` on a 100 ms timer,
  so a retiring client has no route into a QObject that may have been
  destroyed. Three callers still wait, deliberately, because they delete or
  rename the store and must not race an open SQLite connection: resetRustStore,
  resetLocalSession and AppController's account removal, plus the destructor at
  process exit (`waitForRustRetirement`). Found in the same sweep and also
  fixed: `VideoPosterExtractor`'s destructor joined its decoder thread on every
  switch, bounded by the measured **931 ms** Qt Multimedia backend
  initialisation.
  LIVE-VALIDATED: three A->B->A switches on real accounts with
  `LIGHTNING_GUI_STALL_TRACE=1` recorded **no GUI stall for any switch** (the
  only two in the session are at startup), each logging "rust client retired
  off the GUI thread" at 1-4 ms. WHY IT IS STILL OPEN: both fixture accounts
  are tiny — twelve rooms, no avatars — so this proves the wait is no longer on
  the GUI thread, NOT that a large account is fast. The remaining question is
  whether anything ELSE scales with account size; that still needs a capture on
  a real account. Regression guard: `rust-retirement`, whose first version was
  worthless (a 250 ms budget that an empty store passes synchronously) and
  which now requires the work to be OUTSTANDING when the caller returns.
- ~~**An account switch FREEZES the UI**~~ — original report, kept for the
  numbers: reproducibly, on the
  SECOND switch (A→B→A) and not the first. Distinct from the unbounded
  profile-fetch loop (`be195f7`) and from the double-polled JoinHandle
  (`e50eff6`); this is a synchronous block, and `shutdown_managed_tasks`
  does `block_on` on the GUI thread with a 15 s budget over a pool that
  includes ~170 avatar fetches.
  **A 2026-08-26 log makes the leading suspect look WRONG.** The same A→B→A
  round trip reported `teardown_total_ms= 23` on the first switch and
  `teardown_total_ms= 308` on the second — so the second switch IS ~13x
  slower, and it is 0.3 s, not 3-5. Whatever costs seconds is somewhere else,
  and the next round should stop reading `shutdown_managed_tasks` and get a
  `LIGHTNING_GUI_STALL_TRACE` capture across a switch instead. One capture
  beats another theory; that is the standing rule in this file and it applies
  to the theory this file itself wrote down.
  **2026-09-04: THAT CAPTURE EXISTS NOW, AND IT DID NOT REPRODUCE.** A real
  A→B→A round trip on `matrix.smetonis.net`, driven through the GUI with
  `LIGHTNING_GUI_STALL_TRACE=1`:

      switch 1  begin 16:56:20.674 -> login succeeded 16:56:20.917   243 ms
      switch 2  begin 16:57:10.656 -> login succeeded 16:57:10.914   258 ms
      teardown_total_ms = 3 and 1

  NO GUI stall was recorded during either switch. The only stalls in the whole
  session were at startup (271 ms unattributed, 1730 ms rust-poll-drain) and
  one 622 ms rust-poll-drain when the second account was first ADDED — not a
  switch. `shutdown_managed_tasks` is now definitively not the cost: 1-3 ms.
  **This does NOT close the defect.** Both test accounts are small — twelve
  rooms, two members, no avatars — and the recorded suspicion involves ~170
  avatar fetches, so the load is nowhere near the reported case. What is
  established is that the switch MACHINERY is fast and the cost scales with
  something the fixture does not have. The next capture needs a real account
  with real rooms and real avatars; it will not be found on a fixture.
- **The screen share's startup is still VARIABLE**, though far less so, and
  there is now a MEASUREMENT: one live share on 2026-08-26 reported
  `afterPublishMs=135 firstCaptureMs=58 rateStageHoldMs=77` — 77 ms of
  `videorate` hold, not seconds. That is ONE capture on ONE desktop and the
  cause is unchanged, so it bounds the problem rather than closing it; a
  desktop that is genuinely still (no damage) still has nothing to deliver.
  Previously live-confirmed as near-instant on the first share and 1-2 s on a
  restart, against the 5-10 s originally reported. The cause is unchanged and
  unfixed — `videorate` emits nothing until a SECOND input buffer arrives and
  a desktop capture delivers ON DAMAGE, so the wait is "how long until
  something on the screen changes". THREE properties have now been shipped
  against it without measurement and all three made it worse: `min-buffers=8`
  and `keepalive-time=100` each killed the capture outright (and `min-buffers=8`
  is now UNDERSTOOD — see the pipewiresrc note in this section: 8 exceeds the
  buffer ceiling a compositor offers, so on a PipeWire >= 1.6 daemon it cannot
  negotiate at all; `min-buffers=1` is required and shipped), and `compositor`
  as the rate stage cropped a 3840x2160 desktop to its top-left quarter
  (compositor is NOT a scaler — it paints each input at native size on its
  output canvas). A fourth guess is not acceptable.
  **The open lead, measured but NOT shipped:** putting the SIZE ceiling
  BEFORE the rate stage makes `compositor` usable without the crop —
  `sink 1920 / src 1920` against a 4K input, where caps-after gave
  `sink 3840 / src 1920`. What is still unmeasured is the other half, that it
  keeps the instant first frame, and that must go through the suite's own
  `framesFromASingleCaptureBuffer` harness before anything ships.
- ~~**The incoming-call prompt's Accept does nothing.**~~ — **FIXED and
  SHIPPED in 0.8.3** (`87aafd9`, `6e9bd9a`). The card had ONE Accept button
  serving two unrelated lanes: the legacy 1:1 `m.call.*` lane, answered by
  `app.calls.answer()`, and a MatrixRTC ring, which announces a SESSION and is
  answered by JOINING it (`app.groupCall.join()`, gated on
  `app.rtc.joinBlockReason()`). A MatrixRTC ring therefore showed an Accept
  that `CallController::answer()` refused at its own front door. The prompt now
  offers each lane its own control, and `qml/IncomingCallPrompt.qml` carries
  the whole reasoning at the top of the file. Live re-validation of an ANSWERED
  call is still **NOT TESTED** — the button reaches the right code now, which
  is not the same claim.
- **Windows camera runs at 10 fps**, and the fix for the freeze did not touch
  it. `ksvideohelpers.c` exposes `image/jpeg` for MJPG media types, the
  publish bin links `capsrc ! queue ! videoconvert` with no decoder, and
  `libgstjpeg.dll` is absent from the staged plugin list — so an MJPG mode
  cannot negotiate and the camera falls back to raw YUY2, which at 1280x720
  is 18.4 MB/s and hits a USB 2.0 ceiling at 10 fps. Staging the jpeg plugin
  and adding a decoder to the camera branch is the lead; it is a packaging
  change, so it must go through the shipped-artifact check (§16).
  **PACKAGING HALF DONE 2026-09-02** (lightning-deploy): `libgstjpeg.dll` is
  now staged in BOTH Windows lists — the builder sysroot in
  `packaging/windows/Dockerfile` and the shipped zip in
  `stage-windows-runtime.py` — and `jpegdec:libgstjpeg` joins the Wine element
  probe. Note `libjpeg-8.dll` had been staged all along as a LIBRARY
  dependency of Qt and libgstopengl: a DLL of the right name is not the
  element, the same distinction that shipped Windows for months with
  `libgstsctp-1.0-0` present and `sctpenc` missing.
  **THE APP HALF IS DONE — CORRECTED 2026-09-12, this paragraph claimed
  otherwise for ten days.** It is IN HEAD and has been waiting on the image:
  `cameraJpegEntry()` builds the `image/jpeg` chain,
  `jpegCameraChainAvailable()` probes whether the decoder exists and links, the
  capture FALLS BACK to the raw entry when it does not, and `camera chain=
  mjpg|raw (jpeg elements present|absent)` says which was built — so the two
  ways a camera can sit at 10 fps are finally distinguishable in a log. That
  probe is exactly what the Windows guest reported: `camera MJPG chain
  unavailable … no element "jpegenc"`.

  The probe puts `jpegenc ! ` in front of the chain under test so it actually
  carries `image/jpeg`, rather than a raw source that would link past the
  capsfilter and prove nothing — the trap below, applied in advance for once.

  **EVERY PACKAGING PRECONDITION IS NOW SATISFIED AND PROVEN IN CI
  (2026-09-12).** Builder `...-v6` is built and deployed on the runner host
  (`sha256:5c628d4b`, `jpegenc` and `jpegdec` both in `libgstjpeg.dll`), the
  plugin is REQUIRED again rather than optional, and `jpegdec` is in the
  element set the Wine probe runs against the EXTRACTED package — because
  staging the DLL is not the same claim as the element registering. Three
  green `windows-package-test` runs walk the chain: pipeline 205 on v6 at 27
  bundled plugins, 206 with the plugin required at 28, and 207 with the
  element probed at 41 elements.

  Getting there found that the builder Dockerfile had been UNBUILDABLE for ten
  days — its verify stage asserted 27 staged plugins while the install loop
  staged 28 — so the operator step recorded as open could not have succeeded
  if anyone had attempted it.

  **WHAT IS LEFT IS THE ONE THING CI CANNOT DO: a real camera.** The VM has no
  webcam passthrough, so the 10 fps claim can only be closed on a physical
  Windows machine with a USB camera. The evidence to ask for is one log line,
  `camera chain= mjpg (jpeg elements present)`, and a frame rate above 10.
  Note that line fires when a camera is actually STARTED, not at launch — the
  guest's log carries `camera portal unavailable` and nothing else, so a
  machine with no camera cannot produce it either.

  **AN IN-PLACE PORTABLE UPGRADE TO THAT BUILD WAS ATTEMPTED ON THE GUEST AND
  IS INCONCLUSIVE — recorded as unresolved rather than as a result.** The app
  relaunched and is signed in on D3D11, so the portable session survived a
  second upgrade. But `dir` then reported `libgstjpeg.dll` ABSENT from
  `C:\Users\tester\Lightning\gstreamer-1.0`, and the wrapper had echoed
  "expanded" unconditionally after an `Expand-Archive` whose own output it
  never captured — so whether the expand failed, partially applied, or
  succeeded while the file landed elsewhere is NOT established. It says
  nothing about the package: CI proves the shipped zip carries the plugin and
  that `jpegdec` registers from the EXTRACTED tree under Wine. What it might
  say something about is the portable UPDATE path, where a user unzips over an
  existing install — worth re-running with the expand's stderr captured, on a
  guest whose VNC is healthy.

  GENERALISE, because this is the third shape of it this week: a wrapper that
  prints its own success line after a command whose output it discards reports
  success it did not observe. Redirect the command, not the echo.

  WHAT REMAINS IS THE IMAGE, and only that. `libgstjpeg.dll` went into
  `packaging/windows/Dockerfile` and `stage-windows-runtime.py`'s required list
  on 2026-09-02 and the image was never rebuilt, so the plugin has been in the
  recipe and not in the tin. Verified by inspecting `…-v5` directly: neither
  `libgstjpeg.dll` nor `opengl32sw.dll` is in it. Builder tag **v6** exists to
  carry a rebuild and has NO Dockerfile change, because the runner pins by
  exact tag with `pull_policy = "if-not-present"` and a same-tag rebuild would
  never be picked up.

  **`decodebin` in front of that capsfilter is REFUTED, with evidence — do not
  re-propose it.** It builds, then logs `element="decodebin0" … "Delayed
  linking failed."` and `element="capsrc" … "Internal data stream error."`, and
  `aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure` fails because the
  capture is retired. NOT a latency cost: one-buffer wall time 497 ms without
  the decoder, 478 ms through `decodebin`, 486 ms through `jpegenc ! decodebin`.
  A bare `gst-launch` probe of the same three chains PASSES, because it
  negotiates a different format than the engine does (the engine's capture came
  up `A444_16LE`) — "a probe is evidence only if it shares the property under
  test", third occurrence.

  AND THE 10 fps NUMBER IS NOT ANSWERABLE ON THE VM. The claimed cause is a
  USB 2.0 ceiling (raw YUY2 at 1280x720 is 18.4 MB/s); a virtualised
  passthrough does not reproduce it, and the device's mode list can differ
  through the hypervisor. What the guest CAN answer is whether MJPG negotiates
  at all — `camera chain= mjpg` plus `capture negotiated caps=` naming
  `image/jpeg` — and whether the raw fallback still publishes when it cannot.
  The ceiling itself needs Rokas's own hardware.
- ~~**Raise hand is invisible to Element**~~ — **FIXED and LIVE-CONFIRMED
  2026-08-26** in both directions. The wire representation was established by
  READING element-call's own source rather than guessing: an `m.reaction`
  annotating the raiser's OWN `m.call.member` state event with
  `\u{1F590}\u{FE0F}`, lowered by redacting it. Three lanes (our send, two
  sync handlers, one bounded join-time sweep for hands raised before we
  arrived).
- **The `room_list malformed diff rejected` storm — CAUSE FOUND AND FIXED
  2026-09-08 (`b0c27ee`), live confirmation still outstanding.** The capture
  this entry asked for was never needed: the cause was readable. TWO
  PRODUCERS wrote one index base. The SDK's diffs address the vector from
  `entries_with_dynamic_adapters` (20 rooms, growing in batches); the
  snapshot came from `client.rooms()` — the whole state store, different
  order, Spaces included — and both were handed to `handleRoomsEvent`, which
  rebuilt `m_roomOrder`. So mark-as-read, favourite, accept-an-invite,
  create/leave a room and a dozen other ordinary actions replaced the index
  base, the next `set{index}` addressed a different room, was rejected, and
  the rejection called resync, which re-emitted the same snapshot. That is
  why it was account-shape dependent: on a small account the two orders
  coincide. `4185a92` could not have fixed it — it touched only the C++ side.
  The snapshot now has its own event and never writes the index space, and
  resync re-emits from the stream that owns it. Also fixed alongside: an
  index-addressed `remove`/`pop_*` deleted whatever it found with NO id
  check, so a drifted index silently removed a room the SDK never named.
  Do not re-apply the Space exclusion; that one is still in place. WHAT WOULD
  CONFIRM IT: a session on `test_matrix.smetonis.net` with no rejection line
  after a Mark-as-read on a large account.
- **Rooms "lag when they load" on 0.9.0 (Rokas, 2026-09-05; users report
  the same).** MEASURED the same evening with a timestamped log: the history
  fill's page count (see the standing warning above) — 9 pages / 4.2 s on
  a first open, 18 / 11 s on a re-open of a call room. Fixed by lowering the
  invisible-page budget from 60 to 12 plus the wheel-on-short-content
  request; live re-check NOT TESTED at the time of writing. Also changed on
  code evidence: member hydration no longer rebuilds every message body, and
  media rows outside the viewport band no longer fetch at open. Ruled OUT by
  reading: the search sweep (startup and every five minutes, off the GUI
  thread), the per-room deep index and first-unread paging (buttons only).
  **22:48 the same day, after the filter build: "messages are hard to load
  when a call is happening in the room."** No capture yet. Ruled out by
  reading: the per-minute membership state does NOT fire `membersChanged`
  (only a full roster does, RustSdkMatrixClient.cpp ~8824), so the identity
  sweep is not it, and the events are filtered before they become items.
  The leading explanation is the one the maintainer found for the share
  blur: the machine is at 100% CPU during a call, and a page costs ~100-250
  ms of GUI-thread ingest and delegate instantiation that then stretches.
  Needs the stall trace DURING a call, plus the CPU figure at the time.
  For anything that remains, the capture recipe:

      LIGHTNING_GUI_STALL_TRACE=100 QT_LOGGING_RULES="lightning.media.trace=true" \
        scripts/run-dev.sh --log-file /tmp/lightning-roomload.log

  then open three rooms, one media-heavy, and read the stall lines (each
  names a category: `image-decode`, `timeline-diff`, `row-reveal`,
  `timeline-reset`, `rust-poll-drain`) and the `media … cache=` lines.
- **A received share blurs for a moment, then fast-forwards to the present,
  at irregular one-to-five-minute intervals (0.9.0 AppImage, 2026-09-05).**
  A full call log showed the engine's frame counters FLAT — `dropped= 0`,
  no key gaps — through every incident, so the receive path above RTP is
  healthy and the event lives below it: packet loss with a keyframe
  request, or the sender's encoder stalling and bursting. Nothing in the
  client runs on that cadence. The instrument exists now:
  `LIGHTNING_CALL_STATS_TRACE=5` logs, every five seconds and per SSRC,
  packets / loss / jitter / kbps / PLI / NACK / FIR for both webrtcbins and
  the remote side's round trip and fraction lost about what we send. Run
  it on BOTH ends of the call, note the wall-clock time of a blur, and read
  the lines around it; a `pli` step with `lost` climbing is the network, a
  `kbps` dip on the sender's outbound with no loss is the encoder.
  **CAUSE IDENTIFIED BY THE MAINTAINER, 2026-09-05 22:22: it happens when the
  SENDING machine's CPU is at 100%.** That is the encoder case above: `vp8enc`
  (software, `deadline=1 threads=4`, ~0.56 cores at 1080p30 per the table in
  the standing warnings) is starved by the load, frames queue and then burst,
  and the receiver — whose counters were flat — sees a stall and a
  fast-forward. Not fixed. Leads, none shipped and none measured: raise the
  encoder's streaming-thread priority from the pipeline's `stream-status`
  bus message (GStreamer creates those threads itself); trade quality for
  time under load (`cpu-used`, a lower rung); a hardware encoder path
  (VA-API / NVENC), which is a packaging question as much as a code one.
  The stats trace shows this shape as a `kbps` dip on the sender's outbound
  with no loss.
- **Full screen opens on the primary monitor**, not the one the app is on.
  Investigated 2026-09-04 (Task A §6) and NOT changed: `placeOnThisApplications
  Screen()`'s arithmetic is already self-consistent in Qt's own space, and its
  comment correctly records that on Wayland Qt passes no `wl_output` to
  `xdg_toplevel.set_fullscreen` (QTBUG-54883), so the compositor chooses. A
  fourth guess would be a fix without a measurement. What WAS wrong and is
  fixed is the sibling bug: a fresh window CENTRED against
  `Screen.desktopAvailableWidth` — the whole VIRTUAL DESKTOP — so on two
  monitors it aimed at the seam, and on this layout landed at x=6490 on a
  6400-wide desktop and opened invisible. `AppController::centredWindowRect`
  centres inside one screen and VALIDATES the result, falling back to platform
  placement; `window-placement` covers it.

**THE WINDOWS CAMERA'S 10-FPS DEFECT IS REPRODUCED LIVE, AND THE CAUSE IS
NAMED: THE SHIPPED PACKAGE HAS NO JPEG ELEMENTS (2026-09-12, Windows 11 guest,
released 0.9.4 portable, the laptop's own USB webcam passed through).**

    camera MJPG chain unavailable, cameras will use the raw entry:
        no element "jpegenc"
    camera chain= raw (jpeg elements absent )
    capture negotiated caps= video/x-raw, format=(string)YUY2,
        width=(int)1920, height=(int)1080, framerate=(fraction)5/1

The camera WORKS — `capture delivered frames count= 5500`, a real published
track — it is simply starved: raw YUY2 at 1080p is ~62 MB/s, far past USB 2.0,
so the device negotiates **5 fps**. Worse than the 720p@10 this item was
recorded at, because the guest's camera offers 1080p.

**THIS SETTLES THE OPEN OPERATOR STEP, AND THE ANSWER IS THAT IT NEVER
HAPPENED.** §16 records the packaging half as done on 2026-09-02 —
`libgstjpeg.dll` added to BOTH Windows lists — and immediately beside it:
"THE WINDOWS BUILDER IMAGE IS BUILT BY HAND UNDER A FIXED TAG, AND A
DOCKERFILE CHANGE ALONE CHANGES NOTHING… OPEN OPERATOR STEP: rebuild the image
under a new tag." The app asking for `jpegenc` and being told no element
exists is that step's absence, measured on the artifact rather than argued
from the Dockerfile.

So the app half of this defect can now be worked on with a rig that can SEE
it, which it never could before — but it cannot be FIXED until the builder
image is rebuilt, because the element the fix needs is not in the package.

Note what the app did right: it probed for the MJPG chain, found no `jpegenc`,
said so by name, and fell back to the raw entry rather than failing. That is
the documented "build the camera chain for image/jpeg explicitly and FALL BACK
to today's raw chain" design working as intended — the fallback is correct and
the missing element is the defect.

ALSO MEASURED IN THE SAME SESSION, and NOT a defect: `screen share falling
back to the CPU: the GL chain is present but cannot run on this machine`.
§16 says `gpuShareChainUsable()` "has never been observed declining" — this is
the first observation, and it declined correctly and said why, on a guest with
no GPU. The maintainer confirms the GPU share path works on real Windows
hardware, so this is the pre-flight doing its job, not a regression.

**WINDOWS FALLS BACK TO THE SOFTWARE RENDERER WHEREVER THERE IS NO GL DRIVER,
BECAUSE `opengl32sw.dll` IS NOT IN THE PACKAGE (measured 2026-09-12 on a real
Windows 11 guest, released 0.9.4 portable).** From the app's own log:

    Failed to load opengl32sw (The specified module could not be found.)
    Failed to load and resolve WGL/OpenGL functions
    lightning: no usable OpenGL context on the "windows" platform - falling
    back to the software renderer. Video and screen sharing will be slower.

`opengl32sw.dll` is **Qt's own bundled software OpenGL** (Mesa llvmpipe), which
Qt ships precisely so an application keeps a working GL implementation on a
machine whose driver is missing or unusable. It appears **nowhere in
`packaging-ci/`** — `grep -rn opengl32sw` returns nothing. The `opengl32.dll`
in `stage-windows-runtime.py:29` is the SYSTEM_DLLS exclude list, i.e. the
real driver-backed one Windows provides, correctly not bundled; the software
twin is a different file and was never staged.

So the fallback chain loses its middle rung. With `opengl32sw.dll` present Qt
would use a software GL implementation and Qt Quick would still run its OpenGL
scene graph; without it the probe fails outright and the app drops to
`QSGRendererInterface::Software`, Qt Quick's own rasteriser.

**AND "MATERIALLY SLOWER" IS WRONG. RE-RANKED 2026-09-12: THIS IS A
CALL-BREAKING DEFECT.** On the software renderer, call and screen-share video
is not slow — **it is not drawn at all**. Measured twice, and the two agree:

* LIVE, on the Windows guest: winA received 1000+ video frames from a Linux
  screen share, `frames in the clear in ... video= true count= 1000` climbing,
  and the tile on screen was an EMPTY bordered rectangle. Maximising the
  window ruled out clipping.
* LIVE, on Linux, where the backend could be changed as the ONLY variable:
  one client, one call, `QT_QUICK_BACKEND=software` — the counter passed 500
  against an empty rectangle, where the same client on the default backend
  rendered the remote desktop perfectly.

The mechanism is in Qt's own headers. `QSGSoftwareRenderableNode::NodeType`
is a closed list — `SimpleRect, SimpleTexture, Image, Painter, Rectangle,
Glyph, NinePatch, SimpleRectangle, SimpleImage, SpriteNode, RenderNode` — and
Qt Multimedia's `QSGVideoNode` is a `QSGGeometryNode` with a
`QSGVideoMaterial`, which is none of them and is not a `QSGRenderNode` either.
It therefore never gets a renderable node and is never painted, while every
type on that list IS the tile's chrome. The path is RHI-only besides
(`QQuickVideoOutput::initRhiForSink`, `QSGVideoMaterial(..., QRhi*)`), and the
software adaptation has no RHI at all.

So on any Windows host with no usable GL a user joins a call, is heard, hears
everyone, and sees blank rectangles — with the app's own warning telling them
it will merely be "slower". That wording is corrected in `src/main.cpp`.

**WHO THIS BITES**, stated carefully: a Windows machine with a working GPU
driver is unaffected — it gets hardware GL and never reaches this branch. It
bites VMs, RDP sessions, servers, and machines whose driver is missing or
broken. This guest is a VM with no GPU, which is why it showed up here and has
never shown up on a developer's desktop.

**POSSIBLY RELATED, NOT ESTABLISHED:** the open "Lightning is sometimes
bouncing up to 9999 fps" report is from Windows, and a software-rendered Qt
Quick window does not present the way a vsynced GL one does. That is a
HYPOTHESIS. The `lightning: scene graph backend=…` line added in `74ddf08`
names the backend and the panel refresh rate in one line and is the instrument
for it — but it postdates 0.9.4, so it is not in any released Windows build
yet. The next Windows package carries it.

FIX: stage `opengl32sw.dll` alongside the Qt DLLs in
`packaging-ci/scripts/stage-windows-runtime.py`, and assert it, since its
absence is invisible until a host with no driver meets it — the recorded
"graceful fallback and silent absence are the same observable" shape, now for
the sixth time.

**SUPERSEDED 2026-09-12 ON WINDOWS — AND THE ANSWER WAS NOT PACKAGING AT ALL.**
This entry used to end "FIX: stage `opengl32sw.dll`". That is no longer the
outstanding work, and two measurements moved it:

* the builder image was inspected and has NO `opengl32sw.dll`, and never will —
  Fedora's mingw Qt does not ship Qt's Mesa-llvmpipe twin, so there was no list
  to edit;
* and the fallback was OURS. `src/main.cpp`'s probe asks about OPENGL, which is
  right for the AppImage/Wayland case it was written for, and then forced
  `QSGRendererInterface::Software` on EVERY platform. Qt DOCUMENTS Windows'
  default as
  **Direct3D 11** — no OpenGL, WARP when there is no GPU, RHI-backed so
  `QSGVideoNode` renders normally. A Windows box with no WGL was already fine,
  and this code overrode that with the one backend that cannot draw video.

The probe now prefers D3D11 on Windows and Metal on macOS; Software stays the
last resort only where there is no such backend. Verified in the SHIPPED Qt
rather than assumed: `Qt6Gui.dll` in the builder image carries
`QD3D11SwapChain`/`QD3D11Adapter` and imports `d3d11.dll` and `dxgi.dll`, and
`Direct3D11` is an unconditional enumerator in that Qt's
`qsgrendererinterface.h`.

**LIVE-VALIDATED PASS, 2026-09-12 afternoon, and the evidence is a PICTURE.**
On the same Windows 11 guest that counted 1000+ received frames against an
empty rectangle, running the D3D11 build (`lightning: scene graph
backend=d3d11 software=0 platform=windows`), the receiving tile carries the
sending machine's live desktop — the room list, the call bar, the clock, and
the mirror recursion only a live feed produces. Same share, same counters, the
renderer the only variable. The app starts, so the D3D11 creation-failure
worry did not materialise on this guest. Sender was the laptop's Linux client;
the guest holds ONE portable install whose store travels with the exe folder,
so it cannot host two accounts.

**ONE PREMISE IS STILL UNMEASURED.** Whether D3D11 is ALREADY Qt's Windows
default decides whether this restores a shipping-proven configuration or
selects a new one, and the guest cannot answer it — its GL is unusable, so the
probe fires there by construction. It is settled by one line on any Windows box
whose GL works: the probe does not fire, nothing is forced, and `lightning:
scene graph backend=…` reports Qt's own choice. `backend=d3d11` confirms it.
ASK ROKAS for that line from his own Windows machine.

**AND THE GUEST'S CLOCK WAS SEVEN HOURS AHEAD IN UTC** while the earlier
observations on it were made (`Pacific Standard Time` against a UTC hardware
clock). Every `m.call.member` expiry is `created_ts + expires`, so every
membership read as expired and the guest showed no call banner, no Join, no
room-list glyph and a `?` facepile — four things that look like defects and are
not. Fixed with `tzutil /s "UTC"`; the next read went from `participants= 0` to
`participants= 1`. Treat guest call-state observations from before
2026-09-12 13:16 EEST as unreliable, and check `date -u` on the host against a
guest log's own `Z` stamps before believing the next one.

**REJECTED, with the reason, so it is not re-proposed:** "on Windows and macOS
log and call nothing, and inherit Qt's default". That makes behaviour depend on
a default nobody here has measured and which differs between the dev shell's Qt
and the Windows SDK's — the exact class §16 records five times over (Qt 6.8 vs
6.11 proxy `roleNames`, the font fallback, `pipewiresrc min-buffers`, the
GStreamer split, the inline `QPointer`), whose own generalisation is PIN IT.
Inheriting a default is not pinning. It also removes no risk: if the default IS
D3D11 you get the same device creation and the same absence of cross-backend
fallback, with no log line saying what was chosen; and if it is NOT, a failed GL
probe plus no override means Qt tries a backend just proven dead.

ACCEPTED FOLLOW-UP, deliberately not folded into the same change: the GL probe
is the wrong question on Windows and macOS and should eventually be SKIPPED
there outright, so each platform has one pinned configuration rather than two
(GL works -> Qt's unpinned default; GL dead -> pinned D3D11), and so a throwaway
`QOpenGLContext` — itself a hang surface on a broken ICD — is never created.
That moves every Windows user off field-proven behaviour, where today's change
touches only machines with no usable GL. Separate round, separate evidence.

**HOW TO READ THE FIRST WINDOWS RUN, so the result is measured and not
interpreted.** A D3D11 failure looks like `Failed to create RHI (backend …)`
and a process that exits BEFORE a window — not a blank window; if that appears,
`QSG_RHI_BACKEND=software` confirms the backend is the variable, because the
probe block yields to that env var. And **"it starts" is not the PASS**: the
PASS is a tile carrying a remote picture on the same guest that previously
counted 1000+ received frames against an empty rectangle, with the backend as
the only change.

The software-renderer NOTICE is unaffected and still earns its place: a Linux
AppImage host with no usable GL still lands on Software, and that is what it
explains.

**THE SNAP HAS NEVER WORKED. IT INSTALLS AND CANNOT START (found 2026-09-11,
first execution under a real snapd).**

    $ snap run lightning --version
    /snap/lightning/x1/usr/bin/lightning-matrix: error while loading shared
    libraries: libEGL.so.1: cannot open shared object file

Six libraries are unresolved inside the confinement — `libEGL.so.1`,
`libGLX.so.0`, `libGLdispatch.so.0`, `libX11-xcb.so.1`, `libX11.so.6`,
`libxcb.so.1`. All six are **absent from the snap payload AND from `core24`,
and present on the HOST**, which is exactly why `ldd` unconfined resolves
everything and why CI has always passed. `build-snap.sh` copies the AppImage
AppDir's `usr/` verbatim, and linuxdeploy's excludelist deliberately leaves
those on the host — correct for an AppImage, where the host `/usr/lib` is
visible, and fatal for a strictly confined snap, where it is not.
`/var/lib/snapd/lib/gl` is empty: the `opengl` interface carries vendor
drivers, not the base loader.

**STRUCTURAL, not a 0.9.4 accident.** The AppDir of a newer snapshot
(`317bd39`) was extracted and all six checked: all six ABSENT. Every snap
repacked this way fails identically. Fix is packaging-side — stage those
libraries, or plug a `gnome-*-2404` content snap with
`command-chain: desktop-launch`.

`validate-snap.sh` says in as many words that it cannot reach this class ("a
real `snap install --dangerous` needs a running snapd, which the
Docker-outside-of-Docker runner fleet cannot provide"). Fifth appearance of
§16's *graceful fallback and silent absence are the same observable*.

Two more, visible once the payload is run unconfined:
* **Three interfaces do not auto-connect, and the keyring is one**:
  `password-manager-service`, `audio-record`, `camera`. Without the first
  there is no Secret Service and the access token has nowhere to go — §6
  territory. `snap.yaml` flags `audio-record` in a comment and not the
  keyring. All three connect manually.
* **`--call-media-status` misreports the snap's own GStreamer.** It correctly
  loads the bundled 1.26.2 (the host has only 1.24.2) yet prints
  `bundled plugin directory: <none - using system GStreamer>`, because
  `appImageBundledPluginPath()` (`src/calls/GstBootstrap.h:104`) keys on the
  APPIMAGE AppDir layout. That header's own comment says four packaging
  defects here have turned on exactly which GStreamer was loaded, so this
  line telling the wrong story is not cosmetic.

**THE DEB DOES NOT INSTALL ON UBUNTU, WHICH README CLAIMS IT SUPPORTS (found
2026-09-11).** On Ubuntu 24.04 LTS, both the released 0.9.4 deb and a fresh
one:

    Depends: libqt6core6t64 (>= 6.8.2) but 6.4.2+dfsg-21.1build5 is to be installed
    Depends: libgstreamer-plugins-bad1.0-0 (>= 1.26.2) but 1.24.2-1ubuntu4 …
    Depends: qml6-module-qtquick-effects but it is not installable

`dpkg-shlibdeps` runs on Debian 13 (Qt 6.8.2, GStreamer 1.26.2) and bakes
those floors in; Ubuntu 24.04 is on Qt **6.4.2**, and
`qml6-module-qtquick-effects` does not exist in noble at all because
`QtQuick.Effects` is Qt 6.5+. `README.md:143` advertises
`# Debian, Ubuntu, Mint, Pop!_OS`; 24.04 is the current LTS and Mint 22.x /
Pop!_OS 24.04 derive from it. Either the claim narrows to Debian 13+ /
Ubuntu 25.04+, or the deb needs a second build lane. **On Debian 13 the same
deb is clean** — installs, runs, `opengl software=0`, libsecret ready, no QML
warnings.

**THE APP-ID / WINDOW-ICON DEFECT IS CONFIRMED FOR THE FLATPAK**, by the
app's own `--desktop-status`: `app id (desktop file name): lightning`,
`visible launcher entry: NONE`, `visible icon: NONE`. The flatpak exports
`org.lightning_matrix.Lightning.desktop` while the binary stamps `lightning`.
The deb is the control and resolves both. On X11 it still works (WM_CLASS is
`lightning-matrix` and `StartupWMClass` matches); the WAYLAND half is NOT
TESTED for want of a Wayland guest, but `--desktop-status` classifies it.

**THREE THINGS THE FLATPAK SANDBOX BREAKS THAT THE APPIMAGE DOES NOT:**
* **Spell checking is absent, and the diagnostic blames the wrong thing.**
  `--spell-status` says "this machine has no dictionary Lightning can reach",
  but `/usr/share/hunspell` EXISTS in the sandbox and `libenchant-2*` does
  not. Lightning resolves `libenchant-2.so.2` at runtime through QLibrary
  (`SpellBackend.cpp:352`), so this is `no-library`, not `no-dictionary` —
  and `createPlatformSpellBackend` already returns that distinction
  (`SpellBackend.h:71-76`) while `main.cpp:1813` prints the dictionary
  wording for every failure. Two items: the KDE runtime carries no enchant
  and the manifest does not add it, and the diagnostic misdirects whoever
  tries to fix it.
* **The SNI tray cannot own its bus name.** The Flathub manifest adds
  `org.kde.StatusNotifierWatcher=talk`, but there is no `--own-name`:
  `RequestName org.kde.StatusNotifierItem-99-1` is refused with
  `ServiceUnknown` by xdg-dbus-proxy while a control name in the app's own
  namespace is granted. The talk half is fixed and the own half is not.
  Whether Qt still registers via its unique bus name is NOT TESTED.
* Two **25 s D-Bus stalls** at startup (`portal.Settings.ReadAll` and
  `portal.FileChooser`, both NoReply) that the deb and AppImage never incur.
  The guest runs no portal backend, so a real desktop would answer;
  environment-conditioned, not a proven user-facing defect.

**THE RPM INSTALLS ON FEDORA 44 AND NOTHING ELSE, AND THE README SAYS
"Fedora, RHEL" (found 2026-09-11, on the new Fedora test guest).** Measured,
not argued — `dnf install` of the 0.9.4+git rpm on **Fedora 43**:

    nothing provides libQt6Core.so.6(Qt_6.11)(64bit)
    nothing provides libQt6Qml.so.6(Qt_6.11_PRIVATE_API)(64bit)

Fedora 43 ships **qt6-qtbase 6.10.3**. The rpm is built AND validated on
`fedora:44` (`.gitlab-ci.yml`, both `build-rpm` and `validate-rpm` pin the same
digest), which carries Qt 6.11.1 — a deliberate choice, documented in
`packaging-ci/docs/windows-packaging.md`. Three separate things follow and
only the first is obvious:

* **Fedora supports N and N-1.** 43 is current-minus-one and fully supported,
  and the rpm cannot be installed on it at all. `README.md:144` advertises the
  rpm for "Fedora, RHEL" with no version qualifier.
* **"RHEL" is almost certainly false** for any shipping RHEL, which is far
  behind Qt 6.11. Unverified here — no RHEL guest — but it should be checked
  before the claim is repeated.
* **`Qt_6.11_PRIVATE_API` is an ABI trap independent of the above.** Qt's
  private API carries no ABI guarantee, so that dependency pins the package to
  an exact Qt MINOR: Fedora 44 shipping Qt 6.12 breaks it too. Where the
  private-API link comes from has not been traced.

**VALIDATION CANNOT CATCH THIS AND NEVER WILL, because `validate-rpm` runs on
the SAME pinned image `build-rpm` builds on.** That is the recorded "a probe is
evidence only if it shares the property under test" trap inverted — this probe
shares too much. A package's install test has to run somewhere other than the
machine that built it, which is precisely what the new guests are for.

NOT YET ESTABLISHED: whether the rpm installs cleanly on Fedora 44 itself
(the guest is 43; a 44 guest is the obvious next build), and what the download
page claims.

**NOT TESTED, 2026-09-02 audit:** six items in
`docs/security-audit-2026-09-02.md`.

**2026-09-12: THE WINDOWS TRAY-BALLOON NOTIFICATION PATH IS LIVE-VALIDATED —
PASS**, on the Windows 11 guest, driven by a message from a Linux client while
Lightning was minimised. A real toast appears ("Lightning Matrix client" /
"lightningtest in Design Review" with the room avatar), the app logs
`notification delivered through the tray balloon`, and CLICKING IT raises the
app from minimised and opens that room with the unread divider in place. §16
has carried this path as "NOT LIVE-TESTED on Windows or macOS (needs a
packaged build)" since the day it was written. STILL NOT TESTED on Windows:
read-withdrawal of a balloon (Qt cannot do it; it needs WinRT
`ToastNotificationHistory.Remove`) and the notification SOUND. macOS remains
untested entirely.

**2026-09-12, the sound/shortcuts round — LIVE-VALIDATED on a packaged
AppImage, two real clients, a real homeserver.** Driven on the laptop rig
against `matrix.smetonis.net` with the two throwaway fixture accounts, on the
project-6 pipeline 203 artifact (`0.9.4+git20260912.05008f9`). **That AppImage
bundles Qt 6.8.2** — measured from the running process, not assumed — so this
run also answers the standing worry that nothing new here had ever been
rendered on the Qt every deb/rpm/AppImage user actually has, in the one area
(Quick Controls layout inside a `QQuickMenu`) where this project has been bitten
by the 6.8/6.11 split twice.

PASS, each on evidence the layout cannot fake:

* **The in-call microphone level reaches the audio graph.** `microphone gain
  applied: percent= 0 gst= 0 elements= 1` and `percent= 200 gst= 10
  elements= 1`, emitted only after `g_object_set` succeeded on a real named
  `volume` element. This is the round's headline claim and the exact shape of
  the 2026-09-11 claim that had to be withdrawn ("the slider read 200% and
  nothing had reached the audio graph"); the readout is computed from the
  slider's own value and is NOT evidence, which is why the instrument was
  added first. `scripts/gui-suite-calls.sh micgain` is the tracked form.
* **A `Slider` inside a `QQuickMenu` can be dragged** — the menu's ListView
  did not steal it. Driven drags only; a slow or deliberately diagonal HUMAN
  drag is still unexercised.
* **The menu's whole design appears at 200%**: the `mic` glyph swaps to
  `graphic_eq`, the readout tracks, the neutral mark sits at 100, the "Above
  100% amplifies and can clip" warning appears, and the "Reset input volume"
  row appears — all of which are gated on being off neutral.
* **Reset input volume** applies 100 to the engine (`percent= 100 gst= 1
  elements= 1`) and writes 100 to disk.
* **BOTH SURFACES WRITE, independently.** Dragging the SETTINGS slider during
  a live call logs `microphone gain applied: percent= 151 gst= 5.59
  elements= 1` and stores 151, and reopening the in-call chevron menu then
  reads 151%. The reviewer's warning applies and was answered: two sliders
  agreeing proves only that they READ the same value, because Qt does not
  break a QML binding on a C++-side `setValue` — so each surface was driven
  separately and checked against the engine line and the disk, not against
  the other slider.
* **Ctrl+Shift+C is INERT during a live call**, pressed in a different
  RTC-capable room: no `call start requested` line of any kind, and the call
  kept running (17500 -> 19000 frames out). WITH A POSITIVE CONTROL, because
  a dead key and a correctly-gated key look identical — the same key with no
  call running logged `call start requested lane= "matrixrtc"` and
  `sfu joined`. That is the H1 defect's live regression check.
* **Ctrl+Shift+Y leaves the call** (`teardown state= 6`, `membership
  retracted attempts= 1`).
* **Ctrl+Shift+S opens the portal picker and publishes NOTHING** — the portal
  window appeared and no `screen share publishing` line followed. That is the
  privacy-critical half of that key.
* **Ctrl+Shift+A marks every conversation read** and **Ctrl+Shift+R marks the
  open one unread**, both against the real backend with a real unread state
  created by the other client. The mock backend cannot answer either — it does
  not clear unread badges even from its OWN context menu, which is the control
  that proved the fixture rather than the feature was the limitation.
* Ctrl+Shift+T (DM dialog) and Ctrl+Shift+I (Activity Center), the Sound &
  video section, the media-playback and microphone sliders writing to disk,
  the devices leaving Notifications, and the quick switcher reaching the new
  sections: PASS on an isolated Xvfb display (`DISPLAY=:99` + xdotool, which
  cannot reach the maintainer's session).

STILL NOT TESTED: **audibility** — nobody listened, and what is proven is that
the value reaches a real GStreamer `volume` element, exactly the split the
per-participant volume carries; a slow or diagonal human drag on the menu
slider; restart with a call; the PiP window, which declares none of the call keys and
so should be dead for all of them; media playback volume against a real video;
and Windows/macOS, where `requestScreenShare()` takes a different code path
this key has never exercised. Plan, with what would make each look like a pass
while broken: vault note "Lightning/Testing/Sound and shortcuts round — live
test plan".

**THE ACCOUNT SWITCH IS LIVE-VALIDATED — PASS, and it closed one of the
review's follow-ups by finding the UI said the wrong thing.** Driven inside
ONE client on the laptop rig, which has both fixture accounts saved: the
microphone level read 151% for `lightningtest`, 60% for `lightningtest2`, and
151% again on switching back — while the GLOBAL fallback key held 60 the whole
time, so the UI is reading the account key and not the fallback. A fresh
account inherits the last value rather than snapping to a default, which is
`appearanceValue`'s design and is what a user would want. Set different values
on each account first: the reviewer's warning that two untouched accounts both
read 100 and make the scoping invisible is exactly right.

That run also showed the section's own explainer was wrong. "These devices
belong to this computer, not to your account" sat directly under the
microphone LEVEL, which is account-scoped — so the one sentence telling anyone
which of the two they are changing was backwards about one of them. Fixed, and
pinned from both ends by
`theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive`.

ACCEPTED FOLLOW-UPS from that round's review, none blocking: the in-call level
slider cannot be reached by keyboard (`QQuickMenu` arrow navigation visits
menu ITEMS and it sits in a plain `Item`), so not by a screen reader either;
the menu changes height mid-drag when the value crosses 100%; `call.returnToCall`
= Ctrl+Alt+A is Polish AltGr+ą and macOS ⌘⌥A and stays because moving a
shipped default silently rebinds it for every install; and Ctrl+Shift+A
shadows `QKeySequence::Deselect` in every X11 text field.

**2026-09-04, local search and widgets.** Both are LIVE-VALIDATED against
`matrix.smetonis.net` with throwaway fixture accounts (credentials in the
vault, `Lightning/Testing/Test Accounts.md`, never in this repository):
local search finds a Megolm-encrypted message this client sent, and the widget
list returns one openable widget plus three refused with the right reason and
excludes the tombstone.

**The widget QML is now LIVE-VALIDATED TOO, on a packaged AppImage** (later the
same day, 0.8.4+git 6989623, driven through the GUI against
`matrix.smetonis.net`). Four real `im.vector.modular.widgets` state events were
written to a fixture room: the https one lists with an ENABLED Open, the
`http://` and `javascript:` ones list with a DISABLED Open and the not-HTTPS
refusal, and the one with no `url` is dropped from the list entirely. The
consent sheet has been seen: it names the widget, its URL and who added it,
says the site receives the user's IP, and states that Lightning opens widgets
in the browser so the page cannot reach the account, keys or messages. One
inaccuracy worth fixing eventually and not a defect: a `javascript:` URL is
refused with the not-HTTPS wording rather than a scheme-specific one.

SUPERSEDED 2026-09-10 — kept only to say so, because it sent a later round
looking at the wrong surface: this paragraph used to read "STILL NOT SEEN: the
find bar's source strip and coverage line … local search remains validated at
the Rust layer only." Local search HAS since been driven from the GUI (it is
the find bar's History scope, Ctrl+F — not the room-header magnifier, which
opens the SERVER-side panel) and its coverage line was read off the screen.
See the 2026-09-10 live-validation entry above.

Keyboard automation now works — a `ydotool` uinput device plus KWin scripting
for closed-loop pointer positioning, with a focus guard that refuses to type
unless KWin reports the intended window active. The guard exists because
without it a login went into a browser window instead of the client.

**2026-09-05, the tester-report batch, driven through the GUI on the packaged
tree's binary against `test_matrix.smetonis.net` with the fixture account.**
PASS: the room-info tab strip wraps to two rows at the 260 px floor with the
panel inside the window (it used to be pushed off the right edge — the strip's
natural width was the panel's MINIMUM); the Widgets tab offers Add widget…,
the dialog writes a real `im.vector.modular.widgets` event and the list
re-reads with Open + Remove, and Remove writes the tombstone and the list
empties; Lightning Light's search field and composer measure `#F5F9FE` and
Moss Light's `#F6FBF7` (sampled off the capture, no pure white left); the
Settings header is the room header's height; Settings opened with NO GUI
stall ≥100 ms on any of three opens (`LIGHTNING_GUI_STALL_TRACE=100`) where
the same trace showed 428 ms per open before the loader was kept alive; the
GIF picker keeps a typed query across a provider switch. NOT TESTED: the Home
"Set up backup" banner — the fixture account has a backup, so the banner does
not appear; the edit is text and target only. MEASURED AND NOT ADDRESSED: a
theme switch blocks the GUI thread ~940 ms (every token repaints), and the
FIRST Settings close after the kept-alive change logged one 226 ms
unattributed stall that the second and third did not — one observation.
Harness lesson that cost a click batch: a fresh launch came up on the OTHER
monitor and six guarded-nothing clicks went into Steam; every click now goes
through `winclick`, which asks KWin for Lightning's frame and refuses unless
Lightning is the active window. And `QT_FORCE_STDERR_LOGGING=1` is mandatory
for a nohup'd launch, or Qt hands every line to journald and the log file
holds only the nix banner.

**2026-09-05, later the same day — the second report batch, NOT live-validated
(Rokas was at the PC, so no GUI automation; see the harness lesson above).**
Fixed from his screenshots and log, each with a regression test: the
encryption lock beside the room name (`TesterReportFixesTest`); read receipts
hosted by the nearest row that draws a body, so a marker on a
call-membership update no longer vanishes (`timeline-model-diff`); the call
popout hosting the stage's tile grid with shares as their own tiles (load
gate only); a participant volume chosen before their track arrives applied
when the bin is built, and the "nowhere to land" line logged once per key
instead of hundreds of times (`sfu-media-engine`, mutation-proven). The
Windows "volume 0 does not mute" report is the likely same defect and stays
OPEN until a Windows tester confirms. Also from review of the first batch
(CHANGES_REQUESTED, all addressed): the kept-alive Settings screen's
window-level Escape/Ctrl+, Shortcuts are gated on `root.visible` (two
enabled Shortcuts on one sequence make Qt fire NEITHER — proven by a real
key in `SettingsShellQmlTest`), a widget write answered after a room switch
no longer wedges `writing`, `validate_widget_write` is one function the Rust
test actually calls, Remove names the exact state key and is hidden for rows
the reader could not name, and Lightning Light's elevated rung was stepped
to `#E4EEFA` (the tint had collapsed it to 1.017:1).

Two open decisions those rounds created, both recorded rather than taken:
- **The SDK store is not encrypted at rest**, and it holds DECRYPTED
  encrypted-room bodies (`encode_event` serializes the `Decrypted` variant,
  `encode_value` is a no-op with no cypher, Lightning opens
  `sqlite_store(path, None)`). §6's memory-only rule binds Lightning's own
  `CacheStore` and is intact; the property it was protecting is not held by
  the installation. The search index is a second copy of what is already
  there. The audit's route to fixing it is in
  `docs/security-audit-2026-09-02.md`; a naive passphrase bricks every install.
- **Widgets are LISTED and opened externally, never embedded**
  (`docs/widgets.md`). Revisiting that needs a Windows Qt story with WebEngine,
  a Flatpak answer that is not "disable the sandbox", and a decision about
  `QtWebEngineQuick::initialize()` forcing the whole scenegraph to OpenGL.

STILL UNPROVEN, not reported broken:

- **A fresh `QSG_RENDER_TIMING` capture** proving the row window does anything
  in production (`winApplies` > 0, `rows` ≪ `srcRows`). No production frame-cost
  improvement has ever been observed from it.
- **The still-unreproduced freeze after hammering reactions** — hand over a
  build with `LIGHTNING_GUI_STALL_TRACE` enabled. One capture beats three
  theories.
- **GIF-favorite reopen crash** — still only `1502e6b`'s commit message as
  evidence; seven headless scenario families including an ASan build found
  nothing. Needs a real `coredumpctl`/`gdb` backtrace.
- **The Rust `children` payload against a real homeserver** — that a real
  `m.space.child` order arrives in the order its admin set. Adjacent to item 2
  above but not the same claim.
- **`app.` dereferences in creation-time bindings of other `Repeater`
  delegates** (`qml/EmojiPicker.qml`, `qml/SettingsScreen.qml` theme cards) —
  structurally exposed to the poisoned-context-lookup defect fixed in
  `30ee39b`, not observed failing.
- Continue GIF playback, cancellation, resource, cache and malformed-media
  hardening.

**Accepted follow-ups, none blocking:** decide whether a client-side
sanity ceiling should apply when the server advertises no upload limit
(deliberately absent — see §7); `setVoiceRecorderForTest` would be
better taking a `unique_ptr`; `voice_info` computes `info.size` from the
stat size rather than the uploaded bytes; `FakeRecorder` is a PARTIAL
double (`stop()`/`durationMs()` are not virtual, so anyone needing
`stop() → ready()` must extend the seam consciously); the pre-existing
invite/verification notification bodies carry unescaped member-chosen
text.

"Recovering never-backed-up Megolm keys" is **refused, not deferred**: a
key that was never backed up and never shared exists nowhere, every
legitimate recovery path is already implemented, and anything further
would weaken E2EE.

Do not list the implemented GIF browser, favorites/recents, download and
send path, provider networking, thread summaries/attachments,
notification sounds, or E2EE generation isolation as unfinished. Do not
turn possible future ideas into commitments.

---

**THE SNAP STARTS NOW AND ITS GUI DID NOT, AND BOTH HALVES ARE MEASURED
(2026-09-12, a real `snap install --dangerous` in an Ubuntu 24.04 guest under
snapd 2.76.3 — the only place either question can be answered).**

Artifact: project 6 pipeline 198's `build-snap`,
`lightning_0.9.4+git20260911.84a3294_amd64.snap`.

PASS, and it is new: the snap installs and RUNS. `lightning --version` answers
`Lightning 0.9.4`; `--call-media-status` reports the media engine built in,
GStreamer 1.26.2 and both call engines available. `84a3294`'s excludelist-family
staging is what fixed the never-starts defect this file used to record.

FAIL on the shipped artifact: **it cannot open a window.**

    cannot load: /snap/lightning/x2/usr/plugins/platforms/libqxcb.so:
    libSM.so.6: cannot open shared object file: No such file or directory
    qt.qpa.plugin: Could not load the Qt platform plugin "xcb" ... even
    though it was found.

`libSM.so.6` (X session management) and its own dependency `libICE.so.6` were
not staged. Qt's advice on that failure path names `xcb-cursor0`, which IS in
the payload — so the message points at the wrong library and cost a detour.

PROVEN, not predicted: unsquashing the snap, copying the guest's `libSM.so.6`
and `libICE.so.6` into `usr/lib`, repacking and installing gives a **rendered
sign-in window**. Those two libraries were the whole distance between the
shipped snap and a working GUI.

WHY NOTHING CAUGHT IT, and this is the reusable part. A Qt platform plugin is
**dlopened**, so its dependencies are not the binary's: `build-snap.sh`'s
"nothing unresolved" guard asked the BINARY, which was perfectly satisfied.
And no CI job can reach it either — `validate-snap.sh` runs with
`QT_QPA_PLATFORM=offscreen`, which never loads xcb, so a snap whose windowing
is entirely broken passes every check and installs cleanly. Fourth appearance
of "a library loads its own plugins" in this project.

FIXED in `packaging-ci/scripts/build-snap.sh`: `libSM.so.*|libICE.so.*` join
the staged families; `libSM.so.6`/`libICE.so.6` join the named assertion; the
staging PROBE set widens from the two platform-plugin directories to every
`usr/plugins` and `usr/lib/gstreamer-1.0` plugin; and a new guard runs `ldd`
over every plugin against the payload — fatal for a Qt plugin, fatal for
`libgstopengl.so`/`libgstwebrtc.so`, a warning otherwise. **The fixed build has
NOT been run** (it needs a pipeline), so the guard itself is NOT TESTED.

STILL MISSING in the same run, and found only because the window came up:

    Failed to load plugin '.../libgstalsa.so':   libasound.so.2: ...
    Failed to load plugin '.../libgstopengl.so': libGL.so.1: ...

`libGL.so.1` is why that snap logged `no usable OpenGL context on the "xcb"
platform - falling back to the software renderer` — which, per the opengl32sw
entry above, means **the snap would show no call video at all**. Both families
were already in the staged list and neither was staged, because the probe set
did not include the plugins that load them; the widened probe set above is
aimed squarely at this.

NOT TESTED on the snap, and not reachable without a display in the guest that
is more than Xvfb: Wayland (the payload still bundles `libwayland-client.so.0`,
which the AppImage deliberately DELETES over GitHub issue #9 — under
confinement the calculus differs, but it has never been exercised), GPU
acceleration, audio, and any call.


---

**ACCEPTED FOLLOW-UP, not done: the camera-portal state machine has no
coverage, and the one suite that could reach it is built without the media
engine.** Raised in review 2026-09-12 and recorded rather than fixed, because
restructuring call code deserves a round where it can be iterated on.

`SfuCallController::startCameraCapture()` and `publishCameraTrack()` have their
entire bodies inside `#ifdef HAVE_LIGHTNING_WEBRTC`. `call-controller-test` is
the only target that constructs a `SfuCallController`, and it links
`Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test` only — so it never receives that macro
and `startCameraCapture()` is an empty function there. `m_cameraAwaitingPortal`
can therefore never become true in a test, and the four camera cases added with
`21f4a1a` can only call the pure `linuxCameraRoute` predicate, which is exactly
what they do.

Untested as a result, and these are the paths that leak a file descriptor or
wedge the camera: the three fd-ownership exits in the `ready` lambda, the
leave-during-dialog unwind, the second-press unwind, and the call-end cancel.
The lambdas themselves are already OUTSIDE the guard, along with
`setCameraPortal()` and `abandonPendingCamera()`, so a test could wire a real
`CameraPortal` and emit `ready`/`cancelled`/`failed` today — only those two
guarded bodies stand in the way.

THE SHAPE IS ALREADY IN THIS FILE'S NEIGHBOUR: `SfuCallController.h` keeps
`m_engineParticipantVolume`/`m_engineShareVolume` shadow records outside the
media guard, with a comment saying why, and that is what makes
`aStoredVolumeReachesTheEngineWhenTheStreamIdArrivesLate` a real test. Mirror
it — a `cameraAwaitingPortalForTest()` and a publish counter kept outside the
guard — and the state machine becomes drivable without a media engine.

---

**~~`DELAYED_EVENTS_REFUSED` IS PROCESS-GLOBAL~~ — FIXED 2026-09-12, kept for
the reasoning.** It is now a `Mutex<BTreeSet<String>>` keyed by the
homeserver's own URL, so an account switch simply asks a different question.
`one_servers_refusal_does_not_disable_msc4140_for_another` pins it and is
mutation-proven against the old process-global behaviour. What follows is why
it mattered.

`rust/src/rtc.rs` keeps it as a `static AtomicBool`. It latches when a
homeserver proves it will not arm a delayed retraction, and that is a real,
permanent property OF THAT SERVER — for one account it is exactly right, and
the one-way behaviour is deliberate (once latched, the gate stops arming, so
nothing calls `schedule_delayed_leave` again and the clearing write is
effectively unreachable).

Across accounts it is wrong. Sign in to an account on a Synapse that ignores
`?org.matrix.msc4140.delay=`, latch it, then switch to an account on a server
that implements MSC4140, and for the rest of the process the second account:

* gets no server-side retraction, so an unclean exit strands its membership for
  the full expiry — the five-minute ghost participant this round fixed the
  OTHER cause of; and
* is told scheduled send is unsupported (`rust/src/rooms.rs` reads the same
  flag), so it silently falls back to the local queue.

FIXED exactly that way. The two recoveries the old doc comment claimed — a
server that GAINS support, and a mis-latch — are now genuinely reachable,
because the entry that would have to be cleared belongs to one server.

---

**TWO UI CLIPPING ITEMS THE 2026-09-12 WINDOWS SWEEP LEFT OPEN.** The sweep
itself came back mostly clean — room list rows, the composer (its `…` overflow
at 640 px is a designed adaptation, not a break), message rows with long
unbroken tokens, link-preview and attachment cards, the room-info tab strip at
~313 px, and Settings all PASS at 640 / ~1100 / 1280. These two did not.

**1. The last facepile avatar is sliced, at every width including maximised.**
Settled as a CLIP, not the control pill painting over it: a 900% crop shows a
straight vertical cut with pale-green header background beyond it, while the
pill's rounded edge starts well to the right with a clear gap. An opaque pill
would have cut along its own arc and left white.

Owner: `qml/CallSpeakerBubbles.qml`'s `clip: true` on the `anchors.fill: parent`
ListView. Its host, `callHeaderBubblesHost` in `qml/CallStage.qml`, has
`Layout.fillWidth: root.collapsed` — false while expanded — and, unlike the
controls Loader, no `Layout.minimumWidth` floor at all, so it is squeezed below
its natural width. That it fires at 1280 maximised is independent evidence the
header row was over-full even at full width.

**ROOT-CAUSED AND FIXED, 2026-09-12, and this diagnosis was wrong.** The host
was never the problem and neither was the control overflow. The owner is
`CallSpeakerBubbles.qml`'s own `implicitWidth`, which said
`N * (bubbleSize + spacing6)` while the delegate's cell is `bubbleSize + 8` —
the width the SPEAKING RING needs, which the delegate's comment records
widening it to. So with two people the strip asked for 80 px, drew 90, and its
own `clip: true` cut ~10 px off the last avatar. Measured, not argued: the
regression case reports "the strip asks for 80 px and draws 90 px" on the old
formula. The cell is one `cellWidth` property now, read by both.

The instinct NOT to add a floor to the host was right, for the wrong reason —
a floor would have masked an arithmetic error with extra space. What hid it
from every existing test is that the COLLAPSED header fills the strip's width
from its host, so the defect only appears on the spotlight branch where the
strip takes its own implicit width.

**LIVE-VALIDATED PASS** on the pipeline-204 AppImage
(`0.9.4+git20260912.8943ae9`), two clients in a real call with a share running
— which is what puts the strip in the header at all, since the facepile is
absent without a spotlight. Before and after captures at the same window size
and the same crop: the second avatar's right side is cut flat on the old build
and is a complete circle on the new one.

**3. The floating call window's hang-up button did nothing on a 1:1 call, and
its keyboard is still dead.** `qml/CallPipWindow.qml` called
`app.calls.hangUp()` where the invokable is `hangup()`; every other call site
in the tree spells it correctly. On the LEGACY lane the button threw a
TypeError, so the one surface that exists for a minimised window could not
leave the call. FIXED, with a sweep that now checks every `app.calls.X` and
`app.groupCall.X` in `qml/` against the two headers' actual members, because a
QML→C++ name is only checked when the line RUNS and nothing here had ever run
that handler.

STILL OPEN, and an accepted follow-up rather than a defect: `CallPipWindow`
declares exactly ONE `Shortcut` (Escape, for the share-fill mode), and a QML
`Shortcut` is matched by WINDOW — so with the floating window focused, mute,
deafen, camera, leave and screen-share are all dead keys. Its pointer controls
cover every one of those actions, and three of the five predate the 2026-09-12
round. Worth closing when someone can drive the PiP in a test; it is the
window a keyboard user is most likely to be looking at, since the main one is
minimised by definition.

**2. The room-header title elides with room to spare, and the obvious
candidate is REFUTED.** At 640 px: header ~338 px, so `header.width * 0.5`
allows ~169 px; the rendered title ink is ~55 px, followed by ~60 px of EMPTY
space before the icon cluster. So `qml/TimelinePane.qml`'s 50%-of-header cap is
not the binding constraint — the title elides at roughly a third of what the
cap allows — and a layout squeeze cannot leave 60 px unused beside the thing it
squeezed.

**THE HYPOTHESIS IS REFUTED, 2026-09-12, on the real component.** It was that
`Math.ceil(implicitWidth)` reflects the ELIDED content once eliding is active,
making `Layout.maximumWidth` self-referential and ratcheting the width down.
If that were true the title could not recover without something resetting it,
and it does: driven on the laptop rig with "Lightning search fixture" open, the
title elides to "Light…" at a 640 px window and renders IN FULL at both 1200
and 1707 — no reset, no reload, just a resize. A self-referential bound cannot
un-ratchet. `Math.ceil(implicitWidth)` is not the cause and that line should be
left alone.

**And the symptom did not reproduce either.** At 640 px the gap between the
elided title and the first header icon measured ~17 logical px, not the ~60 px
the original sweep recorded — a normal layout gap. The title ink matched (~54
px). The likeliest explanation for the difference is the ICON COUNT: the
original was measured on the Windows guest in an ENCRYPTED room, which carries
the padlock this one does not. So what is left of this item is "the title
elides at a 640 px window", which is a genuine squeeze — the header has about
277 logical px for an avatar, a title and four icons at that size — and not
obviously a defect. Re-open it only with a capture showing real empty space
beside an elided title, and say what the icon cluster held.

NOT TESTED in that sweep and worth covering: code blocks and wide tables (the
one attempt was void — shell quoting truncated the message before it reached
the composer, and the "clipped code block" it appeared to show was simply the
message the app had received), and the call stage's tile grid, which cannot be
judged on a machine that renders no video.

---

**THE SNAP'S STAGED-LIBRARY LIST AND ITS JOB'S APT LIST HAVE TO AGREE, AND
THEY DID NOT (2026-09-12).** A follow-up to the libSM fix, and it would have
turned that fix into a red build rather than a working snap.

`stage_unresolved_libs` copies from what `ldd` RESOLVES ON THE BUILD HOST. So a
family named in `build-snap.sh` that is not installed in the `build-snap` job's
image stages **nothing** — and the payload guard added in the same round then
fails the build. Loud rather than silent, which is the right failure, but still
a failure. `libsm6`/`libice6` were not in that job's apt list.

Found by simulating the staging against pipeline 198's real payload rather than
by reading: copy the named libraries into an unsquashed copy, re-run the guard,
and see what the NEXT layer asks for. That surfaced one more, and it is the
kind nobody guesses — **`libSM` needs `libuuid`**, so `libqxcb.so` would STILL
have failed to load with libSM and libICE both present.

Now in the job's apt list: `libsm6 libice6 libuuid1`, plus `libasound2t64`,
`libdrm2`, `libgbm1`, `libwayland-cursor0`, `libwayland-egl1`, `libfribidi0`,
`libthai0` — the set the widened plugin sweep reported once a window opened.
`libuuid.so.*`, `libfribidi.so.*` and `libthai.so.*` join the staged families.

**AND MY FIRST SWEEP OF THAT WAS A PROBE THAT COULD NOT FAIL.** I reported "no
fatal entry" from a tree into which I had hand-copied `libGL.so.1` and
`libgbm.so.1` — the exact two libraries the staging step turns out to be unable
to produce. The probe did not share the property under test, which §16 records
as a standing trap and which I walked into anyway. Caught in review by running
the REAL functions in the job's own pinned image against the PRISTINE payload.

Two things that run found, both since fixed:

* **`ubuntu:24.04` ships no `readelf`**, and the guard walks each plugin's
  NEEDED graph with it. Without `binutils` the walk yields nothing, every probe
  reports nothing missing, and the guard passes VACUOUSLY on a payload whose
  plugins cannot load — the same silent-absence shape it was written to end.
  `binutils` is now in that job's apt list.
* **`ldd` could not see a GStreamer plugin's own dependencies.** Qt's plugins
  carry `RUNPATH=$ORIGIN/../../lib`, so `ldd` walks into the AppDir and reaches
  libSM; a gst plugin carries `$ORIGIN` alone, so the walk stopped at
  `libgstgl-1.0.so.0 => not found` and never reached `libGL`/`libgbm`. Naming
  those families therefore staged NOTHING and `libgstopengl.so` stayed
  unloadable — meaning the snap kept reproducing the very software-renderer
  condition the call UI's new notice exists to explain. Staging now resolves
  with `LD_LIBRARY_PATH="$TREE/usr/lib"`; measured in the job's image against
  pipeline 198's payload, 10 staged before and 12 after, and the guard then
  passes with no fatal entry and no warnings.

Worth keeping beside that: the family list is not self-sufficient, it is
self-sufficient GIVEN THIS AppDir. `libICE` needs `libbsd`→`libmd`, `libthai`
needs `libdatrie`, `libgbm` needs `libexpat`, and all four resolve only because
linuxdeploy already bundled them.

**NOT TESTED: the resulting build.** None of this has run in CI. What the
review's docker runs establish is that the staging and the guard now behave
correctly on the real image against the real payload; a pipeline is still what
proves the job goes green.

---

**THE SOFTWARE-RENDERER NOTICE COVERS THE CALL UI ONLY.** `qml/CallStage.qml`
explains why no video appears during a call; `VideoPlayerCard.qml`,
`VideoViewerOverlay.qml` and `MessageComposerBar.qml`'s preview will still show
an unexplained empty rectangle on the same machine, because they draw video the
same way and nothing tells them either. Pre-existing and deliberately out of
scope for the round that added the notice — recorded so it is not later read as
closed. `app.softwareRenderer` is already available to all three.
