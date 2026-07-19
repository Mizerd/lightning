# Lightning

A native, modern Matrix desktop client built with Qt and the official Rust Matrix SDK.

> **Development status:** Lightning is under active development. Features may be
> incomplete and bugs should be expected.

[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Matrix](https://img.shields.io/badge/protocol-Matrix-000000.svg)](https://matrix.org/)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52.svg)](https://www.qt.io/)
[![Rust](https://img.shields.io/badge/Rust-SDK-000000.svg)](https://www.rust-lang.org/)
[![Linux](https://img.shields.io/badge/platform-Linux-FCC624.svg)](https://www.kernel.org/)
![Active development](https://img.shields.io/badge/status-active%20development-orange.svg)

## Contents

- [Overview](#overview)
- [Feature highlights](#feature-highlights)
- [GIF support](#gif-support)
- [Screenshots](#screenshots)
- [Architecture](#architecture)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Packaging and releases](#packaging-and-releases)
- [Security and privacy](#security-and-privacy)
- [Project maturity](#project-maturity)
- [Repository access and contributing](#repository-access-and-contributing)
- [Licence](#licence)
- [Project link](#project-link)
- [Contact](#contact)

## Overview

Lightning is a Linux-first native Matrix desktop client. Its interface is
written in Qt 6 and QML, with C++ providing the application layer and the
official Rust Matrix SDK providing Matrix synchronisation, timelines,
encryption, and media operations.

The project aims to provide a responsive native desktop experience, secure
SDK-backed Matrix encryption, modern room and thread workflows, efficient
timeline navigation, open-source development, and compatibility with the
wider Matrix ecosystem.

## Feature highlights

The following features are present in the current codebase. Because Lightning
is still developing, some workflows remain experimental.

### Accounts, rooms, and spaces

- Matrix password login, persistent sessions, session restoration, and logout
- Persistent multi-account support: several signed-in accounts (any mix of
  homeservers) with fast in-app switching from the account menu — no login
  form between accounts, each account in its own isolated SDK and encryption
  store; only the selected account syncs
- Scoped account removal and logout that never touch other accounts; logging
  out continues with the most recently added remaining account
- Joined-room, invitation, and direct-message navigation
- Matrix-native direct-message detection and Space hierarchy navigation
- Keyboard quick switching between rooms, direct messages, Spaces, and invites

### Encryption and recovery

- Encrypted room sending and receiving through the official Rust Matrix SDK
- Automatic key handling, late decryption, and in-place decryption retry
- SAS emoji device verification and read-only session trust information
- Secure Backup recovery-key or passphrase restore and encrypted room-key import
- Encryption health, recovery, and manual retry controls

### Messaging and timelines

- Live room timelines with replies, edits, reactions, redactions, typing
  indicators, and read receipts
- Text, image, and file attachment sending, including encrypted media
- Backward pagination, first-unread navigation, jump-to-latest, and stable reply
  navigation
- Search across the currently loaded timeline without a persistent plaintext
  message index
- Smooth physical-wheel scrolling, touchpad handling, and keyboard timeline
  navigation
- Animated GIF attachment playback and validated inline rendering for direct
  raster links

### Threads

- SDK-backed Matrix thread timelines, replies, and threaded read receipts
- Per-room Threads view, thread panels, follow/unfollow controls, and pagination
- Compact Element-style thread summary cards on room timeline roots
- Text, image, and file attachments in threads, including encrypted rooms

### Desktop experience

- A four-pane design shell (spaces rail, room list, timeline, side panel)
  implemented from the Lightning design handoff, with bundled Manrope and
  JetBrains Mono fonts
- Ten complete semantic themes — Moss Light, Indigo Night, Deep Teal (the
  design-handoff set that System follows), Lightning Light/Dark, Graphite,
  Midnight, Nordic, Purple Dusk, and Warm — all persistent, live-switching,
  and WCAG-AA contrast tested
- A real application icon and desktop entry installed by the build (hicolor
  set, Wayland app-id and X11 WM_CLASS association)
- Native freedesktop notifications, mentions, and active-room suppression
- Global and per-room notification privacy controls
- Local Unicode emoji picker for composing messages and reactions

## GIF support

Lightning includes a multi-provider GIF browser alongside its existing animated
GIF attachment playback and validated inline rendering of direct GIF media. The
browser is actively developed but is now feature-complete for everyday use.

- GIPHY and KLIPY provider tabs, with per-provider attribution shown in the
  picker
- Trending results, debounced search, and client-side category shortcuts
- Pagination and infinite scrolling as results are browsed
- Favourites and a bounded local recent-GIF history
- Safe-search rating selection and a configurable autoplay policy
- Keyboard-navigable, accessible tiles shared by the room and thread composers
- Sending a chosen GIF as real Matrix media into a room or a thread; thread
  sends always land as true `m.thread` replies, and encrypted rooms use the SDK
  media-encryption path exactly like other attachments

Provider access resolves in this order:

1. a runtime override — `LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`;
2. an application key embedded into an official release build;
3. otherwise the provider shows the unconfigured (missing-key) state.

**For users:** official Lightning packages include application-level GIF-provider
configuration, so the GIF browser works immediately after install — you do not
need to create provider keys or set any environment variable. GIF searches are
sent to the selected external provider; provider availability and rate limits
remain that provider's dependency.

**For developers:** a build from source has no embedded key and shows the
missing-key state until you supply a runtime override
(`LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`); these are read at
runtime, so exporting them — for example from a local env file — enables the
browser without rebuilding. Official-package keys are injected by protected CI at
build time. An application key compiled into a distributed desktop binary is
ultimately extractable and is not a Matrix credential; treat it accordingly.

When provider integration is enabled, only the user's search term is sent to the
explicitly selected provider — Matrix room, event, thread, and user identifiers
are never sent. Downloaded provider media is fetched over HTTPS only, with
revalidated redirects, a bounded download size, and GIF magic and dimension
validation before it is uploaded through Matrix rather than sent as a bare
provider URL. Keys are never logged, exposed to QML, or committed to the
repository.

## Screenshots

Screenshots will be added as the interface continues to mature.

## Architecture

Lightning keeps presentation, application state, and Matrix behaviour
separate:

- **QML** owns visual presentation and interaction.
- **C++** owns application state, Qt-facing models, controllers, and lifecycle.
- The **official Rust Matrix SDK** owns Matrix protocol behaviour,
  synchronisation, timelines, encryption, and media operations.

Lightning does not implement its own Matrix cryptography.

## Building

The verified development workflow uses the repository's Nix flake. Linux is
the currently supported development target documented by the project.

Clone the public source and enter the development shell:

```sh
git clone https://gitlab.smetonis.net/Mizerd/lightning.git
cd lightning
nix develop
```

Configure and build the Rust SDK-backed client:

```sh
nix develop -c cmake -S . -B build-rust -G Ninja \
  -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake --build build-rust
```

A non-Rust build is also available for the mock and experimental HTTP
backends:

```sh
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build
```

The Rust backend is required for the SDK-backed Matrix, encryption, and thread
features described above.

## Running

Launch the Rust SDK-backed client from the repository root:

```sh
nix develop -c ./build-rust/matrix-client --backend=rust
```

Lightning is actively developed, so unfinished behaviour and regressions may
be encountered.

## Testing

Run the Rust tests:

```sh
nix develop -c cargo test --manifest-path rust/Cargo.toml
```

Run the Rust-backed C++/QML test suite:

```sh
nix develop -c ctest \
  --test-dir build-rust \
  --output-on-failure
```

The non-Rust build has its own test suite:

```sh
nix develop -c ctest \
  --test-dir build \
  --output-on-failure
```

Build the corresponding tree before running its tests.

## Packaging and releases

This repository contains the Lightning application source. The Debian and RPM
packages are built, tested on a clean system, published to this project's
Package Registry, and attached to its GitLab Releases by a separate
package-building and release-automation project,
[lightning-deploy](https://gitlab.smetonis.net/Mizerd/lightning-deploy).

Official packages are still published under this main Lightning project and
attached to its releases; `lightning-deploy` only holds the pipeline and
automation logic. For package-pipeline details, see that project.

## Security and privacy

Matrix encryption is handled by the official Rust Matrix SDK; Lightning does
not implement custom Matrix cryptography. Sensitive cryptographic material
must not be logged, and decrypted private messages should not be stored in
plaintext application caches.

When GIF-provider integration is enabled, external searches are sent to the
selected provider. Provider API keys and other secrets must never be committed
to the repository. Security-sensitive changes, especially changes involving
end-to-end encryption, require careful review and testing.

Lightning has not been presented as formally security audited.

## Project maturity

Lightning is under active development. APIs, interface design, and behaviour
may change; some features are experimental; and bugs or Matrix interoperability
issues may exist. Users should not treat the client as formally audited or
certified.

## Repository access and contributing

Lightning's source code is publicly readable and may be cloned and studied
under the GPL-3.0-or-later licence. Direct repository access and write
permissions are currently limited to Rokas Smetonis. Public registration,
public forks, or direct merge-request submission may not currently be
available on this GitLab instance. Open-source licensing does not grant direct
write access to the canonical repository.

People interested in contributing, testing, reporting bugs, or requesting
repository access should contact the maintainer. Focused, tested contributions
are encouraged. Changes affecting end-to-end encryption require particular
care, explicit security reasoning, and appropriate tests.

## Licence

Copyright © 2026 Rokas Smetonis

Lightning is free software licensed under the GNU General Public License v3.0
or later. See [LICENSE](LICENSE) for details.

## Project link

Public source: <https://gitlab.smetonis.net/Mizerd/lightning>

## Contact

For contribution enquiries, bug reports, testing feedback, or repository
access requests, contact:

**Rokas Smetonis** — [antrasrokas@gmail.com](mailto:antrasrokas@gmail.com)
