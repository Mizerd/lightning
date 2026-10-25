# Account-specific network profiles: inventory, design and feasibility

Status: **DESIGN AND FEASIBILITY ONLY. Nothing in this file is implemented.**
Written 2026-10-25 for the master prompt's Part 7 (per-account proxies and
Linux tunnel workers), against `main` at 55107ed4. There is no connection
setting in the app, and none may be added that claims more than the table in
section 2 can back.

Why it stops at design: this feature changes how every account reaches its
homeserver, carries proxy credentials, and has to keep the SSRF protections in
section 2 intact. CLAUDE.md section 18 requires one non-author review before
any change like that is committed, and no independent reviewer was available
in the round that wrote this. The pieces that are ready to build are marked
**READY** below. The pieces that are blocked say what blocks them.

## 1. Pinned API findings (verified against this tree)

| Fact | Evidence | Consequence |
|---|---|---|
| matrix-sdk 0.18.0 `ClientBuilder::proxy(url)` sets `reqwest::Proxy::all` on the SDK's own client | `matrix-sdk-0.18.0/src/http_client/native.rs:224-226` | Per-SDK-client proxying needs no global state. |
| That call logs the whole proxy URL at INFO | same file, `info!(proxy_url = p, ...)` | Never put a credential in that URL. Authenticated proxies go through `ClientBuilder::http_client(reqwest::Client)` with `Proxy::basic_auth`. |
| `http_client(..)` replaces the SDK's HTTP settings: user agent, timeouts, TLS minimum (1.2), root certificates | `builder/mod.rs:416-427`, `native.rs:187-230` | A custom client has to set all of them again: `user_agent(USER_AGENT)`, `min_tls_version(TLS_1_2)`, the SDK's timeout and read timeout, and the default roots. It must NEVER set `danger_accept_invalid_certs`. |
| reqwest 0.13.4 is built with `rustls, stream, gzip, deflate`, and `tokio-socks` is not in `Cargo.lock` | `rust/Cargo.toml:49`, `rust/Cargo.lock` | **SOCKS is not compiled in.** Offering it needs reqwest's `socks` feature, which brings in a new crate: a dependency change, so its own release-reviewed round. HTTP(S) proxies (`http://`, `https://` to the proxy) need nothing new. |
| Spec v1.18 `m.invite_permission_config` exists in ruma-events 0.34 | `ruma-events-0.34.0/src/invite_permission_config.rs` | Unrelated to routing. Recorded here because the same round implemented it (Part 6). |

## 2. Traffic inventory

Columns: what it is, which code makes the request, how it would follow a
profile, and the status. The status words mean:

* **SDK** — the request goes through the account's `matrix_sdk::Client`
  (`client.send`, `client.http_client()`), so a proxy set on that client
  covers it.
* **BYPASS** — the request uses its own transport, and a profile would not
  cover it.
* **GLOBAL** — not account-owned, carries no account token, and stays on the
  system route by design.

| Traffic class | Code | Status | Under a strict routed profile |
|---|---|---|---|
| Discovery, `/versions`, login, whoami | `lib.rs` common builder (`build_client`, around 11942); `server_name_or_homeserver_url` | SDK | Covered, **if** the route is on the builder before `build()`. Discovery runs inside `build()`, so a route set after login is too late. |
| OAuth metadata, registration, token exchange and refresh | `oauth.rs` (SDK `OAuth` API; `oauth.rs:1107` whoami via `client.http_client()`) | SDK | Covered. The OAuth bootstrap's in-memory client (empty store path) must get the same route. |
| OAuth/SSO page in the user's browser; loopback callback listener | `QDesktopServices`, `src/auth/OAuthCallbackServer.cpp` (listens on 127.0.0.1) | BYPASS (an external application) | Outside the guarantee. Strict mode must say so before the browser opens, or refuse the sign-in. |
| Sync (sliding sync), room lists, pagination, relations, receipts, typing | SDK | SDK | Covered. |
| E2EE: key queries and claims, to-device, backup, secret storage | SDK | SDK | Covered. Crypto behaviour does not change. |
| Matrix media: download, thumbnails, upload, encrypted attachments | `mediafetch.rs` (`client.send`, authenticated media), upload via SDK | SDK | Covered, redirects included (one reqwest client). |
| Avatars and notification images | `MediaBridge` (C++) through the Rust media path | SDK | Covered. Qt never fetches an avatar itself. |
| Banners, server info, admin tools, RTC membership `/state` | `banner.rs`, `aboutinfo.rs`, `serveradmin.rs`, `rtc.rs:716` (`client.http_client()`) | SDK | Covered. |
| Link previews and direct external images | `rooms.rs::safe_open` (`no_proxy()`, DNS pinned to a validated public address, no redirects followed automatically), `linkmedia.rs` | BYPASS **on purpose** | Must be **blocked**. `no_proxy()` is what keeps the address pinning sound: a proxy resolves the name itself and would defeat the private-address check. Do not remove it. |
| GIF search and GIF download | `gifs.rs` through `safe_get_following_redirects` (same `no_proxy` path) | BYPASS on purpose | Blocked, same reason. Provider traffic carries no Matrix identity, but it still leaves through the host's route. |
| Call SFU discovery and signalling (HTTP plus WebSocket) | `sfu.rs:418` (`no_proxy()`, pinned DNS), `tokio_tungstenite` connect | BYPASS | Blocked. HTTP-proxy support does not reach the WebSocket path. |
| WebRTC ICE, STUN/TURN, RTP | GStreamer `webrtcbin` | BYPASS (UDP) | Blocked. No HTTP or SOCKS profile covers UDP media. |
| Updater, update manifest | `src/update/UpdateManager.cpp` (its own `QNetworkAccessManager`) | GLOBAL | Stays on the system route. Never used to probe an account's route. |
| Widgets, external links, "open in browser" | OS hand-off | BYPASS (an external application) | Outside the guarantee. A hand-off the user asked for is allowed, but must not be presented as routed. |
| Fallback C++ HTTP backend | `CppHttpMatrixClient` | not applicable | Development surface only (CLAUDE.md section 5). No profiles. |
| Background (Labs live-account) handles | `bgsession.rs` through the common builder | SDK | Covered **only if** the route is part of the handle's build inputs and of its adoption hash (section 4). |

So a proxy profile can honestly claim **"Matrix traffic for this account goes
through the proxy"**: everything marked SDK. It cannot claim "all of this
account's traffic". Previews, GIFs and calls have to be switched off for that
account, or the profile has to say plainly that they go direct.

## 3. Settings design (READY)

* **Settings -> Accounts -> account -> Connection**: Default connection, or a
  named profile. Plus a small **Manage connections...** editor: name, type
  (HTTP proxy only, until SOCKS is compiled in), host, port, an optional
  username, and a password stored in the existing `SecretStore`, never in
  settings, in an export, in argv or in a URL.
* **Add account / Login**: the same selector, **before** discovery. Discovery
  and login build a temporary client with the chosen route. Changing the
  choice while discovery runs cancels it through the existing build
  generation and starts again.
* **On failure**: an explicit route defaults to pausing the account. There is
  no silent direct fallback. A direct fallback, if it is ever offered, is an
  advanced choice with a permanent indicator while it is active.
* **Coverage text**: says what the profile covers, which is the SDK rows
  above. Strict mode lists what it turns off: link previews, GIFs, calls, and
  browser sign-in unless the user confirms the hand-off.
* **Storage**: profiles and account assignments are device-local. They are
  left out of settings sync, theme export and the support-diagnostics export.
* **Test connection**: one request to the account's own homeserver
  (`/versions`) through the profile. Never a third-party IP-check service.

## 4. Route identity and lifecycle (READY)

* A profile has a stable local id and a version that changes with every
  edit. The resolved route `{profile id, version, proxy endpoint, credential
  reference}` is an immutable input of `build_client`, and the account
  handle keeps it.
* The Labs live-account adoption hash (the trust, encryption and limits
  snapshot `labs::open_context()` already carries) gains the route version.
  A background handle built under an old route can never be adopted after an
  edit.
* Changing account B's route is a controlled reconnect of B alone: it stops
  taking new work, lets queued sends finish under the original handle,
  retires that handle with token and store ownership intact (the store-close
  machinery in `storeclose.rs`), and builds a new handle. Not a logout, never
  a crypto reset. The RAM-only drafts of a still-live account survive the
  reconnect. A transport failure is never read as a revoked token.
* Deleting a profile that accounts use: each affected account has to get a
  replacement or be left explicitly paused. Deleting never means direct.

## 5. Linux namespace / vopono worker: feasibility result

**NOT FEASIBLE IN THIS CAMPAIGN. Concrete blockers:**

1. **Process model.** Isolation has to be a separate per-account process
   started inside the namespace. `setns` on a thread of the existing
   multithreaded Qt/tokio process is not a sound boundary. Today the SDK
   client, the stores and the timeline all live in the GUI process and reach
   QML through one in-process FFI (`RustClient`). A worker needs a versioned,
   authenticated local IPC for every operation the FFI exposes, about 270
   `mx_rust_*` entry points in `rust/src/lib.rs`. That is a second session architecture, and it
   would compete with the Labs live-account adoption work. It is not an
   extension of it.
2. **Kill switch.** A namespace does not stop traffic from leaving by the
   host route if the tunnel drops. Proving "no IPv4/IPv6 or host DNS
   fallback" needs a launcher whose configuration Lightning can verify, and
   needs controlled tunnel-loss tests. Lightning must not install firewall
   rules, change system DNS or ask for root on its own.
3. **Packaging.** Flatpak and Snap sandboxes cannot create network
   namespaces or join them. AppImage and native packages could, but only with
   an already-configured namespace (vopono, `ip netns`) owned by the user.
   Windows and macOS have no equivalent.

**Existing workaround, and it does NOT satisfy the requirement:** start a
second Lightning instance inside the tunnel (`vopono exec ... lightning-matrix
--profile ...`). That routes one instance, not differently routed accounts
inside one instance.

## 6. Smallest next implementation slice (needs a non-author review)

1. `rust/src/route.rs`: `RouteConfig { id, version, proxy_url_without_credentials, username, password_ref }`
   and a `reqwest::Client` factory that matches `HttpSettings::make_client`
   (user agent, TLS 1.2 minimum, timeouts, default roots), plus
   `Proxy::all(url).basic_auth(..)`. Unit tests that the credential never
   appears in the URL or in a `Debug` print.
2. `build_client` takes `Option<RouteConfig>` and uses `.http_client(..)`
   when it is set. The OAuth bootstrap client and `bgsession.rs` builds pass
   the same value. The route version goes into the adoption hash.
3. A C++ `NetworkProfileStore` (device-local; the password in `SecretStore`)
   and a per-account assignment that `RustSdkMatrixClient` hands to the
   build.
4. Strict mode switches off link previews, GIFs and calls for that account,
   in C++ before any request is made, with the reason shown in place.
5. Tests: NET-001 (no profile, routing unchanged), NET-002 (two local
   instrumented proxies, requests attributed to the right route), NET-009
   (credential scoped to the proxy, absent from origin requests and logs),
   NET-010 (an invalid certificate still fails), NET-017 (the existing SSRF
   suites still pass), NET-021/022 (one route killed: that account pauses,
   the other keeps syncing).
