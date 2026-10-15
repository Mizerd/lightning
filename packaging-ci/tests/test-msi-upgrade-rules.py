#!/usr/bin/env python3
"""An MSI major upgrade must keep every file the new version ships.

The 0.10.2 candidate, installed over 0.10.1 on Windows 11, reported success and
left 133 files missing, Lightning.exe among them. Two properties of the
generated WiX caused it, and this checks both on the generator's real output:

* wixl leaves File/@Version empty unless DefaultVersion is given, and Windows
  Installer treats an unversioned new file as older than any versioned copy
  already installed, so it skipped every versioned component.
* RemoveExistingProducts ran after InstallValidate, i.e. after that decision
  and before the install, so removing the old product deleted the files the
  new one had just declined to copy.

wixl is not needed: the File element's DefaultVersion and the explicit
RemoveExistingProducts element are what wixl turns into those table rows (the
validator asserts the tables themselves on the built MSI).
"""

from __future__ import annotations

import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = pathlib.Path(__file__).resolve().parent
GENERATOR = HERE.parent / "scripts" / "generate-windows-wix.py"
NS = {"w": "http://schemas.microsoft.com/wix/2006/wi"}

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


def fake_pe(version: tuple[int, int, int, int] | None) -> bytes:
    """An "MZ" image with a VS_FIXEDFILEINFO block, as a resource section holds it."""
    body = b"MZ" + b"\0" * 300
    if version is None:
        return body + b"no version resource here"
    a, b, c, d = version
    info = struct.pack("<IIII", 0xFEEF04BD, 0x00010000, (a << 16) | b, (c << 16) | d)
    # Decoys: a signature with the wrong structure version, and a well-formed
    # VS_FIXEDFILEINFO in code or data with no VS_VERSION_INFO key before it.
    decoy = struct.pack("<IIII", 0xFEEF04BD, 0x00020000, 0x00090009, 0)
    stray = struct.pack("<IIII", 0xFEEF04BD, 0x00010000, 0x00090009, 0)
    key = "VS_VERSION_INFO\0".encode("utf-16-le") + b"\0\0"
    return body + decoy + b"\0" * 16 + stray + b"\0" * 8 + key + info + struct.pack("<II", 0, 0)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        stage = root / "stage"
        (stage / "plugins").mkdir(parents=True)
        (stage / "Lightning.exe").write_bytes(fake_pe((0, 10, 2, 0)))
        (stage / "plugins" / "qjpeg.dll").write_bytes(fake_pe((6, 11, 2, 0)))
        (stage / "libnover.dll").write_bytes(fake_pe(None))
        (stage / "Main.qml").write_text("import QtQuick\n", encoding="utf-8")
        (stage / "Lightning.ico").write_bytes(b"\0\0\1\0")
        wxs = root / "out" / "Lightning.wxs"
        metadata = root / "out" / "msi-identity.json"
        subprocess.run([
            sys.executable, str(GENERATOR), "--stage", str(stage),
            "--version", "0.10.2", "--source-sha", "0" * 40,
            "--manufacturer", "Test", "--signing-state", "unsigned",
            "--output", str(wxs), "--metadata", str(metadata),
        ], check=True)
        tree = ET.parse(wxs)

        versions = {f.get("Name"): f.get("DefaultVersion")
                    for f in tree.iterfind(".//w:File", NS)}
        check(versions.get("Lightning.exe") == "0.10.2.0",
              "Lightning.exe carries its PE file version")
        check(versions.get("qjpeg.dll") == "6.11.2.0",
              "a nested versioned DLL carries its version, not the decoy's")
        check(versions.get("libnover.dll") is None,
              "a PE without a version resource stays unversioned")
        check(versions.get("Main.qml") is None, "a non-PE file stays unversioned")
        check(json.loads(metadata.read_text()).get("versioned_files") == 2,
              "the metadata counts the versioned files the validator will expect")

        sequence = tree.find(".//w:InstallExecuteSequence", NS)
        removal = sequence.findall("w:RemoveExistingProducts", NS) if sequence is not None else []
        check(len(removal) == 1 and removal[0].get("After") == "InstallFinalize",
              "the old product is removed after InstallFinalize")
        check(len(tree.findall(".//w:InstallExecuteSequence", NS)) == 1,
              "exactly one InstallExecuteSequence element")
        upgrade = tree.find(".//w:MajorUpgrade", NS)
        check(upgrade is not None and upgrade.get("Schedule") is None,
              "MajorUpgrade carries no Schedule, which wixl 0.106 ignores")

    if failures:
        print(f"{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("MSI upgrade rules hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
