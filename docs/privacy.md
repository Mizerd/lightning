# Lightning privacy policy

**Last reviewed: 2026-08-16, against Lightning 0.7.0.**

This document is derived from the source code, not from marketing copy. Every
claim below names the file that implements it, so it can be checked and so it can
be shown to be wrong if it ever drifts.

Lightning is a desktop [Matrix](https://matrix.org/) client. It is not a service:
there is no Lightning server, no Lightning account, and no backend operated by
the project. Everything Lightning stores lives on the user's own computer, and
everything it sends goes either to the homeserver the user signed in to or to a
service the user explicitly invoked.

## Summary

- **Lightning collects nothing.** There is no analytics, no telemetry, no crash
  reporting, no usage measurement, and no advertising identifier. The project
  receives no personal data from installed clients, by any route.
- **Lightning can check for its own updates, and that is off by default.** The
  check is an anonymous request for two small public files and carries no
  account, device or usage information — see section 5a.
- The **Matrix homeserver the user chooses** receives normal Matrix protocol
  traffic, because that is what a Matrix client does.
- **Third parties** are contacted in exactly two situations, both listed below,
  and both under user control.
- Message content in encrypted rooms is end-to-end encrypted by the official
  Rust Matrix SDK. Lightning implements no cryptography of its own.

## 1. The Matrix homeserver

**Destination:** the homeserver the user names when signing in (for example
`matrix.org`, or any private server). Lightning ships **no** default or
preferred homeserver and has no relationship with any homeserver operator.

**Why:** it is the service the user is a client of.

**Triggered by:** signing in, and thereafter by normal use and background
synchronisation while signed in.

**What is sent:** ordinary Matrix client-server traffic, handled by the official
[`matrix-rust-sdk`](https://github.com/matrix-org/matrix-rust-sdk) (`rust/`):
authentication, sync/Sliding Sync, room and thread timelines, messages,
reactions, receipts, typing notifications, media uploads and downloads, avatars,
device keys and end-to-end-encryption traffic, key backup, device verification,
push rules, and user/room directory lookups the user performs. The client
identifies itself with the user agent `Lightning/<version>`.

**Automatic:** yes, while signed in — this is the application's purpose.

**Can it be disabled:** by signing out or not signing in. There is no offline
mode.

**Note on server-side visibility:** in an *unencrypted* room the homeserver can
see message content, as with any Matrix client. In an *encrypted* room it cannot;
the SDK encrypts before sending. Homeserver-side privacy is governed by that
homeserver's own operator and policy, not by Lightning.

**Discovery:** resolving a user's homeserver may involve a `.well-known` lookup
against the domain part of the Matrix ID the user typed. That domain is supplied
by the user.

## 2. Link previews — third party, **off by default**

**Destination:** whatever website a message links to. Arbitrary third-party
hosts.

**Why:** to show a title/description/thumbnail card, or to render a directly
linked image inline.

**Triggered by:** displaying a message that contains an `https://` URL — *only*
if the user has switched automatic previews on, or has used the per-message
"Load link preview" action.

**Important:** Lightning fetches the linked page **itself, from the user's own
computer** (`rust/src/rooms.rs`, `fetch_url_preview`). It does **not** use the
homeserver's `preview_url` proxy. That means a fetched preview reveals the user's
**IP address**, approximate read timing, and the `Lightning/<version>` user agent
to the linked site — including to a site chosen by whoever sent the message. This
is the reason the feature is not on by default.

**Defaults (`src/app/SettingsManager.cpp`):**

| Setting | Default | Effect |
|---|---|---|
| Automatically load previews in unencrypted rooms | **Off** | No preview is fetched unless the user asks for one |
| Load previews in encrypted rooms | **Off** | Never automatic; each preview needs an explicit per-message action |

**Can it be disabled:** it is already off; both switches live in
**Settings → Privacy & security → Link previews**, and the screen states the
IP-address consequence.

**What is sent when the user does request one:** an HTTPS `GET` for the URL, with
`User-Agent: Lightning/<version>`, an `Accept` header, and
`Accept-Language: en-US,en;q=0.9`. No cookies are stored or sent (the HTTP client
is built without a cookie jar), no Matrix identifiers, no room or event IDs, no
message text beyond the URL itself, and no referrer.

**Safety limits** (`rust/src/rooms.rs`, `safe_get`): HTTPS only; credentials in
URLs refused; `localhost`/`.local` refused; DNS answers must resolve to public
addresses (private, loopback, link-local, multicast and CGNAT ranges are
rejected) and the validated address is pinned so the host cannot DNS-rebind; at
most 4 redirects, each revalidated; 5 s connect and 12 s request timeouts; 2 MiB
HTML and 5 MiB image ceilings; images validated by magic bytes, not by the
claimed type; SVG is never rendered; no JavaScript is ever executed.

## 3. GIF providers (GIPHY, KLIPY) — third party, user-invoked

**Destination:** `api.giphy.com` and `api.klipy.com`, plus those providers' media
CDNs.

**Why:** to browse and send GIFs.

**Triggered by:** the user opening the GIF picker, typing a search, changing
provider tab, paginating, or viewing a saved provider bookmark. The picker's
first request is issued from `onAboutToShow` (`qml/GifPicker.qml`) — that is, on
the user opening it. **Nothing contacts a GIF provider before then**: not at
startup, not on sign-in, not while a room is open.

**What is sent** (`src/gif/GifProvider.cpp`): the provider API key, the user's
search term (trimmed, truncated to 50 characters), a result limit and page
offset, and the safe-search rating. **No Matrix data is ever sent** — no user ID,
room ID, event ID, display name, homeserver, message body, or token. The provider
sees the request's source IP address, as any web request does.

**Can it be disabled:** yes — by not opening the GIF picker. The picker is the
only route to a provider request. The **Saved** and **Recent** tabs issue no
search, trending, category or pagination call at all; a saved *provider bookmark*
is a link, so its thumbnail still loads from that provider's CDN when the tab is
shown, whereas a GIF saved out of a chat is stored on the device and loads with
no network at all.

**Third-party policies** apply to those requests:
[GIPHY privacy policy](https://support.giphy.com/hc/en-us/articles/360032872931-GIPHY-Privacy-Policy),
[KLIPY privacy policy](https://klipy.com/privacy-policy).

**Attribution:** the selected provider's required attribution is displayed in the
picker.

## 4. Links the user clicks

Clicking a link (or a link-preview card) hands the URL to the operating system's
default browser (`src/media/MediaManager.cpp`, `openWebUrl`). From that point the
browser's own privacy behaviour applies. Authenticated Matrix media URLs are
never handed to an external application; media is fetched and decrypted through
Lightning's own media bridge.

## 5. What Lightning does *not* do

Verified by searching the whole source tree (C++, Rust, QML) for the
corresponding integrations — there are none:

- **No analytics or telemetry** of any kind.
- **No crash reporting** — no Sentry, Crashpad, Breakpad, or equivalent.
- **No silent update check.** Automatic update checks are **off by default**.
  When enabled they run at most once every 24 hours; a manual check is always
  available in Settings → Updates. See section 5a for exactly what is sent.
- **No advertising, tracking, or fingerprinting services.**
- **No hard-coded third-party endpoint other than the two GIF providers above.**
  The only other external hostnames in the source are `matrix.to` (used to
  *construct* permalinks — Lightning never fetches it) and documentation links.
- **No QML component fetches an arbitrary remote URL.** Every image shown in the
  interface is routed through the C++ media bridge; there is no
  `Image { source: "https://…" }` anywhere in `qml/`.

## 5a. Update checks — the project's own GitLab, off by default

Lightning can ask whether a newer release exists. This is the only route by
which the project itself receives any request from an installed client, and it
is disabled until the user enables it.

**When it happens.** Never automatically unless the user turns on *Automatic
update checks* in Settings → Updates (default off). When enabled: at most once
per 24 hours, never within the first 30 seconds of launch, and never triggered
by switching room or account. A manual *Check for updates* button is always
available and is always an explicit user action.

**What is contacted.** One host, `gitlab.smetonis.net`, over HTTPS, anonymously
and without credentials — two small public files from the project's package
registry (`update-manifest-v1.json` and its detached signature).

**What is sent.** The URL and the update user agent, which is exactly
`Lightning/<version>` — the application version and nothing more, not even the
operating system. Nothing else is sent. In particular the request never
contains, and Lightning has no code to add: the Matrix user ID,
display name, homeserver, device or session ID, access or refresh token, room,
message or contact data, an installation ID, or any analytics identifier. **No
updater tracking identifier exists** — none is generated, stored or transmitted,
so repeated checks are not linkable by anything Lightning provides.

**What is stored locally.** Only the automatic-check preference, the time of the
last successful check, and the version the user last dismissed. These live
outside every account and contain no Matrix data.

**Downloads.** If the user chooses to install an update, the artifact is fetched
from the same host. It is verified against an Ed25519 signature over the
manifest and the SHA-256 recorded there before anything is installed, and a
failure of either check stops the update with no way to override it. Flatpak and
Snap installations are never downloaded by Lightning — those package managers
own their own updates.

## 6. What is stored on the device

Local only. Nothing here is uploaded anywhere by Lightning.

| Data | Where | Notes |
|---|---|---|
| Access/refresh tokens | OS secret service (libsecret; Windows Credential Manager) | With a clearly flagged insecure fallback if no keyring is available; never exposed to QML |
| Matrix session and crypto store | Per-account application data directory | SDK-owned; contains device and room keys |
| Message cache | Application data directory | **Encrypted-room plaintext is never persisted** — it stays in memory only |
| Settings | QSettings, per account | Theme, layout, privacy switches |
| Saved GIFs/images | Account-scoped store, bounded at 200 items / 64 MiB | Only what the user explicitly starred; records no room, event, or sender; deleted on sign-out and on account removal; disclosed with a **Clear all** control in Settings → Privacy & security |
| Recently used emoji/GIFs | Settings | Bounded local lists |
| Playable media temp files | Session-scoped, mode 0600 | Wiped on sign-out, account switch, and exit |

Signing out or removing an account deletes that account's store, including its
saved-media store.

## 7. Logging and diagnostics

Local debug logs may contain account-scoped paths and a short account slug; they
stay on the user's machine and are never transmitted. Tokens, recovery keys,
room/session keys, secret-storage material, passwords, provider API keys, and
decrypted private message bodies are never logged.

The optional **support diagnostics export** writes a file the user may choose to
share. It carries hashed account identifiers and no filesystem paths, and it is
written locally — Lightning never uploads it.

## 8. Installers, and what they change on your system

The Windows installers transfer nothing anywhere. They contact no network
service, contain no bundled offers, and collect no information — installation is
a local file copy plus the registrations listed below.

| Change | MSI | Setup EXE | Portable ZIP |
|---|---|---|---|
| Files under `%LOCALAPPDATA%\Programs\Lightning` | yes | yes | no (extract anywhere) |
| Start-menu shortcut | yes | yes | no |
| Desktop shortcut | no | optional, off by default | no |
| Uninstall entry (per-user `Uninstall` key) | yes (Windows Installer) | yes | no |
| One per-user registry key recording the install directory | yes | yes | no |
| `PATH`, file associations, URL protocols, services, scheduled tasks, firewall rules, autostart, shell extensions, drivers | **none** | **none** | **none** |
| Administrator rights | not required (per-user) | not required (per-user) | not required |

Uninstalling removes the application files, the shortcuts, and those registry
entries. It deliberately does **not** delete your Matrix session, settings, or
message store: those live in your user profile, outside the install directory,
and destroying user data during an uninstall would be the wrong default. Remove
them by signing out of the account inside Lightning first, which deletes that
account's store, or by deleting the application's data directory afterwards.

**Why the installers do not display this policy during installation.** Under
SignPath Foundation's terms, showing a privacy policy at install time and
offering an opt-out is required of software that transfers user data to systems
the user did not specify. Lightning does not: the homeserver is the one the user
types in when signing in, GIF providers are contacted only while the user is
using the GIF picker, link previews are off by default and each one is a
deliberate action, there is no telemetry, analytics or crash reporting to opt
out of, and update checks are off until the user switches them on. Adding a
consent dialog for transfers that do not happen would be noise, not disclosure.
The policy is instead linked from the project home page and from every release
page, and the controls that do matter
live in **Settings → Privacy & security**. If Lightning ever gains a background
transfer to a service the user did not choose, this reasoning stops holding and
the installers must change with it.

## 9. Children and special categories

Lightning is a general-purpose messaging client and is not directed at children.
It processes whatever the user sends through their chosen homeserver; the project
has no access to it.

## 10. Changes

This policy is versioned in the repository alongside the code it describes. Any
behaviour change that affects it is expected to change this file in the same
commit, and the automated `signpath-compliance` test keeps the policy present and
linked.

## Contact

Rokas Smetonis — <antrasrokas@gmail.com> ·
<https://gitlab.smetonis.net/Mizerd/lightning>
