
use std::any::Any;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

type AnyWeak = Weak<dyn Any + Send + Sync>;

#[derive(Default)]
pub(crate) struct StoreProbe {
    stores: Vec<(&'static str, AnyWeak)>,
    client: Option<Weak<ClientAlive>>,
    owner: Option<usize>,
    serial: Option<u64>,
    path_id: Option<PathId>,
    reservation: Option<Reservation>,
}

/// Lives in the Client's event-handler context map, i.e. inside
/// `ClientInner`, so it is dropped exactly when the last `Client` is.
/// Never read by a handler.
#[derive(Clone)]
pub(crate) struct ClientSentinel(#[allow(dead_code)] Arc<ClientAlive>);

pub(crate) struct ClientAlive;

impl StoreProbe {
    pub(crate) fn store<T: Any + Send + Sync>(&mut self, name: &'static str, store: &Arc<T>) {
        let weak: Weak<T> = Arc::downgrade(store);
        self.stores.push((name, weak as AnyWeak));
    }

    pub(crate) fn set_owner(&mut self, owner: Option<usize>) {
        self.owner = owner;
    }

    pub(crate) fn hold_reservation(&mut self, reservation: Reservation) {
        self.reservation = Some(reservation);
    }

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

#[derive(Default)]
struct Registry {
    probes: HashMap<PathBuf, Vec<Arc<StoreProbe>>>,
    reservations: HashMap<PathId, (u64, usize)>,
}

impl std::ops::Deref for Registry {
    type Target = HashMap<PathBuf, Vec<Arc<StoreProbe>>>;
    fn deref(&self) -> &Self::Target {
        &self.probes
    }
}

impl std::ops::DerefMut for Registry {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.probes
    }
}

fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
}

pub(crate) fn register(store_path: &Path, mut probe: StoreProbe) {
    let converted = probe.reservation.take().map(Reservation::disarm);
    if let Some((id, serial)) = &converted {
        probe.serial = Some(*serial);
        probe.path_id = Some(id.clone());
    }
    if let Ok(mut map) = registry().lock() {
        if let Some((id, serial)) = &converted {
            release_locked(&mut map, id, *serial);
        }
        let list = map.entry(store_path.to_path_buf()).or_default();
        list.retain(|p| !p.is_dead());
        list.push(Arc::new(probe));
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(crate) enum PathId {
    #[cfg_attr(not(unix), allow(dead_code))]
    Inode(u64, u64),
    #[cfg_attr(unix, allow(dead_code))]
    Canonical(PathBuf),
}

pub(crate) fn path_id(path: &Path) -> std::io::Result<PathId> {
    let canonical = std::fs::canonicalize(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        let meta = std::fs::metadata(&canonical)?;
        Ok(PathId::Inode(meta.dev(), meta.ino()))
    }
    #[cfg(not(unix))]
    {
        // std's volume serial and file index are unstable (windows_by_handle);
        // `canonicalize` resolves the final path, true case included.
        Ok(PathId::Canonical(canonical))
    }
}

#[derive(Debug)]
pub(crate) struct Reservation {
    id: PathId,
    serial: u64,
    armed: bool,
}

impl Reservation {
    fn disarm(mut self) -> (PathId, u64) {
        self.armed = false;
        (self.id.clone(), self.serial)
    }
}

impl Drop for Reservation {
    fn drop(&mut self) {
        if self.armed {
            if let Ok(mut map) = registry().lock() {
                release_locked(&mut map, &self.id, self.serial);
            }
        }
    }
}

fn release_locked(map: &mut Registry, id: &PathId, serial: u64) {
    if let Some((holder, count)) = map.reservations.get_mut(id) {
        if *holder == serial {
            *count = count.saturating_sub(1);
            if *count == 0 {
                map.reservations.remove(id);
            }
        }
    }
}

fn foreign_probe_locked(map: &mut Registry, id: &PathId, serial: u64) -> Option<u64> {
    let mut holder = None;
    map.probes.retain(|key, list| {
        list.retain(|p| !p.is_dead());
        if holder.is_none() {
            for p in list.iter() {
                let same = match &p.path_id {
                    Some(pid) => pid == id,
                    None => path_id(key).is_ok_and(|pid| &pid == id),
                };
                if same && p.serial != Some(serial) {
                    holder = Some(p.serial.unwrap_or(0));
                    break;
                }
            }
        }
        !list.is_empty()
    });
    holder
}

static RESERVE_CALLS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

#[cfg(test)]
pub(crate) fn reserve_calls() -> u64 {
    RESERVE_CALLS.load(std::sync::atomic::Ordering::SeqCst)
}

/// Reserve `id` for `serial`, before anything touches a file in it. `Err`
/// names the serial (0: unknown) that holds it.
pub(crate) fn reserve(id: PathId, serial: u64) -> Result<Reservation, u64> {
    RESERVE_CALLS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let Ok(mut map) = registry().lock() else {
        // Cannot tell: refuse, so nothing opens a store another may hold.
        return Err(0);
    };
    if let Some(holder) = foreign_probe_locked(&mut map, &id, serial) {
        return Err(holder);
    }
    match map.reservations.get_mut(&id) {
        Some((holder, _)) if *holder != serial => return Err(*holder),
        Some((_, count)) => *count += 1,
        None => {
            map.reservations.insert(id.clone(), (serial, 1));
        }
    }
    Ok(Reservation { id, serial, armed: true })
}

pub(crate) fn any_held_id(id: &PathId) -> bool {
    let Ok(mut map) = registry().lock() else {
        return true;
    };
    if map.reservations.contains_key(id) {
        return true;
    }
    foreign_probe_locked(&mut map, id, u64::MAX).is_some()
}

pub(crate) fn any_held_path(path: &Path) -> i32 {
    match path_id(path) {
        Ok(id) => i32::from(any_held_id(&id)),
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => 0,
        Err(_) => -1,
    }
}

#[cfg(test)]
pub(crate) fn snapshot(store_path: &Path, owner: usize) -> Vec<Arc<StoreProbe>> {
    snapshot_for(store_path, owner, None)
}

pub(crate) fn snapshot_for(
    store_path: &Path,
    owner: usize,
    serial: Option<u64>,
) -> Vec<Arc<StoreProbe>> {
    let Ok(mut map) = registry().lock() else {
        return Vec::new();
    };
    let Some(list) = map.get_mut(store_path) else {
        return Vec::new();
    };
    list.retain(|p| !p.is_dead());
    let mine = list
        .iter()
        .filter(|p| match p.serial {
            Some(s) => serial == Some(s),
            None => p.owner.is_none_or(|o| o == owner),
        })
        .cloned()
        .collect();
    if list.is_empty() {
        map.remove(store_path);
    }
    mine
}

pub(crate) fn any_held(store_path: &Path) -> bool {
    let Ok(mut map) = registry().lock() else {
        // Cannot tell: assume held, so nothing is moved under an open store.
        return true;
    };
    let Some(list) = map.get_mut(store_path) else {
        return false;
    };
    list.retain(|p| !p.is_dead());
    let held = !list.is_empty();
    if !held {
        map.remove(store_path);
    }
    held
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RuntimeShutdown {
    Completed,
    TimedOut,
    Shared,
}

impl RuntimeShutdown {
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

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Released {
    pub closed: bool,
    pub waited_ms: u64,
    pub client_alive: bool,
    pub still_open: Vec<&'static str>,
    pub builds: usize,
    pub runtime: RuntimeShutdown,
}

impl Released {
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
            ..Default::default()
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
        assert_eq!(snapshot(&path, old).len(), 2);
        drop(old_store);
        drop(unclaimed_store);
        let mine = snapshot(&path, old);
        assert!(mine.is_empty(), "the old handle is waiting for the new one's stores");
        let released = wait_released(&mine, Duration::from_secs(5), RuntimeShutdown::Completed);
        assert!(released.closed && released.waited_ms < 1000, "{}", released.summary());
        assert_eq!(snapshot(&path, new).len(), 1);
        drop(new_store);
    }

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

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "storeclose-{tag}-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn two_serials_racing_for_one_store_get_exactly_one_reservation() {
        let _labs = crate::labs::LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let dir = temp_dir("race");
        for round in 0..50u64 {
            let id = path_id(&dir).unwrap();
            let barrier = Arc::new(std::sync::Barrier::new(2));
            let handles: Vec<_> = [1000 + 2 * round, 1001 + 2 * round]
                .into_iter()
                .map(|serial| {
                    let id = id.clone();
                    let barrier = Arc::clone(&barrier);
                    std::thread::spawn(move || {
                        barrier.wait();
                        reserve(id, serial)
                    })
                })
                .collect();
            let results: Vec<_> = handles.into_iter().map(|h| h.join().unwrap()).collect();
            let wins = results.iter().filter(|r| r.is_ok()).count();
            assert_eq!(wins, 1, "round {round}: {results:?}");
            let loser = results.iter().find_map(|r| r.as_ref().err().copied()).unwrap();
            let winner = results.iter().find_map(|r| r.as_ref().ok()).unwrap();
            assert_eq!(loser, winner.serial, "the refusal names the holder");
            drop(results);
            assert!(!any_held_id(&id), "a dropped reservation is released");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_same_serial_may_re_enter_and_a_reservation_counts_as_held() {
        let _labs = crate::labs::LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let dir = temp_dir("reenter");
        let id = path_id(&dir).unwrap();
        assert_eq!(any_held_path(&dir), 0);
        let first = reserve(id.clone(), 7).expect("first");
        assert_eq!(any_held_path(&dir), 1, "a reservation is a hold");
        let again = reserve(id.clone(), 7).expect("a restore rebuilding after its timeout");
        assert_eq!(reserve(id.clone(), 8).unwrap_err(), 7);
        drop(first);
        assert!(any_held_id(&id), "one re-entry still holds it");
        assert_eq!(reserve(id.clone(), 8).unwrap_err(), 7);
        drop(again);
        assert!(!any_held_id(&id));
        assert!(reserve(id, 8).is_ok());
        assert_eq!(any_held_path(&dir.join("never-created")), 0, "absent is free");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[cfg(unix)]
    #[test]
    fn two_spellings_of_one_directory_are_one_identity() {
        let _labs = crate::labs::LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let dir = temp_dir("alias");
        let link = dir.with_extension("link");
        let _ = std::fs::remove_file(&link);
        std::os::unix::fs::symlink(&dir, &link).unwrap();
        assert_eq!(path_id(&dir).unwrap(), path_id(&link).unwrap());
        let held = reserve(path_id(&dir).unwrap(), 21).unwrap();
        assert_eq!(reserve(path_id(&link).unwrap(), 22).unwrap_err(), 21);
        assert_eq!(any_held_path(&link), 1);
        drop(held);
        let _ = std::fs::remove_file(&link);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn register_converts_the_reservation_and_retirement_keys_on_the_serial() {
        let _labs = crate::labs::LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let dir = temp_dir("convert");
        let id = path_id(&dir).unwrap();
        let store = Arc::new(());
        let mut probe = StoreProbe::default();
        probe.store("state", &store);
        probe.set_owner(Some(0xdead));
        probe.hold_reservation(reserve(id.clone(), 31).unwrap());
        register(&dir, probe);
        assert!(any_held_id(&id), "the probe holds it now");
        assert_eq!(reserve(id.clone(), 32).unwrap_err(), 31, "a live probe refuses another serial");
        assert!(reserve(id.clone(), 31).is_ok(), "its own serial may re-enter");
        assert_eq!(snapshot_for(&dir, 0xdead, Some(32)).len(), 0, "never another serial's");
        assert_eq!(snapshot_for(&dir, 0xbeef, Some(31)).len(), 1, "the serial decides, not the owner");
        drop(store);
        assert!(!any_held_id(&id));
        assert!(reserve(id, 32).is_ok());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_build_from_before_labs_still_refuses_a_reservation() {
        let _labs = crate::labs::LABS_TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let dir = temp_dir("prelabs");
        let store = Arc::new(());
        let mut probe = StoreProbe::default();
        probe.store("crypto", &store);
        register(&dir, probe);
        assert_eq!(reserve(path_id(&dir).unwrap(), 41).unwrap_err(), 0);
        assert_eq!(any_held_path(&dir), 1);
        drop(store);
        assert!(reserve(path_id(&dir).unwrap(), 41).is_ok());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn without_a_serial_the_owner_rule_is_unchanged() {
        let path = std::env::temp_dir().join(format!("storeclose-noserial-{}", std::process::id()));
        let (mine, theirs) = (Arc::new(()), Arc::new(()));
        for (owner, store) in [(Some(1usize), &mine), (Some(2usize), &theirs)] {
            let mut probe = StoreProbe::default();
            probe.store("state", store);
            probe.set_owner(owner);
            register(&path, probe);
        }
        assert_eq!(snapshot(&path, 1).len(), 1);
        assert_eq!(snapshot_for(&path, 1, Some(99)).len(), 1);
        assert_eq!(snapshot_for(&path, 1, None).len(), 1);
        drop(mine);
        drop(theirs);
        assert!(snapshot(&path, 1).is_empty());
    }
}
