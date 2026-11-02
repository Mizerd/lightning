//! Changing the account password (`POST /account/password`), and the probe
//! that decides whether Lightning offers it at all.
//!
//! Two requests, as the spec recommends and as uia.rs does: the first carries
//! no auth and learns the UIA session and flows, the second answers the
//! `m.login.password` stage with the current password. Synapse checks the new
//! password against its policy before UIA, so a refused new password never
//! costs the current one a trip. matrix-sdk's `Account::change_password`
//! cannot set `logout_devices` (whose spec default is TRUE), so the ruma
//! request is sent directly: `logout_devices=false` is on the wire in both
//! requests, and true, the default, is omitted by ruma.
//!
//! Secrets: both passwords arrive as owned Strings held in `Secret`, which
//! scrubs on drop, so every path this module controls (errors, a sign-out
//! between the two requests, an aborted task) zeroes Lightning's copies. The
//! copies moved into ruma's requests, the SDK's clone of each request and the
//! serialized HTTP body are freed without zeroing: the same best effort as
//! uia.rs. Neither password is ever logged, enqueued or returned; only a
//! category crosses the FFI.
//!
//! Events:
//!   password_change_probe  { op_id, lifecycle, known, can_change, management_url }
//!   password_change_result { op_id, lifecycle, ok, category }
//! Categories: wrong_password, weak_password, rate_limited, unsupported,
//! network, failed ("" when ok).

use std::ffi::c_void;
use std::os::raw::c_char;
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::api::client::{
        account::change_password,
        discovery::get_authorization_server_metadata::v1::AccountManagementActionData,
        uiaa::{self, AuthData, AuthType, UiaaInfo},
    },
    HttpError,
};
use serde_json::json;

use crate::rooms::require_client;
use crate::{bridge, cstr_arg, enqueue, ffi_string, RustClient};

const PASSWORD_REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const PROBE_TIMEOUT: Duration = Duration::from_secs(20);

/// An owned secret that zeroes its buffer when dropped. Also holds the
/// room-key export passphrase (keyexport.rs).
pub(crate) struct Secret(pub(crate) String);

impl Secret {
    fn take(&mut self) -> String {
        std::mem::take(&mut self.0)
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        scrub(&mut self.0);
    }
}

fn scrub(secret: &mut String) {
    unsafe {
        for byte in secret.as_mut_vec().iter_mut() {
            std::ptr::write_volatile(byte, 0);
        }
    }
    std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
    secret.clear();
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Failure {
    Transport,
    Uiaa {
        password_stage: bool,
        password_done: bool,
    },
    Api { status: u16, errcode: String },
    Other,
}

#[derive(Debug, PartialEq, Eq)]
enum Next {
    AnswerPasswordStage,
    Finish(&'static str),
}

fn password_stage_available(info: &UiaaInfo) -> bool {
    info.flows.iter().any(|flow| {
        flow.stages.iter().any(|s| matches!(s, AuthType::Password))
            && flow
                .stages
                .iter()
                .filter(|stage| !info.completed.contains(stage))
                .all(|stage| matches!(stage, AuthType::Password))
    })
}

fn failure_of(err: &HttpError) -> Failure {
    if let HttpError::Cached(inner) = err {
        return failure_of(inner);
    }
    if let Some(info) = err.as_uiaa_response() {
        return Failure::Uiaa {
            password_stage: password_stage_available(info),
            password_done: info.completed.contains(&AuthType::Password),
        };
    }
    if let Some(api) = err.as_client_api_error() {
        return Failure::Api {
            status: api.status_code.as_u16(),
            errcode: api
                .error_kind()
                .map(|kind| kind.errcode().to_string())
                .unwrap_or_default(),
        };
    }
    match err {
        HttpError::Reqwest(_) => Failure::Transport,
        _ => Failure::Other,
    }
}

fn api_category(status: u16, errcode: &str, password_sent: bool) -> &'static str {
    if errcode == "M_LIMIT_EXCEEDED" || status == 429 {
        "rate_limited"
    } else if errcode == "M_WEAK_PASSWORD" || errcode.starts_with("M_PASSWORD_") {
        "weak_password"
    // A MAS-delegated Synapse does not serve the endpoint at all.
    } else if errcode == "M_UNRECOGNIZED" || status == 404 || status == 405 {
        "unsupported"
    } else if errcode == "M_FORBIDDEN" {
        if password_sent { "wrong_password" } else { "unsupported" }
    } else {
        "failed"
    }
}

fn after_first_attempt(failure: &Failure) -> Next {
    match failure {
        Failure::Uiaa { password_stage: true, .. } => Next::AnswerPasswordStage,
        Failure::Uiaa { password_stage: false, .. } => Next::Finish("unsupported"),
        Failure::Api { status, errcode } => Next::Finish(api_category(*status, errcode, false)),
        Failure::Transport => Next::Finish("network"),
        Failure::Other => Next::Finish("failed"),
    }
}

fn after_password_attempt(failure: &Failure) -> &'static str {
    match failure {
        Failure::Uiaa { password_done: false, .. } => "wrong_password",
        Failure::Uiaa { password_done: true, .. } => "unsupported",
        Failure::Api { status, errcode } => api_category(*status, errcode, true),
        Failure::Transport => "network",
        Failure::Other => "failed",
    }
}

fn change_request(
    new_password: String,
    logout_devices: bool,
    auth: Option<AuthData>,
) -> change_password::v3::Request {
    let mut request = change_password::v3::Request::new(new_password);
    request.logout_devices = logout_devices;
    request.auth = auth;
    request
}

fn result_event(op_id: u64, lifecycle: u64, category: &str) -> serde_json::Value {
    json!({
        "type": "password_change_result",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "ok": category.is_empty(),
        "category": category,
    })
}

fn probe_event(
    op_id: u64,
    lifecycle: u64,
    can_change: Option<bool>,
    management_url: &str,
) -> serde_json::Value {
    json!({
        "type": "password_change_probe",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "known": can_change.is_some(),
        "can_change": can_change.unwrap_or(true),
        "management_url": management_url,
    })
}

async fn run_change(
    client: matrix_sdk::Client,
    own_user: String,
    mut current: Secret,
    mut new: Secret,
    logout_devices: bool,
    still_current: impl Fn() -> bool,
) -> Option<&'static str> {
    let config = RequestConfig::new()
        .disable_retry()
        .timeout(PASSWORD_REQUEST_TIMEOUT);

    let first = client
        .send(change_request(new.0.clone(), logout_devices, None))
        .with_request_config(config)
        .await;
    let session = match &first {
        Ok(_) => return Some(""),
        Err(err) => {
            let failure = failure_of(err);
            if let Next::Finish(category) = after_first_attempt(&failure) {
                return Some(category);
            }
            err.as_uiaa_response().and_then(|info| info.session.clone())
        }
    };
    drop(first);

    if !still_current() {
        return None;
    }

    let mut auth = uiaa::Password::new(
        uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(own_user)),
        current.take(),
    );
    auth.session = session;
    let second = client
        .send(change_request(
            new.take(),
            logout_devices,
            Some(AuthData::Password(auth)),
        ))
        .with_request_config(config)
        .await;
    Some(match second {
        Ok(_) => "",
        Err(err) => after_password_attempt(&failure_of(&err)),
    })
}

fn change_password(
    bridge: &RustClient,
    current: Secret,
    new: Secret,
    logout_devices: bool,
    op_id: u64,
) -> Result<(), String> {
    if current.0.is_empty() || new.0.is_empty() {
        return Err("a password is empty".to_owned());
    }
    let client = require_client(bridge)?;
    let own_user = client
        .user_id()
        .map(|user| user.to_string())
        .ok_or_else(|| "no authenticated user".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let check = Arc::clone(&timelines);
        let outcome = run_change(client, own_user, current, new, logout_devices, move || {
            check.lifecycle_current(lifecycle)
        })
        .await;
        let Some(category) = outcome else { return };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, result_event(op_id, lifecycle, category));
    });
    Ok(())
}

fn probe(bridge: &RustClient, op_id: u64) -> Result<(), String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let answer = tokio::time::timeout(PROBE_TIMEOUT, async {
            let can_change = client
                .homeserver_capabilities()
                .can_change_password()
                .await
                .ok();
            let oauth = matches!(client.auth_api(), Some(matrix_sdk::AuthApi::OAuth(_)));
            if can_change == Some(true) && !oauth {
                return (can_change, String::new());
            }
            let url = match client.oauth().server_metadata().await {
                Ok(metadata) => metadata
                    .account_management_url_with_action(AccountManagementActionData::Profile)
                    .map(|url| url.to_string())
                    .unwrap_or_default(),
                Err(_) => String::new(),
            };
            (can_change, url)
        })
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (can_change, url) = answer.unwrap_or((None, String::new()));
        enqueue(&events, probe_event(op_id, lifecycle, can_change, &url));
    });
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_change_password(
    ptr: *mut c_void,
    current_password: *const c_char,
    new_password: *const c_char,
    logout_devices: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        let current = Secret(unsafe { cstr_arg(current_password) }?);
        let new = Secret(unsafe { cstr_arg(new_password) }?);
        change_password(handle, current, new, logout_devices, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_password_change_probe(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let handle = unsafe { bridge(ptr)? };
        probe(handle, op_id).map(|_| String::new())
    })
}

#[cfg(test)]
mod tests {
    use std::borrow::Cow;
    use std::collections::BTreeMap;

    use matrix_sdk::ruma::api::{
        auth_scheme::SendAccessToken, client::uiaa::AuthFlow, OutgoingRequest,
        SupportedVersions,
    };

    use super::*;

    fn api(status: u16, errcode: &str) -> Failure {
        Failure::Api { status, errcode: errcode.to_owned() }
    }

    fn body_of(request: change_password::v3::Request) -> serde_json::Value {
        let versions = SupportedVersions::from_parts(&["v1.11".to_owned()], &BTreeMap::new());
        let http = request
            .try_into_http_request::<Vec<u8>>(
                "https://hs.example",
                SendAccessToken::IfRequired("token"),
                Cow::Owned(versions),
            )
            .expect("request serializes");
        serde_json::from_slice(http.body()).expect("json body")
    }

    #[test]
    fn a_password_challenge_is_answered_and_other_stages_are_not() {
        let challenge = Failure::Uiaa { password_stage: true, password_done: false };
        assert_eq!(after_first_attempt(&challenge), Next::AnswerPasswordStage);
        let sso_only = Failure::Uiaa { password_stage: false, password_done: false };
        assert_eq!(after_first_attempt(&sso_only), Next::Finish("unsupported"));
    }

    #[test]
    fn the_first_attempt_maps_server_refusals() {
        let first = |status: u16, errcode: &str| after_first_attempt(&api(status, errcode));
        assert_eq!(first(400, "M_WEAK_PASSWORD"), Next::Finish("weak_password"));
        assert_eq!(first(400, "M_PASSWORD_TOO_SHORT"), Next::Finish("weak_password"));
        assert_eq!(first(429, "M_LIMIT_EXCEEDED"), Next::Finish("rate_limited"));
        assert_eq!(first(429, ""), Next::Finish("rate_limited"));
        assert_eq!(first(404, "M_UNRECOGNIZED"), Next::Finish("unsupported"));
        assert_eq!(first(405, ""), Next::Finish("unsupported"));
        assert_eq!(first(403, "M_FORBIDDEN"), Next::Finish("unsupported"));
        assert_eq!(first(500, "M_UNKNOWN"), Next::Finish("failed"));
        assert_eq!(after_first_attempt(&Failure::Transport), Next::Finish("network"));
        assert_eq!(after_first_attempt(&Failure::Other), Next::Finish("failed"));
    }

    #[test]
    fn the_password_attempt_maps_a_refused_password() {
        let refused = Failure::Uiaa { password_stage: true, password_done: false };
        assert_eq!(after_password_attempt(&refused), "wrong_password");
        assert_eq!(after_password_attempt(&api(403, "M_FORBIDDEN")), "wrong_password");
        let more = Failure::Uiaa { password_stage: false, password_done: true };
        assert_eq!(after_password_attempt(&more), "unsupported");
        assert_eq!(after_password_attempt(&api(429, "M_LIMIT_EXCEEDED")), "rate_limited");
        assert_eq!(after_password_attempt(&api(400, "M_WEAK_PASSWORD")), "weak_password");
        assert_eq!(after_password_attempt(&Failure::Transport), "network");
        assert_eq!(after_password_attempt(&api(502, "")), "failed");
    }

    #[test]
    fn the_password_stage_must_be_able_to_finish_a_flow() {
        let password = UiaaInfo::new(vec![AuthFlow::new(vec![AuthType::Password])]);
        assert!(password_stage_available(&password));
        let with_dummy =
            UiaaInfo::new(vec![AuthFlow::new(vec![AuthType::Password, AuthType::Dummy])]);
        assert!(!password_stage_available(&with_dummy));
        let sso = UiaaInfo::new(vec![AuthFlow::new(vec![AuthType::Sso])]);
        assert!(!password_stage_available(&sso));
        let then_email =
            UiaaInfo::new(vec![AuthFlow::new(vec![AuthType::Password, AuthType::EmailIdentity])]);
        assert!(!password_stage_available(&then_email));
        let either = UiaaInfo::new(vec![
            AuthFlow::new(vec![AuthType::Sso]),
            AuthFlow::new(vec![AuthType::Password]),
        ]);
        assert!(password_stage_available(&either));
    }

    #[test]
    fn keeping_devices_is_explicit_on_the_wire() {
        let keep = body_of(change_request("n".to_owned(), false, None));
        assert_eq!(keep["logout_devices"], json!(false));
        assert!(keep.get("auth").is_none());
        let sign_out = body_of(change_request("n".to_owned(), true, None));
        assert_ne!(sign_out.get("logout_devices"), Some(&json!(false)));
    }

    #[test]
    fn the_password_stage_carries_the_session_and_the_user() {
        let mut auth = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(
                "@a:hs.example".to_owned(),
            )),
            "current".to_owned(),
        );
        auth.session = Some("S1".to_owned());
        let body = body_of(change_request("n".to_owned(), false, Some(AuthData::Password(auth))));
        assert_eq!(body["auth"]["type"], json!("m.login.password"));
        assert_eq!(body["auth"]["session"], json!("S1"));
        assert_eq!(body["auth"]["identifier"]["user"], json!("@a:hs.example"));
    }

    #[test]
    fn events_carry_categories_only() {
        let ok = result_event(7, 3, "");
        assert_eq!(ok["ok"], json!(true));
        let failed = result_event(7, 3, "wrong_password");
        assert_eq!(failed["ok"], json!(false));
        assert_eq!(failed["category"], json!("wrong_password"));
        let keys: Vec<&str> = failed.as_object().unwrap().keys().map(String::as_str).collect();
        assert_eq!(keys.len(), 5, "unexpected keys {keys:?}");

        let unknown = probe_event(8, 3, None, "");
        assert_eq!(unknown["known"], json!(false));
        assert_eq!(unknown["can_change"], json!(true));
        let mas = probe_event(8, 3, Some(false), "https://auth.example/account/");
        assert_eq!(mas["can_change"], json!(false));
        assert_eq!(mas["management_url"], json!("https://auth.example/account/"));
    }

    #[test]
    fn a_taken_secret_leaves_nothing_and_scrub_zeroes_the_buffer() {
        let mut secret = Secret("hunter2".to_owned());
        let taken = secret.take();
        assert_eq!(taken, "hunter2");
        assert!(secret.0.is_empty());

        let mut raw = "hunter2".to_owned();
        let len = raw.len();
        scrub(&mut raw);
        assert!(raw.is_empty());
        assert!(raw.capacity() >= len);
        let bytes = unsafe { std::slice::from_raw_parts(raw.as_ptr(), len) };
        assert!(bytes.iter().all(|b| *b == 0));
    }
}

#[cfg(test)]
mod round_trip_tests {
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };
    use serde_json::{json, Value};

    use super::*;

    const ME: &str = "@me:example.org";

    #[derive(Clone, Copy)]
    enum Answer {
        Accept,
        RefusePassword,
        WeakPassword,
    }

    type Log = Arc<Mutex<Vec<(String, String, String)>>>;

    fn read_request(stream: &mut TcpStream) -> (String, String, String) {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        let head_end = loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                return Default::default();
            }
            buf.extend_from_slice(&chunk[..n]);
            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                break pos + 4;
            }
        };
        let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
        let length = head
            .lines()
            .find_map(|line| {
                let line = line.to_ascii_lowercase();
                line.strip_prefix("content-length:")
                    .map(|v| v.trim().parse::<usize>().unwrap_or(0))
            })
            .unwrap_or(0);
        while buf.len() < head_end + length {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        let mut first = head.lines().next().unwrap_or("").split_whitespace();
        let method = first.next().unwrap_or("").to_owned();
        let path = first.next().unwrap_or("").to_owned();
        let end = (head_end + length).min(buf.len());
        let body = String::from_utf8_lossy(&buf[head_end..end]).to_string();
        (method, path, body)
    }

    fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    fn handle(mut stream: TcpStream, answer: Answer, log: Log) {
        let (method, path, body) = read_request(&mut stream);
        let path_only = path.split('?').next().unwrap_or("").to_owned();
        log.lock().unwrap().push((method.clone(), path_only.clone(), body.clone()));
        if path_only == "/_matrix/client/versions" {
            respond(
                &mut stream,
                "200 OK",
                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#,
            );
        } else if method == "POST" && path_only == "/_matrix/client/v3/account/password" {
            let sent: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
            let challenge = r#"{"flows":[{"stages":["m.login.password"]}],"params":{},"session":"S1"}"#;
            if sent.get("auth").is_none() {
                match answer {
                    Answer::WeakPassword => respond(
                        &mut stream,
                        "400 Bad Request",
                        r#"{"errcode":"M_WEAK_PASSWORD","error":"too weak"}"#,
                    ),
                    _ => respond(&mut stream, "401 Unauthorized", challenge),
                }
            } else {
                match answer {
                    Answer::RefusePassword => respond(
                        &mut stream,
                        "401 Unauthorized",
                        r#"{"flows":[{"stages":["m.login.password"]}],"params":{},"session":"S1","errcode":"M_FORBIDDEN","error":"Invalid password"}"#,
                    ),
                    _ => respond(&mut stream, "200 OK", "{}"),
                }
            }
        } else {
            respond(
                &mut stream,
                "404 Not Found",
                r#"{"errcode":"M_UNRECOGNIZED","error":"Unrecognized request"}"#,
            );
        }
    }

    fn serve(answer: Answer, log: Log) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                match stream {
                    Ok(s) => {
                        let log = Arc::clone(&log);
                        thread::spawn(move || handle(s, answer, log));
                    }
                    Err(_) => break,
                }
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    async fn signed_in_client(addr: &str) -> matrix_sdk::Client {
        let client = matrix_sdk::Client::builder()
            .homeserver_url(format!("http://{addr}"))
            .build()
            .await
            .expect("client");
        let session = MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from(ME).expect("user id"),
                device_id: OwnedDeviceId::from("TESTDEVICE"),
            },
            tokens: SessionTokens { access_token: "test-token".to_owned(), refresh_token: None },
        };
        client
            .matrix_auth()
            .restore_session(session, RoomLoadSettings::default())
            .await
            .expect("restore");
        client
    }

    fn change(
        answer: Answer,
        logout_devices: bool,
        still_current: bool,
    ) -> (Option<&'static str>, Vec<Value>) {
        let log: Log = Arc::new(Mutex::new(Vec::new()));
        let addr = serve(answer, Arc::clone(&log));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let outcome = runtime.block_on(async {
            let client = signed_in_client(&addr).await;
            run_change(
                client,
                ME.to_owned(),
                Secret("old-secret".to_owned()),
                Secret("new-secret".to_owned()),
                logout_devices,
                move || still_current,
            )
            .await
        });
        let bodies = log
            .lock()
            .unwrap()
            .iter()
            .filter(|(m, p, _)| m == "POST" && p == "/_matrix/client/v3/account/password")
            .map(|(_, _, b)| serde_json::from_str(b).expect("json body"))
            .collect();
        (outcome, bodies)
    }

    #[test]
    fn the_password_stage_echoes_the_session_and_keeps_the_devices_choice() {
        let (outcome, bodies) = change(Answer::Accept, false, true);
        assert_eq!(outcome, Some(""));
        assert_eq!(bodies.len(), 2, "{bodies:?}");
        assert!(bodies[0].get("auth").is_none());
        assert_eq!(bodies[0]["new_password"], json!("new-secret"));
        let auth = &bodies[1]["auth"];
        assert_eq!(auth["type"], json!("m.login.password"));
        assert_eq!(auth["session"], json!("S1"));
        assert_eq!(auth["identifier"]["type"], json!("m.id.user"));
        assert_eq!(auth["identifier"]["user"], json!(ME));
        assert_eq!(auth["password"], json!("old-secret"));
        assert_eq!(bodies[1]["new_password"], json!("new-secret"));
        for body in &bodies {
            assert_eq!(body["logout_devices"], json!(false), "{body}");
        }
    }

    #[test]
    fn signing_out_other_devices_leaves_the_flag_at_its_default() {
        let (outcome, bodies) = change(Answer::Accept, true, true);
        assert_eq!(outcome, Some(""));
        assert_eq!(bodies.len(), 2);
        for body in &bodies {
            assert_ne!(body.get("logout_devices"), Some(&json!(false)), "{body}");
        }
    }

    #[test]
    fn a_session_that_ended_mid_round_never_sends_the_password() {
        let (outcome, bodies) = change(Answer::Accept, false, false);
        assert_eq!(outcome, None);
        assert_eq!(bodies.len(), 1, "{bodies:?}");
        assert!(!bodies[0].to_string().contains("old-secret"));
    }

    #[test]
    fn a_refused_password_and_a_weak_one_are_named() {
        let (outcome, bodies) = change(Answer::RefusePassword, false, true);
        assert_eq!(outcome, Some("wrong_password"));
        assert_eq!(bodies.len(), 2);

        let (outcome, bodies) = change(Answer::WeakPassword, false, true);
        assert_eq!(outcome, Some("weak_password"));
        assert_eq!(bodies.len(), 1);
        assert!(!bodies[0].to_string().contains("old-secret"));
    }
}
