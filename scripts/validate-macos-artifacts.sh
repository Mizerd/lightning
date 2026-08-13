#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Structural validation of the assembled Lightning.app. This proves the bundle
# is well-formed, self-contained, arm64, and runnable on this machine — it does
# NOT prove it is distributable. Gatekeeper acceptance is checked and reported
# honestly as a known failure; see docs/macos-packaging.md.

[[ $# -eq 1 ]] || die "usage: validate-macos-artifacts.sh <dist/macos>"
MACOS_DIST="$1"
APP_NAME="Lightning"
APP_DIR="$MACOS_DIST/${APP_NAME}.app"
CONTENTS="$APP_DIR/Contents"
REPORT_DIR="$MACOS_DIST/reports"
mkdir -p "$REPORT_DIR"

[[ -d "$APP_DIR" ]] || die "bundle not found: $APP_DIR"

failures=0
check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        printf '  ok: %s\n' "$desc"
    else
        printf '  FAIL: %s\n' "$desc" >&2
        failures=$((failures + 1))
    fi
}

printf 'Validating %s\n' "$APP_DIR"

# --- bundle structure --------------------------------------------------------
check "Info.plist present"            test -f "$CONTENTS/Info.plist"
check "executable present"            test -x "$CONTENTS/MacOS/$APP_NAME"
check "icon present"                  test -f "$CONTENTS/Resources/${APP_NAME}.icns"
check "PkgInfo present"               test -f "$CONTENTS/PkgInfo"
check "build-info.json present"       test -f "$CONTENTS/Resources/build-info.json"
check "Frameworks directory present"  test -d "$CONTENTS/Frameworks"

# --- Info.plist keys ---------------------------------------------------------
plist_get() { /usr/libexec/PlistBuddy -c "Print :$1" "$CONTENTS/Info.plist" 2>/dev/null; }
for key in CFBundleIdentifier CFBundleExecutable CFBundleName \
           CFBundleShortVersionString CFBundleVersion LSMinimumSystemVersion; do
    check "Info.plist has $key" test -n "$(plist_get "$key")"
done
# Not cosmetic: the client records voice messages, and macOS kills any process
# that opens the microphone without a usage string. A bundle missing this is
# broken at runtime, so it is a hard failure here.
check "Info.plist declares NSMicrophoneUsageDescription" \
    test -n "$(plist_get NSMicrophoneUsageDescription)"

BUNDLE_VERSION="$(plist_get CFBundleShortVersionString || true)"
BUNDLE_ID="$(plist_get CFBundleIdentifier || true)"
printf '  bundle: %s %s\n' "$BUNDLE_ID" "$BUNDLE_VERSION"

# --- architecture ------------------------------------------------------------
ARCHS="$(lipo -archs "$CONTENTS/MacOS/$APP_NAME" 2>/dev/null || echo unknown)"
printf '  architectures: %s\n' "$ARCHS"
check "main executable is arm64" test "$ARCHS" = "arm64"
check "main executable is Mach-O" sh -c \
    "file -b '$CONTENTS/MacOS/$APP_NAME' | grep -q 'Mach-O.*arm64'"

# --- self-containment --------------------------------------------------------
# Every dependency must resolve inside the bundle or in /System|/usr/lib. A
# leftover /opt/homebrew dependency means macdeployqt missed a library and the
# app would only launch on this build machine.
#
# The install NAME (LC_ID_DYLIB) is deliberately excluded. macdeployqt copies
# Homebrew's frameworks without rewriting their own IDs, so every bundled
# framework still calls itself /opt/homebrew/opt/qt*/lib/... That string is what
# a future linker would record, not something dyld resolves at run time — the
# loader follows the *dependent's* load command, which is @rpath or
# @executable_path. Treating the ID as a leak flagged all 46 frameworks on a
# bundle that runs correctly.
#
# `otool -L` prints: line 1 the file path, line 2 the install ID (for
# libraries/frameworks only), then the dependencies. `otool -D` yields the ID on
# its own, so it can be subtracted precisely instead of guessing a line offset.
deps_of() {
    local macho="$1" id
    id="$(otool -D "$macho" 2>/dev/null | tail -n +2 | head -1)"
    otool -L "$macho" 2>/dev/null | tail -n +2 | sed -E 's/^[[:space:]]*//; s/ \(compatibility.*$//' \
        | { if [[ -n "$id" ]]; then grep -vxF "$id"; else cat; fi; }
}

otool -L "$CONTENTS/MacOS/$APP_NAME" >"$REPORT_DIR/otool-main.txt" 2>&1 || true
external="$(deps_of "$CONTENTS/MacOS/$APP_NAME" | grep -E '^(/opt/homebrew|/usr/local)' || true)"
if [[ -n "$external" ]]; then
    printf '  FAIL: executable links libraries outside the bundle:\n%s\n' "$external" >&2
    failures=$((failures + 1))
else
    printf '  ok: executable has no /opt/homebrew or /usr/local dependencies\n'
fi

# Same check across the whole bundle. Every Mach-O is identified by content
# rather than by permission bits or extension, because framework payloads carry
# neither a .dylib suffix nor a consistent mode.
: >"$REPORT_DIR/otool-all.txt"
leaked=0
checked=0
while IFS= read -r candidate; do
    file -b "$candidate" 2>/dev/null | grep -q 'Mach-O' || continue
    checked=$((checked + 1))
    { printf '== %s\n' "${candidate#$APP_DIR/}"; otool -L "$candidate" 2>&1; } >>"$REPORT_DIR/otool-all.txt"
    if deps_of "$candidate" | grep -qE '^(/opt/homebrew|/usr/local)'; then
        printf '  leaked host dependency in: %s\n' "${candidate#$APP_DIR/}" >&2
        leaked=$((leaked + 1))
    fi
done < <(find "$APP_DIR" -type f 2>/dev/null)
if (( leaked > 0 )); then
    printf '  FAIL: %d of %d bundled binaries depend on host-only paths\n' "$leaked" "$checked" >&2
    failures=$((failures + 1))
else
    printf '  ok: none of %d bundled binaries depend on host-only paths\n' "$checked"
fi

# --- Qt runtime --------------------------------------------------------------
# The modules Lightning links; if macdeployqt missed one the app dies at launch.
for fw in QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtSql QtWidgets QtMultimedia; do
    check "framework bundled: $fw" test -d "$CONTENTS/Frameworks/${fw}.framework"
done
# QML plugins live under Resources/qml; without them the UI fails to instantiate
# even though the app links fine.
check "QML modules bundled" test -d "$CONTENTS/Resources/qml"
for qml_mod in QtQuick QtQml; do
    check "QML module present: $qml_mod" test -d "$CONTENTS/Resources/qml/$qml_mod"
done
# Cocoa platform plugin — without it Qt cannot create a window at all.
check "cocoa platform plugin bundled" \
    test -f "$CONTENTS/PlugIns/platforms/libqcocoa.dylib"

qt_fw_count="$(find "$CONTENTS/Frameworks" -maxdepth 1 -name '*.framework' 2>/dev/null | wc -l | tr -d ' ')"
printf '  bundled frameworks: %s\n' "$qt_fw_count"

# --- signature ---------------------------------------------------------------
# Ad-hoc signature must be structurally valid or the bundle will not run on
# Apple Silicon at all.
check "code signature verifies" codesign --verify --deep --strict "$APP_DIR"
codesign -dv --verbose=4 "$APP_DIR" >"$REPORT_DIR/codesign-info.txt" 2>&1 || true

# Gatekeeper is EXPECTED to reject this bundle: it is ad-hoc signed, not
# Developer ID signed, and not notarized. Recorded as evidence rather than
# asserted as a pass, so nobody mistakes a green pipeline for a distributable
# artifact.
if spctl --assess --type execute --verbose=4 "$APP_DIR" >"$REPORT_DIR/spctl.txt" 2>&1; then
    printf '  note: Gatekeeper ACCEPTED the bundle (unexpected for an ad-hoc signature)\n'
    GATEKEEPER=accepted
else
    printf '  expected: Gatekeeper rejects the bundle (unsigned/un-notarized test build)\n'
    GATEKEEPER=rejected
fi

# --- launch smoke ------------------------------------------------------------
# Headless CLI entry points only. The runner has a GUI session, but asserting on
# a real window from CI would be flaky and is not what this proves; this
# confirms the bundled binary and its frameworks actually load and execute.
if "$CONTENTS/MacOS/$APP_NAME" --version >"$REPORT_DIR/version.txt" 2>&1; then
    printf '  ok: bundled binary runs (--version): %s\n' "$(tr -d '\n' <"$REPORT_DIR/version.txt")"
else
    printf '  FAIL: bundled binary could not execute --version\n' >&2
    failures=$((failures + 1))
fi
if "$CONTENTS/MacOS/$APP_NAME" --build-info >"$REPORT_DIR/bundle-build-info.txt" 2>&1; then
    check "bundle reports the Rust backend" \
        grep -qx 'matrix_backend: rust' "$REPORT_DIR/bundle-build-info.txt"
    check "bundle excludes the HTTP backend" \
        grep -qx 'http_backend_compiled: false' "$REPORT_DIR/bundle-build-info.txt"
    check "bundle excludes the mock backend" \
        grep -qx 'mock_backend_compiled: false' "$REPORT_DIR/bundle-build-info.txt"
else
    printf '  FAIL: bundled binary could not execute --build-info\n' >&2
    failures=$((failures + 1))
fi

# --- leak scan ---------------------------------------------------------------
# Same intent as the Windows validator — an artifact must not carry runner
# tokens, provider key VALUES, or the builder's private paths.
#
# Scanning for generic strings like "PRIVATE KEY" does not work here: Qt's TLS
# code contains PEM header literals ("-----BEGIN PRIVATE KEY-----") as ordinary
# format strings, so that pattern matches a perfectly clean bundle. Scanning for
# the variable NAMES is equally meaningless — a name is not a secret. What
# matters is whether a real secret value ended up in the payload, so the actual
# values are scanned when they were supplied.
leak_hits=0
scan_for() {
    local label="$1" pattern="$2"
    if grep -rqa -- "$pattern" "$APP_DIR" 2>/dev/null; then
        printf '  FAIL: bundle contains %s\n' "$label" >&2
        leak_hits=$((leak_hits + 1))
    fi
}
scan_for "a GitLab runner token" 'glrt-'
scan_for "an SSH private key" '-----BEGIN OPENSSH PRIVATE KEY-----'
scan_for "a builder SSH path" "$HOME/.ssh"

# GIF provider keys are a conditional check, not an unconditional one. When the
# project's GIPHY_API_KEY/KLIPY_API_KEY variables are available the build
# *deliberately* compiles them in, exactly like the Windows test path, so the
# picker works without local configuration — finding them is then correct, not a
# leak. The property worth enforcing is the opposite one: a build that reported
# itself keyless must not contain a key. build-info.json records which happened.
#
# Written as full `if` blocks: under `set -e` a trailing `[[ ... ]] && cmd` whose
# test is false makes the whole list return 1 and aborts the script.
GIF_EMBEDDED="$(jq -r '.gif_keys_embedded // false' "$CONTENTS/Resources/build-info.json" 2>/dev/null || echo unknown)"
if [[ "$GIF_EMBEDDED" == "true" ]]; then
    printf '  note: GIF provider keys are intentionally embedded in this build\n'
    printf '        (developer-scoped, expiring artifact — an embedded key is extractable)\n'
else
    if [[ -n "${GIPHY_API_KEY:-}" ]]; then
        scan_for "the Giphy provider key value in a keyless build" "$GIPHY_API_KEY"
    fi
    if [[ -n "${KLIPY_API_KEY:-}" ]]; then
        scan_for "the Klipy provider key value in a keyless build" "$KLIPY_API_KEY"
    fi
fi
if (( leak_hits == 0 )); then
    printf '  ok: no runner tokens, private keys, or builder paths in the bundle\n'
else
    failures=$((failures + leak_hits))
fi

# --- report ------------------------------------------------------------------
BUNDLE_BYTES="$(du -sk "$APP_DIR" | awk '{print $1 * 1024}')"
jq -n \
    --arg bundle_id "$BUNDLE_ID" \
    --arg version "$BUNDLE_VERSION" \
    --arg archs "$ARCHS" \
    --arg gatekeeper "$GATEKEEPER" \
    --argjson frameworks "${qt_fw_count:-0}" \
    --argjson bundle_bytes "$BUNDLE_BYTES" \
    --argjson failures "$failures" \
    '{bundle_identifier:$bundle_id, version:$version, architectures:$archs,
      bundled_frameworks:$frameworks, bundle_bytes:$bundle_bytes,
      signature:"ad-hoc", notarized:false, gatekeeper:$gatekeeper,
      structural_failures:$failures,
      native_macos_acceptance_tested:false}' \
    >"$REPORT_DIR/macos-validation.json"

if (( failures > 0 )); then
    die "macOS bundle validation failed ($failures checks)"
fi
printf 'macOS bundle validation passed (%s, %s bytes)\n' "$ARCHS" "$BUNDLE_BYTES"
