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
%{_bindir}/matrix-client
%{_datadir}/applications/lightning.desktop
%{_datadir}/icons/hicolor/*/apps/lightning.png
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
