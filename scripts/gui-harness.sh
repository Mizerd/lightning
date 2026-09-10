#!/usr/bin/env bash
# Lightning GUI harness — KWin scripting + ydotool on KDE/Wayland.
#
# COMMITTED SO IT STOPS BEING REBUILT. This lived in a session scratchpad and
# was reconstructed from scratch on 2026-09-10 after the previous copy went
# with the scratchpad; the two calibration facts below cost most of that time.
# It is deliberately host-shaped (KDE/Wayland, KWin scripting over qdbus,
# ydotool through a uinput device) and makes no attempt to be portable.
#
# USE:  source scripts/gui-harness.sh
#       PID=$(pgrep -x lightning-matri | head -1)   # comm is TRUNCATED at 15
#       pidclick "$PID" 200 223 ; shot_pid "$PID" /tmp/a.png
#
# PREREQUISITES: ydotoold running against $YDOTOOL_SOCKET with write access to
# /dev/uinput (an ACL entry is enough, no group needed), and ImageMagick for
# shot_pid's crop.
#
# SCROLLING IS scripts/gui-wheel.py, NOT ydotool. ydotool 1.0.4 has no wheel
# command at all — its `click` takes buttons 0x00-0x07 and a wheel is
# REL_WHEEL, an axis. `pdrag` below scrolls a Flickable that is `interactive`,
# but a desktop ScrollView is not, and that made whole panels unreachable
# (the Space Home settings' "Leave Space" among them). gui-wheel.py creates
# its own uinput mouse and emits REL_WHEEL directly:
#     python3 scripts/gui-wheel.py -8 80    # 8 clicks DOWN, 80ms apart
# Position the pointer over the target pane first (moveto), because a wheel
# event goes to whatever is under the cursor.
#
# ALWAYS drive a throwaway fixture account on an ISOLATED XDG profile, and
# confirm it from /proc/<pid>/environ before terminating anything. The
# maintainer's own account, store and crypto are off limits.
#
# TWO TRAPS THIS ENCODES, both of which have cost captures before:
#  1. curpos/activewin MUST be nonce-tagged. journalctl|grep|tail -1 otherwise
#     returns a line from an EARLIER call and every click lands stale.
#  2. KWin's pointer space is LOGICAL (this desktop is scale 1.5); spectacle
#     captures are NATIVE. Convert before clicking off a screenshot.
set -uo pipefail
export YDOTOOL_SOCKET="${YDOTOOL_SOCKET:-$HOME/.ydotool_socket}"
# PINNED, because an ephemeral `nix shell` copy is exactly what a store GC
# collected on 2026-09-10 and the harness came back with no ydotool at all.
export PATH="$HOME/.cache/nix-gcroots/ydotool/bin:$PATH"

kwin_run() {   # run a KWin script, return its journal output for this nonce
    local nonce="K$RANDOM$RANDOM" script="$1"
    local f; f=$(mktemp /tmp/kwin-XXXXXX.js)
    printf '%s\n' "${script//__NONCE__/$nonce}" > "$f"
    local id
    id=$(qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$f" 2>/dev/null \
         | grep -oE '^-?[0-9]+' | head -1)
    [[ -n "$id" ]] || { rm -f "$f"; return 1; }
    qdbus org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null 2>&1
    sleep 0.35
    qdbus org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.stop >/dev/null 2>&1
    rm -f "$f"
    # Strip the journal prefix AND the nonce, so callers get only their payload.
    journalctl --user -n 400 --no-pager 2>/dev/null \
        | grep -F "$nonce" | tail -1 | sed "s/.*$nonce //"
}

geom_pid() {   # geom_pid <pid> -> "x y w h" in LOGICAL coords
    kwin_run "
        var ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
        for (var i = 0; i < ws.length; i++) {
            if (ws[i].pid == $1) {
                var g = ws[i].frameGeometry;
                print('__NONCE__ ' + g.x + ' ' + g.y + ' ' + g.width + ' ' + g.height);
            }
        }"
}

activewin() {  # active window's pid and caption
    kwin_run "
        var a = workspace.activeWindow || workspace.activeClient;
        print('__NONCE__ ' + (a ? a.pid + ' ' + a.caption : 'none'));"
}

focus_pid() {  # raise and activate the window owned by <pid>
    kwin_run "
        var ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
        for (var i = 0; i < ws.length; i++) {
            if (ws[i].pid == $1) {
                workspace.activeWindow = ws[i];
                print('__NONCE__ ok');
            }
        }" >/dev/null
    sleep 0.3
}

# GUARD: refuse to type unless the intended window is actually active. Without
# this a login once went into a browser window.
guard_pid() {
    local want="$1" got
    got=$(activewin | awk '{print $1}')
    [[ "$got" == "$want" ]] || { echo "GUARD: active pid $got != $want" >&2; return 1; }
}

cursorpos() { kwin_run "print('__NONCE__ ' + workspace.cursorPos.x + ',' + workspace.cursorPos.y);"; }

# moveto <x> <y> — CLOSED LOOP, and it has to be.
#
# `ydotool mousemove -a` is NOT usable on this host: measured 2026-09-10, it
# put (400,300) at (2000,0) and then parked every later request at (1,1). Its
# absolute axis range does not correspond to this 5120x1440 logical desktop.
# RELATIVE moves are exact (+100+50 landed at exactly +100+50), so aim by
# delta from where the pointer actually is, then VERIFY and correct. Three
# passes is plenty; the loop exits as soon as it is on target.
moveto() {
    local wx="$1" wy="$2" i cur cx cy
    for i in 1 2 3; do
        cur=$(cursorpos); cx=${cur%,*}; cy=${cur#*,}
        [[ "$cx" =~ ^-?[0-9]+$ && "$cy" =~ ^-?[0-9]+$ ]] || return 1
        (( cx == wx && cy == wy )) && return 0
        ydotool mousemove -x $((wx - cx)) -y $((wy - cy)) 2>/dev/null
        sleep 0.25
    done
    cur=$(cursorpos); cx=${cur%,*}; cy=${cur#*,}
    # Within a pixel is on target; anything further is a real failure and the
    # caller must not click blind.
    (( cx >= wx-1 && cx <= wx+1 && cy >= wy-1 && cy <= wy+1 )) && return 0
    echo "moveto: wanted $wx,$wy but pointer is at $cur" >&2
    return 1
}

pidclick() {   # pidclick <pid> <x> <y>  — coords RELATIVE to the window frame
    local pid="$1" rx="$2" ry="$3"
    focus_pid "$pid" || return 1
    local g; g=$(geom_pid "$pid"); [[ -n "$g" ]] || { echo "no geometry for $pid" >&2; return 1; }
    local gx gy; read -r gx gy _ _ <<<"$g"
    moveto $((gx + rx)) $((gy + ry)) || return 1
    guard_pid "$pid" || return 1
    ydotool click 0xC0
    sleep 0.35
}

typepid() { focus_pid "$1" && guard_pid "$1" && ydotool type --key-delay 12 -- "$2"; sleep 0.2; }
keypid()  { focus_pid "$1" && guard_pid "$1" && ydotool key "${@:2}"; sleep 0.25; }

shot_pid() {   # shot_pid <pid> <out.png> — full screen, cropped to the window
    local pid="$1" out="$2" g gx gy gw gh
    g=$(geom_pid "$pid") || return 1
    read -r gx gy gw gh <<<"$g"
    local full; full=$(mktemp /tmp/shot-XXXXXX.png)
    spectacle -b -n -f -o "$full" >/dev/null 2>&1; sleep 0.6
    # LOGICAL -> NATIVE. Read the real ratio rather than assuming 1.5.
    local sw; sw=$(magick identify -format '%w' "$full")
    local vw; vw=$(kwin_run "print('__NONCE__ ' + workspace.workspaceWidth);")
    local r; r=$(awk -v a="$sw" -v b="${vw:-$sw}" 'BEGIN{printf "%.6f", (b>0? a/b : 1)}')
    magick "$full" -crop "$(awk -v w=$gw -v h=$gh -v x=$gx -v y=$gy -v r=$r \
        'BEGIN{printf "%dx%d+%d+%d", w*r, h*r, x*r, y*r}')" +repage "$out" 2>/dev/null
    rm -f "$full"
    [[ -f "$out" ]] && echo "$out"
}

# Right click, same closed-loop positioning as pidclick.
pidrclick() {
    local pid="$1" rx="$2" ry="$3"
    focus_pid "$pid" || return 1
    local g; g=$(geom_pid "$pid"); [[ -n "$g" ]] || return 1
    local gx gy; read -r gx gy _ _ <<<"$g"
    moveto $((gx + rx)) $((gy + ry)) || return 1
    guard_pid "$pid" || return 1
    ydotool click 0xC1
    sleep 0.45
}
# Convert a coordinate read off a shot_pid capture (NATIVE px) into the
# window-relative LOGICAL coords pidclick wants. scale is native/logical.
img2win() { awk -v x="$1" -v y="$2" -v s="${3:-1.5}" 'BEGIN{printf "%d %d", x/s, y/s}'; }

setgeom_pid() {  # setgeom_pid <pid> <x> <y> <w> <h>   (LOGICAL coords)
    kwin_run "
        var ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
        for (var i = 0; i < ws.length; i++) {
            if (ws[i].pid == $1) {
                ws[i].frameGeometry = { x: $2, y: $3, width: $4, height: $5 };
                print('__NONCE__ ok');
            }
        }" >/dev/null
    sleep 0.6
}

# pdrag <pid> <x1> <y1> <x2> <y2> — press, move in steps, release.
# ydotool 1.0.4 has NO wheel command, so a Flickable is scrolled by dragging.
# The intermediate steps matter: a single jump reads as a click, not a flick.
pdrag() {
    local pid="$1" x1="$2" y1="$3" x2="$4" y2="$5" i n=12
    focus_pid "$pid" || return 1
    local g gx gy; g=$(geom_pid "$pid") || return 1; read -r gx gy _ _ <<<"$g"
    moveto $((gx + x1)) $((gy + y1)) || return 1
    ydotool click 0x40 2>/dev/null            # left button DOWN
    sleep 0.12
    for ((i = 1; i <= n; i++)); do
        moveto $((gx + x1 + (x2 - x1) * i / n)) $((gy + y1 + (y2 - y1) * i / n)) >/dev/null
    done
    ydotool click 0x80 2>/dev/null            # left button UP
    sleep 0.5
}
