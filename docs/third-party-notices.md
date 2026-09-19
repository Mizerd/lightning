# Third-party notices

Lightning itself is **GPL-3.0-or-later** (see [`LICENSE`](../LICENSE)). It
contains no proprietary components and no commercial dual-licensing.

This file records the third-party software and assets that Lightning links
against or redistributes. It is written for the reader who wants to know what is
inside a released package — most importantly the Windows packages, where the
dependencies are shipped rather than resolved by a distribution.

Nothing listed here is authored by Lightning. Upstream binaries are redistributed
exactly as their projects built them and are never re-signed or re-labelled as
Lightning; see [`docs/windows-signing-inventory.md`](windows-signing-inventory.md).

## Runtime dependencies (linked)

| Component | Licence | How it ships |
|---|---|---|
| **Qt 6** (Core, Gui, Network, Qml, Quick, QuickControls2, Multimedia, Svg, plus platform/image/TLS/SQL plugins) | LGPL-3.0 (open-source edition) | Linux: distribution packages. Windows: redistributed DLLs and plugins, unmodified, dynamically linked |
| **matrix-rust-sdk** (`matrix-sdk`, `matrix-sdk-ui`, `matrix-sdk-base` 0.18) and its Rust dependency tree | Apache-2.0 | Statically linked into the Rust bridge; exact versions pinned in [`rust/Cargo.lock`](../rust/Cargo.lock) |
| **FFmpeg** runtime libraries (`avcodec`, `avformat`, `avutil`, `swresample`, `swscale`) | LGPL-2.1-or-later | Windows only, behind Qt Multimedia's FFmpeg backend; redistributed unmodified |
| **MinGW-w64 runtime** (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) | GCC Runtime Library Exception / MinGW-w64 licences | Windows only; the compiler runtime for the cross-built binary |
| **OpenSSL / rustls** and their transitive crates | Apache-2.0 / MIT / ISC as applicable | Per `rust/Cargo.lock`; TLS on Windows also uses the Schannel Qt TLS backend |
| **SQLite** | Public domain | Via Qt's SQL driver and the SDK's store |
| **libsecret** | LGPL-2.1-or-later | Linux only, for OS keyring storage. Windows uses the system Credential Manager |
| **GStreamer** (core, `-base`, `-good`, `-bad`) and `libnice`, `libsrtp2`, `libvpx`, `opus`, `orc`, `webrtc-audio-processing` | LGPL-2.1-or-later (libnice also MPL-1.1; libvpx/opus/libsrtp BSD) | The voice/video call media engine. deb and rpm declare them as dependencies; the AppImage, snap, Windows packages and the macOS bundle BUNDLE them |
| **glib / gobject / gio**, freetype, fontconfig, harfbuzz, graphene, ICU, libpng, libjpeg, libtiff, libwebp, PCRE2, expat, libffi, zlib, bzip2 | LGPL-2.1-or-later, MIT, BSD, FTL and similar, per project | Pulled in as dependencies of Qt and GStreamer; bundled by every self-contained format |

The complete, authoritative Rust dependency list with exact versions is
[`rust/Cargo.lock`](../rust/Cargo.lock). It is lock-file controlled and is not
updated incidentally.

## What each package carries, and where it is

A self-contained package redistributes these libraries, so the licence text has
to travel with them. The AppImage, snap and macOS rows below describe the
**next** release: the licence staging for them landed after 0.9.8, and the
published 0.9.8 AppImage and snap carry only Lightning's GPL-3 and
gst-plugins-good while the published 0.9.8 macOS bundle carries none at all.

| Package | Licence texts it carries |
|---|---|
| **deb / rpm** | `/usr/share/doc/lightning/copyright` and `LICENSE`. Nothing third-party is bundled — GStreamer and Qt are declared dependencies, so the distribution already ships their licences |
| **Flatpak** | Lightning's own; GStreamer and Qt come from the KDE runtime |
| **Windows** | `Lightning/licenses/`: Lightning's GPL-3, seven Qt directories, and thirteen under `lightning-gstreamer` (gstreamer-1.0, `-base`, `-good`, `-bad`, gst-plugins-rs, libnice, libsrtp, libvpx, opus, orc, zlib, webrtc-audio-processing, mingw-runtime). Plus `runtime-dependencies.json`, recording the exact set of PE files staged and their import graph |
| **AppImage / snap** | Lightning's GPL-3; `usr/share/doc/<pkg>/copyright` for ~235 packages, deployed by **linuxdeploy's own** `dpkg-query` pass and present since long before this round; and `usr/share/licenses/third-party/<pkg>.copyright`, a second pass derived from the payload that additionally covers the ten packages hand-staged past linuxdeploy's excludelist |
| **macOS** | `Contents/Resources/licenses/`: Lightning's GPL-3 and the **vendored** gst-plugins-good text from this repository. Whatever the upstream GStreamer framework itself carries is REPORTED in the job log and not yet asserted — nobody has listed that tree, and the build succeeds with a count of zero |

**Known gaps, stated rather than glossed.** The AppImage's FFmpeg closure
(`libavcodec`, `libavformat`, `libavutil` and the codec libraries they use) is
built from Debian's GPL-enabled FFmpeg and is therefore **GPL-2-or-later**, not
LGPL. The Windows package does not yet carry licence text for FFmpeg or for the
Qt/GStreamer support libraries listed above, and the macOS bundle carries only
part of its set. Both are tracked in
[`docs/open-items.md`](open-items.md); the corresponding-source offer those
GPL libraries require is an open decision recorded there.

## Bundled assets

All bundled fonts are open-licensed; the licence text ships next to each one in
[`data/fonts/`](../data/fonts).

| Asset | Licence | File |
|---|---|---|
| Manrope | SIL Open Font License 1.1 | `data/fonts/OFL-Manrope.txt` |
| JetBrains Mono | SIL Open Font License 1.1 | `data/fonts/OFL-JetBrainsMono.txt` |
| Space Grotesk | SIL Open Font License 1.1 | `data/fonts/OFL-SpaceGrotesk.txt` |
| Inter | SIL Open Font License 1.1 | `data/fonts/OFL-Inter.txt` |
| IBM Plex Sans | SIL Open Font License 1.1 | `data/fonts/OFL-IBMPlexSans.txt` |
| Plus Jakarta Sans | SIL Open Font License 1.1 | `data/fonts/OFL-PlusJakartaSans.txt` |
| Source Sans 3 | SIL Open Font License 1.1 | `data/fonts/OFL-SourceSans3.txt` |
| Material Symbols (subset) | Apache License 2.0 | `data/fonts/LICENSE-MaterialSymbols.txt` |

The application icon and logo are Lightning's own artwork, generated by
[`scripts/generate-logo-source.sh`](../scripts/generate-logo-source.sh) and
[`scripts/generate-icons.sh`](../scripts/generate-icons.sh).

The emoji catalogue (`data/emoji-catalog.tsv`) is derived from the Unicode
Consortium's emoji data (Unicode licence). It contains no emoji artwork —
glyphs come from the operating system's own emoji font.

## Services, not components

GIPHY and KLIPY are **external services** Lightning can talk to when the user
opens the GIF picker. No GIPHY or KLIPY SDK, library, or binary is included in
Lightning; the integration is plain HTTPS requests written in this repository.
See [`docs/privacy.md`](privacy.md).

## Packaging components

The Windows installers are produced with **WiX 3 / msitools** (MS-RL / LGPL) and
**NSIS** (zlib/libpng licence). Both are build-time tools; the NSIS-generated
uninstaller stub is the only NSIS-derived code that ships, which is the ordinary
and intended use of that installer.

## Corrections

If anything here is wrong, incomplete, or out of date, please report it to the
maintainer (<antrasrokas@gmail.com>) — misattributing third-party work is a
defect, not a detail.
