//! Opt-in disk-write attribution (`LIGHTNING_IO_TRACE=1`).
//!
//! 2026-10-07 the SDK threads wrote ~1.5 MB/s for the first minutes after
//! start (docs/open-items.md, "Sustained disk writes"). Whether that settles
//! after catch-up is not known, and attributing it needs ptrace, which yama
//! scope 1 blocks. Linux keeps per-thread I/O counters in
//! `/proc/self/task/<tid>/io`, readable without ptrace, so this logs, every
//! 10 s, the bytes each thread NAME wrote since the last line and the
//! cumulative total. Numbers only: no paths, ids or content. Off unless the
//! variable is set; Linux only (elsewhere it does nothing). Lines travel as
//! `rust_trace` events and are logged by the Qt side, so `--log-file` captures
//! them (the bridge's `eprintln!` does not reach it).

use std::collections::BTreeMap;
use std::sync::Once;
use std::time::Duration;

const PERIOD: Duration = Duration::from_secs(10);

/// `write_bytes` from the text of a `/proc/<pid>/task/<tid>/io` file: bytes
/// this thread caused to be sent to storage (not the page-cache `wchar`).
pub(crate) fn parse_write_bytes(io_text: &str) -> Option<u64> {
    io_text
        .lines()
        .find_map(|l| l.strip_prefix("write_bytes:"))
        .and_then(|v| v.trim().parse().ok())
}

/// Per-thread-name growth between two samples (`name -> bytes`), dropping
/// names that wrote nothing. A counter that went backwards (a thread id
/// reused under the same name) counts as zero rather than wrapping.
pub(crate) fn growth(prev: &BTreeMap<String, u64>, now: &BTreeMap<String, u64>) -> BTreeMap<String, u64> {
    now.iter()
        .filter_map(|(name, &n)| {
            let d = n.saturating_sub(prev.get(name).copied().unwrap_or(0));
            (d > 0).then(|| (name.clone(), d))
        })
        .collect()
}

/// One log line; numbers and thread names only.
pub(crate) fn trace_line(t_secs: u64, wrote: u64, total: u64, parts: &[String]) -> String {
    format!("io-trace t={t_secs}s wrote={wrote} total={total} threads=[{}]", parts.join(" "))
}

#[cfg(target_os = "linux")]
fn sample() -> BTreeMap<String, u64> {
    let mut out: BTreeMap<String, u64> = BTreeMap::new();
    let Ok(dir) = std::fs::read_dir("/proc/self/task") else { return out };
    for entry in dir.flatten() {
        let path = entry.path();
        let comm = std::fs::read_to_string(path.join("comm")).unwrap_or_default();
        let Some(bytes) = std::fs::read_to_string(path.join("io"))
            .ok()
            .and_then(|t| parse_write_bytes(&t))
        else {
            continue;
        };
        *out.entry(comm.trim().to_owned()).or_default() += bytes;
    }
    out
}

#[cfg(not(target_os = "linux"))]
fn sample() -> BTreeMap<String, u64> {
    BTreeMap::new()
}

/// The queue the sampler writes to: re-bound by every bridge that starts
/// (accounts come and go; the sampler is per process), cleared by none.
static SINK: std::sync::Mutex<Option<crate::EventQueueRef>> = std::sync::Mutex::new(None);

/// Start the sampler once per process when `LIGHTNING_IO_TRACE` is set, and
/// point it at this bridge's queue.
pub(crate) fn start_if_enabled(events: crate::EventQueueRef) {
    static ONCE: Once = Once::new();
    if std::env::var_os("LIGHTNING_IO_TRACE").is_none() {
        return;
    }
    if let Ok(mut sink) = SINK.lock() {
        *sink = Some(events);
    }
    ONCE.call_once(|| {
        let _ = std::thread::Builder::new().name("lightning-iotrace".into()).spawn(|| {
            let mut prev = sample();
            let mut total: u64 = 0;
            let mut tick: u64 = 0;
            loop {
                std::thread::sleep(PERIOD);
                tick += 1;
                let now = sample();
                let grew = growth(&prev, &now);
                let sum: u64 = grew.values().sum();
                total += sum;
                let parts: Vec<String> =
                    grew.iter().map(|(n, b)| format!("{n}={b}")).collect();
                let sink = SINK.lock().ok().and_then(|s| s.clone());
                if let Some(events) = sink {
                    crate::enqueue(&events, serde_json::json!({
                        "type": "rust_trace",
                        "line": trace_line(tick * PERIOD.as_secs(), sum, total, &parts),
                    }));
                }
                prev = now;
            }
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn write_bytes_is_read_from_the_proc_text() {
        let text = "rchar: 5\nwchar: 6\nsyscr: 1\nsyscw: 1\nread_bytes: 0\nwrite_bytes: 4096\ncancelled_write_bytes: 0\n";
        assert_eq!(parse_write_bytes(text), Some(4096));
        assert_eq!(parse_write_bytes("wchar: 6\n"), None);
        assert_eq!(parse_write_bytes("write_bytes: x\n"), None);
    }

    #[test]
    fn the_trace_line_is_numbers_and_thread_names() {
        assert_eq!(
            trace_line(20, 5, 9, &["a=3".into(), "b=2".into()]),
            "io-trace t=20s wrote=5 total=9 threads=[a=3 b=2]"
        );
    }

    #[test]
    fn growth_reports_only_what_grew_and_never_wraps() {
        let prev = BTreeMap::from([("a".to_owned(), 100), ("b".to_owned(), 50)]);
        let now = BTreeMap::from([("a".to_owned(), 160), ("b".to_owned(), 10), ("c".to_owned(), 7)]);
        let g = growth(&prev, &now);
        assert_eq!(g.get("a"), Some(&60));
        assert_eq!(g.get("b"), None);
        assert_eq!(g.get("c"), Some(&7));
    }
}
