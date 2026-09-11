#!/usr/bin/env bash
# Lightning GUI suite — calls, screen share and per-participant volume.
#
# WHY THIS EXISTS. These four claims had been made from unit tests and from
# reading code, and one of them was WRONG on the user's machine for weeks
# ("set another person's volume, restart the client, the value is gone").
# Two more — "0% mutes" and "above 100% is louder" — are about what reaches a
# GStreamer `volume` element, which no offscreen suite can observe. So they
# are driven here, through the real UI, on a real pair of clients in a real
# call, and every assertion below is on evidence the LAYOUT CANNOT FAKE: a
# log line the engine writes, or a value on disk.
#
# WHAT IT PROVES AND WHAT IT DOES NOT. It proves the control reaches the
# audio graph — `participant volume applied: ... elements=` is written only
# after `g_object_set(element, "volume", …)` succeeded on a real element. It
# does NOT prove audibility; nobody is listening. Report that half as
# NOT TESTED and do not round it up.
#
# USE (on the GUI host, not over a bare ssh exec — see the env block):
#     scripts/gui-suite-calls.sh                 # every check
#     scripts/gui-suite-calls.sh volume share    # just those
#
# PREREQUISITES
#   * TWO Lightning instances already running and already IN A CALL WITH EACH
#     OTHER, on isolated throwaway XDG profiles whose paths contain
#     `profile-A` / `profile-B`. The suite refuses to touch a process whose
#     command line does not name one — the maintainer's own account, store
#     and crypto are off limits, and a mis-aimed ydotool click is how that
#     rule gets broken by accident.
#   * scripts/gui-harness.sh's prerequisites: ydotoold against
#     $YDOTOOL_SOCKET, KWin scripting over qdbus, ImageMagick, spectacle.
#   * A Wayland/KDE session, with DBUS_SESSION_BUS_ADDRESS and
#     WAYLAND_DISPLAY exported. Over ssh:
#         export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
#         export WAYLAND_DISPLAY=wayland-0
#
# COORDINATES ARE CALIBRATED, NOT DISCOVERED. Every click below is relative
# to a window PINNED to 1707x1000 logical (this host is scale 1.5, so that is
# 2560x1500 native), and the suite FAILS rather than clicking blind if KWin
# refuses that geometry — it has refused before. If the call UI is relaid
# out, re-derive the constants with the recipe in RECALIBRATE below; do not
# guess them, and do not weaken an assertion to make a stale one pass.
#
# RECALIBRATE
#   1. Pin the window:  setgeom_pid $A 0 0 1707 1000
#   2. shot_pid $A /tmp/a.png   (spectacle is NATIVE, KWin is LOGICAL)
#   3. Read the control's native centre off the capture and multiply by
#      1707/2560 = 0.6668 to get the logical offset used here.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/gui-harness.sh
source "$HERE/gui-harness.sh"

LT_HOME="${LT_HOME:-$HOME/lt-gui}"
LT_A_LOG="${LT_A_LOG:-$LT_HOME/S.log}"
LT_B_LOG="${LT_B_LOG:-$LT_HOME/B.log}"
LT_A_CONF="${LT_A_CONF:-$LT_HOME/profile-A/config/MatrixClient/matrix-client.conf}"
LT_A_UNIT="${LT_A_UNIT:-lightning-s.service}"
LT_OUT="${LT_OUT:-$LT_HOME/suite-out}"

# --- calibrated control offsets, window pinned to 1707x1000 logical --------
WIN_W=1707 WIN_H=1000
CALL_JOIN_X=1509  CALL_JOIN_Y=57     # room header's call button
SHARE_BTN_X=1123  SHARE_BTN_Y=135    # screen-share button in the call bar
PEOPLE_BTN_X=1063 PEOPLE_BTN_Y=201   # call bar's participants button
VOL_Y=300                            # the volume slider's row in the popup
VOL_MIN_X=1073                       # slider groove, left end  -> 0%
VOL_MAX_X=1303                       # slider groove, right end -> 200%
# The portal's source tiles are placed as FRACTIONS of its own dialog, so a
# different dialog size still hits the first source. Measured 2026-09-11:
# a 738x766 dialog put "Laptop screen" at rel 189,233.
PORTAL_TILE_FX=0.256 PORTAL_TILE_FY=0.304

PASS=0 FAIL=0
declare -a RESULTS=()

ok()   { PASS=$((PASS+1)); RESULTS+=("PASS  $1"); echo "PASS  $1"; }
bad()  { FAIL=$((FAIL+1)); RESULTS+=("FAIL  $1 — $2"); echo "FAIL  $1 — $2" >&2; }
note() { echo "      $*"; }

# lines_since <log> <mark> <regex> — the log's new lines matching <regex>
lines_since() { tail -n +"$2" "$1" 2>/dev/null | grep -E "$3"; }
mark_of()     { wc -l < "$1" 2>/dev/null || echo 0; }

# counter_of <log> <direction> <video> — the newest `frames in the clear`
# count for one lane, or empty when that lane has never carried a frame.
counter_of() {
    grep -E "frames in the clear $2 video= $3 count= [0-9]+" "$1" 2>/dev/null \
        | tail -1 | grep -oE '[0-9]+$'
}

# pid_for <profile-A|profile-B> — and it must be an ISOLATED profile.
pid_for() {
    local want="$1" p
    for p in $(pgrep -x AppRun.wrapped; pgrep -x lightning-matri); do
        grep -qa "$want" "/proc/$p/cmdline" 2>/dev/null && { echo "$p"; return 0; }
        grep -qa "$want" "/proc/$p/environ"  2>/dev/null && { echo "$p"; return 0; }
    done
    return 1
}

shot() {  # shot <pid> <name>
    mkdir -p "$LT_OUT"
    shot_pid "$1" "$LT_OUT/$2.png" >/dev/null 2>&1 \
        && note "capture: $LT_OUT/$2.png"
}

# ---------------------------------------------------------------- preflight
A="" B=""
check_preflight() {
    local why=""
    command -v ydotool >/dev/null 2>&1 || why="no ydotool on PATH"
    pgrep -x ydotoold >/dev/null 2>&1   || why="${why:-ydotoold is not running}"
    command -v magick  >/dev/null 2>&1 || why="${why:-no ImageMagick}"
    [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]] || why="${why:-no DBUS_SESSION_BUS_ADDRESS}"
    A=$(pid_for profile-A) || why="${why:-no Lightning on profile-A}"
    B=$(pid_for profile-B) || why="${why:-no Lightning on profile-B}"
    [[ -r "$LT_A_LOG" && -r "$LT_B_LOG" ]] || why="${why:-logs unreadable: $LT_A_LOG $LT_B_LOG}"
    if [[ -n "$why" ]]; then bad preflight "$why"; return 1; fi
    note "A=$A  B=$B"
    ok preflight
}

# KWIN OVERRIDES GEOMETRY, and it has done it here before (a window asked for
# 10,60 840x980 came back 0,0 853x1021), which invalidates every relative
# coordinate below. So this is an assertion, not a request.
pin_geometry() {   # silent; the caller decides whether it is a result
    setgeom_pid "$A" 0 0 "$WIN_W" "$WIN_H" >/dev/null 2>&1
    sleep 1.5
    local g w h; g=$(geom_pid "$A"); read -r _ _ w h <<<"$g"
    w=${w%%.*}; h=${h%%.*}
    [[ "$w" == "$WIN_W" && "$h" == "$WIN_H" ]] && return 0
    GEOM_WAS="${w}x${h}"
    return 1
}

GEOM_WAS=""
check_geometry() {
    if pin_geometry; then ok geometry; return 0; fi
    bad geometry "KWin gave ${GEOM_WAS}, not ${WIN_W}x${WIN_H}; every offset below is calibrated for ${WIN_W}x${WIN_H}"
    return 1
}

# ------------------------------------------------------------------- checks
# A CALL IS FRAMES MOVING, not a button that lit up. Both directions on both
# clients, sampled twice, because a stalled counter reads exactly like a
# healthy one in a single sample.
check_call() {
    local a_out1 a_in1 b_out1 b_in1 a_out2 a_in2 b_out2 b_in2
    a_out1=$(counter_of "$LT_A_LOG" out false); a_in1=$(counter_of "$LT_A_LOG" in false)
    b_out1=$(counter_of "$LT_B_LOG" out false); b_in1=$(counter_of "$LT_B_LOG" in false)
    if [[ -z "$a_out1$a_in1$b_out1$b_in1" ]]; then
        bad call "no clear-frame counters in either log — are they in a call?"
        return 1
    fi
    sleep 8
    a_out2=$(counter_of "$LT_A_LOG" out false); a_in2=$(counter_of "$LT_A_LOG" in false)
    b_out2=$(counter_of "$LT_B_LOG" out false); b_in2=$(counter_of "$LT_B_LOG" in false)
    note "A out $a_out1->$a_out2  in $a_in1->$a_in2 | B out $b_out1->$b_out2  in $b_in1->$b_in2"
    local stuck=""
    (( ${a_out2:-0} > ${a_out1:-0} )) || stuck+="A-send "
    (( ${a_in2:-0}  > ${a_in1:-0}  )) || stuck+="A-recv "
    (( ${b_out2:-0} > ${b_out1:-0} )) || stuck+="B-send "
    (( ${b_in2:-0}  > ${b_in1:-0}  )) || stuck+="B-recv "
    [[ -z "$stuck" ]] || { bad call "these lanes did not advance: $stuck"; return 1; }
    ok call
}

stored_volume() {  # the newest per-participant volume on A's disk
    grep -oE 'callVolumes\\[a-f0-9]+=[0-9]+' "$LT_A_CONF" 2>/dev/null \
        | tail -1 | grep -oE '[0-9]+$'
}

# open_volume_popup — the call bar's participants button, then the row.
# pidclick_nf inside it: activating a window that owns an open popup closes
# the popup, and pidclick activates.
open_volume_popup() {
    pidclick "$A" "$PEOPLE_BTN_X" "$PEOPLE_BTN_Y" >/dev/null 2>&1
    sleep 2
}

# drag_volume <from-x> <to-x> — a plain click does NOT move this slider
# (measured: the groove ignores it); the handle has to be dragged.
drag_volume() {
    pdrag "$A" "$1" "$VOL_Y" "$2" "$VOL_Y" >/dev/null 2>&1
    sleep 3
}

_volume_case() {  # _volume_case <name> <from> <to> <expected-percent>
    local name="$1" from="$2" to="$3" want="$4" mark applied stored
    mark=$(mark_of "$LT_A_LOG")
    open_volume_popup
    drag_volume "$from" "$to"
    applied=$(lines_since "$LT_A_LOG" "$mark" 'participant volume applied' | tail -1)
    stored=$(stored_volume)
    note "applied: ${applied:-<none>}"
    note "stored:  ${stored:-<none>}"
    shot "$A" "volume-$want"
    if [[ -z "$applied" ]]; then
        # THE ABSENCE OF THE WARNING IS NOT THE PROOF. Say which it was.
        if lines_since "$LT_A_LOG" "$mark" 'nowhere to land' >/dev/null; then
            bad "$name" "the value reached the engine and landed on NO element"
        else
            bad "$name" "the engine never applied a volume — the click missed, or the control is cosmetic"
        fi
        return 1
    fi
    grep -q "percent= $want" <<<"$applied" \
        || { bad "$name" "the engine applied a different percentage: $applied"; return 1; }
    [[ "$stored" == "$want" ]] \
        || { bad "$name" "on disk it reads '${stored:-<none>}', not $want"; return 1; }
    ok "$name"
}

check_volume_mute()    { _volume_case volume-0%-mutes    "$VOL_MAX_X" "$VOL_MIN_X" 0; }
check_volume_boost()   { _volume_case volume-200%-boosts "$VOL_MIN_X" "$VOL_MAX_X" 200; }

# PERSISTENCE IS NOT "THE NUMBER IS STILL IN THE FILE". The reported defect
# was a value that survived to settings and never reached the audio again, so
# the assertion is that the RESTARTED client applies it to a real element
# with nobody touching the slider.
check_volume_persists() {
    local before after mark
    before=$(stored_volume)
    [[ -n "$before" ]] || { bad volume-persists "nothing stored to survive; run the volume checks first"; return 1; }
    systemctl --user restart "$LT_A_UNIT" >/dev/null 2>&1 \
        || { bad volume-persists "could not restart $LT_A_UNIT"; return 1; }
    sleep 60
    A=$(pid_for profile-A) || { bad volume-persists "profile-A did not come back"; return 1; }
    after=$(stored_volume)
    [[ "$after" == "$before" ]] \
        || { bad volume-persists "was $before, is now ${after:-<none>}"; return 1; }
    pin_geometry \
        || { bad volume-persists "after the restart KWin gave ${GEOM_WAS}; the join click would be blind"; return 1; }
    mark=$(mark_of "$LT_A_LOG")
    pidclick "$A" "$CALL_JOIN_X" "$CALL_JOIN_Y" >/dev/null 2>&1
    sleep 25
    shot "$A" "volume-persists"
    local applied
    applied=$(lines_since "$LT_A_LOG" "$mark" 'participant volume applied' | tail -1)
    [[ -n "$applied" ]] \
        || { bad volume-persists "the restarted client never applied the stored volume to an element"; return 1; }
    grep -q "percent= $before" <<<"$applied" \
        || { bad volume-persists "it re-applied something else: $applied"; return 1; }
    note "re-applied with no user input: $applied"
    ok volume-persists
}

# The portal picker is a WINDOW of xdg-desktop-portal-kde, not a popup, so it
# is addressed by its own pid. Lightning gives it 120 s (kRequestTimeoutMs)
# and then releases — answer it in one pass, never across two ssh round trips.
check_share() {
    local marka markb portal g px py pw ph
    marka=$(mark_of "$LT_A_LOG"); markb=$(mark_of "$LT_B_LOG")
    pidclick "$A" "$SHARE_BTN_X" "$SHARE_BTN_Y" >/dev/null 2>&1
    sleep 4
    portal=$(pgrep -f xdg-desktop-portal-kde | head -1)
    g=$(geom_pid "${portal:-0}")
    [[ -n "$g" ]] || { bad share "no portal picker appeared"; return 1; }
    read -r px py pw ph <<<"$g"
    local rx ry
    rx=$(awk -v w="${pw%%.*}" -v f="$PORTAL_TILE_FX" 'BEGIN{printf "%d", w*f}')
    ry=$(awk -v h="${ph%%.*}" -v f="$PORTAL_TILE_FY" 'BEGIN{printf "%d", h*f}')
    pidclick_nf "$portal" "$rx" "$ry" >/dev/null 2>&1 \
        || { bad share "could not click the first source tile"; return 1; }
    sleep 1
    ydotool key 28:1 28:0     # Enter — this dialog has no button row
    sleep 12
    local publishing encoded received
    publishing=$(lines_since "$LT_A_LOG" "$marka" 'screen share publishing' | tail -1)
    encoded=$(lines_since "$LT_A_LOG" "$marka" 'first encoded frame screenShare= true' | tail -1)
    received=$(lines_since "$LT_B_LOG" "$markb" 'frames in the clear in video= true' | tail -1)
    note "A: ${publishing:-<not publishing>}"
    note "A: ${encoded:-<no encoded frame>}"
    note "B: ${received:-<nothing received>}"
    shot "$B" "share-received"
    local why=""
    [[ -n "$publishing" ]] || why="A never published"
    [[ -n "$encoded"    ]] || why="${why:-A published but encoded no frame}"
    [[ -n "$received"   ]] || why="${why:-B received no video frames}"
    [[ -z "$why" ]] || { bad share "$why"; return 1; }
    ok share
}

# ---------------------------------------------------------------------- run
main() {
    local want=("$@")
    (( ${#want[@]} )) || want=(call volume share)
    check_preflight || { echo; echo "0 passed, 1 failed"; return 1; }
    check_geometry  || { echo; echo "0 passed, 1 failed"; return 1; }
    local w
    for w in "${want[@]}"; do
        case "$w" in
            call)   check_call ;;
            volume) check_volume_mute; check_volume_boost; check_volume_persists ;;
            share)  check_share ;;
            *) bad "$w" "no such check (call|volume|share)" ;;
        esac
    done
    echo
    printf '%s\n' "${RESULTS[@]}"
    echo "$PASS passed, $FAIL failed"
    (( FAIL == 0 ))
}

main "$@"
