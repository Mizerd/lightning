#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
shopt -s nullglob
packages=("$ROOT"/dist/*.deb)
(( ${#packages[@]} == 1 )) || die "expected exactly one DEB package"
package="${packages[0]}"
expected="lightning_${DEB_VERSION}_amd64.deb"
[[ "$(basename "$package")" == "$expected" ]] || die "unexpected DEB filename"
[[ ! -e "$ROOT/work/lightning" ]] || die "validation must not receive a source checkout"
command -v nix >/dev/null 2>&1 && die "Nix must be absent from the validation environment"

dpkg-deb --info "$package" | tee "$ROOT/dist/deb-info.txt"
dpkg-deb --contents "$package" | tee "$ROOT/dist/deb-contents.txt"
lintian --fail-on error "$package" | tee "$ROOT/dist/deb-lintian.log"
(cd "$ROOT/dist" && sha256sum -c "$(basename "$package").sha256")

audit_root="$(mktemp -d)"
cleanup() { rm -rf "$audit_root"; }
trap cleanup EXIT
dpkg-deb --extract "$package" "$audit_root"
binary="$audit_root/usr/bin/matrix-client"
[[ -x "$binary" ]] || die "packaged executable is missing"
# The update helper ships beside the application in every format. Without it the
# in-app updater has nothing to hand a verified .deb to and the feature is inert.
updater="$audit_root/usr/bin/lightning-updater"
[[ -x "$updater" ]] || die "packaged update helper is missing"
file "$binary" "$updater" | tee "$ROOT/dist/deb-file.txt"
readelf -d "$binary" "$updater" | tee "$ROOT/dist/deb-readelf.txt"
if grep -E '(RPATH|RUNPATH)' "$ROOT/dist/deb-readelf.txt"; then
    die "DEB executable contains an RPATH or RUNPATH"
fi
if grep -RIlE '/nix/store|/home/roksme|/builds/|LIGHTNING_(GIPHY|KLIPY)_API_KEY=|PRIVATE-TOKEN:|recovery_key=' "$audit_root" | grep -q . || \
   strings "$binary" | grep -qE '/nix/store|/home/roksme|/builds/|LIGHTNING_(GIPHY|KLIPY)_API_KEY=|PRIVATE-TOKEN:|recovery_key='; then
    die "DEB contains a forbidden path or credential marker"
fi
if find "$audit_root" -xdev -type f \( -name '*.o' -o -name '*.a' -o -name '*.cpp' -o -name '*.rs' \) -print -quit | grep -q .; then
    die "DEB contains source or intermediate build files"
fi
if find "$audit_root" -xdev \( -type f -o -type d \) -perm -0002 -print -quit | grep -q .; then
    die "DEB contains world-writable files"
fi
if find "$audit_root" -xdev -type f \( -perm -4000 -o -perm -2000 \) -print -quit | grep -q .; then
    die "DEB contains setuid or setgid files"
fi

apt-get install -y "$package"
test -x /usr/bin/matrix-client
test -x /usr/bin/lightning-updater
desktop-file-validate /usr/share/applications/lightning.desktop
appstreamcli validate --no-net /usr/share/metainfo/lightning.metainfo.xml
version_output="$(cd /tmp && /usr/bin/matrix-client --version)"
printf '%s\n' "$version_output" | tee "$ROOT/dist/deb-version.txt"
[[ "$version_output" == "matrix-client $BASE_VERSION" ]] || die "installed DEB version output is wrong"
if ldd /usr/bin/matrix-client | tee "$ROOT/dist/deb-ldd.txt" | grep -q 'not found'; then
    die "installed DEB executable has missing shared libraries"
fi

set +e
(cd /tmp && timeout 15s env QT_QPA_PLATFORM=offscreen /usr/bin/matrix-client --backend=rust) \
    >"$ROOT/dist/deb-headless.log" 2>&1
headless_status=$?
set -e
[[ "$headless_status" == 0 || "$headless_status" == 124 ]] || {
    cat "$ROOT/dist/deb-headless.log" >&2
    die "DEB headless launch failed with status $headless_status"
}
if grep -Ei 'module .* is not installed|cannot load library|failed to load.*plugin|no such file' "$ROOT/dist/deb-headless.log"; then
    die "DEB headless launch reported a missing runtime component"
fi

# The call media engine, asked of the INSTALLED package. This is the one check
# that can see the 0.8.0 defect: an engine-less build installs cleanly, launches
# cleanly, passes every audit above, and then refuses every call. It also proves
# the GStreamer plugin Depends resolved, because the engine's element probe runs
# against whatever apt actually pulled in.
set +e
(cd /tmp && timeout 60s /usr/bin/matrix-client --call-media-status) \
    >"$ROOT/dist/deb-call-media-status.txt" 2>&1
call_media_status=$?
set -e
assert_call_media_engine DEB "$ROOT/dist/deb-call-media-status.txt" "$call_media_status"

# The generated build-only key header must never ship inside the package.
if grep -q 'LightningGifBuildKeys.h' "$ROOT/dist/deb-contents.txt"; then
    die "DEB contains the generated GIF build-key header"
fi

# GIF provider configuration must hold with every key variable unset, proving
# the values are embedded in the binary rather than read from the environment.
gif_env_clear() {
    env -u GIPHY_API_KEY -u KLIPY_API_KEY \
        -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
        -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY "$@"
}
status_out="$(cd /tmp && gif_env_clear /usr/bin/matrix-client --gif-status)"
printf '%s\n' "$status_out" | tee "$ROOT/dist/deb-gif-status.txt"
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    printf '%s\n' "$status_out" | grep -qx 'GIPHY configured: yes' || \
        die "packaged DEB reports GIPHY unconfigured with keys unset"
    printf '%s\n' "$status_out" | grep -qx 'KLIPY configured: yes' || \
        die "packaged DEB reports KLIPY unconfigured with keys unset"
    # Bounded real trending request per provider using only the embedded keys.
    set +e
    (cd /tmp && gif_env_clear /usr/bin/matrix-client --gif-selftest) \
        >"$ROOT/dist/deb-gif-selftest.txt" 2>&1
    selftest_rc=$?
    set -e
    cat "$ROOT/dist/deb-gif-selftest.txt"
    [[ "$selftest_rc" == 0 ]] || die "packaged DEB GIF provider self-test failed"
    grep -q 'GIPHY request: ok' "$ROOT/dist/deb-gif-selftest.txt" || die "DEB GIPHY live request failed"
    grep -q 'KLIPY request: ok' "$ROOT/dist/deb-gif-selftest.txt" || die "DEB KLIPY live request failed"
    # No key or authenticated provider URL may appear in the diagnostics.
    if grep -Eq 'api_key=|://' "$ROOT/dist/deb-gif-status.txt" "$ROOT/dist/deb-gif-selftest.txt"; then
        die "DEB GIF diagnostic output leaked a key or URL"
    fi
else
    printf 'Build-only DEB: GIF providers are keyless (informational only)\n'
fi

apt-get remove -y lightning
audit_output="$(dpkg --audit)"
[[ -z "$audit_output" ]] || die "dpkg audit reported an inconsistent package state"
if dpkg-query -W -f='${db:Status-Abbrev}' lightning 2>/dev/null | grep -q '^ii'; then
    die "DEB package remains installed after removal"
fi
printf 'DEB clean install, runtime, headless launch, and uninstall passed\n'
