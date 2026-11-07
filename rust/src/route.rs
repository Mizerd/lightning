
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use serde_json::Value;

const DEFAULT_REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum RouteKind {
    Http,
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

pub(crate) fn proxy_url(route: &RouteConfig) -> String {
    let scheme = match route.kind {
        RouteKind::Http => "http",
        RouteKind::Socks5 => "socks5h",
    };
    format!("{scheme}://{}:{}", route.host, route.port)
}

fn client_builder(route: &RouteConfig) -> Result<reqwest::ClientBuilder, &'static str> {
    let mut proxy = reqwest::Proxy::all(proxy_url(route)).map_err(|_| "invalid_route")?;
    if let (Some(user), Some(pass)) = (&route.username, &route.password) {
        proxy = proxy.basic_auth(user, pass);
    }
    Ok(reqwest::Client::builder()
        .user_agent(crate::USER_AGENT)
        .min_tls_version(reqwest::tls::Version::TLS_1_2)
        .timeout(DEFAULT_REQUEST_TIMEOUT)
        // An explicit proxy also turns off the environment's
        // HTTP_PROXY/ALL_PROXY/NO_PROXY handling, so no host is exempted
        // behind the user's back.
        .proxy(proxy))
}

pub(crate) fn http_client(route: &RouteConfig) -> Result<reqwest::Client, &'static str> {
    client_builder(route)?.build().map_err(|_| "route_client_failed")
}

struct Entry {
    owner: usize,
    state: RouteState,
}

fn registry() -> &'static Mutex<HashMap<PathBuf, Entry>> {
    static REGISTRY: OnceLock<Mutex<HashMap<PathBuf, Entry>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) fn key_for(owner: usize, store_path: &Path) -> PathBuf {
    if store_path.as_os_str().is_empty() {
        PathBuf::from(format!("\u{0}lightning-handle-{owner:x}"))
    } else {
        store_path.to_path_buf()
    }
}

pub(crate) fn set(owner: usize, store_path: &Path, state: Option<RouteState>) {
    let key = key_for(owner, store_path);
    let store_path = key.as_path();
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
    let key = key_for(owner, store_path);
    let store_path = key.as_path();
    let mut map = registry().lock().unwrap_or_else(|p| p.into_inner());
    if map.get(store_path).is_some_and(|e| e.owner == owner) {
        map.remove(store_path);
    }
}

pub(crate) fn lookup(store_path: &Path) -> Option<RouteState> {
    let map = registry().lock().unwrap_or_else(|p| p.into_inner());
    map.get(store_path).map(|e| e.state.clone())
}

pub(crate) fn routed(store_path: &Path) -> bool {
    lookup(store_path).is_some()
}

pub(crate) const ROUTE_UNAVAILABLE: &str =
    "network route unavailable: this account's proxy is configured but cannot be used \
     (its password could not be read, or its settings are invalid); not connecting directly";

pub(crate) fn client_for(store_path: &Path) -> Result<Option<reqwest::Client>, &'static str> {
    match lookup(store_path) {
        None => Ok(None),
        Some(RouteState::Unavailable) => Err(ROUTE_UNAVAILABLE),
        Some(RouteState::Configured(route)) => {
            http_client(&route).map(Some).map_err(|_| ROUTE_UNAVAILABLE)
        }
    }
}

/// Like [`client_for`], unbuilt, for a caller that needs its own policy on top.
pub(crate) fn client_for_builder(
    store_path: &Path,
) -> Result<Option<reqwest::ClientBuilder>, &'static str> {
    match lookup(store_path) {
        None => Ok(None),
        Some(RouteState::Unavailable) => Err(ROUTE_UNAVAILABLE),
        Some(RouteState::Configured(route)) => {
            client_builder(&route).map(Some).map_err(|_| ROUTE_UNAVAILABLE)
        }
    }
}

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
        forget(2, &a);
        assert!(routed(&a));
        set(3, &a, Some(RouteState::Unavailable));
        forget(1, &a);
        assert!(routed(&a));
        set(3, &a, None);
        assert!(!routed(&a));
        set(4, Path::new(""), Some(RouteState::Unavailable));
        assert!(!routed(Path::new("")));
        assert!(routed(&key_for(4, Path::new(""))));
        assert!(!routed(&key_for(5, Path::new(""))));
        assert_ne!(key_for(4, Path::new("")), key_for(5, Path::new("")));
        forget(4, Path::new(""));
        assert!(!routed(&key_for(4, Path::new(""))));
    }

    #[test]
    fn a_store_less_handles_memory_client_follows_its_own_route() {
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        let owner = 0xb007usize;
        set(owner, Path::new(""), Some(RouteState::Unavailable));
        let key = key_for(owner, Path::new(""));
        let refused = rt.block_on(crate::build_memory_client("http://127.0.0.1:9", &key));
        assert_eq!(refused.err().as_deref(), Some(ROUTE_UNAVAILABLE));
        let other = key_for(owner + 1, Path::new(""));
        assert!(matches!(client_for(&other), Ok(None)));
        forget(owner, Path::new(""));
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
        assert!(!ROUTE_UNAVAILABLE.contains("127.0.0.1"));
        set(10, &unusable, None);
        set(11, &usable, None);
    }

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

    #[test]
    fn the_sdk_log_filter_never_enables_the_http_transport() {
        for requested in [
            "trace",
            "reqwest=trace",
            "hyper_util=trace,hyper=trace,h2=trace",
            "reqwest::connect=trace",
            "hyper_util::client::legacy::connect::proxy=trace",
            "matrix_sdk=trace,reqwest[{x}]=trace",
            " reqwest = trace ",
        ] {
            let clamped = crate::clamp_transport_tracing(requested);
            let filter = tracing_subscriber::EnvFilter::try_new(&clamped).expect("valid filter");
            let shown = filter.to_string();
            for directive in shown.split(',') {
                let target = directive.split(['=', '[']).next().unwrap_or("").trim();
                for t in ["reqwest", "hyper_util", "hyper", "h2"] {
                    if target == t {
                        assert!(directive.trim().ends_with("=off"), "{requested} -> {shown}");
                    }
                    assert!(!target.starts_with(&format!("{t}::")), "{requested} -> {shown}");
                }
            }
            for t in ["reqwest", "hyper_util", "hyper", "h2"] {
                assert!(shown.contains(&format!("{t}=off")), "{requested} -> {shown}");
            }
        }
        assert!(crate::clamp_transport_tracing("matrix_sdk=debug").starts_with("matrix_sdk=debug,"));
    }

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
