#!/usr/bin/env python3
"""Create a minimal, dependency-closed Windows deployment directory."""

import argparse
import json
import pathlib
import re
import shutil
import subprocess

SYSROOT = pathlib.Path("/usr/x86_64-w64-mingw32/sys-root/mingw")
QT_ROOT = SYSROOT / "lib/qt6"
SYSTEM_DLLS = {
    "advapi32.dll", "authz.dll", "avrt.dll", "bcrypt.dll",
    "bcryptprimitives.dll", "cfgmgr32.dll",
    "comdlg32.dll", "crypt32.dll", "d3d11.dll", "d3d12.dll",
    "d3dcompiler_47.dll", "d3d9.dll", "d2d1.dll", "dbghelp.dll",
    "dnsapi.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll", "dxva2.dll",
    "evr.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    "mf.dll", "mfplat.dll", "mfreadwrite.dll", "mfuuid.dll", "mpr.dll",
    "msvcrt.dll", "netapi32.dll", "ncrypt.dll", "ntdll.dll", "ole32.dll",
    "oleaut32.dll", "opengl32.dll", "powrprof.dll", "propsys.dll",
    "rpcrt4.dll", "secur32.dll", "setupapi.dll", "shcore.dll",
    "shell32.dll", "shlwapi.dll", "user32.dll", "userenv.dll",
    "uxtheme.dll", "version.dll", "winhttp.dll", "winmm.dll",
    "wldap32.dll", "wtsapi32.dll", "ws2_32.dll",
}

PLUGIN_FILES = {
    "iconengines": ("qsvgicon.dll",),
    "imageformats": (
        "qgif.dll", "qico.dll", "qjpeg.dll", "qsvg.dll", "qtiff.dll",
        "qwebp.dll",
    ),
    "multimedia": ("windowsmediaplugin.dll",),
    "networkinformation": ("qnetworklistmanager.dll",),
    "platforms": ("qwindows.dll",),
    "sqldrivers": ("qsqlite.dll",),
    "styles": ("qmodernwindowsstyle.dll",),
    "tls": ("qcertonlybackend.dll", "qschannelbackend.dll"),
}

QML_RUNTIME_ENTRIES = (
    "QML", "Qt", "QtCore", "QtMultimedia", "QtNetwork", "QtQml", "QtQuick",
)


def copy_tree(source: pathlib.Path, destination: pathlib.Path) -> None:
    if not source.is_dir():
        raise SystemExit(f"required runtime directory is missing: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True)


def imports(path: pathlib.Path) -> list[str]:
    output = subprocess.check_output(
        ["x86_64-w64-mingw32-objdump", "-p", str(path)], text=True
    )
    return re.findall(r"DLL Name:\s*(\S+)", output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--stage", type=pathlib.Path, required=True)
    args = parser.parse_args()

    executable = args.build / "matrix-client.exe"
    if not executable.is_file():
        raise SystemExit(f"Windows application executable is missing: {executable}")

    if args.stage.exists():
        shutil.rmtree(args.stage)
    args.stage.mkdir(parents=True)
    shutil.copy2(executable, args.stage / "Lightning.exe")

    # Lightning embeds its own QML but imports Qt's runtime modules. Copy the
    # selected target import families (not the Qt SDK or QtTest imports) so
    # transitive module metadata/plugins remain coherent, including the
    # separately built QtMultimedia module.
    qml_source = QT_ROOT / "qml"
    qml_target = args.stage / "qml"
    qml_target.mkdir()
    for name in QML_RUNTIME_ENTRIES:
        copy_tree(qml_source / name, qml_target / name)
    for name in ("builtins.qmltypes", "jsroot.qmltypes"):
        shutil.copy2(qml_source / name, qml_target / name)
    test_import = qml_target / "Qt/test"
    if test_import.exists():
        shutil.rmtree(test_import)

    for directory, names in PLUGIN_FILES.items():
        destination = args.stage / "plugins" / directory
        destination.mkdir(parents=True)
        for name in names:
            source = QT_ROOT / "plugins" / directory / name
            if not source.is_file():
                raise SystemExit(f"required Qt plugin is missing: {source}")
            shutil.copy2(source, destination / name)

    qwindows = args.stage / "plugins/platforms/qwindows.dll"
    qtmultimedia = args.stage / "qml/QtMultimedia/quickmultimediaplugin.dll"
    if not qwindows.is_file() or not qtmultimedia.is_file():
        raise SystemExit("required qwindows or QtMultimedia QML plugin is missing")

    translations = SYSROOT / "share/qt6/translations"
    if translations.is_dir():
        target = args.stage / "translations"
        target.mkdir()
        for pattern in ("qtbase_*.qm", "qtmultimedia_*.qm"):
            for item in translations.glob(pattern):
                shutil.copy2(item, target / item.name)

    licenses = args.stage / "licenses"
    licenses.mkdir()
    shutil.copy2(args.source / "LICENSE", licenses / "Lightning-GPL-3.0.txt")
    for license_dir in sorted(pathlib.Path("/usr/share/licenses").glob("mingw64-qt6-*")):
        copy_tree(license_dir, licenses / license_dir.name)
    source_licenses = pathlib.Path("/usr/share/licenses/lightning-qtmultimedia-qml-source")
    copy_tree(source_licenses, licenses / source_licenses.name)

    (args.stage / "qt.conf").write_text(
        "[Paths]\nPlugins = plugins\nQml2Imports = qml\nTranslations = translations\n",
        encoding="utf-8",
    )

    available = {p.name.lower(): p for p in (SYSROOT / "bin").glob("*.dll")}
    copied = {p.name.lower() for p in args.stage.rglob("*.dll")}
    queue = sorted([args.stage / "Lightning.exe", *args.stage.rglob("*.dll")])
    scanned: set[pathlib.Path] = set()
    unresolved: dict[str, list[str]] = {}
    graph: dict[str, list[str]] = {}

    while queue:
        pe = queue.pop(0)
        if pe in scanned:
            continue
        scanned.add(pe)
        needed = sorted(set(imports(pe)), key=str.casefold)
        graph[str(pe.relative_to(args.stage))] = needed
        for dll in needed:
            key = dll.lower()
            if key in copied or key in SYSTEM_DLLS or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            source = available.get(key)
            if source is None:
                unresolved.setdefault(dll, []).append(str(pe.relative_to(args.stage)))
                continue
            destination = args.stage / source.name
            shutil.copy2(source, destination)
            copied.add(key)
            queue.append(destination)

    if unresolved:
        raise SystemExit("unresolved Windows DLL imports: " + json.dumps(unresolved, sort_keys=True))

    manifest = {
        "target": "x86_64-pc-windows-gnu",
        "application": "Lightning.exe",
        "pe_files": sorted(str(p.relative_to(args.stage)) for p in scanned),
        "imports": dict(sorted(graph.items())),
    }
    (args.stage / "runtime-dependencies.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
