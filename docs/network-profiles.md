# Account-specific network profiles: inventory, design and feasibility

Status: **v1 IMPLEMENTED 2026-10-26 (per-account HTTP / SOCKS5 proxy), see
section 7. Sections 1-6 are the 2026-10-25 design it was built from; where
section 7 differs, section 7 is what the code does.** Rokas approved the
dependency change ("sure its fine get the crate"). The change is §18
review-gated: credentials, networking and the client builder. The Linux
namespace / tunnel worker (section 5) stays out of scope.

## 1. Pinned API findings (verified against this tree)

| Fact | Evidence | Consequence |
|---|---|---|
| matrix-sdk 0.18.0 `ClientBuilder::proxy(url)` sets `reqwest::Proxy::all` on the SDK's own client | `matrix-sdk-0.18.0/src/http_client/native.rs:224-226` | Per-SDK-client proxying needs no global state. |
| That call logs the whole proxy URL at INFO | same file, `info!(proxy_url = p, ...)` | Never put a credential in that URL. Authenticated proxies go through `ClientBuilder::http_client(reqwest::Client)` with `Proxy::basic_auth`. |
| `http_client(..)` replaces the SDK's HTTP settings: user agent, timeouts, TLS minimum (1.2), root certificates | `builder/mod.rs:416-427`, `native.rs:187-230` | A custom client has to set all of them again: `user_agent(USER_AGENT)`, `min_tls_version(TLS_1_2)`, the SDK's timeout and read timeout, and the default roots. It must NEVER set `danger_accept_invalid_certs`. |
| reqwest 0.13.4 was built with `rustls, stream, gzip, deflate` | `rust/Cargo.toml:49` | SOCKS was not compiled in. **Corrected 2026-10-26:** in reqwest 0.13.4 the `socks` feature is EMPTY (`socks = []` in its Cargo.toml) and only turns on reqwest's own code, which uses `hyper_util::client::legacy::connect::proxy::{SocksV4, SocksV5}`. `hyper-util` 0.1.20 is already in the lock with `client-proxy` enabled by reqwest itself. So enabling it adds **no crate** and changes no `Cargo.lock` entry (features are not recorded there). The earlier "brings in tokio-socks" was reqwest 0.12's shape. |
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

## 7. What v1 implements (2026-10-26)

One route per account: **Direct** (the system's network settings, exactly as
before), an **HTTP proxy** (HTTPS tunnelled with CONNECT), or a **SOCKS5
proxy** with remote DNS (`socks5h`: the proxy resolves the homeserver's name,
the local resolver never sees it). Optional user name and password. No named
profiles, no strict-mode toggle and no Test button: the smallest thing that
can be honest, per Rokas's "no niche settings".

**Where each half lives.** Kind, host, port and user name: the account's
record in the settings file (`accounts/<slug>/networkRoute/*`,
`SettingsManager::networkRouteRecord`), device-local, never synced, never in
an export. The password: the account's `SecretStore` under
`networkRoutePassword`, beside its tokens, so `clearAccountSecrets()` on
sign-out takes it, and removing the account record takes the rest. QML
(`app.networkRoute`, `NetworkRouteController`) sees `hasPassword` only; the
field is write-only. The password leaves C++ only inside the JSON handed to
`mx_rust_set_network_route`, which the caller scrubs. In Rust it lives in the
route registry and in the reqwest `Proxy` (as URL userinfo, which reqwest uses
for SOCKS5 auth and turns into `Proxy-Authorization` for HTTP); `RouteConfig`'s
`Debug` redacts it, `proxy_url()` never contains it, and nothing logs it. We
never call `ClientBuilder::proxy()`, which logs the URL at INFO.

**How a route reaches the SDK.** `RustSdkMatrixClient` creates the handle,
then `applyMediaStoreKey` and `applyNetworkRoute` hand the handle its key and
route, keyed by store path (`rust/src/route.rs`, the same registry shape as
`mediastore.rs`, owned per handle so a retiring handle cannot remove its
successor's). `build_client_with` asks `route::client_for(store_path)` and,
when there is a route, passes `ClientBuilder::http_client()` a reqwest client
built like the SDK's own (`USER_AGENT`, TLS 1.2 minimum, 30 s default timeout,
default roots, no certificate leniency) plus `Proxy::all`. It is set after
every builder call that touches HTTP settings, because `user_agent()` after
`http_client()` silently replaces the custom client. An explicit proxy also
switches off reqwest's environment-proxy handling (`HTTP_PROXY`, `NO_PROXY`),
so no host is exempted behind the user's back. Every request this client
makes follows the route: discovery, login, sync, E2EE, media, thumbnails,
avatars, server-side link previews, RTC `/state`, everything in section 2
marked SDK. A restore that falls back to the recorded homeserver URL builds
through the same function.

**Fail closed.** `NetworkRoute::rustJson` answers `none` only when the
account has no route record. A record it cannot use (password unreadable
because the keyring is locked or unavailable, password missing, a kind this
build does not know, a host that does not validate) is `unavailable`, and
anything Rust cannot parse is recorded as `unavailable` too. For an
unavailable route `build_client_with` refuses before any store opens ("network
route unavailable ... not connecting directly"), and if Rust refuses the route
outright the C++ handle is destroyed and the account is not opened. An
unreachable proxy makes requests fail; reqwest never retries them directly.

**What is refused for a routed account, and why.** Each of these would
otherwise leave directly from the address the route hides:
* client-side link previews (`rooms.rs::fetch_url_preview`): the server-side
  preview still runs, through the route; the client fallback reports
  `blocked_by_route`. `safe_open` keeps its `no_proxy()` and DNS pinning
  unchanged, which is what keeps the SSRF checks sound;
* link media (`linkmedia.rs::link_media_fetch`): `blocked_by_route`;
* calls: `sfu::connect` refuses, `RtcController::joinBlock` reports
  `NetworkRoute` ("network_route") ahead of every other reason, and
  `CallController::placeCall` / `placeCallWithOffer` / `answer` refuse with
  `network_route`. WebRTC media is UDP and no HTTP/SOCKS route here carries it;
* background sessions (Labs live accounts): a routed account is not eligible
  (`SessionPool::eligibility` -> Skip), a start in flight when a route is set
  is dropped, and a running host is retired when its account gains a route.
  It runs in the foreground only, until the keyring worker carries the route
  password too.

**What stays on the system route, stated in the Settings copy.** GIF search
and GIF download (provider traffic carrying no Matrix identity, started by an
explicit picker action), the updater, and anything handed to the browser (OAuth
and SSO pages, links). GIFs differ from section 2's "blocked" line on purpose:
they are not account traffic, and the Settings text says they use the system's
network settings.

**Known limits of v1 (documented, not hidden).**
* **A new account's first sign-in is direct; signing a routed account in
  again is not.** A route belongs to a saved account, and a new account has no
  record until its first sign-in succeeds. Store-less sign-in handles (the
  OAuth/SSO bootstrap, the SSO provider probe, the whoami adoption probe, a
  refused sign-in's revocation) get a route of their own (`route::key_for`
  keys a store-less handle by itself): by account when the account is known
  (`rustJsonForSignIn(..., userId)`), otherwise from the server being signed
  in to — the route of the saved account on that server that has one, so an
  expired sign-in of a routed account goes through its proxy. Two routed
  accounts on one server with different routes make the sign-in
  `unavailable` (refused, never direct) until one is named. Matching uses the
  host[:port] of the typed value against the recorded homeserver and the user
  id's server name. The sign-in page itself opens in the user's browser and
  uses the browser's network settings; the Settings copy says so. Sign-out
  removes the record and the password, so signing the same account in again
  afterwards is a first sign-in.
* **Applies on the next connect.** Saving does not rebuild the session that is
  open; restart or switch away and back. The Settings copy says so.
* **No connection test**, and no "using proxy" indicator outside Settings.
* Proxy-to-homeserver TLS is end to end (CONNECT and SOCKS both tunnel), so
  the proxy sees the homeserver's name and traffic volume, not content. The
  link to the proxy itself is plain (`http://` / SOCKS5), as with every
  desktop proxy setting; an `https://` proxy URL is not offered.

**Platforms.** Proxies need no namespaces or privileges: the same code serves
Linux (native, AppImage, Flatpak, Snap), Windows and macOS. A Flatpak or Snap
reaches a proxy on `127.0.0.1` as long as the sandbox shares the host network,
which both manifests do. The namespace / tunnel worker (section 5) remains out
of scope everywhere.

**Tests.** Rust (`route.rs`): parsing and refusal of invalid input, no
credential in the URL or `Debug`, a client builds for both kinds with and
without auth, the per-store per-owner registry, `client_for` refusing an
unusable route, `build_client_with` refusing before any store is created, and
a request through a dead SOCKS proxy failing rather than reaching a live
direct target. C++ `network-route` (`tests/NetworkRouteTest.cpp`): where each
half is stored, nothing in the settings file holds the password, QML never
gets it, a refused keyring write stores nothing, every unusable state is
`unavailable` and never `none`, sign-out takes it and leaves another account's,
and a source contract on the Settings card. `CallControllerTest` and
`RtcSessionTest`: calls refused for a routed account. Live: NOT TESTED.

**Logging.** `LIGHTNING_RUST_LOG` can never enable `reqwest`, `hyper_util`,
`hyper` or `h2` (their debug/trace output can print the proxy URI):
`clamp_transport_tracing` drops any directive naming them and pins each
`off`, which outranks a bare `trace`. Without `LIGHTNING_RUST_LOG` no
subscriber is installed at all.

**Dependency.** `rust/Cargo.toml`: reqwest gains the `socks` feature. No new
crate, no lock change (see section 1).
