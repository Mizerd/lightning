#!/usr/bin/env bash
# Builds the source RPM that Fedora COPR compiles: the tree at HEAD, the Rust
# crates vendored from rust/Cargo.lock, and a spec stamped with the version.
# Needs git, cargo (network for the crates), rpmbuild, tar, xz and gzip.
# COPR runs it through .copr/Makefile; the copr-srpm CI job runs it on the
# pinned source; by hand: build-copr-srpm.sh <outdir>, then upload the SRPM.
set -Eeuo pipefail
umask 022

OUTDIR="${1:?usage: build-copr-srpm.sh <outdir>}"
TOP="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SPEC_IN="$TOP/packaging-ci/packaging/rpm/lightning-copr.spec.in"

# COPR clones as another user, and git refuses to read such a tree. Allowed
# for this process only; nobody's git configuration is written.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0="$TOP"
cd "$TOP"

commit="$(git rev-parse HEAD)"
short="${commit:0:12}"
cmake_version="$(sed -n 's/^    VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt | head -n1)"
cargo_version="$(sed -n 's/^version = "\([0-9][0-9.]*\)"$/\1/p' rust/Cargo.toml | head -n1)"
[[ -n "$cmake_version" && "$cmake_version" == "$cargo_version" ]] || {
    printf 'version mismatch: CMakeLists.txt=%s rust/Cargo.toml=%s\n' \
        "$cmake_version" "$cargo_version" >&2
    exit 1
}

# A release tag builds as X.Y.Z. Anything else is a snapshot that sorts after
# the release it builds on and before the next one (Fedora's caret form).
# The remote is asked too, in case the clone came without its tags.
tag_commit=""
if [[ "$(git tag --points-at HEAD --list "v$cmake_version")" == "v$cmake_version" ]]; then
    tag_commit="$commit"
elif git remote get-url origin >/dev/null 2>&1; then
    tag_commit="$(git ls-remote --tags origin "v$cmake_version" "v$cmake_version^{}" 2>/dev/null |
        awk '$2 ~ /\^\{\}$/ { peeled = $1 } $2 !~ /\^\{\}$/ { plain = $1 }
             END { print (peeled != "" ? peeled : plain) }')" || tag_commit=""
fi
if [[ "$tag_commit" == "$commit" ]]; then
    version="$cmake_version"
else
    date="$(git log -1 --format=%cd --date=format:%Y%m%d)"
    version="${cmake_version}^${date}git${commit:0:7}"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
name="lightning-$short"
mtime="$(git log -1 --format=%cI)"

git archive --format=tar --prefix="$name/" HEAD | gzip -n >"$work/$name.tar.gz"

mkdir "$work/tree"
tar -xzf "$work/$name.tar.gz" -C "$work/tree"
(
    cd "$work/tree/$name"
    cargo vendor --quiet --locked --manifest-path rust/Cargo.toml rust/vendor >/dev/null
)
tar -C "$work/tree/$name" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="$mtime" -cJf "$work/$name-vendor.tar.xz" rust/vendor

sed -e "s|@VERSION@|$version|g" -e "s|@COMMIT@|$commit|g" \
    "$SPEC_IN" >"$work/lightning.spec"

mkdir -p "$OUTDIR"
rpmbuild -bs "$work/lightning.spec" \
    --define "_sourcedir $work" \
    --define "_srcrpmdir $OUTDIR"
