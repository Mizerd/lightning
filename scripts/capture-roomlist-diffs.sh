#!/usr/bin/env bash
# Capture and judge the "room_list malformed diff rejected" storm.
#
# The storm came from two producers sharing one index base: SDK diffs address
# entries_with_dynamic_adapters while the snapshot came from client.rooms(),
# which has a different order and includes Spaces. On a small account the two
# orders coincide, so this needs a large account with Spaces to confirm.
#
# HOW TO USE IT
#
#   scripts/capture-roomlist-diffs.sh                 # capture, then judge
#   scripts/capture-roomlist-diffs.sh --judge FILE    # judge an existing log
#
# During the capture, on an account with many rooms and Spaces:
#
#   1. let the room list settle,
#   2. Mark as read on a room well down the list (index 0 can still match
#      a drifted base),
#   3. favourite and un-favourite another,
#   4. open a Space and come back to Home,
#   5. quit normally.
#
# A pass is zero rejections. Each rejection names the op, the index, the
# expected room and the room actually at that index; together they show
# which producer drifted.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO/build-rust/lightning-matrix"

judge() {
    local log="$1"
    [[ -f "$log" ]] || { echo "no such log: $log" >&2; return 2; }
    local rejects resyncs
    rejects=$(grep -c "room_list malformed diff rejected" "$log" || true)
    resyncs=$(grep -c "room_list resync" "$log" || true)
    echo "log:         $log"
    echo "rejections:  $rejects"
    echo "resyncs:     $resyncs"
    if [[ "$rejects" -gt 0 ]]; then
        echo
        echo "First five, with the four fields that matter:"
        grep "room_list malformed diff rejected" "$log" | head -5
        echo
        echo "RESULT: FAIL — b0c27ee did not close it on this account shape."
        return 1
    fi
    # Zero is only meaningful if the actions ran; an idle capture also
    # reports zero.
    local marks
    marks=$(grep -cE "read receipt|mark_as_read|markAsRead" "$log" || true)
    if [[ "$marks" -eq 0 ]]; then
        echo
        echo "RESULT: INCONCLUSIVE — no mark-as-read reached the log, so the"
        echo "action that triggers the defect never ran. Re-capture and follow"
        echo "the steps in the header of this script."
        return 3
    fi
    echo
    echo "RESULT: PASS — no rejection after $marks read-receipt line(s)."
    return 0
}

if [[ "${1:-}" == "--judge" ]]; then
    judge "${2:?usage: --judge <logfile>}"
    exit $?
fi

[[ -x "$BIN" ]] || { echo "build build-rust first: $BIN is missing" >&2; exit 2; }
LOG="${LIGHTNING_DIFF_LOG:-/tmp/lightning-roomlist-$(date +%Y%m%d-%H%M%S).log}"

echo "Capturing to $LOG"
echo "Follow the steps in the header of this script, then quit Lightning."
echo
# Required when output is redirected; otherwise Qt logs to journald.
QT_FORCE_STDERR_LOGGING=1 \
QT_LOGGING_RULES="matrix.rust=true" \
    "$REPO/scripts/run-dev.sh" --log-file "$LOG"

echo
judge "$LOG"
