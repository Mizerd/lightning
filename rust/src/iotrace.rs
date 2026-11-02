
use std::collections::BTreeMap;
use std::sync::Once;
use std::time::Duration;

const PERIOD: Duration = Duration::from_secs(10);

pub(crate) fn parse_write_bytes(io_text: &str) -> Option<u64> {
    io_text
        .lines()
        .find_map(|l| l.strip_prefix("write_bytes:"))
        .and_then(|v| v.trim().parse().ok())
}

pub(crate) fn growth(prev: &BTreeMap<String, u64>, now: &BTreeMap<String, u64>) -> BTreeMap<String, u64> {
    now.iter()
        .filter_map(|(name, &n)| {
            let d = n.saturating_sub(prev.get(name).copied().unwrap_or(0));
            (d > 0).then(|| (name.clone(), d))
        })
        .collect()
}

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
