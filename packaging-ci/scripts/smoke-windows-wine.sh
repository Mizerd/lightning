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

# THE CHECK THAT WOULD HAVE CAUGHT THE LAST THREE ROUNDS.
#
# Everything else here proves the payload's SHAPE — the plugins are present,
# every DLL resolves, every symbol resolves. None of it proves the application
# can actually FIND the plugins at runtime, and that is exactly what was broken:
# the plugin path was applied after gst_init had already run, so a package with
# 25 correct plugins beside the exe still refused every call.
#
# `--call-media-status` probes through the same functions AppController calls,
# so a pass here means the shipped binary would offer a call button.
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
    # The bundled directory must be the one BESIDE the exe. Empty here would
    # mean it fell back to a system GStreamer, which a user's machine has not
    # got — a pass that would not survive contact with a real Windows box.
    grep -Eq '^bundled plugin directory: .*gstreamer-1\.0' "$log" || {
        cat "$log" >&2
        die "the packaged build did not use its own bundled plugin directory"
    }
}

# THE IMAGE DECODERS, asked of the shipped exe under Wine, for the same reason:
# a Qt image format is a dlopen'd plugin, so what this package can DRAW is a
# packaging property that no DLL listing and no symbol walk can report.
#
# JPEG XL is deliberately NOT required on Windows: Qt has never shipped a JXL
# plugin (qtimageformats v6.11.1 is dds/icns/jp2/macheif/macjp2/mng/tga/tiff/
# wbmp/webp) and Fedora carries no mingw64 build of KDE's kimageformats, which
# is the only implementation. The required set is what must hold, and
# `--image-format-status` exits 0 while reporting JPEG XL as unavailable.
run_image_format_status() {
    local exe="$1" log="$2"
    if ! timeout 120s wine64 "$exe" --image-format-status >"$log" 2>&1; then
        cat "$log" >&2
        die "the packaged build accepts image formats it cannot decode: $exe"
    fi
    local clean; clean="$(tr -d '\r' <"$log")"
    # Named individually rather than trusting the RESULT line, so a table that
    # quietly demoted one of them cannot pass.
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

# The scope marker the in-app updater reads (src/update/InstallType.cpp) to
# decide whether an upgrade must run per-machine and elevated. A per-user
# install that said "machine" would raise a UAC prompt for every update; a
# per-machine one that said "user" would be upgraded into a SECOND copy.
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

# THE VOICE-DELAY PROPERTY, asked of the shipped exe.
#
# WINE IS NOT WINDOWS and this does not pretend otherwise -- it is the same
# caveat every other check in this file carries, and the reason the required
# ELEMENT list exists beside the required DLL list. What makes it worth having
# anyway: the property under test is a GStreamer queue's own behaviour under a
# starved consumer, the binary is the one that ships, and the alternative was
# nothing at all. The Windows guest has no sound card, so the acoustic rig that
# measures this on Linux cannot run there; four attempts through RDP drifted
# 291 -> 545 ms, which is larger than the effect.
run_queue_selftest() {
    local exe="$1" log="$2" status=0
    timeout 300s wine64 "$exe" --call-queue-selftest >"$log" 2>&1 || status=$?
    # ONE judgement, shared with every Linux format and with macOS. This was a
    # second implementation for a few hours, while lib.sh's promotion note
    # named one place to change.
    assert_queue_selftest "Windows (wine)" "$log" "$status"
}

# THE CALL SOUNDS, asked of the shipped exe. WARN-ONLY, and under Wine in a
# container with no sound server the answer is expected to be UNMEASURED:
# QSoundEffect needs an output device to reach Ready. The transcript is still
# worth having -- it names the output Wine offered and whether the Qt
# Multimedia backend loaded at all. See assert_call_sounds_status in lib.sh.
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
# End-to-end proof that the update helper is actually DELIVERED by the
# installer, not merely present in the stage the installer was built from.
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

# --- ALL USERS (GitHub issue #14) -------------------------------------------
#
# The same two installers, per-machine: Program Files, HKLM, the scope marker
# saying "machine", and a clean uninstall. WINE IS NOT WINDOWS, and here it
# differs in three known ways, so this proves the payload and the registration
# and NOT the Windows behaviour around them: IsUserAnAdmin() is always true, so
# the setup's UAC relaunch never runs; Wine resolves the Start menu per-user
# even for ALLUSERS=1; and Wine does not restore ALLUSERS=1 when an installed
# product is maintained, so the MSI uninstall below passes it explicitly (real
# Windows keeps a product in the context it was installed in).
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
