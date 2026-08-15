#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Authenticode signing hook — DISABLED unless a real signing credential is
# supplied via protected CI variables. There is no fake or self-signed identity.
# When WINDOWS_SIGNING_PFX_B64 is unset (every test pipeline), this is a no-op
# and artifacts stay honestly unsigned. Enabling it also needs osslsigncode in
# the builder image. Signing material is never printed, logged, committed, or
# written into a job artifact; the decoded PFX and password live in mktemp files
# removed immediately after use, and the password is passed via -readpass (not
# argv) so it never appears in the process list.
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
        -h sha256 -t "${WINDOWS_SIGNING_TIMESTAMP_URL:-http://timestamp.digicert.com}" \
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
# Windows is a first-class release target since 0.6.3: it may run in both the
# developer test path (PUBLISH_PACKAGES=false) and the publishing pipeline.
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

# Product metadata for the ONE Lightning-owned PE. SignPath enforces file
# metadata restrictions on signed artifacts, so these values are generated from
# the single canonical release version and verified after staging by
# verify-windows-metadata.py — they cannot drift from the release or from each
# other. The publisher is the maintainer by name: there is no company called
# "Mizerd" (that is a GitLab namespace), and a publisher field a user reads must
# be factual. The internal registry path keeps its historical
# Software\Mizerd\Lightning location on purpose — changing it would orphan the
# uninstall registration of already-installed copies.
WIN_PUBLISHER="Rokas Smetonis"
WIN_COPYRIGHT="Copyright (C) 2026 Rokas Smetonis. GPL-3.0-or-later."
WIN_SIGNING_STATE="$(windows_signing_state)"

# The label and the reality must not diverge. Declaring a build "signed" while
# no signing mechanism exists would stamp that claim into the PE metadata, the
# MSI, the asset names, and the release page — a false statement about a
# security property, which is worse than being plainly unsigned. The opposite
# direction (signing configured, label still unsigned) only understates, so it
# warns.
if windows_signed; then
    [[ -n "${WINDOWS_SIGNING_PFX_B64:-}" || "${LIGHTNING_SIGNING_METHOD:-}" == signpath ]] || \
        die "LIGHTNING_WINDOWS_SIGNED=true but no signing mechanism is configured"
elif [[ -n "${WINDOWS_SIGNING_PFX_B64:-}" ]]; then
    printf 'warning: a signing credential is configured but LIGHTNING_WINDOWS_SIGNED is false; artifacts will be labelled unsigned\n' >&2
fi

IFS=. read -r version_major version_minor version_patch <<<"$BASE_VERSION"
# One resource per Lightning-owned executable. They must NOT share one: the
# metadata gate requires OriginalFilename to name the file it is actually in
# (SignPath applies file-metadata restrictions to signed artifacts), and a
# second binary carrying "Lightning.exe" would be a false claim about which file
# a user is looking at. The resources are attached per target through
# packaging/windows/version-resources.cmake, not through the global
# CMAKE_EXE_LINKER_FLAGS this used to use.
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
# The helper deliberately carries no icon: it is never launched by a user and
# never appears in a shell that would show one.
write_version_rc "$BUILD_DIR/lightning-updater-version.rc" \
    "lightning-updater.exe" "lightning-updater" "Lightning update helper"
( cd "$BUILD_DIR" && x86_64-w64-mingw32-windres lightning-version.rc lightning-version.o )
( cd "$BUILD_DIR" && x86_64-w64-mingw32-windres lightning-updater-version.rc lightning-updater-version.o )

# Populate the complete lockfile cache first because project 6 deliberately
# invokes its actual build with --offline --locked.
/opt/rust/cargo/bin/cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"
/opt/rust/cargo/bin/cargo fetch --locked --target x86_64-pc-windows-gnu \
    --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

# GIF provider keys: embed the project-7 protected+masked CI variables
# GIPHY_API_KEY / KLIPY_API_KEY into this Windows build using the SAME mechanism
# as official release packages (the CMake generator reads the build-only
# LIGHTNING_BUILD_* names and writes them into an untracked build-tree header —
# never a compiler command line, CMakeCache, install rule, package, or log; the
# masked values do not appear in job output). This lets the test binary browse
# GIFs with no local env file or variable on the tester's PC. When the CI
# variables are absent the build stays keyless (the picker shows unconfigured),
# preserving the previous behaviour. A key compiled into a distributed binary is
# ultimately extractable — these test artifacts are developer-scoped and expire.
gif_require=OFF
gif_keys_embedded=false
# Fail closed in a publishing pipeline: an official Windows release must embed
# both provider keys (same rule as the Linux release builds).
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

# Release artifacts report artifact_kind=release; the developer test path keeps
# the unsigned-test marker. Windows packages are unsigned in both cases (the
# signing hook is a no-op unless WINDOWS_SIGNING_PFX_B64 is configured).
if [[ "$PUBLISHING" == true ]]; then
    WIN_ARTIFACT_KIND=release
else
    WIN_ARTIFACT_KIND=unsigned-test
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging/windows/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -ffile-prefix-map=$SOURCE_DIR=/usr/src/lightning -ffile-prefix-map=$ROOT=/usr/src/lightning-deploy" \
    -DCMAKE_PROJECT_INCLUDE="$ROOT/packaging/windows/version-resources.cmake" \
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
# The generator has written the header; drop the key values from the build
# environment so nothing downstream (compile, staging, packaging) sees them.
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true
# The production matrix-client is a GUI-subsystem PE on Windows (WIN32_EXECUTABLE
# set in the app CMake); --version / --help / --build-info still print to a
# parent console. No subsystem flag is passed here.
#
# lightning-updater is built explicitly alongside it. It is a separate, tiny
# console executable (Qt6::Core only) that performs the one install step that
# cannot happen while Lightning is running. Every Windows package is assembled
# from the staged tree below, so a helper that is not built here is a helper
# that ships in none of the three packages -- and the update feature is inert.
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" \
    --target matrix-client lightning-updater

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

# Product-metadata gate. Runs on the staged tree BEFORE anything is packaged or
# signed, so a Lightning-owned binary that disagrees with the release version or
# product name — or an upstream DLL that claims to be Lightning — fails the
# build instead of reaching a signing request. Also emits the signing inventory
# that says which PE files are ours and which are upstream.
python3 "$SCRIPT_DIR/verify-windows-metadata.py" \
    --stage "$STAGE_DIR" \
    --inventory "$ROOT/packaging/windows/signing-inventory.json" \
    --version "$BASE_VERSION" \
    --publisher "$WIN_PUBLISHER" \
    --report "$REPORT_DIR/windows-signing-inventory.json"

# The deterministic signing payload boundary (SignPath needs the artifact to
# exist as a pipeline artifact, by itself, before a signing request). This
# directory holds exactly the Lightning-owned PE files and their checksums — no
# Qt runtime, no installers, no logs. A future signing job submits this path and
# writes the signed binaries back over the staged copies before packaging.
SIGNING_DIR="$WINDOWS_DIST/signing-payload"
mkdir -p "$SIGNING_DIR"
for owned in Lightning.exe lightning-updater.exe; do
    cp "$STAGE_DIR/$owned" "$SIGNING_DIR/$owned"
    ( cd "$SIGNING_DIR" && sha256sum "$owned" >"$owned.sha256" )
done
cp "$STAGE_DIR/build-info.json" "$SIGNING_DIR/build-info.json"

# Sign the staged executables BEFORE they are captured into the portable ZIP /
# MSI / NSIS payloads, so a signed build signs the app itself, not just the
# installers. The updater is signed for the same reason it is signed at all: it
# is the process that replaces the application on disk, and an unsigned helper
# beside a signed app is the weakest link, not a detail.
sign_windows_file "$STAGE_DIR/Lightning.exe"
sign_windows_file "$STAGE_DIR/lightning-updater.exe"

short_sha="${SOURCE_SHA:0:7}"
artifact_base="Lightning-${BASE_VERSION}-${short_sha}-windows-x86_64"
portable="$WINDOWS_DIST/${artifact_base}-portable.zip"
( cd "$WINDOWS_DIST" && find Lightning -type f -print0 | LC_ALL=C sort -z \
    | xargs -0 zip -X -q "$portable" )

# All three Windows packages come from this one staged tree, so the compiled-in
# install type can only describe one of them. It says windows-portable, which is
# correct for the ZIP just created above -- the ZIP has no installer to tell it
# otherwise. The MSI carries an explicit marker file instead, and the NSIS
# installer writes its own at install time (packaging/windows/installer.nsi).
# The updater reads this marker to choose which compiled-in install strategy to
# use; it can never introduce a new one.
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

# The setup EXE writes its own marker on install, so the MSI's copy must not be
# baked into it -- otherwise an NSIS install would claim to be an MSI install
# and the updater would hand a .msi to a directory the MSI does not own.
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
    "$ROOT/packaging/windows/installer.nsi" \
    2>&1 | tee "$REPORT_DIR/nsis.log"
sign_windows_file "$setup"

"$SCRIPT_DIR/validate-windows-artifacts.sh" "$WINDOWS_DIST"
"$SCRIPT_DIR/smoke-windows-wine.sh" "$WINDOWS_DIST"

printf 'Built unsigned Windows test artifacts for Lightning %s (%s)\n' \
    "$BASE_VERSION" "$SOURCE_SHA"
