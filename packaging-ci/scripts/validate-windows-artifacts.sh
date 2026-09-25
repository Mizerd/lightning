#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

DIST="${1:?usage: validate-windows-artifacts.sh DIST_WINDOWS_DIR}"
STAGE="$DIST/Lightning"
REPORTS="$DIST/reports"
mkdir -p "$REPORTS"

mapfile -t msis < <(find "$DIST" -maxdepth 1 -type f -name 'Lightning-*-windows-x86_64.msi' -print)
mapfile -t setups < <(find "$DIST" -maxdepth 1 -type f -name 'Lightning-*-windows-x86_64-setup.exe' -print)
mapfile -t portables < <(find "$DIST" -maxdepth 1 -type f -name 'Lightning-*-windows-x86_64-portable.zip' -print)
[[ ${#msis[@]} -eq 1 && ${#setups[@]} -eq 1 && ${#portables[@]} -eq 1 ]] || \
    die "expected exactly one MSI, setup EXE, and portable ZIP"
msi="${msis[0]}"
setup="${setups[0]}"
portable="${portables[0]}"
[[ -f "$STAGE/Lightning.exe" ]] || die "staged Lightning.exe is missing"
# Without the update helper the in-app updater is inert on Windows.
[[ -f "$STAGE/lightning-updater.exe" ]] || die "staged lightning-updater.exe is missing"
version="$(jq -er '.version' "$STAGE/build-info.json")"

mapfile -t pe_files < <(find "$STAGE" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print | LC_ALL=C sort)
pe_files+=("$setup")
: >"$REPORTS/pe-files.txt"
: >"$REPORTS/pe-imports.txt"
for pe in "${pe_files[@]}"; do
    description="$(file -b "$pe")"
    printf '%s: %s\n' "${pe#"$DIST/"}" "$description" >>"$REPORTS/pe-files.txt"
    [[ "$description" == PE32+*x86-64* ]] || die "not an x86-64 PE file: $pe"
    [[ "$description" != *ELF* ]] || die "ELF file found in Windows payload: $pe"
    x86_64-w64-mingw32-objdump -f "$pe" | grep -F 'architecture: i386:x86-64' >/dev/null || \
        die "PE architecture mismatch: $pe"
    {
        printf '[%s]\n' "${pe#"$DIST/"}"
        x86_64-w64-mingw32-objdump -p "$pe" | sed -n 's/^[[:space:]]*DLL Name: /  /p' | LC_ALL=C sort -fu
    } >>"$REPORTS/pe-imports.txt"
done
# GUI subsystem, so a double-click never shows a console; --version and
# --build-info still attach to a parent console.
x86_64-w64-mingw32-objdump -p "$STAGE/Lightning.exe" | \
    grep -F '(Windows GUI)' >/dev/null || die "application subsystem is not the expected Windows GUI"
# The helper is a console program started by Lightning with a fixed argv.
x86_64-w64-mingw32-objdump -p "$STAGE/lightning-updater.exe" | \
    grep -F '(Windows CUI)' >/dev/null || die "update helper subsystem is not the expected console"

# Before an MSI or setup install, Lightning copies the helper and the libraries
# listed in UpdateManager::helperRuntimeLibraries() out of the install
# directory, because the installer rewrites it. Every non-system import of the
# shipped helper must be in that list or the copied helper fails to start.
helper_imports="$(x86_64-w64-mingw32-objdump -p "$STAGE/lightning-updater.exe" \
    | awk '/DLL Name:/ {print $3}' | sort -u)"
helper_list_source=""
for candidate in \
    "$(project_dir)/work/lightning/src/update/UpdateManager.cpp" \
    "$(project_dir)/src/update/UpdateManager.cpp"; do
    if [ -f "$candidate" ]; then
        helper_list_source="$candidate"
        break
    fi
done
if [ -n "$helper_list_source" ]; then
    while IFS= read -r dll; do
        case "$(printf '%s' "$dll" | tr 'A-Z' 'a-z')" in
            kernel32.dll|msvcrt.dll|user32.dll|advapi32.dll|shell32.dll|ole32.dll|ws2_32.dll|api-ms-*|ucrtbase.dll)
                continue ;;
        esac
        grep -qF "\"$dll\"" "$helper_list_source" \
            || die "the update helper imports $dll, which UpdateManager::helperRuntimeLibraries() does not stage"
    done <<< "$helper_imports"
    echo "update helper imports are all staged by helperRuntimeLibraries()"
else
    echo "note: application source not present, skipping the helper import cross-check"
fi

for required in \
    "$STAGE/Qt6Core.dll" \
    "$STAGE/Qt6Gui.dll" \
    "$STAGE/Qt6Network.dll" \
    "$STAGE/Qt6Qml.dll" \
    "$STAGE/Qt6Quick.dll" \
    "$STAGE/Qt6Multimedia.dll" \
    "$STAGE/libgcc_s_seh-1.dll" \
    "$STAGE/libstdc++-6.dll" \
    "$STAGE/libwinpthread-1.dll" \
    "$STAGE/plugins/platforms/qwindows.dll" \
    "$STAGE/plugins/multimedia/windowsmediaplugin.dll" \
    "$STAGE/plugins/multimedia/ffmpegmediaplugin.dll" \
    "$STAGE/qml/QtMultimedia/qmldir" \
    "$STAGE/qml/QtMultimedia/quickmultimediaplugin.dll" \
    "$STAGE/qml/QtQuick/Controls/qmldir" \
    "$STAGE/qml/QtQuick/Dialogs/qmldir" \
    "$STAGE/qml/QtQuick/Effects/qmldir" \
    "$STAGE/qml/QtQuick/Layouts/qmldir" \
    "$STAGE/qml/QtQuick/Window/qmldir"; do
    [[ -f "$required" ]] || die "required Windows runtime file is missing: $required"
done
for forbidden in "$STAGE/qml/QtTest" "$STAGE/qml/Qt/test" \
    "$STAGE/Qt6Test.dll" "$STAGE/Qt6QuickTest.dll"; do
    [[ ! -e "$forbidden" ]] || die "test-only Qt runtime found in portable payload: $forbidden"
done
# The SVG image plugin would let a received SVG reach a decoder (CLAUDE.md §6).
[[ ! -e "$STAGE/plugins/imageformats/qsvg.dll" ]] || \
    die "the SVG image plugin was staged: plugins/imageformats/qsvg.dll"

# --- The call media engine ---------------------------------------------------
#
# Two separate facts:
# 1. The engine was compiled in. Without the GStreamer WebRTC dev files CMake
#    silently builds without it and every call is refused; the only visible
#    difference is that Lightning.exe imports the GStreamer libraries.
# 2. The plugins shipped. They are dlopen'd, so a missing plugin directory
#    fails at first use rather than at build time.
gst_meta() {
    python3 -c 'import importlib.util,sys
spec = importlib.util.spec_from_file_location("staging", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
print("\n".join(getattr(module, sys.argv[2])) if sys.argv[2] != "GSTREAMER_PLUGIN_DIR"
      else module.GSTREAMER_PLUGIN_DIR)' "$SCRIPT_DIR/stage-windows-runtime.py" "$1"
}
gst_plugin_dir="$(gst_meta GSTREAMER_PLUGIN_DIR)"
mapfile -t gst_plugins < <(gst_meta GSTREAMER_PLUGINS)
mapfile -t gst_elements < <(gst_meta GSTREAMER_ELEMENTS)
[[ ${#gst_plugins[@]} -ge 20 && ${#gst_elements[@]} -ge 30 ]] || \
    die "the staging script declares only ${#gst_plugins[@]} GStreamer plugins and ${#gst_elements[@]} elements"

mapfile -t gst_app_imports < <(
    x86_64-w64-mingw32-objdump -p "$STAGE/Lightning.exe" |
        sed -n 's/^[[:space:]]*DLL Name: //p' | grep -i '^libgst' | LC_ALL=C sort -fu
)
for gst_library in libgstreamer-1.0-0.dll libgstwebrtc-1.0-0.dll \
    libgstsdp-1.0-0.dll libgstapp-1.0-0.dll; do
    printf '%s\n' "${gst_app_imports[@]}" | grep -Fqix "$gst_library" || \
        die "Lightning.exe does not import $gst_library: this build configured WITHOUT the GStreamer call media engine, and every call in this package would refuse"
done
for plugin in "${gst_plugins[@]}"; do
    [[ -f "$STAGE/$gst_plugin_dir/$plugin" ]] || \
        die "staged GStreamer plugin is missing: $gst_plugin_dir/$plugin"
done
# Every named import in the payload must resolve at the symbol level. A normal
# import of a symbol the named DLL does not export fails on Windows with
# ERROR_PROC_NOT_FOUND, but Wine loads it anyway, so the element probe cannot
# catch it. Example: the upstream d3d11 and mediafoundation plugins import a
# libstdc++ symbol that differs between UCRT and msvcrt builds (mbstate_t), so
# they are not shipped (docs/windows-packaging.md).
gst_symbols_report="$REPORTS/windows-import-symbols.txt"
: >"$gst_symbols_report"
declare -A staged_exports=()
while IFS= read -r -d '' pe; do
    staged_exports["$(basename "$pe" | tr 'A-Z' 'a-z')"]=1
done < <(find "$STAGE" -type f -name '*.dll' -print0)
all_exports="$(mktemp)"
while IFS= read -r -d '' pe; do
    x86_64-w64-mingw32-objdump -p "$pe" 2>/dev/null |
        awk '/\+base\[/ {print $NF}'
done < <(find "$STAGE" -type f -name '*.dll' -print0) | LC_ALL=C sort -u >"$all_exports"
unresolved=0
while IFS= read -r -d '' pe; do
    x86_64-w64-mingw32-objdump -p "$pe" 2>/dev/null | awk -v self="$(basename "$pe")" '
        /DLL Name:/ {dll=tolower($3); next}
        /^\t[0-9a-f]+ +<none> +[0-9a-f]+ +/ {print self" "dll" "$4}'
done < <(find "$STAGE" -type f \( -name '*.dll' -o -name '*.exe' \) -print0) |
while read -r self dll sym; do
    # Only DLLs we ship; system imports are resolved by Windows.
    [[ -n "${staged_exports[$dll]:-}" ]] || continue
    LC_ALL=C grep -qxF "$sym" "$all_exports" || printf '%s needs %s::%s\n' "$self" "$dll" "$sym"
done | LC_ALL=C sort -u >"$gst_symbols_report"
unresolved="$(wc -l <"$gst_symbols_report" | tr -d ' ')"
rm -f "$all_exports"
if [[ "$unresolved" -ne 0 ]]; then
    head -20 "$gst_symbols_report" >&2
    die "$unresolved unresolved named import(s) in the Windows payload — every one of these modules fails to load on Windows with ERROR_PROC_NOT_FOUND, and Wine will not tell you"
fi

printf 'call media engine linked in (%d GStreamer imports) with %d bundled plugins, 0 unresolved symbols\n' \
    "${#gst_app_imports[@]}" "${#gst_plugins[@]}"

# The FFmpeg backend needs its runtime libraries or playback falls back to WMF
# and freezes. The DLLs are versioned, so match by family.
for fflib in avcodec avformat avutil swresample swscale; do
    if ! find "$STAGE" -maxdepth 1 -type f -iname "${fflib}-*.dll" -print -quit | grep -q .; then
        die "FFmpeg runtime DLL is missing from the Windows payload: ${fflib}-*.dll"
    fi
done

if find "$STAGE" -type f \( -name '*.so' -o -name '*.a' -o -name '*.o' \) -print -quit | grep -q .; then
    die "Linux/development object found in portable payload"
fi
if find "$STAGE" -type d -name .git -print -quit | grep -q .; then
    die "source-control metadata found in portable payload"
fi
while IFS= read -r candidate; do
    if file -b "$candidate" | grep -Fq ELF; then
        die "ELF binary found in portable payload: $candidate"
    fi
done < <(find "$STAGE" -type f -print)

# Builder paths and credential markers that must never ship.
#
# `/source/` is anchored: unanchored it matched upstream strings such as
# webrtc's ".../system_wrappers/source/field_trial.cc" and the device category
# "Sink/Source/Audio/Video". A path rooted at /source still matches.
readonly FORBIDDEN_RE='(/home/roksme|/builds/[^ ]+|(^|[^[:alnum:]_.+-])/source/|Documents/API|loggins\.txt|10\.195\.35\.[26]|CI_JOB_TOKEN|glrt-|glpat-|gldt-)'

# Prove the scanner against known-bad and known-good samples on every run: a
# regex that silently stops matching would pass a real leak.
scan_self_test() {
    local bad good
    for bad in '/home/roksme/git/lightning' '/builds/Mizerd/lightning-deploy/work' \
               '/source/lightning/main.cpp' 'glpat-EXAMPLETOKENVALUE' \
               'CI_JOB_TOKEN=x' '10.195.35.2'; do
        printf '%s\n' "$bad" | grep -Eiq "$FORBIDDEN_RE" || \
            die "the forbidden-marker scanner no longer matches '$bad' — it would pass a real leak"
    done
    for good in '../webrtc/system_wrappers/source/field_trial.cc' \
                'icu/source/tools/gencmn/gencmn' 'Sink/Source/Audio/Video'; do
        printf '%s\n' "$good" | grep -Eiq "$FORBIDDEN_RE" && \
            die "the forbidden-marker scanner matches upstream string '$good' — it will fail every build on a false positive"
    done
    return 0
}
scan_self_test

scan_forbidden() {
    local file="$1"
    if { strings -a "$file"; strings -a -el "$file"; } 2>/dev/null | \
        grep -Ei "$FORBIDDEN_RE" >/dev/null; then
        die "forbidden build path or credential marker found in ${file#"$DIST/"}"
    fi
}
for candidate in "${pe_files[@]}" "$msi"; do
    scan_forbidden "$candidate"
done
if find "$STAGE" -type f \( -iname '*.key' -o -iname '*.pfx' -o -iname '*.p12' \) \
    -print -quit | grep -q .; then
    die "private-key file type found in portable payload"
fi
while IFS= read -r candidate; do
    case "$(file -b --mime-type "$candidate")" in
        text/*|application/json|application/xml)
            if grep -Ei 'BEGIN (RSA |OPENSSH |EC )?PRIVATE KEY' "$candidate" >/dev/null; then
                die "private-key marker found in text payload: ${candidate#"$STAGE/"}"
            fi
            ;;
    esac
done < <(find "$STAGE" -type f -print)

file -b "$msi" | grep -Fq 'Composite Document File V2 Document' || \
    die "MSI is not a structured-storage installer"
msiinfo suminfo "$msi" >"$REPORTS/msi-summary.txt"
msiinfo tables "$msi" | LC_ALL=C sort >"$REPORTS/msi-tables.txt"
for table in Property Feature Component File Directory Shortcut Upgrade Registry; do
    grep -Fxq "$table" "$REPORTS/msi-tables.txt" || die "MSI table is missing: $table"
    msiinfo export "$msi" "$table" >"$REPORTS/msi-${table}.idt"
    [[ -s "$REPORTS/msi-${table}.idt" ]] || die "MSI table is empty: $table"
done
grep -Fq $'ProductName\tLightning' "$REPORTS/msi-Property.idt" || die "MSI ProductName mismatch"
grep -Fq $'ProductVersion\t'"$version" "$REPORTS/msi-Property.idt" || die "MSI ProductVersion mismatch"
# Compare against the publisher recorded in msi-identity.json rather than a
# duplicated literal.
msi_manufacturer="$(jq -er '.manufacturer' "$REPORTS/msi-identity.json")"
grep -Fq $'Manufacturer\t'"$msi_manufacturer" "$REPORTS/msi-Property.idt" || \
    die "MSI manufacturer is not the declared publisher: $msi_manufacturer"
grep -Eq 'x64|Intel64' "$REPORTS/msi-summary.txt" || die "MSI summary does not declare x64"
grep -Fq 'Lightning.exe' "$REPORTS/msi-File.idt" || die "MSI does not contain Lightning.exe"
grep -Fq 'lightning-updater.exe' "$REPORTS/msi-File.idt" || \
    die "MSI does not contain lightning-updater.exe"
# The install marker is how an MSI install identifies itself to the updater.
# Without it an MSI install detects as portable (the compiled-in type) and the
# helper would swap a directory Windows Installer owns.
grep -Fq '.lightning-install-type' "$REPORTS/msi-File.idt" || \
    die "MSI does not contain the .lightning-install-type marker"
# portable.marker must not reach the MSI, or an installed copy would keep its
# settings, session and crypto store inside a directory Windows Installer owns.
# wixl records every payload file in the File table, so this is a real proof.
if grep -Fq 'portable.marker' "$REPORTS/msi-File.idt"; then
    die "MSI contains portable.marker; an installed copy would detect as portable"
fi
# --- MSI payload completeness ------------------------------------------------
#
# The MSI and the portable ZIP come from one staged tree and must match apart
# from the deliberate markers (portable.marker in the ZIP; install type and
# scope in the MSI). A missing Qt plugin does not stop Lightning launching; it
# silently removes one capability in one package format.
msi_payload="$REPORTS/msi-payload.txt"
zip_payload="$REPORTS/zip-payload.txt"
# The FileName column is "SHORT|Long" for names needing an 8.3 form.
awk -F'\t' 'NR > 3 { split($3, n, "|"); print (n[2] != "" ? n[2] : n[1]) }' \
    "$REPORTS/msi-File.idt" | LC_ALL=C sort >"$msi_payload"
# The File table records names, not paths, so compare basenames.
unzip -Z1 "$portable" | sed 's:.*/::' | grep -v '^$' | LC_ALL=C sort >"$zip_payload"
if grep -Fxq 'qsvg.dll' "$msi_payload" "$zip_payload"; then
    die "the SVG image plugin qsvg.dll is in the MSI or the portable ZIP"
fi

# The install scope marker (issue #14) appears as two File rows of one name
# under opposite component conditions.
msi_only="$(LC_ALL=C comm -23 "$msi_payload" "$zip_payload" \
    | grep -Fxv -e '.lightning-install-type' -e '.lightning-install-scope' || true)"
zip_only="$(LC_ALL=C comm -13 "$msi_payload" "$zip_payload" \
    | grep -Fxv 'portable.marker' || true)"
if [[ -n "$zip_only" ]]; then
    printf 'files in the portable ZIP but MISSING from the MSI:\n%s\n' "$zip_only" >&2
    die "MSI payload is incomplete; an MSI install would be missing files the ZIP and setup EXE have"
fi
if [[ -n "$msi_only" ]]; then
    printf 'files in the MSI but not in the portable ZIP:\n%s\n' "$msi_only" >&2
    die "MSI carries files the staged tree did not ship to the ZIP"
fi
printf 'MSI payload matches the portable ZIP (%d files)\n' "$(wc -l <"$msi_payload")"

# The set comparison cannot see a plugin missing from both sides, so check the
# required plugins against the MSI directly.
for plugin in "${gst_plugins[@]}"; do
    grep -Fxq "$plugin" "$msi_payload" || \
        die "MSI payload does not carry the GStreamer plugin $plugin; calls would refuse after an MSI install"
done

grep -Fq 'StartMenuShortcut' "$REPORTS/msi-Shortcut.idt" || die "MSI shortcut is missing"

# --- Install scope (issue #14) -----------------------------------------------
#
# Per-user stays the default: the package sets no ALLUSERS of its own, and bit
# 3 of the summary Word Count ("elevation not required") must stay set or a
# per-user install raises a UAC prompt. msiinfo labels PID_WORDCOUNT "Source".
if grep -Eq $'^ALLUSERS\t' "$REPORTS/msi-Property.idt"; then
    die "MSI sets ALLUSERS itself; a plain install would no longer be per-user"
fi
word_count="$(sed -n 's/^Source: \([0-9][0-9]*\).*/\1/p' "$REPORTS/msi-summary.txt")"
[[ -n "$word_count" ]] || die "MSI summary information has no Word Count"
(( word_count & 8 )) || die "MSI Word Count $word_count lacks bit 3; a per-user install would ask for elevation"
# ALLUSERS=1 re-points ProgramsDir at Program Files before CostFinalize, in
# both sequences. Fedora's msiinfo exports CRLF lines: strip the CR before any
# whole-line or last-column match (pipeline 257 died on exactly that).
for table in CustomAction InstallExecuteSequence InstallUISequence Component; do
    msiinfo export "$msi" "$table" >"$REPORTS/msi-${table}.idt"
done
tr -d '\r' <"$REPORTS/msi-CustomAction.idt" | \
    grep -Fxq $'LightningPerMachineProgramsDir\t51\tProgramsDir\t[ProgramFiles64Folder]\t' || \
    die "MSI has no per-machine directory action; ALLUSERS=1 would install per-machine into a per-user path"
for table in InstallExecuteSequence InstallUISequence; do
    grep -Eq $'^LightningPerMachineProgramsDir\tALLUSERS=1\t' "$REPORTS/msi-${table}.idt" || \
        die "MSI $table does not run the per-machine directory action under ALLUSERS=1"
done
# The scope marker the updater reads (src/update/InstallType.cpp) and the
# Start-menu shortcut, one component per scope under opposite conditions.
for pair in 'InstallScopeMarker_user:NOT ALLUSERS=1' 'InstallScopeMarker_machine:ALLUSERS=1' \
            'StartMenuShortcutComponent:NOT ALLUSERS=1' 'StartMenuShortcutMachineComponent:ALLUSERS=1'; do
    component="${pair%%:*}"; condition="${pair#*:}"
    awk -F'\t' -v c="$component" -v k="$condition" '{sub(/\r$/, "")} $1 == c && $5 == k {found=1} END {exit !found}' \
        "$REPORTS/msi-Component.idt" || die "MSI component $component is missing or not conditioned on '$condition'"
done
[[ "$(grep -c $'\t.lightning-install-scope\t' "$REPORTS/msi-File.idt" || true)" -eq 2 ]] || \
    die "MSI must carry exactly two .lightning-install-scope files (one per scope)"
# The updater trusts a "machine" marker only when HKLM names the directory
# (the marker is user-writable in a per-user install). Root 2 = HKLM.
awk -F'\t' '{sub(/\r$/, "")} $2 == 2 && $4 == "MsiInstallDir" && $5 == "[INSTALLFOLDER]" && $6 == "StartMenuShortcutMachineComponent" {found=1} END {exit !found}' \
    "$REPORTS/msi-Registry.idt" || die "MSI does not record its per-machine directory as HKLM MsiInstallDir"
upgrade_code="$(jq -er '.upgrade_code' "$REPORTS/msi-identity.json")"
grep -Fq "$upgrade_code" "$REPORTS/msi-Upgrade.idt" || die "MSI UpgradeCode mismatch"

file -b "$setup" | grep -Eq '^PE32\+ executable.*\(GUI\), x86-64' || \
    die "NSIS setup is not an x86-64 GUI PE installer"
{ strings -a "$setup"; strings -a -el "$setup"; } | grep -F Lightning >/dev/null || \
    die "NSIS setup metadata does not contain the product name"
# Payload filenames cannot be asserted from the setup EXE: `SetCompressor
# /SOLID lzma` compresses the file table, so `strings` sees none of them (the
# grep above only matches uncompressed metadata). The helper is covered by the
# stage check at the top, `File /r "${STAGE_DIR}/*"`, and smoke-windows-wine.sh
# running `setup /S`.
#
# Likewise portable.marker's absence from the NSIS payload: the stage makensis
# read must not contain it now. That is structural, not an ordering proof; the
# MSI File-table check and the ZIP extraction check are the real proofs.
if [[ -e "$STAGE/portable.marker" ]]; then
    die "portable.marker is still in the stage; the NSIS payload would carry it"
fi

# --- The artifact the user actually downloads --------------------------------
# Everything above inspects the stage. The ZIP is assembled separately and
# carries a file the stage no longer has, so the portable checks run against a
# fresh extraction of the final artifact.
unzip -l "$portable" >"$REPORTS/portable-contents.txt"
PORTABLE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/lightning-portable-check.XXXXXX")"
cleanup_portable_root() {
    [[ "${PORTABLE_ROOT:-}" == */lightning-portable-check.* ]] && rm -rf -- "$PORTABLE_ROOT"
}
trap cleanup_portable_root EXIT
unzip -qq "$portable" -d "$PORTABLE_ROOT"
EXTRACTED="$PORTABLE_ROOT/Lightning"
[[ -d "$EXTRACTED" ]] || die "portable ZIP does not extract to a Lightning/ directory"

for required in \
    "$EXTRACTED/Lightning.exe" \
    "$EXTRACTED/lightning-updater.exe" \
    "$EXTRACTED/qt.conf" \
    "$EXTRACTED/portable.marker"; do
    [[ -f "$required" ]] || \
        die "extracted portable ZIP is missing: ${required#"$EXTRACTED/"}"
done
# Portable mode is decided by this file's presence; without it the folder
# behaves like an installed copy. Its contents are never read.
[[ -s "$EXTRACTED/portable.marker" ]] || die "portable.marker in the ZIP is empty"
# The gst-plugins-good LGPL text, checked on the extracted artifact rather than
# on the staging script.
good_license="$EXTRACTED/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING"
[[ -s "$good_license" ]] || \
    die "the portable ZIP bundles gst-plugins-good binaries and carries no licence for them: $good_license is missing or empty"
grep -q "GNU LESSER GENERAL PUBLIC LICENSE" "$good_license" || \
    die "the staged gst-plugins-good licence is not the LGPL text"

# An inherited windows-msi marker would send a portable user through msiexec.
[[ ! -e "$EXTRACTED/.lightning-install-type" ]] || \
    die "portable ZIP must not contain an install-type marker"
for forbidden in "$EXTRACTED/qml/QtTest" "$EXTRACTED/qml/Qt/test" \
    "$EXTRACTED/Qt6Test.dll" "$EXTRACTED/Qt6QuickTest.dll"; do
    [[ ! -e "$forbidden" ]] || \
        die "test-only Qt runtime found in the extracted portable ZIP: $forbidden"
done
for plugin in "${gst_plugins[@]}"; do
    [[ -f "$EXTRACTED/$gst_plugin_dir/$plugin" ]] || \
        die "extracted portable ZIP is missing the GStreamer plugin $gst_plugin_dir/$plugin"
done
[[ ! -e "$EXTRACTED/plugins/imageformats/qsvg.dll" ]] || \
    die "the extracted portable ZIP carries the SVG image plugin qsvg.dll"

# Runtime closure over the extracted tree. The plugin set, QML imports and
# system-DLL allowlist are read from stage-windows-runtime.py rather than
# restated, and nothing is version-pinned (FFmpeg is found through the media
# plugin's imports).
python3 - "$SCRIPT_DIR/stage-windows-runtime.py" "$EXTRACTED" \
    "$REPORTS/portable-runtime-closure.json" <<'PY'
import importlib.util
import json
import pathlib
import re
import subprocess
import sys

stage_script, root, report = (
    pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
)

# The staging script guards its own main() behind __name__, so importing it has
# no side effects; this is a data read, not a second invocation.
spec = importlib.util.spec_from_file_location("stage_windows_runtime", stage_script)
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)
system_dlls = {name.lower() for name in staging.SYSTEM_DLLS}

errors: list[str] = []

for directory, names in staging.PLUGIN_FILES.items():
    for name in names:
        if not (root / "plugins" / directory / name).is_file():
            errors.append(f"missing Qt plugin: plugins/{directory}/{name}")
for name in staging.QML_RUNTIME_ENTRIES:
    if not (root / "qml" / name).is_dir():
        errors.append(f"missing QML runtime import: qml/{name}")

pe_files = sorted(
    p for p in root.rglob("*")
    if p.is_file() and p.suffix.lower() in {".exe", ".dll"}
)
if not pe_files:
    errors.append("extracted tree contains no PE files at all")
present = {p.name.lower(): p for p in pe_files}


def objdump(flag: str, path: pathlib.Path) -> str:
    return subprocess.check_output(
        ["x86_64-w64-mingw32-objdump", flag, str(path)], text=True
    )


graph: dict[str, list[str]] = {}
unresolved: dict[str, list[str]] = {}
for pe in pe_files:
    rel = str(pe.relative_to(root))
    header = objdump("-f", pe)
    # Every bundled PE, not a hand-typed subset: one 32-bit or ELF file in the
    # payload makes the folder unusable on the target machine, and it would be
    # discovered by the user, not here.
    if "architecture: i386:x86-64" not in header:
        errors.append(f"not an x86-64 PE: {rel}")
    if "file format pei-x86-64" not in header:
        errors.append(f"not a PE32+ image: {rel}")
    needed = sorted(
        set(re.findall(r"DLL Name:\s*(\S+)", objdump("-p", pe))), key=str.casefold
    )
    graph[rel] = needed
    for dll in needed:
        key = dll.lower()
        if key in present or key in system_dlls:
            continue
        if key.startswith(("api-ms-win-", "ext-ms-win-")):
            continue
        unresolved.setdefault(dll, []).append(rel)

for dll, importers in sorted(unresolved.items()):
    errors.append(
        f"{dll} is imported by {', '.join(importers)} but is neither in the "
        "package nor on the system-DLL allowlist"
    )

# The FFmpeg decode backend, proven by the import walk instead of by a directory
# listing: if the plugin is present but its runtime libraries are not reachable
# from it, Qt silently falls back to WMF and video playback stalls. Matched by
# family so a Qt or FFmpeg version bump does not need an edit here.
ffmpeg_families = {"avcodec", "avformat", "avutil", "swresample", "swscale"}
plugin = root / "plugins/multimedia/ffmpegmediaplugin.dll"
reachable: set[str] = set()
if plugin.is_file():
    queue = [str(plugin.relative_to(root))]
    seen: set[str] = set()
    while queue:
        current = queue.pop(0)
        if current in seen:
            continue
        seen.add(current)
        for dll in graph.get(current, []):
            key = dll.lower()
            target = present.get(key)
            if target is None:
                continue
            reachable.add(key)
            queue.append(str(target.relative_to(root)))
missing_ffmpeg = sorted(
    family for family in ffmpeg_families
    if not any(name.startswith(family) for name in reachable)
)
if missing_ffmpeg:
    errors.append(
        "FFmpeg runtime not reachable from plugins/multimedia/ffmpegmediaplugin.dll: "
        + ", ".join(missing_ffmpeg)
    )

report.write_text(
    json.dumps(
        {
            "root": "Lightning",
            "pe_files": sorted(str(p.relative_to(root)) for p in pe_files),
            "imports": dict(sorted(graph.items())),
            "ffmpeg_reachable": sorted(reachable & {
                name for name in reachable
                if any(name.startswith(f) for f in ffmpeg_families)
            }),
            "errors": errors,
        },
        indent=2,
        sort_keys=True,
    )
    + "\n",
    encoding="utf-8",
)
if errors:
    for message in errors:
        print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)
print(f"extracted portable runtime closed over {len(pe_files)} PE files")
PY

# qt.conf must be relative: an absolute path resolves against the build
# machine's sysroot, or loads code from outside the folder.
while IFS= read -r qtconf_value; do
    case "$qtconf_value" in
        /*|[A-Za-z]:[\\/]*|*'\\'*)
            die "qt.conf carries a non-relative path: $qtconf_value" ;;
    esac
done < <(sed -n 's/^[^=]*=[[:space:]]*//p' "$EXTRACTED/qt.conf")

# No build or CI path anywhere in the extracted tree. Text files are scanned
# line by line so a hit names the offending text.
: >"$REPORTS/portable-path-scan.txt"
# `leak_pattern` applies to every file: nothing may carry a CI token or this
# pipeline's filesystem paths. `own_pattern` (the MinGW sysroot and our source
# root) applies only to Lightning-owned PEs and packaged text files: upstream
# Qt and FFmpeg are built inside /usr/x86_64-w64-mingw32 and legitimately
# contain that string.
leak_pattern='(/root/|/builds/|CI_JOB_TOKEN|glrt-|glpat-|gldt-)'
own_pattern='(/home/[a-z_][a-z0-9_-]*|/usr/x86_64-w64-mingw32|/usr/src/lightning-deploy)'
lightning_owned_pe='^(Lightning\.exe|lightning-updater\.exe)$'
while IFS= read -r candidate; do
    rel="${candidate#"$PORTABLE_ROOT/"}"
    case "$(file -b --mime-type "$candidate")" in
        text/*|application/json|application/xml)
            # Text files are ours (qt.conf, build-info.json, the marker).
            if grep -EnI "$leak_pattern|$own_pattern" "$candidate" \
                | sed "s|^|$rel:|" >>"$REPORTS/portable-path-scan.txt"; then
                die "build or CI path found in packaged text file: $rel (see reports/portable-path-scan.txt)"
            fi
            ;;
        *)
            scan_pattern="$leak_pattern"
            if [[ "$(basename "$candidate")" =~ $lightning_owned_pe ]]; then
                scan_pattern="$leak_pattern|$own_pattern"
            fi
            if { strings -a "$candidate"; strings -a -el "$candidate"; } 2>/dev/null \
                | grep -Eq "$scan_pattern"; then
                die "build or CI path found in packaged binary: $rel"
            fi
            ;;
    esac
done < <(find "$EXTRACTED" -type f -print)

# File listings and import tables cannot see a plugin that fails to load. That
# surfaces as "missing_element:webrtcbin" on the user's machine.
#
# gst-element-probe.exe (built into the builder image, never shipped) does what
# SfuMediaEngine::runtimeAvailable() does: point GST_PLUGIN_PATH at
# `<exe dir>/gstreamer-1.0`, clear the system path, gst_init, and ask for each
# element. Run from inside the extracted tree, it resolves against exactly the
# DLLs the user gets. It runs after the payload scans so its temporary copy is
# never listed. Under Wine this proves registration, not that a call connects.
gst_probe=/usr/local/share/lightning-windows/gst-element-probe.exe
[[ -f "$gst_probe" ]] || \
    die "gst-element-probe.exe is missing from the builder image; rebuild it from packaging/windows/Dockerfile"
GST_PROBE_HOME="$(mktemp -d "${TMPDIR:-/tmp}/lightning-gst-probe.XXXXXX")"
cleanup_gst_probe() {
    rm -f -- "$EXTRACTED/gst-element-probe.exe"
    # Only this prefix: smoke-windows-wine.sh creates its own later.
    [[ "${GST_PROBE_HOME:-}" == */lightning-gst-probe.* ]] && {
        WINEPREFIX="$GST_PROBE_HOME/prefix" wineserver -k >/dev/null 2>&1 || true
        rm -rf -- "$GST_PROBE_HOME"
    }
    # Called directly and from the trap; the guard above returns non-zero on a
    # second call, which `set -e` would take.
    return 0
}
trap 'cleanup_portable_root; cleanup_gst_probe' EXIT
cp "$gst_probe" "$EXTRACTED/gst-element-probe.exe"
gst_probe_env=(env "HOME=$GST_PROBE_HOME" "WINEPREFIX=$GST_PROBE_HOME/prefix"
    WINEARCH=win64 WINEDEBUG=-all)
# Bootstrap the prefix explicitly so a Wine bootstrap failure is not reported
# as a missing element.
timeout 120s "${gst_probe_env[@]}" wineboot -u \
    >"$REPORTS/gst-element-probe-wineboot.log" 2>&1 || true
if ! ( cd "$EXTRACTED" && "${gst_probe_env[@]}" \
        xvfb-run -a wine ./gst-element-probe.exe "${gst_elements[@]}" ) \
        >"$REPORTS/gst-element-probe.txt" 2>&1; then
    cat "$REPORTS/gst-element-probe.txt" >&2
    die "the bundled GStreamer did not provide every element the call engine requires (reports/gst-element-probe.txt)"
fi
cleanup_gst_probe
trap cleanup_portable_root EXIT
printf 'bundled GStreamer registered all %d required elements under Wine\n' \
    "${#gst_elements[@]}"

# The signing payload must exist on its own with a checksum, for a signing job
# to submit.
signing_payload="$DIST/signing-payload"
for owned in Lightning.exe lightning-updater.exe; do
    [[ -f "$signing_payload/$owned" ]] || \
        die "signing payload is missing: signing-payload/$owned"
    [[ -f "$signing_payload/$owned.sha256" ]] || \
        die "signing payload checksum is missing: $owned.sha256"
    ( cd "$signing_payload" && sha256sum -c "$owned.sha256" >/dev/null ) || \
        die "signing payload checksum does not verify: $owned"
done
[[ -f "$REPORTS/windows-signing-inventory.json" ]] || \
    die "windows signing inventory report is missing"
# Exactly the two Lightning-owned PEs; everything else is upstream and never
# re-signed as ours. jq sorts the list.
jq -e '.lightning_owned == ["Lightning.exe", "lightning-updater.exe"]' \
    "$REPORTS/windows-signing-inventory.json" >/dev/null || \
    die "signing inventory does not list exactly the Lightning-owned executables"

( cd "$DIST" && sha256sum "$(basename "$msi")" "$(basename "$setup")" \
    "$(basename "$portable")" >SHA256SUMS-windows.txt )

printf 'Windows structural validation passed: %d PE files, MSI tables, setup, portable ZIP\n' \
    "${#pe_files[@]}"
