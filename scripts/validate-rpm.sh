#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
shopt -s nullglob
packages=("$ROOT"/dist/*.rpm)
(( ${#packages[@]} == 1 )) || die "expected exactly one RPM package"
package="${packages[0]}"

rpm -qpi "$package"
rpm -qlp "$package"
# rpmlint returns non-zero for warnings too; log everything but only treat
# genuine errors (E: lines) as fatal, mirroring the DEB lintian --fail-on error.
rpmlint "$package" >"$ROOT/dist/rpmlint.log" 2>&1 || true
cat "$ROOT/dist/rpmlint.log"
if grep -qE '(^|: )E: ' "$ROOT/dist/rpmlint.log"; then
    die "rpmlint reported errors"
fi
dnf install -y "$package"
desktop-file-validate /usr/share/applications/lightning.desktop
appstreamcli validate --no-net /usr/share/metainfo/lightning.metainfo.xml
matrix-client --version
if ldd /usr/bin/matrix-client | tee "$ROOT/dist/ldd.txt" | grep -q 'not found'; then
    die "installed executable has missing shared libraries"
fi
(cd "$ROOT/dist" && sha256sum -c "$(basename "$package").sha256")
