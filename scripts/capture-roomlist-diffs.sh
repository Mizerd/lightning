#!/usr/bin/env bash
# Capture and judge the "room_list malformed diff rejected" storm.
#
# WHAT THIS ANSWERS. b0c27ee (2026-09-08) found the cause — TWO producers wrote
# one index base: the SDK's diffs address the vector from
# entries_with_dynamic_adapters, while the snapshot came from client.rooms(),
# a different set in a different order with Spaces in it, and both were handed
# to handleRoomsEvent. So mark-as-read, favourite, accept-an-invite, create or
# leave a room replaced the index base, the next set{index} addressed a
# different room, was rejected, and the rejection called resync, which
# re-emitted the same snapshot.
#
# It has never been confirmed on a real account, and it CANNOT be confirmed on
# a small one: on a small account the two orders coincide, which is exactly why
# the report was account-shape dependent.
#
# HOW TO USE IT
#
#   scripts/capture-roomlist-diffs.sh                 # capture, then judge
#   scripts/capture-roomlist-diffs.sh --judge FILE    # judge an existing log
#
# During the capture, on an account with MANY rooms (the more the better, and
# it must have Spaces):
#
#   1. let the room list settle,
#   2. Mark as read on a room well down the list — NOT the first one, since an
#      index-0 write is the one case a drifted base can still get right,
#   3. favourite and un-favourite another,
#   4. open a Space and come back to Home,
#   5. quit normally.
#
# A PASS is zero rejections. Any rejection line names the op, the index it
# addressed, the room it expected and the room the index was actually holding —
# read those four together, because they say which producer drifted.
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
    # ZERO IS ONLY MEANINGFUL IF THE ACTIONS RAN. A capture where nothing was
    # clicked also reports zero, and that is the shape of a vacuous pass.
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
# QT_FORCE_STDERR_LOGGING is MANDATORY for a launch whose output is redirected:
# without it Qt hands every line to journald and the log file holds nothing.
QT_FORCE_STDERR_LOGGING=1 \
QT_LOGGING_RULES="matrix.rust=true" \
    "$REPO/scripts/run-dev.sh" --log-file "$LOG"

echo
judge "$LOG"
