//! "Index all rooms": one queue over the joined rooms, walked strictly one room
//! at a time. Each room is indexed by the SAME bounded backward walk "Index
//! this room" uses (`localsearch::deep_index_room_gated`, `DEEP_MAX_PAGES`),
//! so there is one indexer and one history bound; this module only decides
//! the order, the pacing, and what survives a restart.
//!
//! Load on the homeserver: one `/messages` page at a time, `PAGE_GAP_MS`
//! between pages and `ROOM_GAP_MS` between rooms. matrix-sdk already retries
//! a 429 on the server's `retry_after_ms`; a rate limit that still surfaces
//! backs the whole queue off by the server's hint (`backoff_ms`). A call or a
//! scrolling reader HOLDS the walk between pages; nothing is lost.
//!
//! Persistence: the queue, its position and the rooms already done live in
//! `PROGRESS_FILE` inside the account's own store directory (0600, deleted
//! with the account; cleared with the index). Room ids and counters only —
//! never message text, which stays in the index this module feeds.
//!
//! Undecryptable history is never indexed: the walk counts it, and a room
//! that still holds any is NOT recorded as complete, so the next run revisits
//! it. matrix-sdk's redecryptor rewrites those events in the event-cache
//! store when keys arrive, and that revisit reads them from the store.

use std::collections::{BTreeSet, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::localsearch::{DeepOutcome, Gate, PageGate};

/// File name inside the account's store directory.
pub(crate) const PROGRESS_FILE: &str = "lightning-index-all.json";
const PROGRESS_VERSION: u32 = 1;

/// Pause between two pages of one room. Element Desktop's crawler waits 3 s
/// between requests by default; this stays in that range rather than 4x it.
pub(crate) const PAGE_GAP_MS: u64 = 2_000;
/// Pause between two rooms.
pub(crate) const ROOM_GAP_MS: u64 = 2_000;
/// How often a held or backing-off queue re-reads its controls.
pub(crate) const POLL_MS: u64 = 500;
/// First retry after a failed room; doubles per attempt.
pub(crate) const MIN_BACKOFF_MS: u64 = 2_000;
/// A rate limit without a hint waits at least this long.
pub(crate) const RATE_LIMITED_FLOOR_MS: u64 = 30_000;
/// Ceiling on any wait, including a server's hint (a skewed `DateTime`
/// hint can ask for hours).
pub(crate) const MAX_BACKOFF_MS: u64 = 30 * 60 * 1000;
/// Ordinary failures of one room before the queue moves past it.
pub(crate) const MAX_ROOM_ATTEMPTS: u32 = 3;
/// Rate-limited retries of one room before the queue moves past it. Larger:
/// a rate limit is the server's state, not the room's fault.
pub(crate) const MAX_RATE_LIMITED_ATTEMPTS: u32 = 8;
/// Bounds on what the record holds, so it stays small however many rooms.
pub(crate) const MAX_QUEUE: usize = 5_000;
pub(crate) const MAX_COMPLETE: usize = 20_000;

/// Automatic hold reasons, set from C++ as a bit set.
pub(crate) const HOLD_CALL: u32 = 1;
pub(crate) const HOLD_SCROLL: u32 = 2;

/// What survives a restart.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub(crate) struct Progress {
    #[serde(default)]
    pub version: u32,
    /// The run's rooms in order. Empty when no run is pending.
    #[serde(default)]
    pub queue: Vec<String>,
    /// How many of `queue` are handled (indexed, skipped or given up on).
    #[serde(default)]
    pub position: usize,
    /// The user paused; a restart shows it paused rather than resuming.
    #[serde(default)]
    pub paused: bool,
    /// Rooms walked to the bound or their start with nothing undecryptable
    /// left. Later runs skip them.
    #[serde(default)]
    pub complete: BTreeSet<String>,
    /// Counters of the current run.
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
    /// Time spent working on this run, for the estimate.
    #[serde(default)]
    pub elapsed_ms: u64,
}

impl Progress {
    pub(crate) fn pending(&self) -> bool {
        self.position < self.queue.len()
    }

    pub(crate) fn current(&self) -> Option<&str> {
        self.queue.get(self.position).map(String::as_str)
    }

    /// Repair what a hand-edited or truncated file could hold.
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

/// The order a new run visits rooms: most recently active first (those are
/// the ones people search), rooms already complete left out, duplicates
/// dropped, ties broken on the room id so the order is deterministic.
/// `rooms` is (room id, latest activity in ms; 0 = unknown). Returns the queue
/// and how many rooms were skipped as complete.
pub(crate) fn plan_queue(
    rooms: Vec<(String, u64)>,
    complete: &BTreeSet<String>,
) -> (Vec<String>, u64) {
    let mut seen = HashSet::new();
    let mut skipped = 0u64;
    let mut wanted: Vec<(String, u64)> = Vec::with_capacity(rooms.len());
    for (id, stamp) in rooms {
        if id.is_empty() || !seen.insert(id.clone()) {
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

/// How long the queue waits after a failed room. A server that said how long
/// is obeyed (never below `MIN_BACKOFF_MS`); a rate limit without a hint waits
/// at least `RATE_LIMITED_FLOOR_MS`; anything else backs off exponentially.
/// `attempt` counts from 1. Always capped at `MAX_BACKOFF_MS`.
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

/// A snapshot for the UI. `state`: "idle" (nothing pending), "running",
/// "held" (a call or scrolling), "backoff", "paused", "done", "cancelled",
/// "stopped" (the session ended mid-run; the next session resumes it).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct Status {
    pub state: &'static str,
    pub total: usize,
    pub position: usize,
    pub current_room: String,
    /// Messages written by this run, including the room in progress.
    pub written: u64,
    pub skipped: u64,
    pub undecryptable: u64,
    pub undecryptable_rooms: u64,
    pub failed_rooms: u64,
    pub complete_rooms: usize,
    pub retry_in_ms: u64,
    pub elapsed_ms: u64,
}

/// What `begin` did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Begin {
    /// A run is now pending and the caller must spawn the driver.
    Started,
    /// The driver is already running; a pause or cancel request was withdrawn.
    AlreadyRunning,
    /// `resume_only` and nothing was interrupted.
    NothingPending,
    /// `resume_only` and the pending run was paused by the user.
    PausedPending,
    /// Every joined room is already complete.
    NothingToDo,
}

/// How the driver ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Ending {
    Finished,
    Paused,
    Cancelled,
    /// The session ended (sign-out, account switch, teardown).
    Stopped,
}

/// What happened to one room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RoomResult {
    /// Walked to the bound or the room start.
    Done {
        written: usize,
        undecryptable: usize,
    },
    /// Stopped mid-room (pause, cancel, sign-out); revisited next time.
    Interrupted { written: usize },
    /// The walk failed.
    Failed {
        rate_limited: bool,
        retry_after_ms: Option<u64>,
    },
    /// No longer joined, or unknown: moved past.
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
    /// Loaded lazily from `path`.
    progress: Option<Progress>,
    /// Written by the room in progress, not yet committed.
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
    /// `store_dir` empty (tests, or a bridge without a store): nothing is
    /// persisted.
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
        // A poisoned lock only means a panic elsewhere; the record is plain data.
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
        // A corrupt record costs a re-walk, never a failure.
        serde_json::from_slice::<Progress>(&bytes)
            .map(Progress::sanitized)
            .unwrap_or_default()
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

    /// Start a run, or continue the pending one. `joined` lists the joined
    /// rooms and is only called when a new queue must be planned.
    pub(crate) fn begin(
        &self,
        resume_only: bool,
        joined: impl FnOnce() -> Vec<(String, u64)>,
    ) -> Begin {
        let mut inner = self.lock();
        if inner.running {
            // Only the user's own start withdraws their pause or cancel; the
            // automatic resume on every sync must never override either.
            if !resume_only {
                inner.pause_requested = false;
                inner.cancel_requested = false;
            }
            return Begin::AlreadyRunning;
        }
        let progress = self.loaded(&mut inner);
        if progress.pending() {
            if resume_only && progress.paused {
                return Begin::PausedPending;
            }
            progress.paused = false;
        } else {
            if resume_only {
                return Begin::NothingPending;
            }
            let (queue, skipped) = plan_queue(joined(), &progress.complete);
            progress.reset_run();
            progress.queue = queue;
            progress.skipped = skipped;
            if progress.queue.is_empty() {
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

    /// Ask a running driver to pause at its next check; a pending run that is
    /// not running is marked paused directly.
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

    /// Drop the pending run. Rooms already complete stay complete.
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

    /// The index was cleared: forget every run and every completed room, and
    /// make any late result from a running driver land nowhere.
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

    /// A room was forgotten from the index; it is no longer complete.
    pub(crate) fn forget_room(&self, room_id: &str) {
        let mut inner = self.lock();
        let progress = self.loaded(&mut inner);
        if progress.complete.remove(room_id) {
            let snapshot = progress.clone();
            self.persist(&snapshot);
        }
    }

    /// Automatic holds (bits of `HOLD_*`), replacing the previous set.
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

    /// The UI snapshot. `state` is chosen by the caller when it knows better
    /// (a driver reporting "held" or "backoff"); `None` derives it.
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
            // Interrupted by the end of a session and not paused.
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
        }
    }

    fn set_in_room_written(&self, written: u64) {
        self.lock().in_room_written = written;
    }

    fn set_retry(&self, ms: u64) {
        self.lock().retry_in_ms = ms;
    }

    /// Record whether "held" is what was last reported; true when that
    /// changed, so a long call reports "held" once, not every poll.
    fn note_held(&self, held: bool) -> bool {
        let mut inner = self.lock();
        let changed = inner.held_reported != held;
        inner.held_reported = held;
        changed
    }

    /// Record the room at the head of the queue and move past it. Dropped when
    /// the index was cleared since the driver started (`epoch`), or when the
    /// head is no longer that room.
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
                } else if progress.complete.len() < MAX_COMPLETE {
                    progress.complete.insert(room_id.to_owned());
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

    /// Count what an interrupted room wrote without moving past it.
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

    /// Settle the run under one lock hold, so a `begin` racing the end can
    /// never see `running` false with the old run's state unwritten.
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

    /// The ending decided under the lock: every way a run stops, in priority
    /// order. `alive` is false once this session is over.
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
    /// End of a run: the counters stay readable until the next run so the UI
    /// can report what the finished run did.
    ///
    /// A finished run keeps its queue with the position at its end, so "done"
    /// can still say how many rooms it walked (and `pending()` is false); a
    /// cancelled one drops it. The next run plans a fresh queue either way.
    fn reset_run_keeping_counters(&mut self, ending: Ending) {
        if ending == Ending::Finished {
            self.position = self.queue.len();
        } else {
            self.queue.clear();
            self.position = 0;
        }
        self.paused = false;
    }
}

/// The gate a room walk reads, and the channel it reports through. Owns its
/// parts so a walk future can hold it without borrowing the driver.
#[derive(Clone)]
pub(crate) struct RunGate {
    pub control: Arc<Control>,
    /// False once this session is over.
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

/// Sleep `total` in `POLL_MS` slices through `sleep`, re-reading the controls
/// between slices. False when the run must end.
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

/// Run the queue until it finishes, pauses, is cancelled or the session ends.
/// `walk` indexes one room; `sleep` is `tokio::time::sleep` in production and
/// a recorder in tests. Strictly sequential: one `walk` in flight at a time.
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

    /// A store directory under the system temp dir, removed on drop.
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

    fn rooms(list: &[(&str, u64)]) -> Vec<(String, u64)> {
        list.iter()
            .map(|(id, ts)| ((*id).to_owned(), *ts))
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

    /// A walk that answers from a script, in order, and records the rooms
    /// it was asked for.
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

    // ── Ordering and skipping ────────────────────────────────────────────
    // Old code: there was no queue at all ("Index this room" only), so none
    // of these can run against it; each asserts a property a naive loop over
    // `joined_rooms()` (SDK order, no record) does not have.
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

        // Both complete, one new room joined since.
        let begin = control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1), ("!c:x", 3)]));
        assert_eq!(begin, Begin::Started);
        let status = control.status(None);
        assert_eq!(status.total, 1, "a complete room was queued again");
        assert_eq!(status.current_room, "!c:x");
        assert_eq!(status.skipped, 2);
        control.finish(Ending::Finished);

        // Nothing left to do is its own answer, not a silent no-op.
        let control = Control::new(&store.0);
        let begin = control.begin(false, || rooms(&[("!a:x", 2), ("!b:x", 1)]));
        assert_eq!(begin, Begin::NothingToDo);
    }

    // ── Undecryptable history ────────────────────────────────────────────
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
        // So the next run revisits it.
        assert_eq!(
            control.begin(false, || rooms(&[("!e2e:x", 1)])),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!e2e:x");
    }

    // ── Restart ──────────────────────────────────────────────────────────
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
            // The process dies here: no finish().
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
        // resume_only must not plan a new queue: the joined list is not asked.
        let begin = control.begin(true, || panic!("a resume re-planned the queue"));
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
        assert_eq!(control.begin(true, Vec::new), Begin::PausedPending);
        assert_eq!(control.status(None).state, "paused");
        // An explicit start continues it, from where it was.
        assert_eq!(
            control.begin(false, || panic!("re-planned")),
            Begin::Started
        );
        assert_eq!(control.status(None).current_room, "!a:x");
        // Nothing interrupted and nothing paused: a resume does nothing.
        control.finish(Ending::Finished);
        assert_eq!(control.begin(true, Vec::new), Begin::NothingPending);
    }

    // The app asks to resume on every sync; that must not undo a pause the
    // driver has not reached yet. Clearing the request on any begin() fails.
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
        // The user's own start does withdraw it.
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

    // ── The driver ───────────────────────────────────────────────────────
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
        // Paced: a room gap was slept before the second room.
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
        // Slept: the hint, then two room gaps (retry, next room).
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

    // ── Lifecycle ────────────────────────────────────────────────────────
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
        // Sign-out lands while the first room is being walked.
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
        // The next session of the same account picks it up at the same room.
        let next = Control::new(&store.0);
        assert_eq!(next.begin(true, || panic!("re-planned")), Begin::Started);
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

        // Resume, then cancel mid-room: the queue goes, complete rooms stay.
        assert_eq!(
            control.begin(false, || panic!("re-planned")),
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
