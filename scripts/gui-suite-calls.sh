#!/usr/bin/env bash
# Lightning GUI suite — calls, screen share and per-participant volume.
#
# Drives the real UI on two clients in a live call and asserts only on
# evidence the layout cannot fake: engine log lines or values on disk. It
# proves a control reaches the audio graph (`participant volume applied: ...
# elements=` is logged only after g_object_set succeeded on a real element);
# it does not prove audibility, so report that as NOT TESTED.
#
# Usage (on the GUI host, not over a bare ssh exec; see the env block):
#     scripts/gui-suite-calls.sh                    # every check
#     scripts/gui-suite-calls.sh volume micgain     # just those
#
# Prerequisites
#   * Two Lightning instances already in a call with each other, on isolated
#     throwaway XDG profiles whose paths contain `profile-A` / `profile-B`.
#     The suite refuses any process whose command line names neither.
#   * scripts/gui-harness.sh's prerequisites: ydotoold against
#     $YDOTOOL_SOCKET, KWin scripting over qdbus, ImageMagick, spectacle.
#   * A Wayland/KDE session, with DBUS_SESSION_BUS_ADDRESS and
#     WAYLAND_DISPLAY exported. Over ssh:
#         export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
#         export WAYLAND_DISPLAY=wayland-0
#
# Coordinates are calibrated, not discovered: every click is relative to a
# window pinned to 1707x1000 logical (2560x1500 native at scale 1.5), and the
# suite fails rather than clicking blind if KWin refuses that geometry. After
# a call UI relayout, re-derive the constants per RECALIBRATE; do not guess.
#
# RECALIBRATE
#   1. Pin the window:  setgeom_pid $A 0 0 1707 1000
#   2. shot_pid $A /tmp/a.png   (spectacle is native, KWin is logical)
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
ROOM_ROW_X=165    ROOM_ROW_Y=361     # the fixture room in the room list
SHARE_BTN_X=1123  SHARE_BTN_Y=135    # screen-share button in the call bar
PEOPLE_BTN_X=1063 PEOPLE_BTN_Y=201   # call bar's participants button
VOL_Y=300                            # the volume slider's row in the popup
VOL_MIN_X=1073                       # slider groove, left end  -> 0%
VOL_MAX_X=1303                       # slider groove, right end -> 200%
# Microphone chevron and the level slider inside its menu, measured per
# RECALIBRATE (native 2193,205 and groove 2234..2487 at y 423). These are the
# most likely to go stale after a call-bar change; the check's failure text
# says so, since a missed click and a dead control look identical.
MIC_CHEVRON_X=1463 MIC_CHEVRON_Y=137  # the chevron BESIDE the mic button
MICGAIN_Y=282                         # the level slider's row inside the menu
MICGAIN_MIN_X=1490                    # groove, left end  -> 0%
MICGAIN_MAX_X=1659                    # groove, right end -> 200%
# Source tiles are placed as fractions of the portal dialog so a different
# dialog size still hits the first source (738x766 put "Laptop screen" at
# rel 189,233).
PORTAL_TILE_FX=0.256 PORTAL_TILE_FY=0.304

PASS=0 FAIL=0
HAVE_MAGICK=""
declare -a RESULTS=()

ok()   { PASS=$((PASS+1)); RESULTS+=("PASS  $1"); echo "PASS  $1"; }
bad()  { FAIL=$((FAIL+1)); RESULTS+=("FAIL  $1 — $2"); echo "FAIL  $1 — $2" >&2; }
note() { echo "      $*"; }

# lines_since <log> <mark> <regex> — new lines matching <regex> strictly after
# the mark (tail -n +N starts at N; without +1 a rerun could pass on the
# previous run's evidence).
lines_since() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null | grep -E "$3"; }
mark_of()     { wc -l < "$1" 2>/dev/null || echo 0; }

# counter_of <log> <direction> <video> [stream] — the newest
# `frames in the clear` count for one lane, or empty when that lane has never
# carried a frame. Pass `stream` to pin it to ONE track.
#
# The pattern must tolerate `stream=`, or every counter reads empty and
# two-sample comparisons pass by finding nothing. A lane is not a track: with
# share audio a participant has two `video= false` tracks.
counter_of() {
    local stream="${4:-}"
    grep -E "frames in the clear $2 stream= \"?${stream:-[^ ]*}\"? video= $3 count= [0-9]+" \
        "$1" 2>/dev/null | tail -1 | grep -oE '[0-9]+$'
}

# pid_for <profile-A|profile-B> — refuses anything but an isolated profile.
pid_for() {
    local want="$1" p
    for p in $(pgrep -x AppRun.wrapped; pgrep -x lightning-matri); do
        grep -qa "$want" "/proc/$p/cmdline" 2>/dev/null && { echo "$p"; return 0; }
        grep -qa "$want" "/proc/$p/environ"  2>/dev/null && { echo "$p"; return 0; }
    done
    return 1
}

# Captures are for the operator, never assertions. Without ImageMagick to crop
# to the window, shot_pid falls back to the whole screen.
shot() {  # shot <pid> <name>
    mkdir -p "$LT_OUT"
    if [[ -n "$HAVE_MAGICK" ]] \
       && shot_pid "$1" "$LT_OUT/$2.png" >/dev/null 2>&1; then
        note "capture: $LT_OUT/$2.png"
        return 0
    fi
    spectacle -b -n -f -o "$LT_OUT/$2.png" >/dev/null 2>&1
    sleep 0.6
    [[ -s "$LT_OUT/$2.png" ]] \
        && note "capture (full screen, uncropped): $LT_OUT/$2.png"
}

# ---------------------------------------------------------------- preflight
A="" B=""
check_preflight() {
    local why=""
    command -v ydotool >/dev/null 2>&1 || why="no ydotool on PATH"
    pgrep -x ydotoold >/dev/null 2>&1   || why="${why:-ydotoold is not running}"
    command -v spectacle >/dev/null 2>&1 || why="${why:-no spectacle}"
    [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]] || why="${why:-no DBUS_SESSION_BUS_ADDRESS}"
    # Optional, not a precondition; see shot().
    HAVE_MAGICK=""
    command -v magick >/dev/null 2>&1 && HAVE_MAGICK=1
    [[ -n "$HAVE_MAGICK" ]] \
        || note "no ImageMagick: captures will be full-screen, not cropped"
    A=$(pid_for profile-A) || why="${why:-no Lightning on profile-A}"
    B=$(pid_for profile-B) || why="${why:-no Lightning on profile-B}"
    [[ -r "$LT_A_LOG" && -r "$LT_B_LOG" ]] || why="${why:-logs unreadable: $LT_A_LOG $LT_B_LOG}"
    if [[ -n "$why" ]]; then bad preflight "$why"; return 1; fi
    note "A=$A  B=$B"
    ok preflight
}

# KWin can override requested geometry, which would invalidate every relative
# coordinate, so this asserts rather than requests.
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
# A call is frames moving: both directions on both clients, sampled twice,
# because a stalled counter looks healthy in a single sample.
# stream_ids_in_lane <log> <direction> <video> — the DISTINCT stream ids that
# have reported a clear-frame count in that lane, sorted.
stream_ids_in_lane() {
    grep -oE "frames in the clear $2 stream= \"?[^ \"]+" "$1" 2>/dev/null \
        | awk '{print $NF}' | tr -d '"' | sort -u
}
# ...and how many there are.
streams_in_lane() { stream_ids_in_lane "$@" | wc -l; }

check_call() {
    local a_out1 a_in1 b_out1 b_in1 a_out2 a_in2 b_out2 b_in2
    # counter_of takes the newest line and each track counts from zero, so
    # samples are only comparable when the lane is a single track (share
    # audio adds a second `video= false` one). Refuse otherwise.
    local lane
    for lane in "$LT_A_LOG out" "$LT_A_LOG in" "$LT_B_LOG out" "$LT_B_LOG in"; do
        # shellcheck disable=SC2086
        set -- $lane
        if (( $(streams_in_lane "$1" "$2" false) > 1 )); then
            bad call "the $2 audio lane of $(basename "$1") carries more than one stream (share audio?), so two samples of it are not comparable — restart the clients or check this lane by stream id"
            return 1
        fi
    done
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

# The per-participant volume on A's disk. QSettings' INI backend joins
# sub-keys with a backslash, so the key reads `<slug>\callVolumes\<hex>`.
#
# Require exactly one match, never `tail -1`: a stale entry for an earlier
# participant sorts arbitrarily. More than one is a fixture problem.
stored_volume() {
    local all n
    all=$(grep -oE 'callVolumes\\[a-f0-9]+=[0-9]+' "$LT_A_CONF" 2>/dev/null \
          | grep -oE '[0-9]+$')
    n=$(printf '%s\n' "$all" | grep -c '[0-9]' )
    if [[ "$n" != "1" ]]; then
        # Report on stderr; callers read this through $( ), a subshell.
        echo "      stored_volume: expected exactly one stored participant volume, found $n" >&2
        return 1
    fi
    printf '%s\n' "$all"
}

# open_volume_popup — the call bar's participants button. The volume control
# is a Popup inside the same window, so pidclick's activation is safe here.
open_volume_popup() {
    pidclick "$A" "$PEOPLE_BTN_X" "$PEOPLE_BTN_Y" >/dev/null 2>&1
    sleep 2
}

# drag_volume <from-x> <to-x> — a click on the groove does not move this
# slider; the handle has to be dragged.
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
        # Absence of the warning is not proof; say which it was.
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

# Persistence means the restarted client applies the stored value to a real
# element without the slider being touched, not merely that it is on disk.
check_volume_persists() {
    local before after mark
    before=$(stored_volume)
    [[ -n "$before" ]] || { bad volume-persists "nothing stored to survive; run the volume checks first"; return 1; }
    # The one action that is not scoped to a proven pid: `systemctl --user
    # restart` acts on a unit name. Require the unit's cgroup (not MainPID,
    # which is AppRun for an AppImage) to contain the pid being driven.
    if ! grep -q "$LT_A_UNIT" "/proc/$A/cgroup" 2>/dev/null; then
        bad volume-persists "pid $A is not in $LT_A_UNIT's cgroup — refusing to restart a unit this suite has not proven it owns"
        return 1
    fi
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
    # A restarted client comes up on Home, so open the room before clicking
    # the call button.
    pidclick "$A" "$ROOM_ROW_X" "$ROOM_ROW_Y" >/dev/null 2>&1
    sleep 6
    pidclick "$A" "$CALL_JOIN_X" "$CALL_JOIN_Y" >/dev/null 2>&1
    sleep 25
    shot "$A" "volume-persists"
    # Distinguish "never joined the call" from a volume defect.
    if ! lines_since "$LT_A_LOG" "$mark" 'sfu joined' >/dev/null; then
        bad volume-persists "the restarted client never rejoined the call, so this says nothing about the stored volume — the value DID survive on disk as $before"
        return 1
    fi
    local applied
    applied=$(lines_since "$LT_A_LOG" "$mark" 'participant volume applied' | tail -1)
    [[ -n "$applied" ]] \
        || { bad volume-persists "the restarted client rejoined the call but never applied the stored volume to an element"; return 1; }
    grep -q "percent= $before" <<<"$applied" \
        || { bad volume-persists "it re-applied something else: $applied"; return 1; }
    note "re-applied with no user input: $applied"
    ok volume-persists
}

# Microphone level reaching the audio graph. Neither the readout (derived
# from the slider) nor the stored `microphoneGain` is evidence; only
# `microphone gain applied: … elements=N` with N>0, or its warning when the
# pipeline had nowhere to put it.
#
# Driven through the in-call menu, so it also covers dragging a Slider inside
# a QQuickMenu. No `microphone gain` line of either kind means the click or
# drag never reached the control; the warning means the engine had no target.
_micgain_case() {   # _micgain_case <name> <from-x> <to-x> <expected-percent>
    local name="$1" from="$2" to="$3" want="$4" mark applied nowhere
    mark=$(mark_of "$LT_A_LOG")
    pidclick "$A" "$MIC_CHEVRON_X" "$MIC_CHEVRON_Y" >/dev/null 2>&1
    sleep 2
    pdrag "$A" "$from" "$MICGAIN_Y" "$to" "$MICGAIN_Y" >/dev/null 2>&1
    sleep 3
    applied=$(lines_since "$LT_A_LOG" "$mark" 'microphone gain applied' | tail -1)
    nowhere=$(lines_since "$LT_A_LOG" "$mark" 'microphone gain had nowhere' | tail -1)
    note "applied: ${applied:-<none>}"
    shot "$A" "micgain-$want"
    if [[ -z "$applied" ]]; then
        if [[ -n "$nowhere" ]]; then
            bad "$name" "the drag reached the engine and it had no micvol element to set: $nowhere"
        else
            bad "$name" "no 'microphone gain' line of either kind — the chevron click or the in-menu drag never reached the slider, which says nothing yet about the engine. Re-derive MIC_CHEVRON_* / MICGAIN_* per RECALIBRATE, and check the menu did not steal the drag"
        fi
        return 1
    fi
    grep -q "percent= $want" <<<"$applied" \
        || { bad "$name" "the engine applied a different percentage: $applied"; return 1; }
    grep -qE 'elements= [1-9]' <<<"$applied" \
        || { bad "$name" "the engine reported ZERO elements, so nothing was set: $applied"; return 1; }
    ok "$name"
}

check_micgain_mute()  { _micgain_case micgain-0%-reaches-the-graph "$MICGAIN_MAX_X" "$MICGAIN_MIN_X" 0; }
check_micgain_boost() { _micgain_case micgain-200%-reaches-the-graph "$MICGAIN_MIN_X" "$MICGAIN_MAX_X" 200; }

# The portal picker is a window of xdg-desktop-portal-kde, addressed by its
# own pid. Lightning releases it after 120 s (kRequestTimeoutMs), so answer it
# in one pass.
check_share() {
    local marka markb portal g px py pw ph
    marka=$(mark_of "$LT_A_LOG"); markb=$(mark_of "$LT_B_LOG")
    # Record B's existing inbound video streams so the check cannot pass on
    # an already-running camera.
    local before_streams
    before_streams=$(stream_ids_in_lane "$LT_B_LOG" in true)
    pidclick "$A" "$SHARE_BTN_X" "$SHARE_BTN_Y" >/dev/null 2>&1
    sleep 4
    portal=$(pgrep -f xdg-desktop-portal-kde | head -1)
    g=$(geom_pid "${portal:-0}")
    [[ -n "$g" ]] || { bad share "no portal picker appeared"; return 1; }
    read -r px py pw ph <<<"$g"
    local rx ry
    rx=$(awk -v w="${pw%%.*}" -v f="$PORTAL_TILE_FX" 'BEGIN{printf "%d", w*f}')
    ry=$(awk -v h="${ph%%.*}" -v f="$PORTAL_TILE_FY" 'BEGIN{printf "%d", h*f}')
    # pidclick_nf/keypid end in `sleep`, so `|| bad ...` on them never fires;
    # guards must be explicit.
    pidclick_nf "$portal" "$rx" "$ry" >/dev/null 2>&1
    sleep 1
    # Selecting the source tile usually confirms and closes the dialog; send
    # Enter only if the picker is still open.
    if [[ -n "$(geom_pid "$portal")" ]]; then
        guard_pid "$portal" \
            || { bad share "the picker is still open but not active; Enter would have gone elsewhere"; return 1; }
        keypid "$portal" 28:1 28:0
    else
        note "the source click confirmed on its own; no Enter needed"
    fi
    sleep 12
    local publishing encoded received
    publishing=$(lines_since "$LT_A_LOG" "$marka" 'screen share publishing' | tail -1)
    encoded=$(lines_since "$LT_A_LOG" "$marka" 'first encoded frame screenShare= true' | tail -1)
    # A stream B was not already receiving: the share.
    local new_stream
    new_stream=$(comm -13 <(printf '%s\n' "$before_streams") \
                          <(stream_ids_in_lane "$LT_B_LOG" in true) | head -1)
    if [[ -n "$new_stream" ]]; then
        received=$(grep -F "stream= \"$new_stream\"" "$LT_B_LOG" 2>/dev/null \
                   | grep -E 'frames in the clear in .* video= true' | tail -1)
    else
        received=""
    fi
    note "A: ${publishing:-<not publishing>}"
    note "A: ${encoded:-<no encoded frame>}"
    note "B: ${received:-<no NEW inbound video stream appeared>}"
    shot "$B" "share-received"
    local why=""
    [[ -n "$publishing" ]] || why="A never published"
    [[ -n "$encoded"    ]] || why="${why:-A published but encoded no frame}"
    [[ -n "$received"   ]] || why="${why:-B received no video frames on a stream it was not already receiving; any video already flowing is NOT evidence of this share}"
    [[ -z "$why" ]] || { bad share "$why"; return 1; }
    # What this check does not prove:
    #  * That B rendered a picture: frames were decrypted and handed on. Qt
    #    Quick's software backend has no video node and the counter still
    #    climbs. Use `shot` for rendering.
    #  * That the new stream is the share: a camera enabled in the same
    #    window would also satisfy it.
    #  * That the picture is correct (aspect, crop, staleness).
    note "PASS means B decrypted inbound video frames; it does NOT assert a picture was drawn"
    ok share
}

# ---------------------------------------------------------------------- run
main() {
    local want=("$@")
    (( ${#want[@]} )) || want=(call volume micgain share)
    check_preflight || { echo; echo "0 passed, 1 failed"; return 1; }
    check_geometry  || { echo; echo "0 passed, 1 failed"; return 1; }
    local w
    for w in "${want[@]}"; do
        case "$w" in
            call)   check_call ;;
            volume) check_volume_mute; check_volume_boost; check_volume_persists ;;
            micgain) check_micgain_mute; check_micgain_boost ;;
            share)  check_share ;;
            *) bad "$w" "no such check (call|volume|micgain|share)" ;;
        esac
    done
    echo
    printf '%s\n' "${RESULTS[@]}"
    echo "$PASS passed, $FAIL failed"
    (( FAIL == 0 ))
}

main "$@"
