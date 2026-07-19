# Package layout

Lightning's CMake install target places `matrix-client` in `bin`. QML files
and the emoji catalogue are Qt resources compiled into that executable, and
the Rust SDK bridge is linked statically. The deployment project adds:

```text
/usr/bin/matrix-client
/usr/share/applications/lightning.desktop
/usr/share/metainfo/lightning.metainfo.xml
/usr/share/licenses/lightning/copyright
/usr/share/doc/lightning/LICENSE
/usr/share/doc/lightning/README.md
```

Qt, libsecret, SQLite and other system libraries remain dynamically linked and
are declared as native DEB/RPM dependencies.
Since Lightning 0.7 the source supplies the application icon set
(`/usr/share/icons/hicolor/<size>/apps/lightning.png`) and its own desktop
entry through `cmake --install`; packages ship them as staged.

## Additional single-file formats (since 2026-07-19)

The Flatpak bundle, AppImage, and snap carry the same application payload:

- **Flatpak** (`net.smetonis.Lightning`): built from the pinned source inside
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

GIF provider keys follow the existing pattern in every format: build-only
environment names, embedded at configure time, header scrubbed, `--gif-status`
/ `--gif-selftest` enforced in validation for publishing pipelines, and keys
never written into a manifest, snap.yaml, AppDir, or log.
