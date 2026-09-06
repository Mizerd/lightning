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
# The update helper ships beside the application in all three Windows packages.
# Without it the in-app updater has nothing to hand a verified artifact to, and
# the whole update feature is inert on Windows — a silent, shipped no-op, which
# is exactly the class of regression this check exists to catch.
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
# The production binary is now a GUI-subsystem PE (WIN32_EXECUTABLE) so a normal
# double-click never flashes or leaves a console; --version / --build-info still
# print to a parent console (main.cpp attaches to it).
x86_64-w64-mingw32-objdump -p "$STAGE/Lightning.exe" | \
    grep -F '(Windows GUI)' >/dev/null || die "application subsystem is not the expected Windows GUI"
# The helper is a console program on purpose: it is started by Lightning with a
# fixed argument vector, writes a status file, and must be able to report to a
# parent console. It is never double-clicked.
x86_64-w64-mingw32-objdump -p "$STAGE/lightning-updater.exe" | \
    grep -F '(Windows CUI)' >/dev/null || die "update helper subsystem is not the expected console"

# THE HELPER'S OWN IMPORTS MUST STAY INSIDE THE LIST THE CLIENT COPIES.
#
# Before an MSI or setup install, Lightning copies lightning-updater.exe and a
# short list of libraries OUT of the installation, because the installer
# rewrites every file in it and Windows will not overwrite a mapped image.
# That list lives in UpdateManager::helperRuntimeLibraries(). If the helper
# ever gains an import that is not in it, the staged copy fails to start at a
# user's machine, silently, during an update. So the two are tied together
# here: the shipped binary's non-system imports must all appear in that
# function.
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

# --- The call media engine ---------------------------------------------------
#
# Two separate facts, and a package can have either one without the other.
#
# 1. The engine was COMPILED IN. CMake enables it only when pkg-config finds the
#    GStreamer WebRTC development files; when it does not, the build still
#    succeeds, the packages still publish, and every call in the shipped client
#    refuses with the honest signaling-only message. That failure is invisible to
#    every other check here — the binary launches, signs in and syncs perfectly.
#    The only thing that distinguishes an engine-enabled build from an
#    engine-less one is that Lightning.exe IMPORTS the GStreamer libraries.
# 2. The PLUGINS shipped. They are dlopen'd, so an engine-enabled binary with no
#    `gstreamer-1.0/` directory beside it fails at first use instead of at build
#    time — which is why this is asserted separately from (1).
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
# EVERY NAMED IMPORT IN THE PAYLOAD RESOLVES, at the SYMBOL level.
#
# The staging script already proves every imported DLL is present, and the
# Wine element probe proves the plugins register. Neither can catch this:
# a plugin can carry a NORMAL import of a symbol that the DLL it names does
# not export, and Windows fails that module at LoadLibrary with
# ERROR_PROC_NOT_FOUND. Wine loads it anyway, so the probe passes and the
# feature is dead on the platform it was built for.
#
# That is not hypothetical. libgstd3d11.dll and libgstmediafoundation.dll
# from the upstream SDK import
#     libstdc++-6.dll::_ZNSt7codecvtIwc9_MbstatetEC2Ey
# which our libstdc++ does not export — the SDK is a UCRT build and this
# toolchain is msvcrt, and mingw-w64's wchar.h makes mbstate_t a struct
# under _UCRT and an int otherwise, so the two mangle differently. Those two
# plugins are therefore not shipped (docs/windows-packaging.md). This check
# is what would catch the next one.
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
    # Only DLLs WE ship: a system import (kernel32 &c.) is resolved by Windows.
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

# The FFmpeg backend needs its runtime libraries alongside the plugin, or video
# playback falls back to WMF and freezes. The DLLs are versioned (e.g.
# avcodec-61.dll), so match by family. This is the structural guard that a
# future runtime-staging change cannot silently drop the video backend.
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

# Builder paths and credential markers that must never reach a shipped byte.
#
# `/source/` IS ANCHORED, and that is the whole subtlety. Unanchored it matches
# a path COMPONENT anywhere, which is not what it was ever meant to catch — it
# fired on three upstream strings the moment GStreamer was bundled:
#
#   libgstwebrtcdsp.dll  ../webrtc/system_wrappers/source/field_trial.cc
#   icutu77.dll          icu/source/tools/gencmn/gencmn
#   libgstwinks.dll      Sink/Source/Audio/Video      <- not a path at all
#
# The last one is a device-category string and shows what the loose form was
# really doing: matching the letters "source" between slashes. This builder has
# NO /source/ component anyway — its checkout is $ROOT/work/lightning under
# /builds/, which the pattern above already covers — so the anchored form loses
# no real coverage. `[^[:alnum:]_.+-]` before it means a path ROOTED at
# /source still matches while a component never does.
#
# Nothing else is relaxed: every credential marker and every other builder path
# is byte-for-byte what it was.
readonly FORBIDDEN_RE='(/home/roksme|/builds/[^ ]+|(^|[^[:alnum:]_.+-])/source/|Documents/API|loggins\.txt|10\.195\.35\.[26]|CI_JOB_TOKEN|glrt-|glpat-|gldt-)'

# THE SCANNER HAS TEETH, asserted before it is trusted. A regex that silently
# stops matching is a scanner that passes everything, and this one guards
# credentials — so it is proven against a known-bad sample and a known-good one
# on every run, not reasoned about.
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
# The publisher is generated once in build-windows.sh and recorded in
# msi-identity.json; assert the MSI actually carries that value rather than a
# literal duplicated here, so the two can never drift apart.
msi_manufacturer="$(jq -er '.manufacturer' "$REPORTS/msi-identity.json")"
grep -Fq $'Manufacturer\t'"$msi_manufacturer" "$REPORTS/msi-Property.idt" || \
    die "MSI manufacturer is not the declared publisher: $msi_manufacturer"
grep -Eq 'x64|Intel64' "$REPORTS/msi-summary.txt" || die "MSI summary does not declare x64"
grep -Fq 'Lightning.exe' "$REPORTS/msi-File.idt" || die "MSI does not contain Lightning.exe"
grep -Fq 'lightning-updater.exe' "$REPORTS/msi-File.idt" || \
    die "MSI does not contain lightning-updater.exe"
# The install marker is how an MSI installation identifies itself to the
# updater. All three Windows packages come from one build whose compiled-in
# type says windows-portable, so if wixl ever drops this file an MSI install
# would detect as portable and the helper would swap a whole directory that
# Windows Installer owns, leaving its component state describing files it did
# not write. Nothing else in the pipeline would notice.
grep -Fq '.lightning-install-type' "$REPORTS/msi-File.idt" || \
    die "MSI does not contain the .lightning-install-type marker"
# The other half of the same coin. portable.marker is written into the stage for
# the duration of the `zip` call and removed immediately after (build-windows.sh
# documents the ordering). If it ever leaked into the MSI, an installed copy
# would put its settings, Matrix session and crypto store inside Program Files /
# %LOCALAPPDATA%\Programs -- a directory Windows Installer owns and will replace
# or remove -- instead of the per-user locations it is supposed to use. This is
# the strongest available proof for the MSI because wixl records every payload
# file by name in the File table.
if grep -Fq 'portable.marker' "$REPORTS/msi-File.idt"; then
    die "MSI contains portable.marker; an installed copy would detect as portable"
fi
# --- MSI PAYLOAD COMPLETENESS -----------------------------------------------
#
# Everything above proves the MSI contains four specific files. Nothing proved
# it contains the REST, and that is the gap this closes.
#
# All three Windows packages are produced from one staged tree, so the MSI and
# the portable ZIP must carry the same payload apart from two deliberate
# markers: portable.marker is in the ZIP only, and .lightning-install-type is
# in the MSI only (build-windows.sh documents the ordering). Any other
# difference means wixl did not ship a file that the ZIP and the NSIS setup
# both have.
#
# Why this is worth asserting rather than assuming: a missing Qt plugin does
# not stop Lightning launching. It removes a capability — an image format, a
# multimedia backend, a TLS backend — so the application starts, signs in and
# syncs, and then one feature fails on a machine where the other two package
# formats work. That is indistinguishable from an application bug, and the
# reported "uploads fail from the MSI, but the Setup EXE and the portable ZIP
# are fine" has exactly that shape. This check does not diagnose that report;
# it removes an entire class of cause from the search, in CI, every build.
msi_payload="$REPORTS/msi-payload.txt"
zip_payload="$REPORTS/zip-payload.txt"
# The File table's FileName column carries "SHORT|Long" for names needing an
# 8.3 form; take the long name. Leading columns are tab-separated.
awk -F'\t' 'NR > 3 { split($3, n, "|"); print (n[2] != "" ? n[2] : n[1]) }' \
    "$REPORTS/msi-File.idt" | LC_ALL=C sort >"$msi_payload"
# The ZIP lists "Lightning/<path>"; compare basenames, since the MSI File table
# records names rather than full paths.
unzip -Z1 "$portable" | sed 's:.*/::' | grep -v '^$' | LC_ALL=C sort >"$zip_payload"

# The two deliberate differences, removed from both sides before comparing.
msi_only="$(LC_ALL=C comm -23 "$msi_payload" "$zip_payload" \
    | grep -Fxv '.lightning-install-type' || true)"
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

# The comparison above proves MSI ⊇ ZIP, but only as a set difference — if a
# future change dropped the plugins from the staging script they would be absent
# from BOTH sides and the comparison would still pass. Name them here so the MSI
# is checked against the requirement rather than against its sibling.
for plugin in "${gst_plugins[@]}"; do
    grep -Fxq "$plugin" "$msi_payload" || \
        die "MSI payload does not carry the GStreamer plugin $plugin; calls would refuse after an MSI install"
done

grep -Fq 'StartMenuShortcut' "$REPORTS/msi-Shortcut.idt" || die "MSI shortcut is missing"
upgrade_code="$(jq -er '.upgrade_code' "$REPORTS/msi-identity.json")"
grep -Fq "$upgrade_code" "$REPORTS/msi-Upgrade.idt" || die "MSI UpgradeCode mismatch"

file -b "$setup" | grep -Eq '^PE32\+ executable.*\(GUI\), x86-64' || \
    die "NSIS setup is not an x86-64 GUI PE installer"
{ strings -a "$setup"; strings -a -el "$setup"; } | grep -F Lightning >/dev/null || \
    die "NSIS setup metadata does not contain the product name"
# NOTE: do NOT try to assert the helper's presence by grepping the setup EXE.
# installer.nsi uses `SetCompressor /SOLID lzma`, which compresses the file
# table along with the payload, so no payload filename survives as a plain
# string. Verified with a minimal installer built locally: a file that IS in
# the payload produces ZERO `strings` hits. The `grep -F Lightning` above only
# passes because that word is in the installer's own uncompressed metadata, not
# because it read the payload.
#
# The helper is covered properly instead by, in increasing strength: the staged
# tree assertion at the top of this script, `File /r "${STAGE_DIR}/*"` taking
# the whole stage, and smoke-windows-wine.sh actually running `setup /S` under
# Wine and asserting lightning-updater.exe lands next to Lightning.exe.
#
# The same limitation applies to proving portable.marker is ABSENT from the NSIS
# payload: `strings` cannot see inside a /SOLID lzma payload, so there is no
# direct assertion to make here. What IS assertable is the property that makes
# it true: installer.nsi takes the whole stage with `File /r "${STAGE_DIR}/*"`,
# makensis runs after build-windows.sh has removed the marker, and the stage is
# still on disk now. So the stage must not contain it at this point. That is a
# structural check, not an ordering proof -- an edit that moved the `zip` step
# below makensis would defeat it, which is why the MSI File-table check above
# (a real proof) and the extraction check below (a real proof) exist as well.
if [[ -e "$STAGE/portable.marker" ]]; then
    die "portable.marker is still in the stage; the NSIS payload would carry it"
fi

# --- The artifact the user actually downloads --------------------------------
#
# Everything above inspects $STAGE, the tree the packages were built FROM. That
# is not evidence about the ZIP. The ZIP is assembled by a separate `find | zip`
# step, it deliberately carries a file the stage no longer has, and it is what
# gets extracted onto a machine with no Qt, no MinGW runtime and no build tree.
# So the portable checks run against a fresh extraction of the FINAL artifact.
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
# Portable mode is decided by this file's PRESENCE beside the executable, before
# the first QSettings exists in the process. Without it the extracted folder
# behaves like an installed copy: registry, %LOCALAPPDATA%, Credential Manager,
# and a second login on the next PC -- the exact defect the portable ZIP is for.
# Nothing reads its contents, so this asserts only that it is a real file.
[[ -s "$EXTRACTED/portable.marker" ]] || die "portable.marker in the ZIP is empty"
# An inherited windows-msi marker would send a portable user through msiexec
# against a directory no MSI owns.
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

# Runtime closure over the EXTRACTED tree. The required plugin set, the QML
# import set and the system-DLL allowlist are read out of stage-windows-runtime.py
# itself rather than restated here: a duplicated DLL list rots the first time the
# staging script changes, and a rotted allowlist fails in the direction that
# passes. Nothing in this check is version-pinned -- the FFmpeg runtime is proven
# by walking the media plugin's imports, not by naming avcodec-61.dll.
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

# qt.conf is what points the extracted folder at its own plugins and QML imports.
# One absolute path in it and the folder resolves against the build machine's
# sysroot, which does not exist on the user's PC -- and, on a machine where a
# same-named directory does exist, would load code from outside the folder.
while IFS= read -r qtconf_value; do
    case "$qtconf_value" in
        /*|[A-Za-z]:[\\/]*|*'\\'*)
            die "qt.conf carries a non-relative path: $qtconf_value" ;;
    esac
done < <(sed -n 's/^[^=]*=[[:space:]]*//p' "$EXTRACTED/qt.conf")

# ...and no build/CI path anywhere else in the payload either. The extracted tree
# is scanned in full: text files line by line (so a hit names the file and the
# offending text, which is what makes a false positive diagnosable rather than
# mysterious) and PE files through the same `strings` scan the stage gets.
: >"$REPORTS/portable-path-scan.txt"
# TWO patterns, because "our build machine leaked into the artifact" and
# "this binary was compiled in a sysroot" are different facts.
#
# `leak_pattern` is applied to EVERY file including the upstream Qt and FFmpeg
# DLLs. Nothing here can legitimately appear in a shipped artifact: a CI token
# is a credential, and /root/ or /builds/ is this pipeline's own filesystem.
#
# `own_pattern` adds the paths that are only damning in something WE compiled —
# the MinGW sysroot prefix and this repository's own source root. It is applied
# to Lightning-owned PE files and to every packaged text file, and deliberately
# NOT to upstream DLLs: Qt and FFmpeg are *built inside* /usr/x86_64-w64-mingw32
# by the container image, so that string is present in their payload as a matter
# of course. Scanning them with it would fail the job on its own dependencies,
# which is a broken check rather than a strict one.
leak_pattern='(/root/|/builds/|CI_JOB_TOKEN|glrt-|glpat-|gldt-)'
own_pattern='(/home/[a-z_][a-z0-9_-]*|/usr/x86_64-w64-mingw32|/usr/src/lightning-deploy)'
lightning_owned_pe='^(Lightning\.exe|lightning-updater\.exe)$'
while IFS= read -r candidate; do
    rel="${candidate#"$PORTABLE_ROOT/"}"
    case "$(file -b --mime-type "$candidate")" in
        text/*|application/json|application/xml)
            # Text files are ours by definition (qt.conf, build-info.json, the
            # marker), so they get the strict pattern.
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

# Does the bundled GStreamer actually LOAD in this tree, or are 25 DLLs merely
# present?
#
# Every check above this line is a file listing or an import table. None of them
# can see a plugin that fails to LOAD — a runtime DLL that was never staged, a
# symbol the bundled libstdc++/glib does not export, a plugin built against a
# different ABI. That failure surfaces in the application as
# "missing_element:webrtcbin" and a refused call, on the user's machine, with
# every file sitting right beside the executable.
#
# gst-element-probe.exe is built into the packaging image and never shipped in a
# package. It does exactly what SfuMediaEngine::runtimeAvailable() does: point
# GST_PLUGIN_PATH at `<exe dir>/gstreamer-1.0`, clear GST_PLUGIN_SYSTEM_PATH,
# gst_init, then ask the registry for each element by name. Running it from
# INSIDE the extracted tree resolves its imports against exactly the DLLs the
# user gets. It runs after the payload scans above so the temporary copy is
# never part of any listing this script reports on.
#
# Wine is not Windows and this is not native acceptance: it proves the plugins
# load and register against the bundled runtime, not that a call connects.
gst_probe=/usr/local/share/lightning-windows/gst-element-probe.exe
[[ -f "$gst_probe" ]] || \
    die "gst-element-probe.exe is missing from the builder image; rebuild it from packaging/windows/Dockerfile"
GST_PROBE_HOME="$(mktemp -d "${TMPDIR:-/tmp}/lightning-gst-probe.XXXXXX")"
cleanup_gst_probe() {
    rm -f -- "$EXTRACTED/gst-element-probe.exe"
    # Scoped to this prefix only: smoke-windows-wine.sh runs later and creates
    # its own, and a server left holding a deleted prefix survives the job.
    [[ "${GST_PROBE_HOME:-}" == */lightning-gst-probe.* ]] && {
        WINEPREFIX="$GST_PROBE_HOME/prefix" wineserver -k >/dev/null 2>&1 || true
        rm -rf -- "$GST_PROBE_HOME"
    }
    # Explicit, because this is called directly as well as from the trap: the
    # guard above returns non-zero on a second call, and `set -e` would take it.
    return 0
}
trap 'cleanup_portable_root; cleanup_gst_probe' EXIT
cp "$gst_probe" "$EXTRACTED/gst-element-probe.exe"
gst_probe_env=(env "HOME=$GST_PROBE_HOME" "WINEPREFIX=$GST_PROBE_HOME/prefix"
    WINEARCH=win64 WINEDEBUG=-all)
# Bootstrap the prefix explicitly. Left implicit, the first-run bootstrap output
# lands in the probe's own log and a bootstrap failure then reads as a missing
# element, pointing the reader at the packaging instead of at Wine.
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

# The SignPath artifact boundary: the unsigned Lightning-owned payload must
# exist on its own, with a checksum, so a future signing job has a deterministic
# single file to submit as a GitLab pipeline artifact.
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
# Exactly the two Lightning-owned PE files: the application and the update
# helper that replaces it. Everything else in the payload is upstream and is
# never re-signed as ours. jq sorts the report's list, so this is order-stable.
jq -e '.lightning_owned == ["Lightning.exe", "lightning-updater.exe"]' \
    "$REPORTS/windows-signing-inventory.json" >/dev/null || \
    die "signing inventory does not list exactly the Lightning-owned executables"

( cd "$DIST" && sha256sum "$(basename "$msi")" "$(basename "$setup")" \
    "$(basename "$portable")" >SHA256SUMS-windows.txt )

printf 'Windows structural validation passed: %d PE files, MSI tables, setup, portable ZIP\n' \
    "${#pe_files[@]}"
