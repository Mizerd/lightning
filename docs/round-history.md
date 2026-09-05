# Round history

Moved out of `CLAUDE.md` §16 on 2026-09-03: that file had reached 150,397
characters against a 150,000 limit and was being truncated, silently dropping
its own tail — sections 17 to 19 — from agent context, exactly as §7 was moved
to `docs/feature-contracts.md` on 2026-08-28. Nothing was deleted; the whole
block is below unchanged.

This is a LESSON INDEX, not an inventory, and it is organised by THEME rather
than chronology. Source and `git log` remain authoritative. The standing
warnings, the live-validation record and the open-items inventory all stay in
`CLAUDE.md` §16 — only this block moved.

By THEME, not chronology, and reduced to rules, refutations, deliberate
decisions, measured numbers and live status. Features are §7; the caps
contract, the refutation rule and the probe rule are in the standing warnings.

#### Capture, encoding and the media pipeline

- **Caps evidence.** `WindowCaptureSrc` fixated 1920x1080 against a 3840x2100
  window: half-row shear, top quarter only — under 1920 wide the stride matched
  and only bottom rows were lost. `gdiscreencapsrc` reports FIXED caps, so the
  restricted structure drops out: that is why a MONITOR share got 3840x2160 and
  a WINDOW share did not. With PAR opened downstream and the source's fixate
  silent, 3840x2100 and 3840x2160 ERROR while 1557x1213 passes carrying
  1/2147483647 — every window over the ceiling would have published NOTHING.
  The element fixates 1/1 itself AND a FIXED 1/1 capsfilter sits before the
  source, because `avfvideosrc` declares no PAR and `pipewiresrc` is untestable
  outside a portal.
- **PARs VP8 discards** — videoscale's "keeps the aspect ratio" is false; a
  libwebrtc receiver draws the literal size. 3840x2100 -> 36/35; 1920x1200 ->
  9/10 (11% stretch); 3440x1440 -> 43/32 (34% squash); `ximagesrc` -> 2/1.
  Unreported for months because the maintainer's monitors are 16:9.
- **`videorate` clocks from SEGMENT START, not the first buffer's PTS**, and
  capture sources stamp pipeline RUNNING TIME, so a source started mid-call
  back-fills 30 duplicates per second of CALL AGE — one picture at full rate,
  counters healthy. That was the "camera does not work". First PTS 0/10/174 s
  -> 27/327/5247 out; `skip-to-first=true` -> 27 always. Lightning's own
  element stamps from ZERO and must KEEP that.
- **`videorate` also HOLDS the first buffer until a second arrives**, and a
  PipeWire capture delivers ON DAMAGE, so the wait is "until the screen moves".
  REFUTED here: `skip-to-first` does nothing; `max-duplication-time` keeps the
  hold AND starves the encoder below 30 fps. `keepalive-time=100` re-pushes the
  held buffer, is NOT `min-buffers` renamed, and 100 ms is DIAGNOSTIC (a dead
  capture reports ~10/s vs a live one's up-to-30/s). **UNRECONCILED: open items
  record `keepalive-time=100` as having KILLED the capture.**
- **`min-buffers=8` was an unmeasured guess that made things worse and was
  banned; ban RE-SCOPED 2026-08-28** — it could not negotiate against a
  compositor offering at most 4 buffers on PipeWire >= 1.6, so "no frame
  arrived" was an allocation failure, not proof the property is forbidden.
  `min-buffers=1` is REQUIRED.
- **A counter downstream of `videorate` cannot prove the capture is alive** —
  it repeats the last picture, so a dead capture still encodes, encrypts and
  sends at full rate. Count the capture's own buffers, before it.
- **A desktop capture is VARIABLE RATE** — PipeWire negotiates `framerate=0/1`
  at native size (3840x2160 BGRA), and a range including `0/1` leaves `vp8enc`
  no rate to plan against. Pin `30/1`; sizes stay ranges, being ceilings.
- **`rtpvp8pay` parses the VP8 bitstream and cannot payload an encrypted
  frame** — libwebrtc takes the descriptor from the encoder as METADATA, hence
  `RtpVp8Payloader`, which reads nothing.
- **element-call mints a 16-byte media key, livekit-client 32** — requiring 32
  rejected every Element key for its LENGTH, so an Element peer was inaudible
  while our own media reached them.
- **Identify a received track by TRACK SID from the msid, never a `mid`** — a
  `TrackInfo.mid` belongs to the PUBLISHER's connection.
- **A capture that ends itself must be heard** — closing a shared window
  answers EOS and nothing listened, leaving the track declared and the far end
  frozen. It retires through Stop's path, which sets the transceiver INACTIVE.
- **Verify against something that is not Lightning** — two Lightning clients
  agree on streams a libwebrtc receiver rejects; `livekit-cli` (pion) and an
  independent frame-crypto implementation each refuted a confident wrong
  theory. Full table: `docs/matrixrtc.md`.

#### The v0.9.0 round (Task B), 2026-09-05

Ten features in one round. What is worth keeping is not what they do — that is
`docs/feature-contracts.md` — but what each one refused to do, and what my own
mistakes cost.

- **A rule enforced on one of N paths is not enforced.** Read-receipt privacy
  has THREE Rust send paths (in-room, mark-a-room-read, the thread panel). The
  mode is stored ONCE on the bridge and all three read it; the two that build
  `Receipts` share one `receipts_for_mode`. The same shape drove the pack CRUD
  (one `PackEdit` enum, one writer, two stores) — four near-identical async
  functions is how three of them end up correct.
- **A notification card outlives the account that raised it.** The user can
  switch accounts or sign out while it is on screen. Acting on it under the
  current account would mark another account's room read, or reply from the
  wrong identity — and it would SUCCEED, so nothing would report it. Every
  payload carries the account it was raised for.
- **`inline-reply` arrives in TWO parts on some daemons** (ActionInvoked, then
  NotificationReplied), so the payload must SURVIVE the first. Dropping it
  there fails only on the desktop nobody tested.
- **One sink per track, last attach wins** decided the picture-in-picture
  design. PiP and full screen are mutually exclusive IN CallStageState rather
  than by convention, its surfaces are built only while the window shows, and
  its tiles are Repeaters over live models — a `get(row)` snapshot taken
  before a share's track key arrives never attaches a sink (CallStage learned
  this first).
- **Both of Discord's call keys were already taken** — Ctrl+Shift+M is
  `room.markRead`, Ctrl+Shift+D is in the reserved table — and
  `everySeededDefaultAvoidsTheHardCodedKeys` caught the second. The gate
  works; use free keys and let people rebind.
- **matrix-sdk-ui sanitises INCOMING html with a hard-coded const** and strips
  `data-mx-emoticon`; read the RAW event's `formatted_body` instead, and call
  that AFTER `fill_message_content` or its guard returns early and the feature
  silently does nothing. **Allowing that attribute DISABLES ruma's img-src
  scheme check** — its loop returns on the first attribute with no scheme
  rules — so mxc-only is enforced by our own strip, and the `<img>` is REBUILT
  from validated parts rather than passed through.
- **MSC4108's blocker was our own recorded decision, not the SDK.** The
  new-device direction needs the OAuth device-code grant, which `oauth.rs`
  deliberately does not request (with a test saying so). Only the
  already-signed-in direction shipped. `login_with_qr_code` takes its OWN
  `ClientRegistrationData`, so the other direction can be added later without
  touching the ordinary flow's metadata or that test.
- **MSC4153 is TWO knobs and one setting.** Setting one alone gives an
  asymmetric client. `CrossSignedOrLegacy`, never `CrossSigned` — the strict
  variant refuses legacy Megolm sessions and would turn existing history into
  UTDs the moment someone enabled a privacy setting. Builder-only in 0.18, so
  restart-to-apply, said plainly.
- **`Recommendation` must cross as a STRING.** ruma models it with one known
  variant plus `_Custom`, so Mjolnir's legacy `org.matrix.mjolnir.ban` lands
  in `_Custom` and an enum comparison reads a real ban list as EMPTY. The
  legacy `org.matrix.mjolnir.rule.*` type names matter for the same reason.
- **A removed policy rule is an empty content object**, and must not parse as
  a rule with an empty entity — which matches nothing under a careful matcher
  and EVERYTHING under a careless one.
- **A desktop "share my location" is "paste a link", so it was removed the
  day it was built.** The first version had latitude/longitude fields
  because a desktop has no better input; the maintainer's reaction was
  "why would I not just send the map link directly?", and the honest answer
  is that they would. Wrapping the link in an `m.location` bought a native
  pin on phones and cost a dialog, a menu item and a code path — not worth
  it. RECEIVING stayed, because that is interop with the phones people
  actually use. Generalise: before building an input for a desktop, ask what
  the user would type into it and whether the composer already accepts that.
- **An unreadable coordinate must be ABSENT, not 0,0.** Zero is a real spot in
  the Atlantic; a UI reading it draws a confident link to the wrong place. And
  0,0 is itself a real place, so the flag exists rather than a magic-value
  check. No embedded map, because tiles put every reader's IP at a tile
  server; the OSM link is built from parsed NUMBERS and `UrlLauncher`'s
  allowlist is untouched.

**Four defects of my own that the existing gates caught, and one they did
not.** Caught: `AppTheme.surfaceRaised` (theme-tokens); `place` and `edit`
absent from the icon-font SUBSET — Icon.qml answers an unknown name with an
empty string, so a wrong name is a silently BLANK glyph, not tofu; and
`QAbstractListModel` has NO `count` in QML — a ListView supplies one, the
model does not — so `model.count` reached `qsTr()` as a plural argument and
three "no QML warnings" suites failed on it.

NOT caught by anything: `app.copyToClipboard` does not exist, and
MediaBrowserRow's "Copy link" called it. It would have failed the first time
anyone used the menu item. Found only by writing the same affordance a second
time. **A QML call to a non-existent `app.` method is invisible until the code
path runs** — which is the gap `tests/QmlComponentLoadTest.cpp` was added to
narrow, and does not close: the load gate proves a component instantiates, not
that its handlers work.

**Two contract tests were failing on anchors an earlier commit in the same
session had removed** (`id: mediaList`, "Media & Files"). Both FAILED rather
than silently covering nothing, which is the guard working — and is why a
scoped scan needs a bound whose absence breaks it.

#### Voice-call constraints that must not soften

Contract in `docs/voice-calls.md`. Inbound call/party ids are sender-chosen
text: bounded in Rust, never logged; remotely triggered work is BOUNDED and
idempotent; ignored senders drop before any state change or send; backlog
suppression defaults CLOSED. SDP transport is OPT-IN end to end, bounded
128 KiB, in the single-shot memory-only `calls::SdpStore` (cap 8, wiped on
sign-out/detach/teardown/reset), never on CallSignal, never logged, never in
QML. TURN comes from `/voip/turnServer` only — credentials cross once,
engine-only, never logged, no third-party STUN. `startVoiceCallButton` is
contract-enforced 1:1-DM-only (a legacy invite rings every room member) and
`enabled: false`, contract-pinned so re-enabling is a decision, because no
answered call has been live-validated. Session-identity tokens ride every
GStreamer callback so a reused engine cannot attribute a closed call's queued
event to the next, and registration sits behind an explicit
`enableCallMediaEngine()` so the test fleet never gains an engine it did not
ask for. Pre-answer candidate buffering and RFC 3264 answer-side Opus pt reuse
came from reading GStreamer sources; `m.call.negotiate` is deliberately
unhandled. Suites: `call-controller` 35, `call-ring-policy` 10,
`call-ui-contract` 6, `call-media-loopback` (SKIPs without plugins),
`calls::tests` 10 — loopback proves the engine, not the network.

#### Packaging, platforms and toolchains

- **`GST_PLUGIN_PATH` is read DURING `gst_init`, once**, and two backends each
  ran their own `gst_init_check` while only one set the bundled path — so a
  package with 25 correct plugins and zero unresolved symbols refused every
  call, and every check passed it, because they proved the payload's SHAPE and
  none proved the app could FIND it. One entry point now does path-then-init
  and both backends are BANNED from `gst_init`. GENERALISE: a feature assembled
  at package time needs a check that runs the SHIPPED artifact and asks whether
  it works. Windows and macOS had shipped for months with no media engine, and
  the honest refusal kept anyone from suspecting packaging.
- **Fedora's mingw GStreamer is a trap** — `gstreamer1-plugins-bad-free` ships
  `libgstwebrtc-1.0-0.dll` (the LIBRARY), not the `webrtcbin` PLUGIN, and no
  nice/srtp/opus/vpx. A `.pc` file and a DLL of the right name are not the
  element; use the upstream MinGW SDK.
- **Capture elements AND their property names are per-platform** —
  `v4l2src`/`pipewiresrc` Linux-only, Windows `ksvideosrc`/`gdiscreencapsrc`,
  macOS `avfvideosrc` (± `capture-screen=true`); `gdiscreencapsrc` takes
  `monitor`/`cursor` where `d3d11screencapturesrc` takes `monitor-index`/
  `show-cursor`, and `gst_parse_launch` fails outright on an unknown property.
  Read them from the shipped plugin's own help strings.
- **A UCRT/msvcrt CRT split, and the probe that cannot see it** — mingw-w64's
  `wchar.h` makes `mbstate_t` a struct under `_UCRT` and an `int` otherwise, so
  `libgstd3d11`/`libgstmediafoundation` import a `std::codecvt` symbol absent
  from the staged libstdc++ (12018 exports; that symbol 0 times, the msvcrt
  spelling once). Windows fails a missing NORMAL import at LoadLibrary — **but
  Wine loads the module anyway**, so a Wine element probe passes a feature dead
  on its target platform. Only a symbol-level walk over the staged closure sees
  it (ZERO unresolved across the 24 shipped plugins). A CRT CHOICE, not version
  drift: a GStreamer bump will not fix it.
- **macOS codesign refuses a plain directory of dylibs inside the bundle** —
  the working shape is a SYMLINK from `Contents/MacOS/gstreamer-1.0` to
  `../PlugIns/gstreamer-plugins`. `macdeployqt` also rewrites the app's
  GStreamer glib/gobject/intl deps into `Contents/Frameworks` out of HOMEBREW,
  splitting the GObject type system, so validation asserts every GStreamer
  library the executable loads is the staged copy.
- **Compile-checking a `Q_OS_WIN`-only TU**:
  `x86_64-w64-mingw32-g++-posix -fsyntax-only` in `debian:13.6-slim` against
  Linux Qt/GStreamer headers plus a stub for `QtGui/qwindowdefs_win.h`, leaving
  three glib LP64/LLP64 `static_assert`s. **Prove it reached the end of the
  file** with a probe TU plus a deliberate undeclared identifier — "no errors"
  can mean "gave up early". nixpkgs' `pkgsCross.mingwW64` gcc does NOT work: it
  wants `mcfgthread/gthr.h`.
- **Windows update paths (fixed in 0.7.3).** MSI failed with **1619** because
  msiexec has its own argument parser and rejects Qt's forward-slash path (`/`
  errored, `\` installed). The portable swap renamed the install DIRECTORY
  while the running helper and its mapped Qt DLLs lived inside it — now
  entry-by-entry, since renaming in-use FILES is permitted on Windows while
  deleting them is not, so a stale backup directory must be cleared or update
  #2 fails. AppImage relaunched the MOUNTED binary, not the `.AppImage` it
  replaced; the app icon was passed only as a theme NAME, which resolves in an
  installed deb/rpm and nowhere else.

#### QML, layout and bindings

- **A 5px transparent grab band shows the wrong surface through it**, and the
  rule centred inside it is then NOT on the boundary. Measured at the
  room-list seam: sidebar `#1B242F`, then THREE native pixels of the window
  ground `#0D1117`, then the rule, then the timeline — the boundary sat three
  pixels before the line meant to mark it, and the sliver read as a gap
  between the panels. Two instances, the shell `SplitView` handle and the
  Room Information resizer. Fix is two parts and the second matters as much:
  paint the band as the surface on ONE side, and anchor the rule to that
  edge — centred, the colour changes in one place and the rule is drawn in
  another. Sample pixels across a seam rather than eyeballing it; the
  numbers name the defect instantly.
- **`Layout.fillWidth` DEFAULTS TO TRUE for a nested layout.** A composite
  control whose root is a `RowLayout` takes a share of its host row unless
  the host says otherwise. Bit twice in one round: `SegmentedControl` in the
  find bar (a ~350 px dead gap at 1600 px wide, scaling with the window), and
  `roomHeaderActions`, where the band's `clip: true` turned the lost width
  into CUT ICONS — the People glyph sliced in half and the Room information
  button gone entirely.
- **A Label with `elide` and no `fillWidth` does not elide.** It sits at its
  implicit width and refuses to shrink, and whatever is beside it pays. The
  room header's identity column could not give up space for that reason, so
  the shortfall landed on the action icons. Third occurrence of this shape.
- **A `color` compared to a hex STRING with `===` is never true.** The
  selected name-colour swatch showed no ring for exactly this. `String(c)`
  first.
- **`SmoothWheelArea` is a non-visual handler and has no `anchors`.**
  `anchors.fill: parent` on one is a LOAD-TIME error that took
  RoomInfoPanel, TimelinePane, MainScreen and Main down together and left the
  app exiting silently. Same family as `font.families`; qmlformat cannot see
  either, and only loading the component does.

- **`Layout.fillWidth` DEFAULTS TO TRUE for a nested layout**, so a composite
  control whose root is a `RowLayout` takes a share of its host row's surplus
  unless the host says otherwise. `SegmentedControl` is exactly that, and its
  own trailing filler — which exists to keep the segments packed — turns the
  stolen width into DEAD SPACE. In the find bar that put ~350 px between
  "History" and the search field at 1600 px wide, and ~130 px at 1100 px:
  **it scales with the window, which is what identifies it.** Invisible in
  every host where nothing follows the control, which is why most hosts never
  noticed and why the fix belongs on the host that has two things after it,
  not on the shared control's default. Found by a screenshot; no test saw it.
- **Two producers, one consumer, two spellings of the same key.**
  `MessageSearchController` is fed by SERVER search (`senderDisplayName`) and
  by the LOCAL index (`senderName`), and reads only the first — so every
  local result reached the find bar with an empty sender, in the list and in
  the row's Accessible name alike. The delegate still BUILT, because the role
  exists on the model; it is the payload key underneath that missed, which is
  the half a `required property` cannot defend (contrast the Qt 6.8
  `roleNames` entry, where the role itself is absent and the delegate count
  goes to zero). GENERALISE: when a controller has more than one producer,
  the key names are a contract between them — assert a VALUE from each
  producer, not that the role exists.

- **A `Popup` with `CloseOnPressOutside` closes on the PRESS, and the button
  that opens it is outside it.** So the icon that opens a picker could never
  close it: the press dismissed the panel and the button's own `onClicked` —
  which arrives on the RELEASE — opened it again, one gesture, close then open.
  Reported as the panel blinking and staying. `if (picker.opened) close()`
  cannot fix it, because the popup layer runs first and `opened` already reads
  false; MEASURED, not assumed — the mutation run pinned the ordering. What
  identifies the gesture is that a panel of that kind was dismissed a moment
  ago and the very next thing is a click on its own button. Callers that are
  not that button (menu entries, demo hooks) must say so, or a displaced menu
  action becomes a toggle that does nothing.
- **`QWindow::show()` FORCES the NORMAL state**, so calling it on a window
  already on screen and MAXIMIZED un-maximizes it — and if `onVisibilityChanged`
  persists the maximized flag, the user's window preference is rewritten as a
  side effect. Reported as "clicking a notification in the bell menu minimizes
  Lightning": the frame snapping back to its small remembered size is what that
  looks like. The tray path had learned this and used `visible = true`; the
  notification path had not, which is the general shape — ONE helper for
  "bring the window forward", never two call sites each deciding. Hidden (back
  from the tray) restores through `visible`; Minimized restores the visibility
  the window last had ON SCREEN, tracked in QML because
  `SettingsManager::initialWindowMaximized` is CONSTANT and cannot answer it
  mid-session; a window already on screen has its visibility left alone.
- **Two wheel branches must agree about SIGN, and one of them reached the
  controller by a different door.** `TimelineScrollController::wheelTargetY()`
  negates its argument INTERNALLY ("angleDelta.y > 0 == wheel up == toward the
  top"), which is right for an ordinary Flickable; the rotated timeline cancels
  that by passing `-angleDelta`, so two negations leave `+(angle/120)*per`. The
  smooth-OFF branch went through `notchDistance()`, which negates nothing, and
  then negated once. Turning smooth scrolling off reversed the mouse wheel.
  GENERALISE: when one code path reaches a helper that applies a convention and
  a second reaches a different helper that does not, the convention is the
  thing to assert on — the regression test compares the two settings rather
  than pinning an absolute direction, because the absolute one is a property of
  the rotation.

- **An imperative write to a bound property destroys the binding** — five media
  cache handlers assigned `Image.source` directly, so the first image that
  loaded was the last that Image showed. Use a `resolveTick` the binding READS
  and the handler bumps (an unused local does create the dependency in Qt
  6.11); with an intermediate `readonly property` it must live in THAT binding
  or it is a silent no-op; key handlers on the cache key.
- **A Popup does NOT consume a press landing on it** — `blockInput()` is FALSE
  when `popupItem == item`, so `modal: true` blocks OUTSIDE presses only; the
  2026-08-18 emoji fix assumed the opposite and was INERT. Sink with an
  all-buttons `MouseArea` in `background:`, never with `z`.
- **`visible: running` on a shared busy indicator is a permanent latch** —
  hosts use `running: visible`, together they cycle, and `visible` is EFFECTIVE
  visibility, so one created under a hidden ancestor latches off silently. A
  component owns its animation; the HOST owns visibility.
- **A defaulted C++ parameter QML must pass fails silently** — `setMentionStyle`
  gained `linkColor`, nothing passed it, and every URL and non-self mention
  rendered in the accent for a round. Pin the arity in a test.
- **In a Qt Quick Layout a child's size constraint may only read a width the
  layout does not compute** — `Layout.maximumWidth: parent.width * 0.7` under
  its RowLayout, and a segment sized against `bubble.width` which in Bubbles
  mode IS the segments' own implicit width, were the whole binding-loop log.
- **An invisible `MenuSeparator` still reserves its height** — QQuickMenu's
  ListView honours each item's height, and MenuSeparator's comes from
  contentItem plus padding regardless of `visible`.
- **A JS array bound to a ListView is a model RESET on every change**, so a
  reorder cannot animate and the delegate holding a live drag is destroyed by
  any refresh: if rows must MOVE, the model must be able to say so. A model
  early-returning on identical rows also announces nothing when only your
  per-row PRESENTATION FLAGS changed, so whoever clears such a flag announces it.
- **`QObject::findChild` cannot reach a `Repeater`'s delegates** — proven with
  a CONSTANT objectName absent from a full `findChildren` dump, ruling out a
  failed binding. Walk `childItems()`.
- **A change handler can run BEFORE the bindings depending on the same
  property** — `onTabChanged` read a binding on `tab`, got the tab being LEFT,
  and moved the selection into the tab just left.
- **Window geometry must be restored in BINDINGS, not `Component.onCompleted`**
  — Qt shows the window during `componentComplete()`, which runs first, so the
  user watches it jump. Read through a CONSTANT property, because a notifying
  one feeds the save back into the binding that produced it. A size below the
  window's minimum is REFUSED on write, since Qt reports transient 0x0/1x1
  while a window is shown, hidden to tray or restored from minimized; only the
  WINDOWED state is stored, maximized as its own flag; `QWindow::show()` forces
  NORMAL, so restoring from the tray sets `visible = true`. `QScreen` stays OUT
  of `SettingsManager` (~20 test targets link it against `Qt6::Core` alone), so
  the still-on-screen test is a BAND along the top of the frame in
  AppController — a window spanned across two monitors is not refused.
- **A guard suppressing a signal for a whole gesture needs something firing at
  the END of it** — `onWidthChanged: if (!SplitView.view.resizing) save()`
  never fired again, because the RELEASE moves nothing.
- **`Qt.quit()` is a REQUEST** — QGuiApplication closes every top-level window
  first and ignores the quit if one refuses, so close-to-tray's
  `close.accepted = false` ate Ctrl+Q, the only way out of that mode. Still
  `Qt.quit()`, not `Qt.exit()`: teardown and apply-on-quit hang off
  `aboutToQuit`.
- **A per-row Loader's item parented to `Overlay.overlay` keeps the Loader as
  its destruction owner**, so delegate churn dereferenced a dangling pointer;
  the `detailsDialogComponent` precedent does NOT transfer, a Dialog being a
  Popup that owns its overlay lifetime. Fixed with ONE shared action bar into
  which rows publish only PRIMITIVES, never a QObject reference;
  `forceReleaseActionBar` exists because the ordinary release refuses while the
  pointer is on the bar — right for a live row, wrong for a dying one.
- **Delegates reach the timeline pane only through their `timelineView`** (the
  rotated Flickable), so a pane-root `openReceiptList` was silently swallowed
  by the delegate's existence guard. Such entry points must be
  property-functions ON the Flickable.
- **The layout faults were one shape: a fixed band in a viewport that got
  smaller**, biting Windows at 125-150% scaling and not Linux, since every
  number is unchanged and two thirds as many fit. `CallHeaderBar` declared no
  `implicitWidth`, so its control row laid out at width 0; the spotlight
  strip's flat 96 px made it the bigger half of a short stage; the call panel's
  flat 45% floor bought the header, the dock and ten pixels of picture. The
  floor now asks the STAGE for `minimumUsefulHeight`, and overlay controls are
  ABSENT rather than squeezed.
- **The hidden-image contract is GEOMETRY, not visibility** — a text row in
  place of a 360x270 picture jumps every message above it, so the placeholder
  fills the media box and contributes no implicit size. An `Image` whose
  `visible` is false still holds its decoded pixmap (clear the SOURCE), and an
  `AnimatedImage` behind an opaque placeholder keeps decoding for nobody.
- **`QScreen::geometry()` is device-independent** — a 4K display at 125% listed
  as 3072x1728, a LABEL defect and not a share defect. Resolve a display by
  DEVICE NAME: Qt's screen order, `EnumDisplayMonitors`' order and
  `gdiscreencapsrc`'s `monitor` index are three unrelated enumerations. A
  Chromium window's caption is the TAB's title, so the owning application comes
  from the executable's VERSIONINFO.
- **The bundled Material Symbols font is a SUBSET** — an unmapped name renders
  as tofu and regenerating needs the network, so pick from the mapped set;
  `IconChromeTest` catches it. A brand mark in the raw accent reads as a status
  light, so `AppTheme.wordmarkBolt` keeps Storm's yellow and blends toward the
  header's secondary ink elsewhere.
- **Colour: measure before believing the symptom.** "Needs more colour" was
  SEPARATION — Storm is the most saturated shell (Lab chroma 27.1 vs Moss Light
  0.8), but every surface step was below 1.25:1 and four elevation roles were
  one literal. Hard ceiling: dark identity inks must clear 4.5:1 on four
  surfaces, capping them at luminance 0.0757, which four 1.25 rungs reach
  exactly. Contrast is NOT sufficient for identity colours — nine sender inks
  were really seven (closest pair dE 5.6/7.4) with every one passing AA — and
  an ink used as the base of its own 14% chip fill is checked against THAT.

#### Timeline, scrolling and navigation

- **The scroll teleport was FOUR paths and the reported one was not the obvious
  one.** (1) A CONVERGENCE-based landing budget re-armed forever, because
  `count`/`layoutRowsAtLastPass` change constantly during a scroll; now an
  absolute ~2 s ceiling. (2) **The actual "about 10 seconds" is a scroll-anchor
  RESTORE** — up to `kMaxNavigationBatches` (8) REAL network paginations before
  `targetLocated`, and cancelling in the VIEW cannot help because the landing
  does not exist yet when the reader starts scrolling; hence
  `PaginationController::cancelNavigation()` from `noteReaderTookControl()`.
  (3) Middle-click autoscroll left `userScrollActive` FALSE for the whole
  gesture (it writes contentY directly, so `moving` stays false), so
  `maintainViewAnchor()` took its IDLE branch and ABSOLUTELY restored contentY;
  pre-existing since v0.7.4. (4) Keyboard paging retired nothing. GENERALISE: a
  convergence budget needs an absolute ceiling, and the reader taking the view
  must reach EVERY layer that can move it.
- **After a model reset `contentHeight` still reads the OUTGOING content's
  height** (old delegates linger until deferred destruction), and
  `contentHeight >= height-1` is degenerately true while `height == 0`
  pre-layout, so the hydration gate opened early. Fixed with
  `presentationGeometryStale` plus a `height > 0` guard; both stale suites then
  went green (`timeline-hydration-qml` 8/0, `timeline-pane-qml` 63/0).
- **`SmoothWheelArea` may use only ScrollTuning's STATELESS `notchDistance()`**
  — `wheelTargetY()` mutates controller state owned by the timeline's
  anchoring. Its `parent as Flickable` was NULL in nine panes, leaving the
  shared area inert; the contract test LISTS unconverted panes.
- **State-flood scroll death is still NOT reproduced.** The proxy-suppression
  fix sketched in its commit message was deliberately NOT shipped — it would be
  a fourth speculative scroll change. The blocker is a real capture: a high
  `worstNotchMs` beside a high `stateRows`. Confirmed inefficiency: a collapsed
  state group drawing ONE summary line still instantiates a delegate per member.
- **GUI stall tracing** (`LIGHTNING_GUI_STALL_TRACE`, `src/app/GuiStallTracer`;
  default 250 ms, env value >= 50 overrides): one line per stall, coarse
  RAII-scope category, literal strings only, never content. `stalltrace::Scope`
  writes a single GLOBAL category, so it is inert off the GUI thread — a
  confidently wrong category is worse than `unattributed`.
- **The rail's drop gesture never once grouped, through THREE rules and two
  rounds that each believed they had fixed it.** All three shared one shape: *a
  reading that moves things while the user is still aiming.* Retired, do not
  re-propose: (a) "the middle 24 px of a row is the group zone" — reaching that
  middle means crossing the near edge first, which reorders, so the row under
  the pointer becomes the DRAGGED entry, never a group target; (b) a 320 ms
  dwell plus a 12 px dead zone; (c) `updateDrag(row, !dwellTimer.running)`,
  where `running` is TRUE for the whole 250 ms the dwell is served, so the
  second sample reordered and then stopped the dwell it was waiting for. The
  rule now is Discord's: the TILE is the group target, the GAP between tiles is
  the reorder target, nothing moves while the pointer is on a tile, and there
  is NO dwell because the geometry carries what the dwell stood in for.
  `updateDrag` was REMOVED rather than shimmed for three exclusive verbs
  `hoverGroup`/`hoverGap`/`clearDropTarget`, and the reorder destination
  derives from a GAP index with the `g > dragRow ? g - length : g` conversion
  the row-index version never had — separately why a one-row hover oscillated.
  **§7's rail paragraph still describes the 250 ms dwell as a second guard;
  this entry is the later record.** GENERALISE (third time): fifteen model
  cases passed through every broken rule because they hand the model a state
  production could not produce. `RailDragQmlTest` drives real mouse events at
  tile centres from real delegate geometry and asserts on the STORE; all six
  cases FAILED on the reverted tree.

#### Models, backends and derived data
- **A STORE-ONLY READ ANSWERS "NOTHING" FOREVER, and every unit test passes.**
  Widgets shipped `Room::get_state_events`, which reads the state store and
  never the network. Widget state reaches that store only if sliding sync asked
  for it in `required_state`, and matrix-sdk-ui 0.18's
  `RoomListService::subscribe_to_rooms` takes room ids ONLY — there is no API
  to extend the list. So the answer was empty for every room, always. Eleven
  unit tests were green; the first live run against a real homeserver reported
  `widgets found: 0` in a room that had four. Same shape as
  `m.room.pinned_events`, same answer as `banner.rs`: store first, one raw
  `/state` request second, on demand. GENERALISE: for any state type sliding
  sync does not carry, "the store said nothing" and "the room has none" are the
  same observable, and only a homeserver can tell them apart.
- **`RoomEventCache::events()` is the IN-MEMORY chunk, not the store.** The
  local search index's first backfill paginated N times and then read the
  events — collecting one page's worth however far back it went, because
  Lightning's own jump-to-live trim shrinks that chunk back to roughly one
  page. Reading after EVERY page is what makes the coverage real, and holding
  ONE cache handle across the loop is what stops the chunk shrinking under the
  walk. Measured in production conditions: the sweep wrote 0 rows and the
  interleaved deep index wrote 33 in the same run.
- **FTS5 is a COMPILE-TIME option of SQLite and is not on by default.**
  sqlite.org: disabled by default for the canonical source tree, enabled for
  the amalgamation's configure script. A distro that forgets `--enable-fts5`
  ships without it silently — the same "graceful absence and silent success are
  identical" shape as the AppImage's missing `libgstopengl`. matrix-sdk's
  `bundled-sqlite` makes it a build-time constant on all six platforms
  (libsqlite3-sys sets `-DSQLITE_ENABLE_FTS5` explicitly) and raises the
  feature floor to 3.50.2; on the system path the floor is Debian's and
  flatpak's 3.46.1. It also means the C++ side must NOT link `SQLite::SQLite3`
  as well, or two SQLite implementations end up in one process and which one
  answers is whichever the linker resolved first, silently, per symbol.
- **`unicode61` cannot segment CJK, so a Chinese search matches NOTHING.**
  Chinese has no spaces between words, so a sentence becomes one token. The
  trigram tokenizer matches substrings in every script at the cost of a hard
  three-character minimum — a visible limit beats an invisible one, and
  `remove_diacritics 2` on a trigram table folds case and accents on both the
  stored text AND the query, so no second folded column is needed. Measured on
  3.50.2, and the alternative is pinned by a test so the choice stays defended.

- **TWO ANSWERS FROM THE SAME SERVER, and the client picked the one nobody
  can see.** The Activity bell counted 25 highlights while no room showed
  unread. The bell's seed is `GET /notifications?only=highlight`, whose
  per-notification `read` flag said false; the room list uses
  `highlight_count` (`max(num_unread_mentions, sync highlight_count)`), which
  said nothing was unread. Neither side was "the bug" — the client believed
  the invisible one. Rule: when two server-sourced answers describe the same
  thing, the surface the user is looking at wins, and the badge is reconciled
  against it (per room, the newest N rows stay unseen where N is that room's
  count). A per-notification flag is also not durable state: the earlier fix
  assumed a read receipt would set it and the report proved it did not.

- **The mock backend being RIGHT is how a backend defect survives** —
  `RoomInfo::childRoomIds` is contractually a Space's DIRECT children and was
  that on the mock and HTTP backends, while the Rust backend filled it from
  `descendants` (the transitive closure), so Channels listed a subspace's rooms
  twice and fifteen model tests passed against the mock throughout. GENERALISE:
  when a field's contract is enforced only by the testable backend, the others
  are undefended. Fixed by reading each Space's own `m.space.child` with the
  spec comparator (`order` first, room id tiebreak, empty-`via` skipped).
- **A design where every view is the same list narrowed by a scope cannot
  express a tab** — Channels collapsed every non-`!` scope to `""`, so the rail
  had one way to say anything that was not a Space and DMs had to ride inside
  EVERY view to stay reachable. Keeping the selection VERBATIM and CLASSIFYING
  it made three real views possible. GENERALISE: when a fix must make every
  surface carry something so it stays reachable, what is missing is a PLACE for
  it to be.
- **A layout that becomes the other layout is not a layout** — Channels scoped
  itself to the active Space, so at Home the host rendered Classic and the user
  silently got the layout they had not chosen. The fix removed the premise: no
  `spaceId` at all, and rooms from the CLIENT rather than the Space-scoped,
  chip-filtered `RoomListModel`.
- **A DM is never scoped by a Space, in any filter** — Matrix gives no way for
  a DM to be a Space's child, and a scoped Space dropped the account-wide
  "Rooms" group, the only place a DM could live. The column can now say a
  filter matched nothing (`matchCount`) without claiming the ACCOUNT is empty.
- **`level = parentSpaceIds.isEmpty() ? 0 : 1` is a two-level approximation
  that looks like a hierarchy** — a three-deep tree rendered as a flat pair of
  indents. Real depth is a breadth-first walk with assign-once semantics, which
  is also what makes it cycle-safe and stable under multiple parents; a Space
  the walk never reaches becomes a ROOT rather than being dropped.
- **Announce only what you actually learned** — `DirectAvatarResolver` cached a
  profile answer only when it carried a NON-EMPTY avatar but announced EVERY
  answer, and its owner rebuilds on that signal and re-resolves, so every
  avatar-less peer and every 404 ran rebuild -> fetch -> answer -> rebuild
  forever: one `/profile` and one full rebuild per round trip, per peer. That
  was the slow account switch, a switch clearing the caches and re-arming it,
  and the comment claiming "this cannot feed itself" was false for the two
  commonest answers. Fixed by caching the NEGATIVE and announcing only a face
  learned; the rebuild is coalesced per event-loop turn and resolves children
  against the map it already built, not `directChildRoomsDetailed`, which
  materialised the whole room list and a fresh hash PER SPACE. No test saw it:
  the fixture's `fetchUserProfile()` returns 0, and the resolver skips its
  pending bookkeeping on op 0.
- **A derivation living privately in one model will be wrong in the next** —
  `RoomInfo::avatarUrl` is empty for most DMs, so the Channels column drew
  initials beside a Home strip showing real faces. A late answer must run a
  `rebuild()`, not a bare `dataChanged`: the rows hold a SNAPSHOT.
- **A room-list indicator must not be allowed to ask** — `read_membership_events`
  falls back to a full `/state` whenever the store holds no live membership,
  the normal state of every idle room, so a self-refreshing call glyph would
  issue one `/state` PER ROOM per rebuild. `RoomCallGlyph` reads only what the
  controller knows, `app.rtc.refresh` is banned by contract test, and the
  honest cost is that a call in a room nothing has poked shows nothing.
- **When a row stops being a `StateChange`, grep every branch testing for
  one** — a new `TimelineEvent::CallEvent` silently un-suppressed call events
  in `NotificationManager` (an EMPTY notification per call) and in the Rust
  backend's activity test (blanking the room-list preview).
- **A collapsed folder cannot be reported on, so it must not be written over** —
  `applyArrangement` takes the whole arrangement in one write and a folder LEFT
  OUT keeps its members; without that, a drag past one would empty it.
- **Per-row state cannot live in the delegate** — a timeline row is destroyed
  the moment it leaves the cache buffer. `MediaVisibilityStore` keys by media
  identity, bounded at 4096, and the cap releases the OLDEST rather than
  refusing the newest: refusing to hide what the user just asked to hide is the
  worse failure.
- **A `json!` past serde_json's macro recursion limit is a compile error naming
  no key** — it points at the macro, not the addition. Hoist any nested object
  into its own `let` first.
- **"Mark as read" was a silent no-op for any room but the open one** —
  `markRoomRead` walked the client's timeline, which on the Rust backend holds
  only the ACTIVE room. `mx_rust_mark_room_read` takes the target from
  `Room::latest_event()` and sends the public receipt AND `m.fully_read`.

#### Matrix protocol, privacy and lifecycle decisions

- **Read the reference implementation; do not infer a wire format.** Raised
  hands: three things would have been wrong by inference — the target is the
  sender's OWN `m.call.member` STATE event, not a timeline message (that scopes
  a hand to one call, since rejoining publishes a new membership); the key is
  TWO code points (U+1F590 + U+FE0F, visually identical to the one-code-point
  form in every editor, so the test asserts the seven UTF-8 bytes); and the
  sender must OWN the membership they annotate, or one user could raise
  everybody's hand. A redaction names only what it removed, so "whose hand went
  down" comes from a locally held `reaction id -> identity` map.
- **Message forwarding** re-sends a NEW, unrelated event with NO relation (no
  Matrix forward primitive), so a forwarded thread reply lands as an ordinary
  message. **Media is RE-UPLOADED, never mxc-copied** — the target's members
  may not be entitled to the source mxc under authenticated media, and an
  encrypted source's `file` block carries per-event keys that must not be
  planted in a room that never negotiated them. Filename and MIME are
  re-originated and sanitized: leaf-only filename, type from MAGIC BYTES — NOT
  `QImageReader::format()` (plugin-backed; WebP lives in qtimageformats, which
  the packaged fleet need not carry) and NOT `gif::validateRasterBytes` (whose
  4096 px / 25 MiB caps would refuse a 5K screenshot). Review caught three
  defects: every image forward would have written decrypted bytes into the
  saved-media store; forwarding to any room but the OPEN one failed 100% of the
  time; a server refusal after dispatch was SILENT.
- **Sliding sync delivers `m.room.pinned_events` ONLY inside a room
  SUBSCRIPTION's required state** — the open room is THE one subscription,
  replacing the previous set, and `stop_sync_and_wait` forgets it so a later
  account cannot inherit it. Relatedly, opening a room notified for its own
  backlog, which arrives as live appends while `roomVisibleAtLatest` is false.
- **Server search covers UNENCRYPTED rooms only, and every surface says so** —
  the server cannot search ciphertext, so in an encrypted room the
  loaded-timeline find is the only search and the find bar offers no History
  segment. The only content sent is the typed term.
- **UIA scrubbing is transit hygiene, never a guarantee** — buffers are zeroed
  best-effort, but on the success path the String moves into ruma's
  `uiaa::Password`, which serializes and drops it without zeroing. A real 401
  surfaces sanitized stage NAMES only, the current device is guarded out of
  per-device sign-out, and **OAuth/MAS accounts have NO password stage**, so
  their buttons open the account-management URL, never a fake prompt.
- **The ignore list is the SDK's read-modify-write of `m.ignored_user_list`,
  never a Lightning-local database** — the SDK clears the whole event cache on
  a list change (timelines reset and refetch; expected), and `senderIsIgnored`
  closes the notification race before the server stops sending. Report is
  `Room::report_content` (requires Joined); `report_room` (MSC4151) and
  `report_user` (absent from the SDK) are deliberately NOT offered, and the
  message menu uses the real room id, never the thread composite.
- **Drafts: encrypted rooms are memory-only, and an UNKNOWN encryption state
  fails closed to memory.** Unencrypted rooms persist account-scoped (LRU 256);
  saves are 1 s debounced, and the debounce is STOPPED before every room/thread
  change with the save reading the still-current key.
- **Smaller protocol decisions.** A refused `get_room_preview` still resolves,
  so Join stays offered; knock withdrawal is a Knocked-state `Room::leave`,
  because the normal leave path filters to Joined; the `/hierarchy`-backed list
  is bounded to 10 pages / 200 rows; `restricted_denied` is classified
  separately, never presented as plain invite-only.
  `mx_rust_set_space_child_suggested` reads the CURRENT `m.space.child`,
  preserves via/order, flips only `suggested`, REFUSES a non-child (empty-via
  included) and never promotes one, and "Suggested" shows only when the
  hierarchy KNOWS. `mediaDownloadUrl`/`mediaThumbnailUrl` were the last surface
  handing unauthenticated `/media/v3` links to the browser and now return empty
  on the Rust backend. A `%n` source string renders its "(s)" literally without
  a loaded translation, so "Seen by N people" is branched explicitly, and
  `tsMs` 0 renders nothing rather than a fabricated time.
- **Rail / Space Home** — a SINGLE tap on a real Space opens Space Home (which
  REPLACES the chat view), there is deliberately NO double-tap, and the ONLY
  expansion trigger is the chevron disc, whose band is excluded from the tile's
  tap. `openSpaceHome` is ordered teardown-first, activation-last because the
  loader instantiates SYNCHRONOUSLY and its handlers point RoomInfoController
  at the Space, and the old order wiped the canInvite/canManageSpaceChildren
  gates afterwards. `spaceJoined` drill-in had been an UNFILTERED listener.
- **A keyed dedup must service ALL claimants** — a saved-media star and a Copy
  image racing on the same uncached image left the star stranded forever. Both
  fetch through MediaBridge with pending-key discipline and magic sniffing (SVG
  refused). Reply-to-image thumbnails register the embedded reply event's media
  under the reply target's event id; the media KEY crosses the FFI, never bytes.
- **Presence is a bounded poll because Sliding Sync delivers NO presence
  events** (MSC4186 has no presence extension): one batch per round (raw ruma
  `get_presence`, <= 40 users, 10 s no-retry timeout so sign-out's task join
  cannot stall), 30 s rounds with rotation past the cap. Transient failures
  KEEP the last known state, forbidden/not_found erase it, and two consecutive
  all-forbidden batches of at least two distinct users each latch "server has
  presence disabled" for the session — a single user's 403 never latches.
  **Unknown renders NOTHING, never a fabricated offline.** Own presence is
  gated by the application-wide `sharePresence` (default ON, global not
  per-account; disabling publishes ONE final offline).
- **Login button naming follows Element classic's actual strings** — both
  "Continue in browser" and "Sign in with SSO" open a browser, so naming the
  MECHANISM told the user nothing; what differs is which authority
  authenticates them. Element's order is `["oauthNativeFlow",
  "m.login.password", "m.login.sso"]`, SSO is primary only when there is no
  password flow, and the homeserver host on the browser button derives from
  what the USER typed, because a server must not choose the words on
  Lightning's own button. **RETIRED 2026-09-02: the i18n catalogs are now
  REFRESHED EVERY ROUND.** They used to be deliberately left alone (~27
  strings behind, and `lupdate` rewrote all 10 files warning "Removed plural
  forms as the target language has less forms"). PR #7 added an eleventh
  language, refreshed catalogs and a CTest gate
  (`catalogsMatchTheCurrentSource`) that extracts the live source with
  `lupdate` and diffs it against every catalog, so a round that adds a
  `qsTr()` and skips the refresh now FAILS `localization`. The refresh loop is
  in `docs/localization.md`. The plural-damage risk is real and unchanged:
  count `<numerusform>` after every refresh — 6 forms for `ar`, 3 for
  `ru`/`lt`, 2 for most, 1 for `zh_CN`/`id`.
- **MediaBridge request priorities** (0 explicit playback/save, 1
  avatars/thumbnails, 2 full static, 3 speculative GIF prefetch), two slots
  reserved for interactive classes, a 15 s starvation bound, temp-file pinning
  while a QMediaPlayer holds the file, queued-speculative dropping on room
  switch, byte-sniff rejection of A/V containers on thumbnail-class results,
  offscreen player reclamation (45 s audio, 90 s video). An SDK receipt MOVE
  arrives as adjacent Set diffs, so the poll drain must not split the pair
  across 100 ms ticks. libpipewire was made resolvable in the dev shell so Qt
  Multimedia uses native PipeWire, not the PulseAudio fallback a captured FLAC
  crash aborted in. Receipt-loss mechanisms Lightning cannot fix without
  patching matrix-sdk-ui 0.18: `docs/receipt-semantics.md`.

#### Testing and harness discipline

- **A gate that PARSES what it is supposed to defend keeps passing after the
  thing moves.** `ThemeTokensTest` read the sender-name ink tables out of
  AppTheme.qml as text. When `userColor()` stopped reading those tables and
  started deriving, the test went on validating dead data — green forever,
  defending nothing. A gate over a derivation has to CALL the derivation.
- **A gate that names only some of the presets defends only those.** The same
  case covered seven of eleven themes, and Storm was not among them. That
  omission is exactly how a derivation which rendered Storm's names pure
  black got past a fully green suite; only putting it on screen showed it.
  Enumerate every preset, or the gate is a sample.
- **Every suite can pass while the feature is broken in the app**, when the
  suites all call the C++ and the break is in the QML that reaches it.
  Deleting two dead colour tables also removed the `_nameGrounds` property
  added beside them; `userColor()` then threw a ReferenceError on every call
  and every name rendered black. Nothing in ctest touches that path.
- **`ydotool` types a backtick as a DEAD KEY.** `` `inline code` `` arrived at
  the server as `ìnline code`, and it looked exactly like the renderer
  failing to style `<code>`. Read the event off the server before blaming the
  client for what a harness typed.
- **Cropping past the edge of a screenshot skews every coordinate derived
  from it.** A crop 640 px wide starting 580 px from the right edge silently
  rescales, and clicks computed from that render land a whole tab off. It
  looked like a tab refusing to switch. Keep crops inside the image.

- **`nearTopControllerDrivenBatchesCompensateImmediatelyNotChained` flakes
  ALONE**, not only under `-j8`. Measured 2026-09-03 on identical code with
  nothing touching the timeline: one failure and two passes in three
  consecutive isolated runs, and three DIFFERENT cases of `timeline-pane-qml`
  failed across four full runs the same day. §16's load-sensitive note already
  covers the suite; this widens it — a lower `-j` is not a reliable workaround,
  so re-run the case before reading a failure as a scroll regression.

- **A case that flips a shared setting must RESTORE BEFORE IT ASSERTS.** Test
  binaries here have no `XDG_CONFIG_HOME` isolation, so every case in one
  binary shares a QSettings file — and an assertion that fires while a setting
  is flipped leaves it flipped ON DISK for every later case and every later
  run. Twice in one session: a `smoothScrolling` case failed during its own
  mutation check and the next full run of `timeline-pane-qml` failed
  `realWheelEventEngagesControllerAndLeavesFollowLatest`, on correct code; a
  `hiddenComposerButtons` case did the same and took three `composer-qml` cases
  down with it. The shape that works is MEASURE, RESTORE, THEN ASSERT — collect
  the observations in a loop, put the setting back, and only then compare. The
  second failure is the expensive one, because it indicts an unrelated area.
- **Retiring a test is part of changing the design it pinned.** Merging the
  GIF and sticker buttons deleted the mono "GIF" keycap a 2026-08-21 audit had
  fixed, so `gifKeycapMatchesItsBorderlessGlyphRow` could not survive as
  written. It was REPOINTED, not deleted: the case now asserts the chip is gone
  and that the button replacing it matches the row's geometry, so the original
  defect still cannot come back.

- **A policy test that invokes the policy directly proves nothing about whether
  production ever reaches it** — recorded three times: the row window shipped
  as a permanent no-op, and the rail drop passed fifteen model cases through
  two successive broken rules. **A regression test that does not fail on the
  old code is decoration**; prove it against the unfixed tree.
- **Mutation-check every new sweep and give it a `found > 0` guard.**
  `everyRuntimeChosenIconNameIsMapped`'s C++ half tried to pattern-match its
  call sites, matched NONE of them, and passed on a deliberately broken tree;
  fixed by moving the names into a `kIcon…` block the sweep finds by prefix and
  BANNING the literal form. The Channels suite was checked the same way against
  two mutations of the FIXED tree (a Space view carrying the DM group again: 4
  failures; a Home repeating every Space: 2).
- **Anchor a source scan on the EXPRESSION, never on a fixed window after a
  name** — fourth occurrence. A case read 700 chars after `function
  clampCallPanelHeight`, and the explanatory comment inside pushed the code to
  offset 1016, so it failed on the FIXED tree. Mutation-check both halves.
- **A negated character class matches newlines** — a comment stripper using
  `(?m)\s//[^"']*$` let `[^"']*` cross newlines, so a trailing `//` comment
  consumed every following line until one ended in a quote, silently weakening
  **every scan in that file positioned after a trailing comment**. GENERALISE:
  a "strip comments" regex is a parser; assert something you KNOW is present
  and watch it fail.
- **An offscreen pixel is evidence only once every animation touching it has
  finished.** Four rounds of probes "proved" the Channels column marked the
  wrong row; every reading came from a `--demo-capture` at the default 1400 ms
  settle, and the rows' 90 ms `Behavior on color` had not advanced, so the grab
  held each row's CREATION-time colour. At 6000 ms every row was correct and
  always had been: NOTHING was wrong with the code, and one speculative fix was
  made on that false reading and reverted. A property probe rendered into a
  LABEL can disagree with the pixel for exactly this reason, which is what
  makes the contradiction diagnosable.
- **Suspect the harness first when a measurement indicts something distant** —
  `startSync()` returns silently before login completes, publishing before
  `Connected` puts no track on the wire, and sampling the SFU before a share
  starts looks like a forwarding failure.
- **Ask an agent what it OBSERVED, not what it concluded**, before writing its
  conclusion into a commit message. A "d3d11 and mediafoundation cannot load"
  finding was a static symbol comparison presented as an observed load failure;
  re-run, both plugins loaded under Wine. The DECISION survived (the absent
  import is real and Wine cannot adjudicate it); the reason did not.
- **Six of seven test failures in one round were bad tests, not bad code** — a
  ban regex matching a token named in a COMMENT, an icon regex matching `State
  { name: }`, three fixed-window source scans defeated by added comments, a
  click helper that never scrolled (Qt DROPS a press outside the window), and a
  reflow guard measuring scene coordinates so a scroll read as a reflow. Ask
  what an assertion meant to measure before deciding who is wrong, and repoint
  it with teeth rather than deleting it. `qmlformat` over `qml/*.qml` is a
  seconds-long parse gate worth running before any build.
- **`QAbstractSocket::waitForReadyRead()` cannot work against a server on the
  SAME thread** — blocking the caller is what stops the listener accepting.
  `SsoCallbackTest::deliver()` ended in `waitForReadyRead(3000)`, so each of
  eleven deliveries burned the full bound: **34.5 s of a 34.5 s suite**, and
  **1.4 s** with the wait removed. Pumping the loop in the helper is WORSE —
  the server then answers before the caller arms its `QSignalSpy`, and
  `QSignalSpy::wait()` waits for a NEW signal, so seven cases fail.
- **Contract-suite duplication detection is mechanical** — extract every
  `contains(QStringLiteral("…"))` needle per suite and rank suite PAIRS by
  intersection. One GIF-picker case had grown to 190 lines, 170 of them
  internals, with **26 of its 41 needles asserted again** in a second suite.
  Left alone deliberately: 23 suites each declare their own `MatrixClient`
  subclass with ~13 identical `override {}` stubs (~300 lines; a shared double
  would be a 23-file change).

#### Performance, disk and logging

- **The first `QVideoSink` in a process costs ~931 ms** (lazy Qt Multimedia
  init including a hardware-decoder probe that fails without VAAPI); the same
  extraction on a worker thread is 1 ms, and the per-frame theory was WRONG —
  `toImage()` is 0.24 ms. Two traps: a plain `moveToThread` leaves a MEMBER
  `QTimer` on the creating thread where Qt refuses to start it, silently
  disarming the 6 s watchdog (make it a CHILD); and the reply becomes QUEUED,
  so `disconnect()` no longer reliably cancels one already posted — session
  isolation keys on `m_posterExtracting`, not on the connection.
  `warmMultimediaBackend()` pre-pays the init off-thread for the first inline
  PLAYBACK, whose sink QML builds on the GUI thread and cannot move.
- **A log line that fires per CALLER does not belong in a default-on category;
  only state transitions do.** `avatarSource()` was the only one of five
  `alreadyPending()` branches that logged, and that branch is reached once per
  caller — O(callers), unbounded in a list. Twelve per-request lines moved to
  `lightning.media.trace`, with one counts-only burst summary once activity
  goes quiet. Separately `Avatar.qml` called the bridge from three triggers per
  instance, one of which (`onSizeChanged`) could not change the request at all,
  because `avatarSource` opens with `Q_UNUSED(size)`.
- **Where the disk goes.** A debug `libmatrix_client_rust.a` is **2.1 GB** and
  every one of the ~146 test binaries links it: `lightning-matrix` alone is 906 MB
  in `build-rust` against 124 MB in `build`, and the test binaries total
  **35 GB** there versus 5.1 GB in the non-Rust tree. With the two
  `incremental` caches (25 GB and 13 GB) the repo was 157 GB; those are pure
  caches, costing only the next build's incremental state. `nix store gc` freed
  **63 GB** — pin the dev shell FIRST (`nix develop --profile <path> -c true`,
  registering a root under `/nix/var/nix/gcroots/auto/`) or the GC takes the
  whole Qt/Rust toolchain with it. `split-debuginfo = "unpacked"` in
  `[profile.dev]` is **NOT applied**: a build-config decision for Rokas, and
  `[profile.release]` (which packaging uses) is unaffected either way.

#### Live status for these rounds

**NOT TESTED** live, and do not promote any of them: the Sable-parity round
(three Channels views, member column, call glyph); the 2026-08-19
design-deficit pass (CTest 134/134 both trees is a build result, not a GUI
one); the Element-parity round (`space-child-suggest` 4,
`element-parity-contract` 5); discovery / search / UIA / moderation / drafts;
pins / power levels / join rule / alias (real `m.room.pinned_events` round
trips, Element interop, a homeserver accepting or refusing a write, alias
publication, and the on-screen look of any of it); Matrix presence; the
2026-08-11 media/UX round; the tester report #2 round on Windows; any call
PLACED from a Windows or macOS package; an ANSWERED legacy 1:1 call.
**Live-validated**: MatrixRTC audio, camera and screen share both directions
against Element, and the Windows camera and window share on a packaged build —
CLAUDE.md §16's "Live validation: what Rokas has actually confirmed"
carries the full confirmed list, and stayed there.

