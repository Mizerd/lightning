# Matrix widgets in Lightning

Implemented 2026-09-03/04. Lightning **lists** a room's widgets, resolves and
validates their addresses, tells the user what each one will learn about them,
and **opens them in the user's own browser**. It does not embed them.

That is a decision with evidence behind it, and this document is the evidence —
both for why embedding was not done, and for what would have to change before
it could be.

## What a widget is

A page a room advertises: a Jitsi call, an Etherpad, a dashboard. It lives in
the room's own state, and **any member with permission to write that state can
put one there** — a moderator in most rooms, everybody in some. Every widget is
attacker-chosen until proven otherwise.

Widgets are **entirely unspecified in released Matrix**. MSC1236 is a GitHub
issue whose body links a 2018 Google Doc; MSC2764 ("Widgets, the spec") is
closed and unmerged, and its `travis/widgets` branch holds the only
machine-readable schema there is. What exists is what Element deploys.

## What Lightning implements

* **Discovery.** `im.vector.modular.widgets` room state, and `m.widget` read as
  a courtesy (it has never been deployed — element-web#13111 has been open
  since 2020). Read from the state store first and from a raw `/state` request
  second: `Room::get_state_events` never touches the network, and widget state
  never reaches the store, so the second half is not a fallback but the only
  path that works. See Live validation for how that was found.
* **Identity from the envelope.** The widget id is the **state key** and the
  creator is the **sender**. The content carries `id` and `creatorUserId` too —
  Element does not even write them and reconstructs both from the envelope —
  and trusting them would let a widget claim an id belonging to another, which
  is how a remembered consent gets applied to the wrong page.
* **Liveness.** A widget is live when `type` and `url` are both present and
  non-empty. **An empty content object is the tombstone**: it is how Element
  removes a widget, and reading it as a widget would resurrect every widget
  anybody ever deleted.
* **Templating.** All ten `$matrix_*` / `$org.matrix.*` variables, each value
  unconditionally percent-encoded. **Both spellings of the device id** are
  substituted: matrix-widget-api says `$org.matrix.msc3819.matrix_device_id`
  and matrix-sdk 0.18 writes `$org.matrix.msc2873.matrix_device_id`, and a
  widget expecting the other one would otherwise receive a literal. Names are
  substituted longest-first, so a short name cannot eat the prefix of a longer
  one (Element has exactly this hazard).
* **Validation, in MSC2764's own order.** Refuse a raw URL whose **authority**
  contains a variable *first* — templating is textual, so
  `https://$matrix_display_name.evil.example/` makes the ORIGIN depend on the
  user's own profile, and an origin has to be a property of the room's state
  rather than of who is looking. Substitute *second*. Validate the **result**
  *third*: https only, no userinfo, non-empty host. Validating before
  substitution would pass a URL whose final scheme is `javascript:`.
* **Consent before opening**, listing what *this* widget receives — derived
  from its own URL, so a widget using no variables says only that the site
  learns you connected to it. A notice that overstated would be dismissed
  unread, and then it would protect nobody.
* **A refused widget is still shown**, with the reason. Dropping it would make
  "Lightning will not open this" and "this room has no widgets" the same
  observable — the failure shape §16 records over and over.

Opening goes through `WidgetController::openWidget(row)` — **by row, never by
address**. No QML path, present or future, can hand the desktop a URL that did
not come from the model's own validated list, and the row's URL is re-checked
against `lightning::urls::isOpenableExternally` on the way out.

## Why not embedded

Embedding needs Qt WebEngine. Each of these was measured or read from source,
not assumed:

1. **Windows cannot build it.** The Windows package comes from Fedora's
   `mingw64-qt6-*` RPMs and there is no `mingw64-qt6-qtwebengine`; Chromium
   requires MSVC. Widgets would ship honestly unavailable on Windows, or the
   whole Windows Qt acquisition changes.
2. **Flatpak could only ship it unsandboxed.** Flatpak's unconditional seccomp
   blocklist EPERMs `unshare`, `setns` and `clone(CLONE_NEWUSER)`, so
   Chromium's user-namespace probe fails and the process hits `LOG(FATAL)`.
   The QtWebEngine BaseApp's own wiki instructs `QTWEBENGINE_DISABLE_SANDBOX`.
   **Untrusted web content running unsandboxed beside Megolm keys is not
   something CLAUDE.md §6 permits.**
3. **It changes the whole application's rendering.**
   `QtWebEngineQuick::initialize()` forces the Qt Quick scenegraph to OpenGL
   for the entire process, and must be called before `QGuiApplication`. For a
   client whose timeline is a hand-tuned rotated Flickable with a documented
   frame-cost history, that is a cross-cutting change, not a footnote.
4. **Payload.** ~429 MB (`libQt6WebEngineCore.so.6` is 343 MB unstripped,
   ~242 MiB stripped; resources 15 MB; locales 44 MB). deb ≈262 MB installed.
5. **The SDK's driver has a capability bypass.** `MatrixDriver::send`
   (matrix-sdk 0.18 `widget/matrix.rs:172-178`) short-circuits on a `redacts`
   field **before** any type dispatch, while the capability check upstream
   inspects only `type`, `state_key` and `msgtype`. So a widget granted
   `send.event:m.room.message#m.text` can redact any event it can name. Any
   embedded implementation would have to wrap or compensate for that.
6. **The reference transport does not authenticate its peer.**
   matrix-widget-api's `PostmessageTransport` has `strictOriginCheck = false`
   by default and nothing sets it; `event.source` is never compared to the
   iframe; and the only inbound discriminator is the attacker-supplied
   `widgetId`, trust-on-first-use. MSC2764 already *requires* what the
   reference library does not do.
7. **Wayland risk.** Qt 6.9 has reports of black/transparent page content on
   Wayland; the documented escape hatch is misspelled in the shipped 6.11.1
   binary (`QTWEBENGINE_FORCE_FORCE_USE_GBM`). Lightning is Wayland-first on
   NVIDIA. Undetermined for 6.11, and it would need a real trial.

What opening in the browser gives up is the widget postMessage API — a widget
cannot read the timeline, send on the user's behalf, or stay on screen. What it
buys is a containment boundary the operating system already enforces: a
separate process holding none of this one's tokens, keys or memory. For the
widgets people actually meet, that is the whole of the feature anyway.

**NeoChat reaches the same conclusion** — its `WidgetsPage.qml` calls
`Qt.openUrlExternally()`. nheko has no WebEngine code at all.

## If embedding is ever revisited

The prerequisites, in order:

1. A Windows Qt story that includes WebEngine (MSVC toolchain, or drop widgets
   on Windows and say so).
2. A Flatpak answer that is not "disable the sandbox" — flatpak#5921
   (`--allow-userns`) is still open.
3. A decision on the scenegraph change, with a `QSG_RENDER_TIMING` capture of
   the timeline before and after.
4. The WebRTC triad copied exactly: `LIGHTNING_ENABLE_WEBENGINE` (auto),
   `LIGHTNING_REQUIRE_WEBENGINE` (packaging), `HAVE_LIGHTNING_WEBENGINE`, a
   `--widgets-status` preflight, and a QML `supported` flag so the button is
   absent rather than dead. **Plus shipped-artifact checks before the first
   pipeline run** — a missing `QtWebEngineProcess`, `resources/*.pak` or
   `icudtl.dat` is `qFatal()`, and the project's own repeated lesson is that
   graceful absence and silent success look identical.
5. A shim page that iframes the widget rather than loading it at top level, so
   `sandbox` exists at all; an off-the-record profile destroyed when the widget
   closes; **every Group A Qt signal connected and accepted-then-dropped**
   (`javaScriptDialogRequested`, `fileDialogRequested` and especially
   `authenticationDialogRequested` — unconnected, Qt shows its own dialog, so
   leaving them alone hands an untrusted page a working credential prompt
   inside the client); `permissionRequested` connected and denied (unconnected
   is not deny — the request dangles); and any QWebChannel facade in
   `ApplicationWorld`, because a main-world one is fully reachable by the page
   via `qt.webChannelTransport`.

## What opening externally does not do

Worth stating plainly rather than discovering:

* **`data` keys are not templated.** Element treats every key of the widget's
  `data` object as an extra `$variable`, which is how its Jitsi wrapper gets
  `$conferenceId` and `$domain`. matrix-sdk does not, and neither does
  Lightning — the substitution is a bare global replace with no word boundary,
  so a `data` key sharing a prefix with a longer name silently eats it. A
  widget relying on `data` templating will open with those variables
  unsubstituted.
* **An Element-authored Jitsi widget may not work.** Element replaces the
  widget URL at render time with its own local `jitsi.html#` wrapper and puts
  the conference details in the fragment. The URL actually stored in room
  state varies by Element version, and where it points at a wrapper Lightning
  cannot serve, opening it in a browser will not produce a call. A widget whose
  stored URL is a real meeting address works.
* **No postMessage API**, so no widget can read the timeline, send on the
  user's behalf, stay on screen, request capabilities, or use OpenID. That is
  the trade named at the top of this document.

## Not implemented, deliberately

Account-level widgets (`m.widgets` account data), modal widgets (MSC2790),
`m.stickerpicker` and integration managers. Each needs the postMessage API to
mean anything, and none of them can work through an external browser.

## Live validation

**PASS**, against `matrix.smetonis.net` with a fixture room carrying one real
widget and three hostile ones:

    [live] widgets found: 4
    [live]   "evil-authority" kind="custom" openable=false refusal="templated_authority"
    [live]   "evil-scheme"    kind="custom" openable=false refusal="not_https"
    [live]   "evil-userinfo"  kind="custom" openable=false refusal="has_userinfo"
    [live]   "jitsi"          kind="jitsi"  openable=true  refusal=""

The tombstone (`{}`) is correctly absent. The Jitsi widget resolves to
`https://meet.example.org/!room%3Aserver?user=%40user%3Aserver&theme=storm` —
every variable substituted, the colon and the `@` percent-encoded, and the
theme carried through.

**That run is also what found the one real defect in this feature.** The first
live attempt reported `widgets found: 0` in a room that had four, and every
unit test still passed: `Room::get_state_events` reads the STATE STORE and
never the network, widget state only reaches that store if sliding sync asked
for it in `required_state`, and matrix-sdk-ui 0.18's
`RoomListService::subscribe_to_rooms` takes room ids only — there is no API to
extend the list. So the store answer is empty for every room, always. The fix
is banner.rs's shape: store first, one raw `/state` request second, on demand.

Still NOT TESTED: the QML surface. The list, the consent sheet and the refusal
rows have not been seen on screen, and **on the mock backend they cannot be** —
`RoomInfoController::supported()` reads `supportsRoomManagement()`, which only
the Rust backend implements, so Room Information never opens in the screenshot
demo and the widget list has no host there. A `room-widgets` demo scenario
exists and stops at that gate. Enabling the capability on the mock was
considered and rejected: `ConversationController` and `UserSearchModel` read
the same flag and would then claim support they do not have.

What HAS been checked, by inspection rather than by eye: every key the Rust
bridge emits (`id`, `creator`, `kind`, `name`, `url`, `refusal`, `discloses`)
is one `WidgetController` reads, and every `required property` in the list
delegate (`name`, `kind`, `refusal`, `openable`) is a declared role. That is
the defect class a screenshot of the SEARCH surface did catch this round — a
producer and a consumer disagreeing on a key — so it was worth ruling out
here directly.
