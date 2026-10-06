/*
 * matrix_denoise.h — C ABI of rust/src/denoise.rs: DeepFilterNet microphone
 * noise suppression (issue #20), backed by the vendored libDF in
 * third_party/deepfilternet/ with its model embedded in the binary.
 *
 * Threading: a handle is used by one thread at a time. mx_df_create parses
 * and optimises the model (hundreds of milliseconds): never call it on the
 * GStreamer streaming thread or the GUI thread. mx_df_process is the per-frame
 * call for the streaming thread; it takes no locks, does no I/O and does not
 * log. mx_df_reset swaps in a fresh model state kept ready for it: no
 * allocation, no deallocation, no lock, so it may run on the streaming thread
 * (the next mx_df_process re-prepares the spare state).
 *
 * Every entry point exists in every Rust build. Without the cargo feature
 * `deepfilternet`, mx_df_available() returns 0 and mx_df_create() returns
 * NULL.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MxDfHandle MxDfHandle;

/* Return codes of mx_df_process / mx_df_reset / mx_df_set_atten_lim. */
enum {
    MX_DF_OK = 0,              /* frame processed in place */
    MX_DF_RECOVERED = 1,       /* model produced a non-finite sample: frame
                                  zeroed, state reset, handle still usable */
    MX_DF_ERR_NULL = -1,       /* null handle or buffer; buffer untouched */
    MX_DF_ERR_SIZE = -2,       /* n != frame size; buffer untouched */
    MX_DF_ERR_PROCESS = -3,    /* inference error; buffer untouched */
    MX_DF_ERR_PANIC = -4,      /* panic caught; handle poisoned until
                                  mx_df_reset; buffer untouched */
    MX_DF_ERR_UNAVAILABLE = -5 /* built without DeepFilterNet */
};

/* 1 when this build carries DeepFilterNet, 0 otherwise. */
int mx_df_available(void);

/* atten_lim_db: maximum noise attenuation in dB (>= 100 means unlimited).
 * Returns NULL on any failure. Free with mx_df_destroy. */
MxDfHandle *mx_df_create(float atten_lim_db);

/* Samples per mx_df_process call (480 = 10 ms at 48 kHz); 0 for NULL. */
int mx_df_frame_size(const MxDfHandle *handle);

/* Algorithmic delay added, in samples at 48 kHz (1440 = 30 ms for the
 * shipped DeepFilterNet3 model); 0 for NULL. */
int mx_df_latency_samples(const MxDfHandle *handle);

/* Processes exactly one frame of n mono float samples at 48 kHz, nominally
 * in [-1, 1], in place. Non-finite samples are treated as 0 and the rest are
 * clamped to [-1, 1] before inference. On a negative return the buffer is
 * left as it was, so ignoring the code degrades to pass-through. */
int mx_df_process(MxDfHandle *handle, float *in_out, size_t n);

/* Drops all audio state (model lookahead, recurrent state, STFT buffers) and
 * clears a poisoned handle. Real-time safe: allocation-free. */
int mx_df_reset(MxDfHandle *handle);

/* Changes the attenuation limit of a live handle. */
int mx_df_set_atten_lim(MxDfHandle *handle, float atten_lim_db);

/* Frees a handle; NULL is a no-op. */
void mx_df_destroy(MxDfHandle *handle);

#ifdef __cplusplus
}
#endif
