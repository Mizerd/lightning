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
    "dnsapi.dll", "dsound.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll",
    "dxva2.dll",
    "evr.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    # DirectSound, the Core Audio device enumerator and the kernel-streaming
    # user-mode library: the three OS audio/capture entry points the GStreamer
    # directsound, wasapi2 and winks plugins bind to. All ship in System32 and
    # none is redistributable.
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
    "imageformats": (
        "qgif.dll", "qico.dll", "qjpeg.dll", "qsvg.dll", "qtiff.dll",
        "qwebp.dll",
    ),
    # ffmpegmediaplugin is the FFmpeg decode backend Lightning pins on Windows
    # (QT_MEDIA_BACKEND=ffmpeg); windowsmediaplugin (WMF) is kept as a fallback.
    # Staging the FFmpeg plugin makes the recursive import-walk pull in the
    # avcodec/avformat/avutil/swscale/swresample runtime DLLs automatically.
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

# GStreamer plugins for the MatrixRTC call media engine.
#
# A GStreamer plugin is dlopen'd, never linked, so NOTHING in any executable's
# import table names one and the recursive PE-import walk below cannot discover
# a single one of them. They are copied explicitly and then SEEDED into that
# walk, which is what pulls their own runtime DLLs (libgstreamer-1.0-0.dll,
# libnice-10.dll, libopus-0.dll, libsrtp2-1.dll, liborc-0.4-0.dll and the rest)
# out of the sysroot.
#
# The directory name is a CONTRACT with the application:
# SfuMediaEngine::runtimeAvailable() points GST_PLUGIN_PATH at
# `<exe dir>/gstreamer-1.0` and clears GST_PLUGIN_SYSTEM_PATH before gst_init,
# because the compiled-in default plugin path is the builder's sysroot and does
# not exist on a user's machine. Rename this directory and every call refuses
# with "missing_element:webrtcbin" on a machine that has the plugins on disk.
#
# The set is the engine's own element requirements, mapped to the plugin that
# REGISTERS each one (several other plugins merely mention the names, which is
# why this list was derived from the registering plugin rather than from a
# string match). Windows capture is ksvideosrc + gdiscreencapsrc: the
# mediafoundation and d3d11 plugins are UCRT builds whose `mbstate_t` differs
# from this msvcrt toolchain's, so they import libstdc++ symbols the staged
# libstdc++-6.dll does not export (docs/windows-packaging.md).
GSTREAMER_PLUGIN_DIR = "gstreamer-1.0"
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
    # sctpenc/sctpdec. NOT optional, and nothing in Lightning names them:
    # webrtcbin loads them itself for the DATA CHANNEL, and LiveKit's
    # SUBSCRIBER offer puts a data channel in media section 0. With
    # bundle-policy=max-bundle every audio and video section is bundled onto
    # THAT section's transport, so without this plugin webrtcbin cannot build
    # the transport the media rides on: `_get_or_create_data_channel_transports:
    # code should not be reached`, then not one `pad-added` for the whole call.
    #
    # The failure is silent and one-directional and looked like anything but a
    # missing plugin. Windows SENT audio the far end could hear — our own
    # publisher offer is media-only, so its bundle owner is the audio section —
    # while receiving nothing at all in either media kind. The answer SDP is
    # byte-identical with and without it, which is why comparing SDP text
    # refuted the theory before a control run brought it back.
    "libgstsctp.dll",              # sctpenc, sctpdec
    "libgstsrtp.dll",              # srtpenc, srtpdec, used inside dtlssrtp*
    # glupload, glcolorconvert, glcolorscale, gldownload — the opt-in GPU
    # scale path for a screen share (LIGHTNING_SHARE_GPU=1).
    #
    # THIS IS A SECOND LIST, AND STAGING IT IN THE BUILDER IMAGE IS NOT
    # ENOUGH. packaging/windows/Dockerfile puts the plugin in the image's
    # SYSROOT so the toolchain has it; this tuple is what actually goes into
    # the shipped zip. Adding it there and not here produced a build whose
    # log said `element "glupload" is not available in this build` — the
    # app's own fallback catching a packaging gap, correctly, and the second
    # time in this round that an artifact was nearly handed over claiming a
    # capability it did not have.
    #
    # Its dependencies (libgstgl-1.0-0, libgraphene-1.0-0,
    # libgstcontroller-1.0-0, libjpeg-8, libpng16) need no entry: the seeded
    # import walk below pulls them out of the sysroot, which is exactly what
    # that walk is for.
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

# What the engine asks the registry for. This mirrors SfuMediaEngine.cpp's
# kRequired probe plus the elements its gst_parse pipeline descriptions name;
# `lightningrtpvp8pay` is deliberately absent because Lightning registers that
# one itself and no plugin file carries it. validate-windows-artifacts.sh reads
# this list and runs it against the packaged tree under Wine.
GSTREAMER_ELEMENTS = (
    "appsink", "audioconvert", "audioresample", "audiotestsrc", "autoaudiosink",
    "autoaudiosrc", "capsfilter", "dtlssrtpdec", "dtlssrtpenc", "fakesink",
    "gdiscreencapsrc",
    # The GPU screen-share scale path (LIGHTNING_SHARE_GPU=1). Probed against
    # the SHIPPED tree for the same reason as sctp below: the app degrades to
    # the CPU when these are absent and says so in its log, which is the right
    # behaviour and also means a packaging gap would never fail a build. It
    # would just quietly stop being a GPU path — which is exactly what the
    # previous artifact did.
    "glcolorconvert", "glcolorscale", "gldownload", "glupload",
    "ksvideosrc", "nicesink", "nicesrc", "opusdec", "opusenc",
    "queue", "rtpbin", "rtpopusdepay", "rtpopuspay", "rtpvp8depay", "rtpvp8pay",
    # Probed even though no Lightning pipeline names them: webrtcbin loads
    # them for the data channel, and their absence broke every incoming track
    # while every other check passed. A probe list that only covers what the
    # app spells out cannot see a dependency the element loads for itself.
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
    # The update helper ships beside the application in every Windows package
    # (all three are built from this one stage). Without it the in-app updater
    # has nothing to hand the verified artifact to and the feature is inert, so
    # a missing helper is a build failure rather than a silently smaller stage.
    updater = args.build / "lightning-updater.exe"
    if not updater.is_file():
        raise SystemExit(f"Windows update helper is missing: {updater}")

    if args.stage.exists():
        shutil.rmtree(args.stage)
    args.stage.mkdir(parents=True)
    shutil.copy2(executable, args.stage / "Lightning.exe")
    shutil.copy2(updater, args.stage / "lightning-updater.exe")

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
    # GStreamer and the libraries linked into its plugins (libnice, opus, vpx,
    # libsrtp, orc, zlib, webrtc-audio-processing) are LGPL/BSD redistributables,
    # so their licence texts ship with the binaries that carry them.
    gstreamer_licenses = pathlib.Path("/usr/share/licenses/lightning-gstreamer")
    copy_tree(gstreamer_licenses, licenses / gstreamer_licenses.name)

    (args.stage / "qt.conf").write_text(
        "[Paths]\nPlugins = plugins\nQml2Imports = qml\nTranslations = translations\n",
        encoding="utf-8",
    )

    available = {p.name.lower(): p for p in (SYSROOT / "bin").glob("*.dll")}
    copied = {p.name.lower() for p in args.stage.rglob("*.dll")}
    # Both Lightning-owned executables seed the dependency walk. The helper links
    # only Qt6::Core (plus zlib), so it adds nothing the application does not
    # already pull in — but seeding it is what MAKES that true rather than
    # assumed, and it is what would catch a future helper that grows a new
    # dependency the stage does not carry.
    #
    # `rglob("*.dll")` is what seeds the GStreamer plugins copied above, and it
    # has to: nothing imports them, so without being seeded here their own
    # runtime libraries would never be staged and every plugin would fail to
    # load on the user's machine with the files sitting right beside it.
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
            f"{GSTREAMER_PLUGIN_DIR}/{name}" for name in GSTREAMER_PLUGINS
        ),
        "pe_files": sorted(str(p.relative_to(args.stage)) for p in scanned),
        "imports": dict(sorted(graph.items())),
    }
    (args.stage / "runtime-dependencies.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
