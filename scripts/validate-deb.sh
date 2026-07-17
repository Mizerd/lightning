#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
shopt -s nullglob
packages=("$ROOT"/dist/*.deb)
(( ${#packages[@]} == 1 )) || die "expected exactly one DEB package"
package="${packages[0]}"

dpkg-deb --info "$package"
dpkg-deb --contents "$package"
lintian --fail-on error "$package" | tee "$ROOT/dist/lintian.log"
apt-get install -y "$package"
desktop-file-validate /usr/share/applications/lightning.desktop
appstreamcli validate --no-net /usr/share/metainfo/lightning.metainfo.xml
matrix-client --version
if ldd /usr/bin/matrix-client | tee "$ROOT/dist/ldd.txt" | grep -q 'not found'; then
    die "installed executable has missing shared libraries"
fi
(cd "$ROOT/dist" && sha256sum -c "$(basename "$package").sha256")
