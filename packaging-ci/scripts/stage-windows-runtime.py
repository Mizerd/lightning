#!/usr/bin/env python3
"""Create a minimal, dependency-closed Windows deployment directory."""

import argparse
import json
import pathlib
import re
import shutil
import sys
import subprocess

SYSROOT = pathlib.Path("/usr/x86_64-w64-mingw32/sys-root/mingw")
QT_ROOT = SYSROOT / "lib/qt6"
SYSTEM_DLLS = {
    "advapi32.dll", "authz.dll", "avrt.dll", "bcrypt.dll",
    "bcryptprimitives.dll", "cfgmgr32.dll",
    "comdlg32.dll", "crypt32.dll", "d3d11.dll", "d3d12.dll",
    "d3dcompiler_47.dll", "d3d9.dll", "d2d1.dll", "dbghelp.dll",
    "dnsapi.dll", "dsound.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll",
    "dxva2.dll",
    "evr.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    # DirectSound, the Core Audio device enumerator and kernel streaming: the
    # OS entry points the directsound, wasapi2 and winks plugins bind to.
    "ksuser.dll", "mmdevapi.dll",
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
    # No qsvg.dll: nothing in the UI loads an SVG through Image, and without
    # the plugin a received SVG cannot reach a decoder. Send thumbnails render
    # through Qt6Svg.dll directly. validate-windows-artifacts.sh asserts it.
    "imageformats": (
        "qgif.dll", "qico.dll", "qjpeg.dll", "qtiff.dll", "qwebp.dll",
    ),
    # FFmpeg is the multimedia backend Lightning pins on Windows; WMF is the
    # fallback. The import walk pulls in the FFmpeg runtime DLLs.
    "multimedia": ("windowsmediaplugin.dll", "ffmpegmediaplugin.dll"),
    "networkinformation": ("qnetworklistmanager.dll",),
    "platforms": ("qwindows.dll",),
    "sqldrivers": ("qsqlite.dll",),
    "styles": ("qmodernwindowsstyle.dll",),
    "tls": ("qcertonlybackend.dll", "qschannelbackend.dll"),
}

QML_RUNTIME_ENTRIES = (
    "QML", "Qt", "QtCore", "QtMultimedia", "QtNetwork", "QtQml", "QtQuick",
)

# GStreamer plugins for the call media engine.
#
# Plugins are dlopen'd, so the PE-import walk below cannot find them; they are
# copied explicitly and then seeded into the walk to pull in their own DLLs.
#
# The directory name is a contract with the application: SfuMediaEngine points
# GST_PLUGIN_PATH at `<exe dir>/gstreamer-1.0` and clears the system path
# (the compiled-in default is the builder's sysroot).
#
# Each entry is the plugin that registers the element, not one that merely
# mentions it. Windows capture is ksvideosrc + gdiscreencapsrc: the
# mediafoundation and d3d11 plugins are UCRT builds that import libstdc++
# symbols this msvcrt toolchain's libstdc++-6.dll does not export
# (docs/windows-packaging.md).
GSTREAMER_PLUGIN_DIR = "gstreamer-1.0"
# Plugins staged when the builder image carries them and skipped with a
# warning otherwise. The builder image is built by hand on the runner host
# (docs/windows-runner-operations.md), so a Dockerfile change alone changes
# nothing, and a plugin required before the image carries it fails the build.
# Order for adding a plugin:
#   1. add it to packaging/windows/Dockerfile, bumping the verify stage's
#      plugin count in the same commit;
#   2. build the image on the runner host and point config.toml at the new
#      tag, keeping the previous one in allowed_images;
#   3. run `gitlab-runner verify`;
#   4. only then move it into GSTREAMER_PLUGINS and add its element below.
OPTIONAL_GSTREAMER_PLUGINS: tuple[str, ...] = ()

GSTREAMER_PLUGINS = (
    "libgstapp.dll",               # appsink, appsrc
    "libgstaudioconvert.dll",      # audioconvert
    "libgstaudioresample.dll",     # audioresample
    "libgstaudiotestsrc.dll",      # audiotestsrc
    "libgstautodetect.dll",        # autoaudiosrc, autoaudiosink, autovideosrc
    "libgstcoreelements.dll",      # queue, valve, capsfilter, fakesink, tee
    "libgstdirectsound.dll",       # directsoundsink
    "libgstdirectsoundsrc.dll",    # directsoundsrc
    "libgstdtls.dll",              # dtlssrtpenc, dtlssrtpdec
    "libgstnice.dll",              # nicesrc, nicesink
    "libgstopus.dll",              # opusenc, opusdec
    "libgstrtp.dll",               # rtpopuspay/depay, rtpvp8pay/depay
    "libgstrtpmanager.dll",        # rtpbin and friends, used inside webrtcbin
    # sctpenc/sctpdec: nothing in Lightning names them, but webrtcbin loads
    # them for the data channel that LiveKit's subscriber offer puts in media
    # section 0. Under max-bundle every media section rides that transport, so
    # without this plugin nothing is received, while sending still works.
    "libgstsctp.dll",              # sctpenc, sctpdec
    # jpegdec for UVC cameras' MJPG modes. Without it the camera falls back to
    # raw YUY2, which USB 2.0 limits to about 10 fps at 720p. libjpeg-8.dll
    # (a Qt dependency) is a library, not this element.
    "libgstjpeg.dll",              # jpegdec (MJPG camera modes)
    "libgstlevel.dll",             # level — the capture level meter (v7+)
    "libgstsrtp.dll",              # srtpenc, srtpdec, used inside dtlssrtp*
    # The opt-in GPU scale path for screen share (LIGHTNING_SHARE_GPU=1). The
    # Dockerfile puts the plugin in the sysroot; this tuple is what ships.
    # Its dependencies come through the seeded import walk.
    "libgstopengl.dll",            # glupload, glcolorconvert, glcolorscale
    "libgstvideoconvertscale.dll", # videoconvert, videoscale
    "libgstvideorate.dll",         # videorate
    "libgstvideotestsrc.dll",      # videotestsrc
    "libgstvolume.dll",            # volume
    "libgstvpx.dll",               # vp8enc, vp8dec
    "libgstwasapi.dll",            # wasapisrc/sink
    "libgstwasapi2.dll",           # wasapi2src/sink
    "libgstwebrtc.dll",            # webrtcbin
    "libgstwebrtcdsp.dll",         # webrtcdsp, webrtcechoprobe
    "libgstwinks.dll",             # ksvideosrc (camera)
    "libgstwinscreencap.dll",      # gdiscreencapsrc (screen share)
)

# What the engine asks the registry for: SfuMediaEngine.cpp's kRequired probe
# plus the elements its pipeline descriptions name. `lightningrtpvp8pay` is
# absent because Lightning registers it itself. validate-windows-artifacts.sh
# runs this list against the packaged tree under Wine.
GSTREAMER_ELEMENTS = (
    "appsink", "audioconvert", "audioresample", "audiotestsrc", "autoaudiosink",
    "autoaudiosrc", "capsfilter", "dtlssrtpdec", "dtlssrtpenc", "fakesink",
    "gdiscreencapsrc",
    # GPU share path: the app falls back to the CPU when these are missing, so
    # only this probe would notice a packaging gap.
    "glcolorconvert", "glcolorscale", "gldownload", "glupload",
    # A staged plugin DLL does not prove the element registers; this list does.
    "jpegdec",
    # jpegenc: the app decides whether a camera uses the MJPG chain by
    # building `videotestsrc ! jpegenc ! <entry> ! fakesink`
    # (SfuMediaEngine::jpegCameraChainAvailable). Without it every camera
    # silently falls back to the raw entry.
    "jpegenc",
    "ksvideosrc",
    # The capture level meter. The Dockerfile symbol probe cannot fail for it
    # (element and plugin share a name), so this list is what checks it.
    "level",
    "nicesink", "nicesrc", "opusdec", "opusenc",
    "queue", "rtpbin", "rtpopusdepay", "rtpopuspay", "rtpvp8depay", "rtpvp8pay",
    # Loaded by webrtcbin for the data channel, not named by Lightning.
    "sctpdec", "sctpenc",
    "srtpenc", "tee", "valve", "videoconvert", "videorate", "videoscale",
    "videotestsrc", "volume", "vp8dec", "vp8enc", "webrtcbin", "webrtcdsp",
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

    executable = args.build / "lightning-matrix.exe"
    if not executable.is_file():
        raise SystemExit(f"Windows application executable is missing: {executable}")
    # Every Windows package ships the update helper; without it the in-app
    # updater is inert, so its absence is a build failure.
    updater = args.build / "lightning-updater.exe"
    if not updater.is_file():
        raise SystemExit(f"Windows update helper is missing: {updater}")

    if args.stage.exists():
        shutil.rmtree(args.stage)
    args.stage.mkdir(parents=True)
    shutil.copy2(executable, args.stage / "Lightning.exe")
    shutil.copy2(updater, args.stage / "lightning-updater.exe")

    # Lightning embeds its own QML but imports Qt's runtime modules. Copy the
    # selected import families (not the SDK or QtTest imports).
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

    gstreamer_destination = args.stage / GSTREAMER_PLUGIN_DIR
    gstreamer_destination.mkdir()
    for name in GSTREAMER_PLUGINS:
        source = SYSROOT / "lib/gstreamer-1.0" / name
        if not source.is_file():
            raise SystemExit(
                "required GStreamer plugin is missing from the builder image "
                f"(voice and video calls would be dead in the package): {source}"
            )
        shutil.copy2(source, gstreamer_destination / name)
    staged_optional = []
    for name in OPTIONAL_GSTREAMER_PLUGINS:
        source = SYSROOT / "lib/gstreamer-1.0" / name
        if source.is_file():
            shutil.copy2(source, gstreamer_destination / name)
            staged_optional.append(name)
        else:
            print(
                "WARNING: optional GStreamer plugin is not in the builder image "
                f"and was not staged (rebuild the image to ship it): {name}",
                file=sys.stderr,
            )

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
    # Licence texts for GStreamer and the libraries in its plugins.
    gstreamer_licenses = pathlib.Path("/usr/share/licenses/lightning-gstreamer")
    copy_tree(gstreamer_licenses, licenses / gstreamer_licenses.name)
    # gst-plugins-good's licence is vendored in this repository because the
    # upstream MinGW SDK does not ship it. See PROVENANCE.txt beside the text.
    good_licenses = (args.source / "packaging-ci" / "packaging" / "common"
                     / "licenses" / "gst-plugins-good-1.0")
    if not (good_licenses / "COPYING").is_file():
        raise SystemExit(
            f"the vendored gst-plugins-good licence is missing at "
            f"{good_licenses} — every staged gst-plugins-good binary would "
            f"ship without it")
    # Explicit mode rather than copy_tree: git records only the executable
    # bit, so a checkout under umask 0000 gives 666 files, which the AppImage
    # validator's world-writable scan rejects.
    good_dest = licenses / "lightning-gstreamer" / "gst-plugins-good-1.0"
    good_dest.mkdir(parents=True, exist_ok=True)
    for name in ("COPYING", "PROVENANCE.txt"):
        shutil.copyfile(good_licenses / name, good_dest / name)
        (good_dest / name).chmod(0o644)

    (args.stage / "qt.conf").write_text(
        "[Paths]\nPlugins = plugins\nQml2Imports = qml\nTranslations = translations\n",
        encoding="utf-8",
    )

    available = {p.name.lower(): p for p in (SYSROOT / "bin").glob("*.dll")}
    copied = {p.name.lower() for p in args.stage.rglob("*.dll")}
    # Both executables seed the dependency walk, so a new dependency of the
    # helper is caught. rglob("*.dll") seeds the GStreamer plugins, which
    # nothing imports.
    queue = sorted([
        args.stage / "Lightning.exe",
        args.stage / "lightning-updater.exe",
        *args.stage.rglob("*.dll"),
    ])
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
        "update_helper": "lightning-updater.exe",
        "gstreamer_plugins": sorted(
            f"{GSTREAMER_PLUGIN_DIR}/{name}"
            for name in (*GSTREAMER_PLUGINS, *staged_optional)
        ),
        "pe_files": sorted(str(p.relative_to(args.stage)) for p in scanned),
        "imports": dict(sorted(graph.items())),
    }
    (args.stage / "runtime-dependencies.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
