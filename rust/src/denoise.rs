//! DeepFilterNet microphone noise suppression for calls (issue #20).
//!
//! A small, panic-safe C ABI over the vendored libDF streaming inference path
//! (`third_party/deepfilternet/`, crate `deep_filter`, `df::tract::DfTract`).
//! The C++ side (`src/calls/noise/DeepFilterSuppressor.*`) owns one handle per
//! microphone chain and calls [`mx_df_process`] on the GStreamer streaming
//! thread, one 10 ms frame (480 mono f32 samples at 48 kHz) at a time.
//!
//! The model is embedded with `include_bytes!` from the vendored tarball, so
//! nothing is read from disk or the network at runtime.
//!
//! Real-time notes, measured in the standalone harness (see the vault note
//! `Tasks/2026-10-06-nc-dfn.md`): inference is synchronous and runs on the
//! caller's thread. tract allocates its intermediate tensors per frame; there
//! are no locks, no I/O and no logging on the per-frame path (the vendored
//! copy removes upstream's per-frame `log` calls). Creation (model parse and
//! optimisation) takes tens to hundreds of milliseconds and must NOT run on
//! the streaming thread or the GUI thread. `mx_df_reset` is allocation-free
//! and may run on the streaming thread.
//!
//! Sample contract: mono f32, nominally [-1, 1]. Non-finite input samples are
//! replaced by 0 and finite ones are clamped to [-1, 1] before they reach the
//! model, because a single NaN would otherwise poison libDF's running
//! normalisation state for the rest of the call. If the model nevertheless
//! produces a non-finite sample, the frame is silenced and the state is reset
//! (`MX_DF_RECOVERED`).
//!
//! Without the `deepfilternet` cargo feature every entry point still exists:
//! `mx_df_available` returns 0 and `mx_df_create` returns null, so a C++ build
//! that was configured inconsistently fails safe (pass-through) rather than at
//! link time.

use std::os::raw::{c_float, c_int};

/// Frame processed (enhanced, or libDF's own silence/skip path).
pub const MX_DF_OK: c_int = 0;
/// The model produced a non-finite sample: the frame was zeroed and the
/// state was reset. The handle remains usable.
pub const MX_DF_RECOVERED: c_int = 1;
/// Null handle or null buffer. Buffer untouched.
pub const MX_DF_ERR_NULL: c_int = -1;
/// `n` is not the frame size. Buffer untouched.
pub const MX_DF_ERR_SIZE: c_int = -2;
/// The inference returned an error. Buffer untouched (unprocessed input).
pub const MX_DF_ERR_PROCESS: c_int = -3;
/// A panic was caught. The handle is poisoned (every later process call
/// returns this and leaves the buffer untouched) until `mx_df_reset`.
pub const MX_DF_ERR_PANIC: c_int = -4;
/// Built without the `deepfilternet` feature.
pub const MX_DF_ERR_UNAVAILABLE: c_int = -5;

#[cfg(feature = "deepfilternet")]
mod imp {
    use super::*;
    use std::panic::{catch_unwind, AssertUnwindSafe};

    use df::tract::{DfParams, DfTract, RuntimeParams};
    use df::ndarray::{ArrayView2, ArrayViewMut2};

    /// The vendored DeepFilterNet3 model (upstream `models/DeepFilterNet3_onnx.tar.gz`,
    /// sha256 recorded in third_party/deepfilternet/PROVENANCE.md).
    pub(super) static MODEL_TAR_GZ: &[u8] =
        include_bytes!("../../third_party/deepfilternet/models/DeepFilterNet3_onnx.tar.gz");

    /// Live handles, so the tests can prove `mx_df_destroy` drops them.
    #[cfg(test)]
    static LIVE: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    #[cfg(test)]
    pub(super) static LIVE_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    /// Test hook: the next `process()` panics right after tract has updated
    /// the model state, i.e. a panic in the middle of a frame.
    #[cfg(test)]
    pub(super) static PANIC_AFTER_INFERENCE: std::sync::atomic::AtomicBool =
        std::sync::atomic::AtomicBool::new(false);
    #[cfg(test)]
    pub(super) fn live_handles() -> usize {
        LIVE.load(std::sync::atomic::Ordering::SeqCst)
    }

    pub struct DfHandle {
        model: DfTract,
        /// A fresh state kept ready, so `reset()` is a swap: no allocation,
        /// no deallocation, no model parsing. tract keeps its op states in
        /// private types (pulse delay/pad buffers, scan hidden states), so a
        /// literal in-place zeroing is not possible from outside tract; the
        /// swap gives the same result without touching the heap.
        spare: DfTract,
        /// `spare` is fresh and may be swapped in.
        spare_ready: bool,
        /// `model` has processed nothing since it was made fresh, so a reset
        /// has nothing to do.
        model_fresh: bool,
        /// The template `spare` is re-cloned from after it has been used. The
        /// re-clone happens inside the NEXT `process()` call (which allocates
        /// in tract anyway), never inside `reset()`.
        pristine: DfTract,
        input: Vec<f32>,
        output: Vec<f32>,
        hop: usize,
        latency: usize,
        poisoned: bool,
    }

    pub fn create(atten_lim_db: f32) -> Option<Box<DfHandle>> {
        // Default upstream runtime parameters (as the LADSPA plugin and the
        // `deep-filter` binary use) with the requested attenuation limit.
        // A limit >= 100 dB means "unlimited"; non-finite input falls back to
        // unlimited too.
        let lim = if atten_lim_db.is_finite() { atten_lim_db.abs() } else { 100.0 };
        let rp = RuntimeParams::default_with_ch(1).with_atten_lim(lim);
        let params = DfParams::from_bytes(MODEL_TAR_GZ).ok()?;
        let model = DfTract::new(params, &rp).ok()?;
        let hop = model.hop_size;
        if hop == 0 || model.sr != 48_000 || model.ch != 1 {
            return None;
        }
        // Algorithmic delay: the STFT overlap (fft_size - hop) plus the
        // model's lookahead in frames. Checked against the delay measured by
        // cross-correlation in `reported_latency_matches_the_measured_delay`.
        let latency = model.fft_size.saturating_sub(hop) + model.lookahead * hop;
        let pristine = model.clone();
        let spare = model.clone();
        #[cfg(test)]
        LIVE.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        Some(Box::new(DfHandle {
            model,
            spare,
            spare_ready: true,
            model_fresh: true,
            pristine,
            input: vec![0.0; hop],
            output: vec![0.0; hop],
            hop,
            latency,
            poisoned: false,
        }))
    }

    // The C++ side creates a handle on a control thread and then processes on
    // the streaming thread (never both at once), so the handle moves between
    // threads. tract's state is not `Send` by type: `TValue::Var` is an
    // `Rc<Tensor>`, and op states are `Box<dyn OpState>` without a `Send`
    // bound.
    //
    // SAFETY: every `Rc` and op state reachable from a `DfHandle` is created by
    // `create()` for that handle alone and is shared only between its own
    // `model` and `pristine` (the clone in `create`/`reset`), never with
    // another handle, a global or a thread-local (tract's thread-locals are
    // per-thread scratch buffers and an executor override, neither of which
    // points into model state; checked against tract-linalg/core 0.21.14).
    // The C ABI requires a handle to be used by one thread at a time, and the
    // caller publishes it to the next thread with a synchronising operation
    // (mutex or atomic exchange), which orders all non-atomic `Rc` count
    // updates. Moving the whole handle is therefore sound; using it from two
    // threads at once is not, and the ABI forbids that.
    unsafe impl Send for DfHandle {}

    #[cfg(test)]
    impl Drop for DfHandle {
        fn drop(&mut self) {
            LIVE.fetch_sub(1, std::sync::atomic::Ordering::SeqCst);
        }
    }

    impl DfHandle {
        pub fn frame_size(&self) -> usize {
            self.hop
        }

        pub fn latency(&self) -> usize {
            self.latency
        }

        /// Real-time safe: no allocation and no deallocation on the normal
        /// path (proven with a counting allocator in the harness, see the
        /// vault note). Only after a caught panic inside the spare's re-clone
        /// (no spare ready AND a used model) does it fall back to cloning.
        pub fn reset(&mut self) {
            if !self.model_fresh {
                if self.spare_ready {
                    std::mem::swap(&mut self.model, &mut self.spare);
                    self.spare_ready = false;
                } else {
                    self.model = self.pristine.clone();
                }
                self.model_fresh = true;
            }
            self.poisoned = false;
            self.input.iter_mut().for_each(|x| *x = 0.0);
            self.output.iter_mut().for_each(|x| *x = 0.0);
        }

        /// Makes `spare` fresh again after a reset used it. Allocates (one
        /// clone of the model state): called from `process()` only.
        fn refill_spare(&mut self) {
            if !self.spare_ready {
                self.spare.clone_from(&self.pristine);
                self.spare_ready = true;
            }
        }

        pub fn set_atten_lim(&mut self, db: f32) {
            let lim = if db.is_finite() { db.abs() } else { 100.0 };
            self.model.set_atten_lim(lim);
            self.spare.set_atten_lim(lim);
            self.pristine.set_atten_lim(lim);
        }

        /// Processes exactly one frame in place. `frame.len()` must equal
        /// `frame_size()`; the FFI layer checks it.
        pub fn process(&mut self, frame: &mut [f32]) -> c_int {
            if self.poisoned {
                return MX_DF_ERR_PANIC;
            }
            for (dst, &src) in self.input.iter_mut().zip(frame.iter()) {
                *dst = if src.is_finite() { src.clamp(-1.0, 1.0) } else { 0.0 };
            }
            let hop = self.hop;
            // Marked used BEFORE inference: if tract panics part-way through,
            // the half-updated state must not count as fresh, or the next
            // reset() would keep it.
            self.model_fresh = false;
            let result = {
                let noisy = match ArrayView2::from_shape((1, hop), &self.input[..]) {
                    Ok(v) => v,
                    Err(_) => return MX_DF_ERR_PROCESS,
                };
                let enh = match ArrayViewMut2::from_shape((1, hop), &mut self.output[..]) {
                    Ok(v) => v,
                    Err(_) => return MX_DF_ERR_PROCESS,
                };
                let r = self.model.process(noisy, enh);
                #[cfg(test)]
                if PANIC_AFTER_INFERENCE.swap(false, std::sync::atomic::Ordering::SeqCst) {
                    panic!("injected panic after inference (test)");
                }
                r
            };
            if result.is_err() {
                self.refill_spare();
                return MX_DF_ERR_PROCESS;
            }
            if self.output.iter().any(|x| !x.is_finite()) {
                frame.iter_mut().for_each(|x| *x = 0.0);
                self.reset();
                self.refill_spare();
                return MX_DF_RECOVERED;
            }
            frame.copy_from_slice(&self.output);
            self.refill_spare();
            MX_DF_OK
        }
    }

    pub fn guarded<R>(handle: &mut DfHandle, f: impl FnOnce(&mut DfHandle) -> R) -> Option<R> {
        match catch_unwind(AssertUnwindSafe(|| f(handle))) {
            Ok(r) => Some(r),
            Err(_) => {
                handle.poisoned = true;
                None
            }
        }
    }

    pub fn create_guarded(atten_lim_db: f32) -> Option<Box<DfHandle>> {
        catch_unwind(|| create(atten_lim_db)).ok().flatten()
    }
}

/// Opaque handle type for the C ABI.
#[cfg(feature = "deepfilternet")]
pub type MxDfHandle = imp::DfHandle;
#[cfg(not(feature = "deepfilternet"))]
pub struct MxDfHandle {
    _private: (),
}

/// 1 when this build carries DeepFilterNet, 0 otherwise.
#[no_mangle]
pub extern "C" fn mx_df_available() -> c_int {
    if cfg!(feature = "deepfilternet") {
        1
    } else {
        0
    }
}

/// Creates a suppressor. `atten_lim_db` limits how far noise is attenuated
/// (e.g. 100 = unlimited, 30 = keep some background). Expensive (parses and
/// optimises the model): never call it on a real-time thread. Returns null
/// on any failure, including a caught panic.
#[no_mangle]
pub extern "C" fn mx_df_create(atten_lim_db: c_float) -> *mut MxDfHandle {
    #[cfg(feature = "deepfilternet")]
    {
        match imp::create_guarded(atten_lim_db) {
            Some(h) => Box::into_raw(h),
            None => std::ptr::null_mut(),
        }
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = atten_lim_db;
        std::ptr::null_mut()
    }
}

/// Samples per `mx_df_process` call (480 for the shipped model), or 0 for a
/// null handle.
///
/// # Safety
/// `handle` must be null or a live pointer from `mx_df_create`.
#[no_mangle]
pub unsafe extern "C" fn mx_df_frame_size(handle: *const MxDfHandle) -> c_int {
    #[cfg(feature = "deepfilternet")]
    {
        match unsafe { handle.as_ref() } {
            Some(h) => h.frame_size() as c_int,
            None => 0,
        }
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = handle;
        0
    }
}

/// Algorithmic delay in samples at 48 kHz, or 0 for a null handle.
///
/// # Safety
/// `handle` must be null or a live pointer from `mx_df_create`.
#[no_mangle]
pub unsafe extern "C" fn mx_df_latency_samples(handle: *const MxDfHandle) -> c_int {
    #[cfg(feature = "deepfilternet")]
    {
        match unsafe { handle.as_ref() } {
            Some(h) => h.latency() as c_int,
            None => 0,
        }
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = handle;
        0
    }
}

/// Processes exactly one frame of `n` mono f32 samples in place. Returns one
/// of the `MX_DF_*` codes; on any negative code the buffer is left untouched,
/// so a caller that ignores the code degrades to pass-through.
///
/// # Safety
/// `handle` must be null or a live pointer from `mx_df_create`, used by one
/// thread at a time; `in_out` must be null or valid for `n` reads and writes.
#[no_mangle]
pub unsafe extern "C" fn mx_df_process(
    handle: *mut MxDfHandle,
    in_out: *mut c_float,
    n: usize,
) -> c_int {
    #[cfg(feature = "deepfilternet")]
    {
        let Some(h) = (unsafe { handle.as_mut() }) else {
            return MX_DF_ERR_NULL;
        };
        if in_out.is_null() {
            return MX_DF_ERR_NULL;
        }
        if n != h.frame_size() {
            return MX_DF_ERR_SIZE;
        }
        let frame = unsafe { std::slice::from_raw_parts_mut(in_out, n) };
        imp::guarded(h, |h| h.process(frame)).unwrap_or(MX_DF_ERR_PANIC)
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = (handle, in_out, n);
        MX_DF_ERR_UNAVAILABLE
    }
}

/// Drops all internal state (and clears a poisoned handle) without re-parsing
/// the model. Real-time safe: it swaps in a fresh state kept ready for this,
/// with no allocation, no deallocation, no lock and no I/O, so it may be
/// called on the streaming thread (e.g. on a DISCONT after unmute). The next
/// `mx_df_process` call re-prepares the spare (one state clone, inside a call
/// that allocates in tract anyway). Two resets with no frame in between: the
/// second is a no-op.
///
/// # Safety
/// `handle` must be null or a live pointer from `mx_df_create`, not in use
/// by another thread.
#[no_mangle]
pub unsafe extern "C" fn mx_df_reset(handle: *mut MxDfHandle) -> c_int {
    #[cfg(feature = "deepfilternet")]
    {
        let Some(h) = (unsafe { handle.as_mut() }) else {
            return MX_DF_ERR_NULL;
        };
        match imp::guarded(h, |h| h.reset()) {
            Some(()) => MX_DF_OK,
            None => MX_DF_ERR_PANIC,
        }
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = handle;
        MX_DF_ERR_UNAVAILABLE
    }
}

/// Changes the attenuation limit (dB, >= 100 = unlimited) of a live handle.
///
/// # Safety
/// As for `mx_df_reset`.
#[no_mangle]
pub unsafe extern "C" fn mx_df_set_atten_lim(handle: *mut MxDfHandle, atten_lim_db: c_float) -> c_int {
    #[cfg(feature = "deepfilternet")]
    {
        let Some(h) = (unsafe { handle.as_mut() }) else {
            return MX_DF_ERR_NULL;
        };
        match imp::guarded(h, |h| h.set_atten_lim(atten_lim_db)) {
            Some(()) => MX_DF_OK,
            None => MX_DF_ERR_PANIC,
        }
    }
    #[cfg(not(feature = "deepfilternet"))]
    {
        let _ = (handle, atten_lim_db);
        MX_DF_ERR_UNAVAILABLE
    }
}

/// Frees a handle. Null is a no-op.
///
/// # Safety
/// `handle` must be null or a pointer from `mx_df_create` that is not used
/// again afterwards.
#[no_mangle]
pub unsafe extern "C" fn mx_df_destroy(handle: *mut MxDfHandle) {
    #[cfg(feature = "deepfilternet")]
    if !handle.is_null() {
        let boxed = unsafe { Box::from_raw(handle) };
        // A panic in a Drop impl must not cross the C boundary.
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || drop(boxed)));
    }
    #[cfg(not(feature = "deepfilternet"))]
    let _ = handle;
}

#[cfg(all(test, feature = "deepfilternet"))]
mod tests {
    use super::*;

    const SR: usize = 48_000;
    const HOP: usize = 480;

    /// Deterministic xorshift noise in [-1, 1).
    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> f32 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            ((self.0 >> 40) as f32 / (1u64 << 24) as f32) * 2.0 - 1.0
        }
    }

    /// Two-pole resonator (one formant).
    struct Formant {
        a1: f32,
        a2: f32,
        g: f32,
        y1: f32,
        y2: f32,
    }
    impl Formant {
        fn new() -> Self {
            Formant { a1: 0.0, a2: 0.0, g: 0.0, y1: 0.0, y2: 0.0 }
        }
        fn set(&mut self, freq: f32, bw: f32) {
            let r = (-std::f32::consts::PI * bw / SR as f32).exp();
            let th = 2.0 * std::f32::consts::PI * freq / SR as f32;
            self.a1 = 2.0 * r * th.cos();
            self.a2 = -r * r;
            self.g = 1.0 - r;
        }
        fn tick(&mut self, x: f32) -> f32 {
            let y = self.g * x + self.a1 * self.y1 + self.a2 * self.y2;
            self.y2 = self.y1;
            self.y1 = y;
            y
        }
    }

    /// Deterministic source-filter "speech": a jittered glottal pulse train
    /// (-6 dB/oct) with intonation through five vowel formants, five
    /// syllables per second, 1.5 s of talk then 1.0 s of pause (so the
    /// noise floor can be measured on its own), plus "fan" noise: hiss, a
    /// low rumble and 120 Hz hum. Real speech is not needed to show that the
    /// model separates the two; the quality agent's test uses recordings.
    /// Returns (clean, noise).
    fn speech_and_noise(secs: f32, seed: u64) -> (Vec<f32>, Vec<f32>) {
        const VOWELS: [[f32; 5]; 5] = [
            [730.0, 1090.0, 2440.0, 3400.0, 4500.0],
            [530.0, 1840.0, 2480.0, 3500.0, 4600.0],
            [270.0, 2290.0, 3010.0, 3600.0, 4700.0],
            [570.0, 840.0, 2410.0, 3300.0, 4400.0],
            [300.0, 870.0, 2240.0, 3350.0, 4450.0],
        ];
        const WEIGHTS: [f32; 5] = [1.0, 0.6, 0.3, 0.15, 0.1];
        let n = (secs * SR as f32) as usize;
        let mut clean = vec![0.0f32; n];
        let mut rng = Rng(seed ^ 0x9E37_79B9_7F4A_7C15);
        let mut formants: Vec<Formant> = (0..5).map(|_| Formant::new()).collect();
        let mut next_pulse = 0.0f32;
        let mut syllable = usize::MAX;
        let (mut lp1, mut lp2, mut prev) = (0.0f32, 0.0f32, 0.0f32);
        for (i, s) in clean.iter_mut().enumerate() {
            let t = i as f32 / SR as f32;
            let tt = t % 2.5;
            if tt >= 1.5 {
                continue;
            }
            let k = (tt / 0.2) as usize;
            if k != syllable {
                syllable = k;
                let v = VOWELS[(k * 3 + (t / 2.5) as usize) % 5];
                for (j, f) in formants.iter_mut().enumerate() {
                    f.set(v[j], 60.0 + 40.0 * j as f32);
                }
            }
            let env = (std::f32::consts::PI * ((tt % 0.2) / 0.2)).sin().powf(0.6);
            let f0 = 120.0 + 30.0 * (2.0 * std::f32::consts::PI * 0.6 * t).sin();
            let mut pulse = 0.0;
            if t >= next_pulse {
                next_pulse = t + (1.0 / f0) * (1.0 + 0.01 * rng.next());
                pulse = 1.0;
            }
            lp1 = 0.97 * lp1 + 0.03 * pulse;
            lp2 = 0.97 * lp2 + 0.03 * lp1;
            let src = (lp2 - prev) * 40.0 + 0.002 * rng.next() * env;
            prev = lp2;
            let y: f32 = formants.iter_mut().zip(WEIGHTS).map(|(f, w)| w * f.tick(src)).sum();
            *s = env * y;
        }
        // Talk-segment RMS ~0.08 (about -22 dBFS).
        let e = clean.iter().map(|&v| (v as f64).powi(2)).sum::<f64>() / (n as f64 * 0.6);
        let gain = 0.08 / e.sqrt() as f32;
        clean.iter_mut().for_each(|v| *v *= gain);

        let mut rng = Rng(seed);
        let mut lp = 0.0f32;
        let noise = (0..n)
            .map(|i| {
                let w = rng.next();
                lp = 0.9 * lp + 0.1 * w;
                let t = i as f32 / SR as f32;
                0.03 * w + 0.1 * lp + 0.01 * (2.0 * std::f32::consts::PI * 120.0 * t).sin()
            })
            .collect();
        (clean, noise)
    }

    /// Tests in this module run one at a time: the live-handle count below is
    /// process-wide, and a handle is ~35 MB, so running them serially also
    /// keeps the test binary's peak memory down.
    fn serial() -> std::sync::MutexGuard<'static, ()> {
        imp::LIVE_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn mix(clean: &[f32], noise: &[f32]) -> Vec<f32> {
        clean.iter().zip(noise).map(|(c, n)| c + n).collect()
    }

    fn energy(x: &[f32]) -> f64 {
        x.iter().map(|&v| (v as f64) * (v as f64)).sum::<f64>() / x.len().max(1) as f64
    }

    fn db(ratio: f64) -> f64 {
        10.0 * ratio.max(1e-30).log10()
    }

    fn run(h: *mut MxDfHandle, input: &[f32]) -> Vec<f32> {
        let mut out = Vec::with_capacity(input.len());
        let mut frame = vec![0.0f32; HOP];
        for chunk in input.chunks_exact(HOP) {
            frame.copy_from_slice(chunk);
            let rc = unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) };
            assert!(rc >= 0, "process failed: {rc}");
            out.extend_from_slice(&frame);
        }
        out
    }

    /// (noise-only reduction dB, speech energy change dB): pauses judged on
    /// the middle 0.8 s of every pause, talk on 0.2..1.3 s of every burst,
    /// output shifted back by the reported latency. The first burst is
    /// skipped (model warm-up).
    fn measure(input: &[f32], clean: &[f32], out: &[f32], lat: usize) -> (f64, f64) {
        let (mut ni, mut no, mut ti, mut to) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
        let mut k = 1;
        while ((k as f32 + 1.0) * 2.5 * SR as f32) as usize + lat <= out.len() {
            let base = (k as f32 * 2.5 * SR as f32) as usize;
            let pause = base + (1.6 * SR as f32) as usize..base + (2.4 * SR as f32) as usize;
            let talk = base + (0.2 * SR as f32) as usize..base + (1.3 * SR as f32) as usize;
            ni.extend_from_slice(&input[pause.clone()]);
            no.extend_from_slice(&out[pause.start + lat..pause.end + lat]);
            ti.extend_from_slice(&clean[talk.clone()]);
            to.extend_from_slice(&out[talk.start + lat..talk.end + lat]);
            k += 1;
        }
        (db(energy(&ni) / energy(&no)), db(energy(&to) / energy(&ti)))
    }

    #[test]
    fn frame_size_and_latency_are_sane() {
        let _serial = serial();
        assert_eq!(mx_df_available(), 1);
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        assert_eq!(unsafe { mx_df_frame_size(h) }, HOP as c_int);
        // DeepFilterNet3: 480 samples of STFT overlap + 2 frames lookahead.
        assert_eq!(unsafe { mx_df_latency_samples(h) }, 1440);
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn reported_latency_matches_the_measured_delay() {
        let _serial = serial();
        // The lag that best aligns the output with the clean speech is the
        // processing delay; it must equal what the handle reports, because
        // the C++ side reports that number as the added latency.
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let lat = unsafe { mx_df_latency_samples(h) } as usize;
        let (clean, noise) = speech_and_noise(5.0, 7);
        let out = run(h, &mix(&clean, &noise));
        let mut best = (0usize, f64::MIN);
        for lag in 0..=3 * HOP {
            let c: f64 = (SR..4 * SR).map(|i| clean[i] as f64 * out[i + lag] as f64).sum();
            if c > best.1 {
                best = (lag, c);
            }
        }
        assert!(
            (best.0 as isize - lat as isize).abs() <= 2,
            "measured delay {} samples, reported {lat}",
            best.0
        );
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn reduces_noise_far_more_than_speech() {
        let _serial = serial();
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let lat = unsafe { mx_df_latency_samples(h) } as usize;
        let (clean, noise) = speech_and_noise(10.0, 42);
        let input = mix(&clean, &noise);
        let out = run(h, &input);
        let (noise_reduction, speech_change) = measure(&input, &clean, &out, lat);
        // Measured (2026-10-06): noise floor driven to digital silence,
        // speech -9.5 dB on this synthetic voice (real speech is kept far
        // better, see the vault note). Pass-through would give 0 dB / 0 dB,
        // a plain gain would give equal numbers.
        assert!(noise_reduction > 20.0, "noise reduced by only {noise_reduction:.1} dB");
        assert!(speech_change > -15.0, "speech energy changed by {speech_change:.1} dB");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn attenuation_limit_is_honoured() {
        let _serial = serial();
        // With a 12 dB limit, libDF mixes the noisy signal back in so the
        // noise floor drops by at most ~12 dB.
        let h = mx_df_create(12.0);
        assert!(!h.is_null());
        let lat = unsafe { mx_df_latency_samples(h) } as usize;
        let (clean, noise) = speech_and_noise(10.0, 42);
        let input = mix(&clean, &noise);
        let out = run(h, &input);
        let (noise_reduction, _) = measure(&input, &clean, &out, lat);
        assert!(
            (10.0..14.0).contains(&noise_reduction),
            "12 dB limit gave {noise_reduction:.1} dB"
        );
        // And it can be changed on a live handle.
        assert_eq!(unsafe { mx_df_set_atten_lim(h, 100.0) }, MX_DF_OK);
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        let out = run(h, &input);
        let (noise_reduction, _) = measure(&input, &clean, &out, lat);
        assert!(noise_reduction > 20.0, "unlimited gave {noise_reduction:.1} dB");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn silence_full_scale_and_non_finite_input_never_produce_non_finite_output() {
        let _serial = serial();
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let mut frame = vec![0.0f32; HOP];
        for _ in 0..20 {
            frame.iter_mut().for_each(|x| *x = 0.0);
            assert!(unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) } >= 0);
            assert!(frame.iter().all(|x| x.is_finite()));
        }
        for i in 0..50 {
            frame
                .iter_mut()
                .enumerate()
                .for_each(|(j, x)| *x = if (i + j) % 2 == 0 { 1.0 } else { -1.0 });
            assert!(unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) } >= 0);
            assert!(frame.iter().all(|x| x.is_finite() && x.abs() < 8.0));
        }
        let specials = [f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 1e30, -1e30, f32::MIN_POSITIVE];
        for i in 0..50 {
            frame
                .iter_mut()
                .enumerate()
                .for_each(|(j, x)| *x = specials[(i + j) % specials.len()]);
            assert!(unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) } >= 0);
            assert!(frame.iter().all(|x| x.is_finite()));
        }
        // The running normalisation is not poisoned: ordinary input still
        // yields finite output with the noise floor removed.
        let lat = unsafe { mx_df_latency_samples(h) } as usize;
        let (clean, noise) = speech_and_noise(10.0, 3);
        let input = mix(&clean, &noise);
        let out = run(h, &input);
        assert!(out.iter().all(|x| x.is_finite()));
        let (noise_reduction, _) = measure(&input, &clean, &out, lat);
        assert!(noise_reduction > 20.0, "after NaN input: {noise_reduction:.1} dB");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn bad_arguments_are_rejected_without_touching_the_buffer() {
        let _serial = serial();
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let mut frame = vec![0.25f32; HOP + 1];
        for n in [0, 1, HOP - 1, HOP + 1] {
            assert_eq!(unsafe { mx_df_process(h, frame.as_mut_ptr(), n) }, MX_DF_ERR_SIZE);
            assert!(frame.iter().all(|&x| x == 0.25));
        }
        assert_eq!(unsafe { mx_df_process(h, std::ptr::null_mut(), HOP) }, MX_DF_ERR_NULL);
        assert_eq!(
            unsafe { mx_df_process(std::ptr::null_mut(), frame.as_mut_ptr(), HOP) },
            MX_DF_ERR_NULL
        );
        assert_eq!(unsafe { mx_df_frame_size(std::ptr::null()) }, 0);
        assert_eq!(unsafe { mx_df_latency_samples(std::ptr::null()) }, 0);
        assert_eq!(unsafe { mx_df_reset(std::ptr::null_mut()) }, MX_DF_ERR_NULL);
        assert_eq!(unsafe { mx_df_set_atten_lim(std::ptr::null_mut(), 10.0) }, MX_DF_ERR_NULL);
        unsafe { mx_df_destroy(std::ptr::null_mut()) };
        // Non-finite attenuation limits are accepted as "unlimited".
        let h2 = mx_df_create(f32::NAN);
        assert!(!h2.is_null());
        unsafe { mx_df_destroy(h2) };
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn reset_restores_the_initial_state() {
        let _serial = serial();
        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let (clean, noise) = speech_and_noise(3.0, 9);
        let input = mix(&clean, &noise);
        let first = run(h, &input);
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        let second = run(h, &input);
        assert_eq!(first, second, "reset must make processing reproducible");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn after_reset_the_output_is_exactly_a_fresh_handles() {
        let _serial = serial();
        // The unmute bug: a reset must leave nothing of the pre-reset audio
        // (look-ahead, recurrent state, STFT overlap) to be replayed. So the
        // output after a reset must be bit-identical to a handle that never
        // heard the earlier audio — including after back-to-back resets and
        // resets that land before the spare state was re-prepared.
        let (clean, noise) = speech_and_noise(3.0, 13);
        let before: Vec<f32> = (0..SR)
            .map(|i| 0.8 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / SR as f32).sin())
            .collect();
        let after = mix(&clean, &noise);

        let fresh = mx_df_create(100.0);
        assert!(!fresh.is_null());
        let expected = run(fresh, &after);
        unsafe { mx_df_destroy(fresh) };

        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        for round in 0..3 {
            let _ = run(h, &before);
            assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
            if round == 1 {
                assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK); // back to back
            }
            assert_eq!(run(h, &after), expected, "round {round}");
        }
        // A reset after a single frame (the spare was used by the previous
        // reset and re-prepared by that one frame).
        let mut frame = before[..HOP].to_vec();
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) };
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        assert_eq!(run(h, &after), expected, "reset after one frame");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn a_panic_mid_frame_after_a_reset_is_not_kept_as_a_fresh_state() {
        let _serial = serial();
        let (clean, noise) = speech_and_noise(3.0, 17);
        let tone: Vec<f32> = (0..SR)
            .map(|i| 0.8 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / SR as f32).sin())
            .collect();
        let after = mix(&clean, &noise);
        let fresh = mx_df_create(100.0);
        assert!(!fresh.is_null());
        let expected = run(fresh, &after);
        unsafe { mx_df_destroy(fresh) };

        let h = mx_df_create(100.0);
        assert!(!h.is_null());
        let _ = run(h, &tone);
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        // The first frame after the reset panics after tract has advanced
        // the state: the handle is poisoned and the frame left untouched.
        let mut frame = tone[..HOP].to_vec();
        imp::PANIC_AFTER_INFERENCE.store(true, std::sync::atomic::Ordering::SeqCst);
        assert_eq!(unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) }, MX_DF_ERR_PANIC);
        assert_eq!(frame, tone[..HOP].to_vec());
        assert_eq!(unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) }, MX_DF_ERR_PANIC);
        // The reset must really reset the half-updated state.
        assert_eq!(unsafe { mx_df_reset(h) }, MX_DF_OK);
        assert_eq!(run(h, &after), expected);
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn a_handle_moves_between_threads_like_the_element_moves_it() {
        let _serial = serial();
        // Created on one thread (the element's pool thread), processed on
        // another (the streaming thread), destroyed on a third.
        let h = std::thread::spawn(|| mx_df_create(100.0) as usize).join().unwrap() as *mut MxDfHandle;
        assert!(!h.is_null());
        let lat = unsafe { mx_df_latency_samples(h) } as usize;
        let (clean, noise) = speech_and_noise(10.0, 5);
        let input = mix(&clean, &noise);
        let addr = h as usize;
        let (input2, clean2) = (input.clone(), clean.clone());
        let (noise_reduction, _) = std::thread::spawn(move || {
            let out = run(addr as *mut MxDfHandle, &input2);
            measure(&input2, &clean2, &out, lat)
        })
        .join()
        .unwrap();
        assert!(noise_reduction > 20.0, "on the second thread: {noise_reduction:.1} dB");
        unsafe { mx_df_destroy(h) };
    }

    #[test]
    fn create_destroy_loop_releases_every_handle() {
        let _serial = serial();
        let before = imp::live_handles();
        let mut frame = vec![0.1f32; HOP];
        for _ in 0..20 {
            let h = mx_df_create(30.0);
            assert!(!h.is_null());
            assert_eq!(imp::live_handles(), before + 1);
            for _ in 0..10 {
                unsafe { mx_df_process(h, frame.as_mut_ptr(), HOP) };
            }
            unsafe { mx_df_reset(h) };
            unsafe { mx_df_destroy(h) };
            assert_eq!(imp::live_handles(), before, "a destroyed handle was not dropped");
        }
    }
}
