# Build performance

Lightning's full build used to take roughly 30–50 minutes on a 10-core /
20-thread machine, with every core busy the whole time. This document records
where that time actually went, what was changed, and how to measure it again.

## How to look at your own build

`tools/analyze-build.py` reads Ninja's own `.ninja_log` — no instrumentation, no
rebuild, nothing modified:

```sh
tools/analyze-build.py build-rust
tools/analyze-build.py build-rust --top 40
```

It reports the slowest translation units aggregated across every target that
compiles them, the most expensive targets, the slowest link steps, and how much
time is spent compiling the same source more than once. That last number is what
found the problem below.

One caveat the tool also prints: Ninja's log is cumulative and never forgets an
output, so after the build graph changes it keeps reporting work that is no
longer done. Use it to find where time goes, but measure an improvement by
timing a real build.

## What was actually slow

The dominant cost was not a slow file, a slow compiler, or a slow disk. It was
the same code being compiled dozens of times.

Every Qt-executable test target listed `${APP_LIB_SOURCES}` — the whole
application, about 150 translation units — directly in its own
`qt_add_executable`, and then registered its own private copy of the
`MatrixClient` QML module. There were 32 such targets. Measured from an
untouched `.ninja_log`:

```
total          682.0 CPU-min
compiling      603.6 CPU-min   (5000 objects)
82 sources are compiled by more than one target
Most duplicated: src/matrix/MockMatrixClient.cpp (43 copies)
```

Per-target, the application itself was a rounding error next to its tests:

```
CPU-min   objs  target
   18.4    152  matrix-client          <- the actual product
   18.6    150  timeline-pane-qml-test
   17.9    150  trust-card-test
   17.9    150  invite-people-dialog-qml-test
   ... 28 more targets at ~18 CPU-min each
```

So roughly **85% of a full build was 32 test executables recompiling the same
application sources**, and at `-j18` that is exactly the observed 30–50 minutes
of fully-saturated CPU. The CPU was not the bottleneck; the build graph was.

A second, smaller cost sat in CMake configuration: 121 seconds, of which 119.6
were 51 serial `execute_process` calls, measured with

```sh
cmake -S . -B <throwaway> --profiling-format=google-trace --profiling-output=cfg.trace
```

Every one was `qmlimportscanner`, which Qt runs once per QML-linking executable
at configure time to decide which static QML plugins to link.

## What changed

**1. The application is compiled once for the tests.**
`lightning-app-testlib` is a static Qt QML-module library built from
`APP_LIB_SOURCES`, and the 32 test executables link it instead of rebuilding it.
`_lightning_configure_qml_module()` takes the visibility as a parameter so the
library propagates exactly the include paths, defines and libraries the tests
previously got directly — same sources, same flags, same module URI.

This lives entirely inside `if(BUILD_TESTING)`. Every packaging build configures
with `-DBUILD_TESTING=OFF`, so no release artifact is affected in any way, and
`matrix-client` itself is built exactly as before.

**2. Test executables skip the configure-time QML import scan.**
`QT_QML_MODULE_NO_IMPORT_SCAN` is set on them. The scan exists to link *static*
QML plugins; against a shared Qt build those are resolved at runtime, which is
why Qt itself skips the scan for its own test executables in shared builds. Our
tests are never deployed and link the `MatrixClient` module explicitly.

**3. Cargo is told about all of its sources.**
The `add_custom_command` for the Rust static library listed 5 of the 11 files in
`rust/src`. Editing `oauth.rs`, `search.rs`, `uia.rs`, `discover.rs`,
`ignore.rs` or `gifs.rs` therefore did **not** re-run cargo, and the build
silently linked a stale `libmatrix_client_rust.a`. It now globs the crate's
sources with `CONFIGURE_DEPENDS`. This is a correctness fix that happens to live
in the build system, not a speed optimisation.

**4. ccache is used automatically for source builds.**
When `ccache` is on `PATH` and no compiler launcher was set explicitly, source
builds use it. It is confined to builds whose `LIGHTNING_ARTIFACT_KIND` is
`source`, so packaging is untouched, and an explicit
`-DCMAKE_CXX_COMPILER_LAUNCHER=...` always wins. Absent ccache is not an error.
Disable with `-DLIGHTNING_DISABLE_CCACHE=ON`.

## Measurements

Machine: Intel Core i9-10900K (10 cores / 20 threads), 62 GB RAM, GCC 15.2.0,
CMake 4.1.6, Ninja 1.13.2, Qt 6.11.1, `CMAKE_BUILD_TYPE` unset (Debug), `-j18`.

| Scenario | Before | After |
|---|---:|---:|
| No-op build | — | **1.5 s** |
| Add one Qt/QML test target | ~18.6 CPU-min, 151 objects | **20.7 s, 3 objects** |
| CMake configure | 121 s | **58 s** |
| Full build, all targets | ~682–712 CPU-min / 30–50 min wall | **59 CPU-min / 4 m 54 s wall** |

The "before" full-build figures come from the `.ninja_log` files as they stood
before any change, so they describe real builds rather than a reconstruction.
The "after" full build was a cold ccache, so it measures compiler work, not
cache hits.

The number that matters day to day is the second row: touching one test used to
cost about eighteen CPU-minutes and now costs about twenty seconds.

## Recommended commands

```sh
# configure (once)
nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake -S . -B build -G Ninja

# build everything, tests included
nix develop -c cmake --build build-rust -j18

# build just the application
nix develop -c cmake --build build-rust -j18 --target matrix-client

# build and run one test
nix develop -c cmake --build build-rust -j18 --target update-manifest-test
nix develop -c ctest --test-dir build-rust -j18 -R update-manifest --output-on-failure

# everything
nix develop -c ctest --test-dir build-rust -j18 --output-on-failure
```

Cap parallelism at 18, not 20: two threads are deliberately left for the desktop.

Tests are still part of the default target, so `cmake --build` builds and CTest
runs exactly what it always did. Nothing was removed from the test suite and no
CI step needs changing — the tests simply share one compiled copy of the
application now.

## What is still expensive, and why

- **`matrix-client` itself: ~18 CPU-min** for ~150 translation units, roughly
  12 seconds each. That is genuine work — Qt headers are large and these are
  real, header-heavy C++20 translation units. Reducing it further means
  attacking include cost (precompiled headers, or trimming what the widest
  headers pull in), which is a much larger and riskier change for a smaller
  return than the one above.
- **CMake configure: ~58 s**, still dominated by `qmlimportscanner` for the
  remaining QML-linking executables. Qt runs these serially; there is no
  supported way to parallelise them, and the ones that remain are the targets
  that genuinely need the scan.
- **The Rust static library: ~80 s** when it rebuilds, which it now does when it
  should. Cargo already caches dependencies; only the crate itself recompiles.
- **Linking** is not a bottleneck here. If it becomes one, the dev shell already
  documents `-DCMAKE_LINKER_TYPE=MOLD` as an opt-in.

Distributed compilation (`sccache` with a remote backend, or `distcc`) would fit
the C++ side technically, but it is not worth introducing: the build is now ~5
minutes cold and seconds incrementally, and the remaining time is spread across
many short compilations rather than a few long ones. It would add a dependency
and a service to maintain in exchange for very little.
