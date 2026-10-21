#!/usr/bin/env bash
# The inline video card, its control bar and the expanded view in pure QML
# against the production sources (tests/qml/tst_VideoPlayer.qml): one speed
# for every bar, the speed menu's direction, the narrow-card layout, the
# keyboard map, the remembered position and the end of an expanded video. No
# build. The players play a sound from the tree, so this runs with no sound
# server reachable: nothing is ever audible.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
imports=$(mktemp -d)
trap 'rm -rf "$imports"' EXIT
mkdir -p "$imports/MatrixClient" "$imports/runtime"
chmod 700 "$imports/runtime"
printf 'module MatrixClient\n' > "$imports/MatrixClient/qmldir"
for name in AppTheme VideoPlayerCard VideoControlBar VideoViewerOverlay \
            MediaVolumeControl SoftwareVideoNotice IconButton Icon AppButton \
            AppMenu AppMenuItem MenuKeycap StormNode; do
    ln -s "$repo/qml/$name.qml" "$imports/MatrixClient/$name.qml"
    if [[ $name = AppTheme ]]; then
        printf 'singleton '
    fi
    printf '%s 1.0 %s.qml\n' "$name" "$name"
done >> "$imports/MatrixClient/qmldir"
# AppLocale is a C++ singleton of the real module; the cards only read its
# name, so a QML stand-in with the process locale's name serves here.
printf 'pragma Singleton\nimport QtQuick\nQtObject { readonly property string name: Qt.locale().name }\n' \
    > "$imports/MatrixClient/AppLocale.qml"
printf 'singleton AppLocale 1.0 AppLocale.qml\n' >> "$imports/MatrixClient/qmldir"
runner=${QMLTESTRUNNER:-$(command -v qmltestrunner || command -v qmltestrunner6 || true)}
# CTest treats 77 as skipped: no qmltestrunner here is not a failure.
[[ -n $runner ]] || { echo 'qmltestrunner not found; skipping' >&2; exit 77; }
# No session bus, and no PulseAudio, PipeWire or ALSA configuration: the
# players find no audio device and play silently.
env -u PULSE_SERVER -u PIPEWIRE_REMOTE \
    XDG_RUNTIME_DIR="$imports/runtime" PULSE_SERVER=unix:/nonexistent \
    PIPEWIRE_REMOTE=/nonexistent ALSA_CONFIG_PATH=/nonexistent \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QML_DISABLE_DISK_CACHE=1 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    "$runner" -import "$imports" \
        -input "$repo/tests/qml/tst_VideoPlayer.qml" "$@" \
        | tee "$imports/test.log"
status=${PIPESTATUS[0]}
[[ $status -eq 0 ]] || exit "$status"
# A loaded component is not evidence of correct bindings: Qt can abandon a
# binding after warning and still report the test itself as successful.
# "No audio device detected" and the failed PulseAudio connection are the
# point of the environment above.
if grep -Ev 'No audio device detected|PulseAudioService: pa_context_connect' \
        "$imports/test.log" \
        | grep -Eq '^QWARN|Binding loop detected|ReferenceError:|TypeError:'; then
    echo 'the video player emitted QML warnings' >&2
    exit 1
fi
