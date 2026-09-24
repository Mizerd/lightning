#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Authenticode signing hook, a no-op unless WINDOWS_SIGNING_PFX_B64 is supplied
# through protected CI variables; there is no self-signed identity. Needs
# osslsigncode in the builder image. The PFX and password live only in mktemp
# files removed on return, and -readpass keeps the password out of argv.
sign_windows_file() {
    local target="$1"
    if [[ -z "${WINDOWS_SIGNING_PFX_B64:-}" ]]; then
        printf 'Authenticode signing skipped for %s (unsigned test build)\n' "${target##*/}"
        return 0
    fi
    command -v osslsigncode >/dev/null 2>&1 || \
        die "WINDOWS_SIGNING_PFX_B64 is set but osslsigncode is not installed in the builder image"
    local pfx passfile signed
    pfx="$(mktemp)"; passfile="$(mktemp)"; signed="$(mktemp)"
    # shellcheck disable=SC2064
    trap "rm -f '$pfx' '$passfile' '$signed'" RETURN
    printf '%s' "$WINDOWS_SIGNING_PFX_B64" | base64 -d >"$pfx"
    printf '%s' "${WINDOWS_SIGNING_PASSWORD:-}" >"$passfile"
    osslsigncode sign -pkcs12 "$pfx" -readpass "$passfile" \
        -h sha256 -t "${WINDOWS_SIGNING_TIMESTAMP_URL:-https://timestamp.digicert.com}" \
        -in "$target" -out "$signed" >/dev/null
    mv "$signed" "$target"
    printf 'Authenticode-signed %s\n' "${target##*/}"
}

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
BUILD_DIR="$ROOT/work/windows-build"
WINDOWS_DIST="$ROOT/dist/windows"
STAGE_DIR="$WINDOWS_DIST/Lightning"
REPORT_DIR="$WINDOWS_DIST/reports"

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "Windows packaging requires a full source commit SHA"
# Windows runs in both the test path and the publishing pipeline.
PUBLISHING="${PUBLISH_PACKAGES:-false}"
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

# Product metadata for Lightning-owned PEs, generated from the release version
# and checked after staging by verify-windows-metadata.py (SignPath enforces
# file metadata on signed artifacts). The publisher is the maintainer by name;
# "Mizerd" is only a GitLab namespace. The registry path keeps
# Software\Mizerd\Lightning so existing uninstall registrations stay valid.
WIN_PUBLISHER="Rokas Smetonis"
WIN_COPYRIGHT="Copyright (C) 2026 Rokas Smetonis. GPL-3.0-or-later."
WIN_SIGNING_STATE="$(windows_signing_state)"

# Never label a build signed without a signing mechanism: that would put a
# false security claim into the PE metadata, MSI and release assets. The
# reverse only understates, so it warns.
if windows_signed; then
    [[ -n "${WINDOWS_SIGNING_PFX_B64:-}" || "${LIGHTNING_SIGNING_METHOD:-}" == signpath ]] || \
        die "LIGHTNING_WINDOWS_SIGNED=true but no signing mechanism is configured"
elif [[ -n "${WINDOWS_SIGNING_PFX_B64:-}" ]]; then
    printf 'warning: a signing credential is configured but LIGHTNING_WINDOWS_SIGNED is false; artifacts will be labelled unsigned\n' >&2
fi

IFS=. read -r version_major version_minor version_patch <<<"$BASE_VERSION"
# One resource per executable: OriginalFilename must name the file it is in.
# Attached per target through packaging/windows/version-resources.cmake.
write_version_rc() { # $1 = rc path, $2 = OriginalFilename, $3 = InternalName,
                     # $4 = FileDescription, $5 = "icon" to embed the app icon
    local icon_line=""
    [[ "${5:-}" == icon ]] && icon_line='1 ICON "Lightning.ico"'
    cat >"$1" <<EOF
${icon_line}
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
      VALUE "CompanyName", "${WIN_PUBLISHER}"
      VALUE "FileDescription", "$4"
      VALUE "FileVersion", "${BASE_VERSION}"
      VALUE "InternalName", "$3"
      VALUE "LegalCopyright", "${WIN_COPYRIGHT}"
      VALUE "OriginalFilename", "$2"
      VALUE "ProductName", "Lightning"
      VALUE "ProductVersion", "${BASE_VERSION}"
      VALUE "Comments", "Built from Lightning source ${SOURCE_SHA:0:7} (${WIN_SIGNING_STATE})"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0409, 1252
  END
END
EOF
}
write_version_rc "$BUILD_DIR/lightning-version.rc" \
    "Lightning.exe" "Lightning" "Lightning Matrix client" icon
# The helper carries no icon; users never see it.
write_version_rc "$BUILD_DIR/lightning-updater-version.rc" \
    "lightning-updater.exe" "lightning-updater" "Lightning update helper"
( cd "$BUILD_DIR" && x86_64-w64-mingw32-windres lightning-version.rc lightning-version.o )
( cd "$BUILD_DIR" && x86_64-w64-mingw32-windres lightning-updater-version.rc lightning-updater-version.o )

# The source build runs --offline --locked, so fill the cargo cache first.
/opt/rust/cargo/bin/cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"
/opt/rust/cargo/bin/cargo fetch --locked --target x86_64-pc-windows-gnu \
    --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

# GIF provider keys use the same mechanism as every release build: the CMake
# generator reads the build-only LIGHTNING_BUILD_* variables into an untracked
# header, never a command line, cache entry, package or log. Without the CI
# variables the build is keyless. An embedded key is ultimately extractable.
gif_require=OFF
gif_keys_embedded=false
# A publishing pipeline must embed both keys.
if [[ "$PUBLISHING" == true ]]; then
    [[ -n "${GIPHY_API_KEY:-}" ]] || die "official Windows build requires GIPHY_API_KEY"
    [[ -n "${KLIPY_API_KEY:-}" ]] || die "official Windows build requires KLIPY_API_KEY"
fi
if [[ -n "${GIPHY_API_KEY:-}" && -n "${KLIPY_API_KEY:-}" ]]; then
    export LIGHTNING_BUILD_GIPHY_API_KEY="$GIPHY_API_KEY"
    export LIGHTNING_BUILD_KLIPY_API_KEY="$KLIPY_API_KEY"
    gif_require=ON
    gif_keys_embedded=true
    printf 'Embedding GIF provider keys from CI variables (require=ON)\n'
else
    printf 'GIF provider keys not supplied; building keyless\n'
fi

# artifact_kind=release only in a publishing pipeline. Packages are unsigned
# either way unless WINDOWS_SIGNING_PFX_B64 is configured.
if [[ "$PUBLISHING" == true ]]; then
    WIN_ARTIFACT_KIND=release
else
    WIN_ARTIFACT_KIND=unsigned-test
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging-ci/packaging/windows/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -ffile-prefix-map=$SOURCE_DIR=/usr/src/lightning -ffile-prefix-map=$ROOT=/usr/src/lightning-deploy" \
    -DCMAKE_PROJECT_INCLUDE="$ROOT/packaging-ci/packaging/windows/version-resources.cmake" \
    -DLIGHTNING_APP_VERSION_OBJECT="$BUILD_DIR/lightning-version.o" \
    -DLIGHTNING_UPDATER_VERSION_OBJECT="$BUILD_DIR/lightning-updater-version.o" \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON \
    -DLIGHTNING_RUST_ONLY=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS="$gif_require" \
    -DLIGHTNING_SOURCE_SHA="$SOURCE_SHA" \
    -DLIGHTNING_BUILD_TARGET="x86_64-pc-windows-gnu" \
    -DLIGHTNING_ARTIFACT_KIND="$WIN_ARTIFACT_KIND" \
    -DLIGHTNING_INSTALL_TYPE=windows-portable \
    -DLIGHTNING_UPDATE_PUBKEY_2026A="${UPDATE_SIGNING_PUBKEY_2026A:-}"
# Drop the key values now that the generated header exists.
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true
# lightning-matrix is a GUI-subsystem PE (WIN32_EXECUTABLE in the app CMake).
# lightning-updater is a small console helper that performs the install step
# that cannot run while Lightning is running; every package is assembled from
# the staged tree, so it must be built here or no package carries it.
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" \
    --target lightning-matrix lightning-updater

python3 "$SCRIPT_DIR/stage-windows-runtime.py" \
    --source "$SOURCE_DIR" --build "$BUILD_DIR" --stage "$STAGE_DIR"
cp "$BUILD_DIR/Lightning.ico" "$STAGE_DIR/Lightning.ico"

PACKAGING_SHA="${CI_COMMIT_SHA:-unknown}"
jq -n \
    --arg version "$BASE_VERSION" \
    --arg source_commit "$SOURCE_SHA" \
    --arg packaging_commit "$PACKAGING_SHA" \
    --arg timestamp "$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson gif_keys_embedded "$gif_keys_embedded" \
    --arg build_kind "$WIN_ARTIFACT_KIND" \
    --argjson signed "$(windows_signed && echo true || echo false)" \
    '{version:$version, source_commit:$source_commit,
      packaging_commit:$packaging_commit, target:"x86_64-pc-windows-gnu",
      build_kind:$build_kind, runner_kind:"linux-cross", signed:$signed,
      qt_version:"6.11.1", rust_version:"1.95.0", build_timestamp:$timestamp,
      gif_keys_embedded:$gif_keys_embedded, native_windows_tested:false}' \
    >"$STAGE_DIR/build-info.json"
cp /usr/local/share/lightning-windows-rpms.txt "$REPORT_DIR/builder-rpms.txt"

# Metadata gate on the staged tree, before packaging or signing: a mismatched
# Lightning-owned binary, or an upstream DLL claiming to be Lightning, fails the
# build. Also emits the signing inventory.
python3 "$SCRIPT_DIR/verify-windows-metadata.py" \
    --stage "$STAGE_DIR" \
    --inventory "$ROOT/packaging-ci/packaging/windows/signing-inventory.json" \
    --version "$BASE_VERSION" \
    --publisher "$WIN_PUBLISHER" \
    --report "$REPORT_DIR/windows-signing-inventory.json"

# The signing payload: exactly the Lightning-owned PE files and their
# checksums. A signing job submits this directory and writes the signed
# binaries back over the staged copies before packaging.
SIGNING_DIR="$WINDOWS_DIST/signing-payload"
mkdir -p "$SIGNING_DIR"
for owned in Lightning.exe lightning-updater.exe; do
    cp "$STAGE_DIR/$owned" "$SIGNING_DIR/$owned"
    ( cd "$SIGNING_DIR" && sha256sum "$owned" >"$owned.sha256" )
done
cp "$STAGE_DIR/build-info.json" "$SIGNING_DIR/build-info.json"

# Sign the staged executables before they are packaged. The updater replaces
# the application on disk, so it must be signed as well.
sign_windows_file "$STAGE_DIR/Lightning.exe"
sign_windows_file "$STAGE_DIR/lightning-updater.exe"

short_sha="${SOURCE_SHA:0:7}"
artifact_base="Lightning-${BASE_VERSION}-${short_sha}-windows-x86_64"
portable="$WINDOWS_DIST/${artifact_base}-portable.zip"

# --- portable.marker: the ordering below is the mechanism --------------------
#
# All three packages come from this one staged tree. Lightning treats itself as
# portable when "portable.marker" sits beside its executable, checked before the
# first QSettings is built. The file must therefore exist only for the `zip`
# call: absent when wixl and makensis read the stage, present in the ZIP.
# Moving the zip below the MSI/NSIS steps would break portable mode and make
# both installers portable, with no build error. validate-windows-artifacts.sh
# asserts both halves on the final artifacts.
portable_marker="$STAGE_DIR/portable.marker"
cat >"$portable_marker" <<'PORTABLE_MARKER'
This file makes Lightning run in portable mode.

While it sits beside Lightning.exe, Lightning keeps everything it SAVES --
settings, your signed-in Matrix session, the Matrix SDK store and the
end-to-end-encryption state -- in the "data" folder next to this file. None of
that goes to the Windows registry, AppData or Credential Manager.

While it is running it still uses the normal Windows temporary folder for
things it is playing or recording right now -- a video you open, a voice
message you record. Those are deleted when you sign out or close Lightning,
and nothing is read back from them on the next start, so they do not need to
travel with the folder. A crash can leave one behind.

Close Lightning before copying this folder anywhere. Copying it while
Lightning is running can capture a database mid-write.

Anyone who has a copy of this folder has your signed-in Matrix session. Treat
it like a password, and delete it when you are done with it.

Deleting this file makes Lightning behave like an installed copy again; the
data folder is left alone but is no longer read.
PORTABLE_MARKER

( cd "$WINDOWS_DIST" && find Lightning -type f -print0 | LC_ALL=C sort -z \
    | xargs -0 zip -X -q "$portable" )

# `die` rather than a bare rm, so a refused delete cannot ship an installer
# that detects as portable.
rm -f "$portable_marker"
[[ ! -e "$portable_marker" ]] || \
    die "portable.marker survived into the installer stage; MSI/NSIS would detect as portable"

# The compiled-in install type is windows-portable, correct for the ZIP. The
# MSI carries this marker instead, and the NSIS installer writes its own at
# install time. The updater reads it to pick one of its compiled-in strategies.
install_marker="$STAGE_DIR/.lightning-install-type"
printf 'windows-msi\n' >"$install_marker"

wxs="$BUILD_DIR/Lightning.wxs"
python3 "$SCRIPT_DIR/generate-windows-wix.py" \
    --stage "$STAGE_DIR" --version "$BASE_VERSION" --source-sha "$SOURCE_SHA" \
    --manufacturer "$WIN_PUBLISHER" --signing-state "$WIN_SIGNING_STATE" \
    --output "$wxs" --metadata "$REPORT_DIR/msi-identity.json"
msi="$WINDOWS_DIST/${artifact_base}.msi"
wixl --arch x64 --output "$msi" "$wxs" 2>&1 | tee "$REPORT_DIR/wixl.log"
sign_windows_file "$msi"

# The setup EXE writes its own marker, so the MSI's copy must not be in it;
# otherwise the updater would hand a .msi to an NSIS install.
rm -f "$install_marker"

setup="$WINDOWS_DIST/${artifact_base}-setup.exe"
makensis '-XTarget amd64-unicode' \
    "-DPRODUCT_VERSION=$BASE_VERSION" \
    "-DSOURCE_SHORT_SHA=$short_sha" \
    "-DPUBLISHER=$WIN_PUBLISHER" \
    "-DSIGNING_STATE=$WIN_SIGNING_STATE" \
    "-DCOPYRIGHT=$WIN_COPYRIGHT" \
    "-DSTAGE_DIR=$STAGE_DIR" \
    "-DOUTPUT_FILE=$setup" \
    "$ROOT/packaging-ci/packaging/windows/installer.nsi" \
    2>&1 | tee "$REPORT_DIR/nsis.log"
sign_windows_file "$setup"

"$SCRIPT_DIR/validate-windows-artifacts.sh" "$WINDOWS_DIST"
"$SCRIPT_DIR/smoke-windows-wine.sh" "$WINDOWS_DIST"

printf 'Built unsigned Windows test artifacts for Lightning %s (%s)\n' \
    "$BASE_VERSION" "$SOURCE_SHA"
