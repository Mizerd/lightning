#!/usr/bin/env python3
"""Tests for scripts/verify-windows-metadata.py.

The verifier is a signing gate: if it silently passed everything it would be
worse than not existing, because the packaging log would claim the metadata was
checked. There is no Windows binary on the Linux CI runner, so these tests build
synthetic but structurally valid PE files with real VS_VERSIONINFO resources and
assert that the verifier accepts the good tree and rejects each specific defect
it is meant to catch.
"""

from __future__ import annotations

import json
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
VERIFIER = HERE.parent / "scripts" / "verify-windows-metadata.py"
INVENTORY = HERE.parent / "packaging" / "windows" / "signing-inventory.json"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


# --- synthetic VS_VERSIONINFO ------------------------------------------------


def _pad4(blob: bytes) -> bytes:
    return blob + b"\0" * (-len(blob) % 4)


def _node(key: str, value: bytes, value_length: int, value_type: int,
          children: bytes = b"") -> bytes:
    head = _pad4(struct.pack("<HHH", 0, value_length, value_type)
                 + key.encode("utf-16-le") + b"\0\0")
    body = _pad4(head + value) + children
    return struct.pack("<H", len(body)) + body[2:]


def version_resource(strings: dict[str, str], version: tuple[int, int, int]) -> bytes:
    entries = b"".join(
        _node(key, (value + "\0").encode("utf-16-le"), len(value) + 1, 1)
        for key, value in strings.items()
    )
    table = _node("040904E4", b"", 0, 1, entries)
    string_file_info = _node("StringFileInfo", b"", 0, 1, table)
    major, minor, patch = version
    fixed = struct.pack(
        "<IIIIIIIIIIIII",
        0xFEEF04BD, 0x00010000,
        (major << 16) | minor, (patch << 16),
        (major << 16) | minor, (patch << 16),
        0x3F, 0x0, 0x40004, 0x1, 0x0, 0x0, 0x0,
    )
    return _node("VS_VERSION_INFO", fixed, len(fixed), 0, string_file_info)


# --- synthetic PE ------------------------------------------------------------


def write_pe(path: pathlib.Path, resource: bytes) -> None:
    section_rva = 0x2000
    section_raw = 0x400

    # Resource tree: root -> RT_VERSION(16) -> name 1 -> language 1033 -> data.
    def directory(entry_id: int, offset: int, is_directory: bool) -> bytes:
        header = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1)
        value = offset | (0x80000000 if is_directory else 0)
        return header + struct.pack("<II", entry_id, value)

    root = directory(16, 0x18, True)
    type_dir = directory(1, 0x30, True)
    name_dir = directory(1033, 0x48, False)
    data_entry_offset = 0x48
    resource_offset = 0x58
    data_entry = struct.pack(
        "<IIII", section_rva + resource_offset, len(resource), 0, 0
    )
    rsrc = bytearray(resource_offset + len(resource))
    rsrc[0x00:0x18] = root
    rsrc[0x18:0x30] = type_dir
    rsrc[0x30:0x48] = name_dir
    rsrc[data_entry_offset:data_entry_offset + 16] = data_entry
    rsrc[resource_offset:] = resource
    raw_size = (len(rsrc) + 0x1FF) & ~0x1FF
    rsrc = bytes(rsrc).ljust(raw_size, b"\0")

    e_lfanew = 0x80
    dos = bytearray(e_lfanew)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, e_lfanew)

    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x0022)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)          # PE32+
    struct.pack_into("<I", optional, 108, 16)           # NumberOfRvaAndSizes
    struct.pack_into("<II", optional, 112 + 2 * 8, section_rva, len(rsrc))

    section = struct.pack(
        "<8sIIIIIIHHI", b".rsrc\0\0\0", len(rsrc), section_rva,
        len(rsrc), section_raw, 0, 0, 0, 0, 0x40000040,
    )

    blob = bytearray()
    blob += dos
    blob += b"PE\0\0" + coff + bytes(optional) + section
    blob += b"\0" * (section_raw - len(blob))
    blob += rsrc
    path.write_bytes(bytes(blob))


LIGHTNING_STRINGS = {
    "CompanyName": "Rokas Smetonis",
    "FileDescription": "Lightning Matrix client",
    "FileVersion": "0.6.6",
    "InternalName": "Lightning",
    "LegalCopyright": "Copyright (C) 2026 Rokas Smetonis. GPL-3.0-or-later.",
    "OriginalFilename": "Lightning.exe",
    "ProductName": "Lightning",
    "ProductVersion": "0.6.6",
}

QT_STRINGS = {
    "CompanyName": "The Qt Company Ltd.",
    "FileDescription": "C++ Application Development Framework",
    "FileVersion": "6.11.1",
    "ProductName": "Qt6",
    "ProductVersion": "6.11.1",
}


def build_stage(root: pathlib.Path, lightning: dict[str, str] = LIGHTNING_STRINGS,
                qt: dict[str, str] | None = QT_STRINGS) -> pathlib.Path:
    stage = root / "Lightning"
    (stage / "plugins" / "platforms").mkdir(parents=True)
    write_pe(stage / "Lightning.exe", version_resource(lightning, (0, 6, 6)))
    if qt is not None:
        write_pe(stage / "Qt6Core.dll", version_resource(qt, (6, 11, 1)))
        write_pe(stage / "plugins" / "platforms" / "qwindows.dll",
                 version_resource(qt, (6, 11, 1)))
    return stage


def run_verifier(stage: pathlib.Path, report: pathlib.Path,
                 version: str = "0.6.6") -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(VERIFIER), "--stage", str(stage),
         "--inventory", str(INVENTORY), "--version", version,
         "--publisher", "Rokas Smetonis", "--report", str(report)],
        capture_output=True, text=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)

        print("a correct stage passes and is classified")
        stage = build_stage(root / "good")
        report = root / "good-report.json"
        result = run_verifier(stage, report)
        check(result.returncode == 0, f"correct stage accepted ({result.stderr.strip()})")
        if report.exists():
            data = json.loads(report.read_text())
            check(data["lightning_owned"] == ["Lightning.exe"],
                  "exactly one Lightning-owned PE recorded")
            check(data["upstream_count"] == 2, "both upstream PE files recorded")
            check(data["unclassified"] == [], "nothing unclassified in a known tree")
            owned = [f for f in data["files"] if f["sign_as_lightning"]]
            check(len(owned) == 1 and owned[0]["path"] == "Lightning.exe",
                  "only Lightning.exe is marked for signing")
            qt = [f for f in data["files"] if f["path"] == "Qt6Core.dll"][0]
            check(qt["sign_as_lightning"] is False,
                  "an upstream Qt DLL is never marked for signing")
            check(qt["product_name"] == "Qt6",
                  "upstream metadata is read, not rewritten")
        else:
            check(False, "report was written")

        print("a release-version mismatch fails")
        stage = build_stage(root / "badver", {**LIGHTNING_STRINGS,
                                              "ProductVersion": "0.6.5"})
        result = run_verifier(stage, root / "badver-report.json")
        check(result.returncode != 0, "mismatched ProductVersion rejected")
        check("ProductVersion" in result.stderr, "the failing field is named")

        print("a mismatched product name fails")
        stage = build_stage(root / "badname", {**LIGHTNING_STRINGS,
                                               "ProductName": "Lightning Client"})
        result = run_verifier(stage, root / "badname-report.json")
        check(result.returncode != 0, "mismatched ProductName rejected")

        print("a missing copyright fails")
        without = {k: v for k, v in LIGHTNING_STRINGS.items()
                   if k != "LegalCopyright"}
        stage = build_stage(root / "nocopyright", without)
        result = run_verifier(stage, root / "nocopyright-report.json")
        check(result.returncode != 0, "missing LegalCopyright rejected")

        print("an upstream binary impersonating Lightning fails")
        stage = build_stage(root / "impostor", LIGHTNING_STRINGS,
                            {**QT_STRINGS, "ProductName": "Lightning"})
        result = run_verifier(stage, root / "impostor-report.json")
        check(result.returncode != 0,
              "an upstream DLL claiming ProductName=Lightning is rejected")

        print("a missing Lightning-owned binary fails")
        stage = build_stage(root / "missing", LIGHTNING_STRINGS)
        (stage / "Lightning.exe").unlink()
        result = run_verifier(stage, root / "missing-report.json")
        check(result.returncode != 0, "absent Lightning.exe rejected")

        print("a publisher mismatch fails")
        stage = build_stage(root / "badpublisher",
                            {**LIGHTNING_STRINGS, "CompanyName": "Someone Else"})
        result = run_verifier(stage, root / "badpublisher-report.json")
        check(result.returncode != 0, "mismatched CompanyName rejected")

        print("an unknown upstream DLL warns but does not fail")
        stage = build_stage(root / "unknown")
        write_pe(stage / "mystery-runtime.dll", version_resource(
            {"ProductName": "Mystery", "ProductVersion": "1.0"}, (1, 0, 0)))
        report = root / "unknown-report.json"
        result = run_verifier(stage, report)
        check(result.returncode == 0, "an unclassified upstream DLL does not fail")
        check("mystery-runtime.dll" in result.stderr, "it is reported as a warning")
        data = json.loads(report.read_text())
        check(data["unclassified"] == ["mystery-runtime.dll"],
              "it is recorded as unclassified")

    check_uninstall_contract()

    if failures:
        print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("\nwindows metadata verifier: all checks passed")
    return 0


def check_uninstall_contract() -> None:
    """The setup EXE must stay uninstallable, per-user, and non-destructive.

    SignPath requires software that installs itself to provide uninstall
    facilities. These are source-level assertions because a real uninstall can
    only be exercised on Windows; the native acceptance script covers that.
    """
    print("the NSIS installer keeps its uninstall contract")
    nsi = (HERE.parent / "packaging" / "windows" / "installer.nsi").read_text()

    check("RequestExecutionLevel user" in nsi,
          "installs per-user, without administrator rights")
    check("WriteUninstaller" in nsi, "an uninstaller is written")

    uninstall_key = ("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
                     "\\Lightning")
    for value in ("DisplayName", "DisplayVersion", "Publisher", "DisplayIcon",
                  "UninstallString", "InstallLocation"):
        check(f'"{uninstall_key}" "{value}"' in nsi,
              f"the uninstall entry declares {value}")
    check(f'DeleteRegKey HKCU "{uninstall_key}"' in nsi,
          "uninstalling removes the uninstall entry")
    check('Delete "$SMPROGRAMS\\Lightning\\Lightning.lnk"' in nsi,
          "uninstalling removes the Start-menu shortcut")
    check('RMDir /r "$INSTDIR"' in nsi, "uninstalling removes the install tree")

    # The uninstaller must never recurse over a directory that is not a
    # verified Lightning install root, and must never delete user data.
    check(".lightning-install-root" in nsi,
          "recursive removal is guarded by an install marker")
    for destructive in ('RMDir /r "$APPDATA', 'RMDir /r "$LOCALAPPDATA"',
                        'RMDir /r "$DOCUMENTS', 'RMDir /r "$PROFILE'):
        check(destructive not in nsi,
              f"uninstall does not delete user data ({destructive!r})")

    # No system-wide modifications: SignPath requires system changes to be
    # intentional and disclosed, and Lightning makes none of these.
    for forbidden, what in (
        ("SetEnvironmentVariable", "environment variables"),
        ("EnVar::", "PATH modification"),
        ('WriteRegStr HKCR', "file associations / URL protocols"),
        ("HKLM", "machine-wide registry keys"),
        ("$SMSTARTUP", "autostart entries"),
        ("nsExec::Exec", "arbitrary command execution"),
    ):
        check(forbidden not in nsi, f"the installer makes no {what}")


if __name__ == "__main__":
    sys.exit(main())
