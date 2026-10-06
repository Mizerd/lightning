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
| **deb / rpm** | `/usr/share/doc/lightning/copyright` and `LICENSE` (the rpm, package `lightning-matrix`, uses `doc/lightning-matrix`). Nothing third-party is bundled — GStreamer and Qt are declared dependencies, so the distribution already ships their licences |
| **Flatpak** | Lightning's own; GStreamer and Qt come from the KDE runtime |
| **Windows** | `Lightning/licenses/`: Lightning's GPL-3, seven Qt directories, and thirteen under `lightning-gstreamer` (gstreamer-1.0, `-base`, `-good`, `-bad`, gst-plugins-rs, libnice, libsrtp, libvpx, opus, orc, zlib, webrtc-audio-processing, mingw-runtime). Plus `runtime-dependencies.json`, recording the exact set of PE files staged and their import graph |
| **AppImage / snap** | Lightning's GPL-3; `usr/share/doc/<pkg>/copyright` for ~235 packages, deployed by **linuxdeploy's own** `dpkg-query` pass and present since long before this round; and `usr/share/licenses/third-party/<pkg>.copyright`, a second pass derived from the payload that additionally covers the ten packages hand-staged past linuxdeploy's excludelist |
| **snap only** | Data files core24 lacks, staged from Debian: the **shared-mime-info** MIME database (GPL-2.0-or-later), **xkb-data** keymaps and **fontconfig-config** rules, each with its Debian copyright file at `usr/share/licenses/third-party/<pkg>.copyright` (build-snap.sh refuses to build without them) |
| **macOS** | `Contents/Resources/licenses/`: Lightning's GPL-3 and the **vendored** gst-plugins-good text from this repository. Whatever the upstream GStreamer framework itself carries is REPORTED in the job log and not yet asserted — nobody has listed that tree, and the build succeeds with a count of zero |

**Known gaps, stated rather than glossed.** The AppImage's FFmpeg closure
(`libavcodec`, `libavformat`, `libavutil` and the codec libraries they use) is
built from Debian's GPL-enabled FFmpeg and is therefore **GPL-2-or-later**, not
LGPL. The Windows package does not yet carry licence text for FFmpeg or for the
Qt/GStreamer support libraries listed above, and the macOS bundle carries only
part of its set. Both are tracked in
[`docs/open-items.md`](open-items.md); the corresponding-source offer those
GPL libraries require is an open decision recorded there.

## RNNoise (microphone noise suppression)

Vendored in the source tree, compiled into Lightning, statically linked. It is
what Settings -> Experimental -> RNNoise runs on the outgoing microphone.

| | |
|---|---|
| What | RNNoise, a recurrent-network noise suppressor by Jean-Marc Valin (Xiph.Org) |
| Version | **v0.2**, commit `904a876dce1f9ab8860c0a5000ed151f9f6eef58` (2024-04-14), from https://github.com/xiph/rnnoise |
| Where | [`third_party/rnnoise/`](../third_party/rnnoise/), with its own [`PROVENANCE.md`](../third_party/rnnoise/PROVENANCE.md) (exact files and sha256, how to update) |
| Code licence | BSD-3-Clause. Text: [`third_party/rnnoise/COPYING`](../third_party/rnnoise/COPYING) (Copyright (c) 2007-2017, 2024 Jean-Marc Valin; 2023 Amazon; 2017 Mozilla; 2005-2017 Xiph.Org Foundation; 2003-2004 Mark Borgerding). Authors: [`third_party/rnnoise/AUTHORS`](../third_party/rnnoise/AUTHORS) |
| What ships | The C sources above, unmodified, built as the static library `lightning_rnnoise`; the pre-trained model as generated C (`rnnoise_data.c`, about 29 MB of source, about 5.5 MB of weights in the binary). No `.pth`, no tooling, no download step |
| Model | `rnnoise_data-0b50c45.tar.gz` (the `model_version` of v0.2), sha256 `4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43`, fetched once by hand from `https://media.xiph.org/rnnoise/models/` and committed as generated C. Never fetched at build or run time |
| Model licence | **Not stated by upstream.** The weights are an integral part of the upstream distribution and are treated as BSD-3-Clause by Debian and Gentoo (CC0 by Fedora), per upstream issue #284 (open, unanswered). Training data: CC0 noise, CC BY / CC BY-SA 4.0 speech. Full finding and the fallback option in `PROVENANCE.md`. Treated here as: distributed with the library under its BSD-3-Clause terms, with the residual uncertainty recorded |
| Binary redistribution | The BSD-3-Clause condition (reproduce the copyright notice and disclaimer with binaries) is met by shipping `COPYING` with every package and by this file |

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

**Sign-in provider logos.** The sign-in screen draws the marks of Apple,
Facebook, GitHub, GitLab, Google and X (MSC2858's `twitter`) on single sign-on
buttons, from path data in [Simple Icons](https://simpleicons.org) 16.33.0,
released under **CC0-1.0**. The path strings are embedded in
[`qml/LoginScreen.qml`](../qml/LoginScreen.qml). The marks themselves are
trademarks of their owners and are used only to name the provider a button
signs in with; Simple Icons records no separate licence for these icons. Brand
guidelines: Apple <https://developer.apple.com/design/human-interface-guidelines/sign-in-with-apple>,
Facebook <https://about.meta.com/brand/resources/facebook/logo>,
GitHub <https://github.com/logos>,
GitLab <https://about.gitlab.com/handbook/marketing/corporate-marketing/brand-activation/trademark-guidelines/>,
Google <https://about.google/brand-resource-center/brand-elements/>,
X <https://about.x.com/en/who-we-are/brand-toolkit>. They are drawn in one
colour, the button's own ink, so they stay legible on every theme.

The application icon and logo are Lightning's own artwork, generated by
[`scripts/generate-logo-source.sh`](../scripts/generate-logo-source.sh) and
[`scripts/generate-icons.sh`](../scripts/generate-icons.sh).

The emoji catalogue (`data/emoji-catalog.tsv`) is derived from the Unicode
Consortium's emoji data (Unicode licence). It contains no emoji artwork —
glyphs come from the operating system's own emoji font.

## DeepFilterNet (microphone noise suppression)

Vendored in the source tree, compiled into the Rust bridge, statically linked.
It is what Settings -> Experimental -> DeepFilterNet runs on the outgoing call
microphone. Full provenance, file hashes and every local change:
[`third_party/deepfilternet/PROVENANCE.md`](../third_party/deepfilternet/PROVENANCE.md).

| | |
|---|---|
| What | libDF (crate `deep_filter`), the native Rust streaming inference of DeepFilterNet, by Hendrik Schröter |
| Version | upstream `main` at commit `d375b2d8309e0935d165700c91da9de862a99c31` (2024-10-17), from https://github.com/Rikorose/DeepFilterNet (newest release tag `v0.5.6` = `978576aa`; 89 commits older) |
| Code licence | **MIT OR Apache-2.0**, copyright 2021 Hendrik Schröter. Texts: `third_party/deepfilternet/LICENSE-MIT`, `LICENSE-APACHE` |
| What ships | `libDF/src/lib.rs` and `libDF/src/tract.rs` (modified: per-frame logging removed, adapted to tract 0.21.14 and ndarray 0.16) compiled into `lightning-matrix`; no Python, PyTorch or other DeepFilterNet component |
| Model | **DeepFilterNet3** (`models/DeepFilterNet3_onnx.tar.gz`, 7,983,136 bytes, sha256 `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616`), unmodified, embedded in the binary; never downloaded |
| Model licence | **Not explicitly stated by upstream.** The repository's MIT/Apache-2.0 grant names "all code"; `models/` has no licence file; four upstream issues asking (#697, #700, #709, #712) are unanswered. Lightning redistributes the weights under the repository's MIT OR Apache-2.0 terms on the strength of the author's own distribution of the same weights inside his MIT/Apache-2.0-labelled release binaries — a reading, not an explicit grant. A build with `-DLIGHTNING_ENABLE_DEEPFILTERNET=OFF` contains neither the model nor the code. |

The crates.io packages this adds to the Rust bridge (all pinned in
[`rust/Cargo.lock`](../rust/Cargo.lock), none of them DeepFilterNet itself):

| Licence | Crates |
|---|---|
| MIT OR Apache-2.0 | tract-core, tract-data, tract-hir, tract-linalg, tract-nnef, tract-onnx, tract-onnx-opl, tract-pulse, tract-pulse-opl (all 0.21.14), ndarray 0.16.1, matrixmultiply 0.3.11, rawpointer 0.2.1, rustfft 6.4.1, primal-check 0.3.4, strength_reduce 0.2.4, transpose 0.2.3, num-complex 0.4.6, num-integer 0.1.47, half 2.4.1, ahash 0.8.12, const-random 0.1.18, const-random-macro 0.1.16, hashbrown 0.14.5, dlv-list 0.5.2, itertools 0.12.1, itertools 0.13.0, kstring 2.0.2, liquid 0.26.8, liquid-core 0.26.8, liquid-derive 0.26.8, liquid-lib 0.26.8, memmap2 0.9.11, minimal-lexical 0.2.1, paste 1.0.15, portable-atomic 1.15.0, portable-atomic-util 0.2.8, rand_distr 0.4.3, static_assertions 1.1.0, string-interner 0.15.0, tar 0.4.46, xattr 1.6.1, filetime 0.2.29, bit-set 0.5.3, bit-vec 0.6.3, downcast-rs 1.2.1, dyn-clone 1.0.20, dyn-hash 0.2.2 |
| BlueOak-1.0.0 OR MIT OR Apache-2.0 | anymap3 1.1.0 |
| MIT | realfft 3.5.0, rust-ini 0.21.3, ordered-multimap 0.7.3, derive-new 0.5.9, nom 7.1.3, scan_fmt 0.2.6, crunchy 0.2.4, doc-comment 0.3.4 |
| MIT AND (MIT OR Apache-2.0) | libm 0.2.11 (replaces the 0.2.16 the lock held: every tract 0.21 release pins `=0.2.11`) |
| Apache-2.0 | prost 0.11.9, prost-derive 0.11.9 |
| CC0-1.0 | tiny-keccak 2.0.2 |

Some of these never reach the binary: liquid, liquid-core, liquid-derive,
liquid-lib and kstring only template tract-linalg's assembly kernels at build
time, and prost-derive and derive-new are procedural macros.

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
