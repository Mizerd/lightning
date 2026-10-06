# DeepFilterNet (libDF) — vendored for Lightning

What Settings → Experimental → DeepFilterNet runs on the outgoing call
microphone (GitHub issue #20). Only the native Rust streaming-inference path is
vendored; Python, PyTorch, training, datasets, the LADSPA plugin, the demo and
notebooks are not, and nothing of DeepFilterNet is fetched at configure, build
or run time.

## Upstream

| | |
|---|---|
| Project | DeepFilterNet — "Noise supression using deep filtering", Hendrik Schröter |
| URL | https://github.com/Rikorose/DeepFilterNet |
| Vendored commit | `d375b2d8309e0935d165700c91da9de862a99c31` (branch `main`, committed 2024-10-17; the newest commit upstream has) |
| Nearest release | tag `v0.5.6` → `978576aa8400552a4ce9730838c635aa30db5e61` (2023-08-31). The vendored commit is 89 commits after it. |
| Obtained | 2026-10-06, as the GitHub source archive of that commit (`codeload.github.com/Rikorose/DeepFilterNet/tar.gz/d375b2d8…`, sha256 `49471f3633a24c097d82f3b0d2dbd83a0c1bac3e2f6f6c9a675ef0020ebe5c51`) |
| Crate | `deep_filter` 0.5.7-pre (library name `df`), directory `libDF/` |

Why `main` and not `v0.5.6`: `v0.5.6` pins tract 0.19 (2023); `main` moved libDF
to tract 0.21 and fixed the silence detector so a single quiet frame no longer
zeroes the output ("libdf: Only skip processing if at least 5 frames were close
to zero", `9524a0fa`). The model files are byte-identical in the tag and in the
vendored commit.

## Files taken

| Path here | Upstream path | sha256 here | State |
|---|---|---|---|
| `LICENSE` | `libDF/LICENSE` (= root `LICENSE`) | `f7ef673bf046d823dcd775bdd0768432bd8855f81d0e5e1290a0a48c42e2dca3` | unmodified |
| `LICENSE-MIT` | `libDF/LICENSE-MIT` (= root) | `24e6bb09c928af8d8e56268082f87413247ce36b39dd5d33add2f9893968065e` | unmodified |
| `LICENSE-APACHE` | `libDF/LICENSE-APACHE` (= root) | `1eaee808c5fb6b4e895ba30425285a5cdc5dd25bba2cd230f264c2200c331aec` | unmodified |
| `libDF/src/lib.rs` | `libDF/src/lib.rs` (upstream sha256 `716b6274c60344b25b7824a55d06d0fac0e58603faedbb86b3f3bd1ea6a256d3`) | `30e237ce3762383c1f35d319a9f8bbe09ae0a16aa842e6bb31ac74edbaddc7c9` | modified, see below |
| `libDF/src/tract.rs` | `libDF/src/tract.rs` (upstream sha256 `0f080a766955cf037a1642ebfe6de645b9161985379d5c7cc790a21a04f1e580`) | `01ebe5c275321ce0c10b24c30049bb72cba674b82f988de15a1bb61b5510bf62` | modified, see below |
| `libDF/Cargo.toml` | `libDF/Cargo.toml` (upstream sha256 `172f3526cd144dc7fb351cf7a24d3de89d07413e374c7a25b3e8812bb8503eb7`) | `e6dc35cbcf07c0928fbafd02fbb6f2bda338ce30f12887291a4209cf08f66f17` | REWRITTEN, see below |
| `models/DeepFilterNet3_onnx.tar.gz` | `models/DeepFilterNet3_onnx.tar.gz` | `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616` | unmodified (7,983,136 bytes) |
| `lightning-changes.diff` | — | — | Lightning's: `diff -ru` of upstream `lib.rs`/`tract.rs` against the files here |

Not taken: every other file of the repository, including `libDF/src/{transforms,
dataset,dataloader,augmentations,hdf5_key_cache,util,wav_utils,capi,logging,wasm}.rs`,
`libDF/src/bin/`, `pyDF/`, `pyDF-data/`, `DeepFilterNet/` (Python), `ladspa/`,
`demo/`, `assets/`, `scripts/`, and the other models (`DeepFilterNet3_ll_onnx`,
`DeepFilterNet2*`, `DeepFilterNet.zip`, `DeepFilterNet3.zip`).

## The model

| | |
|---|---|
| Name | DeepFilterNet3, ONNX export as shipped by upstream (`DeepFilterNet3_onnx.tar.gz`; config `[train] model = deepfilternet3`, `max_epochs = 120`) |
| Archive sha256 | `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616` |
| Members | `tmp/export/enc.onnx` (1,954,042 B, sha256 `7c5399d3da8a50ebef1c1a0ae421b33376aa5e45d0e92df16da7e83c9c131916`), `tmp/export/erb_dec.onnx` (3,292,397 B, `ab669a1d10afe20911728b33053a452071042317a90581092b325da7b2f9d895`), `tmp/export/df_dec.onnx` (3,340,803 B, `23114ce3b0f6464b763ee62f7bb8aab6b2a129a21eabd5bcfe59413db05f278a`), `tmp/export/config.ini` (2,067 B, `415eb925d44990d938fb739f514aa3662c1ec0ea836cff044fa1291b82cb4290`) |
| Signal | 48 kHz mono, FFT 960, hop 480 (10 ms), 32 ERB bands, deep filter on the lowest 96 bins, order 5 |
| Latency | 1440 samples = 30 ms: 480 of STFT overlap plus `conv_lookahead = df_lookahead = 2` frames. Measured by cross-correlation in `rust/src/denoise.rs`'s tests: exactly 1440. |
| How it ships | Embedded in the Lightning binary with `include_bytes!` (`rust/src/denoise.rs`), parsed from memory at `mx_df_create`. No file is read and nothing is downloaded at runtime. |

Why this model: it is the smallest current upstream model and the one upstream
uses by default (`default-model`). `DeepFilterNet3_ll` (low latency, no
look-ahead, 10 ms) was considered and not taken: 36 MB instead of 8 MB and a
network twice as wide (hidden 512, three DF layers), for 20 ms less delay.
`DeepFilterNet2*` models are refused by libDF itself ("deprecated").

## Licences

**Code: MIT OR Apache-2.0** (at the user's option), verified: the repository
root and `libDF/` carry identical `LICENSE`, `LICENSE-MIT` and `LICENSE-APACHE`
files; the README says "All code in this repository is dual-licensed under
either MIT … or Apache-2.0"; `libDF/Cargo.toml` declares `MIT/Apache-2.0`;
copyright "2021 Hendrik Schröter". Lightning (GPL-3.0-or-later) may include
either.

**Model weights: NOT explicitly licensed by upstream. Read this before
shipping.**

- The README's grant names "all code in this repository". The `models/`
  directory carries no licence file, and GitHub reports the repository's
  licence as `NOASSERTION`.
- Upstream has been asked and has not answered: issues #697 (2026-07-15),
  #700 (2026-08-12), #709 (2026-09-01) and #712 (2026-09-21) all ask whether
  the MIT/Apache-2.0 grant covers the released checkpoints; all four are open
  with no maintainer reply (one reporter wrote that two e-mails to the author
  went unanswered). Upstream's last commit is 2024-10-17.
- The author's own conduct supports the permissive reading: he distributes
  these weights embedded in his own release binaries (`deep-filter-0.5.6-*`,
  `libdeep_filter_ladspa-0.5.6-*`, assets of the v0.5.6 GitHub release), built
  from crates whose manifests declare `MIT/Apache-2.0`, and libDF's
  `default-model` feature compiles this very tarball into the MIT/Apache crate.
  Third parties republish the weights as MIT or Apache-2.0 (for example
  `Intel/deepfilternet-openvino` on Hugging Face, `license: mit`); those are
  their readings, not a grant.
- No more clearly licensed artifact exists: every published copy derives from
  the same checkpoint, and no crates.io release of `deep_filter` contains a
  model (crates.io stops at 0.2.5, 2022).

Lightning's position, pending the maintainer's decision: the weights are
redistributed under the repository's MIT OR Apache-2.0 terms, with the
copyright notice and both licence texts shipped beside them, on the strength
of the author's own MIT/Apache-labelled distribution of the same bytes. This is
a reading, not an explicit grant, and it is recorded as such in
`docs/third-party-notices.md`. If it is ever rejected, the remedy is to build
with `-DLIGHTNING_ENABLE_DEEPFILTERNET=OFF` (cargo feature `deepfilternet`
off): the mode then reports itself unavailable and nothing of the model is in
the binary. No download mechanism exists or may be added in its place.

## Local modifications

All of them keep upstream's behaviour; the exact text is in
`lightning-changes.diff`.

`libDF/Cargo.toml` is a local rewrite: only the `tract` feature and the
dependencies it needs are kept (`rustfft`, `realfft`, `itertools`,
`num-complex`, `log`, `rust-ini`, `anyhow`, `tract-core`, `tract-onnx`,
`tract-pulse`, `flate2`, `tar`). Dropped: the binaries, `dataset`,
`transforms`, `capi`, `wasm`, `wav-utils`, `vorbis`, `flac`, `use-jemalloc`,
`default-model-ll`, the hdf5 **git** dependency, `ndarray` (taken from tract,
below), `tract-hir` (reached through `tract-onnx`), and the `serde`/`backtrace`
features of `num-complex`/`anyhow`. `publish = false`, licence spelled as an
SPDX expression.

`libDF/src/lib.rs`:
1. `mod` declarations of the modules not vendored are removed, as is upstream's
   unit test (it needs the `rand` dev-dependency).
2. `pub use tract_core::ndarray;` so callers build `DfTract::process`
   arguments with tract's own ndarray version.
3. `#![allow(mismatched_lifetime_syntaxes)]` — a newer rustc lint on unchanged
   upstream signatures.

`libDF/src/tract.rs`:
1. `ndarray` comes from `tract_core::ndarray` (tract 0.21.7 and later use
   ndarray 0.16; upstream still asks for 0.15, which would not type-check
   against the tract Lightning locks).
2. `m.symbol_table.sym("S")` → `m.symbols.sym("S")` (three places): the field
   was renamed in tract 0.21.x.
3. `into_shape` → `into_shape_with_order` (deprecated in ndarray 0.16).
4. Per-frame logging removed from `DfTract::process`/`process_raw` (a
   `log::trace!` and a per-frame "Possible clipping" `log::warn!`, with the
   peak tracking that only fed it): that code runs on the call's real-time
   audio thread, and Lightning can route `log` records into its tracing
   subscriber. Initialisation-time logging is untouched.
5. The `default-model-ll` branch of `DfParams::default()` is removed (that
   model is not vendored), and the `capi` condition on `DfTract::default()`
   is dropped (that feature is not vendored). Lightning enables neither
   `default-model` nor these defaults; it embeds the model itself.

Known upstream behaviour kept as is: `DfTract::init()` does not clear
`rolling_spec_buf_x`, so calling it twice would grow that buffer. Lightning
never calls it after construction — `mx_df_reset` restores a pristine clone
instead.

## Dependencies this brings into rust/Cargo.lock

tract 0.21.14 (core, data, hir, linalg, nnef, onnx, onnx-opl, pulse,
pulse-opl) — 0.21.14 is the newest 0.21 release compatible with Lightning's
lock: tract-linalg 0.21.15+ requires `time < 0.3.42` at build time while
matrix-sdk-crypto 0.18 requires `time >= 0.3.47`. Every tract 0.21 release
pins `libm = "=0.2.11"`, so the lock's existing `libm` moves from 0.2.16 to
0.2.11 (only reachable through `num-traits`, which uses `std` float functions
when built with `std`, as it is here). The full list of new crates, their
versions and licences is in `docs/third-party-notices.md`. All of them declare
a `rust-version` at or below the CI toolchain (1.93) or none at all.

## How to update

1. Download the new upstream commit as a source archive (never a submodule or
   a build-time fetch) and record its URL, commit, date and sha256 here.
2. Copy `libDF/src/lib.rs`, `libDF/src/tract.rs`, the three licence files and
   the chosen `models/*.tar.gz`; re-apply `lightning-changes.diff` (or the
   equivalent for the new tract), and refresh `lightning-changes.diff` and
   every sha256 in this file.
3. Reconcile `libDF/Cargo.toml` with upstream's dependency list; keep
   `default-features = false` and the `tract`-only feature set.
4. In `rust/`: `cargo update -p deep_filter` (network), check every new crate's
   `rust-version` against the CI toolchain, then `scripts/update-cargo-sources.sh`
   so the Flathub `cargo-sources.json` matches the lock.
5. Re-check the model licence question upstream (issues #697/#700/#709/#712)
   and update the section above and `docs/third-party-notices.md`.
6. Run `rust/src/denoise.rs`'s tests and `deepfilter-suppressor-test`; the
   latency assertion (1440) and frame size (480) change only with the model.
