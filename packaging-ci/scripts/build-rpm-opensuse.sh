#!/usr/bin/env bash
# Compiles the COPR source RPM (the copr-srpm job's artifact) on openSUSE Leap
# 16.0, exactly as COPR's opensuse-leap-16.0 chroot does after a tag push:
# lightning-copr.spec.in's %{?suse_version} branch, openSUSE's own Qt,
# GStreamer, OpenSSL and Rust, no network. COPR builds where no pipeline
# watches; this is the watch, and its package is the one testers install on
# openSUSE. It is never published: the release assets stay the Fedora-built
# .rpm, which also installs on Tumbleweed (validate-rpm-opensuse).
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
umask 022

distro="$(. /etc/os-release && printf '%s %s' "${ID:-}" "${VERSION_ID:-}")"
[[ "$distro" == "opensuse-leap 16.0" ]] || die "build-rpm-opensuse runs on openSUSE Leap 16.0, not '$distro'"

shopt -s nullglob
srpms=("$ROOT"/dist/copr/*.src.rpm)
(( ${#srpms[@]} == 1 )) || die "expected the copr-srpm job's one source RPM, found ${#srpms[@]}"
srpm="${srpms[0]}"
srpm_version="$(rpm -qp --qf '%{VERSION}' "$srpm")"
[[ "$srpm_version" == "$BASE_VERSION" || "$srpm_version" == "$BASE_VERSION^"* ]] || \
    die "the source RPM is version $srpm_version, not $BASE_VERSION"

# What the spec's %{?suse_version} branch asks for, resolved by zypper as
# COPR's chroot would. rpmlib() capabilities are rpm's own.
mapfile -t buildrequires < <(rpm -qpR "$srpm" | grep -v '^rpmlib(')
(( ${#buildrequires[@]} >= 20 )) || \
    die "the source RPM declares ${#buildrequires[@]} BuildRequires; the openSUSE branch was not taken"
printf '%s\n' "${buildrequires[@]}" | tee "$ROOT/dist/rpm-opensuse-buildrequires.txt"
zypper --non-interactive install --no-recommends "${buildrequires[@]}"

TOPDIR="$ROOT/work/rpmbuild-opensuse"
rm -rf "$TOPDIR"
mkdir -p "$TOPDIR"
# The spec's own %check runs the binary: version, Rust backend, the call
# engine compiled in, no RPATH, no Qt private-ABI import.
if ! rpmbuild --rebuild "$srpm" \
        --define "_topdir $TOPDIR" \
        --define "_smp_mflags -j${BUILD_JOBS:-2}" \
        2>&1 | tee "$ROOT/dist/rpm-opensuse-rpmbuild.log"; then
    die "rpmbuild failed on openSUSE Leap 16.0 (log above)"
fi

packages=("$TOPDIR"/RPMS/*/*.rpm)
(( ${#packages[@]} == 1 )) || die "expected exactly one binary RPM, found ${#packages[@]}"
# Kept under dist/opensuse/ with rpmbuild's own lightning-matrix-* name, so
# the dist/*.rpm globs of validate-rpm and publication never see it beside
# the release .rpm (the two-.deb hazard), and nothing publishes it.
mkdir -p "$ROOT/dist/opensuse"
PACKAGE="$ROOT/dist/opensuse/$(basename "${packages[0]}")"
cp "${packages[0]}" "$PACKAGE"
write_sha256 "$PACKAGE"
printf 'Built %s\n' "$PACKAGE"
