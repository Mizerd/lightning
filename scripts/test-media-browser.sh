#!/usr/bin/env bash
# The media browser (keyboard, autoscroll, reload when shown again after a
# while) and its row, in pure QML against the production sources and a stub
# model and bridge. No build. Same shape as test-media-browser-tile.sh.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
imports=$(mktemp -d)
trap 'rm -rf "$imports"' EXIT
mkdir -p "$imports/MatrixClient"
printf 'module MatrixClient\n' > "$imports/MatrixClient/qmldir"
for name in AppTheme MediaBrowser MediaBrowserTile MediaBrowserRow Icon AppMenu \
            AppMenuItem AppMenuSeparator MenuKeycap StormNode AppButton AppTextField AppComboBox \
            AppScrollBar SmoothWheelArea MiddleClickScroller IconButton; do
    ln -s "$repo/qml/$name.qml" "$imports/MatrixClient/$name.qml"
    if [[ $name = AppTheme ]]; then
        printf 'singleton '
    fi
    printf '%s 1.0 %s.qml\n' "$name" "$name"
done >> "$imports/MatrixClient/qmldir"
# AppLocale is a C++ singleton in the app; the row only reads its name.
printf 'pragma Singleton\nimport QtQml\nQtObject { property string name: "en" }\n' \
    > "$imports/MatrixClient/AppLocale.qml"
printf 'singleton AppLocale 1.0 AppLocale.qml\n' >> "$imports/MatrixClient/qmldir"
runner=${QMLTESTRUNNER:-$(command -v qmltestrunner || command -v qmltestrunner6 || true)}
[[ -n $runner ]] || { echo 'qmltestrunner not found; skipping' >&2; exit 77; }
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QML_DISABLE_DISK_CACHE=1 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    "$runner" -import "$imports" \
        -input "$repo/tests/qml/tst_MediaBrowser.qml" "$@" \
        | tee "$imports/test.log"
if grep -Eq '^QWARN|Binding loop detected|ReferenceError:|TypeError:' "$imports/test.log"; then
    echo 'media browser emitted QML warnings' >&2
    exit 1
fi
