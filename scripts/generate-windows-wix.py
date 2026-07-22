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
        "Version": args.version, "Manufacturer": "Mizerd",
        "UpgradeCode": guid(UPGRADE_CODE),
    })
    ET.SubElement(product, tag("Package"), {
        "InstallerVersion": "500", "Compressed": "yes",
        "InstallScope": "perUser",
        "Description": "Lightning unsigned Windows test package",
        "Manufacturer": "Mizerd",
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

    target = ET.SubElement(product, tag("Directory"), {"Id": "TARGETDIR", "Name": "SourceDir"})
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

    shortcut_component = ET.SubElement(menu_dir, tag("Component"), {
        "Id": "StartMenuShortcutComponent",
        "Guid": guid(uuid.uuid5(COMPONENT_NAMESPACE, "start-menu-shortcut")),
        "Win64": "yes",
    })
    ET.SubElement(shortcut_component, tag("Shortcut"), {
        "Id": "StartMenuShortcut", "Name": "Lightning",
        "Description": "Lightning Matrix client", "Target": "[INSTALLFOLDER]Lightning.exe",
        "WorkingDirectory": "INSTALLFOLDER", "Icon": "LightningIcon",
    })
    ET.SubElement(shortcut_component, tag("RemoveFolder"), {
        "Id": "RemoveStartMenuFolder", "On": "uninstall"
    })
    ET.SubElement(shortcut_component, tag("RegistryValue"), {
        "Root": "HKCU", "Key": "Software\\Mizerd\\Lightning",
        "Name": "installed", "Type": "integer", "Value": "1", "KeyPath": "yes",
    })
    component_ids.append("StartMenuShortcutComponent")

    feature = ET.SubElement(product, tag("Feature"), {
        "Id": "MainFeature", "Title": "Lightning", "Level": "1",
    })
    for component_id in component_ids:
        ET.SubElement(feature, tag("ComponentRef"), {"Id": component_id})

    ET.indent(wix, space="  ")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(wix).write(args.output, encoding="utf-8", xml_declaration=True)
    args.metadata.write_text(json.dumps({
        "manufacturer": "Mizerd", "product_name": "Lightning",
        "product_version": args.version, "product_code": guid(product_code),
        "upgrade_code": guid(UPGRADE_CODE), "platform": "x64",
        "component_guid_strategy": "UUIDv5 of lowercase staged relative path",
        "product_code_strategy": "UUIDv5 of semantic version and source commit",
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
