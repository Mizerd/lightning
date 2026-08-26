#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

DIST="${1:?usage: smoke-windows-wine.sh DIST_WINDOWS_DIR}"
STAGE="$DIST/Lightning"
REPORTS="$DIST/reports"
version="$(jq -er '.version' "$STAGE/build-info.json")"
gif_embedded="$(jq -r '.gif_keys_embedded // false' "$STAGE/build-info.json")"
msi="$(find "$DIST" -maxdepth 1 -type f -name 'Lightning-*-windows-x86_64.msi' -print -quit)"
setup="$(find "$DIST" -maxdepth 1 -type f -name 'Lightning-*-windows-x86_64-setup.exe' -print -quit)"
[[ -n "$msi" && -n "$setup" ]] || die "installer artifacts are missing"

prefixes=()
cleanup() {
    wineserver -k >/dev/null 2>&1 || true
    local prefix
    for prefix in "${prefixes[@]}"; do
        [[ "$prefix" == /tmp/lightning-wine.* ]] && rm -rf -- "$prefix"
    done
}
trap cleanup EXIT
export WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='winemenubuilder.exe=d'

new_prefix() {
    WINEPREFIX="$(mktemp -d /tmp/lightning-wine.XXXXXX)"
    prefixes+=("$WINEPREFIX")
    export WINEPREFIX
    timeout 90s wineboot -u >/dev/null 2>&1
}

finish_prefix() {
    wineserver -k >/dev/null 2>&1 || true
    timeout 30s wineserver -w >/dev/null 2>&1 || true
    [[ "$WINEPREFIX" == /tmp/lightning-wine.* ]] || die "unsafe Wine prefix cleanup path"
    rm -rf -- "$WINEPREFIX"
}

run_version() {
    local exe="$1" log="$2"
    timeout 60s wine64 "$exe" --version >"$log" 2>&1
    grep -Fq "matrix-client $version" "$log" || die "Wine --version output mismatch: $exe"
}

# The call media plugins are dlopen'd from `gstreamer-1.0/` beside the
# executable, so an installer that delivers Lightning.exe without that directory
# produces an application that starts, signs in and syncs normally and then
# refuses every call. Neither installer's payload can be inspected directly
# (wixl's File table is checked in validate-windows-artifacts.sh; the NSIS
# payload is /SOLID lzma and unreadable), so the delivered install is the proof.
gst_plugin_count="$(find "$STAGE/gstreamer-1.0" -maxdepth 1 -name '*.dll' 2>/dev/null | wc -l)"
[[ "$gst_plugin_count" -ge 20 ]] || \
    die "the staged tree carries only $gst_plugin_count GStreamer plugins"
assert_gstreamer_installed() {
    local root="$1" kind="$2" installed
    installed="$(find "$root/gstreamer-1.0" -maxdepth 1 -name '*.dll' 2>/dev/null | wc -l)"
    [[ "$installed" -eq "$gst_plugin_count" ]] || \
        die "$kind Wine install delivered $installed of $gst_plugin_count GStreamer plugins; calls would refuse in that install"
    [[ -f "$root/gstreamer-1.0/libgstwebrtc.dll" ]] || \
        die "$kind Wine install did not deliver gstreamer-1.0/libgstwebrtc.dll"
}

# Prove the packaged binary defaults to the Rust (E2EE) backend and the native
# Windows secret store — the two production-critical properties. Wine can read a
# GUI-subsystem PE's redirected stdout, so --build-info is capturable here.
# (Wine CANNOT prove the Credential Manager actually works — that is native-only
# and stays NOT TESTED; this only proves the COMPILED default/store selection.)
run_build_info() {
    local exe="$1" log="$2"
    timeout 60s wine64 "$exe" --build-info >"$log" 2>&1
    # The GUI-subsystem PE prints Windows CRLF line endings, so strip the
    # trailing CR before anchored matching (a bare ^...$ would miss "rust\r").
    local clean
    clean="$(tr -d '\r' <"$log")"
    grep -Eq '^default_backend: rust$' <<<"$clean" || die "build-info default_backend is not rust: $exe"
    grep -Eq '^secret_store: windows-credential-manager$' <<<"$clean" || die "build-info secret_store is not windows-credential-manager: $exe"
    grep -Eq '^backends: .*rust' <<<"$clean" || die "build-info does not list the rust backend: $exe"
}

# When GIF keys were embedded from CI variables, prove the EMBEDDED key works by
# clearing every runtime override so only the build-embedded key can report
# "configured: yes". --gif-status prints booleans only and never the key; we
# also assert nothing that looks like a key/URL leaked.
run_gif_status() {
    local exe="$1" log="$2"
    timeout 60s env -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
        -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY \
        -u LIGHTNING_GIF_ENV_FILE \
        wine64 "$exe" --gif-status >"$log" 2>&1
    local clean; clean="$(tr -d '\r' <"$log")"
    grep -Eiq 'GIPHY configured: yes' <<<"$clean" || die "embedded GIPHY key not configured: $exe"
    grep -Eiq 'KLIPY configured: yes' <<<"$clean" || die "embedded KLIPY key not configured: $exe"
    if grep -Eq 'api_key=|://' <<<"$clean"; then die "gif-status leaked a key or URL: $exe"; fi
}

new_prefix
run_version "$STAGE/Lightning.exe" "$REPORTS/wine-portable-version.log"
run_build_info "$STAGE/Lightning.exe" "$REPORTS/wine-portable-build-info.log"
if [[ "$gif_embedded" == "true" ]]; then
    run_gif_status "$STAGE/Lightning.exe" "$REPORTS/wine-portable-gif-status.log"
fi
finish_prefix

new_prefix
marker="$WINEPREFIX/drive_c/users/root/AppData/Local/MatrixClient/preserve-test.marker"
mkdir -p "$(dirname "$marker")"
printf 'preserve\n' >"$marker"
msi_windows="$(winepath -w "$msi")"
timeout 120s wine64 msiexec /i "$msi_windows" /qn /norestart \
    >"$REPORTS/wine-msi-install.log" 2>&1
wineserver -w
msi_exe="$(find "$WINEPREFIX/drive_c/users" -type f -path '*/AppData/Local/Programs/Lightning/Lightning.exe' -print -quit)"
[[ -n "$msi_exe" ]] || die "MSI Wine install did not create Lightning.exe"
# End-to-end proof that the update helper is actually DELIVERED by the
# installer, not merely present in the stage the installer was built from.
[[ -f "$(dirname "$msi_exe")/lightning-updater.exe" ]] || \
    die "MSI Wine install did not create lightning-updater.exe"
assert_gstreamer_installed "$(dirname "$msi_exe")" MSI
run_version "$msi_exe" "$REPORTS/wine-msi-version.log"
run_build_info "$msi_exe" "$REPORTS/wine-msi-build-info.log"
timeout 120s wine64 msiexec /x "$msi_windows" /qn /norestart \
    >"$REPORTS/wine-msi-uninstall.log" 2>&1
wineserver -w
[[ ! -e "$msi_exe" ]] || die "MSI Wine uninstall left Lightning.exe behind"
[[ -f "$marker" ]] || die "MSI uninstall removed simulated user data"
finish_prefix

new_prefix
marker="$WINEPREFIX/drive_c/users/root/AppData/Local/MatrixClient/preserve-test.marker"
mkdir -p "$(dirname "$marker")"
printf 'preserve\n' >"$marker"
timeout 180s wine64 "$setup" /S >"$REPORTS/wine-nsis-install.log" 2>&1
wineserver -w
nsis_exe="$(find "$WINEPREFIX/drive_c/users" -type f -path '*/AppData/Local/Programs/Lightning/Lightning.exe' -print -quit)"
[[ -n "$nsis_exe" ]] || die "NSIS Wine install did not create Lightning.exe"
[[ -f "$(dirname "$nsis_exe")/lightning-updater.exe" ]] || \
    die "NSIS Wine install did not create lightning-updater.exe"
assert_gstreamer_installed "$(dirname "$nsis_exe")" NSIS
run_version "$nsis_exe" "$REPORTS/wine-nsis-version.log"
uninstaller="$(find "$(dirname "$nsis_exe")" -maxdepth 1 -type f -iname 'Uninstall.exe' -print -quit)"
[[ -n "$uninstaller" ]] || die "NSIS uninstaller is missing"
timeout 120s wine64 "$uninstaller" /S >"$REPORTS/wine-nsis-uninstall.log" 2>&1
wineserver -w
[[ ! -e "$nsis_exe" ]] || die "NSIS Wine uninstall left Lightning.exe behind"
[[ -f "$marker" ]] || die "NSIS uninstall removed simulated user data"
finish_prefix

jq -n \
    --arg version "$version" \
    --argjson gif_embedded "$gif_embedded" \
    '{wine_version_tested:true, wine_build_info_tested:true,
      build_info_default_backend_rust:true,
      build_info_secret_store_windows_credential_manager:true,
      gif_keys_embedded:$gif_embedded,
      gif_status_embedded_key_ok:$gif_embedded,
      native_windows_tested:false,
      update_helper_installed_by_msi_and_nsis:true,
      application_version:$version, portable_version:true,
      msi_install_version_uninstall:true, nsis_install_version_uninstall:true,
      simulated_user_data_preserved:true}' >"$REPORTS/wine-smoke.json"
printf 'Wine supplemental smoke passed (portable, MSI, NSIS; --build-info rust+wincred); native Windows NOT TESTED\n'
