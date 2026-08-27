#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
shopt -s nullglob
packages=("$ROOT"/dist/*.rpm)
(( ${#packages[@]} == 1 )) || die "expected exactly one RPM package"
package="${packages[0]}"
expected="lightning-${RPM_VERSION}-${RPM_RELEASE}.x86_64.rpm"
[[ "$(basename "$package")" == "$expected" ]] || die "unexpected RPM filename"
[[ ! -e "$ROOT/work/lightning" ]] || die "validation must not receive a source checkout"
command -v nix >/dev/null 2>&1 && die "Nix must be absent from the validation environment"

rpm -qpi "$package" | tee "$ROOT/dist/rpm-info.txt"
rpm -qpl "$package" | tee "$ROOT/dist/rpm-contents.txt"
rpm -qpR "$package" | tee "$ROOT/dist/rpm-requires.txt"
rpmlint "$package" >"$ROOT/dist/rpm-rpmlint.log" 2>&1 || true
cat "$ROOT/dist/rpm-rpmlint.log"

# Waived rpmlint errors, exact "<check> <argument>" pairs. Each needs a reason
# here, and a waiver that STOPS FIRING is itself a failure below: a stale entry
# is how a real error later slips through under an old justification.
#
# explicit-lib-dependency libnice-gstreamer1
#   rpmlint fires this on any Requires whose name looks like a library, on the
#   premise that rpm's automatic soname dependencies already cover it. They
#   cannot cover this one: libnice-gstreamer1 ships a GStreamer PLUGIN
#   (libgstnice.so) that is dlopen'd from the plugin path at runtime, so it
#   appears in no ELF NEEDED entry of ours and the auto-generator never sees
#   it. Dropping the Requires would produce a package that installs cleanly
#   and then refuses every call — see the comment above it in
#   packaging/rpm/lightning.spec.
rpmlint_waivers=(
    "explicit-lib-dependency libnice-gstreamer1"
)

errors="$(grep -E '(^|: )E: ' "$ROOT/dist/rpm-rpmlint.log" || true)"
for waiver in "${rpmlint_waivers[@]}"; do
    if ! grep -qF ": E: $waiver" <<<"$errors"; then
        die "stale rpmlint waiver (no longer reported): $waiver"
    fi
    printf 'validate-rpm: waived rpmlint error: %s\n' "$waiver"
    errors="$(grep -vF ": E: $waiver" <<<"$errors" || true)"
done

if [[ -n "${errors//[[:space:]]/}" ]]; then
    printf '%s\n' "$errors"
    die "rpmlint reported errors"
fi
(cd "$ROOT/dist" && sha256sum -c "$(basename "$package").sha256")

audit_root="$(mktemp -d)"
cleanup() { rm -rf "$audit_root"; }
trap cleanup EXIT
rpm2cpio "$package" | (cd "$audit_root" && cpio -idm --quiet)
binary="$audit_root/usr/bin/matrix-client"
[[ -x "$binary" ]] || die "packaged executable is missing"
# The update helper ships beside the application in every format. Without it the
# in-app updater has nothing to hand a verified .rpm to and the feature is inert.
updater="$audit_root/usr/bin/lightning-updater"
[[ -x "$updater" ]] || die "packaged update helper is missing"
file "$binary" "$updater" | tee "$ROOT/dist/rpm-file.txt"
readelf -d "$binary" "$updater" | tee "$ROOT/dist/rpm-readelf.txt"
if grep -E '(RPATH|RUNPATH)' "$ROOT/dist/rpm-readelf.txt"; then
    die "RPM executable contains an RPATH or RUNPATH"
fi
if grep -RIlE '/nix/store|/home/roksme|/builds/|LIGHTNING_(GIPHY|KLIPY)_API_KEY=|PRIVATE-TOKEN:|recovery_key=' "$audit_root" | grep -q . || \
   strings "$binary" | grep -qE '/nix/store|/home/roksme|/builds/|LIGHTNING_(GIPHY|KLIPY)_API_KEY=|PRIVATE-TOKEN:|recovery_key='; then
    die "RPM contains a forbidden path or credential marker"
fi
if find "$audit_root" -xdev -type f \( -name '*.o' -o -name '*.a' -o -name '*.cpp' -o -name '*.rs' \) -print -quit | grep -q .; then
    die "RPM contains source or intermediate build files"
fi
if find "$audit_root" -xdev \( -type f -o -type d \) -perm -0002 -print -quit | grep -q .; then
    die "RPM contains world-writable files"
fi
if find "$audit_root" -xdev -type f \( -perm -4000 -o -perm -2000 \) -print -quit | grep -q .; then
    die "RPM contains setuid or setgid files"
fi

dnf install -y "$package"
test -x /usr/bin/matrix-client
test -x /usr/bin/lightning-updater
desktop-file-validate /usr/share/applications/lightning.desktop
appstreamcli validate --no-net /usr/share/metainfo/lightning.metainfo.xml
version_output="$(cd /tmp && /usr/bin/matrix-client --version)"
printf '%s\n' "$version_output" | tee "$ROOT/dist/rpm-version.txt"
[[ "$version_output" == "matrix-client $BASE_VERSION" ]] || die "installed RPM version output is wrong"
if ldd /usr/bin/matrix-client | tee "$ROOT/dist/rpm-ldd.txt" | grep -q 'not found'; then
    die "installed RPM executable has missing shared libraries"
fi

set +e
(cd /tmp && timeout 15s env QT_QPA_PLATFORM=offscreen /usr/bin/matrix-client --backend=rust) \
    >"$ROOT/dist/rpm-headless.log" 2>&1
headless_status=$?
set -e
[[ "$headless_status" == 0 || "$headless_status" == 124 ]] || {
    cat "$ROOT/dist/rpm-headless.log" >&2
    die "RPM headless launch failed with status $headless_status"
}
if grep -Ei 'module .* is not installed|cannot load library|failed to load.*plugin|no such file' "$ROOT/dist/rpm-headless.log"; then
    die "RPM headless launch reported a missing runtime component"
fi

# The call media engine, asked of the INSTALLED package -- see validate-deb.sh
# for why nothing else can see this. It doubles as proof that the spec's
# GStreamer Requires resolved: the engine's element probe runs against whatever
# dnf actually pulled in, and rpm's automatic generator can see none of it.
set +e
(cd /tmp && timeout 60s /usr/bin/matrix-client --call-media-status) \
    >"$ROOT/dist/rpm-call-media-status.txt" 2>&1
call_media_status=$?
set -e
assert_call_media_engine RPM "$ROOT/dist/rpm-call-media-status.txt" "$call_media_status"

# The generated build-only key header must never ship inside the package.
if grep -q 'LightningGifBuildKeys.h' "$ROOT/dist/rpm-contents.txt"; then
    die "RPM contains the generated GIF build-key header"
fi

# GIF provider configuration must hold with every key variable unset, proving
# the values are embedded in the binary rather than read from the environment.
gif_env_clear() {
    env -u GIPHY_API_KEY -u KLIPY_API_KEY \
        -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
        -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY "$@"
}
status_out="$(cd /tmp && gif_env_clear /usr/bin/matrix-client --gif-status)"
printf '%s\n' "$status_out" | tee "$ROOT/dist/rpm-gif-status.txt"
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    printf '%s\n' "$status_out" | grep -qx 'GIPHY configured: yes' || \
        die "packaged RPM reports GIPHY unconfigured with keys unset"
    printf '%s\n' "$status_out" | grep -qx 'KLIPY configured: yes' || \
        die "packaged RPM reports KLIPY unconfigured with keys unset"
    # Bounded real trending request per provider using only the embedded keys.
    set +e
    (cd /tmp && gif_env_clear /usr/bin/matrix-client --gif-selftest) \
        >"$ROOT/dist/rpm-gif-selftest.txt" 2>&1
    selftest_rc=$?
    set -e
    cat "$ROOT/dist/rpm-gif-selftest.txt"
    [[ "$selftest_rc" == 0 ]] || die "packaged RPM GIF provider self-test failed"
    grep -q 'GIPHY request: ok' "$ROOT/dist/rpm-gif-selftest.txt" || die "RPM GIPHY live request failed"
    grep -q 'KLIPY request: ok' "$ROOT/dist/rpm-gif-selftest.txt" || die "RPM KLIPY live request failed"
    # No key or authenticated provider URL may appear in the diagnostics.
    if grep -Eq 'api_key=|://' "$ROOT/dist/rpm-gif-status.txt" "$ROOT/dist/rpm-gif-selftest.txt"; then
        die "RPM GIF diagnostic output leaked a key or URL"
    fi
else
    printf 'Build-only RPM: GIF providers are keyless (informational only)\n'
fi

dnf remove -y lightning
dnf check
if rpm -q lightning >/dev/null 2>&1; then
    die "RPM package remains installed after removal"
fi
printf 'RPM clean install, runtime, headless launch, and uninstall passed\n'
