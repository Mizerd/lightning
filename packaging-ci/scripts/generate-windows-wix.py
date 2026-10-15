#!/usr/bin/env python3
"""Generate deterministic WiX 3 source for the staged Lightning runtime."""

import argparse
import hashlib
import json
import pathlib
import re
import struct
import uuid
import xml.etree.ElementTree as ET

NS = "http://schemas.microsoft.com/wix/2006/wi"
ET.register_namespace("", NS)
UPGRADE_CODE = uuid.UUID("6e186049-dbeb-5d4d-93f6-cfcff4ff327e")
PRODUCT_NAMESPACE = uuid.UUID("fdb58318-5f2c-5cf5-aa6f-aa35f8b1208a")
COMPONENT_NAMESPACE = uuid.UUID("82cf5f7d-d7ef-5f37-b0b8-8745f5ca67e0")
# Read by src/update/InstallType.cpp; the NSIS installer writes the same name.
INSTALL_SCOPE_MARKER = ".lightning-install-scope"


def tag(name: str) -> str:
    return f"{{{NS}}}{name}"


def wix_id(prefix: str, value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.]", "_", value)
    digest = hashlib.sha256(value.encode()).hexdigest()[:12]
    return f"{prefix}_{cleaned[:40]}_{digest}"


def guid(value: uuid.UUID) -> str:
    return str(value).upper()


# The RT_VERSION resource: the UTF-16 key "VS_VERSION_INFO", padded to a
# 32-bit boundary, then VS_FIXEDFILEINFO (dwSignature, dwStrucVersion 1.0,
# then the file version). Anchoring on the key keeps a stray signature in code
# or data from being read as the version.
VERSION_INFO_KEY = "VS_VERSION_INFO\0".encode("utf-16-le")
FIXED_FILE_INFO = struct.pack("<II", 0xFEEF04BD, 0x00010000)


def pe_file_version(path: pathlib.Path) -> str | None:
    """The PE file version ("a.b.c.d") Windows Installer will read from disk.

    The File table must carry it. wixl leaves Version empty, and an empty
    Version makes Windows Installer treat the new file as unversioned, which
    always loses to the versioned copy already installed. On a major upgrade
    every versioned component (Lightning.exe included, 0.10.1.0 -> 0.10.2.0)
    was then "disallowed since the same component with higher versioned
    keyfile exists", and removing the old product deleted the files: 133
    missing, measured on Windows 11 with the 0.10.2 candidate.
    """
    with path.open("rb") as handle:
        if handle.read(2) != b"MZ":
            return None
        data = handle.read()
    index = -1
    key = data.find(VERSION_INFO_KEY)
    while key >= 0 and index < 0:
        # The fixed info follows the key within its 32-bit padding.
        candidate = data.find(FIXED_FILE_INFO, key + len(VERSION_INFO_KEY),
                              key + len(VERSION_INFO_KEY) + 4 + len(FIXED_FILE_INFO))
        if candidate >= 0:
            index = candidate
        else:
            key = data.find(VERSION_INFO_KEY, key + 1)
    if index < 0:
        return None
    ms, ls = struct.unpack_from("<II", data, index + len(FIXED_FILE_INFO))
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--metadata", type=pathlib.Path, required=True)
    # Publisher and signing state are decided once in build-windows.sh so the
    # MSI cannot disagree with the executable it installs.
    parser.add_argument("--manufacturer", required=True)
    parser.add_argument("--signing-state", required=True,
                        choices=("signed", "unsigned"))
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        raise SystemExit("MSI version must be three numeric fields")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        raise SystemExit("source SHA must be a full lowercase commit ID")

    product_code = uuid.uuid5(
        PRODUCT_NAMESPACE, f"Lightning:{args.version}:{args.source_sha}"
    )
    wix = ET.Element(tag("Wix"))
    product = ET.SubElement(wix, tag("Product"), {
        "Id": guid(product_code), "Name": "Lightning", "Language": "1033",
        "Version": args.version, "Manufacturer": args.manufacturer,
        "UpgradeCode": guid(UPGRADE_CODE),
    })
    # One package, two scopes (GitHub issue #14).
    #
    #   * InstallScope="perUser" sets bit 3 of the summary Word Count ("no
    #     elevation required") and sets no ALLUSERS, so a plain install (a
    #     double-click, or the updater of a per-user copy) is per-user in
    #     %LOCALAPPDATA%\Programs\Lightning with no UAC prompt.
    #   * `msiexec /i Lightning.msi ALLUSERS=1` selects per-machine (Program
    #     Files, HKLM, all-users Start menu). Run it elevated: a package that
    #     declares it needs no elevation will not ask for it.
    #
    # Not Microsoft's dual-purpose recipe (ALLUSERS=2 + MSIINSTALLPERUSER=1):
    # Wine does not implement MSIINSTALLPERUSER and installs such a package
    # per-machine, so the Wine smoke test would no longer cover the default.
    #
    # Do not use Root="HKMU": wixl 0.106 writes it as 4, not the -1 the
    # Registry table requires. Per-scope values are two conditioned components.
    ET.SubElement(product, tag("Package"), {
        "InstallerVersion": "500", "Compressed": "yes",
        "InstallScope": "perUser",
        "Description": f"Lightning {args.version} ({args.signing_state})",
        "Manufacturer": args.manufacturer,
    })
    # The old product is removed AFTER the new one is installed, so a component
    # both versions share (one per staged path, same GUID) is only
    # dereferenced and never deleted. Under the default afterInstallValidate
    # the removal runs after CostFinalize has already disallowed every
    # component whose installed keyfile is the higher version, so those are
    # registered, never copied, and then deleted with the old product. This
    # relies on the component rules the GUIDs below follow: one file per
    # component, keyed by its path, with its real version recorded.
    #
    # The cost: a file whose bytes change while its version does not (a
    # toolchain or Qt rebuild at the same version; libwinpthread carries a
    # placeholder 1.0.0.0) is not replaced on upgrade. 0.10.1 -> 0.10.2 has
    # none; the updater's REINSTALLMODE covers its own path.
    ET.SubElement(product, tag("MajorUpgrade"), {
        "DowngradeErrorMessage": "A newer version of Lightning is already installed.",
    })
    ET.SubElement(product, tag("MediaTemplate"), {"EmbedCab": "yes"})
    ET.SubElement(product, tag("Property"), {"Id": "ARPNOMODIFY", "Value": "1"})
    ET.SubElement(product, tag("Icon"), {
        "Id": "LightningIcon", "SourceFile": str(args.stage / "Lightning.ico")
    })
    ET.SubElement(product, tag("Property"), {
        "Id": "ARPPRODUCTICON", "Value": "LightningIcon"
    })

    # Per-machine directory: for ALLUSERS=1 a type-51 action re-points the
    # parent "Programs" directory at Program Files before CostFinalize (as
    # WiX's SetDirectory does). Setting the parent keeps an explicit
    # INSTALLFOLDER on the command line authoritative. It is in both sequences
    # because /qn and /qb skip the UI sequence.
    ET.SubElement(product, tag("CustomAction"), {
        "Id": "LightningPerMachineProgramsDir", "Property": "ProgramsDir",
        "Value": "[ProgramFiles64Folder]",
    })
    sequences = {name: ET.SubElement(product, tag(name))
                 for name in ("InstallExecuteSequence", "InstallUISequence")}
    for sequence in sequences.values():
        ET.SubElement(sequence, tag("Custom"), {
            "Action": "LightningPerMachineProgramsDir", "Before": "CostFinalize",
        }).text = "ALLUSERS=1"
    # Where MajorUpgrade's Schedule="afterInstallFinalize" would put it; wixl
    # 0.106 has no Schedule property and ignores it with a GLib warning.
    ET.SubElement(sequences["InstallExecuteSequence"], tag("RemoveExistingProducts"), {
        "After": "InstallFinalize",
    })

    target = ET.SubElement(product, tag("Directory"), {"Id": "TARGETDIR", "Name": "SourceDir"})
    ET.SubElement(target, tag("Directory"), {"Id": "ProgramFiles64Folder"})
    local = ET.SubElement(target, tag("Directory"), {"Id": "LocalAppDataFolder"})
    programs = ET.SubElement(local, tag("Directory"), {"Id": "ProgramsDir", "Name": "Programs"})
    install = ET.SubElement(programs, tag("Directory"), {"Id": "INSTALLFOLDER", "Name": "Lightning"})
    menu = ET.SubElement(target, tag("Directory"), {"Id": "ProgramMenuFolder"})
    menu_dir = ET.SubElement(menu, tag("Directory"), {"Id": "ProgramMenuDir", "Name": "Lightning"})

    directory_nodes = {pathlib.PurePosixPath("."): install}
    component_ids: list[str] = []
    versioned_files = 0
    for path in sorted((p for p in args.stage.rglob("*") if p.is_file()), key=lambda p: p.as_posix().casefold()):
        relative = pathlib.PurePosixPath(path.relative_to(args.stage).as_posix())
        parent = pathlib.PurePosixPath(".")
        node = install
        for part in relative.parent.parts:
            if part == ".":
                continue
            parent = parent / part
            if parent not in directory_nodes:
                directory_nodes[parent] = ET.SubElement(node, tag("Directory"), {
                    "Id": wix_id("DIR", parent.as_posix()), "Name": part,
                })
            node = directory_nodes[parent]
        component_id = wix_id("CMP", relative.as_posix())
        component_ids.append(component_id)
        component = ET.SubElement(node, tag("Component"), {
            "Id": component_id,
            "Guid": guid(uuid.uuid5(COMPONENT_NAMESPACE, relative.as_posix().casefold())),
            "Win64": "yes",
        })
        file_attributes = {
            "Id": wix_id("FIL", relative.as_posix()), "Source": str(path),
            "Name": path.name, "KeyPath": "yes",
        }
        version = pe_file_version(path)
        if version is not None:
            file_attributes["DefaultVersion"] = version
            versioned_files += 1
        ET.SubElement(component, tag("File"), file_attributes)

    # ALLUSERS is "1" per-machine and unset per-user (Windows Installer resets
    # ALLUSERS=2 to one of those), so these conditions partition every install.
    # The per-user half keeps the component Id and GUID earlier MSIs used.
    per_machine = "ALLUSERS=1"
    per_user = "NOT ALLUSERS=1"

    for suffix, condition, root, guid_key in (
        ("", per_user, "HKCU", "start-menu-shortcut"),
        ("Machine", per_machine, "HKLM", "start-menu-shortcut-machine"),
    ):
        component_id = f"StartMenuShortcut{suffix}Component"
        shortcut_component = ET.SubElement(menu_dir, tag("Component"), {
            "Id": component_id,
            "Guid": guid(uuid.uuid5(COMPONENT_NAMESPACE, guid_key)),
            "Win64": "yes",
        })
        ET.SubElement(shortcut_component, tag("Condition")).text = condition
        ET.SubElement(shortcut_component, tag("Shortcut"), {
            "Id": f"StartMenuShortcut{suffix}", "Name": "Lightning",
            "Description": "Lightning Matrix client", "Target": "[INSTALLFOLDER]Lightning.exe",
            "WorkingDirectory": "INSTALLFOLDER", "Icon": "LightningIcon",
        })
        ET.SubElement(shortcut_component, tag("RemoveFolder"), {
            "Id": f"RemoveStartMenuFolder{suffix}", "On": "uninstall"
        })
        ET.SubElement(shortcut_component, tag("RegistryValue"), {
            "Root": root, "Key": "Software\\Mizerd\\Lightning",
            "Name": "installed", "Type": "integer", "Value": "1", "KeyPath": "yes",
        })
        if root == "HKLM":
            # HKLM is what makes the "machine" scope marker believable to the
            # updater (src/update/InstallType.cpp): the marker file is
            # user-writable in a per-user install. Not "InstallDir", which is
            # the setup EXE's value.
            ET.SubElement(shortcut_component, tag("RegistryValue"), {
                "Root": "HKLM", "Key": "Software\\Mizerd\\Lightning",
                "Name": "MsiInstallDir", "Type": "string", "Value": "[INSTALLFOLDER]",
            })
        component_ids.append(component_id)

    # The scope marker. The updater must upgrade in the same context: a
    # per-machine product upgraded without ALLUSERS=1 is not found by
    # FindRelatedProducts and a second, per-user copy appears. wixl cannot
    # write a file's content at install time, so the MSI carries both versions
    # under opposite conditions and exactly one lands.
    marker_dir = args.output.parent / "install-scope-markers"
    marker_dir.mkdir(parents=True, exist_ok=True)
    for scope, condition in (("user", per_user), ("machine", per_machine)):
        source = marker_dir / scope
        source.write_bytes(f"{scope}\r\n".encode("ascii"))
        component_id = f"InstallScopeMarker_{scope}"
        component = ET.SubElement(install, tag("Component"), {
            "Id": component_id,
            "Guid": guid(uuid.uuid5(COMPONENT_NAMESPACE, f"install-scope-marker-{scope}")),
            "Win64": "yes",
        })
        ET.SubElement(component, tag("Condition")).text = condition
        ET.SubElement(component, tag("File"), {
            "Id": f"InstallScopeMarkerFile_{scope}", "Source": str(source),
            "Name": INSTALL_SCOPE_MARKER, "KeyPath": "yes",
        })
        component_ids.append(component_id)

    feature = ET.SubElement(product, tag("Feature"), {
        "Id": "MainFeature", "Title": "Lightning", "Level": "1",
    })
    for component_id in component_ids:
        ET.SubElement(feature, tag("ComponentRef"), {"Id": component_id})

    ET.indent(wix, space="  ")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(wix).write(args.output, encoding="utf-8", xml_declaration=True)
    args.metadata.write_text(json.dumps({
        "manufacturer": args.manufacturer, "product_name": "Lightning",
        "signing_state": args.signing_state,
        "product_version": args.version, "product_code": guid(product_code),
        "upgrade_code": guid(UPGRADE_CODE), "platform": "x64",
        "component_guid_strategy": "UUIDv5 of lowercase staged relative path",
        "product_code_strategy": "UUIDv5 of semantic version and source commit",
        "install_scopes": ["per-user (default)", "per-machine (ALLUSERS=1)"],
        "versioned_files": versioned_files,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
