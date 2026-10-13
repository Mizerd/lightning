//! A circuit breaker for media downloads, per media origin and per file.
//!
//! A media server that is down answers slowly or not at all, and every fetch
//! to it holds one of MediaBridge's eight slots until its timeout. A timeline
//! full of images from one dead origin then stalls everything else behind it.
//! After a few failures this module REFUSES further requests to that origin
//! (or that one file) instantly, so the UI can say so and move on.
//!
//! Policy (after Sable's `media_health.rs`):
//! * per FILE: 3 consecutive failures open that file's breaker;
//! * per ORIGIN (the mxc server name): 3 DISTINCT files failing within 10
//!   minutes open the origin's breaker. One broken file is not a dead server;
//! * an open breaker refuses for 1 minute, then 5 minutes for every further
//!   trip, then lets exactly ONE request through (half-open). That probe's
//!   success closes the breaker and forgets the history; its failure re-opens
//!   it at the next backoff;
//! * a probe that never reports (its task was aborted) is treated as lost
//!   after `PROBE_LOST_AFTER`, so a breaker cannot stick half-open.
//!
//! Only failures that say the SERVER (or the path to it) is unhealthy count:
//! a 5xx or a transport error. A 404, a refusal, a size cap or a decrypt
//! failure is a statement about one file, not about the server, and never
//! feeds the breaker. This module changes no validation: it only decides
//! whether a request that would have been made is made now.

use std::collections::HashMap;
use std::sync::Mutex;
#[cfg(not(test))]
use std::sync::OnceLock;
use std::time::{Duration, Instant};

pub(crate) const FAILURE_THRESHOLD: usize = 3;
pub(crate) const ORIGIN_WINDOW: Duration = Duration::from_secs(10 * 60);
pub(crate) const BACKOFF: [Duration; 2] = [Duration::from_secs(60), Duration::from_secs(5 * 60)];
pub(crate) const PROBE_LOST_AFTER: Duration = Duration::from_secs(60);
const MAX_FILES: usize = 4096;
const MAX_ORIGINS: usize = 256;

/// The error a refused request fails with. `fetch_uncached` wraps it in
/// `matrix_sdk::Error::UnknownError`; the classifier downcasts it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct BreakerOpen {
    pub retry_after_ms: u64,
}

impl std::fmt::Display for BreakerOpen {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "media server unreachable (breaker open, retry in {} ms)", self.retry_after_ms)
    }
}

impl std::error::Error for BreakerOpen {}

#[derive(Debug, Default)]
struct Breaker {
    /// Index into `BACKOFF` of the CURRENT (or most recent) open period.
    stage: usize,
    open_until: Option<Instant>,
    probe_started: Option<Instant>,
}

impl Breaker {
    /// Read-only: may a request go now, or how long until one may.
    fn peek(&self, now: Instant) -> Result<(), Duration> {
        let Some(until) = self.open_until else {
            return Ok(());
        };
        if now < until {
            return Err(until - now);
        }
        match self.probe_started {
            Some(started) if now.duration_since(started) < PROBE_LOST_AFTER => {
                Err(PROBE_LOST_AFTER - now.duration_since(started))
            }
            _ => Ok(()),
        }
    }

    /// A request is being let through: if half-open, it is THE probe.
    fn admit(&mut self, now: Instant) {
        if self.open_until.is_some_and(|until| now >= until) {
            self.probe_started = Some(now);
        }
    }

    fn half_open(&self, now: Instant) -> bool {
        self.open_until.is_some_and(|until| now >= until)
    }

    fn is_open(&self) -> bool {
        self.open_until.is_some()
    }

    fn trip(&mut self, now: Instant) {
        self.stage = if self.open_until.is_some() {
            (self.stage + 1).min(BACKOFF.len() - 1)
        } else {
            0
        };
        self.open_until = Some(now + BACKOFF[self.stage]);
        self.probe_started = None;
    }
}

#[derive(Debug, Default)]
struct FileState {
    consecutive: usize,
    breaker: Breaker,
}

#[derive(Debug, Default)]
struct OriginState {
    /// Distinct files that failed, with when they last did.
    recent: HashMap<String, Instant>,
    breaker: Breaker,
}

#[derive(Default)]
struct State {
    files: HashMap<String, FileState>,
    origins: HashMap<String, OriginState>,
}

/// The breaker table. Cheap to lock: every operation is a few map lookups.
#[derive(Default)]
pub(crate) struct MediaHealth {
    state: Mutex<State>,
}

impl MediaHealth {
    /// `Ok` to make the request, or `Err(retry_after)`. Letting a request
    /// through a half-open breaker makes it the probe.
    pub(crate) fn check(&self, origin: Option<&str>, file: &str, now: Instant) -> Result<(), Duration> {
        let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
        let file_wait = state.files.get(file).map(|f| f.breaker.peek(now)).unwrap_or(Ok(()));
        let origin_wait = origin
            .and_then(|o| state.origins.get(o))
            .map(|o| o.breaker.peek(now))
            .unwrap_or(Ok(()));
        match (file_wait, origin_wait) {
            (Err(a), Err(b)) => return Err(a.max(b)),
            (Err(a), Ok(())) | (Ok(()), Err(a)) => return Err(a),
            (Ok(()), Ok(())) => {}
        }
        if let Some(f) = state.files.get_mut(file) {
            f.breaker.admit(now);
        }
        if let Some(o) = origin.and_then(|o| state.origins.get_mut(o)) {
            o.breaker.admit(now);
        }
        Ok(())
    }

    /// A fetch from `origin` succeeded: the file is healthy, and so is the
    /// origin, whatever its history.
    pub(crate) fn record_success(&self, origin: Option<&str>, file: &str) {
        let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
        state.files.remove(file);
        if let Some(o) = origin.and_then(|o| state.origins.get_mut(o)) {
            if o.breaker.is_open() {
                o.breaker = Breaker::default();
                o.recent.clear();
            } else {
                o.recent.remove(file);
            }
        }
    }

    /// A fetch failed in a way that says the server is unhealthy.
    pub(crate) fn record_failure(&self, origin: Option<&str>, file: &str, now: Instant) {
        let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
        if state.files.len() >= MAX_FILES && !state.files.contains_key(file) {
            state.files.clear(); // defensive bound
        }
        let f = state.files.entry(file.to_owned()).or_default();
        // A straggler that was already in flight when the breaker opened adds
        // nothing: the period is already running.
        let file_open_and_waiting = f.breaker.is_open() && !f.breaker.half_open(now);
        if !file_open_and_waiting {
            f.consecutive += 1;
            if f.breaker.half_open(now) || f.consecutive >= FAILURE_THRESHOLD {
                f.breaker.trip(now);
            }
        }

        let Some(origin) = origin else { return };
        if state.origins.len() >= MAX_ORIGINS && !state.origins.contains_key(origin) {
            state.origins.clear();
        }
        let o = state.origins.entry(origin.to_owned()).or_default();
        if o.breaker.is_open() && !o.breaker.half_open(now) {
            return;
        }
        o.recent.retain(|_, at| now.duration_since(*at) < ORIGIN_WINDOW);
        o.recent.insert(file.to_owned(), now);
        if o.breaker.half_open(now) || o.recent.len() >= FAILURE_THRESHOLD {
            o.breaker.trip(now);
            o.recent.clear();
        }
    }
}

/// The process-wide table. Origins are global facts (a dead server is dead for
/// every account), so one table serves them all.
#[cfg(not(test))]
pub(crate) fn global() -> &'static MediaHealth {
    static HEALTH: OnceLock<MediaHealth> = OnceLock::new();
    HEALTH.get_or_init(MediaHealth::default)
}

/// Tests: one table per test THREAD (`#[tokio::test]` runs on its own), so a
/// test that makes a mock origin fail with 500s cannot trip the breaker for
/// the next test that reads the same origin name.
#[cfg(test)]
pub(crate) fn global() -> &'static MediaHealth {
    thread_local! {
        static HEALTH: &'static MediaHealth = Box::leak(Box::default());
    }
    HEALTH.with(|h| *h)
}

/// The origin (mxc server name) and file identity of a media source, or None
/// for a URI that is not a well-formed mxc (those never reach the network).
pub(crate) fn identify(uri: &matrix_sdk::ruma::MxcUri) -> Option<(String, String)> {
    let server = uri.server_name().ok()?;
    Some((server.to_string(), uri.to_string()))
}

/// A request that outran its overall timeout (the future was dropped before
/// it could report): the server did not answer in time, which counts.
pub(crate) fn record_timeout(identity: &Option<(String, String)>) {
    if let Some((origin, file)) = identity {
        global().record_failure(Some(origin), file, Instant::now());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const O: Option<&str> = Some("dead.example");

    fn secs(n: u64) -> Duration {
        Duration::from_secs(n)
    }

    #[test]
    fn three_distinct_files_open_the_origin_and_a_fourth_is_refused_at_once() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for f in ["a", "b"] {
            h.record_failure(O, f, t0);
            assert!(h.check(O, "fresh", t0).is_ok(), "two files are not a dead server");
        }
        h.record_failure(O, "c", t0);
        let wait = h.check(O, "never-seen", t0).expect_err("origin is open");
        assert_eq!(wait, BACKOFF[0]);
        // Another origin is untouched.
        assert!(h.check(Some("fine.example"), "x", t0).is_ok());
    }

    #[test]
    fn one_file_failing_repeatedly_opens_only_that_file() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for _ in 0..3 {
            h.record_failure(O, "bad", t0);
        }
        assert!(h.check(O, "bad", t0).is_err());
        assert!(h.check(O, "other", t0).is_ok(), "one broken file is not a dead server");
    }

    #[test]
    fn failures_outside_the_window_do_not_add_up() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        h.record_failure(O, "a", t0);
        h.record_failure(O, "b", t0 + secs(30));
        h.record_failure(O, "c", t0 + ORIGIN_WINDOW + secs(31));
        assert!(h.check(O, "d", t0 + ORIGIN_WINDOW + secs(31)).is_ok());
    }

    #[test]
    fn backoff_is_one_minute_then_five_and_a_probe_is_single() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for f in ["a", "b", "c"] {
            h.record_failure(O, f, t0);
        }
        assert!(h.check(O, "z", t0 + secs(59)).is_err());
        // Half-open: exactly one request goes, the next waits for its verdict.
        let t1 = t0 + secs(60);
        assert!(h.check(O, "z", t1).is_ok(), "the probe");
        assert!(h.check(O, "y", t1).is_err(), "only one probe at a time");
        // The probe fails: re-opened for FIVE minutes.
        h.record_failure(O, "z", t1);
        assert_eq!(h.check(O, "y", t1).unwrap_err(), BACKOFF[1]);
        assert!(h.check(O, "y", t1 + secs(299)).is_err());
        // And it stays at five minutes after another failed probe.
        let t2 = t1 + secs(300);
        assert!(h.check(O, "y", t2).is_ok());
        h.record_failure(O, "y", t2);
        assert_eq!(h.check(O, "x", t2).unwrap_err(), BACKOFF[1]);
    }

    #[test]
    fn a_successful_probe_closes_the_origin_and_forgets_its_history() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for f in ["a", "b", "c"] {
            h.record_failure(O, f, t0);
        }
        let t1 = t0 + secs(60);
        assert!(h.check(O, "z", t1).is_ok());
        h.record_success(O, "z");
        assert!(h.check(O, "anything", t1).is_ok());
        // History is gone: it takes three fresh files to open it again, and
        // the first trip is back to the SHORT backoff.
        h.record_failure(O, "p", t1);
        h.record_failure(O, "q", t1);
        assert!(h.check(O, "r", t1).is_ok());
        h.record_failure(O, "r", t1);
        assert_eq!(h.check(O, "s", t1).unwrap_err(), BACKOFF[0]);
    }

    #[test]
    fn a_probe_that_never_reports_is_lost_not_stuck() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for f in ["a", "b", "c"] {
            h.record_failure(O, f, t0);
        }
        let t1 = t0 + secs(60);
        assert!(h.check(O, "probe", t1).is_ok());
        assert!(h.check(O, "next", t1 + secs(59)).is_err());
        assert!(h.check(O, "next", t1 + PROBE_LOST_AFTER).is_ok());
    }

    #[test]
    fn stragglers_in_flight_do_not_extend_an_open_period() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for f in ["a", "b", "c"] {
            h.record_failure(O, f, t0);
        }
        // Fetches already running when it opened fail later.
        h.record_failure(O, "d", t0 + secs(20));
        h.record_failure(O, "e", t0 + secs(21));
        assert!(h.check(O, "z", t0 + secs(60)).is_ok(), "still the one-minute period");
    }

    #[test]
    fn an_unknown_origin_still_has_a_per_file_breaker() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for _ in 0..3 {
            h.record_failure(None, "f", t0);
        }
        assert!(h.check(None, "f", t0).is_err());
        assert!(h.check(None, "g", t0).is_ok());
    }

    #[test]
    fn a_file_probe_is_not_consumed_by_a_refusing_origin() {
        let h = MediaHealth::default();
        let t0 = Instant::now();
        for _ in 0..3 {
            h.record_failure(O, "f", t0);
        }
        for f in ["a", "b", "c"] {
            h.record_failure(O, f, t0 + secs(30));
        }
        // The file's period is over at 60 s but the origin's runs to 90 s.
        assert!(h.check(O, "f", t0 + secs(70)).is_err());
        // When the origin's period ends, the FILE's probe is still available.
        assert!(h.check(O, "f", t0 + secs(91)).is_ok());
    }
}
