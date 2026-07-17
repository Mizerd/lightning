Name:           lightning
Version:        %{pkg_version}
Release:        %{pkg_release}
Summary:        Lightning Matrix client
License:        LicenseRef-Proprietary
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
%{_datadir}/metainfo/lightning.metainfo.xml
%license %{_datadir}/licenses/lightning/copyright

%post
update-desktop-database -q %{_datadir}/applications || :

%postun
update-desktop-database -q %{_datadir}/applications || :

%changelog
* Fri Jul 17 2026 Mizerd <rsmetonis@gmail.com> - 0.6.0-1
- External private package pipeline
