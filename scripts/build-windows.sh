#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
BUILD_DIR="$ROOT/work/windows-build"
WINDOWS_DIST="$ROOT/dist/windows"
STAGE_DIR="$WINDOWS_DIST/Lightning"
REPORT_DIR="$WINDOWS_DIST/reports"

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "Windows packaging requires a full source commit SHA"
[[ "${PUBLISH_PACKAGES:-false}" == "false" ]] || die "Windows test artifacts cannot be built in a publishing pipeline"
[[ -f "$SOURCE_DIR/CMakeLists.txt" && -f "$ROOT/dist/version.env" ]] || \
    die "run prepare-pinned-source.sh before build-windows.sh"
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || die "resolved source SHA changed"
[[ "$BASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid application version"

case "$BUILD_DIR:$WINDOWS_DIST" in
    "$ROOT/work/windows-build:$ROOT/dist/windows") ;;
    *) die "refusing to clean unexpected Windows output paths" ;;
esac
rm -rf -- "$BUILD_DIR" "$WINDOWS_DIST"
mkdir -p "$BUILD_DIR" "$STAGE_DIR" "$REPORT_DIR"

SOURCE_TIME="$(json_value "$ROOT/dist/source-info.json" commit_time)"
SOURCE_DATE_EPOCH="$(date -u -d "$SOURCE_TIME" +%s)"
export SOURCE_DATE_EPOCH
export CARGO_HOME="${CARGO_HOME:-/cache/cargo-windows}"
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$SOURCE_DIR=/usr/src/lightning --remap-path-prefix=$ROOT=/usr/src/lightning-deploy"
mkdir -p "$CARGO_HOME"

icon_inputs=()
for size in 16 32 48 64 128 192; do
    icon="$SOURCE_DIR/data/icons/hicolor/${size}x${size}/apps/lightning.png"
    [[ -f "$icon" ]] || die "source icon is missing: $icon"
    icon_inputs+=("$icon")
done
magick "${icon_inputs[@]}" "$BUILD_DIR/Lightning.ico"

IFS=. read -r version_major version_minor version_patch <<<"$BASE_VERSION"
cat >"$BUILD_DIR/lightning-version.rc" <<EOF
1 ICON "Lightning.ico"
1 VERSIONINFO
FILEVERSION ${version_major},${version_minor},${version_patch},0
PRODUCTVERSION ${version_major},${version_minor},${version_patch},0
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904E4"
    BEGIN
      VALUE "CompanyName", "Mizerd"
      VALUE "FileDescription", "Lightning Matrix client"
      VALUE "FileVersion", "${BASE_VERSION}"
      VALUE "InternalName", "Lightning"
      VALUE "OriginalFilename", "Lightning.exe"
      VALUE "ProductName", "Lightning"
      VALUE "ProductVersion", "${BASE_VERSION}"
      VALUE "Comments", "Unsigned test build from ${SOURCE_SHA:0:7}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0409, 1252
  END
END
EOF
( cd "$BUILD_DIR" && x86_64-w64-mingw32-windres lightning-version.rc lightning-version.o )

# Populate the complete lockfile cache first because project 6 deliberately
# invokes its actual build with --offline --locked.
/opt/rust/cargo/bin/cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"
/opt/rust/cargo/bin/cargo fetch --locked --target x86_64-pc-windows-gnu \
    --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging/windows/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -ffile-prefix-map=$SOURCE_DIR=/usr/src/lightning -ffile-prefix-map=$ROOT=/usr/src/lightning-deploy" \
    -DCMAKE_EXE_LINKER_FLAGS="$BUILD_DIR/lightning-version.o" \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS=OFF
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" --target matrix-client

python3 "$SCRIPT_DIR/stage-windows-runtime.py" \
    --source "$SOURCE_DIR" --build "$BUILD_DIR" --stage "$STAGE_DIR"
cp "$BUILD_DIR/Lightning.ico" "$STAGE_DIR/Lightning.ico"

PACKAGING_SHA="${CI_COMMIT_SHA:-unknown}"
jq -n \
    --arg version "$BASE_VERSION" \
    --arg source_commit "$SOURCE_SHA" \
    --arg packaging_commit "$PACKAGING_SHA" \
    --arg timestamp "$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)" \
    '{version:$version, source_commit:$source_commit,
      packaging_commit:$packaging_commit, target:"x86_64-pc-windows-gnu",
      build_kind:"unsigned-test", runner_kind:"linux-cross",
      qt_version:"6.11.1", rust_version:"1.95.0", build_timestamp:$timestamp,
      native_windows_tested:false}' >"$STAGE_DIR/build-info.json"
cp /usr/local/share/lightning-windows-rpms.txt "$REPORT_DIR/builder-rpms.txt"

short_sha="${SOURCE_SHA:0:7}"
artifact_base="Lightning-${BASE_VERSION}-${short_sha}-windows-x86_64"
portable="$WINDOWS_DIST/${artifact_base}-portable.zip"
( cd "$WINDOWS_DIST" && find Lightning -type f -print0 | LC_ALL=C sort -z \
    | xargs -0 zip -X -q "$portable" )

wxs="$BUILD_DIR/Lightning.wxs"
python3 "$SCRIPT_DIR/generate-windows-wix.py" \
    --stage "$STAGE_DIR" --version "$BASE_VERSION" --source-sha "$SOURCE_SHA" \
    --output "$wxs" --metadata "$REPORT_DIR/msi-identity.json"
msi="$WINDOWS_DIST/${artifact_base}.msi"
wixl --arch x64 --output "$msi" "$wxs" 2>&1 | tee "$REPORT_DIR/wixl.log"

setup="$WINDOWS_DIST/${artifact_base}-setup.exe"
makensis '-XTarget amd64-unicode' \
    "-DPRODUCT_VERSION=$BASE_VERSION" \
    "-DSOURCE_SHORT_SHA=$short_sha" \
    "-DSTAGE_DIR=$STAGE_DIR" \
    "-DOUTPUT_FILE=$setup" \
    "$ROOT/packaging/windows/installer.nsi" \
    2>&1 | tee "$REPORT_DIR/nsis.log"

"$SCRIPT_DIR/validate-windows-artifacts.sh" "$WINDOWS_DIST"
"$SCRIPT_DIR/smoke-windows-wine.sh" "$WINDOWS_DIST"

printf 'Built unsigned Windows test artifacts for Lightning %s (%s)\n' \
    "$BASE_VERSION" "$SOURCE_SHA"
