#!/usr/bin/env bash
# The media browser's grid tile in pure QML against the production source and
# a recording media bridge: which media class each tile asks for, the bounds on
# full-file fallbacks, and no binding loop when a picture loads. No build.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
imports=$(mktemp -d)
trap 'rm -rf "$imports"' EXIT
mkdir -p "$imports/MatrixClient"
printf 'module MatrixClient\n' > "$imports/MatrixClient/qmldir"
for name in AppTheme MediaBrowserTile Icon AppMenu AppMenuItem MenuKeycap StormNode; do
    ln -s "$repo/qml/$name.qml" "$imports/MatrixClient/$name.qml"
    if [[ $name = AppTheme ]]; then
        printf 'singleton '
    fi
    printf '%s 1.0 %s.qml\n' "$name" "$name"
done >> "$imports/MatrixClient/qmldir"
runner=${QMLTESTRUNNER:-$(command -v qmltestrunner || command -v qmltestrunner6 || true)}
# CTest treats 77 as skipped: no qmltestrunner here is not a failure.
[[ -n $runner ]] || { echo 'qmltestrunner not found; skipping' >&2; exit 77; }
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QML_DISABLE_DISK_CACHE=1 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    "$runner" -import "$imports" \
        -input "$repo/tests/qml/tst_MediaBrowserTile.qml" "$@" \
        | tee "$imports/test.log"
# A loaded component is not evidence of correct bindings: Qt can abandon a
# binding after warning and still report the test itself as successful.
if grep -Eq '^QWARN|Binding loop detected|ReferenceError:|TypeError:' "$imports/test.log"; then
    echo 'media browser tile emitted QML warnings' >&2
    exit 1
fi
