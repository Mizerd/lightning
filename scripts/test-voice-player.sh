#!/usr/bin/env bash
# The voice/audio player's QML in pure QML against the production sources and
# a recording stand-in for app.voicePlayback: the docked strip and the
# popped-out card (free move, resize, snap, persistence relative to the
# window, never off screen, keyboard), the scrubber as a seek target, and the
# timeline row (fixed height, idle seek, waveform, stable controls, text size,
# per-theme contrast). No build.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
imports=$(mktemp -d)
trap 'rm -rf "$imports"' EXIT
mkdir -p "$imports/MatrixClient"
printf 'module MatrixClient\n' > "$imports/MatrixClient/qmldir"
for name in AppTheme VoiceMiniPlayer VoicePlayerBar VoicePlayerLogic \
            VoiceSeekBar VoiceSpeedChip AudioPlayerCard IconButton Icon \
            Avatar Skeleton AnimationFileHold MediaVolumeControl AppMenu \
            AppMenuItem MenuKeycap StormNode MotionPolicy; do
    ln -s "$repo/qml/$name.qml" "$imports/MatrixClient/$name.qml"
    if [[ $name = AppTheme || $name = MotionPolicy ]]; then
        printf 'singleton '
    fi
    printf '%s 1.0 %s.qml\n' "$name" "$name"
done >> "$imports/MatrixClient/qmldir"
# IdentityPalette is a C++ singleton (avatar colours); a fixed stand-in.
cat > "$imports/MatrixClient/IdentityPalette.qml" <<'QML'
pragma Singleton
import QtQuick
QtObject {
    function disc(index, anchor) { return "#6b7280" }
    function ink(index, anchor) { return "#ffffff" }
}
QML
printf 'singleton IdentityPalette 1.0 IdentityPalette.qml\n' \
    >> "$imports/MatrixClient/qmldir"
# AppLocale is a C++ singleton too; the card only reads its name.
cat > "$imports/MatrixClient/AppLocale.qml" <<'QML'
pragma Singleton
import QtQuick
QtObject { readonly property string name: Qt.locale().name }
QML
printf 'singleton AppLocale 1.0 AppLocale.qml\n' >> "$imports/MatrixClient/qmldir"
runner=${QMLTESTRUNNER:-$(command -v qmltestrunner || command -v qmltestrunner6 || true)}
# CTest treats 77 as skipped: no qmltestrunner here is not a failure.
[[ -n $runner ]] || { echo 'qmltestrunner not found; skipping' >&2; exit 77; }
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QML_DISABLE_DISK_CACHE=1 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    "$runner" -import "$imports" \
        -input "$repo/tests/qml/tst_VoicePlayer.qml" "$@" \
        | tee "$imports/test.log"
status=${PIPESTATUS[0]}
# A loaded component is not evidence of correct bindings: Qt can abandon a
# binding after warning and still report the test itself as successful.
if grep -Eq '^QWARN|Binding loop detected|ReferenceError:|TypeError:' "$imports/test.log"; then
    echo 'voice player emitted QML warnings' >&2
    exit 1
fi
exit "$status"
