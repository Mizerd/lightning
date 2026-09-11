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
  **THE APP HALF IS STILL OPEN, and the exact blocking line is now known.**
  It is not `videoconvert`: `captureEntryFilter(false)`
  (`src/calls/SfuMediaEngine.cpp`) is
  `capsfilter caps="video/x-raw,pixel-aspect-ratio=(fraction)1/1"` and it sits
  DIRECTLY after `%1 name=capsrc`, so `image/jpeg` cannot satisfy the very
  first element downstream of the source and no MJPG mode can ever negotiate.
  **`decodebin` there is REFUTED, with evidence — do not re-propose it.**
  Inserting `decodebin ! ` in front of that capsfilter builds, but the bin
  logs `element="decodebin0" ... "Delayed linking failed."` and then
  `element="capsrc" ... "Internal data stream error."`, and
  `aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure` fails because the
  capture is retired. It is NOT a latency cost: measured one-buffer wall time
  is 497 ms without the decoder, 478 ms through `decodebin`, 486 ms through
  `jpegenc ! decodebin`. A bare `gst-launch` probe of the same three chains
  PASSES, because it negotiates a different format than the engine does
  (the engine's capture came up `A444_16LE`) — the recorded "a probe is
  evidence only if it shares the property under test" trap, third occurrence.
  What is left to try, in order: build the camera chain for `image/jpeg`
  explicitly and FALL BACK to today's raw chain when it will not build, which
  is the idiom the GPU share path already uses and logs; or resolve the
  device's caps first and choose. Either needs a real webcam, so it is not
  landing from this machine.
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

**NOT TESTED, 2026-09-02 audit:** six items in
`docs/security-audit-2026-09-02.md`.

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

