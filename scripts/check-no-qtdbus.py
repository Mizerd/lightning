#!/usr/bin/env python3
"""Syntax-check the app's QDBus-naming sources as Windows and macOS compile them.

Usage: check-no-qtdbus.py <build-dir>. The tree must be configured with
CMAKE_EXPORT_COMPILE_COMMANDS=ON and its lightning-matrix_autogen target built.
"""
import json
import pathlib
import re
import shlex
import subprocess
import sys

APP_OBJECTS = "/CMakeFiles/lightning-matrix.dir/"


def main() -> int:
    build = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    src = root / "src"

    naming = {p for p in src.rglob("*") if p.suffix in (".cpp", ".h") and "QDBus" in p.read_text(errors="replace")}
    headers = {p.name for p in naming if p.suffix == ".h"}
    include = re.compile(r'#\s*include\s*[<"](?:[^">]*/)?([^/">]+\.h)[">]')
    wanted = {p for p in naming if p.suffix == ".cpp"}
    for p in src.rglob("*.cpp"):
        if headers.intersection(include.findall(p.read_text(errors="replace"))):
            wanted.add(p)

    entries = {}
    for e in json.loads((build / "compile_commands.json").read_text()):
        if APP_OBJECTS in e.get("output", "") or APP_OBJECTS in e.get("command", ""):
            entries[pathlib.Path(e["directory"], e["file"]).resolve()] = e

    missing = sorted(str(p.relative_to(root)) for p in wanted if p not in entries)
    if missing:
        print("not compiled into lightning-matrix:", *missing, sep="\n  ")

    checked, failed = 0, []
    for path in sorted(p for p in wanted if p in entries):
        e = entries[path]
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        if pathlib.Path(args[0]).name in ("ccache", "sccache"):
            args = args[1:]
        out, stripped, skip = [], 0, False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
                continue
            if a in ("-MD", "-MMD"):
                continue
            if re.fullmatch(r"-DHAVE_QT_DBUS(=.*)?", a):
                stripped += 1
                continue
            out.append(a)
        rel = path.relative_to(root)
        if stripped == 0:
            failed.append(str(rel))
            print(f"FAIL {rel}: compile command has no -DHAVE_QT_DBUS to strip")
            continue
        r = subprocess.run(out + ["-fsyntax-only"], cwd=e["directory"], capture_output=True, text=True)
        checked += 1
        if r.returncode != 0:
            failed.append(str(rel))
            print(f"FAIL {rel}\n{r.stderr}")
        else:
            print(f"ok   {rel}")

    print(f"no-QtDBus syntax check: {checked} checked, {len(failed)} failed, {len(missing)} not in the app")
    return 1 if failed or missing or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
