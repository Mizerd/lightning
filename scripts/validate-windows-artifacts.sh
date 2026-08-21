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
