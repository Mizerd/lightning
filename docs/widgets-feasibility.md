# Matrix widgets: feasibility audit (2026-09-02)

Status: **audit only — not implemented.** Embedding widgets (Element Call,
Jitsi, Etherpad, custom MSC1236 widgets) in a sandboxed web view is
feasible in principle but needs three decisions that are not the kind a
feature round may take on its own: a new heavyweight dependency (Qt
WebEngine, i.e. Chromium), a Cargo feature flag on the SDK, and a Windows
build story that the current toolchain cannot provide. Everything below was
verified against the repository, the vendored crate sources and the deploy
repository; nothing was built.

## What exists already

* **The SDK ships a widget driver.** matrix-sdk 0.18 has `src/widget/`
  behind the `experimental-widgets` feature (`Cargo.toml:91-94`). Its
  shape is FFI-friendly: `WidgetDriver::new(WidgetSettings)` returns a
  driver plus a `WidgetDriverHandle` whose `recv()`/`send()` move raw JSON
  strings — exactly the `postMessage` payloads a web view exchanges — and
  `WidgetDriver::run(room, capabilities_provider)` performs the whole
  MSC2762/MSC2871/MSC2931/MSC2974/MSC2876/MSC3819/MSC4039 protocol. The
  `CapabilitiesProvider` trait (`capabilities.rs:32-40`) is the consent
  hook: it receives the requested capabilities and returns the granted
  subset, as a future, so a C++ consent dialog can be awaited across the
  FFI. `WidgetSettings::generate_webview_url` does the `$matrix_user_id`
  and friends substitution; `new_virtual_element_call_widget` builds an
  Element Call widget from a base URL.
* **The only new crate the feature pulls in is `uuid`**, which is already in
  `rust/Cargo.lock` (1.23.4) and vendored, so `--offline --locked` should
  still resolve. This must be proven with an explicit build before it is
  claimed.
* **Room state reads and the ingest shape are established.**
  `im.vector.modular.widgets` / `m.widget` follow the same
  `get_state_events(StateEventType::from(..))` path as sticker packs and
  MatrixRTC memberships, and a `room_widgets_changed` poke plus a fetch is
  the `room_pinned_changed` shape. The sliding-sync `required_state` list
  must gain the widget event type or the read returns empty (the exact
  failure recorded for `m.room.pinned_events`).
* **The UI slots are clear** and layout-independent: a header
  `IconButton` beside `threadsViewButton` in `qml/TimelinePane.qml`, and a
  right-side panel hosted the way `ThreadPanel` and `RoomInfoPanel` are.
  Both layouts share that pane.

## What blocks it

1. **No Qt WebEngine anywhere in the toolchain.** `nix/devShells.nix` and
   `nix/package.nix` list qtbase, qtdeclarative, qtsvg, qtwayland and
   qtmultimedia only; CMake requests no WebEngine component. Adding it is
   a build-dependency decision (Chromium) with a measurable cost in build
   time, closure size and security surface.
2. **Windows cannot cross-compile it.** The Windows package is built from
   Fedora's `mingw64-qt6-*` RPMs (`packaging/windows/Dockerfile:53-59`)
   and there is no `mingw64-qt6-qtwebengine`. Windows would either need a
   different Qt acquisition (MSVC / official installer, a new pipeline) or
   ship with widgets honestly reported unavailable.
3. **AppImage staging is manual.** `QtWebEngineProcess`, the resource
   `.pak` files, ICU data and locales are not discoverable from ELF
   `NEEDED` entries or QML imports, so linuxdeploy will not stage them;
   `validate-appimage.sh` would need a new required-payload block and the
   AppRun hook a `QTWEBENGINEPROCESS_PATH`. Chromium's sandbox inside an
   AppImage typically also needs a SUID helper or `--no-sandbox`.
4. **The Flatpak runtime was not measured.** `org.kde.Platform//6.9` is
   expected to carry QtWebEngine but no runtime is installed here to
   confirm it.
5. **A recorded project constraint says "no WebView".**
   `tests/StormBandQmlTest.cpp:137-139` and `CMakeLists.txt:1608` encode
   it (scoped to `StormBand.qml`). Reversing it is an owner's decision.
6. **`WidgetSettings` cannot be built from a room state event.** The SDK
   has a literal TODO for it, and ruma 0.34 has no widget content type, so
   parsing MSC1236 widget state (including the `$matrix_*` template
   contract) is Lightning's own code.
7. **Security rules a renderer must honour.** CLAUDE.md §6 forbids handing
   authenticated media URLs or access tokens to anything outside the
   controlled media bridge; a web view is such a target. The widget API's
   identity primitive is `get_openid`, never the access token, and media
   must go through the driver's own `download_file` path.

## Every place a WebEngine dependency would have to land

| Artifact | Where |
|---|---|
| deb | `lightning-deploy/scripts/build-deb.sh` `QML_DEPENDS` (add `qml6-module-qtwebengine`, `libqt6webenginequick6`) |
| rpm | `packaging/rpm/lightning.spec` `Requires` (add `qt6-qtwebengine`) |
| flatpak | manifest `finish-args` (Chromium zygote needs, verify), runtime check |
| AppImage | `scripts/build-appimage.sh` hand-staging block + AppRun hook; `scripts/validate-appimage.sh` required list |
| Windows | `scripts/stage-windows-runtime.py` (QML entry + process/resources), `packaging/windows/Dockerfile` (no package exists), `scripts/validate-windows-artifacts.sh` |
| macOS | `scripts/build-macos.sh` `QT_LINKED_MODULES`, `scripts/validate-macos-artifacts.sh` framework loop |
| CI images | `.gitlab-ci.yml` deb/rpm/appimage build images (`qt6-webengine-dev` / `qt6-qtwebengine-devel`) |
| configure | `scripts/configure-build.sh` (`-DLIGHTNING_REQUIRE_WEBENGINE=ON` beside the WebRTC flag) |

## Recommended shape, if approved

* Copy the WebRTC triad: `LIGHTNING_ENABLE_WEBENGINE` (auto),
  `LIGHTNING_REQUIRE_WEBENGINE` (packaging), `HAVE_LIGHTNING_WEBENGINE`
  compile definition, sources appended under the guard, a `--widgets-status`
  preflight that names the build's answer, and a QML `supported` flag so the
  button is absent rather than dead.
* Rust: enable `experimental-widgets`, add `mx_rust_widget_open(room,
  widget_json, op)` / `mx_rust_widget_post(handle, json)` /
  `mx_rust_widget_grant(handle, capabilities_json)` / `mx_rust_widget_close`
  around `WidgetDriver`, with the consent future resolved by the grant call.
* C++: a `WidgetController` owning one driver per open widget, a
  capability-consent dialog rendering the MSC2762 strings as human
  sentences, a strict URL policy (https only, no userinfo, origin pinned to
  the widget's declared URL, no navigation off-origin), and the web view
  created with JavaScript enabled, local storage off, no plugins, no
  downloads, no geolocation, no clipboard access.
* Packaging in the same round as the code, with the shipped-artifact checks
  extended before the first pipeline run (the lesson of every "plugin an
  element loads for itself" entry in CLAUDE.md §16).
