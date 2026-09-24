# Packaged from a pre-staged install root: no debuginfo subpackage, and
# build-rpm.sh expects exactly one RPM.
%global debug_package %{nil}

Name:           lightning
Version:        %{pkg_version}
Release:        %{pkg_release}
Summary:        Native Matrix desktop client
License:        GPL-3.0-or-later
URL:            https://gitlab.smetonis.net/Mizerd/lightning

Requires:       desktop-file-utils

# GStreamer plugins are dlopen'd, so rpm's dependency generator cannot see
# them; without them every call is refused by the engine's element probe.
#   plugins-base     : opus, audioconvert/resample, videoconvert/scale/rate
#   plugins-good     : rtpopus/rtpvp8 pay+depay, autoaudiosrc/sink, vp8,
#                      ximagesrc (X11 screen share)
#   plugins-bad-free : webrtcbin, dtlssrtpenc/dec, srtp
#   libnice          : ICE transport
#   pipewire         : pipewiresrc (portal screen capture) and pipewiresink
# Unlike Debian, Fedora ships the ALSA and Pulse sinks in plugins-base/good,
# so no separate audio sink package is needed.
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Requires:       gstreamer1-plugins-bad-free
Requires:       libnice-gstreamer1
Requires:       pipewire-gstreamer
# enchant-2 is dlopen'd for spell checking; optional.
Recommends:     enchant2

# Qt image-format plugins are dlopen'd too; qt6-qtbase-gui carries only
# gif/ico/jpeg. WebP (qt6-qtimageformats) is required because the client's
# MIME sniffers accept it. JPEG XL comes only from kf6-kimageformats, which
# pulls a large chain, so it is a Recommends; missing formats are reported by
# --image-format-status.
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
# %install copies the whole staged tree, so every installed file must be
# listed here or rpmbuild aborts on unpackaged files.
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
