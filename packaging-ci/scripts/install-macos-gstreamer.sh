#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Install the official GStreamer macOS framework for the Lightning packaging
# runner, WITHOUT root and WITHOUT touching /Library/Frameworks.
#
# WHY NOT HOMEBREW. Everything else on this runner comes from Homebrew, so that
# was the first candidate and it was rejected on two measurements:
#   * the CI account cannot use it. /opt/homebrew is owned by `roksme` and the
#     `runner` account that executes the shell executor is not in `admin` and
#     has no passwordless sudo, so `brew install gstreamer` cannot write there.
#   * the closure is wrong for a bundle. Homebrew's gstreamer 1.28.6 declares 61
#     required dependencies including gtk4, gtk+3, ffmpeg, x264/x265, librsvg
#     and python@3.14, its dylibs carry absolute /opt/homebrew install names, and
#     its glib/gio/gobject/libintl/libpcre2 would collide by FILENAME with the
#     copies macdeployqt already puts in Contents/Frameworks for Qt.
#   * it does not build webrtc-audio-processing, so `webrtcdsp` — the microphone
#     AGC — would be missing.
#
# The official framework has none of those problems: it is one self-contained
# tree whose libraries are already @rpath-relative (built for relocation), and
# it carries applemedia, osxaudio, nice, dtls, srtp, vpx, opus and webrtcdsp.
#
# It installs, as a .pkg, to /Library/Frameworks — which needs root. It does not
# have to: the .pkg is an xar archive, `pkgutil --expand-full` needs no
# privileges, and every component declares its install location relative to the
# framework root, so the tree can be reassembled anywhere. Nothing in it depends
# on being at /Library/Frameworks; the .pc files use `prefix=${pcfiledir}/../..`
# and the dylibs use @rpath.

GST_VERSION="${GST_VERSION:-1.28.6}"
# ONE knob, and it is the one build-macos.sh and validate-macos-artifacts.sh
# read. LIGHTNING_MACOS_GSTREAMER_PREFIX names the versioned prefix inside the
# framework; the framework root and the download cache are derived from it, so
# relocating the install cannot leave the build looking at the old place.
GST_PREFIX_DEFAULT="$HOME/opt/gstreamer/GStreamer.framework/Versions/1.0"
PREFIX="${LIGHTNING_MACOS_GSTREAMER_PREFIX:-$GST_PREFIX_DEFAULT}"
case "$PREFIX" in
    */GStreamer.framework/Versions/1.0) ;;
    *) die "LIGHTNING_MACOS_GSTREAMER_PREFIX must end in GStreamer.framework/Versions/1.0" ;;
esac
FRAMEWORK="${PREFIX%/Versions/1.0}"
GST_ROOT="${FRAMEWORK%/GStreamer.framework}"
DOWNLOAD_DIR="$GST_ROOT/download"
BASE_URL="https://gstreamer.freedesktop.org/data/pkg/osx/$GST_VERSION"

[[ "$(uname -s)" == Darwin ]] || die "install-macos-gstreamer.sh must run on macOS"
for tool in curl shasum pkgutil ditto otool; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

if [[ -f "$PREFIX/lib/gstreamer-1.0/libgstwebrtc.dylib" && "${FORCE:-false}" != true ]]; then
    have="$("$PREFIX/bin/gst-inspect-1.0" --version 2>/dev/null | head -1 || true)"
    printf 'GStreamer already installed at %s (%s)\n' "$PREFIX" "${have:-unknown}"
    printf 'Set FORCE=true to reinstall.\n'
    exit 0
fi

# WHAT THIS VERIFICATION IS, AND WHAT IT IS NOT.
#
# The digests below are PINNED HERE, in this repository, and are the check that
# decides. Fetching the publisher's own .sha256sum over the same HTTPS host as
# the .pkg proves integrity — a truncated download, a bad CDN cache — and
# nothing about authenticity: anyone who could serve one could serve both, and
# they would move together.
#
# There is no stronger check available on this artifact. Measured, not assumed:
#
#   $ pkgutil --check-signature gstreamer-1.0-1.28.6-universal.pkg
#   Status: no signature
#
# The GStreamer project publishes a detached GPG signature (.asc) instead, which
# would need its release key on this runner to mean anything. Until that is
# provisioned, a pinned digest reviewed in a merge request is the control: a
# silently replaced upstream artifact fails here rather than being installed and
# shipped inside Lightning.app. Bumping GST_VERSION REQUIRES adding its digests.
pkg_digest() {
    case "$1" in
        gstreamer-1.0-1.28.6-universal.pkg)
            printf 'a8eb366c59b7e9e5dc049848fed6bcd203a8878aa7517c051639fda78797c6ad\n' ;;
        gstreamer-1.0-devel-1.28.6-universal.pkg)
            printf '177b1428d0f47b844e7bff2aeeb22047686d802eba21580dab52f4a6fe1dcf02\n' ;;
        *) return 1 ;;
    esac
}

mkdir -p "$DOWNLOAD_DIR"
# The runtime carries the libraries and plugins; the devel package carries the
# headers and the .pc files CMake's pkg_check_modules needs. Both are required.
for pkg in "gstreamer-1.0-$GST_VERSION-universal.pkg" \
           "gstreamer-1.0-devel-$GST_VERSION-universal.pkg"; do
    pinned="$(pkg_digest "$pkg")" \
        || die "no pinned SHA-256 for $pkg — add it before changing GST_VERSION"
    if [[ ! -f "$DOWNLOAD_DIR/$pkg" ]]; then
        printf 'downloading %s\n' "$pkg"
        curl -fsSL -o "$DOWNLOAD_DIR/$pkg.part" "$BASE_URL/$pkg"
        mv "$DOWNLOAD_DIR/$pkg.part" "$DOWNLOAD_DIR/$pkg"
    fi
    got="$(shasum -a 256 "$DOWNLOAD_DIR/$pkg" | awk '{print $1}')"
    [[ "$pinned" == "$got" ]] \
        || die "$pkg does not match the pinned digest (want $pinned, got $got)"
    # Secondary, and only a cross-check: if the publisher's own file disagrees
    # with the pin, something changed upstream and a human should look.
    if curl -fsSL -o "$DOWNLOAD_DIR/$pkg.sha256sum" "$BASE_URL/$pkg.sha256sum" 2>/dev/null; then
        upstream="$(awk '{print $1}' "$DOWNLOAD_DIR/$pkg.sha256sum")"
        [[ "$upstream" == "$pinned" ]] \
            || die "upstream now publishes a different digest for $pkg ($upstream)"
    else
        printf 'note: could not fetch the upstream .sha256sum for %s (pin still enforced)\n' "$pkg"
    fi
    printf 'verified %s  %s\n' "$pkg" "$got"
done

rm -rf -- "$FRAMEWORK" "$GST_ROOT/expand"
mkdir -p "$PREFIX"
for pkg in "gstreamer-1.0-$GST_VERSION-universal.pkg" \
           "gstreamer-1.0-devel-$GST_VERSION-universal.pkg"; do
    expand="$GST_ROOT/expand/${pkg%.pkg}"
    rm -rf -- "$expand"
    mkdir -p "$(dirname "$expand")"
    pkgutil --expand-full "$DOWNLOAD_DIR/$pkg" "$expand"
    installed=0
    for component in "$expand"/*.pkg; do
        [[ -d "$component/Payload" ]] || continue
        installed=$((installed + 1))
        location="$(grep -o 'install-location="[^"]*"' "$component/PackageInfo" \
            | head -1 | sed 's/install-location="//; s/"$//' || true)"
        case "$location" in
            */Versions/1.0)  dest="$PREFIX" ;;
            *GStreamer.framework*) dest="$FRAMEWORK" ;;
            *) die "unexpected install-location in $component: $location" ;;
        esac
        ditto "$component/Payload" "$dest"
    done
    # A flat package, or a layout change upstream, would leave the glob literal
    # and this loop silent — the element check below would then fail pointing at
    # the wrong thing.
    (( installed > 0 )) || die "no installable components found in $pkg"
done
rm -rf -- "$GST_ROOT/expand"

# Prove the install rather than assuming it: the elements below are exactly the
# ones SfuMediaEngine::runtimeAvailable() refuses to register without, plus the
# macOS capture sources the camera and screen share use.
# The versioned names are set alongside the unversioned ones because GStreamer
# reads GST_PLUGIN_PATH_1_0 / GST_PLUGIN_SYSTEM_PATH_1_0 / GST_REGISTRY_1_0
# FIRST: a leftover in the runner's environment would otherwise decide what this
# check actually scanned.
export GST_REGISTRY="$GST_ROOT/registry-install-check.bin"
export GST_REGISTRY_1_0="$GST_REGISTRY"
export GST_PLUGIN_SYSTEM_PATH="$PREFIX/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH_1_0="$GST_PLUGIN_SYSTEM_PATH"
export GST_PLUGIN_PATH=
export GST_PLUGIN_PATH_1_0=
rm -f "$GST_REGISTRY"
missing=0
for element in \
    webrtcbin nicesrc nicesink dtlssrtpenc dtlssrtpdec opusenc opusdec \
    rtpopuspay rtpopusdepay audioconvert audioresample audiotestsrc fakesink \
    autoaudiosrc autoaudiosink queue valve volume capsfilter vp8enc vp8dec \
    rtpvp8pay rtpvp8depay videoconvert videoscale videotestsrc videorate \
    identity level appsink appsrc autovideosrc avfvideosrc osxaudiosrc \
    osxaudiosink tee funnel webrtcdsp webrtcechoprobe compositor
do
    if ! "$PREFIX/bin/gst-inspect-1.0" "$element" >/dev/null 2>&1; then
        printf '  MISSING element: %s\n' "$element" >&2
        missing=$((missing + 1))
    fi
done
(( missing == 0 )) || die "$missing required GStreamer elements are absent from $PREFIX"

for module in gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
              gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0; do
    [[ -f "$PREFIX/lib/pkgconfig/$module.pc" ]] \
        || die "pkg-config module missing from the install: $module"
done

printf 'GStreamer %s installed at %s\n' "$GST_VERSION" "$PREFIX"
printf 'every required element resolves; all six pkg-config modules present\n'
printf 'point builds at it with: PKG_CONFIG_PATH=%s/lib/pkgconfig\n' "$PREFIX"
