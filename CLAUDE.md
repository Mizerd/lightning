## Project Overview

This project is a from-scratch **native desktop Matrix client** written primarily in **C++**.

The goal is to build a fast, native, cross-platform app similar in ambition to Element Classic / Element X, but **not** based on Element Web, Electron, Tauri, or a browser wrapper.

The app should target:

1. Linux first, especially NixOS
2. Windows later
3. macOS later

The main stack should be:

* C++20 or C++23
* Qt 6
* QML for UI
* CMake for builds
* SQLite or Qt storage for local cache/settings
* Qt Linguist for translations
* Native platform APIs where needed
* Optional Matrix Rust SDK through FFI only where it makes sense

The UI must be a real native Qt/QML desktop app.

Do not build a web app.
Do not use Electron.
Do not use Tauri.
Do not use a webview for the chat UI.
Do not fork Element Web.

Qt WebEngine or the system browser may only be used for SSO/OIDC login redirects.

---

## Core Design Rule

Use C++ for as much of the app as realistically possible.

C++ should own:

* App lifecycle
* Window/navigation state
* Room list UI logic
* Timeline UI logic
* Message composer logic
* Settings
* Account management
* Multi-account switching
* Media UI
* Notifications
* Themes
* Translations
* Local cache integration
* Platform integration
* Packaging

However, do **not** reinvent unsafe cryptography.

For modern Matrix end-to-end encryption, sliding sync, and complex compatibility features, prefer a replaceable backend interface that can later call the Matrix Rust SDK through FFI.

The app should be structured so the Matrix backend can be swapped:

```text
Qt/QML UI
    ↓
C++ application layer
    ↓
C++ MatrixClient interface
    ↓
Mock backend / C++ HTTP backend / Rust SDK FFI backend
```

---

## Important Architecture Requirements

Keep these layers separate:

1. **UI Layer**

   * QML files
   * No direct Matrix protocol logic
   * Uses C++ models/controllers exposed to QML

2. **Application Layer**

   * C++ controllers
   * Owns app navigation, selected room, selected account, settings

3. **Matrix Backend Interface**

   * Pure C++ interface
   * Abstracts login, sync, rooms, timelines, sending, media, crypto

4. **Backend Implementations**

   * MockMatrixClient for v0.1
   * Later CppHttpMatrixClient for basic Matrix HTTP API experiments
   * Later RustSdkMatrixClient for E2EE/sliding sync

5. **Storage Layer**

   * Settings
   * Local cache
   * Account/session metadata
   * Tokens must not be casually stored in plaintext

6. **Platform Layer**

   * Notifications
   * Tray
   * Keychain/Secret Service/KWallet
   * File opening
   * Autostart later

---

## Development Priorities

Start with a working native app skeleton.

Do not try to implement all Matrix features immediately.

The first milestone must compile and run.

Use a mock backend first so the UI and architecture can be built cleanly before real Matrix protocol complexity is added.

---

## Version Roadmap

### v0.1 — Native App Shell With Mock Backend

Must include:

* Qt app launches
* Main window opens
* Login screen exists
* Settings screen exists
* Homeserver URL field exists
* Mock login works
* Room list screen exists
* Timeline screen exists
* Message composer exists
* Mock rooms appear
* Mock messages appear
* Sending a message appends it to the mock timeline
* Basic dark/light/system theme structure
* Clean CMake project
* Linux/NixOS build instructions

No real Matrix network calls are required in v0.1.

### v0.2 — Basic Real Matrix Client

Add:

* Real homeserver login
* Session restore
* Logout
* Real room list
* Real joined rooms
* Basic sync
* Send text messages
* Receive text messages
* Timeline pagination
* Display names
* Avatars
* Basic error handling

This may start with plain Matrix Client-Server HTTP API.

### v0.3 — Usable Chat Features

Add:

* Media upload/download
* Image preview
* File sending
* Replies
* Edits
* Redactions/deletes
* Reactions
* Read receipts
* Typing indicators
* Mentions
* Local timeline cache

### v0.4 — Encryption

Add E2EE properly.

Do not manually implement Matrix cryptography unless there is a very strong reason.

Prefer Matrix Rust SDK or another modern maintained crypto backend.

Add:

* Encrypted room support
* Send encrypted messages
* Receive encrypted messages
* Secure key/session storage
* Undecryptable message UI
* Device verification basics
* Cross-signing later
* Invisible cryptography UX later

### v0.5 — Advanced Matrix UX

Add:

* Spaces
* Threads
* Multi-account
* Legacy SSO
* OAuth 2.0 / OIDC login
* Multi-language UI
* Sliding sync if backend supports it

### v1.0 — Polished Native Matrix Client

Add:

* Strong error handling
* Good offline behavior
* Polished notifications
* Secure storage
* Cross-platform packaging
* Full settings UI
* Accessibility
* Lithuanian translation
* Proper app branding
* Stable release build

---

## Required C++ Classes

Create these as separate headers/sources where appropriate:

### AppController

Owns high-level app state.

Responsibilities:

* App initialization
* Screen/navigation state
* Current account
* Current room
* Expose global state to QML

### AccountManager

Responsibilities:

* Add account
* Remove account
* Switch account
* List accounts
* Keep per-account data separate
* Prevent cross-account session mixing

### AuthManager

Responsibilities:

* Password login flow
* Token/session restore
* Logout
* SSO/OIDC flow coordination
* Browser redirect handling later

### MatrixClient

A pure C++ interface.

Responsibilities:

* login
* logout
* restore session
* sync
* list rooms
* get timeline
* send message
* send media
* fetch profile info
* expose errors/status

Do not tie QML directly to any concrete Matrix backend.

### MockMatrixClient

A fake backend for v0.1.

Responsibilities:

* Return mock rooms
* Return mock messages
* Pretend login succeeds
* Append sent messages locally
* Allow UI testing without network

### RoomListModel

A Qt model exposed to QML.

Responsibilities:

* Room name
* Room avatar
* Last message preview
* Unread count
* Encrypted room marker
* Space/group info later

### TimelineModel

A Qt model exposed to QML.

Responsibilities:

* Message body
* Sender
* Timestamp
* Message type
* Sending status
* Edited/deleted state later
* Reactions later
* Thread metadata later

### MessageComposer

Responsibilities:

* Draft text
* Send text message
* Attach files later
* Reply/edit mode later

### SettingsManager

Responsibilities:

* Homeserver URL
* Theme
* Language
* Startup behavior
* Notification settings
* Store non-secret preferences

### NotificationManager

Responsibilities:

* Native desktop notifications
* Unread counts
* Mention notifications
* Tray integration later

### CryptoManager

Interface only at first.

Responsibilities:

* Declare crypto boundary
* Encrypted room support later
* Device verification later
* Key storage later

Do not fake complete encryption.

### MediaManager

Responsibilities:

* Upload media later
* Download media later
* Local preview/cache
* Open files externally

### SpaceManager

Responsibilities:

* Space hierarchy later
* Rooms grouped by Spaces
* Space navigation model

### ThreadManager

Responsibilities:

* Thread metadata
* Open thread
* Reply in thread
* Thread timeline later

---

## Feature Classification

When implementing or planning features, classify them honestly:

### Easy in C++

Examples:

* Qt/QML UI
* Room list model
* Timeline rendering
* Settings screen
* Themes
* Translations
* Basic local cache
* Mock backend
* Message composer
* Native app shell

### Possible in C++ but Time-Consuming

Examples:

* Basic Matrix login
* Basic Matrix sync
* Sending text messages
* Media upload/download
* Replies
* Edits
* Redactions
* Reactions
* Read receipts
* Typing indicators
* Multi-account
* Spaces UI
* Threads UI

### Better Delegated to Matrix Rust SDK

Examples:

* End-to-end encryption
* Device verification
* Cross-signing
* Sliding sync
* Long-term Matrix compatibility
* Complex sync state handling
* Invisible cryptography UX backend logic

### Not Recommended to Implement Manually

Examples:

* Custom cryptography
* Deprecated libolm-based new implementation
* Full Matrix E2EE from scratch
* Full calls/VoIP stack at the beginning
* A complete Element replacement in the first milestone

---

## Coding Style

Use:

* Modern C++20 or C++23
* CMake
* Qt naming conventions where useful
* Small files
* Clear interfaces
* No giant monolithic source files
* No fake complete implementations for hard features
* TODO comments where features are intentionally stubbed
* Strong separation between UI and backend
* Models/controllers exposed cleanly to QML

Avoid:

* Electron
* Tauri
* Web chat UI
* JavaScript frontend
* HTML-rendered UI
* Hardcoded secrets
* Plaintext token storage without warning
* Implementing cryptography manually
* Pretending mock features are production-ready

---

## First Task

Generate the v0.1 codebase.

The v0.1 app should:

1. Compile on Linux.
2. Launch a native Qt/QML desktop window.
3. Show a login/settings screen.
4. Allow entering a homeserver URL.
5. Use a mock login.
6. Show mock rooms.
7. Open a mock room.
8. Show mock messages.
9. Allow typing and sending a message.
10. Append the sent message to the timeline.
11. Keep backend replaceable through the MatrixClient interface.

Generate:

* Project structure
* CMakeLists.txt
* src files
* include files
* qml files
* README.md
* Optional Nix dev shell
* Build/run commands

Do not generate one giant file.

Do not implement real Matrix E2EE yet.

Do not use web UI.

Prioritize a clean native C++/Qt foundation.
