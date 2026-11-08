
use std::collections::{BTreeSet, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::localsearch::{DeepOutcome, Gate, PageGate};

pub(crate) const PROGRESS_FILE: &str = "lightning-index-all.json";
const PROGRESS_VERSION: u32 = 2;

pub(crate) const PAGE_GAP_MS: u64 = 5_000;
pub(crate) const ROOM_GAP_MS: u64 = 10_000;
pub(crate) const POLL_MS: u64 = 500;
pub(crate) const MIN_BACKOFF_MS: u64 = 2_000;
pub(crate) const RATE_LIMITED_FLOOR_MS: u64 = 30_000;
pub(crate) const MAX_BACKOFF_MS: u64 = 30 * 60 * 1000;
pub(crate) const MAX_ROOM_ATTEMPTS: u32 = 3;
pub(crate) const MAX_RATE_LIMITED_ATTEMPTS: u32 = 8;
pub(crate) const MAX_QUEUE: usize = 5_000;
pub(crate) const MAX_COMPLETE: usize = 20_000;

pub(crate) const HOLD_CALL: u32 = 1;
pub(crate) const HOLD_SCROLL: u32 = 2;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub(crate) struct Progress {
    #[serde(default)]
    pub version: u32,
    #[serde(default)]
    pub queue: Vec<String>,
    #[serde(default)]
    pub position: usize,
    #[serde(default)]
    pub paused: bool,
    #[serde(default)]
    pub complete: BTreeSet<String>,
    #[serde(default)]
    pub written: u64,
    #[serde(default)]
    pub skipped: u64,
    #[serde(default)]
    pub undecryptable: u64,
    #[serde(default)]
    pub undecryptable_rooms: u64,
    #[serde(default)]
    pub failed_rooms: u64,
    #[serde(default)]
    pub elapsed_ms: u64,
    /// Wall-clock ms of the last run that left nothing to walk; 0 = never.
    #[serde(default)]
    pub finished_ms: u64,
    /// Rooms last walked with undecryptable history and no room key since: a
    /// background start skips them, a user-started run does not.
    #[serde(default)]
    pub utd_rooms: BTreeSet<String>,
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
        .unwrap_or(0)
}

impl Progress {
    pub(crate) fn pending(&self) -> bool {
        self.position < self.queue.len()
    }

    pub(crate) fn current(&self) -> Option<&str> {
        self.queue.get(self.position).map(String::as_str)
    }

    fn sanitized(mut self) -> Self {
        self.queue.truncate(MAX_QUEUE);
        if self.position > self.queue.len() {
            self.position = self.queue.len();
        }
        while self.complete.len() > MAX_COMPLETE {
            let Some(last) = self.complete.iter().next_back().cloned() else {
                break;
            };
            self.complete.remove(&last);
        }
        self
    }

    fn reset_run(&mut self) {
        self.queue.clear();
        self.position = 0;
        self.paused = false;
        self.written = 0;
        self.skipped = 0;
        self.undecryptable = 0;
        self.undecryptable_rooms = 0;
        self.failed_rooms = 0;
        self.elapsed_ms = 0;
    }
}

pub(crate) fn plan_queue(
    rooms: Vec<(String, u64, bool)>,
    complete: &BTreeSet<String>,
) -> (Vec<String>, u64) {
    let mut seen = HashSet::new();
    let mut skipped = 0u64;
    let mut wanted: Vec<(String, u64)> = Vec::with_capacity(rooms.len());
    for (id, stamp, encrypted) in rooms {
        if !encrypted || id.is_empty() || !seen.insert(id.clone()) {
            continue;
        }
        if complete.contains(&id) {
            skipped += 1;
            continue;
        }
        wanted.push((id, stamp));
    }
    wanted.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    wanted.truncate(MAX_QUEUE);
    (wanted.into_iter().map(|(id, _)| id).collect(), skipped)
}

pub(crate) fn backoff_ms(rate_limited: bool, retry_after_ms: Option<u64>, attempt: u32) -> u64 {
    let shift = attempt.clamp(1, 16) - 1;
    let exponential = MIN_BACKOFF_MS.saturating_mul(1u64 << shift);
    let chosen = match retry_after_ms {
        Some(hint) => hint.max(MIN_BACKOFF_MS),
        None if rate_limited => exponential.max(RATE_LIMITED_FLOOR_MS),
        None => exponential,
    };
    chosen.min(MAX_BACKOFF_MS)
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct Status {
    pub state: &'static str,
    pub total: usize,
    pub position: usize,
    pub current_room: String,
    pub written: u64,
    pub skipped: u64,
    pub undecryptable: u64,
    pub undecryptable_rooms: u64,
    pub failed_rooms: u64,
    pub complete_rooms: usize,
    pub retry_in_ms: u64,
    pub elapsed_ms: u64,
    pub finished_ms: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Begin {
    Started,
    AlreadyRunning,
    NothingPending,
    PausedPending,
    NothingToDo,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Ending {
    Finished,
    Paused,
    Cancelled,
    Stopped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RoomResult {
    Done {
        written: usize,
        undecryptable: usize,
    },
    Interrupted { written: usize },
    Failed {
        rate_limited: bool,
        retry_after_ms: Option<u64>,
    },
    Gone,
}

#[derive(Debug, Default)]
struct Inner {
    running: bool,
    pause_requested: bool,
    cancel_requested: bool,
    /// Bumped when the index is cleared: a room finished under an older
    /// epoch must not be recorded as complete in the new, empty index.
    epoch: u64,
    progress: Option<Progress>,
    in_room_written: u64,
    retry_in_ms: u64,
    held_reported: bool,
}

/// The per-account control block. One per bridge, i.e. per store directory,
/// so a run can only ever write the index and record of its own account.
pub(crate) struct Control {
    path: Option<PathBuf>,
    inner: Mutex<Inner>,
    hold: AtomicU32,
}

impl Control {
    pub(crate) fn new(store_dir: &Path) -> Self {
        let path = if store_dir.as_os_str().is_empty() {
            None
        } else {
            Some(store_dir.join(PROGRESS_FILE))
        };
        Self {
            path,
            inner: Mutex::new(Inner::default()),
            hold: AtomicU32::new(0),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn loaded<'a>(&self, inner: &'a mut Inner) -> &'a mut Progress {
        if inner.progress.is_none() {
            inner.progress = Some(self.read_file());
        }
        inner.progress.get_or_insert_with(Progress::default)
    }

    fn read_file(&self) -> Progress {
        let Some(path) = self.path.as_ref() else {
            return Progress::default();
        };
        let Ok(bytes) = std::fs::read(path) else {
            return Progress::default();
        };
        let mut progress = serde_json::from_slice::<Progress>(&bytes)
            .map(Progress::sanitized)
            .unwrap_or_default();
        if progress.version < 2 {
            // Rooms completed before file names were indexed were walked
            // without them, and `plan_queue` would skip them for ever. Forget
            // them once. Nothing is started here: the queue, position and
            // paused flag are untouched, and the walk happens only when the
            // user starts a run, at the existing pacing. The upgraded version
            // is written by the next persist.
            progress.complete.clear();
            progress.version = PROGRESS_VERSION;
        }
        progress
    }

    /// Write the record atomically (temp file, then rename), 0600 on unix.
    /// Never creates the directory: a store deleted under a late write stays
    /// deleted.
    fn persist(&self, progress: &Progress) {
        let Some(path) = self.path.as_ref() else {
            return;
        };
        let mut record = progress.clone();
        record.version = PROGRESS_VERSION;
        let Ok(bytes) = serde_json::to_vec(&record) else {
            return;
        };
        let tmp = path.with_extension("json.tmp");
        let written = (|| -> std::io::Result<()> {
            let mut options = std::fs::OpenOptions::new();
            options.write(true).create(true).truncate(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                options.mode(0o600);
            }
            let mut file = options.open(&tmp)?;
            file.write_all(&bytes)?;
            file.flush()?;
            drop(file);
            std::fs::rename(&tmp, path)
        })();
        if written.is_err() {
            let _ = std::fs::remove_file(&tmp);
        }
    }

    pub(crate) fn begin(
        &self,
        resume_only: bool,
        joined: impl FnOnce() -> Vec<(String, u64, bool)>,
    ) -> Begin {
        self.begin_with(resume_only, false, joined)
    }

    /// The automatic start: resumes or plans a run like `begin(false)`, but a
    /// run the user paused stays paused.
    pub(crate) fn begin_background(
        &self,
        joined: impl FnOnce() -> Vec<(String, u64, bool)>,
    ) -> Begin {
        self.begin_with(false, true, joined)
    }

    fn begin_with(
        &self,
        resume_only: bool,
        background: bool,
        joined: impl FnOnce() -> Vec<(String, u64, bool)>,
    ) -> Begin {
        let mut inner = self.lock();
        if inner.running {
            if !resume_only && !background {
                inner.pause_requested = false;
                inner.cancel_requested = false;
            }
            return Begin::AlreadyRunning;
        }
        let joined = joined();
        let progress = self.loaded(&mut inner);
        let eligible: HashSet<&str> = joined
            .iter()
            .filter(|(_, _, encrypted)| *encrypted)
            .map(|(id, _, _)| id.as_str())
            .collect();
        let before = progress.clone();
        for (id, _, encrypted) in &joined {
            if !encrypted {
                progress.complete.remove(id);
            }
        }
        let old_position = progress.position;
        progress.position = progress
            .queue
            .iter()
            .take(old_position)
            .filter(|id| eligible.contains(id.as_str()))
            .count();
        progress.queue.retain(|id| eligible.contains(id.as_str()));
        if !progress.pending() {
            progress.paused = false;
        }
        if *progress != before {
            self.persist(progress);
        }
        if progress.pending() {
            if (resume_only || background) && progress.paused {
                return Begin::PausedPending;
            }
            progress.paused = false;
        } else {
            if resume_only {
                return Begin::NothingPending;
            }
            let joined = if background {
                joined
                    .into_iter()
                    .filter(|(id, _, _)| !progress.utd_rooms.contains(id))
                    .collect()
            } else {
                joined
            };
            let (queue, skipped) = plan_queue(joined, &progress.complete);
            progress.reset_run();
            progress.queue = queue;
            progress.skipped = skipped;
            if progress.queue.is_empty() {
                progress.finished_ms = now_ms();
                let snapshot = progress.clone();
                self.persist(&snapshot);
                return Begin::NothingToDo;
            }
        }
        let snapshot = progress.clone();
        inner.running = true;
        inner.pause_requested = false;
        inner.cancel_requested = false;
        inner.in_room_written = 0;
        inner.retry_in_ms = 0;
        inner.held_reported = false;
        drop(inner);
        self.persist(&snapshot);
        Begin::Started
    }

    pub(crate) fn pause(&self) {
        let mut inner = self.lock();
        if inner.running {
            inner.pause_requested = true;
            return;
        }
        let progress = self.loaded(&mut inner);
        if progress.pending() && !progress.paused {
            progress.paused = true;
            let snapshot = progress.clone();
            self.persist(&snapshot);
        }
    }

    pub(crate) fn cancel(&self) {
        let mut inner = self.lock();
        if inner.running {
            inner.cancel_requested = true;
            return;
        }
        let progress = self.loaded(&mut inner);
        if progress.pending() || progress.paused {
            progress.reset_run();
            let snapshot = progress.clone();
            self.persist(&snapshot);
        }
    }

    pub(crate) fn reset(&self) {
        let mut inner = self.lock();
        inner.epoch = inner.epoch.wrapping_add(1);
        if inner.running {
            inner.cancel_requested = true;
        }
        inner.progress = Some(Progress::default());
        if let Some(path) = self.path.as_ref() {
            let _ = std::fs::remove_file(path);
        }
    }

    /// Room keys arrived for these rooms: a background start may revisit them.
    pub(crate) fn keys_arrived(&self, room_ids: impl Iterator<Item = String>) {
        let mut inner = self.lock();
        let progress = self.loaded(&mut inner);
        let mut changed = false;
        for id in room_ids {
            changed |= progress.utd_rooms.remove(&id);
        }
        if changed {
            let snapshot = progress.clone();
            drop(inner);
            self.persist(&snapshot);
        }
    }

    pub(crate) fn forget_room(&self, room_id: &str) {
        let mut inner = self.lock();
        let progress = self.loaded(&mut inner);
        if progress.complete.remove(room_id) {
            let snapshot = progress.clone();
            self.persist(&snapshot);
        }
    }

    pub(crate) fn set_hold(&self, bits: u32) {
        self.hold.store(bits, Ordering::Relaxed);
    }

    pub(crate) fn held(&self) -> bool {
        self.hold.load(Ordering::Relaxed) != 0
    }

    pub(crate) fn running(&self) -> bool {
        self.lock().running
    }

    fn epoch(&self) -> u64 {
        self.lock().epoch
    }

    fn stop_requested(&self) -> bool {
        let inner = self.lock();
        inner.pause_requested || inner.cancel_requested
    }

    fn current_room(&self) -> Option<String> {
        let mut inner = self.lock();
        self.loaded(&mut inner).current().map(str::to_owned)
    }

    pub(crate) fn status(&self, state: Option<&'static str>) -> Status {
        let mut inner = self.lock();
        let running = inner.running;
        let in_room = inner.in_room_written;
        let retry_in_ms = inner.retry_in_ms;
        let progress = self.loaded(&mut inner);
        let derived = if running {
            "running"
        } else if progress.pending() && progress.paused {
            "paused"
        } else if progress.pending() {
            "stopped"
        } else {
            "idle"
        };
        Status {
            state: state.unwrap_or(derived),
            total: progress.queue.len(),
            position: progress.position,
            current_room: progress.current().unwrap_or_default().to_owned(),
            written: progress.written.saturating_add(in_room),
            skipped: progress.skipped,
            undecryptable: progress.undecryptable,
            undecryptable_rooms: progress.undecryptable_rooms,
            failed_rooms: progress.failed_rooms,
            complete_rooms: progress.complete.len(),
            retry_in_ms,
            elapsed_ms: progress.elapsed_ms,
            finished_ms: progress.finished_ms,
        }
    }

    fn set_in_room_written(&self, written: u64) {
        self.lock().in_room_written = written;
    }

    fn set_retry(&self, ms: u64) {
        self.lock().retry_in_ms = ms;
    }

    fn note_held(&self, held: bool) -> bool {
        let mut inner = self.lock();
        let changed = inner.held_reported != held;
        inner.held_reported = held;
        changed
    }

    fn commit_room(&self, epoch: u64, room_id: &str, result: RoomResult, spent: Duration) {
        let mut inner = self.lock();
        inner.in_room_written = 0;
        inner.retry_in_ms = 0;
        if inner.epoch != epoch {
            return;
        }
        let progress = self.loaded(&mut inner);
        if progress.current() != Some(room_id) {
            return;
        }
        progress.elapsed_ms = progress
            .elapsed_ms
            .saturating_add(u64::try_from(spent.as_millis()).unwrap_or(u64::MAX));
        match result {
            RoomResult::Done {
                written,
                undecryptable,
            } => {
                progress.written = progress.written.saturating_add(written as u64);
                progress.undecryptable =
                    progress.undecryptable.saturating_add(undecryptable as u64);
                if undecryptable > 0 {
                    progress.undecryptable_rooms += 1;
                    if progress.utd_rooms.len() < MAX_COMPLETE {
                        progress.utd_rooms.insert(room_id.to_owned());
                    }
                } else {
                    progress.utd_rooms.remove(room_id);
                    if progress.complete.len() < MAX_COMPLETE {
                        progress.complete.insert(room_id.to_owned());
                    }
                }
            }
            RoomResult::Failed { .. } => progress.failed_rooms += 1,
            RoomResult::Gone | RoomResult::Interrupted { .. } => {}
        }
        progress.position += 1;
        let snapshot = progress.clone();
        drop(inner);
        self.persist(&snapshot);
    }

    fn commit_partial(&self, epoch: u64, written: usize, spent: Duration) {
        let mut inner = self.lock();
        inner.in_room_written = 0;
        if inner.epoch != epoch {
            return;
        }
        let progress = self.loaded(&mut inner);
        progress.written = progress.written.saturating_add(written as u64);
        progress.elapsed_ms = progress
            .elapsed_ms
            .saturating_add(u64::try_from(spent.as_millis()).unwrap_or(u64::MAX));
    }

    fn finish(&self, ending: Ending) {
        let mut inner = self.lock();
        inner.running = false;
        inner.pause_requested = false;
        inner.cancel_requested = false;
        inner.in_room_written = 0;
        inner.retry_in_ms = 0;
        let progress = self.loaded(&mut inner);
        match ending {
            Ending::Finished | Ending::Cancelled => progress.reset_run_keeping_counters(ending),
            Ending::Paused => progress.paused = true,
            // The session is ending and its store may be about to be deleted:
            // write nothing. Every finished room is already on disk.
            Ending::Stopped => return,
        }
        let snapshot = progress.clone();
        drop(inner);
        self.persist(&snapshot);
    }

    fn ending_now(&self, alive: &dyn Fn() -> bool) -> Option<Ending> {
        if !alive() {
            return Some(Ending::Stopped);
        }
        let inner = self.lock();
        if inner.cancel_requested {
            Some(Ending::Cancelled)
        } else if inner.pause_requested {
            Some(Ending::Paused)
        } else {
            None
        }
    }
}

impl Progress {
    fn reset_run_keeping_counters(&mut self, ending: Ending) {
        if ending == Ending::Finished {
            self.position = self.queue.len();
            self.finished_ms = now_ms();
        } else {
            self.queue.clear();
            self.position = 0;
        }
        self.paused = false;
    }
}

#[derive(Clone)]
pub(crate) struct RunGate {
    pub control: Arc<Control>,
    pub alive: Arc<dyn Fn() -> bool + Send + Sync>,
    pub emit: Arc<dyn Fn(Status) + Send + Sync>,
}

impl PageGate for RunGate {
    fn gate(&self) -> Gate {
        if !(self.alive)() || self.control.stop_requested() {
            Gate::Stop
        } else if self.control.held() {
            if self.control.note_held(true) {
                (self.emit)(self.control.status(Some("held")));
            }
            Gate::Wait
        } else {
            if self.control.note_held(false) {
                (self.emit)(self.control.status(Some("running")));
            }
            Gate::Go
        }
    }

    fn page_gap(&self) -> Duration {
        Duration::from_millis(PAGE_GAP_MS)
    }

    fn on_page(&self, so_far: &DeepOutcome) {
        self.control.set_in_room_written(so_far.written as u64);
        (self.emit)(self.control.status(Some("running")));
    }
}

async fn wait_checked<S, SF>(gate: &RunGate, total: Duration, sleep: &mut S) -> bool
where
    S: FnMut(Duration) -> SF,
    SF: std::future::Future<Output = ()>,
{
    let poll = Duration::from_millis(POLL_MS);
    let mut left = total;
    while !left.is_zero() {
        if gate.control.ending_now(&*gate.alive).is_some() {
            return false;
        }
        let step = left.min(poll);
        sleep(step).await;
        left -= step;
    }
    gate.control.ending_now(&*gate.alive).is_none()
}

pub(crate) async fn drive<W, WF, S, SF>(gate: RunGate, mut walk: W, mut sleep: S) -> Ending
where
    W: FnMut(String, RunGate) -> WF,
    WF: std::future::Future<Output = RoomResult>,
    S: FnMut(Duration) -> SF,
    SF: std::future::Future<Output = ()>,
{
    let control = Arc::clone(&gate.control);
    let epoch = control.epoch();
    let mut first_room = true;
    let mut failures: u32 = 0;
    let mut rate_limited_failures: u32 = 0;
    let mut last_room = String::new();
    loop {
        if let Some(ending) = control.ending_now(&*gate.alive) {
            control.finish(ending);
            let state = match ending {
                Ending::Finished => "done",
                Ending::Paused => "paused",
                Ending::Cancelled => "cancelled",
                Ending::Stopped => "stopped",
            };
            if ending != Ending::Stopped {
                (gate.emit)(control.status(Some(state)));
            }
            return ending;
        }
        if control.held() {
            if control.note_held(true) {
                (gate.emit)(control.status(Some("held")));
            }
            let _ = wait_checked(&gate, Duration::from_millis(POLL_MS), &mut sleep).await;
            continue;
        }
        control.note_held(false);
        let Some(room_id) = control.current_room() else {
            control.finish(Ending::Finished);
            (gate.emit)(control.status(Some("done")));
            return Ending::Finished;
        };
        if room_id != last_room {
            failures = 0;
            rate_limited_failures = 0;
            last_room = room_id.clone();
        }
        if !first_room && !wait_checked(&gate, Duration::from_millis(ROOM_GAP_MS), &mut sleep).await
        {
            continue;
        }
        first_room = false;
        (gate.emit)(control.status(Some("running")));

        let started = Instant::now();
        let result = walk(room_id.clone(), gate.clone()).await;
        let spent = started.elapsed();
        match result {
            RoomResult::Interrupted { written } => {
                control.commit_partial(epoch, written, spent);
            }
            RoomResult::Failed {
                rate_limited,
                retry_after_ms,
            } => {
                let attempt = if rate_limited {
                    rate_limited_failures += 1;
                    rate_limited_failures
                } else {
                    failures += 1;
                    failures
                };
                let limit = if rate_limited {
                    MAX_RATE_LIMITED_ATTEMPTS
                } else {
                    MAX_ROOM_ATTEMPTS
                };
                let wait = backoff_ms(rate_limited, retry_after_ms, attempt);
                if attempt >= limit {
                    control.commit_room(epoch, &room_id, result, spent);
                }
                control.set_retry(wait);
                (gate.emit)(control.status(Some("backoff")));
                let _ = wait_checked(&gate, Duration::from_millis(wait), &mut sleep).await;
                control.set_retry(0);
            }
            RoomResult::Done { .. } | RoomResult::Gone => {
                control.commit_room(epoch, &room_id, result, spent);
                (gate.emit)(control.status(Some("running")));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicBool;

    struct Store(PathBuf);

    impl Store {
        fn new(tag: &str) -> Self {
            let path = std::env::temp_dir()
                .join(format!("lightning-indexall-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).expect("test store dir");
            Store(path)
        }
    }

    impl Drop for Store {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn rooms(list: &[(&str, u64)]) -> Vec<(String, u64, bool)> {
        list.iter()
            .map(|(id, ts)| ((*id).to_owned(), *ts, true))
            .collect()
    }

    fn gate_for(
        control: &Arc<Control>,
        alive: Arc<AtomicBool>,
    ) -> (RunGate, Arc<Mutex<Vec<Status>>>) {
        let seen: Arc<Mutex<Vec<Status>>> = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&seen);
        let gate = RunGate {
            control: Arc::clone(control),
            alive: Arc::new(move || alive.load(Ordering::SeqCst)),
            emit: Arc::new(move |status: Status| sink.lock().unwrap().push(status)),
        };
        (gate, seen)
    }

    fn scripted(
        script: Vec<RoomResult>,
    ) -> (
        impl FnMut(String, RunGate) -> std::future::Ready<RoomResult>,
        Arc<Mutex<Vec<String>>>,
    ) {
        let asked: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let log = Arc::clone(&asked);
        let mut script: VecDeque<RoomResult> = script.into();
        let walk = move |room: String, _gate: RunGate| {
            log.lock().unwrap().push(room);
            std::future::ready(script.pop_front().unwrap_or(RoomResult::Done {
                written: 0,
                undecryptable: 0,
            }))
        };
        (walk, asked)
    }

    fn recorder() -> (
        impl FnMut(Duration) -> std::future::Ready<()>,
        Arc<Mutex<Vec<Duration>>>,
    ) {
        let slept: Arc<Mutex<Vec<Duration>>> = Arc::new(Mutex::new(Vec::new()));
        let log = Arc::clone(&slept);
        (
            move |d: Duration| {
                log.lock().unwrap().push(d);
                std::future::ready(())
            },
            slept,
        )
    }

    #[test]
    fn background_queue_filters_unencrypted_rooms_before_sorting_and_completion() {
        let complete = ["!public-done:x".to_owned(), "!private-done:x".to_owned()]
            .into_iter()
            .collect();
        let (queue, skipped) = plan_queue(
            vec![
                ("!public:x".into(), 999, false),
                ("!private:x".into(), 1, true),
                ("!public-done:x".into(), 900, false),
                ("!private-done:x".into(), 2, true),
            ],
            &complete,
        );
        assert_eq!(queue, vec!["!private:x"]);
        assert_eq!(skipped, 1, "only eligible complete rooms count as skipped");
    }

    #[test]
    fn background_upgrade_drops_public_rooms_and_preserves_resume_position() {
        let store = Store::new("encrypted-upgrade");
        let old = Progress {
            version: 2,
            queue: vec![
                "!public-done:x".into(),
                "!private-done:x".into(),
                "!public:x".into(),
                "!private:x".into(),
            ],
            position: 2,
            paused: true,
            complete: ["!public-done:x".into(), "!private-done:x".into()]
                .into_iter()
                .collect(),
            written: 42,
            ..Progress::default()
        };
        std::fs::write(
            store.0.join(PROGRESS_FILE),
            serde_json::to_vec(&old).unwrap(),
        )
        .unwrap();
        let control = Control::new(&store.0);
        let joined = || {
            vec![
                ("!public-done:x".into(), 4, false),
                ("!private-done:x".into(), 3, true),
                ("!public:x".into(), 2, false),
                ("!private:x".into(), 1, true),
            ]
        };
        assert_eq!(control.begin(true, joined), Begin::PausedPending);
        let upgraded: Progress =
            serde_json::from_slice(&std::fs::read(store.0.join(PROGRESS_FILE)).unwrap()).unwrap();
        assert_eq!(upgraded.queue, vec!["!private-done:x", "!private:x"]);
        assert_eq!(upgraded.position, 1);
        assert_eq!(upgraded.written, 42);
        assert!(upgraded.paused);
        assert_eq!(
            upgraded.complete,
            ["!private-done:x".into()].into_iter().collect()
        );
        let restarted = Control::new(&store.0);
        assert_eq!(restarted.begin(false, joined), Begin::Started);
        assert_eq!(restarted.status(None).current_room, "!private:x");
    }

    #[test]
    fn a_record_from_before_file_names_forgets_completed_rooms_once_and_starts_nothing() {
        let store = Store::new("filename-upgrade");
        let old = Progress {
            version: 1,
            queue: vec!["!todo:x".into()],
            position: 0,
            paused: true,
            complete: ["!done:x".into()].into_iter().collect(),
            ..Progress::default()
        };
        std::fs::write(store.0.join(PROGRESS_FILE), serde_json::to_vec(&old).unwrap()).unwrap();
        let control = Control::new(&store.0);
        let status = control.status(None);
        assert_eq!(status.complete_rooms, 0, "a pre-file-name completion survived");
        assert_eq!(status.total, 1);
        let again = Progress { version: 2, complete: ["!done:x".into()].into_iter().collect(), ..Progress::default() };
        std::fs::write(store.0.join(PROGRESS_FILE), serde_json::to_vec(&again).unwrap()).unwrap();
        let reread = Control::new(&store.0);
        assert_eq!(reread.status(None).complete_rooms, 1);
    }

    #[test]
    fn background_upgrade_with_only_public_rooms_leaves_nothing_pending() {
        let store = Store::new("public-only-upgrade");
        let old = Progress {
            version: 2,
            queue: vec!["!public:x".into()],
            paused: true,
            ..Progress::default()
        };
        std::fs::write(
            store.0.join(PROGRESS_FILE),
            serde_json::to_vec(&old).unwrap(),
        )
        .unwrap();
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(true, || vec![("!public:x".into(), 1, false)]),
            Begin::NothingPending
        );
        let upgraded: Progress =
            serde_json::from_slice(&std::fs::read(store.0.join(PROGRESS_FILE)).unwrap()).unwrap();
        assert!(upgraded.queue.is_empty());
        assert!(!upgraded.paused);
        assert_eq!(
            control.begin(false, || vec![("!public:x".into(), 1, false)]),
            Begin::NothingToDo
        );
    }

    #[test]
    fn background_room_encrypted_later_is_picked_up_even_if_old_walk_completed_it() {
        let store = Store::new("encrypted-later");
        let old = Progress {
            version: 2,
            complete: ["!later:x".into()].into_iter().collect(),
            ..Progress::default()
        };
        std::fs::write(
            store.0.join(PROGRESS_FILE),
            serde_json::to_vec(&old).unwrap(),
        )
        .unwrap();
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(false, || vec![("!later:x".into(), 1, false)]),
            Begin::NothingToDo
        );
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(false, || vec![("!later:x".into(), 2, true)]),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!later:x");
    }

    #[test]
    fn the_queue_visits_the_most_recent_rooms_first_and_skips_complete_ones() {
        let complete: BTreeSet<String> = ["!done:x".to_owned()].into_iter().collect();
        let (queue, skipped) = plan_queue(
            rooms(&[
                ("!old:x", 100),
                ("!done:x", 900),
                ("!new:x", 500),
                ("!tie-b:x", 300),
                ("!tie-a:x", 300),
                ("!new:x", 500),
                ("", 999),
                ("!unknown:x", 0),
            ]),
            &complete,
        );
        assert_eq!(
            queue,
            vec!["!new:x", "!tie-a:x", "!tie-b:x", "!old:x", "!unknown:x"],
            "most recent first, ties by id, duplicates and blanks dropped"
        );
        assert_eq!(skipped, 1, "the complete room was not reported as skipped");
    }

    #[test]
    fn a_new_run_skips_rooms_a_previous_run_completed() {
        let store = Store::new("skip");
        let control = Arc::new(Control::new(&store.0));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        let epoch = control.epoch();
        control.commit_room(
            epoch,
            "!a:x",
            RoomResult::Done {
                written: 3,
                undecryptable: 0,
            },
            Duration::ZERO,
        );
        control.commit_room(
            epoch,
            "!b:x",
            RoomResult::Done {
                written: 1,
                undecryptable: 0,
            },
            Duration::ZERO,
        );
        control.finish(Ending::Finished);

        let begin = control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1), ("!c:x", 3)]));
        assert_eq!(begin, Begin::Started);
        let status = control.status(None);
        assert_eq!(status.total, 1, "a complete room was queued again");
        assert_eq!(status.current_room, "!c:x");
        assert_eq!(status.skipped, 2);
        control.finish(Ending::Finished);

        let control = Control::new(&store.0);
        let begin = control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)]));
        assert_eq!(begin, Begin::NothingToDo);
    }

    #[test]
    fn a_background_start_walks_only_new_rooms_and_keeps_a_user_pause() {
        let store = Store::new("background");
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin_background(|| rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        control.pause();
        control.finish(Ending::Paused);
        assert_eq!(
            control.begin_background(|| rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::PausedPending,
            "a background start overrode the user's pause"
        );
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        let epoch = control.epoch();
        for room in ["!a:x", "!b:x"] {
            control.commit_room(
                epoch,
                room,
                RoomResult::Done { written: 1, undecryptable: 0 },
                Duration::ZERO,
            );
        }
        control.finish(Ending::Finished);
        let finished = control.status(None).finished_ms;
        assert!(finished > 0, "a finished run left no timestamp");

        let control = Control::new(&store.0);
        assert_eq!(control.status(None).finished_ms, finished);
        assert_eq!(
            control.begin_background(|| rooms(&[("!a:x", 2), ("!b:x", 1), ("!new:x", 3)])),
            Begin::Started
        );
        let status = control.status(None);
        assert_eq!(status.total, 1, "a finished room was walked again");
        assert_eq!(status.current_room, "!new:x");
        control.finish(Ending::Cancelled);

        control.reset();
        assert_eq!(control.status(None).finished_ms, 0, "Clear index kept the timestamp");
    }

    #[test]
    fn a_background_start_skips_undecryptable_rooms_until_keys_arrive() {
        let control = Control::new(Path::new(""));
        assert_eq!(control.begin(false, || rooms(&[("!e2e:x", 1)])), Begin::Started);
        let epoch = control.epoch();
        control.commit_room(
            epoch,
            "!e2e:x",
            RoomResult::Done { written: 1, undecryptable: 3 },
            Duration::ZERO,
        );
        control.finish(Ending::Finished);
        assert_eq!(
            control.begin_background(|| rooms(&[("!e2e:x", 1)])),
            Begin::NothingToDo,
            "a background start re-walked a room no key had arrived for"
        );
        control.keys_arrived(std::iter::once("!e2e:x".to_owned()));
        assert_eq!(control.begin_background(|| rooms(&[("!e2e:x", 1)])), Begin::Started);
        control.finish(Ending::Cancelled);
        assert_eq!(control.begin(false, || rooms(&[("!e2e:x", 1)])), Begin::Started);
    }

    #[test]
    fn a_room_with_undecryptable_history_is_not_recorded_complete() {
        let control = Control::new(Path::new(""));
        assert_eq!(
            control.begin(false, || rooms(&[("!e2e:x", 1)])),
            Begin::Started
        );
        let epoch = control.epoch();
        control.commit_room(
            epoch,
            "!e2e:x",
            RoomResult::Done {
                written: 10,
                undecryptable: 4,
            },
            Duration::ZERO,
        );
        let status = control.status(None);
        assert_eq!(
            status.complete_rooms, 0,
            "a room with UTDs was marked complete"
        );
        assert_eq!(status.undecryptable, 4);
        assert_eq!(status.undecryptable_rooms, 1);
        control.finish(Ending::Finished);
        assert_eq!(
            control.begin(false, || rooms(&[("!e2e:x", 1)])),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!e2e:x");
    }

    #[test]
    fn a_restart_resumes_from_the_persisted_position() {
        let store = Store::new("resume");
        {
            let control = Control::new(&store.0);
            let list = rooms(&[("!r1:x", 3), ("!r2:x", 2), ("!r3:x", 1)]);
            assert_eq!(control.begin(false, || list), Begin::Started);
            let epoch = control.epoch();
            control.commit_room(
                epoch,
                "!r1:x",
                RoomResult::Done {
                    written: 5,
                    undecryptable: 0,
                },
                Duration::from_millis(40),
            );
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(store.0.join(PROGRESS_FILE))
                .unwrap()
                .permissions()
                .mode();
            assert_eq!(
                mode & 0o077,
                0,
                "the record is readable by others: {mode:o}"
            );
        }
        let control = Control::new(&store.0);
        let begin = control.begin(true, || rooms(&[("!r3:x", 9), ("!r2:x", 1), ("!r1:x", 0)]));
        assert_eq!(begin, Begin::Started, "an interrupted run did not resume");
        let status = control.status(None);
        assert_eq!(status.position, 1);
        assert_eq!(
            status.current_room, "!r2:x",
            "the run restarted from the top"
        );
        assert_eq!(status.written, 5);
        assert_eq!(status.elapsed_ms, 40);
    }

    #[test]
    fn a_paused_run_stays_paused_across_a_restart_until_asked() {
        let store = Store::new("paused");
        {
            let control = Control::new(&store.0);
            assert_eq!(
                control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
                Begin::Started
            );
            control.pause();
            control.finish(Ending::Paused);
        }
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(true, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::PausedPending
        );
        assert_eq!(control.status(None).state, "paused");
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!a:x");
        control.finish(Ending::Finished);
        assert_eq!(control.begin(true, Vec::new), Begin::NothingPending);
    }

    #[test]
    fn a_resume_on_sync_never_withdraws_a_pending_pause() {
        let control = Control::new(Path::new(""));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
        control.pause();
        assert_eq!(control.begin(true, Vec::new), Begin::AlreadyRunning);
        assert!(
            control.stop_requested(),
            "a resume on sync withdrew the pause"
        );
        assert_eq!(control.begin(false, Vec::new), Begin::AlreadyRunning);
        assert!(!control.stop_requested());
    }

    #[test]
    fn a_corrupt_record_costs_a_rewalk_not_a_failure() {
        let store = Store::new("corrupt");
        std::fs::write(store.0.join(PROGRESS_FILE), b"{not json").unwrap();
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!a:x");
    }

    #[tokio::test]
    async fn the_driver_walks_rooms_one_at_a_time_in_queue_order() {
        let control = Arc::new(Control::new(Path::new("")));
        assert_eq!(
            control.begin(false, || rooms(&[("!b:x", 1), ("!a:x", 2)])),
            Begin::Started
        );
        let (gate, statuses) = gate_for(&control, Arc::new(AtomicBool::new(true)));
        let (walk, asked) = scripted(vec![]);
        let (sleep, slept) = recorder();
        assert_eq!(drive(gate, walk, sleep).await, Ending::Finished);
        assert_eq!(*asked.lock().unwrap(), vec!["!a:x", "!b:x"]);
        let total: Duration = slept.lock().unwrap().iter().sum();
        assert!(
            total >= Duration::from_millis(ROOM_GAP_MS),
            "no pause between rooms: {total:?}"
        );
        let last = statuses.lock().unwrap().last().cloned().unwrap();
        assert_eq!(last.state, "done");
        assert_eq!(
            (last.position, last.total),
            (2, 2),
            "done forgot what it walked"
        );
        assert_eq!(control.status(None).state, "idle");
        assert_eq!(control.status(None).complete_rooms, 2);
    }

    #[tokio::test]
    async fn a_rate_limited_room_waits_the_servers_retry_after_then_retries_the_same_room() {
        let control = Arc::new(Control::new(Path::new("")));
        assert_eq!(
            control.begin(false, || rooms(&[("!busy:x", 2), ("!next:x", 1)])),
            Begin::Started
        );
        let (gate, statuses) = gate_for(&control, Arc::new(AtomicBool::new(true)));
        let (walk, asked) = scripted(vec![
            RoomResult::Failed {
                rate_limited: true,
                retry_after_ms: Some(7_300),
            },
            RoomResult::Done {
                written: 2,
                undecryptable: 0,
            },
            RoomResult::Done {
                written: 1,
                undecryptable: 0,
            },
        ]);
        let (sleep, slept) = recorder();
        assert_eq!(drive(gate, walk, sleep).await, Ending::Finished);
        assert_eq!(
            *asked.lock().unwrap(),
            vec!["!busy:x", "!busy:x", "!next:x"],
            "a rate-limited room was skipped instead of retried"
        );
        let backoff = statuses
            .lock()
            .unwrap()
            .iter()
            .find(|s| s.state == "backoff")
            .cloned()
            .expect("no backoff was reported");
        assert_eq!(
            backoff.retry_in_ms, 7_300,
            "the server's retry_after was not honoured"
        );
        let total: u64 = slept
            .lock()
            .unwrap()
            .iter()
            .map(|d| d.as_millis() as u64)
            .sum();
        assert!(
            total >= 7_300 + ROOM_GAP_MS,
            "the queue went back to the server before retry_after: slept {total} ms"
        );
        assert_eq!(control.status(None).failed_rooms, 0);
    }

    #[test]
    fn backoff_honours_the_hint_floors_and_caps() {
        assert_eq!(backoff_ms(true, Some(7_300), 1), 7_300);
        assert_eq!(
            backoff_ms(true, Some(10), 1),
            MIN_BACKOFF_MS,
            "a hint of 10 ms hammered the server"
        );
        assert_eq!(backoff_ms(true, None, 1), RATE_LIMITED_FLOOR_MS);
        assert_eq!(backoff_ms(false, None, 1), MIN_BACKOFF_MS);
        assert_eq!(backoff_ms(false, None, 3), MIN_BACKOFF_MS * 4);
        assert_eq!(backoff_ms(true, Some(u64::MAX), 1), MAX_BACKOFF_MS);
        assert_eq!(backoff_ms(false, None, 200), MAX_BACKOFF_MS);
    }

    #[tokio::test]
    async fn a_room_that_keeps_failing_is_given_up_on_and_the_queue_moves_on() {
        let control = Arc::new(Control::new(Path::new("")));
        assert_eq!(
            control.begin(false, || rooms(&[("!bad:x", 2), ("!ok:x", 1)])),
            Begin::Started
        );
        let (gate, _) = gate_for(&control, Arc::new(AtomicBool::new(true)));
        let fail = RoomResult::Failed {
            rate_limited: false,
            retry_after_ms: None,
        };
        let (walk, asked) = scripted(vec![fail; MAX_ROOM_ATTEMPTS as usize]);
        let (sleep, _) = recorder();
        assert_eq!(drive(gate, walk, sleep).await, Ending::Finished);
        let asked = asked.lock().unwrap().clone();
        assert_eq!(
            asked.iter().filter(|r| *r == "!bad:x").count(),
            MAX_ROOM_ATTEMPTS as usize
        );
        assert_eq!(asked.last().unwrap(), "!ok:x");
        let status = control.status(None);
        assert_eq!(status.failed_rooms, 1);
        assert_eq!(
            status.complete_rooms, 1,
            "a failed room was recorded complete"
        );
    }

    #[tokio::test]
    async fn a_session_ending_stops_the_run_and_writes_nothing_more() {
        let store = Store::new("lifecycle");
        let control = Arc::new(Control::new(&store.0));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 3), ("!b:x", 2), ("!c:x", 1)])),
            Begin::Started
        );
        let alive = Arc::new(AtomicBool::new(true));
        let (gate, statuses) = gate_for(&control, Arc::clone(&alive));
        let asked: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let log = Arc::clone(&asked);
        let flag = Arc::clone(&alive);
        let walk = move |room: String, _gate: RunGate| {
            log.lock().unwrap().push(room);
            flag.store(false, Ordering::SeqCst);
            std::future::ready(RoomResult::Interrupted { written: 0 })
        };
        let (sleep, _) = recorder();
        let before = std::fs::read(store.0.join(PROGRESS_FILE)).unwrap();
        assert_eq!(drive(gate, walk, sleep).await, Ending::Stopped);
        assert_eq!(
            *asked.lock().unwrap(),
            vec!["!a:x"],
            "the run walked on after sign-out"
        );
        assert!(!control.running());
        assert!(
            statuses
                .lock()
                .unwrap()
                .iter()
                .all(|s| s.state != "stopped"),
            "a status was emitted into a session that is over"
        );
        let after = std::fs::read(store.0.join(PROGRESS_FILE)).unwrap();
        assert_eq!(
            before, after,
            "the record was rewritten after the session ended"
        );
        let next = Control::new(&store.0);
        assert_eq!(
            next.begin(true, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        assert_eq!(next.status(None).current_room, "!a:x");
    }

    #[tokio::test]
    async fn pause_and_cancel_take_effect_at_the_next_check() {
        let control = Arc::new(Control::new(Path::new("")));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        let (gate, statuses) = gate_for(&control, Arc::new(AtomicBool::new(true)));
        let paused_by = Arc::clone(&control);
        let walk = move |_room: String, _gate: RunGate| {
            paused_by.pause();
            std::future::ready(RoomResult::Done {
                written: 1,
                undecryptable: 0,
            })
        };
        let (sleep, _) = recorder();
        assert_eq!(drive(gate.clone(), walk, sleep).await, Ending::Paused);
        let status = control.status(None);
        assert_eq!(status.state, "paused");
        assert_eq!(status.position, 1, "the finished room was not kept");
        assert_eq!(statuses.lock().unwrap().last().unwrap().state, "paused");

        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)])),
            Begin::Started
        );
        let cancelled_by = Arc::clone(&control);
        let walk = move |_room: String, _gate: RunGate| {
            cancelled_by.cancel();
            std::future::ready(RoomResult::Interrupted { written: 0 })
        };
        let (sleep, _) = recorder();
        assert_eq!(drive(gate, walk, sleep).await, Ending::Cancelled);
        let status = control.status(None);
        assert_eq!(status.state, "idle");
        assert_eq!(status.total, 0);
        assert_eq!(status.complete_rooms, 1);
    }

    #[tokio::test]
    async fn a_hold_waits_without_walking_and_resumes_when_released() {
        let control = Arc::new(Control::new(Path::new("")));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
        control.set_hold(HOLD_CALL);
        let (gate, statuses) = gate_for(&control, Arc::new(AtomicBool::new(true)));
        let (walk, asked) = scripted(vec![]);
        let released = Arc::clone(&control);
        let mut slices = 0u32;
        let sleep = move |_d: Duration| {
            slices += 1;
            if slices == 3 {
                released.set_hold(0);
            }
            std::future::ready(())
        };
        assert_eq!(drive(gate, walk, sleep).await, Ending::Finished);
        assert!(statuses.lock().unwrap().iter().any(|s| s.state == "held"));
        assert_eq!(*asked.lock().unwrap(), vec!["!a:x"]);
    }

    #[test]
    fn clearing_the_index_drops_a_late_result_and_every_record() {
        let store = Store::new("reset");
        let control = Control::new(&store.0);
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
        let epoch = control.epoch();
        control.reset();
        control.commit_room(
            epoch,
            "!a:x",
            RoomResult::Done {
                written: 9,
                undecryptable: 0,
            },
            Duration::ZERO,
        );
        let status = control.status(None);
        assert_eq!(
            status.complete_rooms, 0,
            "a room finished before the clear was recorded after it"
        );
        assert!(
            !store.0.join(PROGRESS_FILE).exists(),
            "the record outlived the index"
        );
    }

    #[test]
    fn forgetting_a_room_makes_it_eligible_again() {
        let control = Control::new(Path::new(""));
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
        let epoch = control.epoch();
        control.commit_room(
            epoch,
            "!a:x",
            RoomResult::Done {
                written: 1,
                undecryptable: 0,
            },
            Duration::ZERO,
        );
        control.finish(Ending::Finished);
        control.forget_room("!a:x");
        assert_eq!(
            control.begin(false, || rooms(&[("!a:x", 1)])),
            Begin::Started
        );
    }
}
