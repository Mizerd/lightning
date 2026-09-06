# The binary is packaged from a pre-staged install root; do not split out a
# separate -debuginfo/-debugsource subpackage (there are no build sources here
# and the "exactly one RPM" packaging check expects a single artifact).
%global debug_package %{nil}

Name:           lightning
Version:        %{pkg_version}
Release:        %{pkg_release}
Summary:        Native Matrix desktop client
License:        GPL-3.0-or-later
URL:            https://gitlab.smetonis.net/Mizerd/lightning

Requires:       desktop-file-utils

# Voice/video calling. GStreamer PLUGINS are dlopen'd from a plugin path at
# runtime, so RPM's automatic dependency generator cannot see them — it reads
# ELF NEEDED entries, and the binary links only gstreamer core/webrtc/sdp.
# Without these the package installs cleanly and then refuses every call,
# because the engine's runtime element probe fails.
#
#   plugins-base     : opus, audioconvert/resample, videoconvert/scale/rate
#   plugins-good     : rtpopus/rtpvp8 pay+depay, autoaudiosrc/sink, vp8
#   plugins-bad-free : webrtcbin, dtlssrtpenc/dec, srtp (the WebRTC core)
#   libnice          : the ICE transport webrtcbin requires
#   pipewire         : pipewiresrc, the Wayland/portal screen-capture source,
#                      and pipewiresink
#
# The three sinks autoaudiosink can resolve to are all covered already, which
# is NOT true on Debian and is worth recording so nobody "fixes" it here:
# libgstalsa.so is in gstreamer1-plugins-base and libgstpulseaudio.so in
# gstreamer1-plugins-good on Fedora, whereas Debian ships ALSA in a separate
# gstreamer1.0-alsa package (see CALL_DEPENDS in scripts/build-deb.sh). Without
# a sink a call installs, reports every engine check green — the probe only asks
# for the `autodetect` FACTORIES — and produces no sound.
# plugins-good also carries libgstximagesrc, the X11 screen-share fallback.
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Requires:       gstreamer1-plugins-bad-free
Requires:       libnice-gstreamer1
Requires:       pipewire-gstreamer
# Spell checking is enchant-2 resolved at RUNTIME (dlopen), never linked, so
# rpm cannot see it. Recommends, not Requires: without it the composer reports
# "spell checking unavailable" and everything else works. Dictionaries are the
# user's (hunspell-*).
Recommends:     enchant2

# Qt IMAGE-FORMAT plugins, invisible to rpm's automatic dependency generator
# for exactly the same reason as the GStreamer plugins above: they are
# dlopen'd, never linked. Fedora's qt6-qtbase-gui carries libqgif, libqico and
# libqjpeg and nothing else, so without these the RPM decodes GIF/ICO/JPEG plus
# qtbase's built-in PNG/BMP/PPM/XBM/XPM — and no more.
#
# qt6-qtimageformats is a hard Requires because it provides libqwebp.so, and
# Lightning's own byte sniffers ACCEPT image/webp (rooms::sniff_image_mime and
# its C++ twins). Without it the client accepts, forwards and re-uploads a
# format it cannot draw.
#
# kf6-kimageformats is a Recommends — dnf installs weak dependencies by
# default — and it provides kimg_jxl.so, the ONLY Qt JPEG XL decoder that
# exists anywhere: upstream qtimageformats has never contained one. Weak
# rather than hard because it pulls libheif, x265, LibRaw and OpenEXR for the
# one format Lightning wants from it, and because the client can be asked
# which formats the running build decodes (`lightning-matrix
# --image-format-status`), so its absence is visible rather than silent.
Requires:       qt6-qtimageformats
Recommends:     kf6-kimageformats

%description
A native C++ and Qt Matrix desktop client with the Matrix Rust SDK backend.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a %{stage_root}/. %{buildroot}/

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/lightning.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/lightning.metainfo.xml

%files
%{_bindir}/lightning-matrix
# The update helper, installed alongside the application by cmake --install.
# %install copies the WHOLE staged tree and rpm's default
# _unpackaged_files_terminate_build is 1, so omitting this line does not ship a
# smaller package — it aborts build-rpm with "Installed (but unpackaged)
# file(s) found: /usr/bin/lightning-updater", exactly as the missing scalable
# icon broke pipeline 97.
%{_bindir}/lightning-updater
%{_datadir}/applications/lightning.desktop
%{_datadir}/icons/hicolor/*/apps/lightning.png
%{_datadir}/icons/hicolor/scalable/apps/lightning.svg
%{_datadir}/metainfo/lightning.metainfo.xml
%license %{_datadir}/licenses/lightning/copyright
%license %{_docdir}/lightning/LICENSE
%doc %{_docdir}/lightning/README.md

%post
update-desktop-database -q %{_datadir}/applications || :

%postun
update-desktop-database -q %{_datadir}/applications || :

%changelog
* Fri Jul 17 2026 Mizerd <rsmetonis@gmail.com> - %{pkg_version}-%{pkg_release}
- Automated package build from Lightning source %{pkg_source_sha}
