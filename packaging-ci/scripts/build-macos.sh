#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Native macOS build. It runs on a shell executor on Apple hardware, not in a
# container: Apple's SDK and codesign cannot be containerised. So the toolchain
# is whatever the host has (recorded in build-info.json), and the working
# directory persists between jobs (every output path is removed explicitly).
#
# The artifact is published as a download-only asset: ad-hoc signed and
# un-notarized. This script uploads nothing. See docs/macos-packaging.md.

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
BUILD_DIR="$ROOT/work/macos-build"
MACOS_DIST="$ROOT/dist/macos"
APP_NAME="Lightning"
APP_DIR="$MACOS_DIST/${APP_NAME}.app"
REPORT_DIR="$MACOS_DIST/reports"

# Gatekeeper, TCC consent and notarization bind to this identifier; changing it
# re-prompts every user for permissions.
BUNDLE_ID="net.smetonis.lightning"
# The deployment target is the highest minos among the Qt frameworks the app
# links. Homebrew's Qt has no single floor (qtbase is 14.0, qtdeclarative and
# qtmultimedia are 26.0), so reading QtCore alone would claim a floor the
# bundle cannot load on.
QT_LINKED_MODULES="QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtSql QtWidgets QtMultimedia"

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "macOS packaging requires a full source commit SHA"

# The release never depends on this host (allow_failure plus an optional need)
# and the bundle never enters the signed update manifest; both are asserted by
# tests/test-pipeline-config.py. Publication happens on Linux from this job's
# artifact.
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
# The host toolchain is not pinned by an image, so fail early on a missing tool.
BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"
QT_PREFIX="${QT_PREFIX:-$BREW_PREFIX/opt/qt}"
# Lightning needs OpenSSL 3 headers. macOS ships none, and Homebrew's openssl@3
# is keg-only, so CMake must be pointed at it.
OPENSSL_PREFIX="${OPENSSL_PREFIX:-$BREW_PREFIX/opt/openssl@3}"
CARGO_BIN="${CARGO_BIN:-$HOME/.cargo/bin}"
# GStreamer comes from the official relocatable framework in the runner's home,
# not Homebrew: see install-macos-gstreamer.sh for why.
GSTREAMER_PREFIX="${LIGHTNING_MACOS_GSTREAMER_PREFIX:-$HOME/opt/gstreamer/GStreamer.framework/Versions/1.0}"
export PATH="$CARGO_BIN:$QT_PREFIX/bin:$BREW_PREFIX/bin:$PATH"

# xcodebuild is not required: the Command Line Tools provide the compiler and
# SDK, and xcodebuild fails when xcode-select points at a CLT install.
for tool in cmake ninja cargo rustc macdeployqt xcrun clang iconutil sips codesign ditto zip \
            pkg-config lipo install_name_tool otool; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found on the runner: $tool"
done
[[ -d "$QT_PREFIX/lib/cmake/Qt6" ]] || die "Qt 6 not found at $QT_PREFIX"

# The media engine is configured out silently when any of these six modules is
# missing, producing a bundle that refuses every call. Fail instead.
#
# The .pc files are relocatable, so a search path is the whole configuration.
# It is passed per command rather than exported so that crates probing
# pkg-config during the cargo build (aws-lc-sys, libsqlite3-sys) cannot pick
# up the framework's modules.
GST_PKG_CONFIG_PATH="$GSTREAMER_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ -f "$GSTREAMER_PREFIX/lib/pkgconfig/gstreamer-webrtc-1.0.pc" ]] || \
    die "GStreamer not found at $GSTREAMER_PREFIX (run scripts/install-macos-gstreamer.sh)"
for gst_module in gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
                  gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0; do
    PKG_CONFIG_PATH="$GST_PKG_CONFIG_PATH" pkg-config --exists "$gst_module" || \
        die "pkg-config cannot resolve $gst_module from $GSTREAMER_PREFIX"
done
GSTREAMER_VERSION="$(PKG_CONFIG_PATH="$GST_PKG_CONFIG_PATH" pkg-config --modversion gstreamer-1.0)"

# `command -v openssl` finds Apple's LibreSSL, which has no headers. Check for
# the keg's headers and static libcrypto instead.
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

# Also applies to the C sources the `cc` crate compiles; without it the linker
# warns that objects were built for a newer macOS than the target.
export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"

RUST_VERSION="$(rustc --version)"
CMAKE_VERSION="$(cmake --version | head -1)"
MACOS_VERSION="$(sw_vers -productVersion)"

# Record which developer toolchain (Xcode or Command Line Tools) was used.
DEVELOPER_DIR_ACTIVE="$(xcode-select -p 2>/dev/null || true)"
XCODE_VERSION="$(xcodebuild -version 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g; s/ $//' || true)"
[[ -n "$XCODE_VERSION" ]] || XCODE_VERSION="Command Line Tools (xcodebuild unavailable)"
SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
SDK_VERSION="$(xcrun --show-sdk-version 2>/dev/null || true)"
[[ -n "$SDK_PATH" ]] || die "no macOS SDK is available (xcrun --show-sdk-path failed)"

printf 'macOS %s | SDK %s | %s | Qt %s | %s | %s | %s | GStreamer %s\n' \
    "$MACOS_VERSION" "${SDK_VERSION:-unknown}" "$XCODE_VERSION" \
    "$QT_VERSION" "$RUST_VERSION" "$CMAKE_VERSION" "$OPENSSL_VERSION" \
    "$GSTREAMER_VERSION"
printf 'developer dir: %s\n' "${DEVELOPER_DIR_ACTIVE:-unknown}"

# --- clean output paths ------------------------------------------------------
# The shell executor reuses its build directory, so remove stale output. The
# case guard keeps a mangled variable from reaching rm -rf.
case "$BUILD_DIR:$MACOS_DIST" in
    "$ROOT/work/macos-build:$ROOT/dist/macos") ;;
    *) die "refusing to clean unexpected macOS output paths" ;;
esac
rm -rf -- "$BUILD_DIR" "$MACOS_DIST"
mkdir -p "$BUILD_DIR" "$MACOS_DIST" "$REPORT_DIR"

# Persistent Rust target directory outside the cleaned tree (and outside the
# build directory, which the shell executor cleans between jobs), so the Matrix
# SDK is not rebuilt from cold every pipeline. Cargo's fingerprints decide
# reuse, and the Rust tree never sees the GIF provider keys.
CACHE_ROOT="${LIGHTNING_MACOS_CACHE:-$HOME/Library/Caches/lightning-ci}"
mkdir -p "$CACHE_ROOT/cargo-target"
ln -sfn "$CACHE_ROOT/cargo-target" "$BUILD_DIR/rust"

SOURCE_TIME="$(json_value "$ROOT/dist/source-info.json" commit_time)"
# BSD date needs +0300 rather than git's +03:00. No fallback to "now": that
# would silently make the build unreproducible.
SOURCE_TIME_NORM="$(printf '%s' "$SOURCE_TIME" \
    | sed -E 's/Z$/+0000/; s/([+-][0-9]{2}):([0-9]{2})$/\1\2/')"
SOURCE_DATE_EPOCH="$(date -u -j -f '%Y-%m-%dT%H:%M:%S%z' "$SOURCE_TIME_NORM" +%s)" \
    || die "could not parse source commit time: $SOURCE_TIME"
export SOURCE_DATE_EPOCH
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$SOURCE_DIR=/usr/src/lightning --remap-path-prefix=$ROOT=/usr/src/lightning-deploy"

# --- GIF provider keys -------------------------------------------------------
# The CMake generator reads the build-only LIGHTNING_BUILD_* variables into an
# untracked header; keys never reach a command line, cache entry or log.
# Without keys the client builds keyless.
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
# The source build is --offline --locked, so fill the cargo cache first.
cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

# Security.framework and CoreFoundation: rustls' platform verifier needs them,
# and the crate's link directive is lost because CMake, not cargo, links the
# Rust static library.
#
# -headerpad_max_install_names is required: stage-macos-gstreamer.sh rewrites
# the executable's GStreamer install names to longer paths, which do not fit
# in the default Mach-O header pad.
MACOS_LINK_FRAMEWORKS="-framework Security -framework CoreFoundation -Wl,-headerpad_max_install_names"

# libcrypto is linked statically: the keg-only openssl@3 would otherwise leave
# an absolute /opt/homebrew load command that macdeployqt does not relocate.
# If Homebrew stops shipping libcrypto.a, bundle the dylib instead; never leave
# the host path in the binary. PKG_CONFIG_PATH is scoped to this command.
PKG_CONFIG_PATH="$GST_PKG_CONFIG_PATH" \
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
    -DLIGHTNING_REQUIRE_QT_SVG=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS="$gif_require" \
    -DLIGHTNING_SOURCE_SHA="$SOURCE_SHA" \
    -DLIGHTNING_BUILD_TARGET="aarch64-apple-darwin" \
    -DLIGHTNING_ARTIFACT_KIND=unsigned-test

# Drop the key values now that the generated header exists.
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true

cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" --target lightning-matrix

BUILT_BINARY="$BUILD_DIR/lightning-matrix"
[[ -x "$BUILT_BINARY" ]] || die "lightning-matrix was not produced at $BUILT_BINARY"

# A macOS build that compiled the HTTP or mock backend is not the same app.
build_info_text="$("$BUILT_BINARY" --build-info)"
printf '%s\n' "$build_info_text" | tee "$REPORT_DIR/build-info.txt"
printf '%s\n' "$build_info_text" | grep -qx 'matrix_backend: rust' \
    || die "built binary is not Rust-only (matrix_backend != rust)"
printf '%s\n' "$build_info_text" | grep -qx 'http_backend_compiled: false' \
    || die "built binary compiled the HTTP backend"
printf '%s\n' "$build_info_text" | grep -qx 'mock_backend_compiled: false' \
    || die "built binary compiled the mock backend"

# The media engine's CMake probe fails silently and --build-info does not
# report it, so check that the binary links GStreamer.
otool -L "$BUILT_BINARY" | grep -q 'libgstreamer-1\.0' \
    || die "the built binary does not link GStreamer — the media engine was not compiled"

# --- assemble the .app bundle ------------------------------------------------
# Lightning's CMake never sets MACOSX_BUNDLE, so the bundle is assembled here.
CONTENTS="$APP_DIR/Contents"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
cp "$BUILT_BINARY" "$CONTENTS/MacOS/$APP_NAME"
chmod 0755 "$CONTENTS/MacOS/$APP_NAME"

# 512@2x is omitted rather than upscaled; macOS scales the real 512 better.
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

# NSMicrophoneUsageDescription is required: macOS terminates a process that
# opens the microphone without it.
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
# -qmldir is required because Lightning's QML is compiled in as resources, so
# macdeployqt cannot otherwise see which Qt QML modules it imports.
# The console output drops the ~40 "Cannot resolve rpath" / "using QList"
# pairs for modules pruned below; the full log is kept in
# reports/macdeployqt.log and any other error still prints.
macdeployqt "$APP_DIR" \
    -qmldir="$SOURCE_DIR" \
    -verbose=1 2>&1 \
    | tee "$REPORT_DIR/macdeployqt.log" \
    | { grep -vE '^ERROR: +(Cannot resolve rpath|using QList)' || true; }

rpath_noise="$(grep -c 'Cannot resolve rpath' "$REPORT_DIR/macdeployqt.log" || true)"
printf 'macdeployqt: %s unresolvable-rpath lines filtered from the console (see reports/macdeployqt.log)\n' \
    "$rpath_noise"

# --- prune modules the app does not import -----------------------------------
# macdeployqt deploys every plugin in its default categories, including a
# virtual-keyboard input context and the QML modules it drags in. Lightning
# imports none of them, and they arrive without their frameworks (Homebrew
# rpaths name per-module prefixes that do not exist for them). Anything
# genuinely required would fail validation at the end of this script.
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

# macdeployqt deploys the SVG image-format plugin once QtSvg is linked (for
# send-side SVG thumbnails). Nothing received may be decoded as SVG, so it
# never ships. The SVG icon engine stays: QIcon reads only the app's icons.
for svg_plugin in "$CONTENTS"/PlugIns/imageformats/*svg*; do
    [[ -e "$svg_plugin" ]] || continue
    rm -f -- "$svg_plugin"
    printf 'pruned (SVG is never decoded): %s\n' "${svg_plugin#"$APP_DIR"/}"
done

# --- repair load commands macdeployqt left pointing at the host ---------------
# macdeployqt does not always wait for install_name_tool ("QProcess: Destroyed
# while process is still running"), which intermittently leaves an absolute
# /opt/homebrew dependency behind. Rewrite any remaining host reference to the
# bundled copy; a host dependency that is not bundled stops the build.
repaired=0
while IFS= read -r macho; do
    install_name="$(otool -D "$macho" 2>/dev/null | sed -n '2p')"
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        # LC_ID_DYLIB is the binary's own identity, not a dependency.
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

# --- bundle the GStreamer runtime the call engine dlopens ---------------------
# After macdeployqt and the repair pass: otherwise macdeployqt would deploy
# GStreamer's glib over the copy Qt needs. The staging script owns its own
# install names.
"$SCRIPT_DIR/stage-macos-gstreamer.sh" "$APP_DIR" "$GSTREAMER_PREFIX"

# --- metadata ----------------------------------------------------------------
# Written before signing: Contents/Resources is sealed, so nothing may modify
# the bundle between codesign and ditto.
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
    --arg gstreamer "$GSTREAMER_VERSION" \
    --argjson gif_keys_embedded "$gif_keys_embedded" \
    '{version:$version, source_commit:$source_commit,
      packaging_commit:$packaging_commit, target:"aarch64-apple-darwin",
      bundle_identifier:$bundle_id, build_kind:"unsigned-test",
      runner_kind:"native-macos-shell", architectures:["arm64"],
      universal_binary:false, signed:false, signature:"ad-hoc",
      notarized:false, gatekeeper_accepted:false,
      deployment_target:$deployment_target,
      toolchain:{qt:$qt, rust:$rust, cmake:$cmake, xcode:$xcode,
                 macos_sdk:$sdk, developer_dir:$developer_dir, macos:$macos,
                 gstreamer:$gstreamer},
      call_media_engine:true, call_media_backend:"gstreamer",
      build_timestamp:$timestamp, gif_keys_embedded:$gif_keys_embedded,
      native_macos_tested:false, calls_live_tested:false}' \
    >"$CONTENTS/Resources/build-info.json"

# --- licences ----------------------------------------------------------------
# Lightning is GPL-3.0-or-later and must ship its licence text; the bundle
# also stages gst-plugins-good. Licensing for the rest of the bundled stack is
# tracked in docs/open-items.md. Staged before signing (Contents/Resources is
# sealed).
LICENSE_DIR="$CONTENTS/Resources/licenses"
mkdir -p "$LICENSE_DIR"
[[ -f "$SOURCE_DIR/LICENSE" ]] \
    || die "the source tree has no LICENSE: the bundle cannot ship Lightning's own GPL-3 text"
install -m 0644 "$SOURCE_DIR/LICENSE" "$LICENSE_DIR/Lightning-GPL-3.0.txt"

# gst-plugins-good text is vendored in this repository; see PROVENANCE.txt
# there. The path is under packaging-ci/: project_dir() is the repository root,
# where a packaging/ directory also exists without these files.
GOOD_LICENSE_SRC="$ROOT/packaging-ci/packaging/common/licenses/gst-plugins-good-1.0"
[[ -f "$GOOD_LICENSE_SRC/COPYING" ]] \
    || die "the vendored gst-plugins-good licence is missing at $GOOD_LICENSE_SRC: the bundle stages its binaries and must carry its licence"
mkdir -p "$LICENSE_DIR/lightning-gstreamer/gst-plugins-good-1.0"
install -m 0644 "$GOOD_LICENSE_SRC/COPYING" \
    "$LICENSE_DIR/lightning-gstreamer/gst-plugins-good-1.0/COPYING"
install -m 0644 "$GOOD_LICENSE_SRC/PROVENANCE.txt" \
    "$LICENSE_DIR/lightning-gstreamer/gst-plugins-good-1.0/PROVENANCE.txt"

# Whatever licence text the upstream framework ships. Reported, not asserted.
gst_license_files=0
while IFS= read -r f; do
    rel="${f#"$GSTREAMER_PREFIX"/}"
    mkdir -p "$LICENSE_DIR/gstreamer-framework/$(dirname "$rel")"
    install -m 0644 "$f" "$LICENSE_DIR/gstreamer-framework/$rel"
    gst_license_files=$((gst_license_files + 1))
done < <(find "$GSTREAMER_PREFIX/share" \
    \( -iname 'COPYING*' -o -iname 'LICENSE*' -o -iname 'LICENCE*' \) \
    -type f 2>/dev/null)
printf 'licences: staged Lightning GPL-3, vendored gst-plugins-good, and %s file(s) found in the GStreamer framework prefix\n' \
    "$gst_license_files"

# --- signature ---------------------------------------------------------------
# Ad-hoc signature: on Apple Silicon every executable page must be signed, and
# install_name_tool rewrites invalidate Homebrew's signatures. It does not make
# the bundle pass Gatekeeper.
#
# Signed inside-out rather than with the deprecated --deep, which left some
# rewritten dylibs with stale signatures.
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

# Frameworks after their payloads, before the app that contains them.
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

# Outside the .app, so writing it after signing is safe.
cp "$CONTENTS/Resources/build-info.json" "$MACOS_DIST/build-info.json"

# --- validate then package ---------------------------------------------------
# Validate before zipping so a broken bundle never becomes an artifact. The
# validator probes the bundled plugins with the SDK's gst-inspect-1.0.
export LIGHTNING_MACOS_GSTREAMER_PREFIX="$GSTREAMER_PREFIX"
"$SCRIPT_DIR/validate-macos-artifacts.sh" "$MACOS_DIST"

short_sha="${SOURCE_SHA:0:7}"
artifact_base="Lightning-${BASE_VERSION}-${short_sha}-macos-arm64"
archive="$MACOS_DIST/${artifact_base}.zip"
# ditto preserves framework symlinks and the signature; zip -r does not.
( cd "$MACOS_DIST" && ditto -c -k --sequesterRsrc --keepParent \
    "${APP_NAME}.app" "$(basename "$archive")" )
write_sha256 "$archive"

printf 'Built unsigned macOS test bundle for Lightning %s (%s)\n' "$BASE_VERSION" "$SOURCE_SHA"
printf 'Artifact: %s\n' "$archive"
