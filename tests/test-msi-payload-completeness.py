#!/usr/bin/env python3
"""The MSI must carry the same payload as the portable ZIP.

All three Windows packages are produced from one staged tree, so the MSI and
the ZIP differ by exactly two deliberate markers and nothing else. Before this
check the validation asserted that the MSI contained four specific files and
said nothing about the rest — so an MSI missing, say, a Qt image-format or TLS
plugin would ship. That does not stop Lightning launching; it removes one
capability, which then fails on a machine where the ZIP and the setup EXE work.

The reported "uploads fail from the MSI, but the Setup EXE and the portable ZIP
are fine" has exactly that shape. This does not diagnose that report — it
removes a whole class of cause from the search, in CI, on every build.

Exercises the real awk/comm pipeline from validate-windows-artifacts.sh against
a real File table layout, including the "SHORT|Long" FileName form.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPT = ROOT / "scripts" / "validate-windows-artifacts.sh"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


def idt(rows: list[tuple[str, str]]) -> str:
    """A File table export: three header lines, then File/Component/FileName/..."""
    out = [
        "File\tComponent_\tFileName\tFileSize\tVersion\tLanguage\tAttributes\tSequence",
        "s72\ts72\tl255\ti4\tS72\tS20\tI2\ti4",
        "File\tFile",
    ]
    for index, (file_id, name) in enumerate(rows, start=1):
        out.append(f"FIL_{file_id}\tCMP_{file_id}\t{name}\t2\t\t\t512\t{index}")
    return "\n".join(out) + "\n"


def parse_msi_names(table: str) -> list[str]:
    """Run the validator's own awk expression, so this cannot drift from it."""
    source = SCRIPT.read_text(encoding="utf-8")
    match = re.search(r"awk -F'[^']*' '(NR > 3 \{[^']*\})'", source)
    if not match:
        raise AssertionError("could not find the File-table awk program in the validator")
    program = match.group(1)
    with tempfile.NamedTemporaryFile("w", suffix=".idt", delete=False) as handle:
        handle.write(table)
        path = handle.name
    result = subprocess.run(["awk", "-F\t", program, path],
                            capture_output=True, text=True, check=True)
    return sorted(line for line in result.stdout.splitlines() if line)


def compare(msi: list[str], zip_names: list[str]) -> tuple[list[str], list[str]]:
    """The validator's comparison: the two deliberate markers are exempt."""
    msi_set, zip_set = set(msi), set(zip_names)
    zip_only = sorted(zip_set - msi_set - {"portable.marker"})
    msi_only = sorted(msi_set - zip_set - {".lightning-install-type"})
    return zip_only, msi_only


print("MSI payload completeness")

# The File-table parse, including the 8.3 "SHORT|Long" form wixl emits for
# names that need one. Taking the short name there would compare rubbish.
names = parse_msi_names(idt([
    ("a", "Lightning.exe"),
    ("b", "QSCHAN~1.DLL|qschannelbackend.dll"),
    ("c", ".lightning-install-type"),
]))
check(names == [".lightning-install-type", "Lightning.exe", "qschannelbackend.dll"],
      "the File table parse takes the LONG name from a SHORT|Long pair")

# A healthy pair differs by exactly the two markers.
msi = [".lightning-install-type", "Lightning.exe", "Qt6Core.dll", "qwebp.dll"]
zips = ["Lightning.exe", "Qt6Core.dll", "portable.marker", "qwebp.dll"]
zip_only, msi_only = compare(msi, zips)
check(not zip_only and not msi_only,
      "a matching MSI and ZIP compare clean across the two deliberate markers")

# The failure this exists for: a plugin present in the ZIP and absent from the
# MSI. The application still launches; one capability is simply gone.
zip_only, msi_only = compare([n for n in msi if n != "qwebp.dll"], zips)
check(zip_only == ["qwebp.dll"],
      "a plugin missing from the MSI is caught and named")

# The reverse, which would mean the MSI shipped something the stage did not.
zip_only, msi_only = compare(msi + ["stray.dll"], zips)
check(msi_only == ["stray.dll"], "a file only in the MSI is caught and named")

# The markers must stay exempt in the direction they belong to, and ONLY that
# direction: portable.marker appearing in the MSI is a different, already-
# asserted failure (an installed copy would keep its crypto store inside a
# directory Windows Installer owns).
zip_only, msi_only = compare(msi + ["portable.marker"], zips)
check(msi_only == [], "portable.marker in the ZIP alone is not a payload difference")

if failures:
    print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
    sys.exit(1)
print("MSI payload completeness tests passed")
