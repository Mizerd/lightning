# RNNoise (vendored)

Real-time noise suppression for the outgoing microphone (issue #20,
Settings -> Experimental). Everything Lightning needs is in this directory:
nothing is cloned, fetched or downloaded at configure, build or run time, and
there is no script here that could.

| | |
|---|---|
| Upstream | https://github.com/xiph/rnnoise (convenience copy of https://gitlab.xiph.org/xiph/rnnoise) |
| Release | **v0.2** (annotated tag object `c9137adac37fe21ede831f8a0aa31c17560c01e7`) |
| Commit | `904a876dce1f9ab8860c0a5000ed151f9f6eef58` |
| Commit date | 2024-04-14 21:07:37 -0400 |
| Latest release tag at vendoring (2026-10-06) | v0.2 (tags: v0.1, v0.1.1, v0.2). `main` has moved on (HEAD `70f1d256acd4b34a572f999a05c87bf00b67730d`, 2025-02-22, with a newer model); it is not a release and was NOT taken |
| Code licence | BSD-3-Clause, see `COPYING` (Xiph.Org, Mozilla, Amazon, Jean-Marc Valin, Mark Borgerding) |
| Authors | `AUTHORS` |
| Vendored on | 2026-10-06 |

## Model (weights)

RNNoise v0.2 does not keep its weights in the repository. `autogen.sh` runs
`download_model.sh`, which fetches the file named in `model_version` from
`https://media.xiph.org/rnnoise/models/`. For the v0.2 commit:

| | |
|---|---|
| `model_version` at the tag | `0b50c45` |
| File | `rnnoise_data-0b50c45.tar.gz` (21 MB; the same bytes are also published as `rnnoise_data-4ac81c5c...tar.gz`, and `media.xiph.org/rnnoise/SHA256SUMS.txt` lists both names under one hash) |
| SHA-256 of the tarball | `4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43` |
| Tarball dated | 2024-04-15 (`media.xiph.org/rnnoise/models/`) |
| Taken from it | `src/rnnoise_data.c` (29,271,713 bytes) and `src/rnnoise_data.h` (926 bytes), generated C, byte-for-byte |
| NOT taken | `rnnoise_data_little.c/.h` (the half-size alternative model, 15 MB), `models/rnnoise8Za_25.pth` (PyTorch checkpoint, 11 MB, training artefact) |
| Architecture | conv1 (65->128) + conv2 (128->384) + 3 x GRU(384) + dense(32) + VAD head; float and int8 tables, 48 kHz full band, 480-sample frames |

The tarball was downloaded once by hand when vendoring and checked against the
hash above; it is not kept in the tree. To use the smaller model instead,
replace `src/rnnoise_data.c/.h` with the `_little` pair (upstream README,
"Loadable Models").

### Licence of the weights: finding

**Not stated by upstream.** Evidence gathered 2026-10-06:

- `COPYING` (BSD-3-Clause) covers the code; the weights are not in the source
  repository and neither `README`, `COPYING`, `datasets.txt` nor the model
  tarball (5 members: two `.c`, two `.h`, one `.pth`) carries any licence text.
- Upstream issue https://github.com/xiph/rnnoise/issues/284 ("Please state the
  license of the distributed model weights"), opened 2026-09-04, is **still
  open**, with a follow-up comment (2026-09-24) asking specifically about
  `0b50c45`. No maintainer answer.
- README says the shipped models are "trained using only the publicly available
  datasets listed below": `datasets.txt` = OpenSLR corpora (mostly CC BY-SA 4.0
  or CC BY 4.0, Google / Hi-Fi TTS), plus the contributed noise data, which
  `media.xiph.org/rnnoise/README.txt` releases under CC0. Whether a trained
  model inherits ShareAlike from its training data is a legal question nobody
  upstream has addressed; the issue above raises it too.
- Practice, as reported in that issue (those packagers' files were not re-read
  here): Debian and Gentoo treat `src/rnnoise_data.{c,h}` as BSD-3-Clause under
  the project `COPYING`; Fedora tags the model `CC0-1.0`. Upstream's own build
  fetches the weights for every build, i.e. they are an integral part of the
  distributed library.

**Verdict: de-facto redistributable together with the library, NOT explicitly
licensed.** The weights carry no terms of their own; the upstream build process
and every packager cited treat them as part of the BSD-3-Clause distribution,
and nothing in the evidence is more restrictive than that (training data: CC0,
CC BY, CC BY-SA, none non-commercial). This is a residual risk, not a
clearance. The options, for Rokas to decide:

1. Accept (current state): vendor v0.2 + `0b50c45`, keep this finding and issue
   #284 linked, re-check the issue at each update.
2. Fall back to weights that are unambiguously covered by the repository
   `COPYING`: **v0.1.1's `src/rnn_data.c/.h`**, which are committed in the
   source repository itself and so ship under that tree's BSD `COPYING`. Not
   vendored: it is the 2018 network (GRU 24/48/96, different code and weights
   format, clearly lower quality) and would replace the whole of `src/` here,
   not just the weights.
3. Ask upstream (comment on #284) and wait.

## Files taken (sha256; upstream v0.2 plus the model tarball)

Only what `rnnoise_create` / `rnnoise_process_frame` need. The C library built
from them is `lightning_rnnoise` in `cmake/ThirdPartyRnnoise.cmake` (the three
`x86/` SIMD files only on x86, with runtime CPU dispatch).

| File | sha256 |
|---|---|
| `AUTHORS` | `348b0956050157b718a11e1464633619b709b19b45172f336cef2205aa0cec63` |
| `COPYING` | `45d37ca1cdb278c088e1aa85e0e65ca3a534ed86a28dcc96ca16810248a61d35` |
| `include/rnnoise.h` | `424d75212a781147f40666f511309d1c054d946eca44af2de4cd1b3346f09276` |
| `src/arch.h` | `1686a3ca5817bb7d99049a1801c6ab3a7e91a5d0a6804b1c1cafb36e5eeb62f8` |
| `src/celt_lpc.c` | `58eca412376dfd954f4940709df521cf1d2cbee1b7ca2b166bd0bffbda2c5b8d` |
| `src/celt_lpc.h` | `2190d9bd519c4528770a28fb0e8b599eb195782fb5dcd91378eb92e3ca7d5148` |
| `src/common.h` | `fa6069db8f7fd18d79d5bea55cded6f9019570b2886908323270d07225c7c0ce` |
| `src/cpu_support.h` | `acb2591c1dfb97783a0a68fea76279dcb1155f0b7e6b95f51448a590f301c4b2` |
| `src/denoise.c` | `a7759a51af88381d135ed90f6e73919a6f9540845e437749758f6f0dc608e4c4` |
| `src/denoise.h` | `3ea8149b47b51b76a1eef308cc38168731693b74ed3f64330b55583f92dcc071` |
| `src/kiss_fft.c` | `2a9f9ee0d961ec6516864477640d78aedbf1c3ecbe1680727b4b93d486c09679` |
| `src/_kiss_fft_guts.h` | `0214dfe2aabb3b81d88fe814b8253fc2ec6da12bbbc98e08c1011a5bd60e3b29` |
| `src/kiss_fft.h` | `434613feace5466e73fb6593f3abeaca6c7da3ec5af3f4a475f54a6962324526` |
| `src/nnet_arch.h` | `76068e63007b072b0c20410cee89c0ab5158438d5201b466850354c0aee33688` |
| `src/nnet.c` | `3a2c1dc09ad7e9dfe3cf32cfb94694935dc462488da8f513d1f308db39f7c087` |
| `src/nnet_default.c` | `1402eb611b7fcc1fe7fa18639f0395e0af47a783ef87450318411483516e9011` |
| `src/nnet.h` | `3552cf2fcfc4db28ac683bcc8a5dc7b72fad7140b3df43d0f8d2d5afe3abe4dd` |
| `src/opus_types.h` | `e2c1075e26567b8d57706d2f0042e36d5d440721fc3fd27a6fce9f0fc8a31557` |
| `src/parse_lpcnet_weights.c` | `d523113abe3c387b5481d8aa75d61b22f5e6cbcf54110f68cf901204d443de12` |
| `src/pitch.c` | `cfe6f5ffb4b5f5967ce2e64f22cc978c1f8196eecdb2fa532d4dd1ff56f6ca58` |
| `src/pitch.h` | `020be5b3fbc5289cd76930b0534efaa25dbda0cecd8643ee345cfd9d875a3818` |
| `src/rnn.c` | `1a72e1d568c7930e89bdfd072291930775ac842bf5373e26adb6350c3fbb0454` |
| `src/rnn.h` | `2fe9f33534fbfa35a7d5c81323f1ec4e9ec06e28328815c7c88880e83f994566` |
| `src/rnnoise_data.c` (model) | `522b6a64fded05bf85e58c06206eafe57ce7d94f3af58c725b17628b481d7890` |
| `src/rnnoise_data.h` (model) | `09ff880bddd0fc74a2ae0e5ec6c8d65714031b08d0c3f672493acd9e189c5855` |
| `src/rnnoise_tables.c` | `f81353479c6f8912755a4cf5838590c36ba2c523a4a438e4f949743fb4945623` |
| `src/vec_avx.h` | `8fb02f2f54822e8bf0189c2ccd7e9071d7654d4c0630e2a6a6b15a3accfa7ac2` |
| `src/vec.h` | `fbdf49c54d60cfaa81607caef2f2d83bebde51de2a53aee2607b114350425a8e` |
| `src/vec_neon.h` | `225dbe25b0df10bcb1ee01eabfcc7cab2e27d1b30c0ff2c79bd3a517c918523e` |
| `src/x86/dnn_x86.h` | `8b17955037daf4f9def4975d4b24c343b822abe62f6ad09a9d204ad00eb3aa33` |
| `src/x86/nnet_avx2.c` | `c49cec6a38ee554660b4dd6958eb12450eac7bb6e28ce07d5a814a4a9d0fbbf2` |
| `src/x86/nnet_sse4_1.c` | `e32fc91e403fb0c105ae9bcbbcf0339ce0b49e01110d8485520321f4a605f9ba` |
| `src/x86/x86_arch_macros.h` | `b6118942870b62ff78731edc3025e55230c3fadaf28ec2f4024d786210631505` |
| `src/x86/x86cpu.c` | `214502bc62afa326b1f2f97693611623df9eea90a21abd292893626cd5c8080d` |
| `src/x86/x86cpu.h` | `589e0af48849594b7a9e93af74b68417ad14f28f702d665633fd738a8a430734` |
| `src/x86/x86_dnn_map.c` | `2c29a555c599b146f49798b8b8e35313c2381ffae7b33637168f194bf04c4436` |

Left out on purpose: `dump_features.c`, `dump_rnnoise_tables.c`,
`write_weights.c`, `rnn_train.py`, `compile.sh` (training and tooling),
`torch/`, `training/`, `examples/`, `doc/`, `scripts/`, the autotools files,
`download_model.sh`, `model_version`, `datasets.txt`, `README`.

## Local modifications

**None.** Every file above is byte-identical to upstream v0.2 (and, for the two
`rnnoise_data` files, to the model tarball). Lightning-specific build settings
live in `cmake/ThirdPartyRnnoise.cmake`, not in these files:
`-DRNN_ENABLE_X86_RTCD -DCPU_INFO_BY_ASM` (x86, GCC/Clang/MinGW), per-file
`-msse4.1` and `-mavx -mfma -mavx2` for the two SIMD kernels, `-O2` forced in
Debug, warnings off for this target.

## Behaviour measured here

Standalone `cc` build of this tree (not a Lightning build). Synthetic voice
(harmonic stack, formant envelope, syllable gating, -20 dBFS) plus seeded
noise, 10 s at 48 kHz, gaps and syllables measured after a 2 s warm-up:

| Noise | Gap suppression | Voiced-segment change |
|---|---|---|
| white -30 dBFS | 36.8 dB | -0.8 dB |
| white -40 dBFS | 40.0 dB | -0.3 dB |
| pink -30 dBFS | 32.6 dB | -0.8 dB |
| pink -40 dBFS | 34.5 dB | -0.4 dB |

Algorithmic latency 960 samples (20 ms: the analysis overlap-add plus one frame
of look-ahead, `delayed_X` in `denoise.c`), found by cross-correlation. Cost
1.8-3.6% of one core for real-time audio (x86-64, AVX2 dispatch).
`tests/RnnoiseSuppressorTest.cpp` asserts these with margin.

## How to update

1. Pick a RELEASE tag (`git ls-remote --tags https://github.com/xiph/rnnoise`).
2. In a scratch directory, clone it, check out the tag, read `model_version`,
   fetch `https://media.xiph.org/rnnoise/models/rnnoise_data-<model_version>.tar.gz`
   by hand and verify its sha256 against
   `https://media.xiph.org/rnnoise/SHA256SUMS.txt`.
3. Copy the files listed above (plus any new source in upstream's
   `RNNOISE_SOURCES` in `Makefile.am`) and the two generated data files; update
   `cmake/ThirdPartyRnnoise.cmake` if the source list or SIMD flags changed.
4. Re-determine the model licence (check issue #284), re-run the standalone
   measurement and `rnnoise-suppressor-test`, update this file and
   `docs/third-party-notices.md`.
5. Never add a download step to CMake, scripts or packaging.
