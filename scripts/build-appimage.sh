#!/usr/bin/env bash
# Build the Lightning AppImage from the shared Release staged tree.
#
# Reuses configure-build.sh (same build, GIF-key handling, and RPATH
# removal as the deb/rpm), then bundles Qt libraries, platform plugins, and
# the dynamic QML modules with pinned linuxdeploy releases. AppImage is
# unsandboxed by design; the bundle must be self-contained apart from the
# documented base-system excludelist (glibc, GL, X11).
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

rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGE/usr" "$APPDIR/usr"
strip --strip-unneeded "$APPDIR/usr/bin/matrix-client"
test -x "$APPDIR/usr/bin/lightning-updater" || die "update helper missing from the staged tree"
strip --strip-unneeded "$APPDIR/usr/bin/lightning-updater"

# The QML runtime modules ELF scanning cannot discover. Keep aligned with
# the deb QML_DEPENDS list in build-deb.sh (source of both: the production
# QML import scan).
export QML_SOURCES_PATHS="$ROOT/work/lightning/qml"
# Offscreen is required so the validation job (and headless users) can run
# the bundle; wayland keeps the primary platform native.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-generic.so;libqwayland-egl.so"
export VERSION="$LOGICAL_VERSION"
export LDAI_OUTPUT="$OUT"
export APPIMAGE_EXTRACT_AND_RUN=1

# Extra --library arguments accumulated below. Declared here so the
# expansion is well-defined under `set -u` when no plugins are staged.
LINUXDEPLOY_PLUGIN_ARGS=()

# linuxdeploy-plugin-qt finds Qt through qmake6.
command -v qmake6 >/dev/null || die "qmake6 missing in build image"
export QMAKE=$(command -v qmake6)

# linuxdeploy's excludelist keeps libgpg-error on the host but bundles
# libgcrypt, and the two are version-locked (trixie's libgcrypt needs
# gpgrt_* symbols older distros lack — seen live on Ubuntu 24.04 in
# validate-snap). Ship the exact libgpg-error the bundled libgcrypt was
# built against.
# BOTH executables are declared. linuxdeploy only resolves libraries for, and
# rewrites the RPATH of, the executables it is told about — an unlisted binary
# is copied along by the usr/ tree and then fails to start on any host without
# Qt6, which is precisely the host an AppImage exists for. The helper is the
# process that installs the update, so a helper that cannot start turns the
# whole feature into a silent failure at the last step.
# Voice/video calling: GStreamer PLUGINS are dlopen'd from a plugin path, so
# linuxdeploy cannot discover them the way it discovers linked libraries —
# it walks ELF NEEDED entries, and the binary links only gstreamer
# core/webrtc/sdp. Staging them into the AppDir BEFORE linuxdeploy runs
# means it also resolves and bundles each plugin's own dependencies.
#
# An AppImage that ships the binary without these installs and launches
# perfectly and then refuses every call, because the engine's runtime
# element probe fails — the worst kind of packaging bug, because nothing
# about it looks like packaging.
GST_PLUGIN_SRC="/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
GST_PLUGIN_DEST="$APPDIR/usr/lib/gstreamer-1.0"
# NOT `if [ -d ... ]`. The previous revision skipped this whole block when the
# directory was absent, and the build job installed no GStreamer at all, so it
# was skipped on every build: no plugins staged, no AppRun hook written, and a
# green pipeline. Absence is now the loud case, because on THIS image the
# plugins are installed by the job and their absence can only mean the job
# changed.
[[ -d "$GST_PLUGIN_SRC" ]] || die "no GStreamer plugins at $GST_PLUGIN_SRC: the build job did not install the runtime plugin packages, so the AppImage would bundle none and refuse every call"
mkdir -p "$GST_PLUGIN_DEST"

# Exactly what the call engine loads, and nothing else -- bundling the whole
# directory would add tens of megabytes of codecs nothing ever opens. Each name
# was resolved against this build image from the elements the Linux source
# actually names: SfuMediaEngine::runtimeAvailable's kRequired list, every
# element appearing in a pipeline description, AND every element probed at
# runtime through elementAvailable()/gst_element_factory_find() OUTSIDE
# kRequired. That last clause is not padding -- it is where `ximagesrc` lives,
# and an element no call requires is invisible to every runtime check we have.
# A MISSING one is a packaging regression rather than a distribution
# limitation, so it is fatal here.
#
# Grouped by why it is needed:
#   coreelements                        queue valve capsfilter tee fakesink identity
#   webrtc nice dtls srtp               webrtcbin and its ICE/DTLS-SRTP transport
#   sctp                                NOT named anywhere in Lightning. webrtcbin
#                                       loads it ITSELF for the data channel, and
#                                       LiveKit's subscriber offer puts one in
#                                       media section 0 -- which under
#                                       bundle-policy=max-bundle owns the transport
#                                       every audio and video section rides on.
#                                       Windows shipped for months able to SEND and
#                                       unable to RECEIVE because this was missing.
#   opus rtp rtpmanager vpx             the codecs and their RTP payloaders
#   app                                 appsink/appsrc: the received-video path
#   audioconvert audioresample          format conversion on both audio legs
#   audiotestsrc videotestsrc           in kRequired: the engine REFUSES without them
#   videoconvertscale videorate volume  the publish chain and per-participant volume
#   autodetect pulseaudio alsa          device selection, enumeration, and a
#                                       real sink on every desktop: autoaudiosink
#                                       resolves to pipewiresink, pulsesink or
#                                       alsasink depending on the host, and an
#                                       AppImage carries no system plugins to
#                                       fall back on
#   pipewire                            pipewiresrc: portal screen capture
#   ximagesrc                           the X11 screen-share fallback, used when
#                                       no xdg portal answers. It is NOT in
#                                       kRequired -- correctly, a call does not
#                                       need it -- so `--call-media-status` is
#                                       GREEN on a bundle without it while the
#                                       feature is DEAD: SfuCallController probes
#                                       the RUNNING REGISTRY, and the hook below
#                                       REPLACES the system plugin path, so the
#                                       host's plugins-good is invisible. The
#                                       route then refuses with "install
#                                       gst-plugins-good" -- a package the user
#                                       very likely already has and which would
#                                       change nothing. Nothing we can run
#                                       against the artifact can see its absence,
#                                       so staging it is the only defence.
#   video4linux2                        v4l2src: the camera (autovideosrc is
#                                       deliberately not used -- see the source)
#   webrtcdsp                           the microphone AGC, live-confirmed audible
#   audioparsers playback typefindfunctions   supporting demux/parse paths
GST_REQUIRED_PLUGINS=(
    libgstcoreelements
    libgstwebrtc libgstnice libgstdtls libgstsrtp libgstsctp
    libgstopus libgstrtp libgstrtpmanager libgstvpx
    libgstapp
    libgstaudioconvert libgstaudioresample libgstaudiotestsrc
    libgstvideotestsrc libgstvideoconvertscale libgstvideorate libgstvolume
    libgstautodetect libgstpulseaudio libgstalsa
    libgstpipewire libgstvideo4linux2 libgstximagesrc
    libgstwebrtcdsp
    libgstaudioparsers libgstplayback libgsttypefindfunctions
)
for plugin in "${GST_REQUIRED_PLUGINS[@]}"; do
    [[ -f "$GST_PLUGIN_SRC/$plugin.so" ]] || \
        die "required GStreamer plugin $plugin.so is not installed in the build image"
    cp "$GST_PLUGIN_SRC/$plugin.so" "$GST_PLUGIN_DEST/"
done

# Declared to linuxdeploy so their own NEEDED libraries are bundled and their
# RPATHs rewritten; without this they load on the build image and nowhere else.
for staged_plugin in "$GST_PLUGIN_DEST"/*.so; do
    [[ -e "$staged_plugin" ]] || continue
    LINUXDEPLOY_PLUGIN_ARGS+=(--library "$staged_plugin")
done

# linuxdeploy's generated AppRun sources every apprun-hooks/*.sh. Without this
# hook the plugins are bundled and never found: GStreamer scans its COMPILED-IN
# system path, which points at the build image.
mkdir -p "$APPDIR/apprun-hooks"
cat >"$APPDIR/apprun-hooks/gstreamer.sh" <<'HOOK'
# Point GStreamer at the plugins bundled beside the binary.
export GST_PLUGIN_SYSTEM_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
# The plugin registry is a CACHE and GStreamer rewrites it whenever the
# plugin set changes. An AppImage mount is read-only and its path changes
# every run, so leaving the registry at its default makes every launch
# re-scan and print warnings it cannot act on. Keep it in the user's cache,
# namespaced so it cannot collide with a system GStreamer's registry.
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/lightning/gst-registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY_1_0")" 2>/dev/null || true
HOOK

"$TOOLS/linuxdeploy" --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/lightning.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/192x192/apps/lightning.png" \
    --executable "$APPDIR/usr/bin/matrix-client" \
    --executable "$APPDIR/usr/bin/lightning-updater" \
    --library /lib/x86_64-linux-gnu/libgpg-error.so.0 \
    "${LINUXDEPLOY_PLUGIN_ARGS[@]}" \
    --plugin qt \
    --output appimage

test -s "$OUT" || die "AppImage not produced at $OUT"
write_sha256 "$OUT"

# Hand the fully bundled AppDir to the snap job so it does not recompile.
tar -C "$ROOT/work" -I 'zstd -T0 -6' -cf \
    "dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst" appdir
echo "built $OUT"
