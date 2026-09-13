# Package layout

Lightning's CMake install target places `matrix-client` **and**
`lightning-updater` in `bin`. QML files and the emoji catalogue are Qt resources
compiled into the application executable, and the Rust SDK bridge is linked
statically. The deployment project adds:

```text
/usr/bin/matrix-client
/usr/bin/lightning-updater
/usr/share/applications/lightning.desktop
/usr/share/metainfo/lightning.metainfo.xml
/usr/share/licenses/lightning/copyright
/usr/share/doc/lightning/LICENSE
/usr/share/doc/lightning/README.md
```

Qt, libsecret, SQLite and other system libraries remain dynamically linked and
are declared as native DEB/RPM dependencies (derived from **both** executables
by `dpkg-shlibdeps`).

## The update helper

`lightning-updater` is a second, deliberately tiny executable that links only
Qt6::Core: it performs the one install step that cannot happen while Lightning
is running, and its link line is its isolation guarantee — no network, no
Matrix, no store, no tokens. It never downloads anything and only ever receives
an artifact Lightning has already verified.

**Every format must ship it**, or the in-app updater has nothing to hand the
verified download to and the feature is a silent no-op. That is enforced rather
than assumed:

- `configure-build.sh` fails if the staged tree lacks it (covers deb, rpm,
  AppImage and, through the AppImage's AppDir, the snap);
- `packaging/rpm/lightning.spec` lists it under `%files` — omitting it does not
  ship a smaller RPM, it aborts `rpmbuild` on an unpackaged file;
- `build-appimage.sh` passes it to `linuxdeploy --executable`, so its libraries
  and `$ORIGIN` RPATH are handled — an unlisted binary would be copied along and
  then fail to start on a host without Qt6;
- `stage-windows-runtime.py` stages it and seeds it into the DLL dependency
  walk, so all three Windows packages carry it;
- each `validate-*.sh` asserts it is present in the built package.

Flatpak and Snap ship it too (it comes from the same install rule) but never run
it: those installs are updated by their own ecosystems, and Lightning refuses to
self-install there.
Since Lightning 0.7 the source supplies the application icon set
(`/usr/share/icons/hicolor/<size>/apps/lightning.png`) and its own desktop
entry through `cmake --install`; packages ship them as staged.

## Additional single-file formats (since 2026-07-19)

The Flatpak bundle, AppImage, and snap carry the same application payload:

- **Flatpak** (`org.lightning_matrix.Lightning`): built from the pinned source inside
  the org.kde.Platform 6.9 sandbox (Flatpak binaries must link the runtime's
  Qt); desktop file, icons, and metainfo exported under the app id. Finish
  args: network, Wayland + fallback X11, dri, pulseaudio,
  org.freedesktop.secrets, Notifications.
- **AppImage**: the DESTDIR-staged `/usr` tree plus bundled Qt libraries,
  platform plugins (wayland, xcb deps, offscreen) and the dynamic QML modules
  (same list the deb declares), assembled with pinned linuxdeploy releases.
  Unsandboxed by design; only base-system libraries (glibc, GL, X11) are
  expected from the host.
- **Snap**: the AppImage job's bundled AppDir packed with mksquashfs plus
  `meta/snap.yaml` (base core24, strict confinement declared, plugs incl.
  password-manager-service for libsecret). snapcraft/snapd cannot run on the
  Docker fleet, so CI validation is the structural equivalent (unsquash,
  metadata checks, launcher run, payload audit) — a live `snap install` test
  requires a real snapd host.

  **AND STRUCTURAL VALIDATION IS NOT ENOUGH, proven 2026-09-13.** The first
  time this snap was run the way a user runs it — real snapd, Ubuntu 24.04,
  strict confinement — it failed three ways before showing a window, and
  every check in CI was green throughout. Under strict confinement `/usr` is
  the BASE SNAP's, so anything the host has is unreachable unless an
  interface bind-mounts it, and snapd remaps `XDG_RUNTIME_DIR` to
  `$XDG_RUNTIME_DIR/snap.lightning`, so every socket the session leaves in
  the real runtime dir sits one level up. The launcher now bridges the
  compositor, PipeWire and Pulse sockets, stages xkb keymaps and the
  fontconfig configuration, and takes graphics from the `gpu-2404` content
  snap. Account: `docs/round-history.md`, 2026-09-13.

  **Four interfaces are MANUAL on install and the snap is degraded without
  them.** None auto-connects (content auto-connection needs the publishers to
  match; the other three are privileged), so a user who does nothing gets no
  microphone, no camera, no saved session and no hardware GL — and on the
  software renderer Qt Quick draws no call or screen-share video at all:

  ```sh
  snap connect lightning:gpu-2404 mesa-2404:gpu-2404
  snap connect lightning:audio-record
  snap connect lightning:camera
  snap connect lightning:password-manager-service
  ```

  These need a store snap-declaration granting auto-connection before the
  snap is fit to publish.

GIF provider keys follow the existing pattern in every format: build-only
environment names, embedded at configure time, header scrubbed, `--gif-status`
/ `--gif-selftest` enforced in validation for publishing pipelines, and keys
never written into a manifest, snap.yaml, AppDir, or log.
