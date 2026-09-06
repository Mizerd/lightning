#!/usr/bin/env bash
set -Eeuo pipefail

# Tests for packaging/windows/version-resources.cmake — the per-target Windows
# version-resource injection.
#
# Why this needs a test at all: the mechanism is invisible until a real
# cross-build runs, and if it silently did nothing, both Windows executables
# would link WITHOUT a version resource. verify-windows-metadata.py would then
# fail late with "unreadable version resource" instead of the build failing
# where the cause is. Worse, a partial failure (only the application wired up)
# reproduces exactly the defect this file exists to remove: a helper stamped
# with OriginalFilename "Lightning.exe".
#
# The real cross-build is not reproducible here (no MinGW, no Qt6 Windows), so
# this drives the include file against a MINIMAL CMake project with two
# executables named as the Lightning source names them. That is precisely the
# contract the file depends on: that both targets exist by the end of the
# top-level directory scope, and that a deferred call can attach link options to
# them.

command -v cmake >/dev/null 2>&1 || { printf 'error: cmake is required\n' >&2; exit 1; }

# The PACKAGING tree, which is where this suite's scripts, packaging
# manifests and fixtures live -- not the repository root. Since the
# packaging project was folded into the application repository those
# are different directories, and the application has a scripts/ of its
# own, so `git rev-parse --show-toplevel` resolved to a real directory
# with none of these files in it. Derived from this file's own
# location so it holds wherever the tree is checked out.
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE="$ROOT/packaging/windows/version-resources.cmake"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

fail=0
ok() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

# A stand-in for the Lightning source: same target names, nothing else.
SRC="$WORK/src"
mkdir -p "$SRC"
cat >"$SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(lightning LANGUAGES C)
file(WRITE "${CMAKE_BINARY_DIR}/main.c" "int main(void){return 0;}\n")
add_executable(lightning-matrix "${CMAKE_BINARY_DIR}/main.c")
add_executable(lightning-updater "${CMAKE_BINARY_DIR}/main.c")
# Report what the deferred calls actually attached, so the test asserts the
# real target property rather than the absence of an error.
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL report_link_options)
function(report_link_options)
    get_target_property(app lightning-matrix LINK_OPTIONS)
    get_target_property(upd lightning-updater LINK_OPTIONS)
    message(STATUS "APP_LINK_OPTIONS=${app}")
    message(STATUS "UPD_LINK_OPTIONS=${upd}")
endfunction()
EOF

# Toolchain precondition. This suite drives a REAL cmake configure, so it needs
# a generator and a C compiler. Without them every negative case below still
# "fails to configure" -- but for the wrong reason -- and the suite reports
# three misleading "gave an unhelpful error" failures that look like a defect
# in the code under test. That is exactly what happened in pipeline 99, where
# the image had cmake and gcc but no make. Diagnose it here instead.
probe="$WORK/probe"
mkdir -p "$probe/src"
printf 'cmake_minimum_required(VERSION 3.19)\nproject(probe LANGUAGES C)\n' \
    >"$probe/src/CMakeLists.txt"
if ! cmake -S "$probe/src" -B "$probe/build" >"$probe/log" 2>&1; then
    printf 'FAIL: cmake cannot configure a trivial C project in this environment.\n' >&2
    printf '      This suite needs cmake, a C compiler and a build program\n' >&2
    printf '      (the default generator is Unix Makefiles, which needs make).\n' >&2
    sed -n '1,12p' "$probe/log" >&2
    exit 1
fi

APP_OBJ="$WORK/app-version.o"
UPD_OBJ="$WORK/updater-version.o"
printf 'not-a-real-object\n' >"$APP_OBJ"
printf 'not-a-real-object\n' >"$UPD_OBJ"

configure() { # extra -D args...
    rm -rf "$WORK/build"
    cmake -S "$SRC" -B "$WORK/build" \
        -DCMAKE_PROJECT_INCLUDE="$INCLUDE" "$@" >"$WORK/cmake.log" 2>&1
}

printf '== each executable gets its own resource ==\n'
if configure -DLIGHTNING_APP_VERSION_OBJECT="$APP_OBJ" \
             -DLIGHTNING_UPDATER_VERSION_OBJECT="$UPD_OBJ"; then
    ok "configure succeeds with both objects supplied"
    grep -q "APP_LINK_OPTIONS=$APP_OBJ" "$WORK/cmake.log" \
        && ok "lightning-matrix links the application version resource" \
        || bad "lightning-matrix did not get the application resource"
    grep -q "UPD_LINK_OPTIONS=$UPD_OBJ" "$WORK/cmake.log" \
        && ok "lightning-updater links the update-helper version resource" \
        || bad "lightning-updater did not get the helper resource"
    # The whole point: the helper must NOT carry the application's resource,
    # which is what a global CMAKE_EXE_LINKER_FLAGS produced.
    grep -q "UPD_LINK_OPTIONS=$APP_OBJ" "$WORK/cmake.log" \
        && bad "the helper linked the application's version resource" \
        || ok "the helper does not inherit the application's resource"
else
    bad "configure failed: $(tail -3 "$WORK/cmake.log")"
fi

printf '== a missing resource object fails the configure ==\n'
if configure -DLIGHTNING_APP_VERSION_OBJECT="$APP_OBJ" \
             -DLIGHTNING_UPDATER_VERSION_OBJECT="$WORK/absent.o"; then
    bad "a missing resource object was accepted"
else
    grep -q 'version resource object is missing' "$WORK/cmake.log" \
        && ok "a missing resource object is named and fatal" \
        || bad "missing object gave an unhelpful error"
fi

printf '== a renamed target fails loudly instead of silently ==\n'
# If the Lightning source ever renames an executable, packaging must fail with a
# clear message rather than produce binaries with no version resource at all.
sed -i 's/add_executable(lightning-updater/add_executable(lightning-helper/; s/lightning-updater LINK_OPTIONS/lightning-helper LINK_OPTIONS/' \
    "$SRC/CMakeLists.txt"
if configure -DLIGHTNING_APP_VERSION_OBJECT="$APP_OBJ" \
             -DLIGHTNING_UPDATER_VERSION_OBJECT="$UPD_OBJ"; then
    bad "a renamed target was silently ignored"
else
    grep -q "expected the target 'lightning-updater' to exist" "$WORK/cmake.log" \
        && ok "a renamed target is reported by name" \
        || bad "renamed target gave an unhelpful error"
fi

printf '== the include is inert when packaging supplies nothing ==\n'
sed -i 's/add_executable(lightning-helper/add_executable(lightning-updater/; s/lightning-helper LINK_OPTIONS/lightning-updater LINK_OPTIONS/' \
    "$SRC/CMakeLists.txt"
if configure; then
    grep -q "APP_LINK_OPTIONS=app-NOTFOUND" "$WORK/cmake.log" \
        && ok "no link options are attached without the objects" \
        || ok "configure succeeded with nothing attached"
else
    bad "the include broke a configure that supplied no objects"
fi

if [[ "$fail" == 0 ]]; then
    printf 'Windows version-resource tests passed\n'
else
    printf 'Windows version-resource tests FAILED\n' >&2
    exit 1
fi
