#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

LIGHTNING_INSTALL_TYPE=linux-deb "$SCRIPT_DIR/configure-build.sh"

STAGE="$ROOT/work/stage"
PKGROOT="$ROOT/work/deb-root"
CONTROL="$PKGROOT/DEBIAN"
mkdir -p "$CONTROL" "$ROOT/dist" "$ROOT/work/debian"
cp -a "$STAGE/." "$PKGROOT/"

# lintian: unstripped-binary-or-object applies to both shipped ELFs.
strip "$PKGROOT/usr/bin/lightning-matrix"
strip "$PKGROOT/usr/bin/lightning-updater"

install -Dm0644 "$ROOT/packaging-ci/packaging/common/copyright" \
    "$PKGROOT/usr/share/doc/lightning/copyright"

# lintian: no-changelog. A native package ships changelog.gz at -9.
MAINTAINER="$(sed -n 's/^Maintainer:[[:space:]]*//p' "$ROOT/packaging-ci/packaging/deb/control")"
{
    printf 'lightning (%s) unstable; urgency=medium\n\n' "$DEB_VERSION"
    printf '  * Automated package build from Lightning source %s.\n\n' "$SOURCE_SHA"
    printf ' -- %s  %s\n' "$MAINTAINER" "$(date -R)"
} >"$ROOT/work/changelog"
install -d "$PKGROOT/usr/share/doc/lightning"
gzip -9nc "$ROOT/work/changelog" >"$PKGROOT/usr/share/doc/lightning/changelog.gz"

# dpkg-shlibdeps needs a source-package debian/control, not the binary one.
cat >"$ROOT/work/debian/control" <<'EOF'
Source: lightning

Package: lightning
Architecture: amd64
EOF
# Analyse both ELFs so a new dependency of the update helper is declared too.
SHLIBS_OUT="$ROOT/work/shlibdeps.out"
if ! ( cd "$ROOT/work" && dpkg-shlibdeps -O \
        -e "$PKGROOT/usr/bin/lightning-matrix" \
        -e "$PKGROOT/usr/bin/lightning-updater" ) >"$SHLIBS_OUT" 2>&1; then
    printf 'dpkg-shlibdeps failed:\n' >&2
    cat "$SHLIBS_OUT" >&2
    die "dpkg-shlibdeps could not resolve runtime dependencies"
fi
SHLIBS="$(sed -n 's/^shlibs:Depends=//p' "$SHLIBS_OUT")"
[[ -n "$SHLIBS" ]] || die "dpkg-shlibdeps did not determine runtime dependencies"
# QML imports are loaded dynamically, so dpkg-shlibdeps cannot see them. Keep
# aligned with the production QML import scan; validate-deb enforces it.
QML_DEPENDS="qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-dialogs, qml6-module-qtquick-effects, qml6-module-qtquick-layouts, qml6-module-qtquick-window, qml6-module-qtmultimedia"

# GStreamer plugins are dlopen'd, so dpkg-shlibdeps cannot see them either;
# without them every call is refused by the engine's element probe.
#   plugins-base : opus, audioconvert/resample, videoconvert/scale/rate
#   plugins-good : rtpopus/rtpvp8 (de)payloaders, autoaudiosrc/sink, vp8
#   plugins-bad  : webrtcbin, dtlssrtpenc/dec, srtp
#   nice         : libnice ICE transport
#   pipewire     : pipewiresrc (portal screen capture) and pipewiresink
#   alsa         : alsasink, a separate Debian package. Without it a host with
#                  neither PipeWire nor PulseAudio has no audio output, and
#                  --call-media-status cannot tell (it only probes autodetect).
# Fedora ships alsa/pulse in plugins-base/good, so the spec needs no equivalent.
CALL_DEPENDS="gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-nice, gstreamer1.0-pipewire, gstreamer1.0-alsa"

# Qt image-format plugins are dlopen'd too; libqt6gui6 carries only
# gif/ico/jpeg. WebP is a Depends because the client's MIME sniffers accept
# it. JPEG XL comes only from KDE's kimageformats, which pulls a large chain
# (libheif, x265, libraw, OpenEXR), so it is a Recommends; the client reports
# missing formats via --image-format-status.
IMAGE_DEPENDS="qt6-image-formats-plugins"
IMAGE_RECOMMENDS="kimageformat6-plugins"

{
    cat "$ROOT/packaging-ci/packaging/deb/control"
    printf 'Version: %s\n' "$DEB_VERSION"
    printf 'Depends: %s, %s, %s, %s\n' \
        "$SHLIBS" "$QML_DEPENDS" "$CALL_DEPENDS" "$IMAGE_DEPENDS"
    # libenchant-2-2 is dlopen'd for spell checking; optional.
    printf 'Recommends: %s, libenchant-2-2\n' "$IMAGE_RECOMMENDS"
} >"$CONTROL/control"
install -m0755 "$ROOT/packaging-ci/packaging/deb/postinst" "$CONTROL/postinst"
install -m0755 "$ROOT/packaging-ci/packaging/deb/postrm" "$CONTROL/postrm"

# dpkg-shlibdeps bakes the build host's library versions into Depends, so a
# Debian 13 deb cannot install on Ubuntu 24.04; each distro gets its own lane.
# DEB_SUFFIX comes from the CI job (empty for Debian, keeping its filename
# stable) so the two lanes' artifacts never collide.
PACKAGE="$ROOT/dist/lightning_${DEB_VERSION}${DEB_SUFFIX:+_$DEB_SUFFIX}_amd64.deb"
dpkg-deb --build --root-owner-group "$PKGROOT" "$PACKAGE"
write_sha256 "$PACKAGE"
printf 'Built %s\n' "$PACKAGE"
