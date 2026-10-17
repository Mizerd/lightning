#!/usr/bin/env bash
set -Eeuo pipefail

# assert_share_audio_exclusion (lib.sh), GitHub #26: a package whose screen
# shares would send the call back to everyone in it must not be released. The
# 0.10.1 AppImage shipped without audiomixer, so its "Entire system" share
# sound fell back to the output monitor on every machine. Its real
# --call-media-status transcript (quoted in GitHub #37) is the first fixture:
# it passed every check that existed, and must not pass this one in a release.

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LIB="$HERE/../scripts/lib.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
ok() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

cat >"$WORK/appimage-0.10.1.txt" <<'EOF'
call media engine built in: yes
lightning.calls.gst: gstreamer initialised bundled= true
bundled plugin directory: /tmp/.mount_LightniNNBIL/usr/lib/gstreamer-1.0
plugin scanner: /tmp/.mount_LightniNNBIL/usr/libexec/gstreamer-1.0/gst-plugin-scanner
gstreamer: initialised, GStreamer 1.26.2
1:1 call engine: available
group call (SFU) engine: available
microphone noise suppression: off=available webrtc=available rnnoise=available deepfilternet=available
camera compressed (MJPG) chain: available
X11 screen capture (ximagesrc): available

RESULT: calls can be placed and answered.
EOF
# The same package reporting, as a build with this check reports it.
sed '/^RESULT:/i screen-share sound exclusion capability: unavailable (missing: element audiomixer)' \
    "$WORK/appimage-0.10.1.txt" >"$WORK/includes.txt"
sed '/^RESULT:/i screen-share sound exclusion capability: available (per-application capture)' \
    "$WORK/appimage-0.10.1.txt" >"$WORK/leaves-out.txt"
# Wine writes CRLF.
sed 's/$/\r/' "$WORK/leaves-out.txt" >"$WORK/leaves-out-crlf.txt"
grep -q 'unavailable' "$WORK/includes.txt" && grep -q 'exclusion capability: available' "$WORK/leaves-out.txt" \
    || { echo "fixture construction failed" >&2; exit 1; }

check() { # name function fixture publish expect(pass|fail) [message]
    local name="$1" fn="$2" fixture="$3" publish="$4" expect="$5" want="${6:-}" rc=0
    ( export PUBLISH_PACKAGES="$publish"
      # shellcheck source=../scripts/lib.sh
      source "$LIB"
      if [[ "$fn" == engine ]]; then
          assert_call_media_engine AppImage "$WORK/$fixture" 0
      else
          assert_share_audio_exclusion AppImage "$WORK/$fixture"
      fi ) >"$WORK/log.txt" 2>&1 || rc=$?
    if [[ "$expect" == pass ]]; then
        if [[ "$rc" != 0 ]]; then
            bad "$name: refused ($(tail -1 "$WORK/log.txt"))"
        elif [[ -n "$want" ]] && ! grep -qF "$want" "$WORK/log.txt"; then
            bad "$name: passed without saying '$want'"
        else
            ok "$name"
        fi
    elif [[ "$rc" == 0 ]]; then
        bad "$name: accepted"
    elif grep -qF "$want" "$WORK/log.txt"; then
        ok "$name"
    else
        bad "$name: refused for the wrong reason ($(tail -1 "$WORK/log.txt"))"
    fi
}

printf '== screen-share sound leaves the call out ==\n'
check "a release refuses the shipped 0.10.1 AppImage transcript" \
    exclusion appimage-0.10.1.txt true fail "does not say whether"
check "a release refuses it through the shared engine assertion too" \
    engine appimage-0.10.1.txt true fail "does not say whether"
check "a release refuses a package without excluding capture" \
    exclusion includes.txt true fail "cannot capture shared sound while excluding this call"
check "a release accepts a package that leaves the call out" \
    exclusion leaves-out.txt true pass "exclusion capability: available"
check "the engine assertion accepts it as well" \
    engine leaves-out.txt true pass "exclusion capability: available"
check "Wine's CRLF does not hide the line" \
    exclusion leaves-out-crlf.txt true pass "exclusion capability: available"
check "a build-only pipeline warns about the old transcript" \
    exclusion appimage-0.10.1.txt false pass "WARNING: AppImage"
check "a build-only pipeline warns about an incapable package" \
    exclusion includes.txt false pass "cannot capture shared sound while excluding this call"

if [[ "$fail" != 0 ]]; then
    echo "test-share-audio-exclusion: FAILED" >&2
    exit 1
fi
echo "test-share-audio-exclusion: all checks passed"
