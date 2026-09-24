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

# The payload checks prove shape, not that the application finds its plugins
# at runtime. `--call-media-status` probes through the same functions
# AppController uses, so a pass means the shipped binary would offer calls.
run_call_media_status() {
    local exe="$1" log="$2"
    if ! timeout 120s wine64 "$exe" --call-media-status >"$log" 2>&1; then
        cat "$log" >&2
        die "the packaged build cannot place calls: $exe"
    fi
    grep -Fq "RESULT: calls can be placed and answered" "$log" || {
        cat "$log" >&2
        die "call media status did not confirm a usable engine: $exe"
    }
    # The plugin directory must be the bundled one: a user's machine has no
    # system GStreamer to fall back to.
    grep -Eq '^bundled plugin directory: .*gstreamer-1\.0' "$log" || {
        cat "$log" >&2
        die "the packaged build did not use its own bundled plugin directory"
    }
}

# Image decoders are dlopen'd Qt plugins, so only the shipped exe can say what
# it can draw. JPEG XL is not required on Windows: Qt ships no JXL plugin and
# Fedora has no mingw64 kimageformats build. `--image-format-status` exits 0
# while reporting it unavailable.
run_image_format_status() {
    local exe="$1" log="$2"
    if ! timeout 120s wine64 "$exe" --image-format-status >"$log" 2>&1; then
        cat "$log" >&2
        die "the packaged build accepts image formats it cannot decode: $exe"
    fi
    local clean; clean="$(tr -d '\r' <"$log")"
    # Named individually so a table that demoted one format cannot pass.
    local fmt
    for fmt in png jpeg gif bmp webp; do
        grep -Fqx "required image/$fmt: decodable" <<<"$clean" || {
            cat "$log" >&2
            die "the packaged build cannot decode image/$fmt: $exe"
        }
    done
}

run_version() {
    local exe="$1" log="$2"
    timeout 60s wine64 "$exe" --version >"$log" 2>&1
    grep -Fq "Lightning $version" "$log" || die "Wine --version output mismatch: $exe"
}

# An installer that delivers Lightning.exe without `gstreamer-1.0/` produces an
# app that refuses every call. The NSIS payload cannot be inspected directly
# (/SOLID lzma), so the delivered install is the proof.
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

# The scope marker the updater reads (src/update/InstallType.cpp). A wrong
# "machine" would prompt UAC on every update; a wrong "user" would upgrade a
# per-machine install into a second copy.
assert_install_scope() {
    local root="$1" expected="$2" kind="$3" actual
    actual="$(tr -d '\r\n' <"$root/.lightning-install-scope" 2>/dev/null || true)"
    [[ "$actual" == "$expected" ]] || \
        die "$kind Wine install wrote install scope '${actual:-<missing>}', expected '$expected'"
}

# A per-user copy must not exist beside a per-machine one the smoke just made.
assert_no_per_user_copy() {
    local kind="$1"
    if find "$WINEPREFIX/drive_c/users" -type f -path '*/AppData/Local/Programs/Lightning/Lightning.exe' \
        -print -quit | grep -q .; then
        die "$kind all-users install ALSO created a per-user copy"
    fi
}

# The packaged binary must default to the Rust backend and the Windows secret
# store. This proves the compiled selection only; whether Credential Manager
# works is native-only and NOT TESTED.
run_build_info() {
    local exe="$1" log="$2"
    timeout 60s wine64 "$exe" --build-info >"$log" 2>&1
    # The output is CRLF; strip CR before anchored matching.
    local clean
    clean="$(tr -d '\r' <"$log")"
    grep -Eq '^default_backend: rust$' <<<"$clean" || die "build-info default_backend is not rust: $exe"
    grep -Eq '^secret_store: windows-credential-manager$' <<<"$clean" || die "build-info secret_store is not windows-credential-manager: $exe"
    grep -Eq '^backends: .*rust' <<<"$clean" || die "build-info does not list the rust backend: $exe"
}

# With runtime overrides cleared, only a build-embedded key can report
# "configured: yes". --gif-status prints booleans only; also assert that no
# key or URL leaked.
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

# Voice-delay self-test on the shipped exe. Wine is not Windows, but the
# property is a GStreamer queue's behaviour under a starved consumer, and the
# Windows guest has no sound card for the acoustic measurement.
run_queue_selftest() {
    local exe="$1" log="$2" status=0
    timeout 300s wine64 "$exe" --call-queue-selftest >"$log" 2>&1 || status=$?
    # Shared judgement with every Linux format and macOS.
    assert_queue_selftest "Windows (wine)" "$log" "$status"
}

# Call sounds, warn-only. Under Wine with no sound server the result is
# expected to be unmeasured; the transcript still shows the output Wine offered
# and whether the Qt Multimedia backend loaded. See lib.sh.
run_call_sounds_status() {
    local exe="$1" log="$2" status=0
    timeout 60s wine64 "$exe" --call-sounds-status >"$log" 2>&1 || status=$?
    assert_call_sounds_status "Windows (wine)" "$log" "$status"
}

new_prefix
run_version "$STAGE/Lightning.exe" "$REPORTS/wine-portable-version.log"
run_call_media_status "$STAGE/Lightning.exe" \
    "$REPORTS/wine-portable-call-media-status.log"
run_queue_selftest "$STAGE/Lightning.exe" \
    "$REPORTS/wine-portable-queue-selftest.log"
run_call_sounds_status "$STAGE/Lightning.exe" \
    "$REPORTS/wine-portable-call-sounds-status.log"
run_image_format_status "$STAGE/Lightning.exe" \
    "$REPORTS/wine-portable-image-format-status.log"
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
# The helper must be delivered by the installer, not just present in the stage.
[[ -f "$(dirname "$msi_exe")/lightning-updater.exe" ]] || \
    die "MSI Wine install did not create lightning-updater.exe"
assert_gstreamer_installed "$(dirname "$msi_exe")" MSI
assert_install_scope "$(dirname "$msi_exe")" user MSI
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
assert_install_scope "$(dirname "$nsis_exe")" user NSIS
run_version "$nsis_exe" "$REPORTS/wine-nsis-version.log"
uninstaller="$(find "$(dirname "$nsis_exe")" -maxdepth 1 -type f -iname 'Uninstall.exe' -print -quit)"
[[ -n "$uninstaller" ]] || die "NSIS uninstaller is missing"
timeout 120s wine64 "$uninstaller" /S >"$REPORTS/wine-nsis-uninstall.log" 2>&1
wineserver -w
[[ ! -e "$nsis_exe" ]] || die "NSIS Wine uninstall left Lightning.exe behind"
[[ -f "$marker" ]] || die "NSIS uninstall removed simulated user data"
finish_prefix

# --- All users (GitHub issue #14) --------------------------------------------
#
# Both installers per-machine: Program Files, HKLM, a "machine" scope marker
# and a clean uninstall. This proves payload and registration, not Windows
# behaviour: under Wine IsUserAnAdmin() is always true (no UAC relaunch), the
# Start menu resolves per-user even for ALLUSERS=1, and ALLUSERS=1 is not
# restored for maintenance, so the MSI uninstall passes it explicitly.
machine_key='HKLM\Software\Mizerd\Lightning'
machine_arp='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning'
reg_value() { # $1 key, $2 value name -> the data, or nothing
    wine64 reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | \
        awk -v n="$2" '$1 == n { sub(/^[ \t]+/, ""); sub(/^[^ \t]+[ \t]+[^ \t]+[ \t]+/, ""); print }'
}

new_prefix
machine_root="$WINEPREFIX/drive_c/Program Files/Lightning"
timeout 180s wine64 "$setup" /S /ALLUSERS >"$REPORTS/wine-nsis-allusers-install.log" 2>&1
wineserver -w
[[ -f "$machine_root/Lightning.exe" ]] || die "NSIS /S /ALLUSERS did not install into Program Files"
[[ -f "$machine_root/lightning-updater.exe" ]] || \
    die "NSIS /S /ALLUSERS did not install lightning-updater.exe"
assert_no_per_user_copy NSIS
assert_gstreamer_installed "$machine_root" "NSIS all-users"
assert_install_scope "$machine_root" machine "NSIS all-users"
[[ "$(reg_value "$machine_key" InstallDir)" == 'C:\Program Files\Lightning' ]] || \
    die "NSIS all-users install did not record its directory under HKLM"
[[ "$(reg_value "$machine_arp" DisplayVersion)" == "$version" ]] || \
    die "NSIS all-users install has no HKLM uninstall entry for $version"
run_version "$machine_root/Lightning.exe" "$REPORTS/wine-nsis-allusers-version.log"
timeout 120s wine64 "$machine_root/Uninstall.exe" /S >"$REPORTS/wine-nsis-allusers-uninstall.log" 2>&1
wineserver -w
[[ ! -e "$machine_root/Lightning.exe" ]] || die "NSIS all-users uninstall left Lightning.exe behind"
[[ -z "$(reg_value "$machine_arp" DisplayName)" ]] || \
    die "NSIS all-users uninstall left its HKLM uninstall entry behind"
finish_prefix

new_prefix
machine_root="$WINEPREFIX/drive_c/Program Files/Lightning"
msi_windows="$(winepath -w "$msi")"
timeout 120s wine64 msiexec /i "$msi_windows" ALLUSERS=1 /qn /norestart \
    >"$REPORTS/wine-msi-allusers-install.log" 2>&1
wineserver -w
[[ -f "$machine_root/Lightning.exe" ]] || die "MSI ALLUSERS=1 did not install into Program Files"
[[ -f "$machine_root/lightning-updater.exe" ]] || \
    die "MSI ALLUSERS=1 did not install lightning-updater.exe"
assert_no_per_user_copy MSI
assert_gstreamer_installed "$machine_root" "MSI all-users"
assert_install_scope "$machine_root" machine "MSI all-users"
[[ "$(reg_value "$machine_key" installed)" == 0x1 ]] || \
    die "MSI ALLUSERS=1 did not install its per-machine (HKLM) shortcut component"
[[ "$(reg_value "$machine_key" MsiInstallDir)" == 'C:\Program Files\Lightning\' ]] || \
    die "MSI ALLUSERS=1 did not record HKLM MsiInstallDir; the updater would treat it as per-user"
run_version "$machine_root/Lightning.exe" "$REPORTS/wine-msi-allusers-version.log"
timeout 120s wine64 msiexec /x "$msi_windows" ALLUSERS=1 /qn /norestart \
    >"$REPORTS/wine-msi-allusers-uninstall.log" 2>&1
wineserver -w
[[ ! -e "$machine_root/Lightning.exe" ]] || die "MSI all-users uninstall left Lightning.exe behind"
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
      simulated_user_data_preserved:true,
      install_scope_marker_user:true,
      nsis_all_users_install_uninstall:true, msi_all_users_install_uninstall:true,
      all_users_uac_relaunch_tested:false}' >"$REPORTS/wine-smoke.json"
printf 'Wine supplemental smoke passed (portable, MSI, NSIS, both installers per-user and all-users; --build-info rust+wincred); native Windows NOT TESTED\n'
