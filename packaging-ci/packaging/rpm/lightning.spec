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

# Everything below is dlopen'd, so rpm's dependency generator cannot see it,
# and it is named so that one RPM resolves on Fedora AND openSUSE: package
# names differ there (gstreamer1-plugins-bad-free vs gstreamer-plugins-bad,
# libnice-gstreamer1 vs gstreamer-libnice, ...), the capabilities below do not.
#
# GStreamer: one element per plugin package the call engine probes, by the
# gstreamer1(element-...) provides both distributions generate.
#   webrtcbin    : -bad(-free) — webrtcbin, dtlssrtpenc/dec, srtp
#   nicesrc      : the libnice plugin (ICE transport)
#   opusenc      : -base — opus, audio/videoconvert, videoscale, videorate
#   vp8enc       : -good — vp8, rtpopus/rtpvp8 pay+depay, ximagesrc
#   autoaudiosrc : -good — autoaudiosrc/sink (plus pulse; ALSA is in -base)
#   pipewiresrc  : the PipeWire plugin (portal screen capture)
%global gst_element() gstreamer1(element-%1)()(%{__isa_bits}bit)
Requires:       %{gst_element webrtcbin}
Requires:       %{gst_element nicesrc}
Requires:       %{gst_element opusenc}
Requires:       %{gst_element vp8enc}
Requires:       %{gst_element autoaudiosrc}
Requires:       %{gst_element pipewiresrc}
# enchant-2 is dlopen'd for spell checking; optional.
Recommends:     libenchant-2.so.2()(%{__isa_bits}bit)

# QML modules the UI imports. Fedora ships them inside the Qt library
# packages (provides qt6qml(<module>)); openSUSE ships them separately
# (provides qt6qmlimport(<module>)). Without them no window loads.
%global qml_module() (qt6qml(%1) or qt6qmlimport(%1))
Requires:       %{qml_module QtQuick}
Requires:       %{qml_module QtQuick.Controls}
Requires:       %{qml_module QtQuick.Controls.Basic}
Requires:       %{qml_module QtQuick.Layouts}
Requires:       %{qml_module QtQuick.Effects}
Requires:       %{qml_module QtQuick.Dialogs}
Requires:       %{qml_module QtQuick.Window}
Requires:       %{qml_module QtQuick.Shapes}
Requires:       %{qml_module QtMultimedia}

# Qt image-format plugins: WebP is required because the client's MIME
# sniffers accept it (no common provide, so a rich dependency). JPEG XL comes
# only from kf6-kimageformats, which pulls a large chain, so it is a
# Recommends; missing formats are reported by --image-format-status.
Requires:       (qt6-qtimageformats or qt6-imageformats)
Recommends:     kf6-kimageformats
# A colour emoji font (Fedora's name, then openSUSE's): without one a minimal
# install draws emoji monochrome or as tofu.
Recommends:     (google-noto-color-emoji-fonts or noto-coloremoji-fonts)

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
