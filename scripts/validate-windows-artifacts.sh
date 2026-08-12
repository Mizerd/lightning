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

scan_forbidden() {
    local file="$1"
    if { strings -a "$file"; strings -a -el "$file"; } 2>/dev/null | \
        grep -Ei '(/home/roksme|/builds/[^ ]+|/source/|Documents/API|loggins\.txt|10\.195\.35\.[26]|CI_JOB_TOKEN|glrt-|glpat-|gldt-)' >/dev/null; then
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
grep -Fq 'StartMenuShortcut' "$REPORTS/msi-Shortcut.idt" || die "MSI shortcut is missing"
upgrade_code="$(jq -er '.upgrade_code' "$REPORTS/msi-identity.json")"
grep -Fq "$upgrade_code" "$REPORTS/msi-Upgrade.idt" || die "MSI UpgradeCode mismatch"

file -b "$setup" | grep -Eq '^PE32\+ executable.*\(GUI\), x86-64' || \
    die "NSIS setup is not an x86-64 GUI PE installer"
{ strings -a "$setup"; strings -a -el "$setup"; } | grep -F Lightning >/dev/null || \
    die "NSIS setup metadata does not contain the product name"

unzip -l "$portable" >"$REPORTS/portable-contents.txt"
grep -Fq 'Lightning/Lightning.exe' "$REPORTS/portable-contents.txt" || \
    die "portable ZIP does not contain Lightning.exe"
grep -Fq 'Lightning/plugins/platforms/qwindows.dll' "$REPORTS/portable-contents.txt" || \
    die "portable ZIP does not contain qwindows.dll"

# The SignPath artifact boundary: the unsigned Lightning-owned payload must
# exist on its own, with a checksum, so a future signing job has a deterministic
# single file to submit as a GitLab pipeline artifact.
signing_payload="$DIST/signing-payload"
[[ -f "$signing_payload/Lightning.exe" ]] || \
    die "signing payload is missing: signing-payload/Lightning.exe"
[[ -f "$signing_payload/Lightning.exe.sha256" ]] || \
    die "signing payload checksum is missing"
( cd "$signing_payload" && sha256sum -c Lightning.exe.sha256 >/dev/null ) || \
    die "signing payload checksum does not verify"
[[ -f "$REPORTS/windows-signing-inventory.json" ]] || \
    die "windows signing inventory report is missing"
jq -e '.lightning_owned == ["Lightning.exe"]' \
    "$REPORTS/windows-signing-inventory.json" >/dev/null || \
    die "signing inventory does not list exactly the Lightning-owned executable"

( cd "$DIST" && sha256sum "$(basename "$msi")" "$(basename "$setup")" \
    "$(basename "$portable")" >SHA256SUMS-windows.txt )

printf 'Windows structural validation passed: %d PE files, MSI tables, setup, portable ZIP\n' \
    "${#pe_files[@]}"
