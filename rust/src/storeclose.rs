//! Whether anything in this process still holds an account's SQLite stores.
//!
//! GitHub #2 (Windows): sign-out waited for the bridge handle to be retired
//! and took that as "the store is closed". It was not: `mx_rust_destroy`
//! returned in 1 ms while every database in the store directory stayed open
//! until the process exited, so the deletion failed and the user was told the
//! repair did not finish. Retiring a handle is not the signal. The signal is
//! the stores themselves being dropped, so each store a client build opens is
//! recorded here as a `Weak`, beside a sentinel that lives exactly as long as
//! the `Client` (it is stored in the Client's own handler context), and
//! retirement waits, bounded, for all of them to fail to upgrade.
//!
//! The state store is recorded through the `Arc` matrix-sdk-base accepts for
//! it (`impl StateStore for Arc<T>`); the crypto, event-cache and media stores
//! through the `Arc<T>` conversions their `Into*Store` traits provide, which
//! keep the allocation, so each `Weak` dies when the SDK's last reference
//! does. An in-memory media store holds no file and is not recorded.
//!
//! Probes are recorded per store directory AND per handle (the owner
//! `mediastore` recorded for that directory when the stores were opened), so
//! retiring an old handle never waits for the stores a newer handle for the
//! same account has just opened (A -> B -> A).
//!
//! A dead `Weak` is not the whole answer: matrix-sdk-sqlite runs its queries
//! in `spawn_blocking` jobs that hold a pool connection, and such a job does
//! not keep the store `Arc` alive. So the runtime's own shutdown is part of
//! the verdict: a shutdown that ran out its budget may have left a job, and
//! its connection, behind (`runtime_shutdown=timed_out`).
//!
//! Only names and counts are reported, never a path (a store path carries the
//! Matrix localpart, CLAUDE.md §6).

use std::any::Any;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

type AnyWeak = Weak<dyn Any + Send + Sync>;

/// The stores one client build opened, and that Client's sentinel.
#[derive(Default)]
pub(crate) struct StoreProbe {
    stores: Vec<(&'static str, AnyWeak)>,
    client: Option<Weak<ClientAlive>>,
    /// The handle (`RustClient` address) the stores were opened for, as
    /// `mediastore` recorded it for the directory. `None` when no handle had
    /// claimed the directory (no media key was ever set for it): such a probe
    /// is waited for by any handle retiring from that directory.
    owner: Option<usize>,
}

/// Lives in the Client's event-handler context map, i.e. inside
/// `ClientInner`, so it is dropped exactly when the last `Client` is.
/// Never read by a handler.
#[derive(Clone)]
pub(crate) struct ClientSentinel(#[allow(dead_code)] Arc<ClientAlive>);

pub(crate) struct ClientAlive;

impl StoreProbe {
    /// Record one store. `T` is the concrete SQLite store the SDK is handed
    /// as `Arc<T>`.
    pub(crate) fn store<T: Any + Send + Sync>(&mut self, name: &'static str, store: &Arc<T>) {
        let weak: Weak<T> = Arc::downgrade(store);
        self.stores.push((name, weak as AnyWeak));
    }

    /// Which handle these stores belong to.
    pub(crate) fn set_owner(&mut self, owner: Option<usize>) {
        self.owner = owner;
    }

    /// Plant the sentinel in `client` and remember it.
    pub(crate) fn attach_client(&mut self, client: &matrix_sdk::Client) {
        let alive = Arc::new(ClientAlive);
        self.client = Some(Arc::downgrade(&alive));
        client.add_event_handler_context(ClientSentinel(alive));
    }

    fn is_dead(&self) -> bool {
        self.client.as_ref().is_none_or(|c| c.strong_count() == 0)
            && self.stores.iter().all(|(_, w)| w.strong_count() == 0)
    }
}

fn registry() -> &'static Mutex<HashMap<PathBuf, Vec<Arc<StoreProbe>>>> {
    static REGISTRY: OnceLock<Mutex<HashMap<PathBuf, Vec<Arc<StoreProbe>>>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Record a successful client build's stores under its store directory.
pub(crate) fn register(store_path: &Path, probe: StoreProbe) {
    if let Ok(mut map) = registry().lock() {
        let list = map.entry(store_path.to_path_buf()).or_default();
        list.retain(|p| !p.is_dead());
        list.push(Arc::new(probe));
    }
}

/// The probes recorded for `store_path` by the handle `owner` (or by no
/// handle) that are still alive now. Taken by retirement BEFORE it drops
/// anything. A newer handle's stores in the same directory are not this
/// handle's to wait for.
pub(crate) fn snapshot(store_path: &Path, owner: usize) -> Vec<Arc<StoreProbe>> {
    let Ok(mut map) = registry().lock() else {
        return Vec::new();
    };
    let Some(list) = map.get_mut(store_path) else {
        return Vec::new();
    };
    list.retain(|p| !p.is_dead());
    let mine = list
        .iter()
        .filter(|p| p.owner.is_none_or(|o| o == owner))
        .cloned()
        .collect();
    if list.is_empty() {
        map.remove(store_path);
    }
    mine
}

/// How the retiring handle's runtime shutdown ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RuntimeShutdown {
    /// Every task and blocking job ended within the budget.
    Completed,
    /// The budget ran out: a blocking job (an SQLite query holding a pool
    /// connection) may still be running.
    TimedOut,
    /// Another thread still held the runtime (a sign-in thread inside
    /// `run_async_on`), so it was not shut down here and keeps running.
    Shared,
}

impl RuntimeShutdown {
    /// `shutdown_timeout` returns no verdict; it returns early only when every
    /// blocking thread has exited, so a shutdown that took the whole budget is
    /// one that gave up.
    pub(crate) fn from_elapsed(elapsed: Duration, budget: Duration) -> Self {
        if elapsed >= budget {
            Self::TimedOut
        } else {
            Self::Completed
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Completed => "completed",
            Self::TimedOut => "timed_out",
            Self::Shared => "shared",
        }
    }
}

/// What the wait found.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Released {
    /// Every recorded store (and Client) is gone.
    pub closed: bool,
    pub waited_ms: u64,
    /// Whether a `Client` was still alive at the end.
    pub client_alive: bool,
    /// Names of stores still held at the end ("state", "crypto", ...).
    pub still_open: Vec<&'static str>,
    /// How many client builds were being waited for.
    pub builds: usize,
    /// How the runtime shutdown before the wait ended. Anything but
    /// `Completed` means a blocking job may still hold a connection, whatever
    /// the probes say.
    pub runtime: RuntimeShutdown,
}

impl Released {
    /// `store_closed=… waited_ms=… builds=… client_alive=… still_open=…
    /// runtime_shutdown=…`. Names and counts only.
    pub(crate) fn summary(&self) -> String {
        format!(
            "store_closed={} waited_ms={} builds={} client_alive={} still_open={} \
             runtime_shutdown={}",
            self.closed,
            self.waited_ms,
            self.builds,
            self.client_alive,
            if self.still_open.is_empty() {
                "none".to_owned()
            } else {
                self.still_open.join(",")
            },
            self.runtime.as_str()
        )
    }
}

/// Wait until every probe in `probes` is dead, or `budget` runs out, and
/// give the verdict together with how the runtime shutdown before it ended.
/// Polls: the last references may be dropped on another thread (a detached
/// sync thread finishing its cancellation, a blocking close completing).
pub(crate) fn wait_released(
    probes: &[Arc<StoreProbe>],
    budget: Duration,
    runtime: RuntimeShutdown,
) -> Released {
    let start = Instant::now();
    let deadline = start + budget;
    loop {
        if probes.iter().all(|p| p.is_dead()) || Instant::now() >= deadline {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let client_alive = probes
        .iter()
        .any(|p| p.client.as_ref().is_some_and(|c| c.strong_count() > 0));
    let mut still_open: Vec<&'static str> = probes
        .iter()
        .flat_map(|p| p.stores.iter())
        .filter(|(_, w)| w.strong_count() > 0)
        .map(|(name, _)| *name)
        .collect();
    still_open.sort_unstable();
    still_open.dedup();
    Released {
        closed: !client_alive && still_open.is_empty() && runtime == RuntimeShutdown::Completed,
        waited_ms: start.elapsed().as_millis() as u64,
        client_alive,
        still_open,
        builds: probes.len(),
        runtime,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_dropped_store_and_client_read_as_released() {
        let store = Arc::new(5u32);
        let alive = Arc::new(ClientAlive);
        let probe = Arc::new(StoreProbe {
            stores: vec![("state", Arc::downgrade(&store) as AnyWeak)],
            client: Some(Arc::downgrade(&alive)),
            owner: None,
        });
        let held = wait_released(&[Arc::clone(&probe)], Duration::from_millis(20), RuntimeShutdown::Completed);
        assert!(!held.closed);
        assert!(held.client_alive);
        assert_eq!(held.still_open, vec!["state"]);
        assert!(held.waited_ms >= 20, "the wait was not bounded by its budget");

        drop(alive);
        let held = wait_released(&[Arc::clone(&probe)], Duration::from_millis(20), RuntimeShutdown::Completed);
        assert!(!held.closed, "a store outliving its Client is still open");
        assert!(!held.client_alive);

        drop(store);
        let released = wait_released(&[probe], Duration::from_secs(5), RuntimeShutdown::Completed);
        assert!(released.closed);
        assert!(released.waited_ms < 1000, "a released store must not wait out the budget");
        assert_eq!(
            released.summary(),
            format!(
                "store_closed=true waited_ms={} builds=1 client_alive=false still_open=none \
                 runtime_shutdown=completed",
                released.waited_ms
            )
        );
    }

    #[test]
    fn a_release_on_another_thread_ends_the_wait() {
        let store = Arc::new(());
        let mut probe = StoreProbe::default();
        probe.store("crypto", &store);
        let probe = Arc::new(probe);
        let dropper = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(30));
            drop(store);
        });
        let released = wait_released(&[probe], Duration::from_secs(5), RuntimeShutdown::Completed);
        dropper.join().unwrap();
        assert!(released.closed);
    }

    #[test]
    fn snapshot_forgets_dead_builds() {
        let path = std::env::temp_dir().join(format!("storeclose-snapshot-{}", std::process::id()));
        let store = Arc::new(());
        let mut probe = StoreProbe::default();
        probe.store("state", &store);
        register(&path, probe);
        assert_eq!(snapshot(&path, 1).len(), 1);
        drop(store);
        assert!(snapshot(&path, 1).is_empty());
    }

    // A -> B -> A: the old A retiring must not wait for the new A's stores in
    // the same directory, or it waits out its budget and reports them open.
    #[test]
    fn a_handle_waits_only_for_its_own_builds() {
        let path = std::env::temp_dir().join(format!("storeclose-owner-{}", std::process::id()));
        let (old, new) = (0x1000usize, 0x2000usize);
        let old_store = Arc::new(());
        let new_store = Arc::new(());
        let unclaimed_store = Arc::new(());
        for (owner, store) in [(Some(old), &old_store), (Some(new), &new_store), (None, &unclaimed_store)] {
            let mut probe = StoreProbe::default();
            probe.store("state", store);
            probe.set_owner(owner);
            register(&path, probe);
        }
        // Its own build and the unclaimed one, never the successor's.
        assert_eq!(snapshot(&path, old).len(), 2);
        drop(old_store);
        drop(unclaimed_store);
        let mine = snapshot(&path, old);
        assert!(mine.is_empty(), "the old handle is waiting for the new one's stores");
        let released = wait_released(&mine, Duration::from_secs(5), RuntimeShutdown::Completed);
        assert!(released.closed && released.waited_ms < 1000, "{}", released.summary());
        // The successor's stores are still recorded for the successor.
        assert_eq!(snapshot(&path, new).len(), 1);
        drop(new_store);
    }

    // The probes alone cannot see a blocking job that outlived the runtime
    // shutdown: it holds a pool connection, not the store Arc.
    #[test]
    fn a_runtime_shutdown_that_ran_out_of_time_is_not_a_closed_store() {
        let budget = Duration::from_millis(1500);
        assert_eq!(
            RuntimeShutdown::from_elapsed(Duration::from_millis(3), budget),
            RuntimeShutdown::Completed
        );
        assert_eq!(RuntimeShutdown::from_elapsed(budget, budget), RuntimeShutdown::TimedOut);
        assert_eq!(
            RuntimeShutdown::from_elapsed(Duration::from_millis(1600), budget),
            RuntimeShutdown::TimedOut
        );
        for (runtime, word) in [
            (RuntimeShutdown::Completed, "completed"),
            (RuntimeShutdown::TimedOut, "timed_out"),
            (RuntimeShutdown::Shared, "shared"),
        ] {
            // Every probe dead: only the runtime decides.
            let released = wait_released(&[], Duration::from_millis(10), runtime);
            assert_eq!(released.closed, runtime == RuntimeShutdown::Completed, "{runtime:?}");
            let summary = released.summary();
            assert!(
                summary.starts_with(&format!("store_closed={} ", released.closed)),
                "{summary}"
            );
            assert!(summary.ends_with(&format!(" runtime_shutdown={word}")), "{summary}");
            assert!(summary.contains(" builds=0 ") && summary.contains(" still_open=none "));
        }
    }
}
