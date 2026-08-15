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
    UPDATE_SIGNING_KEY_ID UPDATE_SIGNING_KEY_B64 UPDATE_SIGNING_PUBKEY_2026A \
    2>/dev/null || true

CANARY_G="CANARY_GIPHY_injx_11"
CANARY_K="CANARY_KLIPY_injx_22"
fail=0
ok() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

# validate-release-request.sh also gates the update-signing key triple, so a
# publishing request needs a consistent one before its GIF assertions can be
# reached. Generated fresh here; nothing is committed and nothing is printed.
# (The gate's own behaviour is covered by tests/test-update-manifest.sh.)
command -v openssl >/dev/null 2>&1 || { printf 'error: openssl is required\n' >&2; exit 1; }
SIGN_KEY="$WORK/update-signing.pem"
openssl genpkey -algorithm ed25519 -out "$SIGN_KEY" 2>/dev/null
chmod 600 "$SIGN_KEY"
SIGN_KEY_B64="$(openssl base64 -A -in "$SIGN_KEY")"
SIGN_PUB_B64="$(openssl pkey -in "$SIGN_KEY" -pubout -outform DER 2>/dev/null \
    | tail -c 32 | openssl base64 -A)"

# --- 1. Presence gate in validate-release-request.sh -----------------------
gate_env() {
    env CI_PROJECT_DIR="$WORK" TARGET_PROJECT_ID=6 LIGHTNING_PROJECT_ID=6 \
        PUBLISH_PACKAGES=true RELEASE_ACTION=attach-existing \
        RELEASE_VERSION=0.6.2 SOURCE_REF=v0.6.2 \
        UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$SIGN_KEY_B64" \
        UPDATE_SIGNING_PUBKEY_2026A="$SIGN_PUB_B64" "$@"
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
if grep -qF "$SIGN_KEY_B64" "$WORK/g3.log" || grep -q 'BEGIN PRIVATE KEY' "$WORK/g3.log"; then
    bad "gate leaked update-signing key material"
else
    ok "gate prints no update-signing key material"
fi

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
    # DESTDIR install: stage fake executables so the caller's checks pass.
    # BOTH are staged, because the real install rule installs both
    # (install(TARGETS matrix-client lightning-updater ...)) and
    # configure-build.sh now refuses a stage without the update helper.
    dest="\${DESTDIR:?}"
    mkdir -p "\$dest/usr/bin"
    printf '#!/bin/sh\necho "matrix-client 0.6.2"\necho "matrix_backend: rust"\necho "http_backend_compiled: false"\necho "mock_backend_compiled: false"\n' > "\$dest/usr/bin/matrix-client"
    chmod +x "\$dest/usr/bin/matrix-client"
    printf '#!/bin/sh\nexit 0\n' > "\$dest/usr/bin/lightning-updater"
    chmod +x "\$dest/usr/bin/lightning-updater"
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

# LIGHTNING_INSTALL_TYPE is mandatory: it decides which install strategy the
# updater will use, so configure-build.sh refuses to guess it. The per-format
# build scripts set it; this test is about GIF keys, so it supplies a valid one
# and lets a case override it. (The requirement itself is exercised below.)
run_cfg() {
    : >"$ARGLOG"; : >"$ENVLOG"
    ( cd "$WORK/proj" && env "PATH=$BIN:$PATH" "CI_PROJECT_DIR=$WORK/proj" \
        BUILD_JOBS=1 LIGHTNING_INSTALL_TYPE=linux-deb "$@" \
        bash "$ROOT/scripts/configure-build.sh" ) \
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

# --- 3. install type and update trust root ---------------------------------
# The install type decides which compiled-in install strategy the updater uses,
# so a wrong value offers a user the wrong kind of update. There is deliberately
# no default: guessing a package type would be worse than failing.
printf '== install type and update trust root ==\n'
if run_cfg PUBLISH_PACKAGES=false LIGHTNING_INSTALL_TYPE=; then
    bad "configure-build ran without an install type"
else
    # ${VAR:?} treats unset and empty identically, which is what the per-format
    # build scripts can realistically get wrong.
    grep -q 'LIGHTNING_INSTALL_TYPE' "$WORK/cfg.log" && ok "a missing install type is rejected by name" \
        || bad "missing install type gave an unhelpful error"
fi
if run_cfg PUBLISH_PACKAGES=false LIGHTNING_INSTALL_TYPE=windows-msi; then
    bad "a non-Linux install type was accepted by the Linux build path"
else
    ok "an install type this build cannot produce is rejected"
fi
if run_cfg PUBLISH_PACKAGES=false LIGHTNING_INSTALL_TYPE=linux-rpm \
        UPDATE_SIGNING_PUBKEY_2026A=PUBKEYCANARYAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=; then
    grep -q 'LIGHTNING_INSTALL_TYPE=linux-rpm' "$ARGLOG" \
        && ok "the install type reaches the build" || bad "install type not passed to cmake"
    # The update trust root is a PUBLIC key: unlike the GIF keys it is NOT
    # secret and IS passed on the command line, which is where the source's
    # cache variable expects it.
    grep -q 'LIGHTNING_UPDATE_PUBKEY_2026A=PUBKEYCANARY' "$ARGLOG" \
        && ok "the update public key reaches the build" || bad "update public key not passed to cmake"
else
    bad "configure-build with an install type failed: $(tail -3 "$WORK/cfg.log")"
fi
# A build with no trust root compiled in is allowed (it fails closed at runtime)
# and must not be turned into a build failure here; refusing to PUBLISH such a
# build is validate-release-request.sh's job, not this script's.
if run_cfg PUBLISH_PACKAGES=false LIGHTNING_INSTALL_TYPE=linux-deb; then
    grep -q 'LIGHTNING_UPDATE_PUBKEY_2026A=' "$ARGLOG" \
        && ok "an absent update public key still configures (fails closed at runtime)" \
        || bad "the update public key flag disappeared"
else
    bad "keyless-trust-root configure failed: $(tail -3 "$WORK/cfg.log")"
fi

if [[ "$fail" == 0 ]]; then printf 'GIF key injection tests passed\n'; else printf 'GIF key injection tests FAILED\n' >&2; exit 1; fi
