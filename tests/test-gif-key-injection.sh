#!/usr/bin/env bash
set -Eeuo pipefail

# Verify the packaging scripts handle GIF provider keys safely, using SYNTHETIC
# canary values only. Covers the publish-time presence gate and the mapping of
# CI variables into the build-only generation step, asserting the value reaches
# the build via the environment (never a command line) and is never printed.

ROOT="$(git rev-parse --show-toplevel)"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# Hermetic against real pipeline variables: a protected-branch pipeline
# injects the actual GIPHY/KLIPY project variables, which would make the
# "publish without keys" cases pass the gate legitimately. Only the
# synthetic canaries below may reach the scripts under test.
unset GIPHY_API_KEY KLIPY_API_KEY \
    LIGHTNING_GIPHY_API_KEY LIGHTNING_KLIPY_API_KEY \
    LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY \
    2>/dev/null || true

CANARY_G="CANARY_GIPHY_injx_11"
CANARY_K="CANARY_KLIPY_injx_22"
fail=0
ok() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

# --- 1. Presence gate in validate-release-request.sh -----------------------
gate_env() {
    env CI_PROJECT_DIR="$WORK" TARGET_PROJECT_ID=6 LIGHTNING_PROJECT_ID=6 \
        PUBLISH_PACKAGES=true RELEASE_ACTION=attach-existing \
        RELEASE_VERSION=0.6.2 SOURCE_REF=v0.6.2 "$@"
}

printf '== presence gate ==\n'
if gate_env "$ROOT/scripts/validate-release-request.sh" >"$WORK/g1.log" 2>&1; then
    bad "publish without keys was accepted"
else
    grep -q 'requires the GIPHY_API_KEY' "$WORK/g1.log" && ok "missing GIPHY rejected" || bad "wrong error for missing GIPHY"
fi

if gate_env GIPHY_API_KEY="$CANARY_G" "$ROOT/scripts/validate-release-request.sh" >"$WORK/g2.log" 2>&1; then
    bad "publish with only GIPHY accepted"
else
    grep -q 'requires the KLIPY_API_KEY' "$WORK/g2.log" && ok "missing KLIPY rejected" || bad "wrong error for missing KLIPY"
fi

gate_env GIPHY_API_KEY="$CANARY_G" KLIPY_API_KEY="$CANARY_K" \
    "$ROOT/scripts/validate-release-request.sh" >"$WORK/g3.log" 2>&1 \
    && ok "both keys accepted" || bad "both keys unexpectedly rejected"
grep -qx 'GIPHY_API_KEY is configured' "$WORK/g3.log" && ok "GIPHY presence reported" || bad "no GIPHY presence line"
grep -qx 'KLIPY_API_KEY is configured' "$WORK/g3.log" && ok "KLIPY presence reported" || bad "no KLIPY presence line"
if grep -q "$CANARY_G\|$CANARY_K" "$WORK/g3.log"; then bad "gate leaked a key value"; else ok "gate prints no key value"; fi

# --- 2. configure-build.sh key mapping (env, not command line) -------------
# Stub the toolchain so no real build runs. The stub cmake records its configure
# argv and the LIGHTNING_BUILD_* environment it received.
printf '== configure-build key mapping ==\n'
BIN="$WORK/bin"; mkdir -p "$BIN"
ARGLOG="$WORK/cmake-argv.log"; ENVLOG="$WORK/cmake-env.log"

cat >"$BIN/cmake" <<STUB
#!/usr/bin/env bash
set -e
case "\$1" in
  --version) echo "cmake version 0.0-stub"; exit 0 ;;
  --build) exit 0 ;;
  --install)
    # DESTDIR install: stage a fake executable so the caller's checks pass.
    dest="\${DESTDIR:?}"
    mkdir -p "\$dest/usr/bin"
    printf '#!/bin/sh\necho "matrix-client 0.6.2"\necho "matrix_backend: rust"\necho "http_backend_compiled: false"\necho "mock_backend_compiled: false"\n' > "\$dest/usr/bin/matrix-client"
    chmod +x "\$dest/usr/bin/matrix-client"
    exit 0 ;;
  -S|*)
    # Configure invocation: record argv and the build-only key environment.
    printf '%s\n' "\$*" >> "$ARGLOG"
    printf 'GIPHY=%s\nKLIPY=%s\n' "\${LIGHTNING_BUILD_GIPHY_API_KEY:-<unset>}" "\${LIGHTNING_BUILD_KLIPY_API_KEY:-<unset>}" >> "$ENVLOG"
    exit 0 ;;
esac
STUB
for tool in cargo rustc patchelf strip; do
    printf '#!/usr/bin/env bash\necho "%s 0.0-stub"\nexit 0\n' "$tool" >"$BIN/$tool"
done
chmod +x "$BIN"/*

# Minimal fake Lightning source tree configure-build.sh requires.
SRC="$WORK/proj/work/lightning"
mkdir -p "$SRC/rust"
printf 'project(matrix-client VERSION 0.6.2)\n' >"$SRC/CMakeLists.txt"
printf '[package]\nname="x"\n' >"$SRC/rust/Cargo.toml"
printf '# lock\n' >"$SRC/rust/Cargo.lock"
printf 'GPL\n' >"$SRC/LICENSE"; printf '# readme\n' >"$SRC/README.md"
# packaging/common assets are copied from the real repo checkout.
mkdir -p "$WORK/proj/packaging"
cp -r "$ROOT/packaging/common" "$WORK/proj/packaging/common"

run_cfg() {
    : >"$ARGLOG"; : >"$ENVLOG"
    ( cd "$WORK/proj" && env "PATH=$BIN:$PATH" "CI_PROJECT_DIR=$WORK/proj" \
        BUILD_JOBS=1 "$@" bash "$ROOT/scripts/configure-build.sh" ) \
        >"$WORK/cfg.log" 2>&1
}

# Publishing build: keys mapped into the environment, REQUIRE flag on.
if run_cfg PUBLISH_PACKAGES=true GIPHY_API_KEY="$CANARY_G" KLIPY_API_KEY="$CANARY_K"; then
    grep -q 'LIGHTNING_REQUIRE_GIF_KEYS=ON' "$ARGLOG" && ok "REQUIRE flag ON when publishing" || bad "REQUIRE flag not ON"
    grep -q "GIPHY=$CANARY_G" "$ENVLOG" && ok "GIPHY mapped into build env" || bad "GIPHY not mapped to env"
    grep -q "KLIPY=$CANARY_K" "$ENVLOG" && ok "KLIPY mapped into build env" || bad "KLIPY not mapped to env"
    if grep -q "$CANARY_G\|$CANARY_K" "$ARGLOG"; then bad "key value leaked onto cmake command line"; else ok "no key on cmake command line"; fi
    if grep -q "$CANARY_G\|$CANARY_K" "$WORK/cfg.log"; then bad "key value leaked to build log"; else ok "no key in build log"; fi
else
    bad "publishing configure-build failed: $(tail -3 "$WORK/cfg.log")"
fi

# Build-only: no keys required, REQUIRE flag off, no build-only env exported.
if run_cfg PUBLISH_PACKAGES=false; then
    grep -q 'LIGHTNING_REQUIRE_GIF_KEYS=OFF' "$ARGLOG" && ok "REQUIRE flag OFF for build-only" || bad "REQUIRE flag not OFF"
    grep -q 'GIPHY=<unset>' "$ENVLOG" && ok "no build-only key env for build-only" || bad "unexpected key env in build-only"
else
    bad "build-only configure-build failed: $(tail -3 "$WORK/cfg.log")"
fi

if [[ "$fail" == 0 ]]; then printf 'GIF key injection tests passed\n'; else printf 'GIF key injection tests FAILED\n' >&2; exit 1; fi
