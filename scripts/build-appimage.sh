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

# Declared to linuxdeploy so their own NEEDED libraries are bundled into
# usr/lib, where the AppRun's LD_LIBRARY_PATH will find them.
#
# THE SOURCE PATH, NOT THE STAGED COPY, and the difference is the whole bug.
# Handing linuxdeploy a file that is ALREADY inside the AppDir makes it treat
# the library as deployed and skip it, so it never walks that plugin's own
# NEEDED list. Pipeline 139 staged all 28 plugins correctly and bundled none
# of their dependencies: libgstsctp, libgstallocators, libgstnet,
# libgstbadaudio, libnice, libvpx, libsrtp2 and libasound were all absent, so
# every interesting plugin failed to load and the engine reported
# `missing_element:webrtcbin` — an AppImage with a complete plugin directory
# and no calling. Caught by validate-appimage's launch check, which is exactly
# what it was added for.
#
# linuxdeploy also drops its own copy of each plugin into usr/lib. That is
# harmless: GStreamer only scans GST_PLUGIN_SYSTEM_PATH_1_0, which the AppRun
# hook points at usr/lib/gstreamer-1.0, so the copies in usr/lib are never
# loaded as plugins — they are just the price of getting their dependencies
# resolved.
for plugin in "${GST_REQUIRED_PLUGINS[@]}"; do
    LINUXDEPLOY_PLUGIN_ARGS+=(--library "$GST_PLUGIN_SRC/$plugin.so")
done

# AND THEN COPY THE DEPENDENCIES OURSELVES.
#
# NOTE, and re-test this before trusting it: the belief that `--library` never
# walks a plugin's NEEDED list is TRUE of 139/140 and FALSE of 141. Pipeline
# 141's build log shows linuxdeploy deploying dependencies for each staged
# plugin and copying libgstsctp-1.0.so.0, libgstallocators-1.0.so.0,
# libgstnet-1.0.so.0 and libsrtp2.so.1 into usr/lib itself, rpath set to
# $ORIGIN. Those four were PRESENT in 141 and still unreachable, because a
# plugin in usr/lib/gstreamer-1.0 resolving $ORIGIN never looks in usr/lib.
# So this loop may now be redundant; it is kept because it is harmless and
# because nothing has re-tested removing it. What fixed 141 is the hook's
# LD_LIBRARY_PATH, not this copy.
#
# The 139/140 history, which the source-path form above addresses:
# libgstsctp-1.0.so.0, libgstallocators-1.0.so.0, libgstnet-1.0.so.0,
# libgstbadaudio-1.0.so.0, libnice.so.10, libvpx.so.9 and libsrtp2.so.1 all
# absent — so webrtcbin did not exist and the engine reported
# `missing_element:webrtcbin`. A complete plugin directory and no calling.
#
# So resolve them with the loader itself and copy what is missing. `ldd`
# answers with the paths the dynamic linker WOULD use, which is the same
# question the AppImage asks at runtime, and it recurses — so one pass over
# the staged plugins covers their transitive closure too.
#
# WHAT IS DELIBERATELY NOT COPIED: anything already in the AppDir (linuxdeploy
# put it there and rewrote it), and the base-system set linuxdeploy's own
# excludelist leaves on the host — glibc and its siblings, the X libraries,
# ALSA. Bundling those is how an AppImage breaks on a host whose loader
# disagrees with the build image's.
gst_dep_copied=0
gst_dep_skipped=0
while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    dep_name="$(basename "$dep")"
    # Already bundled by linuxdeploy, in either location.
    [[ -e "$APPDIR/usr/lib/$dep_name" ]] && { gst_dep_skipped=$((gst_dep_skipped+1)); continue; }
    case "$dep_name" in
        # The loader, the C/C++ runtime and their siblings: never bundle.
        ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
        libgcc_s.so.*|libstdc++.so.*|libresolv.so.*)
            gst_dep_skipped=$((gst_dep_skipped+1)); continue ;;
        # Base system per linuxdeploy's excludelist: X, ALSA, GL, D-Bus.
        libX*.so.*|libxcb*.so.*|libasound.so.*|libGL*.so.*|libEGL*.so.*|\
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
# A plugin set this size cannot have zero private dependencies. Zero means the
# resolution silently produced nothing, which is how this shipped twice.
[[ "$gst_dep_copied" -gt 0 ]] || \
    die "resolved no GStreamer plugin dependencies at all — the bundle would ship plugins that cannot load"

# Every staged plugin must now resolve against the AppDir, not against this
# build image. Asked of the loader with the AppDir as the search path, which
# is the arrangement the AppImage actually runs in.
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

# linuxdeploy's generated AppRun sources every apprun-hooks/*.sh. Without this
# hook the plugins are bundled and never found: GStreamer scans its COMPILED-IN
# system path, which points at the build image.
mkdir -p "$APPDIR/apprun-hooks"
cat >"$APPDIR/apprun-hooks/gstreamer.sh" <<'HOOK'
# Point GStreamer at the plugins bundled beside the binary.
export GST_PLUGIN_SYSTEM_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
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
# THE COST, stated rather than glossed: LD_LIBRARY_PATH is inherited by every
# child process, so a browser launched for OAuth or a permalink starts with
# this bundle's glib/gio/dbus ahead of the host's. That is bounded by the
# copy loop's excludelist -- glibc, libstdc++, libgcc, GL/EGL, drm/gbm and X
# are deliberately never bundled -- and build-snap.sh:57 already makes the
# same trade. The surgical alternative is patchelf --set-rpath '$ORIGIN/..'
# on the staged plugins, which leaks into no child; it needs to run AFTER
# linuxdeploy has set its own rpath, so it needs a packaging step this script
# does not have yet. Revisit with a pipeline available to test it.
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
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

# ASK THE PACKED ARTIFACT, and ask it the right question.
#
# Pipelines 139, 140 and 141 each verified something upstream of the squashfs
# and shipped anyway. The gate that actually caught them is validate-appimage,
# which launches the binary on an image carrying no Qt and no GStreamer. This
# block is an EARLIER WARNING in the job that can still fix it, not a
# replacement for that gate.
#
# It must not repeat 141's mistake in a new costume. A bare `ldd ... | grep
# "not found"` has NO TEETH HERE: this job apt-installs the GStreamer runtime,
# so every library a plugin needs resolves from /usr/lib/x86_64-linux-gnu
# whether or not it was bundled. The question is therefore not "did the loader
# find it" but "did it find it INSIDE THE BUNDLE".
verify_dir="$(mktemp -d -p "$ROOT/work")"
trap 'rm -rf "$verify_dir"' EXIT
if ! ( cd "$verify_dir" && "$ROOT/$OUT" --appimage-extract >extract.log 2>&1 ); then
    cat "$verify_dir/extract.log" >&2 || true
    die "could not extract the built AppImage for verification"
fi
verify_root="$verify_dir/squashfs-root"

# The hook is what makes usr/lib reachable from usr/lib/gstreamer-1.0 at all.
# Both assertions are anchored: linuxdeploy emits one `source` line PER hook,
# so a bare 'apprun-hooks' match is satisfied by the Qt hook alone, and a bare
# 'LD_LIBRARY_PATH' match is satisfied by this file's own comments.
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

# Resolve each plugin the way the AppRun arranges it, then reject any
# dependency satisfied from OUTSIDE the bundle unless it is base system --
# the same allowlist the copy loop applies, for the same reason.
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

write_sha256 "$OUT"

# Hand the fully bundled AppDir to the snap job so it does not recompile.
tar -C "$ROOT/work" -I 'zstd -T0 -6' -cf \
    "dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst" appdir
echo "built $OUT"
