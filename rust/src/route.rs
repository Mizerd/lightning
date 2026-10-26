//! Per-account network route: an explicit HTTP (CONNECT) or SOCKS5 proxy for
//! everything one account's matrix-sdk client sends (docs/network-profiles.md).
//!
//! What it covers: the account's `matrix_sdk::Client` HTTP transport, built
//! here (`http_client`) and handed to the SDK, so discovery, login, sync,
//! E2EE key traffic, media up/download, thumbnails and every Lightning call
//! through `client.http_client()` / `client.send()` follow the route.
//!
//! What it does NOT cover, and is therefore refused for a routed account
//! (`routed()`), never sent directly: the client-side link preview and link
//! media fetch (they pin a validated public IP and send no proxy, so a proxy
//! would void the SSRF checks, and going direct would leak the address the
//! user is hiding), and calls (SFU signalling over a WebSocket and WebRTC
//! media over UDP, which no HTTP/SOCKS proxy here carries). GIF providers and
//! the updater are not account traffic and keep the system setting.
//!
//! Fail closed: a route that is configured but cannot be used (its password
//! could not be read from the keyring, or the configuration is invalid) is
//! recorded as `Unavailable`, and building the account's client is refused;
//! nothing falls back to a direct connection. A proxy that is configured but
//! unreachable makes every request fail (reqwest does not retry without the
//! proxy, and an explicit proxy disables the environment's proxy settings).
//!
//! Secrets: the password arrives once per handle from C++ (read from the
//! keyring there), lives only in this registry, is never logged, never put in
//! a URL string that anything prints, and `Debug` redacts it.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use serde_json::Value;

/// The SDK's own default request timeout (matrix-sdk 0.18 `HttpSettings`),
/// kept so a routed client behaves like an unrouted one. Per-request configs
/// (sync long-polls) still override it.
const DEFAULT_REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum RouteKind {
    /// HTTP proxy, tunnelling HTTPS with CONNECT.
    Http,
    /// SOCKS5 with remote DNS (`socks5h`), so the account's host names are
    /// resolved by the proxy and never by the local resolver.
    Socks5,
}

#[derive(Clone, PartialEq, Eq)]
pub(crate) struct RouteConfig {
    pub(crate) kind: RouteKind,
    pub(crate) host: String,
    pub(crate) port: u16,
    pub(crate) username: Option<String>,
    pub(crate) password: Option<String>,
}

impl std::fmt::Debug for RouteConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RouteConfig")
            .field("kind", &self.kind)
            .field("host", &self.host)
            .field("port", &self.port)
            .field("username", &self.username.as_ref().map(|_| "<set>"))
            .field("password", &self.password.as_ref().map(|_| "<redacted>"))
            .finish()
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub(crate) enum RouteState {
    Configured(RouteConfig),
    /// A route is configured for the account but cannot be used now.
    Unavailable,
}

const MAX_HOST: usize = 253;
const MAX_USERNAME: usize = 256;
const MAX_PASSWORD: usize = 1024;

fn valid_host(host: &str) -> bool {
    if host.is_empty() || host.len() > MAX_HOST {
        return false;
    }
    // A bracketed IPv6 literal, or a name / IPv4 made of host characters
    // only: nothing that could carry a path, userinfo, port or scheme.
    if let Some(inner) = host.strip_prefix('[').and_then(|h| h.strip_suffix(']')) {
        return inner.parse::<std::net::Ipv6Addr>().is_ok();
    }
    host.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-')
        && !host.starts_with('-')
        && !host.starts_with('.')
}

fn clean_secret(value: Option<&Value>, max: usize) -> Result<Option<String>, &'static str> {
    match value.and_then(Value::as_str) {
        None | Some("") => Ok(None),
        Some(s) if s.len() > max || s.chars().any(char::is_control) => Err("invalid_credentials"),
        Some(s) => Ok(Some(s.to_owned())),
    }
}

/// Parses what C++ sends: `{"state":"none"}`, `{"state":"unavailable"}`, or
/// `{"state":"configured","kind":"http"|"socks5","host","port","username"?,
/// "password"?}`. A username needs a password and the other way round.
pub(crate) fn parse(json: &str) -> Result<Option<RouteState>, &'static str> {
    let value: Value = serde_json::from_str(json).map_err(|_| "invalid_route")?;
    match value.get("state").and_then(Value::as_str) {
        Some("none") => return Ok(None),
        Some("unavailable") => return Ok(Some(RouteState::Unavailable)),
        Some("configured") => {}
        _ => return Err("invalid_route"),
    }
    let kind = match value.get("kind").and_then(Value::as_str) {
        Some("http") => RouteKind::Http,
        Some("socks5") => RouteKind::Socks5,
        _ => return Err("invalid_route_kind"),
    };
    let host = value.get("host").and_then(Value::as_str).unwrap_or("").trim().to_owned();
    if !valid_host(&host) {
        return Err("invalid_route_host");
    }
    let port = value.get("port").and_then(Value::as_u64).unwrap_or(0);
    if port == 0 || port > u64::from(u16::MAX) {
        return Err("invalid_route_port");
    }
    let username = clean_secret(value.get("username"), MAX_USERNAME)?;
    let password = clean_secret(value.get("password"), MAX_PASSWORD)?;
    if username.is_some() != password.is_some() {
        return Err("invalid_credentials");
    }
    Ok(Some(RouteState::Configured(RouteConfig {
        kind,
        host,
        port: port as u16,
        username,
        password,
    })))
}

/// The proxy URL WITHOUT credentials (they are attached with
/// `Proxy::basic_auth`, which for SOCKS5 becomes the SOCKS username/password).
pub(crate) fn proxy_url(route: &RouteConfig) -> String {
    let scheme = match route.kind {
        RouteKind::Http => "http",
        RouteKind::Socks5 => "socks5h",
    };
    format!("{scheme}://{}:{}", route.host, route.port)
}

/// The account's HTTP transport: the SDK's own settings (user agent, TLS 1.2
/// minimum, default request timeout, default trust roots, never any
/// certificate leniency) plus exactly one proxy for every request.
pub(crate) fn http_client(route: &RouteConfig) -> Result<reqwest::Client, &'static str> {
    let mut proxy = reqwest::Proxy::all(proxy_url(route)).map_err(|_| "invalid_route")?;
    if let (Some(user), Some(pass)) = (&route.username, &route.password) {
        proxy = proxy.basic_auth(user, pass);
    }
    reqwest::Client::builder()
        .user_agent(crate::USER_AGENT)
        .min_tls_version(reqwest::tls::Version::TLS_1_2)
        .timeout(DEFAULT_REQUEST_TIMEOUT)
        // An explicit proxy also turns off the environment's
        // HTTP_PROXY/ALL_PROXY/NO_PROXY handling, so no host is exempted
        // behind the user's back.
        .proxy(proxy)
        .build()
        .map_err(|_| "route_client_failed")
}

// ---- The per-account route, by store path -----------------------------------
//
// Like the media-store key (mediastore.rs): build_client() takes only a store
// path, so the route is looked up by it. Each entry belongs to the handle that
// set it, so a handle being retired cannot remove its successor's.

struct Entry {
    owner: usize,
    state: RouteState,
}

fn registry() -> &'static Mutex<HashMap<PathBuf, Entry>> {
    static REGISTRY: OnceLock<Mutex<HashMap<PathBuf, Entry>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Records this handle's route for its store (`None`: no route, the system
/// default as before). An empty path (the in-memory bootstrap client of a
/// first sign-in) is never routed: see docs/network-profiles.md.
pub(crate) fn set(owner: usize, store_path: &Path, state: Option<RouteState>) {
    if store_path.as_os_str().is_empty() {
        return;
    }
    let mut map = registry().lock().unwrap_or_else(|p| p.into_inner());
    match state {
        Some(state) => {
            map.insert(store_path.to_path_buf(), Entry { owner, state });
        }
        None => {
            map.remove(store_path);
        }
    }
}

pub(crate) fn forget(owner: usize, store_path: &Path) {
    let mut map = registry().lock().unwrap_or_else(|p| p.into_inner());
    if map.get(store_path).is_some_and(|e| e.owner == owner) {
        map.remove(store_path);
    }
}

pub(crate) fn lookup(store_path: &Path) -> Option<RouteState> {
    let map = registry().lock().unwrap_or_else(|p| p.into_inner());
    map.get(store_path).map(|e| e.state.clone())
}

/// Whether the account behind this store has a route (usable or not): the
/// direct-connection features refuse to run for it.
pub(crate) fn routed(store_path: &Path) -> bool {
    lookup(store_path).is_some()
}

/// The refusal build_client reports for a route that cannot be used.
pub(crate) const ROUTE_UNAVAILABLE: &str =
    "network route unavailable: this account's proxy is configured but cannot be used \
     (its password could not be read, or its settings are invalid); not connecting directly";

/// The HTTP client the account behind `store_path` must use: `Ok(None)` for
/// an account without a route (the SDK builds its default client, as
/// before), `Ok(Some)` for a usable one, and `Err` for a route that cannot
/// be used, which refuses the build rather than going direct.
pub(crate) fn client_for(store_path: &Path) -> Result<Option<reqwest::Client>, &'static str> {
    match lookup(store_path) {
        None => Ok(None),
        Some(RouteState::Unavailable) => Err(ROUTE_UNAVAILABLE),
        Some(RouteState::Configured(route)) => {
            http_client(&route).map(Some).map_err(|_| ROUTE_UNAVAILABLE)
        }
    }
}

/// The refusal a direct-connection feature reports for a routed account.
pub(crate) const BLOCKED_BY_ROUTE: &str = "blocked_by_route";

#[cfg(test)]
mod tests {
    use super::*;

    fn configured(json: &str) -> RouteConfig {
        match parse(json).expect("parses").expect("a route") {
            RouteState::Configured(r) => r,
            RouteState::Unavailable => panic!("unavailable"),
        }
    }

    #[test]
    fn none_unavailable_and_configured_parse() {
        assert_eq!(parse(r#"{"state":"none"}"#), Ok(None));
        assert_eq!(parse(r#"{"state":"unavailable"}"#), Ok(Some(RouteState::Unavailable)));
        let r = configured(r#"{"state":"configured","kind":"socks5","host":"127.0.0.1","port":1080}"#);
        assert_eq!(r.kind, RouteKind::Socks5);
        assert_eq!(proxy_url(&r), "socks5h://127.0.0.1:1080");
        let r = configured(r#"{"state":"configured","kind":"http","host":"proxy.example","port":3128}"#);
        assert_eq!(proxy_url(&r), "http://proxy.example:3128");
        let r = configured(r#"{"state":"configured","kind":"http","host":"[::1]","port":8080}"#);
        assert_eq!(proxy_url(&r), "http://[::1]:8080");
    }

    #[test]
    fn invalid_routes_are_refused_never_guessed() {
        for bad in [
            r#"{}"#,
            r#"{"state":"configured","kind":"ftp","host":"h","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"user@h","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"h/path","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"h:80","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"[nope]","port":1}"#,
            r#"{"state":"configured","kind":"http","host":"h","port":0}"#,
            r#"{"state":"configured","kind":"http","host":"h","port":70000}"#,
            r#"{"state":"configured","kind":"http","host":"h","port":1,"username":"u"}"#,
            r#"{"state":"configured","kind":"http","host":"h","port":1,"password":"p"}"#,
            r#"{"state":"configured","kind":"http","host":"h","port":1,"username":"u\n","password":"p"}"#,
            "not json",
        ] {
            assert!(parse(bad).is_err(), "{bad} must be refused");
        }
    }

    #[test]
    fn credentials_never_reach_the_url_or_debug() {
        let r = configured(
            r#"{"state":"configured","kind":"http","host":"h","port":1,"username":"alice","password":"s3cret-pw"}"#,
        );
        assert!(!proxy_url(&r).contains("alice"));
        assert!(!proxy_url(&r).contains("s3cret-pw"));
        let debug = format!("{r:?}");
        assert!(!debug.contains("s3cret-pw") && !debug.contains("alice"), "{debug}");
    }

    #[test]
    fn a_routed_client_builds_for_both_kinds_and_with_auth() {
        for json in [
            r#"{"state":"configured","kind":"http","host":"127.0.0.1","port":3128}"#,
            r#"{"state":"configured","kind":"socks5","host":"127.0.0.1","port":1080}"#,
            r#"{"state":"configured","kind":"socks5","host":"localhost","port":1080,"username":"u","password":"p"}"#,
        ] {
            assert!(http_client(&configured(json)).is_ok(), "{json}");
        }
    }

    #[test]
    fn the_registry_is_per_store_and_per_owner() {
        let a = PathBuf::from("/tmp/route-test-a");
        let b = PathBuf::from("/tmp/route-test-b");
        set(1, &a, Some(RouteState::Unavailable));
        assert!(routed(&a));
        assert!(!routed(&b));
        // Another handle forgetting does not remove the entry it does not own.
        forget(2, &a);
        assert!(routed(&a));
        // A successor replaces it, and the predecessor's forget leaves it.
        set(3, &a, Some(RouteState::Unavailable));
        forget(1, &a);
        assert!(routed(&a));
        set(3, &a, None);
        assert!(!routed(&a));
        // The in-memory bootstrap path is never routed.
        set(4, Path::new(""), Some(RouteState::Unavailable));
        assert!(!routed(Path::new("")));
    }

    #[test]
    fn the_client_for_a_store_follows_its_route_and_refuses_an_unusable_one() {
        let none = PathBuf::from("/tmp/route-test-client-none");
        let unusable = PathBuf::from("/tmp/route-test-client-unusable");
        let usable = PathBuf::from("/tmp/route-test-client-usable");
        assert!(matches!(client_for(&none), Ok(None)));
        set(10, &unusable, Some(RouteState::Unavailable));
        assert_eq!(client_for(&unusable).err(), Some(ROUTE_UNAVAILABLE));
        set(11, &usable, parse(r#"{"state":"configured","kind":"http","host":"127.0.0.1","port":3128}"#).unwrap());
        assert!(matches!(client_for(&usable), Ok(Some(_))));
        // The refusal names no secret and no host.
        assert!(!ROUTE_UNAVAILABLE.contains("127.0.0.1"));
        set(10, &unusable, None);
        set(11, &usable, None);
    }

    /// A store path whose route is set refuses to build its Matrix client
    /// when the route cannot be used: build_client_with never reaches the
    /// network or the store.
    #[test]
    fn building_a_client_on_an_unusable_route_is_refused() {
        let dir = std::env::temp_dir().join(format!("route-test-build-{}", std::process::id()));
        set(12, &dir, Some(RouteState::Unavailable));
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        let result = rt.block_on(crate::build_client_with(
            crate::HomeserverInput::Url("http://127.0.0.1:9"),
            &dir,
        ));
        set(12, &dir, None);
        let err = result.err().expect("an unusable route refuses the build");
        assert_eq!(err, ROUTE_UNAVAILABLE);
        assert!(!dir.exists(), "no store was created");
    }

    /// Fail closed against a real dead proxy: a request through a proxy on a
    /// closed local port fails; it is not retried directly. The target is a
    /// local server that DOES answer a direct request (the control proves
    /// it), so a direct fallback would turn the failure into a success.
    #[test]
    fn an_unreachable_proxy_fails_instead_of_going_direct() {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let target = listener.local_addr().unwrap();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                let mut buf = [0u8; 1024];
                let _ = stream.read(&mut buf);
                let _ = stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
        });
        // A port nothing listens on: bound, then released.
        let dead = std::net::TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        let url = format!("http://{target}/");

        let direct = reqwest::Client::builder().no_proxy().build().unwrap();
        let control = rt.block_on(async {
            direct.get(&url).timeout(Duration::from_secs(5)).send().await
        });
        assert!(control.is_ok(), "the control: the target answers a direct request");

        for kind in ["socks5", "http"] {
            let client = http_client(&configured(&format!(
                r#"{{"state":"configured","kind":"{kind}","host":"127.0.0.1","port":{dead}}}"#
            )))
            .unwrap();
            let result = rt.block_on(async {
                client.get(&url).timeout(Duration::from_secs(5)).send().await
            });
            assert!(result.is_err(), "{kind}: a dead proxy must not fall back to the direct target");
        }
    }
}
