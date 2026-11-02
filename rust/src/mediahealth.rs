
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
    stage: usize,
    open_until: Option<Instant>,
    probe_started: Option<Instant>,
}

impl Breaker {
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
    recent: HashMap<String, Instant>,
    breaker: Breaker,
}

#[derive(Default)]
struct State {
    files: HashMap<String, FileState>,
    origins: HashMap<String, OriginState>,
}

#[derive(Default)]
pub(crate) struct MediaHealth {
    state: Mutex<State>,
}

impl MediaHealth {
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

    pub(crate) fn record_failure(&self, origin: Option<&str>, file: &str, now: Instant) {
        let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
        if state.files.len() >= MAX_FILES && !state.files.contains_key(file) {
            state.files.clear();
        }
        let f = state.files.entry(file.to_owned()).or_default();
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

pub(crate) fn identify(uri: &matrix_sdk::ruma::MxcUri) -> Option<(String, String)> {
    let server = uri.server_name().ok()?;
    Some((server.to_string(), uri.to_string()))
}

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
        let t1 = t0 + secs(60);
        assert!(h.check(O, "z", t1).is_ok(), "the probe");
        assert!(h.check(O, "y", t1).is_err(), "only one probe at a time");
        h.record_failure(O, "z", t1);
        assert_eq!(h.check(O, "y", t1).unwrap_err(), BACKOFF[1]);
        assert!(h.check(O, "y", t1 + secs(299)).is_err());
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
        assert!(h.check(O, "f", t0 + secs(70)).is_err());
        assert!(h.check(O, "f", t0 + secs(91)).is_ok());
    }
}
