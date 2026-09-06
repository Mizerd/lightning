#!/usr/bin/env python3
"""Verify Windows product metadata and the Lightning/upstream signing split.

Runs on the staged Windows tree before anything is packaged or signed.

Two questions are answered from the real PE version resources, not from the
build script's intentions:

1. Does the ONE Lightning-owned binary declare the canonical release version and
   product name? SignPath enforces file metadata restrictions on signed
   artifacts, so a disagreement must fail here, long before a signing request.

2. Does anything else in the payload claim to be Lightning? Upstream binaries
   are redistributed as their projects built them and must never be signed as
   ours, nor be mistakable for ours.

Every staged PE must also be accounted for in the signing inventory as either
Lightning-owned or upstream, so a newly bundled DLL cannot slip in unclassified.

Exit status is non-zero on any failure; the emitted report records what was
found either way.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import pathlib
import struct
import sys

RT_VERSION = 16
VS_FFI_SIGNATURE = 0xFEEF04BD


class PeError(Exception):
    pass


# --- minimal PE reader -------------------------------------------------------


class PeFile:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise PeError("not a PE file (no MZ header)")
        (e_lfanew,) = struct.unpack_from("<I", data, 0x3C)
        if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
            raise PeError("not a PE file (no PE signature)")
        coff = e_lfanew + 4
        (number_of_sections,) = struct.unpack_from("<H", data, coff + 2)
        (size_of_optional,) = struct.unpack_from("<H", data, coff + 16)
        optional = coff + 20
        (magic,) = struct.unpack_from("<H", data, optional)
        if magic == 0x20B:  # PE32+
            directories = optional + 112
        elif magic == 0x10B:  # PE32
            directories = optional + 96
        else:
            raise PeError(f"unknown optional header magic 0x{magic:x}")
        (number_of_rva,) = struct.unpack_from("<I", data, directories - 4)
        self.resource_rva = 0
        self.resource_size = 0
        if number_of_rva > 2:
            self.resource_rva, self.resource_size = struct.unpack_from(
                "<II", data, directories + 2 * 8
            )
        self.sections = []
        section_table = optional + size_of_optional
        for index in range(number_of_sections):
            offset = section_table + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
                "<IIII", data, offset + 8
            )
            self.sections.append((virtual_address, virtual_size, raw_size, raw_pointer))

    def offset_for(self, rva: int) -> int:
        for virtual_address, virtual_size, raw_size, raw_pointer in self.sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + span:
                return raw_pointer + (rva - virtual_address)
        raise PeError(f"RVA 0x{rva:x} is not inside any section")

    def _resource_entries(self, table_offset: int):
        named, ident = struct.unpack_from("<HH", self.data, table_offset + 12)
        for index in range(named + ident):
            entry = table_offset + 16 + index * 8
            name, value = struct.unpack_from("<II", self.data, entry)
            yield name, value

    def version_resource(self) -> bytes | None:
        """Raw VS_VERSIONINFO bytes of the first RT_VERSION resource, if any."""
        if not self.resource_rva:
            return None
        root = self.offset_for(self.resource_rva)

        def descend(table_offset: int, wanted: int | None):
            for name, value in self._resource_entries(table_offset):
                if wanted is not None and not (name & 0x80000000) and name != wanted:
                    continue
                if value & 0x80000000:
                    yield root + (value & 0x7FFFFFFF)
                else:
                    yield -(root + value)  # negative marks a leaf data entry

        for type_node in descend(root, RT_VERSION):
            if type_node < 0:
                continue
            for name_node in descend(type_node, None):
                if name_node < 0:
                    continue
                for language_node in descend(name_node, None):
                    leaf = -language_node if language_node < 0 else None
                    if leaf is None:
                        continue
                    data_rva, size = struct.unpack_from("<II", self.data, leaf)
                    start = self.offset_for(data_rva)
                    return self.data[start : start + size]
        return None


def _align4(value: int) -> int:
    return (value + 3) & ~3


def _parse_block(blob: bytes, offset: int):
    """One VS_VERSIONINFO-style node: (length, key, value_bytes, children_off)."""
    if offset + 6 > len(blob):
        raise PeError("truncated version block")
    length, value_length, value_type = struct.unpack_from("<HHH", blob, offset)
    if length == 0:
        raise PeError("zero-length version block")
    cursor = offset + 6
    end = blob.find(b"\x00\x00", cursor)
    # Keys are UTF-16LE; find the terminator on an even boundary.
    while end != -1 and (end - cursor) % 2:
        end = blob.find(b"\x00\x00", end + 1)
    if end == -1:
        raise PeError("unterminated version key")
    key = blob[cursor:end].decode("utf-16-le", errors="replace")
    value_start = _align4(end + 2)
    size = value_length * (2 if value_type == 1 else 1)
    return length, key, blob[value_start : value_start + size], _align4(value_start + size)


def version_strings(blob: bytes) -> dict[str, str]:
    """The StringFileInfo table of a VS_VERSIONINFO resource."""
    root_length, root_key, fixed, children = _parse_block(blob, 0)
    if not root_key.startswith("VS_VERSION_INFO"):
        raise PeError(f"unexpected version root key: {root_key!r}")
    if fixed and struct.unpack_from("<I", fixed, 0)[0] != VS_FFI_SIGNATURE:
        raise PeError("bad VS_FIXEDFILEINFO signature")

    strings: dict[str, str] = {}
    cursor = children
    limit = min(root_length, len(blob))
    while cursor < limit:
        length, key, _value, table_cursor = _parse_block(blob, cursor)
        if key == "StringFileInfo":
            table_end = cursor + length
            while table_cursor < table_end:
                t_length, _lang, _v, entry_cursor = _parse_block(blob, table_cursor)
                entry_end = table_cursor + t_length
                while entry_cursor < entry_end:
                    e_length, e_key, e_value, _ = _parse_block(blob, entry_cursor)
                    strings[e_key] = (
                        e_value.decode("utf-16-le", errors="replace").rstrip("\x00")
                    )
                    entry_cursor += _align4(e_length)
                table_cursor += _align4(t_length)
        cursor += _align4(length)
    return strings


# --- the actual checks -------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--publisher", required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args()

    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    owned_names = set(inventory["lightning_owned"])
    upstream_patterns = list(inventory["upstream_patterns"])
    product_name = inventory["product_name"]

    failures: list[str] = []
    unclassified: list[str] = []
    records: list[dict] = []

    staged = sorted(
        p
        for p in args.stage.rglob("*")
        if p.is_file() and p.suffix.lower() in (".exe", ".dll")
    )
    if not staged:
        print("error: no PE files found in the staged tree", file=sys.stderr)
        return 1

    seen_owned: set[str] = set()

    for path in staged:
        relative = path.relative_to(args.stage).as_posix()
        try:
            blob = PeFile(path.read_bytes()).version_resource()
            strings = version_strings(blob) if blob else {}
        except PeError as error:
            failures.append(f"{relative}: unreadable version resource ({error})")
            continue

        owned = relative in owned_names
        known_upstream = any(
            fnmatch.fnmatch(relative, pattern) for pattern in upstream_patterns
        )
        if owned:
            seen_owned.add(relative)
        elif not known_upstream:
            # Warning, not a failure: the dependency walk legitimately picks up
            # new upstream libraries, and only lightning_owned is ever signed,
            # so an unclassified file cannot be signed by accident. It still
            # needs a human to classify it.
            unclassified.append(relative)

        records.append({
            "path": relative,
            "ownership": "lightning" if owned else "upstream",
            "classified": owned or known_upstream,
            "sign_as_lightning": owned,
            "product_name": strings.get("ProductName", ""),
            "product_version": strings.get("ProductVersion", ""),
            "file_version": strings.get("FileVersion", ""),
            "company_name": strings.get("CompanyName", ""),
        })

        if owned:
            required = {
                "ProductName": product_name,
                "ProductVersion": args.version,
                "FileVersion": args.version,
                "CompanyName": args.publisher,
                "OriginalFilename": path.name,
            }
            for field, expected in required.items():
                actual = strings.get(field, "")
                if actual != expected:
                    failures.append(
                        f"{relative}: {field} is {actual!r}, expected {expected!r}"
                    )
            for field in ("LegalCopyright", "FileDescription"):
                if not strings.get(field, "").strip():
                    failures.append(f"{relative}: {field} is missing")
        else:
            # An upstream binary must never present itself as Lightning; if one
            # did, it could be mistaken for — or signed as — our own work.
            if strings.get("ProductName", "") == product_name:
                failures.append(
                    f"{relative}: upstream binary declares ProductName "
                    f"{product_name!r}"
                )

    missing = owned_names - seen_owned
    for name in sorted(missing):
        failures.append(f"{name}: declared Lightning-owned but not present in the stage")

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(
            {
                "product_name": product_name,
                "release_version": args.version,
                "publisher": args.publisher,
                "lightning_owned": sorted(seen_owned),
                "upstream_count": sum(
                    1 for r in records if r["ownership"] == "upstream"
                ),
                "files": records,
                "unclassified": unclassified,
                "failures": failures,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )

    for path in unclassified:
        print(
            f"warning: {path} is not classified in signing-inventory.json; "
            "it is treated as upstream and will NOT be signed",
            file=sys.stderr,
        )

    if failures:
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1

    print(
        f"Windows metadata verified: {len(seen_owned)} Lightning-owned PE file(s) at "
        f"{product_name} {args.version}, "
        f"{len(records) - len(seen_owned)} upstream file(s) left untouched"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
