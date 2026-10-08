//! SDK leases are unnecessary only while the application owns its OS profile lock.
//! Keep the SDK's multi-process protection for callers without that guarantee.

use std::sync::atomic::{AtomicBool, Ordering};

use matrix_sdk::cross_process_lock::CrossProcessLockConfig;

static PROFILE_LOCK_HELD: AtomicBool = AtomicBool::new(false);

/// Called before any clients are created, only after the GUI owns the profile
/// lock. That OS lock must remain held until actual process exit, including
/// asynchronous SDK retirement. There is deliberately no reset operation.
#[no_mangle]
pub extern "C" fn mx_rust_profile_lock_held() {
    PROFILE_LOCK_HELD.store(true, Ordering::Release);
}

pub(crate) fn config() -> CrossProcessLockConfig {
    config_for_profile_lock(PROFILE_LOCK_HELD.load(Ordering::Acquire))
}

fn config_for_profile_lock(held: bool) -> CrossProcessLockConfig {
    if held {
        CrossProcessLockConfig::SingleProcess
    } else {
        // The SDK's default "main" is shared by every process. A lease holder
        // must identify this process, otherwise both processes can renew it.
        CrossProcessLockConfig::multi_process(format!("lightning-{}", std::process::id()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_os_profile_lock_allows_single_process_stores() {
        assert!(matches!(config_for_profile_lock(true), CrossProcessLockConfig::SingleProcess));
    }

    #[test]
    fn unguarded_callers_keep_process_specific_sdk_leases() {
        let config = config_for_profile_lock(false);
        assert_eq!(config.holder_name(), Some(format!("lightning-{}", std::process::id()).as_str()));
    }
}
