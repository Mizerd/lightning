#!/usr/bin/env bash
# Regenerate the bundled Material Symbols Rounded subset (Apache-2.0).
#
# Downloads the upstream variable font + codepoints, subsets it to exactly
# the icon names listed below (the set qml/Icon.qml maps), and instances it
# to the design defaults (wght 400, FILL 0, GRAD 0, opsz 24). Adding an
# icon = add its name here AND to the map in qml/Icon.qml, then rerun.
# Requires fonttools (e.g. nix shell nixpkgs#python3Packages.fonttools).
set -euo pipefail
cd "$(dirname "$0")/.."
ICONS="home search add add_circle add_reaction settings group groups person person_add \
person_remove block undo \
forum chat_bubble info call videocam push_pin notifications notifications_off \
mood mic send close check check_circle done_all lock lock_open verified_user \
palette devices science expand_more expand_less alternate_email edit_square \
unfold_more play_arrow arrow_back arrow_forward more_horiz more_vert star \
refresh download attach_file image description logout delete visibility \
visibility_off content_copy reply gif_box keyboard_return workspaces tag \
shield key warning error schedule format_bold format_italic strikethrough_s \
code link format_list_bulleted format_quote \
pause stop volume_up volume_off open_in_full close_fullscreen graphic_eq \
speed zoom_in zoom_out fit_screen chevron_left chevron_right \
account_circle radio_button_checked radio_button_unchecked bolt keyboard_tab \
group_add person_search pets restaurant sports_esports flight lightbulb \
emoji_symbols flag explore travel_explore \
mic_off call_end videocam_off screen_share stop_screen_share front_hand \
headset_mic headset_off grid_view"

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
base="https://github.com/google/material-design-icons/raw/master/variablefont"
curl -sfL -o "$work/msr.ttf" \
    "$base/MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf"
curl -sfL -o "$work/msr.codepoints" \
    "$base/MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D.codepoints"

unicodes=$(python3 - "$work/msr.codepoints" $ICONS <<'EOF'
import sys
cps = dict(line.split() for line in open(sys.argv[1]))
names = sys.argv[2:]
missing = [n for n in names if n not in cps]
assert not missing, f"unknown icon names: {missing}"
print(",".join(f"U+{cps[n]}" for n in names))
EOF
)
pyftsubset "$work/msr.ttf" --unicodes="$unicodes" \
    --output-file="$work/sub.ttf"
fonttools varLib.instancer "$work/sub.ttf" \
    wght=400 FILL=0 GRAD=0 opsz=24 \
    -o data/fonts/MaterialSymbolsRounded-subset.ttf
ls -la data/fonts/MaterialSymbolsRounded-subset.ttf
