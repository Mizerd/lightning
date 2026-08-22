#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Native macOS build. Unlike every other target in this project it runs on a
# SHELL executor on real Apple hardware (the M1 Mac mini, 10.195.35.7), not in a
# Docker container: Apple's SDK, frameworks, and codesign cannot be containerised
# and cross-compiling a Qt/QML app to macOS is not supported. Consequences:
#   * the toolchain is installed on the host, not pinned in an image, so this
#     script records the versions it actually used into build-info.json;
#   * the working directory persists between jobs, so every output path is
#     removed explicitly below rather than assumed clean.
#
# As of 0.7.5 this artifact IS published as a download-only release asset. It
# remains unsigned and un-notarized, so Gatekeeper blocks it until the user
# explicitly allows it; the download page carries that walkthrough and states
# the limits. Nothing about the BUILD changed — build_kind stays unsigned-test,
# and this script still uploads nothing. See docs/macos-packaging.md.

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
BUILD_DIR="$ROOT/work/macos-build"
MACOS_DIST="$ROOT/dist/macos"
APP_NAME="Lightning"
APP_DIR="$MACOS_DIST/${APP_NAME}.app"
REPORT_DIR="$MACOS_DIST/reports"

# Reverse-DNS identifier under the maintainer's own domain. It is the identity
# Gatekeeper, TCC (microphone consent), and any future notarization ticket bind
# to, so it must stay stable once artifacts exist in the wild — changing it
# re-prompts every user for permissions and invalidates notarization.
BUNDLE_ID="net.smetonis.lightning"
# Deployment target is DERIVED from the Qt frameworks the app actually links,
# not hardcoded. Homebrew's Qt does not have one floor: qtbase modules (QtCore,
# QtGui, QtNetwork, QtSql, QtWidgets) are built minos 14.0 while qtdeclarative
# and qtmultimedia (QtQml, QtQuick, QtQuickControls2, QtMultimedia) are built
# minos 26.0. Reading only QtCore and hardcoding 14.0 produced a bundle whose
# Info.plist claimed macOS 14 support while linking frameworks that require 26 —
# it would simply fail to load there, and the linker said so:
#   ld: warning: building for macOS-14.0, but linking with dylib
#       '.../QtQuick.framework/...' which was built for newer version 26.0
# Taking the maximum keeps the claim true and self-corrects when Homebrew's Qt
# is rebuilt against a different SDK.
QT_LINKED_MODULES="QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtSql QtWidgets QtMultimedia"

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "macOS packaging requires a full source commit SHA"

# The build kind stays unsigned-test whatever the pipeline, and this script
# still uploads nothing — publication is done by publish-packages on Linux,
# from this job's artifact. What the pipeline guarantees instead is that the
# release never DEPENDS on this host (allow_failure plus an optional need) and
# that the bundle never enters the signed update manifest, both asserted by
# tests/test-pipeline-config.py.
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    printf 'Publishing pipeline: building the macOS bundle.\n'
    printf 'It is ad-hoc signed and un-notarized; it ships as a download that\n'
    printf 'the user must allow past Gatekeeper, and never as an update.\n'
fi

[[ "$(uname -s)" == Darwin ]] || die "build-macos.sh must run on macOS"
[[ "$(uname -m)" == arm64 ]] || die "expected an Apple Silicon (arm64) runner"

[[ -f "$SOURCE_DIR/CMakeLists.txt" && -f "$ROOT/dist/version.env" ]] || \
    die "run prepare-pinned-source.sh before build-macos.sh"
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || die "resolved source SHA changed"
[[ "$BASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid application version"

# --- toolchain ---------------------------------------------------------------
# Discovered, not assumed: the host toolchain is not pinned by an image, so a
# missing or moved tool must fail here with a clear message instead of halfway
# through a 30-minute build.
BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"
QT_PREFIX="${QT_PREFIX:-$BREW_PREFIX/opt/qt}"
# Lightning verifies update-manifest signatures with OpenSSL 3's EVP API and its
# CMake does find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto). macOS ships
# no OpenSSL 3 headers at all (only the legacy LibreSSL-backed libssl stubs),
# and Homebrew keg-onlys openssl@3 — it is deliberately NOT symlinked into
# $BREW_PREFIX, so CMake cannot find it without being told where it is. Same
# discovered-not-assumed idiom as QT_PREFIX above.
OPENSSL_PREFIX="${OPENSSL_PREFIX:-$BREW_PREFIX/opt/openssl@3}"
CARGO_BIN="${CARGO_BIN:-$HOME/.cargo/bin}"
export PATH="$CARGO_BIN:$QT_PREFIX/bin:$BREW_PREFIX/bin:$PATH"

# xcodebuild is deliberately NOT in this list. A Ninja/clang build needs only
# the compiler and an SDK, both of which the Command Line Tools provide, and
# `xcodebuild` fails outright when xcode-select points at a CLT instance rather
# than Xcode.app — which is what Homebrew's installer leaves behind. Requiring
# it would fail a build that is perfectly able to proceed. (Full Xcode IS
# required for the future iOS target; that is a separate check for a separate
# job.)
for tool in cmake ninja cargo rustc macdeployqt xcrun clang iconutil sips codesign ditto zip; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found on the runner: $tool"
done
[[ -d "$QT_PREFIX/lib/cmake/Qt6" ]] || die "Qt 6 not found at $QT_PREFIX"

# `command -v openssl` is NOT the check: it finds Apple's /usr/bin/openssl,
# which is LibreSSL and has no development headers. What the build needs is the
# Homebrew keg's headers and the static libcrypto archive, so assert exactly
# those. `brew install openssl@3` provides them.
[[ -f "$OPENSSL_PREFIX/include/openssl/evp.h" ]] || \
    die "OpenSSL 3 headers not found at $OPENSSL_PREFIX (run: brew install openssl@3)"
[[ -f "$OPENSSL_PREFIX/lib/libcrypto.a" ]] || \
    die "libcrypto.a not found at $OPENSSL_PREFIX/lib (run: brew install openssl@3)"
OPENSSL_VERSION="$("$OPENSSL_PREFIX/bin/openssl" version 2>/dev/null || echo unknown)"

QT_VERSION="$("$QT_PREFIX/bin/qmake" -query QT_VERSION)"

MACOS_DEPLOYMENT_TARGET="$(
    for fw in $QT_LINKED_MODULES; do
        fw_bin="$QT_PREFIX/lib/${fw}.framework/Versions/A/${fw}"
        [ -f "$fw_bin" ] || continue
        otool -l "$fw_bin" 2>/dev/null \
            | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'
    done | sort -V | tail -1
)"
[[ "$MACOS_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+$ ]] || \
    die "could not determine the Qt deployment floor from $QT_PREFIX"

# Applies to cargo/rustc and to the C sources the `cc` crate compiles (sha3,
# aes, jitterentropy, blake3). Without it those objects are built against the
# host SDK while the C++ link targets the Qt floor, which the linker reports as
# "object file was built for newer macOS version than being linked".
export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"

RUST_VERSION="$(rustc --version)"
CMAKE_VERSION="$(cmake --version | head -1)"
MACOS_VERSION="$(sw_vers -productVersion)"

# Record which developer toolchain actually produced the binary rather than
# assuming Xcode. Both are viable here, and build-info.json should not claim an
# Xcode build when the Command Line Tools did the work.
DEVELOPER_DIR_ACTIVE="$(xcode-select -p 2>/dev/null || true)"
XCODE_VERSION="$(xcodebuild -version 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g; s/ $//' || true)"
[[ -n "$XCODE_VERSION" ]] || XCODE_VERSION="Command Line Tools (xcodebuild unavailable)"
SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
SDK_VERSION="$(xcrun --show-sdk-version 2>/dev/null || true)"
[[ -n "$SDK_PATH" ]] || die "no macOS SDK is available (xcrun --show-sdk-path failed)"

printf 'macOS %s | SDK %s | %s | Qt %s | %s | %s | %s\n' \
    "$MACOS_VERSION" "${SDK_VERSION:-unknown}" "$XCODE_VERSION" \
    "$QT_VERSION" "$RUST_VERSION" "$CMAKE_VERSION" "$OPENSSL_VERSION"
printf 'developer dir: %s\n' "${DEVELOPER_DIR_ACTIVE:-unknown}"

# --- clean output paths ------------------------------------------------------
# The shell executor reuses its build directory, so stale output from a previous
# job would otherwise be republished as if this job had produced it. Guarded so
# a mangled variable can never turn this into a destructive rm.
case "$BUILD_DIR:$MACOS_DIST" in
    "$ROOT/work/macos-build:$ROOT/dist/macos") ;;
    *) die "refusing to clean unexpected macOS output paths" ;;
esac
rm -rf -- "$BUILD_DIR" "$MACOS_DIST"
mkdir -p "$BUILD_DIR" "$MACOS_DIST" "$REPORT_DIR"

# Persistent Rust target directory, outside the tree this script deletes.
# Lightning's CMake pins CARGO_TARGET_DIR to <build>/rust, so without this every
# pipeline recompiles the entire Matrix SDK from cold (tens of minutes). Same
# mechanism configure-build.sh uses with the Linux runners' /cache mount, and
# safe for the same reason: cargo's own fingerprints decide what is reusable and
# the Rust tree never sees the GIF provider keys.
#
# GitLab's shell executor cleans ignored files from the build directory between
# jobs, so the cache deliberately lives outside it.
CACHE_ROOT="${LIGHTNING_MACOS_CACHE:-$HOME/Library/Caches/lightning-ci}"
mkdir -p "$CACHE_ROOT/cargo-target"
ln -sfn "$CACHE_ROOT/cargo-target" "$BUILD_DIR/rust"

SOURCE_TIME="$(json_value "$ROOT/dist/source-info.json" commit_time)"
# BSD date cannot parse git's %cI offset (+03:00) because strptime %z wants
# +0300, so normalise before converting. Falling back to "now" would silently
# make the build unreproducible, hence the hard failure.
SOURCE_TIME_NORM="$(printf '%s' "$SOURCE_TIME" \
    | sed -E 's/Z$/+0000/; s/([+-][0-9]{2}):([0-9]{2})$/\1\2/')"
SOURCE_DATE_EPOCH="$(date -u -j -f '%Y-%m-%dT%H:%M:%S%z' "$SOURCE_TIME_NORM" +%s)" \
    || die "could not parse source commit time: $SOURCE_TIME"
export SOURCE_DATE_EPOCH
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$SOURCE_DIR=/usr/src/lightning --remap-path-prefix=$ROOT=/usr/src/lightning-deploy"

# --- GIF provider keys -------------------------------------------------------
# Same mechanism as the Linux and Windows builds: the CMake generator reads the
# build-only LIGHTNING_BUILD_* names and writes an untracked build-tree header.
# Never a command line, cache entry, install rule, or log. Absent keys build a
# keyless client (the GIF picker reports unconfigured), which is the normal case
# for this developer test path.
gif_require=OFF
gif_keys_embedded=false
if [[ -n "${GIPHY_API_KEY:-}" && -n "${KLIPY_API_KEY:-}" ]]; then
    export LIGHTNING_BUILD_GIPHY_API_KEY="$GIPHY_API_KEY"
    export LIGHTNING_BUILD_KLIPY_API_KEY="$KLIPY_API_KEY"
    gif_require=ON
    gif_keys_embedded=true
    printf 'Embedding GIF provider keys from CI variables (require=ON)\n'
else
    printf 'GIF provider keys not supplied; building keyless\n'
fi

# --- build -------------------------------------------------------------------
# Project 6 builds --offline --locked, so the lockfile cache must be complete
# before cmake drives cargo.
cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

# Apple system frameworks the Rust static library needs at link time.
#
# matrix-sdk reaches TLS through rustls, whose platform verifier calls into
# Security.framework (SecTrust*, SecPolicyCreateSSL, SecCertificate*) on Apple
# targets. When cargo links a binary itself it honours the crate's
# `cargo:rustc-link-lib=framework=Security` directive — but here CMake links
# libmatrix_client_rust.a into a C++ executable, so that directive is never
# seen and the link fails with ten undefined _Sec* symbols. Supplying the
# framework here is packaging configuration, not a source patch: the Lightning
# source is unmodified, exactly as the Windows cross-build supplies its own
# linker inputs.
MACOS_LINK_FRAMEWORKS="-framework Security -framework CoreFoundation"

# libcrypto is linked STATICALLY on macOS, on purpose. Homebrew's openssl@3 is
# keg-only, so a dynamically linked bundle would carry an absolute
# /opt/homebrew/opt/openssl@3/... load command; macdeployqt only relocates what
# it walks from the Qt frameworks, and the load-command repair pass below would
# then die because libcrypto.3.dylib was never bundled. Static linking removes
# the deployment question entirely and keeps the .app self-contained, which is
# what every other non-Qt dependency in this bundle already is. (If a future
# Homebrew stops shipping libcrypto.a, drop OPENSSL_USE_STATIC_LIBS and add
# libcrypto to the bundling pass — do not leave the host path in the binary.)
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_EXE_LINKER_FLAGS="$MACOS_LINK_FRAMEWORKS" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -ffile-prefix-map=$SOURCE_DIR=/usr/src/lightning -ffile-prefix-map=$ROOT=/usr/src/lightning-deploy" \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON \
    -DLIGHTNING_RUST_ONLY=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS="$gif_require" \
    -DLIGHTNING_SOURCE_SHA="$SOURCE_SHA" \
    -DLIGHTNING_BUILD_TARGET="aarch64-apple-darwin" \
    -DLIGHTNING_ARTIFACT_KIND=unsigned-test

# The generated header has been written; drop the key values so nothing
# downstream (compile, staging, packaging) can see or persist them.
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true

cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" --target matrix-client

BUILT_BINARY="$BUILD_DIR/matrix-client"
[[ -x "$BUILT_BINARY" ]] || die "matrix-client was not produced at $BUILT_BINARY"

# Fail closed on the Rust-only invariant before anything is bundled, matching
# the Linux path: a macOS build that silently compiled the HTTP or mock backend
# is not the same application.
build_info_text="$("$BUILT_BINARY" --build-info)"
printf '%s\n' "$build_info_text" | tee "$REPORT_DIR/build-info.txt"
printf '%s\n' "$build_info_text" | grep -qx 'matrix_backend: rust' \
    || die "built binary is not Rust-only (matrix_backend != rust)"
printf '%s\n' "$build_info_text" | grep -qx 'http_backend_compiled: false' \
    || die "built binary compiled the HTTP backend"
printf '%s\n' "$build_info_text" | grep -qx 'mock_backend_compiled: false' \
    || die "built binary compiled the mock backend"

# --- assemble the .app bundle ------------------------------------------------
# Lightning's CMake targets Linux/Windows layouts (install(TARGETS) into bin/,
# WIN32_EXECUTABLE on Windows) and never sets MACOSX_BUNDLE, so the bundle is
# assembled here rather than by cmake --install. Keeping it out of the source
# tree means macOS packaging cannot regress the Linux install rules.
CONTENTS="$APP_DIR/Contents"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
cp "$BUILT_BINARY" "$CONTENTS/MacOS/$APP_NAME"
chmod 0755 "$CONTENTS/MacOS/$APP_NAME"

# Icon: build a real .icns from the source's hicolor PNGs. 512@2x (1024px) is
# deliberately omitted rather than upscaled — a blurry synthesized icon looks
# worse in the Dock than letting macOS scale the genuine 512.
ICONSET="$BUILD_DIR/${APP_NAME}.iconset"
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
icon_src() { printf '%s/data/icons/hicolor/%sx%s/apps/lightning.png' "$SOURCE_DIR" "$1" "$1"; }
declare -a icon_map=(
    "16:icon_16x16.png" "32:icon_16x16@2x.png"
    "32:icon_32x32.png" "64:icon_32x32@2x.png"
    "128:icon_128x128.png" "256:icon_128x128@2x.png"
    "256:icon_256x256.png" "512:icon_256x256@2x.png"
    "512:icon_512x512.png"
)
for entry in "${icon_map[@]}"; do
    size="${entry%%:*}"; name="${entry#*:}"
    src="$(icon_src "$size")"
    [[ -f "$src" ]] || die "source icon is missing: $src"
    cp "$src" "$ICONSET/$name"
done
iconutil --convert icns "$ICONSET" --output "$CONTENTS/Resources/${APP_NAME}.icns"

# Info.plist. NSMicrophoneUsageDescription is REQUIRED, not decorative: the
# client records voice messages through QMediaCaptureSession/QAudioInput, and
# macOS terminates any process that touches the microphone without a usage
# string. Omitting it would crash the app the first time a user holds the
# record button.
cat >"$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>${APP_NAME}</string>
    <key>CFBundleExecutable</key><string>${APP_NAME}</string>
    <key>CFBundleIconFile</key><string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>${APP_NAME}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${BASE_VERSION}</string>
    <key>CFBundleVersion</key><string>${BASE_VERSION}</string>
    <key>LSMinimumSystemVersion</key><string>${MACOS_DEPLOYMENT_TARGET}</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.social-networking</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>Copyright (C) 2026 Rokas Smetonis. GPL-3.0-or-later.</string>
    <key>NSMicrophoneUsageDescription</key><string>Lightning needs the microphone to record voice messages.</string>
    <key>NSCameraUsageDescription</key><string>Lightning needs the camera only if you attach a photo or video you capture.</string>
</dict>
</plist>
PLIST
printf 'APPL????' >"$CONTENTS/PkgInfo"

# --- bundle the Qt runtime ---------------------------------------------------
# macdeployqt copies the frameworks and the QML modules the app imports, then
# rewrites the Mach-O load commands to @executable_path/../Frameworks. -qmldir
# is required: Lightning's own QML is compiled into the binary as resources, so
# without a source scan macdeployqt cannot see which Qt QML modules (QtQuick,
# QtQuick.Controls, QtMultimedia, ...) the app actually imports and would ship a
# bundle that fails at first window.
# The console output is filtered; the log file is not. macdeployqt emits a pair
# of lines per unresolvable dependency —
#   ERROR: Cannot resolve rpath "@rpath/QtVirtualKeyboard.framework/..."
#   ERROR:  using QList("/opt/homebrew/opt/qtdeclarative/lib", ...)
# — roughly 40 of them, for modules this script prunes immediately afterwards
# and for dylibs (libwebp, libsharpyuv, libbrotlicommon) that it resolves on a
# later pass anyway. They are printed during deployment, so pruning cannot
# prevent them; they can only be filtered. Every line still lands verbatim in
# reports/macdeployqt.log, and the count is reported below, so nothing is
# hidden — only moved out of the way. Any OTHER macdeployqt error still prints.
macdeployqt "$APP_DIR" \
    -qmldir="$SOURCE_DIR" \
    -verbose=1 2>&1 \
    | tee "$REPORT_DIR/macdeployqt.log" \
    | { grep -vE '^ERROR: +(Cannot resolve rpath|using QList)' || true; }

rpath_noise="$(grep -c 'Cannot resolve rpath' "$REPORT_DIR/macdeployqt.log" || true)"
printf 'macdeployqt: %s unresolvable-rpath lines filtered from the console (see reports/macdeployqt.log)\n' \
    "$rpath_noise"

# --- prune modules the app does not import -----------------------------------
# macdeployqt deploys every plugin in its default categories, not only the ones
# this app can reach. On this Qt that pulls in a virtual-keyboard input context
# and, through it, the VirtualKeyboard/Timeline/StateMachine/Pdf QML modules.
# Lightning imports none of them (its only imports are QtQuick, QtQuick.Controls
# [.Basic], QtQuick.Dialogs, QtQuick.Effects, QtQuick.Layouts, QtQuick.Window,
# QtMultimedia and its own MatrixClient module).
#
# They do not merely waste space, they arrive broken. Homebrew's Qt frameworks
# carry rpaths naming the per-module prefixes (.../opt/qtbase/lib,
# .../opt/qtdeclarative/lib) and there is no such prefix for qt3d, qtscxml or
# qtpdf, so macdeployqt cannot resolve their frameworks and emits a long wall of
#   ERROR: Cannot resolve rpath "@rpath/QtVirtualKeyboard.framework/..."
# then copies the QML module and its plugin in anyway, without the framework it
# needs. Removing them deletes dead payload and the noise it generates. Anything
# genuinely required would fail the validation run at the end of this script.
for dead in \
    "$CONTENTS/PlugIns/platforminputcontexts" \
    "$CONTENTS/Resources/qml/QtQuick/VirtualKeyboard" \
    "$CONTENTS/Resources/qml/QtQuick/Timeline" \
    "$CONTENTS/Resources/qml/QtQuick/Pdf" \
    "$CONTENTS/Resources/qml/QtQml/StateMachine"
do
    [[ -e "$dead" ]] || continue
    rm -rf -- "$dead"
    printf 'pruned unused module: %s\n' "${dead#"$APP_DIR"/}"
done

# --- repair load commands macdeployqt left pointing at the host ---------------
# macdeployqt drives install_name_tool through QProcess and does not always wait
# for it. Its own log carries
#   QProcess: Destroyed while process ("install_name_tool") is still running.
# and when that race bites, a binary keeps an absolute /opt/homebrew dependency.
# That bundle only runs on this machine, and it is intermittent: the same commit
# produced a clean bundle in pipeline 90 and one broken plugin
# (PlugIns/quick/libqtquicktemplates2plugin.dylib -> QtNetwork) in pipeline 91.
#
# So do not trust macdeployqt to have finished. Sweep every Mach-O and rewrite
# any remaining host reference to the copy already inside the bundle. A host
# dependency whose framework is NOT bundled is a real missing dependency and
# stops the build rather than shipping something that cannot run elsewhere.
repaired=0
while IFS= read -r macho; do
    install_name="$(otool -D "$macho" 2>/dev/null | sed -n '2p')"
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        # The install name (LC_ID_DYLIB) is the binary's own identity, not a
        # dependency; macdeployqt leaves it as-is and that is harmless.
        [[ "$dep" == "$install_name" ]] && continue

        case "$dep" in
            "$QT_PREFIX"/*|/opt/homebrew/*|/usr/local/*) ;;
            *) continue ;;
        esac

        # .../SomeFramework.framework/Versions/A/SomeFramework  or  .../libfoo.1.dylib
        if [[ "$dep" == *.framework/* ]]; then
            fw="${dep##*/}"
            rel="Frameworks/${fw}.framework/Versions/A/${fw}"
        else
            rel="Frameworks/${dep##*/}"
        fi

        [[ -f "$CONTENTS/$rel" ]] || \
            die "bundled $(basename "$macho") needs $dep, which macdeployqt did not bundle"

        install_name_tool -change "$dep" "@executable_path/../$rel" "$macho"
        printf 'repaired %s: %s\n' "${macho#"$APP_DIR"/}" "$dep"
        repaired=$((repaired + 1))
    done < <(otool -L "$macho" 2>/dev/null | tail -n +2 | awk '{print $1}')
done < <(find "$APP_DIR" -type f \( -perm -u+x -o -name '*.dylib' \) -exec sh -c \
    'file -b "$1" | grep -q "Mach-O" && printf "%s\n" "$1"' _ {} \;)

printf 'load-command repair pass: %d rewritten\n' "$repaired"

# --- metadata ----------------------------------------------------------------
# Written BEFORE signing, deliberately. build-info.json lives inside
# Contents/Resources, so adding it after the bundle is sealed invalidates the
# signature ("code or signature have been modified"). Nothing may modify the
# bundle between codesign and ditto.
PACKAGING_SHA="${CI_COMMIT_SHA:-unknown}"
jq -n \
    --arg version "$BASE_VERSION" \
    --arg source_commit "$SOURCE_SHA" \
    --arg packaging_commit "$PACKAGING_SHA" \
    --arg bundle_id "$BUNDLE_ID" \
    --arg timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --arg qt "$QT_VERSION" \
    --arg rust "$RUST_VERSION" \
    --arg cmake "$CMAKE_VERSION" \
    --arg xcode "$XCODE_VERSION" \
    --arg sdk "${SDK_VERSION:-unknown}" \
    --arg developer_dir "${DEVELOPER_DIR_ACTIVE:-unknown}" \
    --arg macos "$MACOS_VERSION" \
    --arg deployment_target "$MACOS_DEPLOYMENT_TARGET" \
    --argjson gif_keys_embedded "$gif_keys_embedded" \
    '{version:$version, source_commit:$source_commit,
      packaging_commit:$packaging_commit, target:"aarch64-apple-darwin",
      bundle_identifier:$bundle_id, build_kind:"unsigned-test",
      runner_kind:"native-macos-shell", architectures:["arm64"],
      universal_binary:false, signed:false, signature:"ad-hoc",
      notarized:false, gatekeeper_accepted:false,
      deployment_target:$deployment_target,
      toolchain:{qt:$qt, rust:$rust, cmake:$cmake, xcode:$xcode,
                 macos_sdk:$sdk, developer_dir:$developer_dir, macos:$macos},
      build_timestamp:$timestamp, gif_keys_embedded:$gif_keys_embedded,
      native_macos_tested:false}' \
    >"$CONTENTS/Resources/build-info.json"

# --- signature ---------------------------------------------------------------
# Ad-hoc (-) signature, NOT a Developer ID identity. On Apple Silicon every
# executable page must carry a valid signature or the kernel refuses to run the
# process, and macdeployqt's install_name_tool rewrites invalidate the signatures
# Homebrew's dylibs shipped with — so this re-sign is what makes the bundle
# runnable at all. It does NOT make it distributable: Gatekeeper still blocks it
# on other machines because it is neither Developer ID signed nor notarized.
#
# Signed inside-out, deepest first, rather than with `codesign --deep`. --deep is
# deprecated and does not reliably re-sign every nested Mach-O that macdeployqt
# rewrote — it left libbrotlicommon.1.dylib with a stale signature, which then
# failed verification as "code or signature have been modified". Signing each
# nested binary explicitly and the bundle last is the supported order.
: >"$REPORT_DIR/codesign.log"
nested_signed=0
while IFS= read -r macho; do
    [[ "$macho" == "$CONTENTS/MacOS/$APP_NAME" ]] && continue
    file -b "$macho" 2>/dev/null | grep -q 'Mach-O' || continue
    codesign --force --sign - --timestamp=none "$macho" >>"$REPORT_DIR/codesign.log" 2>&1 \
        || die "failed to sign nested binary: ${macho#$APP_DIR/}"
    nested_signed=$((nested_signed + 1))
done < <(find "$APP_DIR" -type f)
printf 'ad-hoc signed %d nested binaries\n' "$nested_signed"

# Framework bundles carry their own bundle signature; sign them after their
# payloads and before the app that contains them.
while IFS= read -r fw; do
    codesign --force --sign - --timestamp=none "$fw" >>"$REPORT_DIR/codesign.log" 2>&1 \
        || die "failed to sign framework: ${fw#$APP_DIR/}"
done < <(find "$CONTENTS/Frameworks" -maxdepth 1 -type d -name '*.framework' 2>/dev/null)

# The app bundle last, so its seal covers the already-signed contents.
codesign --force --sign - --timestamp=none "$APP_DIR" >>"$REPORT_DIR/codesign.log" 2>&1 \
    || die "failed to sign the app bundle"
codesign --verify --deep --strict --verbose=2 "$APP_DIR" >>"$REPORT_DIR/codesign.log" 2>&1 \
    || die "the signed bundle does not verify — see reports/codesign.log"
printf 'bundle signature verifies (ad-hoc)\n'

# Copy for the artifact browser. This one lives outside the .app, so writing it
# after signing is safe.
cp "$CONTENTS/Resources/build-info.json" "$MACOS_DIST/build-info.json"

# --- validate then package ---------------------------------------------------
# Validation runs on the finished bundle BEFORE it is zipped, so a broken
# bundle cannot become a downloadable artifact.
"$SCRIPT_DIR/validate-macos-artifacts.sh" "$MACOS_DIST"

short_sha="${SOURCE_SHA:0:7}"
artifact_base="Lightning-${BASE_VERSION}-${short_sha}-macos-arm64"
archive="$MACOS_DIST/${artifact_base}.zip"
# ditto preserves symlinks, resource forks, and the code signature; a plain
# `zip -r` corrupts framework symlink layout and breaks the signature.
( cd "$MACOS_DIST" && ditto -c -k --sequesterRsrc --keepParent \
    "${APP_NAME}.app" "$(basename "$archive")" )
write_sha256 "$archive"

printf 'Built unsigned macOS test bundle for Lightning %s (%s)\n' "$BASE_VERSION" "$SOURCE_SHA"
printf 'Artifact: %s\n' "$archive"
