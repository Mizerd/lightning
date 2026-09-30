#!/usr/bin/env bash
# Fedora COPR (mizerd/lightning) builds Lightning from .copr/Makefile when a
# release tag is pushed, where no pipeline watches. This assembles the same
# source RPM from the pinned source, exactly as COPR does, and compares the
# runtime dependencies rpm expands from its spec with the release RPM's, so a
# COPR build that would break shows up here first.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
umask 022

SOURCE_DIR="$ROOT/work/lightning"
[[ -d "$SOURCE_DIR/.git" ]] || die "the pinned source checkout is missing"
if [[ ! -f "$SOURCE_DIR/.copr/Makefile" ]]; then
    printf 'copr-srpm: the pinned source predates the COPR entry point; nothing to check\n'
    exit 0
fi

out="$ROOT/dist/copr"
rm -rf "$out"
# COPR's own invocation: make -f <clone>/.copr/Makefile srpm outdir=... spec=...
make -f "$SOURCE_DIR/.copr/Makefile" srpm outdir="$out" spec=

shopt -s nullglob
srpms=("$out"/*.src.rpm)
(( ${#srpms[@]} == 1 )) || die "expected exactly one source RPM, found ${#srpms[@]}"
srpm="${srpms[0]}"
rpm -qpl "$srpm" | tee "$ROOT/dist/copr-srpm-contents.txt"
# The spec and the two tarballs, nothing else.
(( $(wc -l <"$ROOT/dist/copr-srpm-contents.txt") == 3 )) || \
    die "the source RPM should hold the spec and two tarballs"
grep -qx 'lightning.spec' "$ROOT/dist/copr-srpm-contents.txt" || die "the source RPM has no lightning.spec"
grep -qE '^lightning-[0-9a-f]{12}[.]tar[.]gz$' "$ROOT/dist/copr-srpm-contents.txt" || \
    die "the source RPM has no source tarball"
grep -qE '^lightning-[0-9a-f]{12}-vendor[.]tar[.]xz$' "$ROOT/dist/copr-srpm-contents.txt" || \
    die "the source RPM has no vendored crates"

# Built from the pinned commit, at the version the pipeline resolved.
rpm -qp --qf '%{VERSION}\n' "$srpm" | tee "$ROOT/dist/copr-srpm-version.txt"
srpm_version="$(cat "$ROOT/dist/copr-srpm-version.txt")"
[[ "$srpm_version" == "$BASE_VERSION" || "$srpm_version" == "$BASE_VERSION^"* ]] || \
    die "the source RPM is version $srpm_version, not $BASE_VERSION"

spec_dir="$(mktemp -d)"
trap 'rm -rf "$spec_dir"' EXIT
# rpm writes payload names as ./lightning.spec
(cd "$spec_dir" && rpm2cpio "$srpm" | cpio -idm --quiet '*lightning.spec')
grep -q "^%global commit $SOURCE_SHA\$" "$spec_dir/lightning.spec" || \
    die "the source RPM was not stamped with the pinned commit $SOURCE_SHA"

# Expanded by rpm, not compared as text: what COPR's package will require
# against what the release RPM requires. desktop-file-utils, and the /bin/sh
# rpm adds for them, serve only the release spec's %post/%postun scriptlets.
expand() { # spec [defines...]
    local spec="$1"; shift
    { rpmspec -q --requires "$@" "$spec"; rpmspec -q --recommends "$@" "$spec" | sed 's/^/recommends: /'; } \
        | grep -vx -e 'desktop-file-utils' -e '/bin/sh' | sort
}
expand "$spec_dir/lightning.spec" >"$ROOT/dist/copr-requires.txt"
expand "$ROOT/packaging-ci/packaging/rpm/lightning.spec" \
    --define "pkg_version $RPM_VERSION" --define "pkg_release $RPM_RELEASE" \
    --define "pkg_source_sha $SOURCE_SHA" --define "stage_root /nonexistent" \
    >"$ROOT/dist/copr-release-requires.txt"
cat "$ROOT/dist/copr-requires.txt"
(( $(wc -l <"$ROOT/dist/copr-requires.txt") >= 15 )) || \
    die "rpmspec expanded too few dependencies; the comparison below would prove nothing"
diff -u "$ROOT/dist/copr-release-requires.txt" "$ROOT/dist/copr-requires.txt" || \
    die "the COPR package and the release RPM require different things (diff above)"
printf 'copr-srpm: %s assembled; its runtime dependencies match the release RPM\n' "$(basename "$srpm")"
