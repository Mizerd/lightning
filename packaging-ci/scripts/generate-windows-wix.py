#!/usr/bin/env python3
"""Generate deterministic WiX 3 source for the staged Lightning runtime."""

import argparse
import hashlib
import json
import pathlib
import re
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--metadata", type=pathlib.Path, required=True)
    # The publisher a user sees in Programs and Features, and whether this
    # build is signed, are decided once in build-windows.sh and passed in, so
    # the MSI can never disagree with the executable it installs.
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
    # ONE PACKAGE, TWO SCOPES (GitHub issue #14).
    #
    #   * InstallScope="perUser" sets bit 3 of the summary Word Count ("no
    #     elevation required") and adds NO ALLUSERS property, so an install
    #     with no properties -- a double-click, or the in-app updater of a
    #     per-user copy -- is per-user, lands in
    #     %LOCALAPPDATA%\Programs\Lightning and never shows a UAC prompt:
    #     byte-for-byte the layout every earlier MSI had.
    #   * `msiexec /i Lightning.msi ALLUSERS=1` selects the per-machine context
    #     (Program Files, HKLM, the all-users Start menu). Run it elevated --
    #     from an administrator prompt, Intune, SCCM, WAPT or GPO, all of which
    #     run as SYSTEM or an administrator -- because a package that declares
    #     it needs no elevation cannot be relied on to ask for it.
    #
    # WHY NOT Microsoft's "dual-purpose" recipe (ALLUSERS=2 +
    # MSIINSTALLPERUSER=1 with the tree under ProgramFiles64Folder, which
    # Windows maps to %LOCALAPPDATA%\Programs per-user): Wine does not
    # implement MSIINSTALLPERUSER. Measured with wixl 0.106 + Wine 11, that
    # package installs a plain double-click PER-MACHINE into Program Files, so
    # the only automated MSI install this project runs would stop describing
    # the default a user gets. ALLUSERS unset means per-user on Wine and on
    # every Windows alike, and the per-machine directory is chosen below by a
    # property-setting action instead.
    #
    # DO NOT use Root="HKMU" for the registry values: wixl 0.106 writes it as
    # 4, not the -1 the Registry table requires. Per-scope values are two
    # components with opposite conditions instead.
    ET.SubElement(product, tag("Package"), {
        "InstallerVersion": "500", "Compressed": "yes",
        "InstallScope": "perUser",
        "Description": f"Lightning {args.version} ({args.signing_state})",
        "Manufacturer": args.manufacturer,
    })
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

    # PER-MACHINE DIRECTORY. The tree is the per-user one (LocalAppDataFolder
    # \Programs\Lightning, unchanged); for ALLUSERS=1 a type-51 action
    # re-points its "Programs" directory at Program Files before CostFinalize
    # resolves anything, which is exactly how WiX's own SetDirectory works. It
    # sets the PARENT, not INSTALLFOLDER, so an administrator's explicit
    # INSTALLFOLDER=D:\Apps\Lightning on the command line still wins. It is
    # in both sequences because /qn and /qb skip the UI sequence.
    ET.SubElement(product, tag("CustomAction"), {
        "Id": "LightningPerMachineProgramsDir", "Property": "ProgramsDir",
        "Value": "[ProgramFiles64Folder]",
    })
    for sequence in ("InstallExecuteSequence", "InstallUISequence"):
        ET.SubElement(ET.SubElement(product, tag(sequence)), tag("Custom"), {
            "Action": "LightningPerMachineProgramsDir", "Before": "CostFinalize",
        }).text = "ALLUSERS=1"

    target = ET.SubElement(product, tag("Directory"), {"Id": "TARGETDIR", "Name": "SourceDir"})
    ET.SubElement(target, tag("Directory"), {"Id": "ProgramFiles64Folder"})
    local = ET.SubElement(target, tag("Directory"), {"Id": "LocalAppDataFolder"})
    programs = ET.SubElement(local, tag("Directory"), {"Id": "ProgramsDir", "Name": "Programs"})
    install = ET.SubElement(programs, tag("Directory"), {"Id": "INSTALLFOLDER", "Name": "Lightning"})
    menu = ET.SubElement(target, tag("Directory"), {"Id": "ProgramMenuFolder"})
    menu_dir = ET.SubElement(menu, tag("Directory"), {"Id": "ProgramMenuDir", "Name": "Lightning"})

    directory_nodes = {pathlib.PurePosixPath("."): install}
    component_ids: list[str] = []
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
        ET.SubElement(component, tag("File"), {
            "Id": wix_id("FIL", relative.as_posix()), "Source": str(path),
            "Name": path.name, "KeyPath": "yes",
        })

    # ALLUSERS is "1" in the per-machine context and unset in the per-user one
    # (Windows Installer also resets an ALLUSERS=2 to one of those two), so
    # these two conditions partition every install. The per-user half keeps the component Id and GUID the per-user-
    # only MSI always used.
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
            # What makes the "machine" scope marker believable to the in-app
            # updater (src/update/InstallType.cpp): only an administrator can
            # write HKLM, while the marker file is user-writable in a per-user
            # install. NOT "InstallDir" -- that is the setup EXE's value, and
            # the NSIS installer would take it for its own installation.
            ET.SubElement(shortcut_component, tag("RegistryValue"), {
                "Root": "HKLM", "Key": "Software\\Mizerd\\Lightning",
                "Name": "MsiInstallDir", "Type": "string", "Value": "[INSTALLFOLDER]",
            })
        component_ids.append(component_id)

    # THE SCOPE MARKER. The in-app updater must hand an upgrade to the SAME
    # context: a per-machine product upgraded without ALLUSERS=1 is not found
    # by FindRelatedProducts (it is context-scoped) and a second, per-user copy
    # appears beside it. The NSIS installer writes the same file at install
    # time; an MSI cannot write a file's CONTENT at install time with wixl, so
    # it carries both versions under opposite conditions and exactly one lands.
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
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
