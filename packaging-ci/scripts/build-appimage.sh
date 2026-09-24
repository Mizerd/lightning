#!/usr/bin/env bash
# Build the Lightning AppImage from the shared Release staged tree.
#
# Reuses configure-build.sh, then bundles Qt, plugins and QML modules with
# pinned linuxdeploy releases. The bundle must be self-contained apart from
# linuxdeploy's base-system excludelist (glibc, GL, X11, Wayland).
#
# Outputs:
#   dist/Lightning-<LOGICAL_VERSION>-x86_64.AppImage
#   dist/lightning-appdir-<LOGICAL_VERSION>.tar.zst   (consumed by build-snap)
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"

LIGHTNING_INSTALL_TYPE=linux-appimage "$SCRIPT_DIR/configure-build.sh"
load_versions

STAGE="$ROOT/work/stage"
APPDIR="$ROOT/work/appdir"
TOOLS="$ROOT/work/appimage-tools"
OUT="dist/Lightning-${LOGICAL_VERSION}-x86_64.AppImage"

# Pinned bundling tools (dated upstream releases, checksum-verified).
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20240109-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256=c86d6540f1df31061f02f539a2d3445f8d7f85cc3994eee1e74cd1ac97b76df0
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20240109-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA256=f53349093d333a6558c560844c1a0f64a3b6bd077bf02740af3ad3dbb8827433
# appimagetool packs (see PRUNE below). `continuous` is a moving tag, so the
# sha256 pin fails the build if upstream republishes; refresh it deliberately.
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256=a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0

fetch_tool() {
    local url=$1 sha=$2 out=$3
    if [ ! -f "$out" ] || ! echo "$sha  $out" | sha256sum -c --quiet 2>/dev/null; then
        curl -fsSL -o "$out" "$url"
    fi
    echo "$sha  $out" | sha256sum -c --quiet || die "checksum mismatch for $out"
    chmod +x "$out"
}
mkdir -p "$TOOLS" dist
fetch_tool "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256" "$TOOLS/linuxdeploy"
fetch_tool "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA256" "$TOOLS/linuxdeploy-plugin-qt"
fetch_tool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "$TOOLS/appimagetool"

rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGE/usr" "$APPDIR/usr"
strip --strip-unneeded "$APPDIR/usr/bin/lightning-matrix"
test -x "$APPDIR/usr/bin/lightning-updater" || die "update helper missing from the staged tree"
strip --strip-unneeded "$APPDIR/usr/bin/lightning-updater"

# QML modules ELF scanning cannot discover; keep aligned with QML_DEPENDS in
# build-deb.sh.
export QML_SOURCES_PATHS="$ROOT/work/lightning/qml"
# offscreen for validation and headless use; wayland for native sessions.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-generic.so;libqwayland-egl.so"
# Plugin directories linuxdeploy-plugin-qt does not deploy by default:
#   tls                        — OpenSSL backend; without it every Qt https
#                                request (update check, GIF) fails
#   wayland-shell-integration  — xdg-shell; without it Qt falls back to
#                                XWayland, where screen shares are black
#   wayland-decoration-client / wayland-graphics-integration-client — client
#                                side decorations and the EGL path
export EXTRA_QT_PLUGINS="tls;wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
export VERSION="$LOGICAL_VERSION"
export LDAI_OUTPUT="$OUT"
export APPIMAGE_EXTRACT_AND_RUN=1

# Extra --library arguments; declared up front for `set -u`.
LINUXDEPLOY_PLUGIN_ARGS=()

# linuxdeploy-plugin-qt finds Qt through qmake6.
command -v qmake6 >/dev/null || die "qmake6 missing in build image"
export QMAKE=$(command -v qmake6)

# GStreamer plugins are dlopen'd, so linuxdeploy cannot discover them from
# ELF NEEDED entries. Without them every call is refused by the engine's
# element probe.
GST_PLUGIN_SRC="/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
GST_PLUGIN_DEST="$APPDIR/usr/lib/gstreamer-1.0"
# Fatal rather than skipped: the job installs the plugins, so absence means
# the job changed.
[[ -d "$GST_PLUGIN_SRC" ]] || die "no GStreamer plugins at $GST_PLUGIN_SRC: the build job did not install the runtime plugin packages, so the AppImage would bundle none and refuse every call"
mkdir -p "$GST_PLUGIN_DEST"

# Many staged plugins are gst-plugins-good (LGPL-2.1); ship its licence. The
# text is vendored because host packages do not reliably carry it.
GOOD_LICENSE_SRC="$ROOT/packaging-ci/packaging/common/licenses/gst-plugins-good-1.0"
[[ -f "$GOOD_LICENSE_SRC/COPYING" ]] || die "the vendored gst-plugins-good licence is missing at $GOOD_LICENSE_SRC: the AppImage bundles its binaries and must carry its licence"
APPIMAGE_LICENSE_DEST="$APPDIR/usr/share/licenses/lightning-gstreamer/gst-plugins-good-1.0"
# `install -m`, not `cp -a`: the CI checkout runs under umask 0000, and
# copying its 666 modes fails validate-appimage's world-writable check.
install -d -m 0755 "$APPIMAGE_LICENSE_DEST"
install -m 0644 "$GOOD_LICENSE_SRC/COPYING" "$APPIMAGE_LICENSE_DEST/COPYING"
install -m 0644 "$GOOD_LICENSE_SRC/PROVENANCE.txt" \
    "$APPIMAGE_LICENSE_DEST/PROVENANCE.txt"
install -Dm644 "$ROOT/LICENSE" \
    "$APPDIR/usr/share/licenses/Lightning-GPL-3.0.txt"

# Exactly the plugins the engine loads: its kRequired list, every element in
# a pipeline description, and every element probed at runtime outside
# kRequired. The hook below replaces the system plugin path, so an element
# missing here is missing for good; absence is fatal.
#   coreelements                        queue valve capsfilter tee fakesink identity
#   webrtc nice dtls srtp               webrtcbin and its ICE/DTLS-SRTP transport
#   sctp                                loaded by webrtcbin itself for the data
#                                       channel, which owns the bundled transport;
#                                       without it nothing is received
#   opus rtp rtpmanager vpx             codecs and RTP payloaders
#   app                                 appsink/appsrc: received video
#   audioconvert audioresample          audio format conversion
#   audiotestsrc videotestsrc           required by the engine
#   videoconvertscale videorate volume  publish chain, per-participant volume
#   autodetect pulseaudio alsa          device selection and a sink on any host
#   pipewire                            pipewiresrc: portal screen capture
#   ximagesrc                           X11 screen-share fallback (not in
#                                       kRequired, so no runtime check sees it)
#   video4linux2                        v4l2src: the camera
#   webrtcdsp                           microphone AGC
#   audioparsers playback typefindfunctions   demux/parse support
#   opengl                              GPU scaling for screen share (else
#                                       silently CPU); libGL/libEGL stay on the
#                                       host per linuxdeploy's excludelist
GST_REQUIRED_PLUGINS=(
    libgstcoreelements
    libgstwebrtc libgstnice libgstdtls libgstsrtp libgstsctp
    libgstopus libgstrtp libgstrtpmanager libgstvpx
    libgstapp
    libgstaudioconvert libgstaudioresample libgstaudiotestsrc
    libgstvideotestsrc libgstvideoconvertscale libgstvideorate libgstvolume
    libgstopengl
    libgstautodetect libgstpulseaudio libgstalsa
    libgstpipewire libgstvideo4linux2 libgstximagesrc
    libgstwebrtcdsp
    libgstaudioparsers libgstplayback libgsttypefindfunctions
    # libgstlevel — capture level meter, the only way to tell a live
    #               microphone from a silent one
    # libgstjpeg  — the camera's MJPG chain; raw-only cannot reach 720p30 on
    #               USB 2.0
    libgstlevel libgstjpeg
)
for plugin in "${GST_REQUIRED_PLUGINS[@]}"; do
    [[ -f "$GST_PLUGIN_SRC/$plugin.so" ]] || \
        die "required GStreamer plugin $plugin.so is not installed in the build image"
    cp "$GST_PLUGIN_SRC/$plugin.so" "$GST_PLUGIN_DEST/"
done

# NSS's PKCS#11 modules. Debian's libsrtp2 uses NSS, which dlopens
# libsoftokn3 (and it libfreebl3) at runtime from beside libnss3, so no NEEDED
# list names them. On a host without NSS (always, under snap confinement) SRTP
# cannot initialise and calls carry no media. Staged here for the AppImage and
# the snap. The directory is derived from libnss3.so, as NSS itself does;
# Debian has moved these before.
NSS_MODULE_SRC="$(dirname "$(ldconfig -p | awk '/libnss3\.so/ {print $NF; exit}')")"
[[ -n "$NSS_MODULE_SRC" && -d "$NSS_MODULE_SRC" ]] || \
    die "cannot locate libnss3.so in the build image: libsrtp2 is built against NSS here, so without its modules SRTP cannot initialise and every call carries no media"
NSS_REQUIRED_MODULES=(
    libsoftokn3     # PKCS#11 softoken: the module libsrtp's ciphers come from
    libfreebl3      # the primitives softokn itself dlopens
    libfreeblpriv3  # softokn prefers this one where it exists
    libnssdbm3      # legacy DBM database module, probed during init
    libnssckbi      # built-in roots; cheap, and NSS probes for it
)
for mod in "${NSS_REQUIRED_MODULES[@]}"; do
    [[ -f "$NSS_MODULE_SRC/$mod.so" ]] || \
        die "required NSS module $mod.so is not in $NSS_MODULE_SRC: SRTP would fail to initialise on any host without NSS of its own, and ALWAYS under strict snap confinement"
    cp "$NSS_MODULE_SRC/$mod.so" "$APPDIR/usr/lib/"
done

# gst-plugin-scanner builds the registry in a separate process for crash
# isolation. Its path is compiled into libgstreamer, so without a bundled copy
# every launch warns "External plugin loader failed" and scans in-process.
# Debian has moved it between releases, so search the known locations.
GST_SCANNER_DEST="$APPDIR/usr/libexec/gstreamer-1.0"
gst_scanner_src=""
for candidate in \
    "/usr/libexec/gstreamer-1.0/gst-plugin-scanner" \
    "/usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner" \
    "/usr/lib/x86_64-linux-gnu/gstreamer-1.0/gst-plugin-scanner"; do
    if [[ -x "$candidate" ]]; then
        gst_scanner_src="$candidate"
        break
    fi
done
[[ -n "$gst_scanner_src" ]] || \
    die "gst-plugin-scanner is not in any known location in the build image: GStreamer would print 'External plugin loader failed' at every launch and scan in-process"
mkdir -p "$GST_SCANNER_DEST"
cp "$gst_scanner_src" "$GST_SCANNER_DEST/gst-plugin-scanner"
# Explicit mode: a non-executable helper behaves like an absent one.
chmod 0755 "$GST_SCANNER_DEST/gst-plugin-scanner"
printf 'Staged gst-plugin-scanner from %s\n' "$gst_scanner_src"

# libpipewire dlopens its own SPA plugins and modules from build-time paths
# and reads its module list from client.conf, so all three are staged and
# pointed at by the AppRun hook; without them pipewiresrc fails with "can't
# make support.system handle" (screen share only; audio and camera do not use
# libpipewire). Only support/ and videoconvert/ (named by client.conf) are
# staged: the other SPA directories need libraries that are not bundled.
SPA_SRC="/usr/lib/x86_64-linux-gnu/spa-0.2"
PW_MODULE_SRC="/usr/lib/x86_64-linux-gnu/pipewire-0.3"
for spa_subdir in support videoconvert; do
    [[ -d "$SPA_SRC/$spa_subdir" ]] || \
        die "SPA plugin directory $SPA_SRC/$spa_subdir is missing (install libspa-0.2-modules)"
    mkdir -p "$APPDIR/usr/lib/spa-0.2/$spa_subdir"
    cp "$SPA_SRC/$spa_subdir"/*.so "$APPDIR/usr/lib/spa-0.2/$spa_subdir/"
done

# Debian's client.conf loads all but module-rt without `ifexists nofail`, so a
# missing one makes pw_context_new() fail. The rest of the directory needs
# libraries that are not bundled.
PW_REQUIRED_MODULES=(
    libpipewire-module-protocol-native
    libpipewire-module-client-node
    libpipewire-module-client-device
    libpipewire-module-adapter
    libpipewire-module-metadata
    libpipewire-module-session-manager
    libpipewire-module-rt
)
mkdir -p "$APPDIR/usr/lib/pipewire-0.3"
for pw_module in "${PW_REQUIRED_MODULES[@]}"; do
    [[ -f "$PW_MODULE_SRC/$pw_module.so" ]] || \
        die "PipeWire module $pw_module.so is missing (install libpipewire-0.3-modules)"
    cp "$PW_MODULE_SRC/$pw_module.so" "$APPDIR/usr/lib/pipewire-0.3/"
done

# libpipewire has no built-in module list; without client.conf
# pw_context_new() fails. Debian's copy matches the bundled libpipewire.
PW_CLIENT_CONF=""
for candidate in /opt/pipewire-conf/usr/share/pipewire/client.conf \
                 /usr/share/pipewire/client.conf; do
    [[ -f "$candidate" ]] && { PW_CLIENT_CONF="$candidate"; break; }
done
[[ -n "$PW_CLIENT_CONF" ]] || \
    die "pipewire client.conf not found; screen share would fail at pw_context_new"
mkdir -p "$APPDIR/usr/share/pipewire"
cp "$PW_CLIENT_CONF" "$APPDIR/usr/share/pipewire/client.conf"
printf 'PipeWire client stack staged: %d SPA dirs, %d modules, client.conf from %s\n' \
    2 "${#PW_REQUIRED_MODULES[@]}" "$PW_CLIENT_CONF"

# --- Qt image-format plugins --------------------------------------------------
#
# Qt image formats are dlopen'd plugins; linuxdeploy-plugin-qt deploys only
# qtbase's gif/ico/jpeg. WebP is required because the client's MIME sniffers
# accept it. JPEG XL comes only from KDE's kimageformats (kimg_jxl.so), never
# from Qt, so Windows and macOS have none.
#
# Deliberately not shipped:
#   avif  -- pulls three AV1 encoders and ~20 abseil libraries (20+ MB)
#   heif  -- libheif dlopens its own codec plugins, so it would register and
#            decode nothing; HEVC licensing too
#   svg   -- SVG must never reach a media path as active content (security)
QT_IMAGE_PLUGIN_DEST="$APPDIR/usr/plugins/imageformats"
# kimageformat6-plugins is unpacked into /opt/kimageformats rather than
# installed, which keeps its heavy dependencies and unwanted plugins out of the
# image.
QT_IMAGE_PLUGIN_SRC_QT="/usr/lib/x86_64-linux-gnu/qt6/plugins/imageformats"
QT_IMAGE_PLUGIN_SRC_KF="/opt/kimageformats/usr/lib/x86_64-linux-gnu/qt6/plugins/imageformats"
# name:source-directory, named individually (no globs).
QT_IMAGE_REQUIRED_PLUGINS=(
    "libqwebp.so:$QT_IMAGE_PLUGIN_SRC_QT"
    "kimg_jxl.so:$QT_IMAGE_PLUGIN_SRC_KF"
)
mkdir -p "$QT_IMAGE_PLUGIN_DEST"
for entry in "${QT_IMAGE_REQUIRED_PLUGINS[@]}"; do
    img_plugin="${entry%%:*}"
    img_src="${entry#*:}/$img_plugin"
    [[ -f "$img_src" ]] || die "Qt image-format plugin $img_plugin not found at $img_src: the build job did not install/unpack the package that provides it, so the AppImage would ship a client that accepts image formats it cannot decode"
    cp "$img_src" "$QT_IMAGE_PLUGIN_DEST/"
    # The source path, not the staged copy (see the GStreamer note below).
    LINUXDEPLOY_PLUGIN_ARGS+=(--library "$img_src")
done
printf 'Qt image-format plugins staged: %d (%s)\n' \
    "${#QT_IMAGE_REQUIRED_PLUGINS[@]}" \
    "$(printf '%s ' "${QT_IMAGE_REQUIRED_PLUGINS[@]%%:*}")"

# ── Qt Wayland integration plugins ───────────────────────────────────────
# The pinned linuxdeploy-plugin-qt ignores the Wayland entries in
# EXTRA_QT_PLUGINS, so these are hand-staged. Without libxdg-shell.so Qt falls
# back to XWayland, where screen shares capture a black root window.
QT_PLUGIN_SRC_BASE="/usr/lib/x86_64-linux-gnu/qt6/plugins"
QT_WAYLAND_REQUIRED_PLUGINS=(
    "wayland-shell-integration/libxdg-shell.so"
)
QT_WAYLAND_OPTIONAL_DIRS=(
    "wayland-decoration-client"
    "wayland-graphics-integration-client"
)
for wl_rel in "${QT_WAYLAND_REQUIRED_PLUGINS[@]}"; do
    wl_src="$QT_PLUGIN_SRC_BASE/$wl_rel"
    [[ -f "$wl_src" ]] || die "Qt Wayland plugin $wl_rel not found at $wl_src: the build job did not install qt6-wayland, so the AppImage would fall back to XWayland (black screen share)"
    mkdir -p "$APPDIR/usr/plugins/$(dirname "$wl_rel")"
    cp "$wl_src" "$APPDIR/usr/plugins/$wl_rel"
    LINUXDEPLOY_PLUGIN_ARGS+=(--library "$wl_src")
done
wl_optional=0
for wl_dir in "${QT_WAYLAND_OPTIONAL_DIRS[@]}"; do
    if [[ -d "$QT_PLUGIN_SRC_BASE/$wl_dir" ]] && compgen -G "$QT_PLUGIN_SRC_BASE/$wl_dir/*.so" >/dev/null; then
        mkdir -p "$APPDIR/usr/plugins/$wl_dir"
        cp "$QT_PLUGIN_SRC_BASE/$wl_dir"/*.so "$APPDIR/usr/plugins/$wl_dir/"
        wl_optional=$((wl_optional+1))
    fi
done
printf 'Qt Wayland integration plugins staged: %d required + %d optional dir(s)\n' \
    "${#QT_WAYLAND_REQUIRED_PLUGINS[@]}" "$wl_optional"

# Declared to linuxdeploy so their NEEDED libraries are bundled into usr/lib.
# Pass the source path: a file already inside the AppDir is treated as
# deployed and its NEEDED list is never walked. The extra copies linuxdeploy
# drops into usr/lib are harmless, since GStreamer only scans
# GST_PLUGIN_SYSTEM_PATH_1_0.
for plugin in "${GST_REQUIRED_PLUGINS[@]}"; do
    LINUXDEPLOY_PLUGIN_ARGS+=(--library "$GST_PLUGIN_SRC/$plugin.so")
done

# Also copy the plugins' dependencies ourselves, resolved transitively by
# ldd. This may be redundant with linuxdeploy's own walk, but it is harmless
# and has not been re-tested without. Skip anything already in the AppDir and
# the base-system set linuxdeploy's excludelist leaves on the host.
gst_dep_copied=0
gst_dep_skipped=0
while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    dep_name="$(basename "$dep")"
    # Already bundled by linuxdeploy.
    [[ -e "$APPDIR/usr/lib/$dep_name" ]] && { gst_dep_skipped=$((gst_dep_skipped+1)); continue; }
    case "$dep_name" in
        # The loader, the C/C++ runtime and their siblings: never bundle.
        ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
        libgcc_s.so.*|libstdc++.so.*|libresolv.so.*)
            gst_dep_skipped=$((gst_dep_skipped+1)); continue ;;
        # Base system per linuxdeploy's excludelist: X, ALSA, GL, D-Bus and
        # Wayland (a bundled libwayland-client broke the host's Mesa EGL,
        # issue #9). The packed-plugin audit below uses the same list.
        libX*.so.*|libxcb*.so.*|libasound.so.*|libGL*.so.*|libEGL*.so.*|\
        libwayland-*.so.*|\
        libdrm.so.*|libgbm.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*)
            gst_dep_skipped=$((gst_dep_skipped+1)); continue ;;
    esac
    cp -Ln "$dep" "$APPDIR/usr/lib/$dep_name" 2>/dev/null &&         gst_dep_copied=$((gst_dep_copied+1))
done < <(
    for plugin in "${GST_REQUIRED_PLUGINS[@]}"; do
        ldd "$GST_PLUGIN_SRC/$plugin.so" 2>/dev/null \
            | awk '/=> \// { print $3 }'
    done | sort -u
)
printf 'GStreamer plugin dependencies: %d copied, %d already present or base system\n' \
    "$gst_dep_copied" "$gst_dep_skipped"
# Zero copied means the resolution silently produced nothing.
[[ "$gst_dep_copied" -gt 0 ]] || \
    die "resolved no GStreamer plugin dependencies at all — the bundle would ship plugins that cannot load"

# Every staged plugin must resolve against the AppDir, as it will at runtime.
gst_unresolved=""
for staged_plugin in "$GST_PLUGIN_DEST"/*.so; do
    [[ -e "$staged_plugin" ]] || continue
    while IFS= read -r missing; do
        gst_unresolved+=" $(basename "$staged_plugin"):$missing"
    done < <(
        LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$staged_plugin" 2>/dev/null \
            | awk '/not found/ { print $1 }'
    )
done
[[ -z "$gst_unresolved" ]] || \
    die "staged GStreamer plugins cannot load from the bundle:$gst_unresolved"
printf 'All %d staged GStreamer plugins resolve against the AppDir\n' \
    "${#GST_REQUIRED_PLUGINS[@]}"

# The same for the image-format plugins, whose codec libraries (libwebp,
# libjxl) nothing else in the bundle links.
img_dep_copied=0
img_dep_skipped=0
while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    dep_name="$(basename "$dep")"
    [[ -e "$APPDIR/usr/lib/$dep_name" ]] && { img_dep_skipped=$((img_dep_skipped+1)); continue; }
    case "$dep_name" in
        ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
        libgcc_s.so.*|libstdc++.so.*|libresolv.so.*)
            img_dep_skipped=$((img_dep_skipped+1)); continue ;;
        libX*.so.*|libxcb*.so.*|libasound.so.*|libGL*.so.*|libEGL*.so.*|\
        libwayland-*.so.*|\
        libdrm.so.*|libgbm.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*)
            img_dep_skipped=$((img_dep_skipped+1)); continue ;;
        # Qt is deployed by linuxdeploy; an unpatched Debian copy placed
        # first would never be replaced (`cp -n`).
        libQt6*)
            img_dep_skipped=$((img_dep_skipped+1)); continue ;;
    esac
    cp -Ln "$dep" "$APPDIR/usr/lib/$dep_name" 2>/dev/null \
        && img_dep_copied=$((img_dep_copied+1))
done < <(
    for entry in "${QT_IMAGE_REQUIRED_PLUGINS[@]}"; do
        ldd "${entry#*:}/${entry%%:*}" 2>/dev/null | awk '/=> \// { print $3 }'
    done | sort -u
)
printf 'Qt image-format plugin dependencies: %d copied, %d already present or base system\n' \
    "$img_dep_copied" "$img_dep_skipped"
# Qt links neither libwebp nor libjxl, so zero copied means nothing resolved.
[[ "$img_dep_copied" -gt 0 ]] || \
    die "resolved no Qt image-format plugin dependencies at all — the bundle would ship plugins that cannot decode"

img_unresolved=""
for staged_plugin in "$QT_IMAGE_PLUGIN_DEST"/*.so; do
    [[ -e "$staged_plugin" ]] || continue
    while IFS= read -r missing; do
        img_unresolved+=" $(basename "$staged_plugin"):$missing"
    done < <(
        LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$staged_plugin" 2>/dev/null \
            | awk '/not found/ { print $1 }'
    )
done
[[ -z "$img_unresolved" ]] || \
    die "staged Qt image-format plugins cannot load from the bundle:$img_unresolved"
printf 'All %d staged Qt image-format plugins resolve against the AppDir\n' \
    "${#QT_IMAGE_REQUIRED_PLUGINS[@]}"

# linuxdeploy's AppRun sources every apprun-hooks/*.sh. Without this hook
# GStreamer scans its compiled-in path, which names the build image.
mkdir -p "$APPDIR/apprun-hooks"
cat >"$APPDIR/apprun-hooks/gstreamer.sh" <<'HOOK'
# EVERY variable this hook sets is preserved first under the AppImage
# convention (APPIMAGE_ORIGINAL_<NAME>), so a process Lightning spawns -- the
# browser for OAuth or a link, a media player -- can be given back the
# session's own values instead of this bundle's. GST_PLUGIN_SYSTEM_PATH_1_0
# in particular REPLACES the host's plugin path: a player inheriting it would
# see only the 28 plugins bundled here and lose every system codec. The
# client's UrlLauncher restores or removes each one from these.
for _lightning_var in GST_PLUGIN_SYSTEM_PATH_1_0 GST_PLUGIN_PATH_1_0 GST_REGISTRY_1_0 \
                      GST_PLUGIN_SCANNER_1_0 GST_PLUGIN_SCANNER \
                      SPA_PLUGIN_DIR PIPEWIRE_MODULE_DIR PIPEWIRE_CONFIG_DIR; do
    eval "export APPIMAGE_ORIGINAL_${_lightning_var}=\"\${${_lightning_var}:-}\""
done
unset _lightning_var
# Point GStreamer at the plugins bundled beside the binary.
export GST_PLUGIN_SYSTEM_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
# AND AT THE REGISTRY HELPER THAT SCANS THEM. Without this GStreamer looks for
# it at the path compiled into libgstreamer -- the build image's -- prints
# "External plugin loader failed" and scans in-process, losing the crash
# isolation that a separate process buys. BOTH spellings, because GStreamer
# reads the versioned one first and falls back to the plain one, and a host
# value left in the unversioned variable would otherwise win the fallback.
# These name ONE EXECUTABLE, never a colon-joined list.
export GST_PLUGIN_SCANNER_1_0="$APPDIR/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
export GST_PLUGIN_SCANNER="$APPDIR/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
# AND MAKE THE PLUGINS' OWN DEPENDENCIES RESOLVABLE. linuxdeploy's AppRun sets
# no LD_LIBRARY_PATH at all -- it relies entirely on rewriting RUNPATH to
# $ORIGIN on the files it deploys itself. The plugins here, and the libraries
# they need, are staged by this script and never pass through that rewrite, so
# they keep whatever RUNPATH the distro shipped. A plugin in
# usr/lib/gstreamer-1.0/ resolving $ORIGIN looks in gstreamer-1.0/, NOT in
# usr/lib/ where its dependencies actually are. That is pipeline 141, whose
# build log shows linuxdeploy itself placing libgstsctp-1.0.so.0 and three
# others INTO usr/lib -- present, rpath $ORIGIN, and unreachable from
# gstreamer-1.0/: "libgstsctp-1.0.so.0: cannot open shared object file".
# (139 and 140 are a different failure: those libraries were not bundled at
# all, and no search path would have helped.)
#
# MEASURED ON PIPELINE 141'S OWN ARTIFACT, not inferred. Extracted, its 28
# plugins and all six libraries the launch reported missing are PRESENT; the
# plugins carry RUNPATH $ORIGIN, the packed AppRun contains no LD_LIBRARY_PATH
# at all, and resolving one plugin the way that AppRun arranges it reports
# 10 dependencies "not found" -- which drops to 0 with usr/lib on the path.
# Bundled and unreachable, and this line is what reaches them.
#
# WHY NOT patchelf --set-rpath '$ORIGIN/..' ON THE PLUGINS, which would leak
# into no child process: linuxdeploy REWRITES their RUNPATH to $ORIGIN itself
# (verified on that artifact), so patching before it runs is overwritten, and
# patching after it runs cannot be packed by it -- `--output appimage`
# re-runs "Deploying dependencies for existing files" and would reset them.
# It needs the pack step replaced with a direct appimagetool call. That is a
# real improvement and an untested restructuring; it is not being made blind
# in the same round that fixes the defect.
#
# THE COST, stated rather than glossed: LD_LIBRARY_PATH is inherited by every
# child process, so a browser launched for OAuth or a permalink starts with
# this bundle's glib/gio/dbus ahead of the host's. Bounded by the copy loop's
# excludelist -- glibc, libstdc++, libgcc, GL/EGL, drm/gbm and X are never
# bundled -- and build-snap.sh:57 already makes the same trade. The original
# value is preserved below under the AppImage convention so the client can
# restore a clean environment for processes it spawns; nothing reads it yet,
# and that is the follow-up rather than a claim.
export APPIMAGE_ORIGINAL_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# THE PIPEWIRE CLIENT STACK, staged beside the plugins by this script and
# invisible to linuxdeploy, which only ever rewrote RUNPATH on what it deployed.
# Without these three, screen sharing dies at `pw_loop_new` or `pw_context_new`
# while audio and video RECEIVE keep working -- the exact shape of the 142
# report. The module dir goes on LD_LIBRARY_PATH too: module-client-node has
# module-protocol-native as a NEEDED with an absolute Debian DT_RUNPATH, and
# although config ordering means it is already loaded under its SONAME by then,
# LD_LIBRARY_PATH is searched before DT_RUNPATH and this costs nothing.
export SPA_PLUGIN_DIR="$APPDIR/usr/lib/spa-0.2"
export PIPEWIRE_MODULE_DIR="$APPDIR/usr/lib/pipewire-0.3"
export PIPEWIRE_CONFIG_DIR="$APPDIR/usr/share/pipewire"
export LD_LIBRARY_PATH="$APPDIR/usr/lib/pipewire-0.3${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The plugin registry is a CACHE and GStreamer rewrites it whenever the
# plugin set changes. An AppImage mount is read-only and its path changes
# every run, so leaving the registry at its default makes every launch
# re-scan and print warnings it cannot act on. Keep it in the user's cache,
# namespaced so it cannot collide with a system GStreamer's registry.
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/lightning/gst-registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY_1_0")" 2>/dev/null || true
HOOK

# Deploy only. appimagetool packs below, because linuxdeploy's own
# `--output appimage` re-deploys dependencies and would restore the libraries
# pruned in between.
# Both executables are declared: linuxdeploy only bundles libraries for, and
# rewrites the RPATH of, executables it is told about.
# libgpg-error is on linuxdeploy's excludelist but libgcrypt is bundled, and
# the two are version-locked, so ship the matching libgpg-error.
"$TOOLS/linuxdeploy" --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/lightning.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/192x192/apps/lightning.png" \
    --executable "$APPDIR/usr/bin/lightning-matrix" \
    --executable "$APPDIR/usr/bin/lightning-updater" \
    --library /lib/x86_64-linux-gnu/libgpg-error.so.0 \
    "${LINUXDEPLOY_PLUGIN_ARGS[@]}" \
    --plugin qt

# Prune the Wayland client libraries: they belong to the host. The host's
# Mesa EGL, handed an older bundled libwayland-client lacking
# wl_display_dispatch_queue_timeout, fails to initialise and the app aborts
# (issue #9). libQt6WaylandClient stays. validate-appimage.sh asserts their
# absence.
PRUNE_HOST_LIBS=(
    libwayland-client.so.0
    libwayland-cursor.so.0
    libwayland-egl.so.1
)
for lib in "${PRUNE_HOST_LIBS[@]}"; do
    found=0
    while IFS= read -r hit; do
        rm -f "$hit"
        found=1
        echo "pruned from the payload (host owns it): ${hit#$APPDIR/}"
    done < <(find "$APPDIR/usr/lib" -maxdepth 1 -name "$lib*" 2>/dev/null)
    [ "$found" = 1 ] || echo "note: $lib was not bundled, nothing to prune"
done

# Third-party licences. linuxdeploy already deploys Debian copyright files for
# what it bundles, but not for libraries staged past it or unpacked into /opt.
# This pass attributes every bundled shared object to its Debian package via
# the dpkg file lists (Policy 12.5 makes the copyright file mandatory) and
# ships that copyright; deriving the set from the payload keeps it correct
# across base-image bumps. An object that cannot be attributed is fatal.
# Packages unpacked into /opt with `dpkg-deb -x` are in no dpkg list, so their
# own extracted copyright files are indexed too.
#
# Part of the ffmpeg closure is GPL-2+ rather than LGPL; see
# docs/open-items.md for the source-offer decision.
LICENSE_INDEX="$ROOT/work/licence-basename-index"
: >"$LICENSE_INDEX"
for list in /var/lib/dpkg/info/*.list; do
    pkg="$(basename "$list" .list)"
    pkg="${pkg%%:*}"
    awk -v p="$pkg" '{ n = $0; sub(/.*\//, "", n); if (n != "") print n "\t" p "\t/usr/share/doc/" p "/copyright\t" $0 }' \
        "$list"
done >>"$LICENSE_INDEX"
for unpacked in /opt/kimageformats /opt/pipewire-conf; do
    [ -d "$unpacked" ] || continue
    while IFS= read -r copyright; do
        pkg="$(basename "$(dirname "$copyright")")"
        find "$unpacked" -type f -printf '%f\t'"$pkg"'\t'"$copyright"'\t%p\n'
    done < <(find "$unpacked/usr/share/doc" -maxdepth 2 -name copyright 2>/dev/null)
done >>"$LICENSE_INDEX"
sort -u -o "$LICENSE_INDEX" "$LICENSE_INDEX"
test -s "$LICENSE_INDEX" || die "could not build a licence basename index: no /var/lib/dpkg/info/*.list"

THIRD_PARTY_LICENSES="$APPDIR/usr/share/licenses/third-party"
install -d -m 0755 "$THIRD_PARTY_LICENSES"

# Lightning's own shared objects, covered by Lightning-GPL-3.0.txt. Currently
# none (the Rust bridge is linked statically). Named, not pattern-matched, so a
# third-party file cannot be absolved by accident.
OUR_OWN_OBJECTS=(
    liblightning_rust_bridge.so
    libmatrix_rust_bridge.so
)
is_our_own() {
    local n="$1"
    for o in "${OUR_OWN_OBJECTS[@]}"; do [ "$n" = "$o" ] && return 0; done
    return 1
}
# GNU build-id: preserved by strip and patchelf, unlike the file bytes.
build_id() {
    readelf -n "$1" 2>/dev/null | awk '/Build ID:/ { print $NF; exit }'
}

harvest_total=0
harvest_attributed=0
harvest_unattributed=()
harvest_ambiguous=()
declare -A HARVEST_PKGS=()
while IFS= read -r -d '' object; do
    base="$(basename "$object")"
    harvest_total=$((harvest_total + 1))
    if is_our_own "$base"; then
        harvest_attributed=$((harvest_attributed + 1))
        continue
    fi
    # A basename can belong to several packages (e.g. Qt 5 and Qt 6 plugins
    # share names). Disambiguate by byte compare, then by build-id (linuxdeploy
    # strips and patches what it deploys). Remaining ambiguity is fatal: a
    # guessed attribution could ship the wrong licence.
    mapfile -t cands < <(awk -v n="$base" -F'\t' '$1 == n { print }' "$LICENSE_INDEX")
    hit=""
    if [ "${#cands[@]}" -eq 1 ]; then
        hit="${cands[0]}"
    elif [ "${#cands[@]}" -gt 1 ]; then
        for cand in "${cands[@]}"; do
            src="${cand##*$'\t'}"
            if [ -f "$src" ] && cmp -s "$object" "$src"; then
                hit="$cand"
                break
            fi
        done
        if [ -z "$hit" ]; then
            obj_bid="$(build_id "$object")"
            if [ -n "$obj_bid" ]; then
                for cand in "${cands[@]}"; do
                    src="${cand##*$'\t'}"
                    if [ -f "$src" ] && [ "$(build_id "$src")" = "$obj_bid" ]; then
                        hit="$cand"
                        break
                    fi
                done
            fi
        fi
        if [ -z "$hit" ]; then
            harvest_ambiguous+=("${object#$APPDIR/} -> $(printf '%s\n' "${cands[@]}" | cut -f2 | sort -u | tr '\n' ' ')")
            continue
        fi
    fi
    if [ -n "$hit" ]; then
        harvest_attributed=$((harvest_attributed + 1))
        pkg_name="$(printf '%s' "$hit" | cut -f2)"
        pkg_copyright="$(printf '%s' "$hit" | cut -f3)"
        HARVEST_PKGS["$pkg_name"]="$pkg_copyright"
    else
        harvest_unattributed+=("${object#$APPDIR/}")
    fi
done < <(find "$APPDIR/usr" -type f -name '*.so*' -print0)

if [ "${#harvest_ambiguous[@]}" -gt 0 ]; then
    printf 'ambiguous payload object: %s\n' "${harvest_ambiguous[@]}" >&2
    die "${#harvest_ambiguous[@]} bundled shared objects match more than one Debian package by basename and none of the candidates by content, so the licence that applies to them cannot be decided"
fi

if [ "${#harvest_unattributed[@]}" -gt 0 ]; then
    printf 'unattributed payload object: %s\n' "${harvest_unattributed[@]}" >&2
    die "${#harvest_unattributed[@]} of $harvest_total bundled shared objects could not be traced to a Debian package, so their licence terms are unknown and cannot be shipped. If one of them is OURS, name it in OUR_OWN_OBJECTS above"
fi

harvest_copied=0
harvest_bytes=0
for pkg in $(printf '%s\n' "${!HARVEST_PKGS[@]}" | sort); do
    src="${HARVEST_PKGS[$pkg]}"
    test -f "$src" \
        || die "Debian package $pkg owns a bundled library and has no $src (Policy 12.5 requires one)"
    install -m 0644 "$src" "$THIRD_PARTY_LICENSES/$pkg.copyright"
    harvest_copied=$((harvest_copied + 1))
    harvest_bytes=$((harvest_bytes + $(stat -c %s "$src")))
done

# Assert the count so a short harvest cannot pass silently.
[ "$harvest_copied" -ge 100 ] \
    || die "only $harvest_copied third-party licence files were harvested; this payload has always needed well over a hundred, so the index or the walk is broken"
printf 'third-party licences: %s payload objects, %s Debian packages, %s files, %s bytes\n' \
    "$harvest_total" "${#HARVEST_PKGS[@]}" "$harvest_copied" "$harvest_bytes"

ARCH=x86_64 "$TOOLS/appimagetool" --no-appstream "$APPDIR" "$OUT"

test -s "$OUT" || die "AppImage not produced at $OUT"

# Verify the packed artifact (an early warning; validate-appimage is the real
# gate). This image has the GStreamer runtime installed, so a bare "not found"
# check proves nothing: require dependencies to resolve inside the bundle.
verify_dir="$(mktemp -d -p "$ROOT/work")"
trap 'rm -rf "$verify_dir"' EXIT
if ! ( cd "$verify_dir" && "$ROOT/$OUT" --appimage-extract >extract.log 2>&1 ); then
    cat "$verify_dir/extract.log" >&2 || true
    die "could not extract the built AppImage for verification"
fi
verify_root="$verify_dir/squashfs-root"

# Anchored matches: a bare 'apprun-hooks' also matches the Qt hook's line, and
# a bare 'LD_LIBRARY_PATH' matches the hook's comments.
[[ -f "$verify_root/AppRun" ]] || die "packed bundle has no AppRun"
grep -q 'gstreamer\.sh' "$verify_root/AppRun" \
    || die "packed AppRun does not source gstreamer.sh; the GStreamer hook is inert"
[[ -f "$verify_root/apprun-hooks/gstreamer.sh" ]] \
    || die "packed bundle has no apprun-hooks/gstreamer.sh"
grep -q '^export LD_LIBRARY_PATH="\$APPDIR/usr/lib' "$verify_root/apprun-hooks/gstreamer.sh" \
    || die "packed GStreamer hook does not export LD_LIBRARY_PATH; plugin dependencies will not resolve"

packed_plugins=0
for plugin_name in "${GST_REQUIRED_PLUGINS[@]}"; do
    [[ -e "$verify_root/usr/lib/gstreamer-1.0/$plugin_name.so" ]] \
        && packed_plugins=$((packed_plugins+1))
done
[[ "$packed_plugins" -eq "${#GST_REQUIRED_PLUGINS[@]}" ]] || \
    die "AppImage carries $packed_plugins of ${#GST_REQUIRED_PLUGINS[@]} GStreamer plugins"

# Reject any non-base dependency resolved from outside the bundle (same
# allowlist as the copy loop).
packed_escaped=""
for staged_plugin in "$verify_root/usr/lib/gstreamer-1.0"/*.so; do
    [[ -e "$staged_plugin" ]] || continue
    while IFS= read -r line; do
        dep_name="${line%% *}"
        dep_path="$line"; dep_path="${dep_path#* }"
        case "$dep_name" in
            ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
            libgcc_s.so.*|libstdc++.so.*|libresolv.so.*|\
            libX*.so.*|libxcb*.so.*|libasound.so.*|libGL*.so.*|libEGL*.so.*|\
            libwayland-*.so.*|\
            libdrm.so.*|libgbm.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*)
                continue ;;
        esac
        [[ "$dep_path" == "$verify_root"/* ]] && continue
        packed_escaped+=" $(basename "$staged_plugin")->$dep_name"
    done < <(
        LD_LIBRARY_PATH="$verify_root/usr/lib" ldd "$staged_plugin" 2>/dev/null \
            | awk '/ => \// { print $1, $3 } / not found/ { print $1, "MISSING" }'
    )
done
if [[ -n "$packed_escaped" ]]; then
    printf 'packed usr/lib holds %d libraries, %d of them libgst*\n' \
        "$(find "$verify_root/usr/lib" -maxdepth 1 -name '*.so*' | wc -l)" \
        "$(find "$verify_root/usr/lib" -maxdepth 1 -name 'libgst*' | wc -l)"
    die "packed plugins resolve dependencies from outside the bundle (they would be missing on a user's machine):$packed_escaped"
fi
printf 'Packed AppImage: %d GStreamer plugins, every non-base dependency satisfied from inside the bundle\n' \
    "$packed_plugins"

# Image-format plugins in the packed squashfs, not just the AppDir.
packed_img=0
for entry in "${QT_IMAGE_REQUIRED_PLUGINS[@]}"; do
    img_plugin="${entry%%:*}"
    [[ -e "$verify_root/usr/plugins/imageformats/$img_plugin" ]] \
        && packed_img=$((packed_img+1)) \
        || printf 'MISSING from packed AppImage: usr/plugins/imageformats/%s\n' "$img_plugin" >&2
done
[[ "$packed_img" -eq "${#QT_IMAGE_REQUIRED_PLUGINS[@]}" ]] || \
    die "AppImage carries $packed_img of ${#QT_IMAGE_REQUIRED_PLUGINS[@]} Qt image-format plugins; it would accept image formats it cannot decode"

# Present is not loadable: dependencies must resolve inside the bundle (this
# image has libwebp and libjxl installed).
packed_img_escaped=""
for staged_plugin in "$verify_root/usr/plugins/imageformats"/*.so; do
    [[ -e "$staged_plugin" ]] || continue
    while IFS= read -r line; do
        dep_name="${line%% *}"
        dep_path="${line#* }"
        case "$dep_name" in
            ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
            libgcc_s.so.*|libstdc++.so.*|libresolv.so.*|\
            libX*.so.*|libxcb*.so.*|libasound.so.*|libGL*.so.*|libEGL*.so.*|\
            libwayland-*.so.*|\
            libdrm.so.*|libgbm.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*)
                continue ;;
        esac
        [[ "$dep_path" == "$verify_root"/* ]] && continue
        packed_img_escaped+=" $(basename "$staged_plugin")->$dep_name"
    done < <(
        LD_LIBRARY_PATH="$verify_root/usr/lib" ldd "$staged_plugin" 2>/dev/null \
            | awk '/ => \// { print $1, $3 } / not found/ { print $1, "MISSING" }'
    )
done
[[ -z "$packed_img_escaped" ]] || \
    die "packed image-format plugins resolve dependencies from outside the bundle (they would be missing on a user's machine):$packed_img_escaped"
printf 'Packed AppImage: %d Qt image-format plugins (%s), every non-base dependency satisfied from inside the bundle\n' \
    "$packed_img" "$(printf '%s ' "${QT_IMAGE_REQUIRED_PLUGINS[@]%%:*}")"

# The PipeWire client stack in the packed artifact.
for pw_required in usr/lib/spa-0.2/support/libspa-support.so \
                   usr/lib/pipewire-0.3/libpipewire-module-protocol-native.so \
                   usr/lib/pipewire-0.3/libpipewire-module-client-node.so \
                   usr/share/pipewire/client.conf; do
    [[ -e "$verify_root/$pw_required" ]] || \
        die "packed AppImage is missing $pw_required; screen sharing would fail at pw_context_new"
done
for pw_hook_var in SPA_PLUGIN_DIR PIPEWIRE_MODULE_DIR PIPEWIRE_CONFIG_DIR; do
    grep -q "^export $pw_hook_var=\"\$APPDIR/" "$verify_root/apprun-hooks/gstreamer.sh" \
        || die "packed GStreamer hook does not export $pw_hook_var; screen sharing would fail"
done
printf 'Packed AppImage: PipeWire client stack present and pointed at by the hook\n'

write_sha256 "$OUT"

# Hand the fully bundled AppDir to the snap job so it does not recompile.
tar -C "$ROOT/work" -I 'zstd -T0 -6' -cf \
    "dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst" appdir
echo "built $OUT"
