# Open items and the NOT TESTED inventory

## 2026-09-18 — two fixes shipped WITHOUT a live repro, deliberately recorded as such

Both are real by construction and both were shipped in `9fcabb4e`. Neither is
live-validated, and neither may be promoted on the strength of the reasoning
below.

**The image viewer's thumbnail strip.** `mediaSource()` answers a cache miss
with an empty string and dispatches a fetch whose bytes arrive as
`mediaCached`; the strip's `source` touched nothing bumped from that signal,
so a picture the strip had not already cached could never appear. The strip
now carries the same `resolveTick` pairing as EmojiPicker. **It did not
reproduce**: the fixture room renders identically on the fixed and the unfixed
build, because it is small enough that every row's bytes are cached by the
media band before the viewer is opened. A repro needs a room whose images are
outside the band — deep history, or a room opened and scrolled past quickly.

**The bubble tap's exclusion for the action bar** (eighth instance of that
shape in `MessageDelegate.qml`). The bar is a plain `Rectangle` anchored over
the bubble's top-right corner, so its 2px padding and the 2px gaps between its
buttons are not covered by anything that accepts a press and reach the
bubble's own TapHandler, which toggles the pin. The strips exist
geometrically; **no stable synthesized click demonstrated it.** Every probe
point either landed inside an `IconButton`'s hit area or activated a button
and moved the app to a different state, and the one reading that appeared to
show the fall-through came from a probe crop that also contained the
composer's top edge — a layout shift read as the bar unpinning. See
`docs/round-history.md`, 2026-09-18, for the corrected instrument.

**Two latent items found in the same sweep and deliberately NOT changed**, for
want of any reachable symptom:

* `SettingsScreen.qml` binds `visible: app.canEditOwnAvatar()`, a Q_INVOKABLE
  reading `m_client->supportsOwnProfileEditing()`. Settings is built once and
  warm-latched for the session, so the answer is frozen at the first account's.
  Every real account uses the same backend, so no difference is reachable
  today.
* `AppSwitch.qml`'s TapHandler takes the default `DragThreshold` policy, so an
  ancestor handler fires on the same press. `SettingsScreen.qml` already
  documents this and works around it by NOT nesting an AppSwitch inside a
  clickable row, so nothing in the tree reaches it. `gesturePolicy:
  WithinBounds` would fix it app-wide and change press-and-drag-off behaviour
  on ~30 switches, which is not a change to make without a report.

## 2026-09-18 — a peer ENCRYPTING into a call we believe is clear is not reported

Found while fixing the opposite direction (the joiner requiring encryption in
an unencrypted room — see `docs/round-history.md`, 2026-09-18). The receive
probe's two branches are not symmetric:

* `required` and no key -> the frame is DROPPED, counted, and announced.
* NOT required and no key -> the frame is **passed through and counted as
  `passed`** (`src/calls/SfuMediaEngine.cpp`, the `!haveKey && !required`
  branch). A peer that IS encrypting then feeds ciphertext into the
  depayloader while `frames in the clear` climbs and the log reports media
  flowing normally.

It fails in the safe direction — garbage downstream, never a plaintext leak,
and the confidentiality-critical branch does drop — which is why it is
recorded rather than fixed. Telling "ciphertext on a call we believe is
clear" from real cleartext needs the frame-crypto trailer test
(`CallFrameCryptor.cpp`: the IV-length byte must be 12), used as a WINDOWED
verdict and never per frame, because a clear frame passes it ~1 time in 256.

The 2026-09-18 fix removes the common way the two clients disagree at all, so
this state should now be rare. It is written down because a known gap with no
written home gets rediscovered from a user report.

## 2026-09-17 — the Flathub REPO lint finally ran, and its two errors are the harness

The maintainer's gate is "on the vm that supports flatpak, run the full lint
and confirm it is good for submission". `flatpak-builder-lint manifest` has
passed for a while and is mutation-proven twice. The `builddir` and `repo`
lints need a real sandbox build, and **three attempts produced nothing at
all** — the third was reported here as "the build failed to produce a repo",
which was true and unexplained.

**WHY, and it is worth remembering: no session bus.** `flatpak-builder` running
inside the `org.flatpak.Builder` sandbox resolves its sdk by running
`flatpak info` ON THE HOST through the spawn portal (`FB: Running 'flatpak info
--arch=x86_64 --show-commit org.kde.Sdk 6.11' on host`). That needs a session
bus carrying `org.freedesktop.Flatpak`, and the rig container had none, so
every build died at init with

```
Failed to init: Unable to find sdk org.kde.Sdk version 6.11
```

while `flatpak info org.kde.Sdk//6.11` in the same shell printed the ref.
`dbus-run-session` is the entire fix — D-Bus activates the portal from
`/usr/libexec/flatpak-portal` on its own; nothing has to be started by hand.

**THE BUILD NOW RUNS END TO END** and the repo lint with it: Qt and Rust
compile inside the sandbox from the GitHub mirror at the pinned tag, the app
and its `.Debug` export to an OSTree repo (93.0 MB and 213.3 MB), all four
screenshots download and land in `files/share/app-info/media`, icons export at
six sizes plus the SVG, and the desktop file, icon and metainfo are renamed to
the app id.

**Two errors remain, and both are about the FORM of appstream media URLs, not
about the manifest:**

```
appstream-external-screenshot-url
appstream-remote-icon-not-mirrored
```

The evidence that this is the harness:

* `flatpak-builder` passes `--media-baseurl` to `appstreamcli compose` (the
  flag is in the binary's own strings), so the catalogue is written as
  `<components media_baseurl="https://dl.flathub.org/media/">` plus RELATIVE
  image paths. That is the canonical AppStream form.
* the linter tests each `<image>` with
  `startswith("https://dl.flathub.org/media")` and **never resolves
  media_baseurl** — the string does not appear anywhere in
  `flatpak_builder_lint`.
* `--compose-url-policy=full` does not change the output.
* REFUTED on the way: that Debian's flatpak-builder 1.4.4 and Flathub's 1.4.9
  differ here. A full rebuild under 1.4.9 produced exactly the same two errors.
* The screenshots themselves ARE mirrored — the files are in
  `app-info/media` — so what differs is the spelling in the catalogue, not
  whether the mirroring happened.

**AND THE CONTROL WAS RUN, so this is established rather than argued.** A
second app was built with nothing in common but the build command: a minimal
`org.example.LintProbe` — its own app id, its own metainfo, one screenshot, a
shell script for a binary. Same two errors:

```
appstream-external-screenshot-url
appstream-remote-icon-not-mirrored
```

The control does one more thing, and it is the half that makes it worth
having: it ALSO failed a check Lightning passes —
`url-homepage-missing`, a real defect in the probe's own metainfo. So the
linter's content checks are working and they do discriminate between the two
apps. What does not discriminate is the URL-form pair, which fires on both.

**STATUS: manifest lint PASS (mutation-proven twice). Repo lint RUNS, every
content check in it passes, and its only two errors are a property of the
local toolchain that a second app reproduces exactly.** That is as far as this
rig can take it; the remaining question — whether Flathub's own pipeline
skips those two for a submission or resolves `media_baseurl` upstream — is
answerable only by submitting. It is not reported as a full green.

## 2026-09-17 — voice delay: PASS on Linux for the NUMBER, NOT TESTED everywhere else

The maintainer requires a delay claim to hold on **three platforms, Windows
mandatory**. It holds on **one**, for less than it first claimed, and this says
so rather than padding the count. §3 of the guide allows exactly PASS, FAIL and
NOT TESTED, and this section used to say "CONFIRMED" and "NOT MEASURABLE".

**Linux — PASS for the number** (`docs/live-validation.md`): 265.2 ms baseline,
268.1 ms after freezing the sender 1.5 s, delta **+2.9 ms**.

**Linux — NOT TESTED for the claim that the queue fix is what produced it.**
There is no negative control: the pre-fix binary was never run, so "against
roughly +1000 ms on the unfixed queue" was a prediction, and it has been struck
from the live-validation entry. The induced stall may also not exercise the
mechanism at all — `SIGSTOP` freezes producer and consumer together, and the
defect is the consumer falling behind the producer. Both are recorded in full
beside the measurement.

**Windows — NOT MEASURABLE THROUGH RDP, and the attempt is recorded so nobody
repeats it.** The guest has no sound card, so its playback rides an RDP audio
path that has its own adaptive jitter buffer. Four runs of the identical rig,
minutes apart, on an unchanged call:

```
291.8 ms   354.1 ms   491.0 ms   545.0 ms
```

That is the instrument drifting, not the call changing. The stall delta stayed
small in every run (+1.1, -43, +21, +39 ms — never the +1000 the bug produces),
which is weak evidence the backlog is absent on Windows too, but an instrument
that moves 250 ms between identical runs cannot confirm anything.

**THE OBVIOUS WORKAROUND WAS TRIED AND IS STRUCTURALLY BLOCKED.** Windows can
echo a recording device straight back to a playback device — Sound control
panel, Recording, Remote Audio, Properties, Listen, "Listen to this device" —
and that would have given the RDP floor and the through-Lightning copy in ONE
recording, leaving through the same RDP output at the same moment, which
cancels the drift instead of subtracting it. It was enabled and produced
digital silence (`peak_env=1`): the guest's only audio device IS the RDP one,
and Windows will not listen to a device through itself. The setting was
restored afterwards.

So the floor cannot be measured from inside this guest at all. It needs a guest
with a real emulated sound card, or a recorder running in Windows.

**What would actually measure it**, and it needs no new insight, only time:
inject and capture INSIDE the guest — record the guest's own sink to a WAV in
Windows and move only the file out — which turns a two-host clock problem into
a one-host one. Or loop the far end's output back into its own input inside the
guest, detect the return at the sender, and halve, which needs one clock and
one machine.

**Two confounds this rig has, worth knowing before the next attempt.** Every
client on the laptop shares ONE virtual microphone, so every client in the call
captures the burst and sends its own copy — the recording then holds three
onsets and the earliest wins, attributing the number to the wrong participant.
And a browser client left in the call keeps capturing that microphone even when
nobody is looking at it. The detector reports the onset COUNT for exactly this
reason, and every Windows run above was flagged by it rather than silently
averaged.

**So: the three-platform bar is NOT met.** One platform is confirmed, and the
mandatory one is unmeasured.


## 2026-09-16 night — Flathub manifest lint re-run, and what it does NOT cover

**PASS, and proven live.** Re-run in the laptop's `flathub-rig` container
(Debian + `org.flatpak.Builder`, flatpak 1.16.6) against the Flathub submission
manifest: `flatpak-builder-lint manifest` exits 0 with no output. A silent pass
is not evidence, so both mutations from the earlier round were repeated and both
fired — `app-id` renamed gives `appid-filename-mismatch`, `--filesystem=host`
added gives `finish-args-host-filesystem-access` — and the restored manifest
lints clean again.

**One trap worth writing down**: the lint runs inside the flatpak sandbox, whose
uid cannot write to a directory owned by the host user, so it dies with
`Can't create state directory: ... Permission denied` before it reads anything.
That failure looks like a lint failure and is not one. Copy the manifest set to
a directory the sandbox can write (`chmod 0777`) and run there.

**WHAT THIS DOES NOT COVER, and it matters for the word "submission":**

* The manifest in the rig names **tag `v0.9.4`**. A manifest is only
  submission-ready for the tag it actually builds, so this must be re-run with
  the tag and commit of whatever release is submitted. Not yet done.
* `flatpak-builder-lint repo` was NOT run tonight — it needs a full sandbox
  build of the app, which is hours. The earlier round ran it at `v0.9.6` and it
  passed; that is the standing evidence, and it is evidence about v0.9.6.
* Everything in the Flathub submission checklist that is not a linter finding —
  screenshots, branding, donation link, `x-checker-data` — is unchanged and
  still open (vault: `Lightning/Tasks/Flathub submission preparation.md`).


## 2026-09-16 — the Windows builder's Qt pins rotted; moved to 6.11.2

**RESOLVED IN THE RECIPE, image build in progress at the time of writing.**
Fedora withdrew every 6.11.1 Qt package the Windows builder pins, so the
Dockerfile described an image that could not be made. A v7 build died on
`No match for argument: qt6-qtshadertools-devel-6.11.1-1.fc44.x86_64`, having
reached step 7 of 11 only because layers 1-10 came from the v6 cache on that
host; without it the build fails at the FIRST Qt line.

Decision taken: **move the pins to 6.11.2** rather than chase the withdrawn
NVRs through Fedora's archive. A recipe that cannot be realised is worse than a
version move, and the archive would have added a fragile source to keep a Qt
that Fedora has already replaced. Consequence to state plainly: **the Qt that
ships to Windows users moves 6.11.1 -> 6.11.2 with the next Windows package.**

**The rot is the Qt stack and nothing else** — all 31 pinned NVRs in the
Dockerfile were queried against the repos from inside the running v6 image:
23 available, 8 gone, and the 8 are exactly the Qt set. **The first version of
that probe was wrong and said all 31 were gone**, because it used a dnf5
argument that does not exist so every query came back empty. Asking it about a
package known present and one known absent is what caught it, before it reached
a commit or a report. GENERALISE, and this is the second time in one session:
**a probe that returns "absent" for everything is a broken probe until it has
been shown to return "present" for something.**

Two traps recorded beside the pins for the next bump: the release numbers are
NOT uniform (`qtmultimedia` is `-2`, the other six are `-1`), and the
qtmultimedia SOURCE tarball is pinned separately — its new sha256 was taken
from Qt's published `.sha256` AND confirmed by downloading the 10.2 MB file,
because `download.qt.io` without `--location` answers with a 306-byte mirror
page that hashes to something plausible and is not the file.

Remaining, and only after the image verifies: point the host's `config.toml` at
the new tag (keeping v6 in `allowed_images`), restart and `gitlab-runner
verify`, then the promotion commit moving `libgstlevel.dll` into
`GSTREAMER_PLUGINS` and `"level"` into `GSTREAMER_ELEMENTS`, then a
`windows-package-test`. The gst-plugins-good licence gap below should be folded
into whichever image build comes next.

## 2026-09-16 — NO published 0.9.7 package has the capture level meter

**MEASURED, on both artifacts, and it is wider than the Windows note says.**
0.9.7's headline diagnostic is the capture level meter: silence encrypts
exactly like speech, so the meter is the only thing in the engine that can tell
a live microphone from a dead one, and a whole day went into the crypto path
for a capture that was producing nothing. A live call from each published
artifact logs the same line:

```
publishing microphone: valve drop= false device-channels= 0 dsp= true level= false
```

- **Windows portable** (`Lightning-0.9.7-bc5dcd5-windows-x86_64-portable.zip`):
  the hand-built builder image does not carry `libgstlevel.dll`. Builder v7 adds
  it; `stage-windows-runtime.py` keeps the plugin OPTIONAL until v7 is deployed.
- **AppImage** (`Lightning-0.9.7-x86_64.AppImage`): `libgstlevel` was added to
  `build-appimage.sh`'s `GST_REQUIRED_PLUGINS` in `8e744c7`, which is AFTER the
  0.9.7 release commit `bc5dcd5`. So the fix is on `main` and in no release.

Consequence to state plainly rather than bury: **the mic-silence badge and the
sustained-silence warning cannot fire in any 0.9.7 package.** The `level= false`
branch is not a failure — the engine deliberately builds the chain without the
meter when the factory is absent — so a user sees no badge whether their
microphone is live or dead, which is the exact state the feature was written to
end. The RTP pad probe (`rtp packets handed to webrtcbin`) still works
everywhere and is what carried the proof in tonight's interop runs.

**AND THE PUBLISHED RELEASE NOTES PROMISE IT, WHICH MAKES THIS USER-FACING.**
`docs/releases/v0.9.7.md` tells the reader "Lightning now measures what it is
actually sending" and lists three things. Traced through the code, two of the
three cannot happen in any 0.9.7 package:

* *"the microphone's level goes into the log every few seconds"* — NOT
  delivered; the line comes from the `level` element that is not there.
* *"if the microphone produces nothing at all while you are unmuted, you get a
  warning in the call header"* — NOT delivered. `localAudioSilent` is emitted
  ONLY from `SfuMediaEngine::handleMicLevelAt`, which is fed by that element's
  bus messages, so with no element there is no signal and the badge cannot
  fire.
* *"if the capture never starts, the log says that instead of staying quiet"* —
  **delivered.** That watchdog sits on the RTP pad probe, which is independent
  of the meter and works on every platform; it is what carried the proof in the
  2026-09-16 interop runs.

Rewriting a published release body is the maintainer's call and is not done
here. The next release note should say the meter arrived in the release that
actually carries it.

NOT a regression and NOT a defect in the code: both halves are packaging, both
are fixed on `main`, and neither is released.

**Scope check, so nobody widens this by guessing:** the exact-NVR pinning that
rotted for Windows is not repeated elsewhere. A sweep of `packaging-ci/` for
`*-N.N.N-N.fcNN` patterns matches the Windows Dockerfile and its two docs and
nothing else; no other lane pins a distro package by exact version.


## 2026-09-17 — RESOLVED, and it was never Windows-only

**The licence text is vendored in this repository now**
(`packaging-ci/packaging/common/licenses/gst-plugins-good-1.0/`) and staged by
`stage-windows-runtime.py` and `build-appimage.sh`, both of which FAIL if it
is missing rather than shipping without it. `test-pipeline-config.py` asserts
the file, its content and both call sites.

**THE SCOPE BELOW WAS WRONG, and a wrong scope is not a basis for a maintainer
decision.** It framed this as a property of the Windows package. A review
found the AppImage copies eleven gst-plugins-good binaries out of the build
host — `libgstrtp`, `libgstrtpmanager`, `libgstvpx`, `libgstautodetect`,
`libgstpulseaudio`, `libgstalsa`, `libgstvideo4linux2`, `libgstximagesrc`,
`libgstlevel`, `libgstvolume`, `libgstaudioparsers` — and staged NO licence
text of any kind: a grep for `licen|COPYING|LICENSE` across that script and
its validator returned one hit, a comment about HEVC. The snap repacks the
AppImage, so it inherits the gap. Deb and rpm link the system GStreamer and
Flatpak uses the runtime's, so those three are clean.

**AND THE BLOCKER WAS AN ASSUMPTION, NOT A CONSTRAINT.** The text below says
fixing it "means SOURCING the text rather than copying it", which was read as
needing a 960 MB builder-image rebuild. It does not: the licence is a file,
the repository is a place to keep a file, and both stages read from the source
tree they already have.

**STILL THE MAINTAINER'S CALL, and deliberately left open**: whether to
publish a written offer for the corresponding source of those plugins, and
where. LGPL-2.1 section 6 allows several ways to satisfy it and the choice is
about the project, not about packaging. Recorded in PROVENANCE.txt beside the
text.

The original entry follows, because the evidence in it is what made the fix
possible.

## 2026-09-16 — the Windows package ships gst-plugins-good binaries without its licence

**OPEN, compliance, pre-existing, and cheap to fix in the NEXT builder image.**
`packaging-ci/packaging/windows/Dockerfile` copies licences for
`gstreamer-1.0 gst-plugins-base-1.0 gst-plugins-bad-1.0 gst-plugins-rs libnice
opus libvpx libsrtp orc zlib webrtc-audio-processing mingw-runtime`, and
`stage-windows-runtime.py` ships that directory into the package. **There is no
`gst-plugins-good-1.0` in it** — verified by listing
`/usr/share/licenses/lightning-gstreamer` inside the live v6 image, which
returns twelve names and not that one.

Meanwhile the image stages plugins that ARE gst-plugins-good: `libgstjpeg`,
`libgstrtp`, `libgstrtpmanager`, `libgstautodetect`, `libgstdirectsound`,
`libgstdirectsoundsrc`, `libgstvpx` and, from v7, `libgstlevel`. So a Windows
package distributes LGPL binaries whose licence text it does not carry. The gap
predates the level meter by months; v7 makes it one plugin wider.

Found by the independent review of the v7 change, which flagged it and
correctly declined to block on it. **FIXED in the Dockerfile the same evening**
and folded into the v7 image build rather than costing its own rebuild.

**Verified on the ARTIFACT, not just the image**: `unzip -l` on the shipped
0.9.7 Windows portable lists exactly twelve directories under
`Lightning/licenses/lightning-gstreamer/` — `gstreamer-1.0`,
`gst-plugins-base-1.0`, `gst-plugins-bad-1.0`, `gst-plugins-rs`, `libnice`,
`libsrtp`, `libvpx`, `mingw-runtime`, `opus`, `orc`,
`webrtc-audio-processing`, `zlib` — and good's is not among them.

**SETTLED: THE SDK DOES NOT SHIP IT, so no name would have worked.** I first
inferred `gst-plugins-good-1.0` from its `base`/`bad` siblings and the build
refused it — loud, correct, and it cost one GStreamer layer while leaving the
committed Dockerfile briefly unbuildable. Rather than guess a third time at
960 MB a go, the SDK was extracted ONCE to a path that is not deleted
(`/srv/gst-sdk-licences/licence-dirs.txt` on 10.195.35.2): **88 licence
directories, and nothing matching "good"** — `gst-plugins-bad-1.0`,
`gst-plugins-base-1.0`, `gst-plugins-rs`, `gst-rtsp-server-1.0` and
`gstreamer-1.0` are all there; good's is simply absent.

**So this cannot be fixed by copying, and it needs a decision rather than a
patch.** gst-plugins-good is LGPL-2.1+ and its COPYING lives in the
gst-plugins-good release, not in the MinGW SDK, so shipping it means sourcing
the text from upstream and vendoring it — a choice about what this project
distributes and from where. **It is an LGPL compliance defect, not cosmetics:
every Windows package Lightning has ever shipped carries gst-plugins-good
binaries with no licence text.** Shipping another release knowing that is a
different act from the eight times it shipped unknowingly, which is exactly why
this is the maintainer's call and is written here instead of being quietly
patched at 1 a.m.


## 2026-09-16 — after 0.9.6

**THE FILTERED-HISTORY VIEWPORT FILL IS FIXED AND NOT LIVE-TESTED, AND THE
REASON IS WORTH RECORDING.** The 2026-09-16 round (see `docs/round-history.md`)
makes a room whose recent history is MatrixRTC churn fill its viewport instead
of stopping after eight pages with one message on screen. It is proven by
automated coverage that FAILS on the unfixed tree at both layers, including one
case that asserts only the user-visible outcome — the viewport ends up full
with nothing scrolling it. **No live Matrix room was opened.** The verification
laptop has no route to `matrix.smetonis.net`: its WireGuard default route is
dead and the wifi it is on carries LAN only, so `curl` to the homeserver times
out and no account can be signed in at all. The live check when a networked
machine is available: open the DM the maintainer reported, with
`LIGHTNING_SCROLL_TRACE=1`, and confirm the viewport fills without scrolling
and the trace shows `filtered=1` with `emptyPages=` climbing rather than
`fill-declined reason=noProgressBudget`.

**And the budget may still be too small for a long enough call.** 60 pages is
~1200 filtered events at the 20-per-page the maintainer's log measured. A run
longer than that still stops with a blank viewport, and the reader is back to
scrolling. Nothing observed says his room is that long — his capture reached
232 — but the number is a bounded guess matched to the constant the 2026-09-15
round already accepted, NOT a measurement. If it is reported again, the trace
line to read is `fill-declined reason=emptyPageBudget`, and the answer is a
measurement of how long these runs actually get, not another raise.

**Media messages now notify, but NOT LIVE-TESTED.** `557d21b` makes an image,
video, voice message or file arriving in a room with no timeline open produce a
notification and an Activity row. No real desktop notification has been SEEN
for one. That is the obvious first live check next time someone is at a
machine: send a picture from Element to a Lightning account with the room
closed, and confirm the toast says "Sent an image" and the Activity row carries
the camera icon.

**A voice message notifies as "Sent an audio file" on the sync path.** The
payload carries no is-voice flag, so `mediaIsVoice` is false and
`NotificationManager` cannot pick the voice wording. Cosmetic, known, not
fixed.

**The room-list stale-ordering symptom is STILL OPEN and needs a product
decision from Rokas.** `c01bfa8` landed a backstop for a different, real
mechanism, and deliberately does NOT stamp a room from MatrixRTC membership
churn. A room whose newest event is churn therefore still has no ordering
producer. Stamping from churn would make an idle call outrank a live
conversation; that trade is his to make, not one to take at 02:00.

**The reload path's media rows are kind-only.** `reloadRoomTimeline` emits no
`media_mxc`, mimetype, size or dimensions and never did, so a media row built
there names a file it cannot fetch. Harmless only while that function has no
callers — verified: only its declaration and definition exist, and
`reloadRoomTimelineAtLive` is a different function.


Moved out of `CLAUDE.md` §16 on 2026-09-11: that file had reached 140,752
characters against its own 140,000 rewrite threshold (and a hard 150,000 limit
past which it truncates SILENTLY, dropping its own tail — §§17-19 — from agent
context). This is the FOURTH such move: §7 to `docs/feature-contracts.md` on
2026-08-28, §16's round history to `docs/round-history.md` on 2026-09-03, §2's
release inventory to `docs/release-operations.md` on 2026-09-10, and this.
Nothing was deleted; the whole block is below unchanged.

This one was chosen because §16 says outright that it is "a LESSON INDEX, not
an inventory" — and an inventory is exactly what this block is. The lessons,
refutations and standing warnings stay in `CLAUDE.md`; the list of what is
open and what has never been tested lives here.

**READ THIS BEFORE claiming anything is fixed, before promoting anything to
tested, and before proposing a fix in an area it covers.** An item here that
says NOT TESTED has not been tested, however plausible the code reads.

### Closed by the 2026-09-13 packaged-flatpak sweep

**`expires_for_refresh()` — CLOSED, PASS.** A two-party call between the
packaged flatpak and an AppImage ran 19m30s, ~4x the five-minute window, with
both clients reporting `participants= 2` and both `frames in the clear in`
counters climbing at the end. This was the longest-standing unexercised call
fix.

Also closed on a PACKAGE for the first time, all flatpak: screen share through
the portal (the RECEIVING client's own window drew the desktop, on the default
RHI backend — not a counter and not the sharer's self-view), share audio, call
audio FRAMES in both directions (**audibility remains NOT TESTED**), group
power control, threads end to end, the command palette executing an action, a
freedesktop notification DISPLAYED and **both of its actions PRESSED** (Mark as
read sent two real read receipts and cleared the `(1 unread)` caption; the
inline Reply landed a message decrypted on the peer, from a client that was
never focused), the call stage's tile grid checked against a known state (two
tiles, correct names, and the crossed-microphone badge on the PEER's tile only
after it muted), the updater's check, and token + crypto-store persistence
across a restart. Detail and the full
NOT-TESTED residue in `docs/round-history.md`, 2026-09-13 (afternoon).

**THE CONFINED SNAP'S DEAD MEDIA PATH — CLOSED, LIVE-VALIDATED PASS
2026-09-13.** Pipeline 214's snap carries the NSS modules and a two-party call
on the confined snap reached 3000 frames in the clear BOTH ways with zero srtp
errors; `received track` appeared for the first time on this lane. It is NSS:
Debian builds libsrtp2 against NSS, which dlopens libsoftokn3/libfreebl3 from a
runtime-derived path that no ELF walk can see, and core24 has no NSS to fall
back to. The AppImage carries the same gap. Staged and asserted now; the next
pipeline's snap is the test. Original investigation notes below. Everything else on the snap works: it
starts, renders on real GL, signs in, syncs, notifies, joins a call and shows
"Voice connected". But `frames in the clear` never appears in either
direction and `received track` never fires. Three hypotheses are eliminated —
not a missing plugin (29 are staged and the env points at them), not the
publisher's bus error tearing the call down (the handler never calls
`failed()`), not Matrix-level (membership, media key, SDP answer and ICE all
good). 0.9.4 already ships a Snap that could not start at all, so this is an
improvement rather than a regression, but calls do NOT work on it. Two rig
limits bound further work here: no audio daemon and no portal in the snap
container. See round-history 2026-09-13.

**Still NOT TESTED after it**: screen share and camera on the SNAP (no portal
in that container); recovery/key-backup setup and therefore
cross-user verification (both fixture accounts report cross-signing and secret
storage "Missing", and the setup flow puts a generated recovery key on screen,
which §6 forbids capturing); media send and the file chooser under either sandbox (ATTEMPTED on the
flatpak this round and blocked by the rig — a tmpfs is mounted over the
document portal's fuse mount, so the portal's returned path is unreachable
inside the sandbox; see round-history 2026-09-13); audibility of any call; a notification body CLICK (as distinct from
its two action buttons, both of which passed); and the speaker ring on a call
tile.

### Open items and NOT TESTED inventory

OPEN DEFECTS, reported live and not yet confirmed fixed. These are the list.

**QT MULTIMEDIA STILL COMES UP AT EVERY LAUNCH, and the trigger is not
located (2026-09-15).** `CallDeviceController`'s constructor is deliberately
empty because touching QMediaDevices initialises the Qt Multimedia backend,
which on a PipeWire desktop prints a `spaVisitChoice: parse error` per device
(the symbol is in `libQt6Multimedia.so.6` — confirmed by walking the binary's
shared libraries — so the messages are Qt's own pod parser and what is ours is
waking it). `enableCallMediaEngine()`'s unconditional `applyDevices()` was one
such waker and is now gated on a stored device preference, **but the noise
survives that fix**: with no preference stored the gate skipped the call and Qt
Multimedia still initialised before `voice-call media engine active` is logged.
`GstCallMediaBackend::runtimeAvailable()` is pure GStreamer element probing and
`setMediaBackend` touches no devices. `applySfuDevices()` a few lines below has
the identical shape and is the first place to look.

**THE `GstIntRange` CRITICALS ARE NOT LIGHTNING'S — LOCATED 2026-09-15, AND THE
SUSPECT WAS WRONG.** A burst of
`GStreamer-CRITICAL ... range start is not smaller than end for GstIntRange`
fires after the main screen loads, and
`perApplicationCaptureAvailable()`'s unfiltered device monitor was recorded
here as the suspect. It is not. **`gst-device-monitor-1.0` — a stock GStreamer
tool with no Lightning code in the process — produces 28 of the identical
criticals on the maintainer's desktop.** So it is GStreamer's own device
provider building a degenerate `GstIntRange` (`start >= end`) out of one of the
machine's audio devices, and every client on that machine that enumerates audio
devices will print it.

**Consequences worth keeping.** It is HOST- and DEVICE-dependent, not
build-dependent, so its presence or absence in a log says nothing about a
build. It is noise: nothing in the enumeration fails, and the devices are
listed correctly afterwards. And the fix is not ours to make — do not spend
another round hunting it in `src/calls/`. If it ever needs closing, the move is
to identify the offending device's SPA rate choice and report it upstream to
GStreamer, not to change Lightning.

**Separately, and NOT the same thing:** `spaVisitChoice: parse error` lines are
**Qt Multimedia's** PipeWire pod parser (`libQt6Multimedia.so.6`), not
GStreamer at all. `519ee1f` stopped Lightning forcing Qt Multimedia up at every
launch, and that gate works as designed — but it deliberately still pays the
init for a user who HAS stored a device preference, and such a user therefore
still sees these lines. Two different libraries, two different triggers, one
log.

**"WAITING FOR KEYS" — FIXED AND LIVE-VALIDATED PASS, 2026-09-15.** Two
screenshots of the same rows going from "Waiting for keys…" to their text with
no Retry press, no passphrase and no interaction in between, on the committed
code, verified twice. See `docs/live-validation.md`. The mechanism: The
automatic path the sections below say was missing now exists:
`recover_keys_for_utds` (`rust/src/timeline.rs`) runs a bounded per-session
backup download for any undecryptable event that arrives, on the room's
initial snapshot and on every diff, and then retries decryption for exactly
those sessions — the same machinery the manual Retry button uses, which is why
it needs no new SDK surface. `mark_backup_attempt` stopped being a permanent
set: each key records its attempt count and time, a first try is always
allowed, then the wait doubles from 30 s to a ~32 min ceiling, and after five
attempts the key is left alone for the rest of the lifecycle. Nothing polls —
an attempt only happens when an undecryptable row is actually in front of the
user.

**What was deliberately NOT done:** `BackupDownloadStrategy::OneShot` stays.
Switching to `AfterDecryptionFailure` is the obvious lever and it was tried in
v0.7 and reverted, because it fetches one key per freshly-failing event and
left already-rendered history encrypted after verification; OneShot is what
bulk-downloads everything when a session is verified. The gap was never the
strategy — it was that nothing re-ran after the single pass a room gets when
it opens. `automatic-room-key-forwarding` is likewise still off.

**KNOWN LIMITATION:** a homeserver that answers `M_UNRECOGNIZED` or
`M_FORBIDDEN` for the backup endpoint stops that key for the rest of the
session — a server that gains the endpoint mid-session is not re-probed until
the next launch or a manual recovery. Accepted deliberately: the alternative
is retrying a permanent refusal for ever, which is what pass 3 of the review
was about. Note the realistic 403 here is `M_WRONG_ROOM_KEYS_VERSION`, which
ruma models as a TUPLE variant and which therefore lands on `Inconclusive` —
bounded at 32 minutes and self-correcting once the SDK refreshes the version —
so the `Forbidden` lever rarely fires on this endpoint.

**KNOWN LIMITATION:** the hook is on the ROOM and THREAD timelines only. A thread's
undecryptable replies are not covered, which matches the pre-existing shape of
`retry_decryption_after_import` (active room only). A key imported for the
room decrypts the thread's copy on that thread's next retry, but nothing
triggers one automatically.

**LIVE VALIDATION: PASS** (2026-09-15, on `e72d97d`). What it does NOT cover:
Element interoperability, recovery across a room switch, and the THREAD
timeline hook — the room hook is what was exercised.

The mechanism this fix closes, recorded when it was still open: A user reported
undecryptable messages that re-entering the recovery passphrase fixed. An
audit of the whole path found the structural answer, and it is larger than the
report: **Lightning has exactly ONE automatic route from "the key is in my
backup" to "the row decrypts", and it runs at most once per room per session.**

Two of the three mechanisms §9's diagram used to name are NOT WIRED, both
verified against the tree rather than inferred:

- `automatic-room-key-forwarding` is not among the features `rust/Cargo.toml`
  requests and `matrix-sdk` is `default-features = false`, so
  `create_outgoing_key_request` is `#[cfg]`'d out. **Lightning has never sent
  an `m.room_key_request` on a decryption failure**, in any version —
  `git log -S` over `rust/Cargo.toml` shows the string has never been there.
- `BackupDownloadStrategy::OneShot` (`rust/src/lib.rs`) makes matrix-sdk
  install neither the UTD event handler nor the `BackupDownloadTask`, so a
  decryption failure triggers no backup fetch.

What remains is `download_backup_keys_for_room`, deduplicated per room per
lifecycle; its only re-entry points are the startup and `BackupState::Enabled`
edges, each at most once per session and **active room only**. And
`mx_rust_recover_from_backup` is the ONLY caller of `clear_backup_attempt`
anywhere — so typing the passphrase is literally the only thing in the tree
that can force a second pass. That asymmetry explains BOTH halves of the
report (stuck on its own, cured by the passphrase) with no bug anywhere: it is
what the code is written to do.

**NOT a 0.9.5 regression.** That structure has been unchanged since v0.6.3
(July 2026); `git log bcea599..8d5d0ca` over the crypto/supervisor paths
touches none of it, and the offline-restore work (`dcbfe39`, `92e9864`) is
NOT an ancestor of v0.9.5 and configures encryption identically anyway (both
paths funnel through one `build_client_with`; only the homeserver input line
differs). "Back" is most likely the same never-fixed structural gap recurring.

**Ranked candidates, still requiring a capture to choose between:** (1) the
room's one pass had already run; (2) the backup key was never in the crypto
store, so `are_enabled()` was false and every pass returned silently; (3) the
report is the ACCOUNT-LEVEL banner ("Requesting encryption keys from your
verified session…", `CryptoBootstrapModel`), not the per-row string, in which
case manual recovery is the designed remedy and the finding is copy; (4) the
pass ran and failed its two network attempts; (5) the key arrived and the row
never refreshed — weakest, both known defects of that shape were fixed before
0.9.5. **Ask which surface he saw; a screenshot separates (3) from the rest in
one step.**

**INSTRUMENTED 2026-09-15 so the next capture is decisive.**
`download_backup_keys_for_room` had THREE silent early returns, so "backups
are not usable", "already attempted this lifecycle" and "the pass ran and
found nothing" were one indistinguishable absence — while
`CryptoBootstrapModel` reads **Ready** either way, because its download field
simply stays empty. They now emit `skipped_no_backup_key`,
`skipped_already_attempted` and `skipped_bad_room_id`.

**THAT VOCABULARY SHIPPED ON THE WRONG EVENT KIND AND A REVIEW CAUGHT IT.**
The first cut sent the skips as `kind: backup_download`, and the commit
message, the source comment and this file all said "behaviour is unchanged by
construction, the model compares that field only against `started` and
`failed`". The comparisons ARE inert; **the assignment is not.**
`CryptoBootstrapModel` assigns `m_download` unconditionally and does not
return, so `recompute()` read a skip as "not failed" and therefore **Ready** —
a room that ran NO pass erased what a room that FAILED one had recorded,
retiring the recovery banner on an ordinary room switch and claiming Ready
over history that was never restored. That is §6's "never report a cleanup as
successful when it removed nothing", in the round whose whole purpose was to
make this path honest, and all 207 tests passed on it because none had ever
fed a skip into that model. Skips now carry their own kind, the
`backup_download` branch refuses a `skipped_` state regardless, and
`aSkippedPassDoesNotRetireTheEscalation` pins both.
**GENERALISE: proving nothing COMPARES a field is not proving nothing ASSIGNS
it.**

The two SDK log lines that separate the halves are
`Failed to decrypt a room event … session_id=SSSS` and
`Successfully imported room keys … room_keys={…}` — if `SSSS` never appears in
an import line the key never arrived; if it appears while the row still reads
"Waiting for keys…" the key arrived and the row never updated. Nobody has ever
told those two apart.

**THE "TO-DEVICE DELIVERY IS BROKEN" REPORT WAS THE HARNESS, AND THE HARNESS
BUG SILENTLY INVALIDATED A WHOLE ROUND (2026-09-15, RESOLVED).** An earlier run
found that no device of `lightningtest2` could receive a Megolm room key while
the sender's SDK log reported success, and recorded it as possibly the
homeserver or possibly Lightning. **It was neither, and both are exonerated by
direct measurement:**

- **The homeserver is fine**, proven WITHOUT Lightning: a raw `/sendToDevice` +
  `/sync` between two brand-new accounts delivered both a custom type and
  `m.room.encrypted` — the type that carries Megolm keys.
- **Lightning is fine**, proven twice: two brand-new accounts, one brand-new
  device each, fresh encrypted room, `Received a new megolm room key` with the
  matching session id and the plaintext rendered.

**The cause: four Lightning instances running concurrently on ONE profile,
sharing one device id and access token and racing for that device's to-device
queue.** Whichever synced first consumed and acked the events; the rest saw
nothing. The symptom is indistinguishable from a dead transport — a healthy
sync loop, 27+ cycles, zero to-device events.

**And the reason four were running is a `pgrep` idiom this file has warned
about before, in a new costume.** `pgrep -f "log-file …" | head -1` matches the
`nix develop -c ./build-rust/lightning-matrix … --log-file …` WRAPPER as well
as the app, and `head -1` returned the wrapper — so every `kill` reported
success and left the app alive. `~/lt-sweep/catchkey.sh` uses the identical
idiom, and the previous round's devices all shared one device id, so **that
round's negative result was almost certainly this same bug**. Strongly
evidenced, not proved: the run was not resurrected.

**FIX THE HARNESS BEFORE THE NEXT E2EE ROUND:** match `^\./build-rust/lightning-matrix`
or a PID captured at launch, never a `-f` pattern that the launcher's own
command line also contains. **GENERALISE: a `pgrep -f` pattern that appears in
the wrapper's argv kills nothing and reports success** — the third member of
the family that already holds `$(pgrep -c x || echo 0)` and the wait loop that
matches its own command line.

**Refuted in the same round; do not re-propose without saying which claim was
refuted:** `IdentityBasedStrategy`/strict device trust (absent from all four
profiles, so it was off); Synapse device-key immutability (measured — 1.156.0
ACCEPTS a replacement identity key for an existing device id); and Olm account
recreation under a reused device id (never happened, identity keys constant).

**Fixture health, worth knowing:** `@lightningtest` has **19** devices,
`@lightningtest2` **7**, and **neither has a cross-signing master key**.
`lightningtest3` and `lightningtest4` were created for this reason.

**Bonus finding, not chased:** with strict device trust ON and nobody
cross-signed, **sending fails outright** — the message sat as "failed · Retry ·
Cancel" and no to-device request was created at all. Worth knowing before that
setting is ever promoted.

**THE STALE ROOM-LIST REPORT IS A CALL-CHURN DEFECT, AND THE 2026-09-16
RECENCY BACKSTOP DOES NOT FIX IT. NOT FIXED.** A user reported that the room
list shows a stale last-message time and position, and that OPENING the room
corrects both. A round traced it to Lightning's live event handler matching
`Text | Notice | Emote` with `_ => return`, built a sliding-lane recency
harvest, and it was reviewed and tested. **Then the desktop sweep measured it,
and two things came out that stop it shipping:**

- **The old code did not reproduce the defect.** With the harvest disabled AND
  the pre-fix fallback restored, an `m.image` into a closed room still moved
  that room to the top with a fresh time. The `_ => return` handler is real,
  but it feeds the OPEN room's timeline; the room list's `last_activity_ms`
  comes from `room_payload`, a **separate producer that handles images fine**.
  So in a healthy sync session the plain closed-room media case was never
  broken, and the fix changes nothing observable there.
- **The reported symptom is still present WITH the fix.** A room seeded with 3
  real messages and then 30 `m.call.member` events showed **no time, no
  preview and bottom-of-list position** — below a room two hours older — and
  stayed that way for 3.5 minutes and across a full restart, until it was
  opened. That is the report, verbatim.

**The likely mechanism, stated as the hypothesis it is:** sliding sync runs
`DEFAULT_LIST_TIMELINE_LIMIT = 1`, so the response carries only the room's
NEWEST event. In a call room that is churn, the allow-list correctly declines
it, and no producer is left. Not measured — measuring it is the next step.

**Why this is genuinely hard rather than a missing line:** the room whose churn
was declined ordered by its last real MESSAGE once opened (01:09, not the
01:12 churn), and that is the behaviour we want. Stamping from churn would fix
the blank row by making a room with an idle call outrank a room somebody just
spoke in. **Which of the two a user wants is a product decision, not an
implementation detail**, and it is the maintainer's to make.

**The backstop itself is sound and is NOT lost** — reviewed, mutation-tested,
monotonic, and it cannot make ordering worse. It is simply held out of 0.9.6
because a change with no demonstrated benefit does not belong in a release,
and because shipping it would let a release note claim a fix that measurement
says is not one.

**THE 2026-09-15 WINDOWS GUEST ROUND — two guests driven at once, and four
results worth keeping.** Clock checked first, as this file requires: `tzutil
/g` = UTC and the guest was 18 s from the laptop's `date -u`, so the 7-hour
skew that invalidated the 2026-09-12 call-state readings is gone and stayed
gone.

- **THE WINDOWS CAMERA DELIVERS ~29.8 fps SUSTAINED — RE-MEASURED WITH THE
  SHUTTER OPEN, 2026-09-15. The 10 fps ceiling is GONE in this configuration.**
  The first pass that day measured 29.79 fps on one run and an exactly steady
  10.00 fps on three others, and Windows then reported the camera *"blocked or
  turned off by a switch"* — the laptop's privacy shutter was closed, so none
  of those numbers meant anything. **The maintainer opened it and the round was
  re-run.** Shutter confirmed open from inside Windows first (a real lit
  picture in Settings, and three consecutive captures of the same static scene
  hashing differently — sensor noise, not a frozen frame).

  Three independent runs, each a fresh launch and a fresh call, rate taken from
  the `capture delivered frames count=` probe on the `capsrc` src pad (what the
  DEVICE emits, upstream of `jpegdec`):

      8500 frames / 285.264 s = 29.797 fps
      6500 frames / 218.033 s = 29.812 fps
      4500 frames / 150.961 s = 29.809 fps

  Flat rather than averaged: every 500-frame bucket in every run is 16.76-16.79
  s. Chain and caps identical in all three — `camera chain= mjpg (jpeg elements
  present )`, `image/jpeg 1920x1080 30/1`, `firstCaptureMs= 514-535`. **The
  negotiated rate and the delivered rate now agree**, which is exactly what the
  previous round could not say.

  Same guest measured 0.9.4's raw chain at `YUY2 1920x1080 framerate=5/1`, so
  raw = 5 and MJPG = 29.8 on one sensor and one passthrough. The 10.00 fps was
  also ruled out as a device-selection artefact: with the FHD function
  disabled, `ksvideosrc` answered `No video capture devices found` and the
  publish failed, so the IR/`AvStream Media Device` function is not openable by
  it at all and the device was necessarily the same in every run.

  **BOUNDARY, stated rather than glossed:** this is a QEMU `usb-host`
  passthrough, not bare metal, so it does not measure the HOST's USB 2.0 bus.
  The raw-YUY2 bandwidth ceiling that the original theory named still needs
  physical hardware. What is settled — and never had been — is the delivered
  rate on a working sensor through the shipped MJPG chain.
- **THE SELF-VIEW TILE IS FINE.** The previous round saw Lightning's
  crossed-camera placeholder while frames were being delivered, but the sensor
  was shuttered, so a placeholder and black video were indistinguishable. With
  a working sensor the "You" tile shows the camera image, live (a cat visible
  in two later captures and absent in two earlier ones). **No change to
  `SfuMediaEngine.cpp` is indicated**; the `localCameraStreamId()` match works.
  The earlier entry is withdrawn.

  **AND IT CAME BACK ON 2026-09-16 — AND MY EXPLANATION OF IT IS WITHDRAWN.**
  A Windows interop run saw a blank self-view and a camera-off placeholder in
  Element while the track published 5000 frames at a steady 30 fps. I stopped
  the guest, read the webcam on the host (`/dev/video0` mean=1.9e-07,
  stddev=5.4e-05) and called it the sensor. **Three things are wrong with
  that.** It measures a different OS through a different driver after the guest
  released the device. `ffmpeg -frames:v 1` takes a UVC device's FIRST frame,
  before auto-exposure and AGC converge, and a stddev that low is as consistent
  with a zero-filled buffer as with a dark room. And decisively: **a dark sensor
  paints BLACK VIDEO, not a placeholder** — zooming the capture shows the "You"
  tile is the tile's dark-grey background with a centred crossed-camera glyph,
  which is the no-picture state, not a video surface full of dark pixels.

  The check that would have settled it was already on this page: the 2026-09-15
  entry above confirmed the shutter FROM INSIDE the guest — a lit picture in
  Settings, and three consecutive stills of a static scene hashing differently
  (sensor noise, not a frozen frame). Use that, not a host reading.

  **What actually settles it, most decisive first:**
  1. Take the sensor out of the experiment — a known-good source (a lit scene
     with the shutter confirmed open, or a synthetic source) in a
     Lightning-to-Lightning call. If the picture renders, the render path is
     fine. If it does not, there is a real defect and it has now been explained
     away twice.
  2. In the guest, before the call: Windows Camera shows a lit picture, and
     three stills of a static scene hash differently.
  3. Screenshot what the FAR end draws, and say which of the two it is: a black
     rectangle or a camera-off placeholder. They are different defects and my
     notes have claimed both.
  4. Any host-side capture at all: `-frames:v 30`, and report mean AND stddev
     of a LATE frame.

  **AND THE SENSOR IS NOT DARK — MEASURED PROPERLY, 2026-09-16 night.** Redone
  the way point 4 says: a frame 30 in rather than the first reads
  `mean=0.155 stddev=0.0397 max=1` on `/dev/video0` — a properly exposed
  picture with real contrast. The original `mean=1.9e-07` was `-frames:v 1`
  catching a UVC device before auto-exposure and AGC converge, which is the
  known artefact the review named. So the basis for "it was the sensor" is not
  merely unsupported, it is **refuted**: this camera produces a usable image.

  What that leaves: a Windows client that published 5000 camera frames at a
  steady 30 fps, from a sensor now known to produce a picture, and drew the
  no-picture state at both ends. **That is a real defect until something proves
  otherwise**, and the remaining honest gap is narrow — the measurement is of
  the HOST, after the guest released the device, at a different time from the
  call. Closing it is step 2 above and takes a minute: in the guest, before the
  call, Windows Camera shows a lit picture and three stills of a static scene
  hash differently.

  **AND THE LOG NARROWS IT SHARPLY — read from the failing session itself.**
  Three greps of `win97-0.9.7-interop.log`, the run that failed:

  ```
  scene graph backend=d3d11 software=0
  camera chain= mjpg (jpeg elements present )
  capture negotiated caps= image/jpeg, width=1920, height=1080, framerate=30/1
  capture delivered frames count= 2000
  ```

  and **zero** occurrences of `frames decrypted but NOT rendered`.

  That kills three hypotheses at once. It is **not the software renderer** —
  the session is on Direct3D, so `softwareRendererHidesVideo` is false and the
  tile is not being suppressed by it. It is **not an MJPG negotiate failure** —
  the chain Windows uniquely takes negotiated 1080p30 and delivered 2000 real
  frames, which is the failure mode the code comment warns about and it did not
  happen. And it is **not frames arriving with nowhere to go** — that warning
  exists and never fired.

  So capture, negotiation, DECODE, encode and publish are all healthy — and
  that last part is no longer an inference from the capture counter, which sits
  on `capsrc`'s src pad UPSTREAM of `jpegdec` and the encoder and so could only
  ever say what the camera produced. The publishing bin's own encoder pad
  logged `publish first encoded frame screenShare= false afterPublishMs= 894
  firstCaptureMs= 786`, and the encrypt probe on that same pad then climbed
  past 6500 while the capture counter reached 2000 — the rate stage duplicating
  a 10 fps capture up to the pinned 30. So `jpegdec`, the newest and least
  exercised element in that chain, is exonerated too.

  The fault is **downstream of capture, in getting a frame onto a surface**. In QML terms the
  tile's `videoLoader` is `visible: active && item && item.hasFrame`, where
  `hasFrame` is `videoSink.videoSize.width > 0` — so the self-view sink never
  received a sized frame while the tee's other branch encoded 2000. That is the
  narrow place to look: the `selfvidsink` appsink branch and `onVideoSample`.

  **The near end and the far end are still being treated as one fact and they
  are not.** The self-view fails through `app.groupCall.cameraOn` and the local
  tee; the far end fails through `cameraKnown && cameraOn` and mid routing.
  They may have different causes and the logs should be read separately.

  **AND ON 2026-09-17 IT DID NOT REPRODUCE AT ALL.** Same package, same guest,
  same camera, in a real call against the published Linux AppImage: the
  self-view drew a recognisable picture on the MJPG chain AND on the raw chain
  (forced by moving `libgstjpeg.dll` out of the package, which needs no new
  build), and the far end logged `frames decrypted ... video= true count= 1000
  dropped= 0` with zero `NOT rendered` warnings. Numbers and method in
  `docs/live-validation.md`.

  So the hypothesis above — that the fault is in the self-view branch, and the
  narrower one that MJPG is the cause — is **REFUTED by an A/B in one session**.
  Nothing in the code changed between the failing session and this one, so this
  does not say the defect was fixed. It says the defect is not a property of
  the package, the chain, the camera or the renderer, and that whatever
  produced it on 2026-09-16 was not present a day later.

  **What that leaves, and it is the honest remaining gap**: an intermittent
  fault with no identified trigger. The next step is no longer a hypothesis
  about the pipeline — it is to find what differed between the two sessions.
  Candidates worth checking before guessing again: the guest's clock (it has
  been wrong by seven hours once already, and an expired membership makes a
  participant's video vanish at the far end), how many stale RTC memberships
  were in the room, and whether the far end that saw nothing was Sable or
  Element rather than Lightning.

  Camera on Windows is **NOT REPRODUCED, NOT EXPLAINED**. It does not block a
  release. Nobody may say it is fixed, and nobody may say the picture never
  arrives either — it arrived, measured, at both ends.

- **THE TRAY BALLOON'S READ-WITHDRAWAL IS CONFIRMED BROKEN ON WINDOWS, no
  longer merely predicted.** Display and click routing PASS again (toast with
  the room avatar; a click raised the app from minimised and opened the room).
  But after `read receipt sent event_id= …` with the unread badge and the
  title's "(1 unread)" both cleared, the toast was **still on screen 23
  minutes later**, having survived the read, an application restart AND a full
  in-app update. Exactly what the code comment predicts — Qt cannot withdraw a
  balloon; it needs WinRT `ToastNotificationHistory.Remove`. Notification
  SOUND remains NOT TESTED: the guest has no audio hardware at all.
  Observation, not a verdict: the FIRST toast after launch rendered the sender
  as the raw MXID and later ones rendered the display name, so profile
  hydration lags the first notification.
- **"volume 0 does not mute" is STRUCTURALLY untestable on this guest** —
  `Get-PnpDevice -Class AudioEndpoint` returns nothing, Lightning agrees ("No
  microphone was found"), and `participant volume applied: … elements=` only
  fires for a REMOTE participant, of which a single guest has none. NOT
  TESTED, and the microphone-gain line was deliberately not substituted for
  it; that is a different claim.
- **THE WINDOWS PORTABLE UPDATE PATH IS NOW LIVE-VALIDATED PASS**, 0.9.4 ->
  the real 0.9.5, end to end: the manifest fetch, download and verification
  all work on Windows ("Update 0.9.5 downloaded and verified"), the folder
  swap and relaunch are clean, and afterwards the exe reads FileVersion 0.9.5
  / `Built from Lightning source 8d5d0ca`, still signed in, rooms and read
  state intact, 28 GStreamer plugins present, with the old install kept as
  `Lightning.lightning-previous`. This closes part of the standing "every
  Windows update path is untested" item. **MSI and installer paths remain NOT
  TESTED** — this is the portable path alone.
- **NEW DEFECT, ROOT-CAUSED AND FIXED THE SAME DAY: a Windows camera or
  microphone preference could never be stored.** Choosing the webcam showed it
  selected and read back "System default" on the next visit.
  `sanitizedDeviceId()` in `SettingsManager.cpp` refused any id containing a
  backslash, and a Windows QMediaDevices id is a device path that BEGINS with
  two (`\\?\usb#vid_…`) — so every one of them stored as the empty string,
  which means "system default". The rule came from a GStreamer pipeline
  concern and its own comment named PipeWire, so it could only ever fire on
  the platform it was not written for. Nothing interpolates these ids on
  Windows: the 1:1 helper returns early there and the SFU engine sets the
  property on the PARSED element. Fixed; the storage rule keeps its length
  bound and control-character refusal. See `docs/round-history.md`.
  **Sharper than first recorded, and worse:** the selection does not survive a
  PAGE REVISIT, not merely the next session — leaving Sound & video for
  Appearance and coming straight back already reads System default. And because
  `resolveActive()` returns empty for an empty preference, `SfuMediaEngine`
  skips `applyBindingTo("capsrc", …)` entirely, so **on 0.9.5 "System default"
  and an explicit pick are the SAME engine configuration and the explicit one
  is unreachable**. The fix is on `main` above 0.9.5 and is therefore **NOT
  TESTED on Windows** — the guest still runs 0.9.5.
- **FIXED AND LIVE-VALIDATED PASS ON THE PACKAGED WINDOWS BUILD (2026-09-15,
  `5bf1db5`): `--log-file` created no file, and `--version` printed nothing.**
  Measured before and after on the shipped bytes — `src/main.cpp` is
  byte-identical between `v0.9.5` and `e72d97d`, so the installed 0.9.5 was a
  valid BEFORE; the AFTER is the portable zip from pipeline **217** built from
  the fix.

  | command | BEFORE | AFTER |
  |---|---|---|
  | `--version > file 2>&1` | **0 bytes** | 17 bytes, `Lightning 0.9.5` |
  | `--version --log-file L1` | **no file** | exists, carries the version |
  | `--log-file L2 --version` | 133 B, **banner only** | banner + version |
  | `--call-media-status --log-file L3` | **no file** | full status block |
  | `--log-file L4 --call-media-status` | **0 status lines** | 1 |

  **THREE defects, and only one was about Windows.** `preflightParse()` is a
  single left-to-right walk in which every terminating flag ends in `return r`,
  so a `--log-file` standing after one was never read — reproduced on Linux
  too, so this was never Windows-specific. The status commands PRINT rather
  than log, so the handler never saw their output and the file held only its
  own header. And `configureWindowsConsole()` called `freopen("CONOUT$")`
  unconditionally, destroying an inherited shell redirect — that is the 0-byte
  file, and the handle must be sampled BEFORE `AttachConsole`, which replaces
  it.

  The A4 file now carries what the whole feature exists for, including
  `RESULT: calls can be placed and answered.` — so a tester on Windows can
  finally answer "why can I not call from this build".

  **Still NOT TESTED:** macOS (the ordering fix is platform-independent by
  construction but has not run on a Mac); the other status flags individually
  on Windows (converted through the shared path, not exercised one by one);
  and `--version --console` still opens a console that closes on exit, which is
  why `--log-file` is the answer for capture.
- **THE TRAY BALLOON'S READ-WITHDRAWAL NEEDS THE WINDOWS NOTIFICATION PATH
  REPLACED, NOT A WITHDRAWAL CALL ADDED. Feasibility established 2026-09-15;
  deliberately NOT implemented.**

  **The cheap options are refuted by evidence already in hand.** The live round
  recorded the toast surviving an application restart — and app exit destroys
  the tray icon (implicit `NIM_DELETE`). So if deleting the icon dismissed the
  toast, the restart would have cleared it. It did not: the notification has
  been promoted into the Action Center, where the shell owns it and the tray
  icon does not. That single observation kills both
  `QSystemTrayIcon::hide()/show()` and `Shell_NotifyIcon(NIM_MODIFY)` with an
  empty `szInfo`. The 10 s timeout is a hint Windows ignores, so bounding the
  lifetime is not a fix either.

  **The toolchain is NOT the blocker — measured in a real mingw64 container,
  not reasoned.** `windows.ui.notifications.h`, `roapi.h` and
  `libruntimeobject.a` are present and
  `IToastNotificationHistory::RemoveGroupedTagWithId` resolves. The **C++
  projection route is blocked** (mingw's own `windows.foundation.h` collides:
  `IReference<boolean>` and `IReference<BYTE>` are distinct in the IDL and both
  `unsigned char` in C++), but the **C ABI route compiles AND links** with
  `-DINITGUID -lruntimeobject -lole32`.

  **The cost is the blocker.** A WinRT toast is a different delivery mechanism,
  not a call bolted onto the existing one: it needs a registered
  **AppUserModelID**, and there is none anywhere in the source or packaging —
  the portable zip has no installer, so the app would have to write its own
  Start Menu entry on first run. And click routing, which is live-validated
  PASS today through Qt's `messageClicked`, would move to a registered
  `ToastActivatorCLSID` COM server — **losing a working feature to gain
  withdrawal would be a net regression.** Roughly 300-400 lines of hand-written
  C-ABI vtable calls, none of it checkable by any Linux-hosted test.

  **Scope it as "replace Windows notification delivery with WinRT toasts,
  keeping click routing", as its own round.** The interim position is honest:
  the stale toast is cosmetic, because the click still routes correctly to the
  room — the harm is a toast that looks unread after it has been read. macOS
  has the same hole with a different API and the same shape of cost.
- ~~**`--version` and `--call-media-status` produce NO OUTPUT on the packaged
  Windows build**~~ — see the entry above; fixed and confirmed on shipped bytes. Measured three ways (cmd redirect,
  `Start-Process -RedirectStandardOutput`, and a timed redirect showing the
  process ran 1.2 s, exited 0 and wrote 0 bytes). `--call-media-status
  --log-file \\host.lan\Data\cms.log` exited 0 and created **no file at
  all**, which is the more serious half — `--log-file` is documented as
  working on every platform. Consequence: the shipped-artifact self-checks
  that §16 and §10 lean on cannot be read from a Windows package today, and
  build identity had to be taken from the exe's version resource instead.
  A GUI-subsystem binary having no attached console is the obvious lead for
  the stdout half; it does NOT explain the missing log file.
- **OBSERVED, NOT DIAGNOSED: the local self-view tile shows the no-video
  placeholder while the camera is publishing.** With `capture delivered frames
  count= 1000` and `frames in the clear out … video= true count= 1000`, the
  "You" tile drew Lightning's own crossed-camera glyph — not black video,
  which matters because the sensor really is shuttered. The self-view branch
  is `SfuMediaEngine.cpp` (`appsink name=selfvidsink` -> `onVideoSample` with
  `localCameraStreamId()`); that stream-id match is where to look, and there
  is no self-view diagnostic in the log at all.

**THE 2026-09-15 LAYOUT AUDIT: four defects fixed, and what it did NOT cover.**
Modern, Compact and Bubbles were each driven against a real room on two
throwaway accounts, with mixed own/other messages, wrapping bodies, group
headers and live read receipts, and the action bar pinned on a receipt row in
each. Full account in `docs/round-history.md`, 2026-09-15.

- **NOT COVERED by that audit**, and none of it should be read as confirmed:
  images, video and audio rows in any layout; reactions and reply previews;
  thread panels in Bubbles; the room-activity and call-event rows; any layout
  at a non-default text size or interface zoom — which matters because the
  reporter blamed their scaling, and the defect turned out to be independent
  of it but the SCALED cases were still never rendered.
- **IN BUBBLES THE ACTION BAR STILL COVERS A SHORT OWN MESSAGE.** Seen in the
  audit and deliberately NOT changed: an own bubble is right-aligned and the
  bar sits at the row's top-right, so a one-word bubble disappears under it
  while hovered. Element Web behaves the same way, and moving the bar to the
  left of an own bubble is a design decision rather than a defect fix. Worth
  a maintainer's opinion before anybody "fixes" it.
- **`sharevalve` is still a seam** (below), so muting the share's audio
  independently of the microphone remains unimplemented.

**THE 2026-09-14 USER-REPORT ROUND IS FIXED IN SOURCE AND NOT LIVE-VALIDATED
ANYWHERE.** Three reports against 0.9.5, all three root-caused and fixed, none
of them driven against the thing that reported them. Full account in
`docs/round-history.md`, 2026-09-14.

- **Offline restore** (a homeserver that died put the user on the login page).
  Covered by a Rust case that kills a real loopback homeserver under a real
  SDK client with a real sqlite store, and asserts the old path can no longer
  build while the new one can. **NOT TESTED against a real account with a real
  homeserver down**, and the two things that most deserve a live look are
  whether cached TIMELINES open (the event cache is persisted; nothing here
  proved it opens with no server) and whether local search works in that
  state. Also NOT TESTED: the first offline start of an account that has not
  signed in since this build, which still fails — the recorded URL is written
  by a successful build and there is nothing to fall back to before one.
- **Share audio** (`unexpected reference "shareaudiomix"`, and the call being
  torn down for it). Both halves have a test that fails on the unfixed tree.
  **NOT TESTED live**, and per-application share audio has NEVER been
  exercised on any machine — it could not parse, so every observation of
  "share audio works" to date is the sink-monitor fallback. Do not read the
  2026-09-13 flatpak sweep's share-audio PASS as covering it.
- **Share-audio MUTE is a seam, not a feature.** The share's Opus chain
  carries `valve name=sharevalve drop=false` and its comment used to read as
  though muting the share independently of the microphone were implemented.
  Nothing in the tree looks that valve up (the microphone's `micvalve` IS
  driven), so there is no such control. Found in review 2026-09-14,
  pre-existing, comment corrected; the feature is not written.
- **The call-join freeze (GitHub issue #12).** NOT REPRODUCED. The fix is a
  narrowing (no enumeration when there is no device preference) plus a 2.5 s
  bound with a per-klass latch. Whether it closes that reporter's freeze is
  unknown, and the honest next step is their log — `--log-file PATH` — from a
  build that carries it, not another guess.

**`channels-home` IN SCREENSHOT-DEMO MODE RENDERS THE CLASSIC LAYOUT, NOT
CHANNELS (found 2026-09-13, NOT diagnosed).** Launching
`--demo-scenario=channels-home` produces a capture byte-comparable to
`home-overview`: the same Home room list, the same timeline, no Channels
column. `--demo-scenario=classic-home` and `channels-space` were not tried.

It was found while capturing website screenshots, so the evidence is a picture
rather than a log, and the scenario was simply dropped from that set. What is
NOT established: whether the scenario fails to activate, activates and is
overridden, or activates correctly and the capture happens before the layout
switch lands. The last is plausible and cheap to test first -- the same
capture run needed its delay raised from 3500ms to 9000ms before ANY timeline
was populated, so this may be the same timing and not a defect at all.

Worth an answer because the Channels layout is a shipped navigation mode and
`channels-home` is how anyone would photograph it.


**RECOVERY AND KEY BACKUP: FOUR KNOWN DEFECTS, NOT FIXED, AND THE WHOLE
FEATURE IS STILL NOT TESTED BY USE (2026-09-13, shipped that way in 0.9.5).**
The audit that found and fixed six of them (`2eb38b1`, round-history
2026-09-13 night) read the code rather than driving it, because §6 forbids
capturing a recovery key and the setup flow displays one. These four survived
it. **None can destroy a key** — that is why they are follow-ups — but every
one of them is a user being told something untrue:

- **A setup interrupted by quitting says nothing about it.** An `enable`
  aborted by teardown after secret storage exists but before the key is
  delivered leaves the account changed and the user uninformed. The 1500 ms
  action-pool join is the mechanism and the FFI's own comment already concedes
  it and describes the real fix.
- **`recover()` reports "Recovery complete" when nothing was restored** —
  specifically when 4S held no backup key. Same class as §6's "never report a
  cleanup as successful when it removed nothing".
- **A mistyped recovery key shows an untranslated SDK error** rather than
  "that key is wrong", which is the one moment the message has to be plain.
- **The restore panel can wedge at "Restoring…"** if the session ends
  mid-restore.

**NOT TESTED, and it cannot be tested the usual way.** No release has ever
live-validated this feature. Both fixture accounts report cross-signing and
secret storage "Missing", so even reaching the flow means generating a key on
screen. If it is ever to be exercised, the capture rule has to be solved first
— not waived.

**EVERY LINUX PACKAGE LANE IS GREEN AND FIVE OF THEM ARE NOW GUI-VALIDATED ON
THEIR TARGET DISTRO (2026-09-12, pipeline 208 at `cd21193`).** Twelve jobs —
six builds, six validations — all success. Beyond CI, each package was
installed in a clean guest on the laptop and launched against the real
homeserver with an already-signed-in fixture profile, so the packaged binary
rendered on a real display and synced:

| lane | install | GStreamer | Qt | GUI |
| --- | --- | --- | --- | --- |
| Ubuntu deb (26.04) | `apt`, deps resolved | 1.28.2 | — | signed in, synced |
| Debian deb (13.6) | `apt`, deps resolved | 1.26.2 | **6.8.2** | signed in, synced |
| Fedora rpm (44) | `dnf` | 1.28.7 | — | signed in, synced |
| Flatpak | `flatpak install` | 1.26.11 | — | CLI only |
| AppImage | n/a | bundled | 6.8.2 | validated earlier the same day |

All report `call media engine built in: yes` with both the 1:1 and SFU engines
available, across **four different GStreamer versions** — which is exactly the
split that has bitten this project before. The Debian run is the one worth
keeping: it is Qt **6.8.2**, the version that produced the
`QConcatenateTablesProxyModel::roleNames()` and inline-`QPointer` defects.

**THE SNAP IS THE ONE LANE WITH NO LIVE INSTALL ANYWHERE.** `validate-snap`
says so itself — "structural + payload; live snapd install is [not done]" —
and the laptop has no snapd, so nothing has ever run that artifact. It is not
a defect, it is an untested lane, and it should be named as such rather than
counted with the others.

- **THE macOS ASSET CANNOT BE UPLOADED, AND A RELEASE WOULD SHIP WITHOUT IT
  IN SILENCE (found 2026-09-12, pipeline 208).** `macos-package-test` BUILDS
  and VALIDATES perfectly — "macOS bundle validation passed (arm64,
  289443840 bytes)", every check green — and then dies on
  `Uploading artifacts as "archive" to coordinator... 413 Payload Too Large`.

  The cause is **Cloudflare**. `gitlab.smetonis.net` resolves to Cloudflare
  addresses and answers `server: cloudflare`; a deliberate 150 MB POST to it
  returns 413, which is the free plan's 100 MB request-body cap. That is the
  same "approximately 100 MiB request limit" `docs/windows-runner-operations.md`
  already records, and the Windows manager was moved to the host-internal
  `http://10.195.35.2` endpoint precisely because of it — it uploaded a 345 MB
  artifact through that route today without complaint. GitLab's own nginx is
  `client_max_body_size 0` and the external nginx allows `2000m`, so neither of
  those is the limiter.

  **WHY IT MATTERS MORE THAN A RED JOB.** The job is `allow_failure: true` and
  `publish-packages` needs it `optional` — deliberately, so one sleeping Mac
  cannot block a release. With this failure that safety valve is now permanent
  and silent: the pipeline goes green and publishes every other artifact, and
  macOS just is not there. It is the only lane whose absence a green pipeline
  will not report.

  **WHAT IS NOT EXPLAINED.** The same job with the same ~289 MB bundle
  SUCCEEDED at the 0.9.4 release (pipeline 186, 2026-09-09, upload
  `201 Created`, bundle 289,222,656 bytes against 289,443,840 now — 221 KB
  apart, which cannot cross a 100 MB line). So the artifact did not change and
  the path did. GitLab moved 19.2.4 -> 19.2.6 in that window, and the Mac
  mini's runner URL has not been inspected. Do not guess: read the Mac
  runner's `config.toml` `url` first.

  **THE FIX IS KNOWN AND ALREADY PROVEN ON ANOTHER RUNNER**: point the Mac
  mini's runner at the host-internal GitLab endpoint for coordinator traffic,
  exactly as the Windows manager does, keeping source clones on HTTPS. It
  needs access to the Mac mini, which this session does not have.

  **IT HAPPENED AGAIN AT THE 0.9.5 RELEASE, AND 0.9.5 SHIPPED WITHOUT macOS
  (2026-09-13, pipeline 215).** Identical shape: the bundle built and passed
  every check on the Mac — "macOS bundle validation passed (arm64, 289460224
  bytes)", `--version` reporting `Lightning 0.9.5`, calls, plugin directory,
  registry helper, image formats, signature, no secrets — and then
  `413 Payload Too Large` on the artifact upload. So this is no longer a thing
  that might bite a release; it has cost one, and it will cost every release
  until the Mac mini's runner URL is changed.

  Two numbers worth keeping, because they show how little headroom there ever
  was: 0.9.4's artifact uploaded at **104,855,066 bytes**, which is **2,534
  bytes under 100 MiB**. It did not "work before and break now" by any margin
  anyone could have noticed — it cleared the limit by two and a half kilobytes.
  Whatever else moved in that window, the lane was one small build away from
  this outcome the whole time.

  **AND THE PUBLISHED 0.9.5 RELEASE NOTES SAY macOS "is published".** The
  notes are read from `docs/releases/v0.9.5.md` at the resolved release commit,
  so the sentence was fixed in the repository AFTER the pipeline had already
  used it. The repository file carries a marked correction; the GitLab and
  GitHub release bodies still carry the original wording, and changing those is
  a maintainer decision (§14 treats a published release as immutable).

- ~~**The camera does not work at all**~~ — **FIXED and LIVE-CONFIRMED on
  Windows 2026-08-27** (`31e6048`), as is the window share's aspect ratio and
  resizing a shared window mid-share. It was never the camera, and "screen share
  works on the same publish path" was the clue rather than the puzzle: the
  window share worked because Lightning's OWN capture element stamps PTS from
  ZERO, and every other source stamps the pipeline's RUNNING TIME.
  `videorate` starts its output clock at SEGMENT START, so a camera switched
  on three minutes into a call handed it a first PTS of three minutes and it
  owed thirty duplicate frames for every second of that — emitted as fast as
  the encoder would take them. ONE picture at full rate, every counter
  healthy. See `videorate skip-to-first` below and §16's rate-stage note.
  **The 10 fps ceiling is a SEPARATE and still-open item**: `ksvideosrc`
  negotiated YUY2 1280x720@10 because `libgstjpeg.dll` is not staged, so an
  MJPG mode cannot negotiate and raw YUY2 at 720p saturates USB 2.0. Fixing
  the freeze did not raise the rate. The control lag is also unclosed
  (`firstCaptureMs` 794-811 ms, the KS device open running on the GUI thread
  inside `gst_element_sync_state_with_parent`); it needs a
  `LIGHTNING_GUI_STALL_TRACE` capture, not a theory.
- **An account switch FREEZES the UI for 3-5 seconds** — **the WAIT IS NOW OFF
  THE GUI THREAD (2026-09-04, Task A §2), and the defect is not yet closed.**
  `releaseRustHandle()` used to run `mx_rust_shutdown_tasks` and
  `mx_rust_destroy` on the thread that draws the window; the second drops the
  tokio runtime and therefore blocks until every in-flight spawn_blocking
  finishes, SQLite closes included. Both now run on a two-thread retirement
  pool that takes ownership of the handle. Safe because **Rust never calls
  back into C++** — events are PULLED by `pollRustEvents()` on a 100 ms timer,
  so a retiring client has no route into a QObject that may have been
  destroyed. Three callers still wait, deliberately, because they delete or
  rename the store and must not race an open SQLite connection: resetRustStore,
  resetLocalSession and AppController's account removal, plus the destructor at
  process exit (`waitForRustRetirement`). Found in the same sweep and also
  fixed: `VideoPosterExtractor`'s destructor joined its decoder thread on every
  switch, bounded by the measured **931 ms** Qt Multimedia backend
  initialisation.
  LIVE-VALIDATED: three A->B->A switches on real accounts with
  `LIGHTNING_GUI_STALL_TRACE=1` recorded **no GUI stall for any switch** (the
  only two in the session are at startup), each logging "rust client retired
  off the GUI thread" at 1-4 ms. WHY IT IS STILL OPEN: both fixture accounts
  are tiny — twelve rooms, no avatars — so this proves the wait is no longer on
  the GUI thread, NOT that a large account is fast. The remaining question is
  whether anything ELSE scales with account size; that still needs a capture on
  a real account. Regression guard: `rust-retirement`, whose first version was
  worthless (a 250 ms budget that an empty store passes synchronously) and
  which now requires the work to be OUTSTANDING when the caller returns.
- ~~**An account switch FREEZES the UI**~~ — original report, kept for the
  numbers: reproducibly, on the
  SECOND switch (A→B→A) and not the first. Distinct from the unbounded
  profile-fetch loop (`be195f7`) and from the double-polled JoinHandle
  (`e50eff6`); this is a synchronous block, and `shutdown_managed_tasks`
  does `block_on` on the GUI thread with a 15 s budget over a pool that
  includes ~170 avatar fetches.
  **A 2026-08-26 log makes the leading suspect look WRONG.** The same A→B→A
  round trip reported `teardown_total_ms= 23` on the first switch and
  `teardown_total_ms= 308` on the second — so the second switch IS ~13x
  slower, and it is 0.3 s, not 3-5. Whatever costs seconds is somewhere else,
  and the next round should stop reading `shutdown_managed_tasks` and get a
  `LIGHTNING_GUI_STALL_TRACE` capture across a switch instead. One capture
  beats another theory; that is the standing rule in this file and it applies
  to the theory this file itself wrote down.
  **2026-09-04: THAT CAPTURE EXISTS NOW, AND IT DID NOT REPRODUCE.** A real
  A→B→A round trip on `matrix.smetonis.net`, driven through the GUI with
  `LIGHTNING_GUI_STALL_TRACE=1`:

      switch 1  begin 16:56:20.674 -> login succeeded 16:56:20.917   243 ms
      switch 2  begin 16:57:10.656 -> login succeeded 16:57:10.914   258 ms
      teardown_total_ms = 3 and 1

  NO GUI stall was recorded during either switch. The only stalls in the whole
  session were at startup (271 ms unattributed, 1730 ms rust-poll-drain) and
  one 622 ms rust-poll-drain when the second account was first ADDED — not a
  switch. `shutdown_managed_tasks` is now definitively not the cost: 1-3 ms.
  **This does NOT close the defect.** Both test accounts are small — twelve
  rooms, two members, no avatars — and the recorded suspicion involves ~170
  avatar fetches, so the load is nowhere near the reported case. What is
  established is that the switch MACHINERY is fast and the cost scales with
  something the fixture does not have. The next capture needs a real account
  with real rooms and real avatars; it will not be found on a fixture.
- **The screen share's startup is still VARIABLE**, though far less so, and
  there is now a MEASUREMENT: one live share on 2026-08-26 reported
  `afterPublishMs=135 firstCaptureMs=58 rateStageHoldMs=77` — 77 ms of
  `videorate` hold, not seconds. **THREE MORE CAPTURES ON A SECOND MACHINE,
  2026-09-12** (the laptop, packaged AppImage): `122/59/63`, `124/62/62` and
  `127/61/66`. So the bound now rests on four captures across two hosts and
  they are tightly clustered — 122-135 ms to the first encoded frame, 58-62 ms
  to first capture, 62-77 ms of rate-stage hold — rather than on a single
  reading. OBSERVATION, not a conclusion: every one of those shares was started
  through the portal picker, and the picker's own dismissal damages the screen
  immediately before capture begins, which is exactly the second buffer
  `videorate` is waiting for. That may be why the hold is so consistent here
  and why the originally reported 5-10 s has not been reproducible on either
  machine. It is NOT a measurement of a genuinely still desktop, which remains
  the case the mechanism predicts is slow and which nothing has yet captured.
  The cause is unchanged, so it bounds the problem rather than closing it; a
  desktop that is genuinely still (no damage) still has nothing to deliver.
  Previously live-confirmed as near-instant on the first share and 1-2 s on a
  restart, against the 5-10 s originally reported. The cause is unchanged and
  unfixed — `videorate` emits nothing until a SECOND input buffer arrives and
  a desktop capture delivers ON DAMAGE, so the wait is "how long until
  something on the screen changes". THREE properties have now been shipped
  against it without measurement and all three made it worse: `min-buffers=8`
  and `keepalive-time=100` each killed the capture outright (and `min-buffers=8`
  is now UNDERSTOOD — see the pipewiresrc note in this section: 8 exceeds the
  buffer ceiling a compositor offers, so on a PipeWire >= 1.6 daemon it cannot
  negotiate at all; `min-buffers=1` is required and shipped), and `compositor`
  as the rate stage cropped a 3840x2160 desktop to its top-left quarter
  (compositor is NOT a scaler — it paints each input at native size on its
  output canvas). A fourth guess is not acceptable.
  **The open lead, measured but NOT shipped:** putting the SIZE ceiling
  BEFORE the rate stage makes `compositor` usable without the crop —
  `sink 1920 / src 1920` against a 4K input, where caps-after gave
  `sink 3840 / src 1920`. What is still unmeasured is the other half, that it
  keeps the instant first frame, and that must go through the suite's own
  `framesFromASingleCaptureBuffer` harness before anything ships.
- ~~**The incoming-call prompt's Accept does nothing.**~~ — **FIXED and
  SHIPPED in 0.8.3** (`87aafd9`, `6e9bd9a`). The card had ONE Accept button
  serving two unrelated lanes: the legacy 1:1 `m.call.*` lane, answered by
  `app.calls.answer()`, and a MatrixRTC ring, which announces a SESSION and is
  answered by JOINING it (`app.groupCall.join()`, gated on
  `app.rtc.joinBlockReason()`). A MatrixRTC ring therefore showed an Accept
  that `CallController::answer()` refused at its own front door. The prompt now
  offers each lane its own control, and `qml/IncomingCallPrompt.qml` carries
  the whole reasoning at the top of the file. Live re-validation of an ANSWERED
  call is still **NOT TESTED** — the button reaches the right code now, which
  is not the same claim.
- **Windows camera runs at 10 fps**, and the fix for the freeze did not touch
  it. `ksvideohelpers.c` exposes `image/jpeg` for MJPG media types, the
  publish bin links `capsrc ! queue ! videoconvert` with no decoder, and
  `libgstjpeg.dll` is absent from the staged plugin list — so an MJPG mode
  cannot negotiate and the camera falls back to raw YUY2, which at 1280x720
  is 18.4 MB/s and hits a USB 2.0 ceiling at 10 fps. Staging the jpeg plugin
  and adding a decoder to the camera branch is the lead; it is a packaging
  change, so it must go through the shipped-artifact check (§16).
  **PACKAGING HALF DONE 2026-09-02** (lightning-deploy): `libgstjpeg.dll` is
  now staged in BOTH Windows lists — the builder sysroot in
  `packaging/windows/Dockerfile` and the shipped zip in
  `stage-windows-runtime.py` — and `jpegdec:libgstjpeg` joins the Wine element
  probe. Note `libjpeg-8.dll` had been staged all along as a LIBRARY
  dependency of Qt and libgstopengl: a DLL of the right name is not the
  element, the same distinction that shipped Windows for months with
  `libgstsctp-1.0-0` present and `sctpenc` missing.
  **THE APP HALF IS DONE — CORRECTED 2026-09-12, this paragraph claimed
  otherwise for ten days.** It is IN HEAD and has been waiting on the image:
  `cameraJpegEntry()` builds the `image/jpeg` chain,
  `jpegCameraChainAvailable()` probes whether the decoder exists and links, the
  capture FALLS BACK to the raw entry when it does not, and `camera chain=
  mjpg|raw (jpeg elements present|absent)` says which was built — so the two
  ways a camera can sit at 10 fps are finally distinguishable in a log. That
  probe is exactly what the Windows guest reported: `camera MJPG chain
  unavailable … no element "jpegenc"`.

  The probe puts `jpegenc ! ` in front of the chain under test so it actually
  carries `image/jpeg`, rather than a raw source that would link past the
  capsfilter and prove nothing — the trap below, applied in advance for once.

  **EVERY PACKAGING PRECONDITION IS NOW SATISFIED AND PROVEN IN CI
  (2026-09-12).** Builder `...-v6` is built and deployed on the runner host
  (`sha256:5c628d4b`, `jpegenc` and `jpegdec` both in `libgstjpeg.dll`), the
  plugin is REQUIRED again rather than optional, and `jpegdec` is in the
  element set the Wine probe runs against the EXTRACTED package — because
  staging the DLL is not the same claim as the element registering. Three
  green `windows-package-test` runs walk the chain: pipeline 205 on v6 at 27
  bundled plugins, 206 with the plugin required at 28, and 207 with the
  element probed at 41 elements.

  Getting there found that the builder Dockerfile had been UNBUILDABLE for ten
  days — its verify stage asserted 27 staged plugins while the install loop
  staged 28 — so the operator step recorded as open could not have succeeded
  if anyone had attempted it.

  **CLOSED — LIVE-VALIDATED PASS on the guest, 2026-09-12, with the laptop's
  own USB webcam passed through.** (An earlier line here said the VM had no
  passthrough and the claim could only be closed on physical hardware. That
  was wrong: `/dev/bus/usb` is mounted into the container, the host's
  `/dev/video*` is empty because QEMU holds the device, and this item's own
  paragraph above records the defect being REPRODUCED that way.)

  Before, on the released 0.9.4 portable:

      camera chain= raw (jpeg elements absent )
      capture negotiated caps= video/x-raw, format=(string)YUY2,
          width=(int)1920, height=(int)1080, framerate=(fraction)5/1

  After, on the pipeline-207 portable that carries `libgstjpeg.dll`:

      camera chain= mjpg (jpeg elements present )
      capture negotiated caps= image/jpeg, width=(int)1920,
          height=(int)1080, framerate=(fraction)30/1,
          pixel-aspect-ratio=(fraction)1/1
      capture delivered frames count= 500

  Same guest, same webcam, same 1080p: **5 fps to 30 fps negotiated**, frames
  sustained (10 -> 500 over ~32 s through the publish stage), and a real
  picture on screen rather than a counter. The control-lag half improved with
  it: `firstCaptureMs= 424` against the 794-811 ms this item records for the
  raw path, because the KS device opens far faster for an MJPG mode.

  The `camera chain=` line fires when a camera is STARTED, not at launch, so a
  machine with no camera cannot produce it.

  **AN IN-PLACE PORTABLE UPGRADE TO THAT BUILD WAS ATTEMPTED ON THE GUEST AND
  IS INCONCLUSIVE — recorded as unresolved rather than as a result.** The app
  relaunched and is signed in on D3D11, so the portable session survived a
  second upgrade. But `dir` then reported `libgstjpeg.dll` ABSENT from
  `C:\Users\tester\Lightning\gstreamer-1.0`, and the wrapper had echoed
  "expanded" unconditionally after an `Expand-Archive` whose own output it
  never captured — so whether the expand failed, partially applied, or
  succeeded while the file landed elsewhere is NOT established. It says
  nothing about the package: CI proves the shipped zip carries the plugin and
  that `jpegdec` registers from the EXTRACTED tree under Wine. What it might
  say something about is the portable UPDATE path, where a user unzips over an
  existing install — worth re-running with the expand's stderr captured, on a
  guest whose VNC is healthy.

  GENERALISE, because this is the third shape of it this week: a wrapper that
  prints its own success line after a command whose output it discards reports
  success it did not observe. Redirect the command, not the echo.

  WHAT REMAINS IS THE IMAGE, and only that. `libgstjpeg.dll` went into
  `packaging/windows/Dockerfile` and `stage-windows-runtime.py`'s required list
  on 2026-09-02 and the image was never rebuilt, so the plugin has been in the
  recipe and not in the tin. Verified by inspecting `…-v5` directly: neither
  `libgstjpeg.dll` nor `opengl32sw.dll` is in it. Builder tag **v6** exists to
  carry a rebuild and has NO Dockerfile change, because the runner pins by
  exact tag with `pull_policy = "if-not-present"` and a same-tag rebuild would
  never be picked up.

  **`decodebin` in front of that capsfilter is REFUTED, with evidence — do not
  re-propose it.** It builds, then logs `element="decodebin0" … "Delayed
  linking failed."` and `element="capsrc" … "Internal data stream error."`, and
  `aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure` fails because the
  capture is retired. NOT a latency cost: one-buffer wall time 497 ms without
  the decoder, 478 ms through `decodebin`, 486 ms through `jpegenc ! decodebin`.
  A bare `gst-launch` probe of the same three chains PASSES, because it
  negotiates a different format than the engine does (the engine's capture came
  up `A444_16LE`) — "a probe is evidence only if it shares the property under
  test", third occurrence.

  AND THE 10 fps NUMBER IS NOT ANSWERABLE ON THE VM. The claimed cause is a
  USB 2.0 ceiling (raw YUY2 at 1280x720 is 18.4 MB/s); a virtualised
  passthrough does not reproduce it, and the device's mode list can differ
  through the hypervisor. What the guest CAN answer is whether MJPG negotiates
  at all — `camera chain= mjpg` plus `capture negotiated caps=` naming
  `image/jpeg` — and whether the raw fallback still publishes when it cannot.
  The ceiling itself needs Rokas's own hardware.
- ~~**Raise hand is invisible to Element**~~ — **FIXED and LIVE-CONFIRMED
  2026-08-26** in both directions. The wire representation was established by
  READING element-call's own source rather than guessing: an `m.reaction`
  annotating the raiser's OWN `m.call.member` state event with
  `\u{1F590}\u{FE0F}`, lowered by redacting it. Three lanes (our send, two
  sync handlers, one bounded join-time sweep for hands raised before we
  arrived).
- **The `room_list malformed diff rejected` storm — CAUSE FOUND AND FIXED
  2026-09-08 (`b0c27ee`), live confirmation still outstanding.** The capture
  this entry asked for was never needed: the cause was readable. TWO
  PRODUCERS wrote one index base. The SDK's diffs address the vector from
  `entries_with_dynamic_adapters` (20 rooms, growing in batches); the
  snapshot came from `client.rooms()` — the whole state store, different
  order, Spaces included — and both were handed to `handleRoomsEvent`, which
  rebuilt `m_roomOrder`. So mark-as-read, favourite, accept-an-invite,
  create/leave a room and a dozen other ordinary actions replaced the index
  base, the next `set{index}` addressed a different room, was rejected, and
  the rejection called resync, which re-emitted the same snapshot. That is
  why it was account-shape dependent: on a small account the two orders
  coincide. `4185a92` could not have fixed it — it touched only the C++ side.
  The snapshot now has its own event and never writes the index space, and
  resync re-emits from the stream that owns it. Also fixed alongside: an
  index-addressed `remove`/`pop_*` deleted whatever it found with NO id
  check, so a drifted index silently removed a room the SDK never named.
  Do not re-apply the Space exclusion; that one is still in place. WHAT WOULD
  CONFIRM IT: a session on `test_matrix.smetonis.net` with no rejection line
  after a Mark-as-read on a large account.
- **Rooms "lag when they load" on 0.9.0 (Rokas, 2026-09-05; users report
  the same).** MEASURED the same evening with a timestamped log: the history
  fill's page count (see the standing warning above) — 9 pages / 4.2 s on
  a first open, 18 / 11 s on a re-open of a call room. Fixed by lowering the
  invisible-page budget from 60 to 12 plus the wheel-on-short-content
  request; live re-check NOT TESTED at the time of writing. Also changed on
  code evidence: member hydration no longer rebuilds every message body, and
  media rows outside the viewport band no longer fetch at open. Ruled OUT by
  reading: the search sweep (startup and every five minutes, off the GUI
  thread), the per-room deep index and first-unread paging (buttons only).
  **22:48 the same day, after the filter build: "messages are hard to load
  when a call is happening in the room."** No capture yet. Ruled out by
  reading: the per-minute membership state does NOT fire `membersChanged`
  (only a full roster does, RustSdkMatrixClient.cpp ~8824), so the identity
  sweep is not it, and the events are filtered before they become items.
  The leading explanation is the one the maintainer found for the share
  blur: the machine is at 100% CPU during a call, and a page costs ~100-250
  ms of GUI-thread ingest and delegate instantiation that then stretches.
  Needs the stall trace DURING a call, plus the CPU figure at the time.
  For anything that remains, the capture recipe:

      LIGHTNING_GUI_STALL_TRACE=100 QT_LOGGING_RULES="lightning.media.trace=true" \
        scripts/run-dev.sh --log-file /tmp/lightning-roomload.log

  then open three rooms, one media-heavy, and read the stall lines (each
  names a category: `image-decode`, `timeline-diff`, `row-reveal`,
  `timeline-reset`, `rust-poll-drain`) and the `media … cache=` lines.
  **2026-09-18, MEASURED ON THE CURRENT BUILD (`6077d13`) — it does not
  reproduce on the fixture account, and the one slow open in the same log is
  fully diagnosed.** Five rooms opened on the laptop rig, timed from the
  `timeline open` line to the last `pagination completed` before the next
  open (`scripts/room-open-latency.py`, added in the same round so the
  measurement can be repeated rather than re-invented): **262 ms / 550 ms / 571 ms / 942 ms / 1255 ms**,
  1-4 pages each, including both call rooms and the `Call Churn 0915` churn
  fixture. Every walk ended `reached_start= true`, which is why they are
  cheap: a small room runs out of history before the budget runs out.

  The same log holds **one 13.7 s open over 17 pages that added 25 rows**,
  earlier the same day on the build the laptop was carrying before this
  round, and its page-by-page shape is the mechanism, not a guess:

  | page | `nextBatch` | events the filter saw | dropped as RTC | rows added | cost |
  |---|---|---|---|---|---|
  | 1 | 20 | 27 | 21 | 3 | 0.3 s |
  | 2 | 20 | 20 | 20 | 0 | 0.2 s |
  | 3 | 60 | 20 | 20 | 0 | 0.3 s |
  | 4 | 180 | 60 | 60 | 0 | 0.7 s |
  | 5 | 180 | 181 | 181 | 0 | 1.0 s |
  | 6 | 180 | 179 | 179 | 0 | 0.9 s |
  | 7 | 180 | 180 | 180 | 0 | 1.0 s |

  (`filterOffered` in the log is CUMULATIVE for the open — 27, 47, 67, 127,
  308, 487, 667 — so the per-page column above is its delta. Do not read the
  raw number as a page size.)

  So the cost is not the filter and not the ingest: **the SDK escalates
  `nextBatch` 20 → 60 → 180, and in a room whose history is dense MatrixRTC
  churn each 180-event page costs about a second while adding no rows.** The
  2026-09-16 fix that gave filtered pages their own 60-page budget is what
  keeps it walking — correctly, since the alternative it replaced was a room
  asserting its own emptiness — so the remaining question is not "why does it
  walk" but **whether a viewport fill should be bounded in TIME as well as in
  pages**, and show what it has while the rest arrives.

  NOT REPRODUCED on a real account with real history; this fixture account's
  rooms are small, and every measurement above reached the start of the room.
- **A received share blurs for a moment, then fast-forwards to the present,
  at irregular one-to-five-minute intervals (0.9.0 AppImage, 2026-09-05).**
  A full call log showed the engine's frame counters FLAT — `dropped= 0`,
  no key gaps — through every incident, so the receive path above RTP is
  healthy and the event lives below it: packet loss with a keyframe
  request, or the sender's encoder stalling and bursting. Nothing in the
  client runs on that cadence. The instrument exists now:
  `LIGHTNING_CALL_STATS_TRACE=5` logs, every five seconds and per SSRC,
  packets / loss / jitter / kbps / PLI / NACK / FIR for both webrtcbins and
  the remote side's round trip and fraction lost about what we send. Run
  it on BOTH ends of the call, note the wall-clock time of a blur, and read
  the lines around it; a `pli` step with `lost` climbing is the network, a
  `kbps` dip on the sender's outbound with no loss is the encoder.
  **CAUSE IDENTIFIED BY THE MAINTAINER, 2026-09-05 22:22: it happens when the
  SENDING machine's CPU is at 100%.** That is the encoder case above: `vp8enc`
  (software, `deadline=1 threads=4`, ~0.56 cores at 1080p30 per the table in
  the standing warnings) is starved by the load, frames queue and then burst,
  and the receiver — whose counters were flat — sees a stall and a
  fast-forward. Not fixed. Leads, none shipped and none measured: raise the
  encoder's streaming-thread priority from the pipeline's `stream-status`
  bus message (GStreamer creates those threads itself); trade quality for
  time under load (`cpu-used`, a lower rung); a hardware encoder path
  (VA-API / NVENC), which is a packaging question as much as a code one.
  The stats trace shows this shape as a `kbps` dip on the sender's outbound
  with no loss.
- **Full screen opens on the primary monitor**, not the one the app is on.
  Investigated 2026-09-04 (Task A §6) and NOT changed: `placeOnThisApplications
  Screen()`'s arithmetic is already self-consistent in Qt's own space, and its
  comment correctly records that on Wayland Qt passes no `wl_output` to
  `xdg_toplevel.set_fullscreen` (QTBUG-54883), so the compositor chooses. A
  fourth guess would be a fix without a measurement. What WAS wrong and is
  fixed is the sibling bug: a fresh window CENTRED against
  `Screen.desktopAvailableWidth` — the whole VIRTUAL DESKTOP — so on two
  monitors it aimed at the seam, and on this layout landed at x=6490 on a
  6400-wide desktop and opened invisible. `AppController::centredWindowRect`
  centres inside one screen and VALIDATES the result, falling back to platform
  placement; `window-placement` covers it.

**THE WINDOWS CAMERA'S 10-FPS DEFECT IS REPRODUCED LIVE, AND THE CAUSE IS
NAMED: THE SHIPPED PACKAGE HAS NO JPEG ELEMENTS (2026-09-12, Windows 11 guest,
released 0.9.4 portable, the laptop's own USB webcam passed through).**

    camera MJPG chain unavailable, cameras will use the raw entry:
        no element "jpegenc"
    camera chain= raw (jpeg elements absent )
    capture negotiated caps= video/x-raw, format=(string)YUY2,
        width=(int)1920, height=(int)1080, framerate=(fraction)5/1

The camera WORKS — `capture delivered frames count= 5500`, a real published
track — it is simply starved: raw YUY2 at 1080p is ~62 MB/s, far past USB 2.0,
so the device negotiates **5 fps**. Worse than the 720p@10 this item was
recorded at, because the guest's camera offers 1080p.

**THIS SETTLES THE OPEN OPERATOR STEP, AND THE ANSWER IS THAT IT NEVER
HAPPENED.** §16 records the packaging half as done on 2026-09-02 —
`libgstjpeg.dll` added to BOTH Windows lists — and immediately beside it:
"THE WINDOWS BUILDER IMAGE IS BUILT BY HAND UNDER A FIXED TAG, AND A
DOCKERFILE CHANGE ALONE CHANGES NOTHING… OPEN OPERATOR STEP: rebuild the image
under a new tag." The app asking for `jpegenc` and being told no element
exists is that step's absence, measured on the artifact rather than argued
from the Dockerfile.

So the app half of this defect can now be worked on with a rig that can SEE
it, which it never could before — but it cannot be FIXED until the builder
image is rebuilt, because the element the fix needs is not in the package.

Note what the app did right: it probed for the MJPG chain, found no `jpegenc`,
said so by name, and fell back to the raw entry rather than failing. That is
the documented "build the camera chain for image/jpeg explicitly and FALL BACK
to today's raw chain" design working as intended — the fallback is correct and
the missing element is the defect.

ALSO MEASURED IN THE SAME SESSION, and NOT a defect: `screen share falling
back to the CPU: the GL chain is present but cannot run on this machine`.
§16 says `gpuShareChainUsable()` "has never been observed declining" — this is
the first observation, and it declined correctly and said why, on a guest with
no GPU. The maintainer confirms the GPU share path works on real Windows
hardware, so this is the pre-flight doing its job, not a regression.

**WINDOWS FALLS BACK TO THE SOFTWARE RENDERER WHEREVER THERE IS NO GL DRIVER,
BECAUSE `opengl32sw.dll` IS NOT IN THE PACKAGE (measured 2026-09-12 on a real
Windows 11 guest, released 0.9.4 portable).** From the app's own log:

    Failed to load opengl32sw (The specified module could not be found.)
    Failed to load and resolve WGL/OpenGL functions
    lightning: no usable OpenGL context on the "windows" platform - falling
    back to the software renderer. Video and screen sharing will be slower.

`opengl32sw.dll` is **Qt's own bundled software OpenGL** (Mesa llvmpipe), which
Qt ships precisely so an application keeps a working GL implementation on a
machine whose driver is missing or unusable. It appears **nowhere in
`packaging-ci/`** — `grep -rn opengl32sw` returns nothing. The `opengl32.dll`
in `stage-windows-runtime.py:29` is the SYSTEM_DLLS exclude list, i.e. the
real driver-backed one Windows provides, correctly not bundled; the software
twin is a different file and was never staged.

So the fallback chain loses its middle rung. With `opengl32sw.dll` present Qt
would use a software GL implementation and Qt Quick would still run its OpenGL
scene graph; without it the probe fails outright and the app drops to
`QSGRendererInterface::Software`, Qt Quick's own rasteriser.

**AND "MATERIALLY SLOWER" IS WRONG. RE-RANKED 2026-09-12: THIS IS A
CALL-BREAKING DEFECT.** On the software renderer, call and screen-share video
is not slow — **it is not drawn at all**. Measured twice, and the two agree:

* LIVE, on the Windows guest: winA received 1000+ video frames from a Linux
  screen share, `frames in the clear in ... video= true count= 1000` climbing,
  and the tile on screen was an EMPTY bordered rectangle. Maximising the
  window ruled out clipping.
* LIVE, on Linux, where the backend could be changed as the ONLY variable:
  one client, one call, `QT_QUICK_BACKEND=software` — the counter passed 500
  against an empty rectangle, where the same client on the default backend
  rendered the remote desktop perfectly.

The mechanism is in Qt's own headers. `QSGSoftwareRenderableNode::NodeType`
is a closed list — `SimpleRect, SimpleTexture, Image, Painter, Rectangle,
Glyph, NinePatch, SimpleRectangle, SimpleImage, SpriteNode, RenderNode` — and
Qt Multimedia's `QSGVideoNode` is a `QSGGeometryNode` with a
`QSGVideoMaterial`, which is none of them and is not a `QSGRenderNode` either.
It therefore never gets a renderable node and is never painted, while every
type on that list IS the tile's chrome. The path is RHI-only besides
(`QQuickVideoOutput::initRhiForSink`, `QSGVideoMaterial(..., QRhi*)`), and the
software adaptation has no RHI at all.

So on any Windows host with no usable GL a user joins a call, is heard, hears
everyone, and sees blank rectangles — with the app's own warning telling them
it will merely be "slower". That wording is corrected in `src/main.cpp`.

**WHO THIS BITES**, stated carefully: a Windows machine with a working GPU
driver is unaffected — it gets hardware GL and never reaches this branch. It
bites VMs, RDP sessions, servers, and machines whose driver is missing or
broken. This guest is a VM with no GPU, which is why it showed up here and has
never shown up on a developer's desktop.

**POSSIBLY RELATED, NOT ESTABLISHED:** the open "Lightning is sometimes
bouncing up to 9999 fps" report is from Windows, and a software-rendered Qt
Quick window does not present the way a vsynced GL one does. That is a
HYPOTHESIS. The `lightning: scene graph backend=…` line added in `74ddf08`
names the backend and the panel refresh rate in one line and is the instrument
for it — but it postdates 0.9.4, so it is not in any released Windows build
yet. The next Windows package carries it.

FIX: stage `opengl32sw.dll` alongside the Qt DLLs in
`packaging-ci/scripts/stage-windows-runtime.py`, and assert it, since its
absence is invisible until a host with no driver meets it — the recorded
"graceful fallback and silent absence are the same observable" shape, now for
the sixth time.

**SUPERSEDED 2026-09-12 ON WINDOWS — AND THE ANSWER WAS NOT PACKAGING AT ALL.**
This entry used to end "FIX: stage `opengl32sw.dll`". That is no longer the
outstanding work, and two measurements moved it:

* the builder image was inspected and has NO `opengl32sw.dll`, and never will —
  Fedora's mingw Qt does not ship Qt's Mesa-llvmpipe twin, so there was no list
  to edit;
* and the fallback was OURS. `src/main.cpp`'s probe asks about OPENGL, which is
  right for the AppImage/Wayland case it was written for, and then forced
  `QSGRendererInterface::Software` on EVERY platform. Qt DOCUMENTS Windows'
  default as
  **Direct3D 11** — no OpenGL, WARP when there is no GPU, RHI-backed so
  `QSGVideoNode` renders normally. A Windows box with no WGL was already fine,
  and this code overrode that with the one backend that cannot draw video.

The probe now prefers D3D11 on Windows and Metal on macOS; Software stays the
last resort only where there is no such backend. Verified in the SHIPPED Qt
rather than assumed: `Qt6Gui.dll` in the builder image carries
`QD3D11SwapChain`/`QD3D11Adapter` and imports `d3d11.dll` and `dxgi.dll`, and
`Direct3D11` is an unconditional enumerator in that Qt's
`qsgrendererinterface.h`.

**LIVE-VALIDATED PASS, 2026-09-12 afternoon, and the evidence is a PICTURE.**
On the same Windows 11 guest that counted 1000+ received frames against an
empty rectangle, running the D3D11 build (`lightning: scene graph
backend=d3d11 software=0 platform=windows`), the receiving tile carries the
sending machine's live desktop — the room list, the call bar, the clock, and
the mirror recursion only a live feed produces. Same share, same counters, the
renderer the only variable. The app starts, so the D3D11 creation-failure
worry did not materialise on this guest. Sender was the laptop's Linux client;
the guest holds ONE portable install whose store travels with the exe folder,
so it cannot host two accounts.

**ONE PREMISE IS STILL UNMEASURED.** Whether D3D11 is ALREADY Qt's Windows
default decides whether this restores a shipping-proven configuration or
selects a new one, and the guest cannot answer it — its GL is unusable, so the
probe fires there by construction. It is settled by one line on any Windows box
whose GL works: the probe does not fire, nothing is forced, and `lightning:
scene graph backend=…` reports Qt's own choice. `backend=d3d11` confirms it.
ASK ROKAS for that line from his own Windows machine.

**AND THE GUEST'S CLOCK WAS SEVEN HOURS AHEAD IN UTC** while the earlier
observations on it were made (`Pacific Standard Time` against a UTC hardware
clock). Every `m.call.member` expiry is `created_ts + expires`, so every
membership read as expired and the guest showed no call banner, no Join, no
room-list glyph and a `?` facepile — four things that look like defects and are
not. Fixed with `tzutil /s "UTC"`; the next read went from `participants= 0` to
`participants= 1`. Treat guest call-state observations from before
2026-09-12 13:16 EEST as unreliable, and check `date -u` on the host against a
guest log's own `Z` stamps before believing the next one.

**REJECTED, with the reason, so it is not re-proposed:** "on Windows and macOS
log and call nothing, and inherit Qt's default". That makes behaviour depend on
a default nobody here has measured and which differs between the dev shell's Qt
and the Windows SDK's — the exact class §16 records five times over (Qt 6.8 vs
6.11 proxy `roleNames`, the font fallback, `pipewiresrc min-buffers`, the
GStreamer split, the inline `QPointer`), whose own generalisation is PIN IT.
Inheriting a default is not pinning. It also removes no risk: if the default IS
D3D11 you get the same device creation and the same absence of cross-backend
fallback, with no log line saying what was chosen; and if it is NOT, a failed GL
probe plus no override means Qt tries a backend just proven dead.

ACCEPTED FOLLOW-UP, deliberately not folded into the same change: the GL probe
is the wrong question on Windows and macOS and should eventually be SKIPPED
there outright, so each platform has one pinned configuration rather than two
(GL works -> Qt's unpinned default; GL dead -> pinned D3D11), and so a throwaway
`QOpenGLContext` — itself a hang surface on a broken ICD — is never created.
That moves every Windows user off field-proven behaviour, where today's change
touches only machines with no usable GL. Separate round, separate evidence.

**HOW TO READ THE FIRST WINDOWS RUN, so the result is measured and not
interpreted.** A D3D11 failure looks like `Failed to create RHI (backend …)`
and a process that exits BEFORE a window — not a blank window; if that appears,
`QSG_RHI_BACKEND=software` confirms the backend is the variable, because the
probe block yields to that env var. And **"it starts" is not the PASS**: the
PASS is a tile carrying a remote picture on the same guest that previously
counted 1000+ received frames against an empty rectangle, with the backend as
the only change.

The software-renderer NOTICE is unaffected and still earns its place: a Linux
AppImage host with no usable GL still lands on Software, and that is what it
explains.

**THE SNAP HAS NEVER WORKED. IT INSTALLS AND CANNOT START (found 2026-09-11,
first execution under a real snapd).**

    $ snap run lightning --version
    /snap/lightning/x1/usr/bin/lightning-matrix: error while loading shared
    libraries: libEGL.so.1: cannot open shared object file

Six libraries are unresolved inside the confinement — `libEGL.so.1`,
`libGLX.so.0`, `libGLdispatch.so.0`, `libX11-xcb.so.1`, `libX11.so.6`,
`libxcb.so.1`. All six are **absent from the snap payload AND from `core24`,
and present on the HOST**, which is exactly why `ldd` unconfined resolves
everything and why CI has always passed. `build-snap.sh` copies the AppImage
AppDir's `usr/` verbatim, and linuxdeploy's excludelist deliberately leaves
those on the host — correct for an AppImage, where the host `/usr/lib` is
visible, and fatal for a strictly confined snap, where it is not.
`/var/lib/snapd/lib/gl` is empty: the `opengl` interface carries vendor
drivers, not the base loader.

**STRUCTURAL, not a 0.9.4 accident.** The AppDir of a newer snapshot
(`317bd39`) was extracted and all six checked: all six ABSENT. Every snap
repacked this way fails identically. Fix is packaging-side — stage those
libraries, or plug a `gnome-*-2404` content snap with
`command-chain: desktop-launch`.

`validate-snap.sh` says in as many words that it cannot reach this class ("a
real `snap install --dangerous` needs a running snapd, which the
Docker-outside-of-Docker runner fleet cannot provide"). Fifth appearance of
§16's *graceful fallback and silent absence are the same observable*.

Two more, visible once the payload is run unconfined:
* **Three interfaces do not auto-connect, and the keyring is one**:
  `password-manager-service`, `audio-record`, `camera`. Without the first
  there is no Secret Service and the access token has nowhere to go — §6
  territory. `snap.yaml` flags `audio-record` in a comment and not the
  keyring. All three connect manually.
* **`--call-media-status` misreports the snap's own GStreamer.** It correctly
  loads the bundled 1.26.2 (the host has only 1.24.2) yet prints
  `bundled plugin directory: <none - using system GStreamer>`, because
  `appImageBundledPluginPath()` (`src/calls/GstBootstrap.h:104`) keys on the
  APPIMAGE AppDir layout. That header's own comment says four packaging
  defects here have turned on exactly which GStreamer was loaded, so this
  line telling the wrong story is not cosmetic.

**THE DEB DOES NOT INSTALL ON UBUNTU, WHICH README CLAIMS IT SUPPORTS (found
2026-09-11).** On Ubuntu 24.04 LTS, both the released 0.9.4 deb and a fresh
one:

    Depends: libqt6core6t64 (>= 6.8.2) but 6.4.2+dfsg-21.1build5 is to be installed
    Depends: libgstreamer-plugins-bad1.0-0 (>= 1.26.2) but 1.24.2-1ubuntu4 …
    Depends: qml6-module-qtquick-effects but it is not installable

`dpkg-shlibdeps` runs on Debian 13 (Qt 6.8.2, GStreamer 1.26.2) and bakes
those floors in; Ubuntu 24.04 is on Qt **6.4.2**, and
`qml6-module-qtquick-effects` does not exist in noble at all because
`QtQuick.Effects` is Qt 6.5+. `README.md:143` advertises
`# Debian, Ubuntu, Mint, Pop!_OS`; 24.04 is the current LTS and Mint 22.x /
Pop!_OS 24.04 derive from it. Either the claim narrows to Debian 13+ /
Ubuntu 25.04+, or the deb needs a second build lane. **On Debian 13 the same
deb is clean** — installs, runs, `opengl software=0`, libsecret ready, no QML
warnings.

**THE APP-ID / WINDOW-ICON DEFECT IS CONFIRMED FOR THE FLATPAK**, by the
app's own `--desktop-status`: `app id (desktop file name): lightning`,
`visible launcher entry: NONE`, `visible icon: NONE`. The flatpak exports
`org.lightning_matrix.Lightning.desktop` while the binary stamps `lightning`.
The deb is the control and resolves both. On X11 it still works (WM_CLASS is
`lightning-matrix` and `StartupWMClass` matches); the WAYLAND half is NOT
TESTED for want of a Wayland guest, but `--desktop-status` classifies it.

**THREE THINGS THE FLATPAK SANDBOX BREAKS THAT THE APPIMAGE DOES NOT:**
* **Spell checking is absent, and the diagnostic blames the wrong thing.**
  `--spell-status` says "this machine has no dictionary Lightning can reach",
  but `/usr/share/hunspell` EXISTS in the sandbox and `libenchant-2*` does
  not. Lightning resolves `libenchant-2.so.2` at runtime through QLibrary
  (`SpellBackend.cpp:352`), so this is `no-library`, not `no-dictionary` —
  and `createPlatformSpellBackend` already returns that distinction
  (`SpellBackend.h:71-76`) while `main.cpp:1813` prints the dictionary
  wording for every failure. Two items: the KDE runtime carries no enchant
  and the manifest does not add it, and the diagnostic misdirects whoever
  tries to fix it.
* **The SNI tray cannot own its bus name.** The Flathub manifest adds
  `org.kde.StatusNotifierWatcher=talk`, but there is no `--own-name`:
  `RequestName org.kde.StatusNotifierItem-99-1` is refused with
  `ServiceUnknown` by xdg-dbus-proxy while a control name in the app's own
  namespace is granted. The talk half is fixed and the own half is not.
  Whether Qt still registers via its unique bus name is NOT TESTED.
* Two **25 s D-Bus stalls** at startup (`portal.Settings.ReadAll` and
  `portal.FileChooser`, both NoReply) that the deb and AppImage never incur.
  The guest runs no portal backend, so a real desktop would answer;
  environment-conditioned, not a proven user-facing defect.

**THE RPM INSTALLS ON FEDORA 44 AND NOTHING ELSE, AND THE README SAYS
"Fedora, RHEL" (found 2026-09-11, on the new Fedora test guest).** Measured,
not argued — `dnf install` of the 0.9.4+git rpm on **Fedora 43**:

    nothing provides libQt6Core.so.6(Qt_6.11)(64bit)
    nothing provides libQt6Qml.so.6(Qt_6.11_PRIVATE_API)(64bit)

Fedora 43 ships **qt6-qtbase 6.10.3**. The rpm is built AND validated on
`fedora:44` (`.gitlab-ci.yml`, both `build-rpm` and `validate-rpm` pin the same
digest), which carries Qt 6.11.1 — a deliberate choice, documented in
`packaging-ci/docs/windows-packaging.md`. Three separate things follow and
only the first is obvious:

* **Fedora supports N and N-1.** 43 is current-minus-one and fully supported,
  and the rpm cannot be installed on it at all. `README.md:144` advertises the
  rpm for "Fedora, RHEL" with no version qualifier.
* **"RHEL" is almost certainly false** for any shipping RHEL, which is far
  behind Qt 6.11. Unverified here — no RHEL guest — but it should be checked
  before the claim is repeated.
* **`Qt_6.11_PRIVATE_API` is an ABI trap independent of the above.** Qt's
  private API carries no ABI guarantee, so that dependency pins the package to
  an exact Qt MINOR: Fedora 44 shipping Qt 6.12 breaks it too. Where the
  private-API link comes from has not been traced.

**VALIDATION CANNOT CATCH THIS AND NEVER WILL, because `validate-rpm` runs on
the SAME pinned image `build-rpm` builds on.** That is the recorded "a probe is
evidence only if it shares the property under test" trap inverted — this probe
shares too much. A package's install test has to run somewhere other than the
machine that built it, which is precisely what the new guests are for.

NOT YET ESTABLISHED: whether the rpm installs cleanly on Fedora 44 itself
(the guest is 43; a 44 guest is the obvious next build), and what the download
page claims.

**NOT TESTED, 2026-09-02 audit:** six items in
`docs/security-audit-2026-09-02.md`.

**2026-09-12: THE WINDOWS TRAY-BALLOON NOTIFICATION PATH IS LIVE-VALIDATED —
PASS**, on the Windows 11 guest, driven by a message from a Linux client while
Lightning was minimised. A real toast appears ("Lightning Matrix client" /
"lightningtest in Design Review" with the room avatar), the app logs
`notification delivered through the tray balloon`, and CLICKING IT raises the
app from minimised and opens that room with the unread divider in place. §16
has carried this path as "NOT LIVE-TESTED on Windows or macOS (needs a
packaged build)" since the day it was written. STILL NOT TESTED on Windows:
read-withdrawal of a balloon (Qt cannot do it; it needs WinRT
`ToastNotificationHistory.Remove`) and the notification SOUND. macOS remains
untested entirely.

**2026-09-12, the sound/shortcuts round — LIVE-VALIDATED on a packaged
AppImage, two real clients, a real homeserver.** Driven on the laptop rig
against `matrix.smetonis.net` with the two throwaway fixture accounts, on the
project-6 pipeline 203 artifact (`0.9.4+git20260912.05008f9`). **That AppImage
bundles Qt 6.8.2** — measured from the running process, not assumed — so this
run also answers the standing worry that nothing new here had ever been
rendered on the Qt every deb/rpm/AppImage user actually has, in the one area
(Quick Controls layout inside a `QQuickMenu`) where this project has been bitten
by the 6.8/6.11 split twice.

PASS, each on evidence the layout cannot fake:

* **The in-call microphone level reaches the audio graph.** `microphone gain
  applied: percent= 0 gst= 0 elements= 1` and `percent= 200 gst= 10
  elements= 1`, emitted only after `g_object_set` succeeded on a real named
  `volume` element. This is the round's headline claim and the exact shape of
  the 2026-09-11 claim that had to be withdrawn ("the slider read 200% and
  nothing had reached the audio graph"); the readout is computed from the
  slider's own value and is NOT evidence, which is why the instrument was
  added first. `scripts/gui-suite-calls.sh micgain` is the tracked form.
* **A `Slider` inside a `QQuickMenu` can be dragged** — the menu's ListView
  did not steal it. Driven drags only; a slow or deliberately diagonal HUMAN
  drag is still unexercised.
* **The menu's whole design appears at 200%**: the `mic` glyph swaps to
  `graphic_eq`, the readout tracks, the neutral mark sits at 100, the "Above
  100% amplifies and can clip" warning appears, and the "Reset input volume"
  row appears — all of which are gated on being off neutral.
* **Reset input volume** applies 100 to the engine (`percent= 100 gst= 1
  elements= 1`) and writes 100 to disk.
* **BOTH SURFACES WRITE, independently.** Dragging the SETTINGS slider during
  a live call logs `microphone gain applied: percent= 151 gst= 5.59
  elements= 1` and stores 151, and reopening the in-call chevron menu then
  reads 151%. The reviewer's warning applies and was answered: two sliders
  agreeing proves only that they READ the same value, because Qt does not
  break a QML binding on a C++-side `setValue` — so each surface was driven
  separately and checked against the engine line and the disk, not against
  the other slider.
* **Ctrl+Shift+C is INERT during a live call**, pressed in a different
  RTC-capable room: no `call start requested` line of any kind, and the call
  kept running (17500 -> 19000 frames out). WITH A POSITIVE CONTROL, because
  a dead key and a correctly-gated key look identical — the same key with no
  call running logged `call start requested lane= "matrixrtc"` and
  `sfu joined`. That is the H1 defect's live regression check.
* **Ctrl+Shift+Y leaves the call** (`teardown state= 6`, `membership
  retracted attempts= 1`).
* **Ctrl+Shift+S opens the portal picker and publishes NOTHING** — the portal
  window appeared and no `screen share publishing` line followed. That is the
  privacy-critical half of that key.
* **Ctrl+Shift+A marks every conversation read** and **Ctrl+Shift+R marks the
  open one unread**, both against the real backend with a real unread state
  created by the other client. The mock backend cannot answer either — it does
  not clear unread badges even from its OWN context menu, which is the control
  that proved the fixture rather than the feature was the limitation.
* Ctrl+Shift+T (DM dialog) and Ctrl+Shift+I (Activity Center), the Sound &
  video section, the media-playback and microphone sliders writing to disk,
  the devices leaving Notifications, and the quick switcher reaching the new
  sections: PASS on an isolated Xvfb display (`DISPLAY=:99` + xdotool, which
  cannot reach the maintainer's session).

STILL NOT TESTED: **audibility** — nobody listened, and what is proven is that
the value reaches a real GStreamer `volume` element, exactly the split the
per-participant volume carries; a slow or diagonal human drag on the menu
slider; restart with a call; the PiP window, which declares none of the call keys and
so should be dead for all of them; media playback volume against a real video;
and Windows/macOS, where `requestScreenShare()` takes a different code path
this key has never exercised. Plan, with what would make each look like a pass
while broken: vault note "Lightning/Testing/Sound and shortcuts round — live
test plan".

**THE ACCOUNT SWITCH IS LIVE-VALIDATED — PASS, and it closed one of the
review's follow-ups by finding the UI said the wrong thing.** Driven inside
ONE client on the laptop rig, which has both fixture accounts saved: the
microphone level read 151% for `lightningtest`, 60% for `lightningtest2`, and
151% again on switching back — while the GLOBAL fallback key held 60 the whole
time, so the UI is reading the account key and not the fallback. A fresh
account inherits the last value rather than snapping to a default, which is
`appearanceValue`'s design and is what a user would want. Set different values
on each account first: the reviewer's warning that two untouched accounts both
read 100 and make the scoping invisible is exactly right.

That run also showed the section's own explainer was wrong. "These devices
belong to this computer, not to your account" sat directly under the
microphone LEVEL, which is account-scoped — so the one sentence telling anyone
which of the two they are changing was backwards about one of them. Fixed, and
pinned from both ends by
`theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive`.

ACCEPTED FOLLOW-UPS from that round's review, none blocking: the in-call level
slider cannot be reached by keyboard (`QQuickMenu` arrow navigation visits
menu ITEMS and it sits in a plain `Item`), so not by a screen reader either;
the menu changes height mid-drag when the value crosses 100%; `call.returnToCall`
= Ctrl+Alt+A is Polish AltGr+ą and macOS ⌘⌥A and stays because moving a
shipped default silently rebinds it for every install; and Ctrl+Shift+A
shadows `QKeySequence::Deselect` in every X11 text field.

**2026-09-04, local search and widgets.** Both are LIVE-VALIDATED against
`matrix.smetonis.net` with throwaway fixture accounts (credentials in the
vault, `Lightning/Testing/Test Accounts.md`, never in this repository):
local search finds a Megolm-encrypted message this client sent, and the widget
list returns one openable widget plus three refused with the right reason and
excludes the tombstone.

**The widget QML is now LIVE-VALIDATED TOO, on a packaged AppImage** (later the
same day, 0.8.4+git 6989623, driven through the GUI against
`matrix.smetonis.net`). Four real `im.vector.modular.widgets` state events were
written to a fixture room: the https one lists with an ENABLED Open, the
`http://` and `javascript:` ones list with a DISABLED Open and the not-HTTPS
refusal, and the one with no `url` is dropped from the list entirely. The
consent sheet has been seen: it names the widget, its URL and who added it,
says the site receives the user's IP, and states that Lightning opens widgets
in the browser so the page cannot reach the account, keys or messages. One
inaccuracy worth fixing eventually and not a defect: a `javascript:` URL is
refused with the not-HTTPS wording rather than a scheme-specific one.

SUPERSEDED 2026-09-10 — kept only to say so, because it sent a later round
looking at the wrong surface: this paragraph used to read "STILL NOT SEEN: the
find bar's source strip and coverage line … local search remains validated at
the Rust layer only." Local search HAS since been driven from the GUI (it is
the find bar's History scope, Ctrl+F — not the room-header magnifier, which
opens the SERVER-side panel) and its coverage line was read off the screen.
See the 2026-09-10 live-validation entry above.

Keyboard automation now works — a `ydotool` uinput device plus KWin scripting
for closed-loop pointer positioning, with a focus guard that refuses to type
unless KWin reports the intended window active. The guard exists because
without it a login went into a browser window instead of the client.

**2026-09-05, the tester-report batch, driven through the GUI on the packaged
tree's binary against `test_matrix.smetonis.net` with the fixture account.**
PASS: the room-info tab strip wraps to two rows at the 260 px floor with the
panel inside the window (it used to be pushed off the right edge — the strip's
natural width was the panel's MINIMUM); the Widgets tab offers Add widget…,
the dialog writes a real `im.vector.modular.widgets` event and the list
re-reads with Open + Remove, and Remove writes the tombstone and the list
empties; Lightning Light's search field and composer measure `#F5F9FE` and
Moss Light's `#F6FBF7` (sampled off the capture, no pure white left); the
Settings header is the room header's height; Settings opened with NO GUI
stall ≥100 ms on any of three opens (`LIGHTNING_GUI_STALL_TRACE=100`) where
the same trace showed 428 ms per open before the loader was kept alive; the
GIF picker keeps a typed query across a provider switch. NOT TESTED: the Home
"Set up backup" banner — the fixture account has a backup, so the banner does
not appear; the edit is text and target only. MEASURED AND NOT ADDRESSED: a
theme switch blocks the GUI thread ~940 ms (every token repaints), and the
FIRST Settings close after the kept-alive change logged one 226 ms
unattributed stall that the second and third did not — one observation.
Harness lesson that cost a click batch: a fresh launch came up on the OTHER
monitor and six guarded-nothing clicks went into Steam; every click now goes
through `winclick`, which asks KWin for Lightning's frame and refuses unless
Lightning is the active window. And `QT_FORCE_STDERR_LOGGING=1` is mandatory
for a nohup'd launch, or Qt hands every line to journald and the log file
holds only the nix banner.

**2026-09-05, later the same day — the second report batch, NOT live-validated
(Rokas was at the PC, so no GUI automation; see the harness lesson above).**
Fixed from his screenshots and log, each with a regression test: the
encryption lock beside the room name (`TesterReportFixesTest`); read receipts
hosted by the nearest row that draws a body, so a marker on a
call-membership update no longer vanishes (`timeline-model-diff`); the call
popout hosting the stage's tile grid with shares as their own tiles (load
gate only); a participant volume chosen before their track arrives applied
when the bin is built, and the "nowhere to land" line logged once per key
instead of hundreds of times (`sfu-media-engine`, mutation-proven). The
Windows "volume 0 does not mute" report is the likely same defect and stays
OPEN until a Windows tester confirms. Also from review of the first batch
(CHANGES_REQUESTED, all addressed): the kept-alive Settings screen's
window-level Escape/Ctrl+, Shortcuts are gated on `root.visible` (two
enabled Shortcuts on one sequence make Qt fire NEITHER — proven by a real
key in `SettingsShellQmlTest`), a widget write answered after a room switch
no longer wedges `writing`, `validate_widget_write` is one function the Rust
test actually calls, Remove names the exact state key and is hidden for rows
the reader could not name, and Lightning Light's elevated rung was stepped
to `#E4EEFA` (the tint had collapsed it to 1.017:1).

Two open decisions those rounds created, both recorded rather than taken:
- **The SDK store is not encrypted at rest**, and it holds DECRYPTED
  encrypted-room bodies (`encode_event` serializes the `Decrypted` variant,
  `encode_value` is a no-op with no cypher, Lightning opens
  `sqlite_store(path, None)`). §6's memory-only rule binds Lightning's own
  `CacheStore` and is intact; the property it was protecting is not held by
  the installation. The search index is a second copy of what is already
  there. The audit's route to fixing it is in
  `docs/security-audit-2026-09-02.md`; a naive passphrase bricks every install.
- **Widgets are LISTED and opened externally, never embedded**
  (`docs/widgets.md`). Revisiting that needs a Windows Qt story with WebEngine,
  a Flatpak answer that is not "disable the sandbox", and a decision about
  `QtWebEngineQuick::initialize()` forcing the whole scenegraph to OpenGL.

STILL UNPROVEN, not reported broken:

- **A fresh `QSG_RENDER_TIMING` capture** proving the row window does anything
  in production (`winApplies` > 0, `rows` ≪ `srcRows`). No production frame-cost
  improvement has ever been observed from it.
- **The still-unreproduced freeze after hammering reactions** — hand over a
  build with `LIGHTNING_GUI_STALL_TRACE` enabled. One capture beats three
  theories.
- **GIF-favorite reopen crash** — still only `1502e6b`'s commit message as
  evidence; seven headless scenario families including an ASan build found
  nothing. Needs a real `coredumpctl`/`gdb` backtrace.
- **The Rust `children` payload against a real homeserver** — that a real
  `m.space.child` order arrives in the order its admin set. Adjacent to item 2
  above but not the same claim.
- **`app.` dereferences in creation-time bindings of other `Repeater`
  delegates** (`qml/EmojiPicker.qml`, `qml/SettingsScreen.qml` theme cards) —
  structurally exposed to the poisoned-context-lookup defect fixed in
  `30ee39b`, not observed failing.
- Continue GIF playback, cancellation, resource, cache and malformed-media
  hardening.

**Accepted follow-ups, none blocking:** decide whether a client-side
sanity ceiling should apply when the server advertises no upload limit
(deliberately absent — see §7); `setVoiceRecorderForTest` would be
better taking a `unique_ptr`; `voice_info` computes `info.size` from the
stat size rather than the uploaded bytes; `FakeRecorder` is a PARTIAL
double (`stop()`/`durationMs()` are not virtual, so anyone needing
`stop() → ready()` must extend the seam consciously); the pre-existing
invite/verification notification bodies carry unescaped member-chosen
text.

"Recovering never-backed-up Megolm keys" is **refused, not deferred**: a
key that was never backed up and never shared exists nowhere, every
legitimate recovery path is already implemented, and anything further
would weaken E2EE.

Do not list the implemented GIF browser, favorites/recents, download and
send path, provider networking, thread summaries/attachments,
notification sounds, or E2EE generation isolation as unfinished. Do not
turn possible future ideas into commitments.

---

**THE SNAP STARTS NOW AND ITS GUI DID NOT, AND BOTH HALVES ARE MEASURED
(2026-09-12, a real `snap install --dangerous` in an Ubuntu 24.04 guest under
snapd 2.76.3 — the only place either question can be answered).**

Artifact: project 6 pipeline 198's `build-snap`,
`lightning_0.9.4+git20260911.84a3294_amd64.snap`.

PASS, and it is new: the snap installs and RUNS. `lightning --version` answers
`Lightning 0.9.4`; `--call-media-status` reports the media engine built in,
GStreamer 1.26.2 and both call engines available. `84a3294`'s excludelist-family
staging is what fixed the never-starts defect this file used to record.

FAIL on the shipped artifact: **it cannot open a window.**

    cannot load: /snap/lightning/x2/usr/plugins/platforms/libqxcb.so:
    libSM.so.6: cannot open shared object file: No such file or directory
    qt.qpa.plugin: Could not load the Qt platform plugin "xcb" ... even
    though it was found.

`libSM.so.6` (X session management) and its own dependency `libICE.so.6` were
not staged. Qt's advice on that failure path names `xcb-cursor0`, which IS in
the payload — so the message points at the wrong library and cost a detour.

PROVEN, not predicted: unsquashing the snap, copying the guest's `libSM.so.6`
and `libICE.so.6` into `usr/lib`, repacking and installing gives a **rendered
sign-in window**. Those two libraries were the whole distance between the
shipped snap and a working GUI.

WHY NOTHING CAUGHT IT, and this is the reusable part. A Qt platform plugin is
**dlopened**, so its dependencies are not the binary's: `build-snap.sh`'s
"nothing unresolved" guard asked the BINARY, which was perfectly satisfied.
And no CI job can reach it either — `validate-snap.sh` runs with
`QT_QPA_PLATFORM=offscreen`, which never loads xcb, so a snap whose windowing
is entirely broken passes every check and installs cleanly. Fourth appearance
of "a library loads its own plugins" in this project.

FIXED in `packaging-ci/scripts/build-snap.sh`: `libSM.so.*|libICE.so.*` join
the staged families; `libSM.so.6`/`libICE.so.6` join the named assertion; the
staging PROBE set widens from the two platform-plugin directories to every
`usr/plugins` and `usr/lib/gstreamer-1.0` plugin; and a new guard runs `ldd`
over every plugin against the payload — fatal for a Qt plugin, fatal for
`libgstopengl.so`/`libgstwebrtc.so`, a warning otherwise. **The fixed build has
NOT been run** (it needs a pipeline), so the guard itself is NOT TESTED.

STILL MISSING in the same run, and found only because the window came up:

    Failed to load plugin '.../libgstalsa.so':   libasound.so.2: ...
    Failed to load plugin '.../libgstopengl.so': libGL.so.1: ...

`libGL.so.1` is why that snap logged `no usable OpenGL context on the "xcb"
platform - falling back to the software renderer` — which, per the opengl32sw
entry above, means **the snap would show no call video at all**. Both families
were already in the staged list and neither was staged, because the probe set
did not include the plugins that load them; the widened probe set above is
aimed squarely at this.

NOT TESTED on the snap, and not reachable without a display in the guest that
is more than Xvfb: Wayland (the payload still bundles `libwayland-client.so.0`,
which the AppImage deliberately DELETES over GitHub issue #9 — under
confinement the calculus differs, but it has never been exercised), GPU
acceleration, audio, and any call.


---

**ACCEPTED FOLLOW-UP, not done: the camera-portal state machine has no
coverage, and the one suite that could reach it is built without the media
engine.** Raised in review 2026-09-12 and recorded rather than fixed, because
restructuring call code deserves a round where it can be iterated on.

`SfuCallController::startCameraCapture()` and `publishCameraTrack()` have their
entire bodies inside `#ifdef HAVE_LIGHTNING_WEBRTC`. `call-controller-test` is
the only target that constructs a `SfuCallController`, and it links
`Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test` only — so it never receives that macro
and `startCameraCapture()` is an empty function there. `m_cameraAwaitingPortal`
can therefore never become true in a test, and the four camera cases added with
`21f4a1a` can only call the pure `linuxCameraRoute` predicate, which is exactly
what they do.

Untested as a result, and these are the paths that leak a file descriptor or
wedge the camera: the three fd-ownership exits in the `ready` lambda, the
leave-during-dialog unwind, the second-press unwind, and the call-end cancel.
The lambdas themselves are already OUTSIDE the guard, along with
`setCameraPortal()` and `abandonPendingCamera()`, so a test could wire a real
`CameraPortal` and emit `ready`/`cancelled`/`failed` today — only those two
guarded bodies stand in the way.

THE SHAPE IS ALREADY IN THIS FILE'S NEIGHBOUR: `SfuCallController.h` keeps
`m_engineParticipantVolume`/`m_engineShareVolume` shadow records outside the
media guard, with a comment saying why, and that is what makes
`aStoredVolumeReachesTheEngineWhenTheStreamIdArrivesLate` a real test. Mirror
it — a `cameraAwaitingPortalForTest()` and a publish counter kept outside the
guard — and the state machine becomes drivable without a media engine.

---

**~~`DELAYED_EVENTS_REFUSED` IS PROCESS-GLOBAL~~ — FIXED 2026-09-12, kept for
the reasoning.** It is now a `Mutex<BTreeSet<String>>` keyed by the
homeserver's own URL, so an account switch simply asks a different question.
`one_servers_refusal_does_not_disable_msc4140_for_another` pins it and is
mutation-proven against the old process-global behaviour. What follows is why
it mattered.

`rust/src/rtc.rs` keeps it as a `static AtomicBool`. It latches when a
homeserver proves it will not arm a delayed retraction, and that is a real,
permanent property OF THAT SERVER — for one account it is exactly right, and
the one-way behaviour is deliberate (once latched, the gate stops arming, so
nothing calls `schedule_delayed_leave` again and the clearing write is
effectively unreachable).

Across accounts it is wrong. Sign in to an account on a Synapse that ignores
`?org.matrix.msc4140.delay=`, latch it, then switch to an account on a server
that implements MSC4140, and for the rest of the process the second account:

* gets no server-side retraction, so an unclean exit strands its membership for
  the full expiry — the five-minute ghost participant this round fixed the
  OTHER cause of; and
* is told scheduled send is unsupported (`rust/src/rooms.rs` reads the same
  flag), so it silently falls back to the local queue.

FIXED exactly that way. The two recoveries the old doc comment claimed — a
server that GAINS support, and a mis-latch — are now genuinely reachable,
because the entry that would have to be cleared belongs to one server.

---

**TWO UI CLIPPING ITEMS THE 2026-09-12 WINDOWS SWEEP LEFT OPEN.** The sweep
itself came back mostly clean — room list rows, the composer (its `…` overflow
at 640 px is a designed adaptation, not a break), message rows with long
unbroken tokens, link-preview and attachment cards, the room-info tab strip at
~313 px, and Settings all PASS at 640 / ~1100 / 1280. These two did not.

**1. The last facepile avatar is sliced, at every width including maximised.**
Settled as a CLIP, not the control pill painting over it: a 900% crop shows a
straight vertical cut with pale-green header background beyond it, while the
pill's rounded edge starts well to the right with a clear gap. An opaque pill
would have cut along its own arc and left white.

Owner: `qml/CallSpeakerBubbles.qml`'s `clip: true` on the `anchors.fill: parent`
ListView. Its host, `callHeaderBubblesHost` in `qml/CallStage.qml`, has
`Layout.fillWidth: root.collapsed` — false while expanded — and, unlike the
controls Loader, no `Layout.minimumWidth` floor at all, so it is squeezed below
its natural width. That it fires at 1280 maximised is independent evidence the
header row was over-full even at full width.

**ROOT-CAUSED AND FIXED, 2026-09-12, and this diagnosis was wrong.** The host
was never the problem and neither was the control overflow. The owner is
`CallSpeakerBubbles.qml`'s own `implicitWidth`, which said
`N * (bubbleSize + spacing6)` while the delegate's cell is `bubbleSize + 8` —
the width the SPEAKING RING needs, which the delegate's comment records
widening it to. So with two people the strip asked for 80 px, drew 90, and its
own `clip: true` cut ~10 px off the last avatar. Measured, not argued: the
regression case reports "the strip asks for 80 px and draws 90 px" on the old
formula. The cell is one `cellWidth` property now, read by both.

The instinct NOT to add a floor to the host was right, for the wrong reason —
a floor would have masked an arithmetic error with extra space. What hid it
from every existing test is that the COLLAPSED header fills the strip's width
from its host, so the defect only appears on the spotlight branch where the
strip takes its own implicit width.

**LIVE-VALIDATED PASS** on the pipeline-204 AppImage
(`0.9.4+git20260912.8943ae9`), two clients in a real call with a share running
— which is what puts the strip in the header at all, since the facepile is
absent without a spotlight. Before and after captures at the same window size
and the same crop: the second avatar's right side is cut flat on the old build
and is a complete circle on the new one.

**3. The floating call window's hang-up button did nothing on a 1:1 call, and
its keyboard is still dead.** `qml/CallPipWindow.qml` called
`app.calls.hangUp()` where the invokable is `hangup()`; every other call site
in the tree spells it correctly. On the LEGACY lane the button threw a
TypeError, so the one surface that exists for a minimised window could not
leave the call. FIXED, with a sweep that now checks every `app.calls.X` and
`app.groupCall.X` in `qml/` against the two headers' actual members, because a
QML→C++ name is only checked when the line RUNS and nothing here had ever run
that handler.

STILL OPEN, and an accepted follow-up rather than a defect: `CallPipWindow`
declares exactly ONE `Shortcut` (Escape, for the share-fill mode), and a QML
`Shortcut` is matched by WINDOW — so with the floating window focused, mute,
deafen, camera, leave and screen-share are all dead keys. Its pointer controls
cover every one of those actions, and three of the five predate the 2026-09-12
round. Worth closing when someone can drive the PiP in a test; it is the
window a keyboard user is most likely to be looking at, since the main one is
minimised by definition.

**2. The room-header title elides with room to spare, and the obvious
candidate is REFUTED.** At 640 px: header ~338 px, so `header.width * 0.5`
allows ~169 px; the rendered title ink is ~55 px, followed by ~60 px of EMPTY
space before the icon cluster. So `qml/TimelinePane.qml`'s 50%-of-header cap is
not the binding constraint — the title elides at roughly a third of what the
cap allows — and a layout squeeze cannot leave 60 px unused beside the thing it
squeezed.

**THE HYPOTHESIS IS REFUTED, 2026-09-12, on the real component.** It was that
`Math.ceil(implicitWidth)` reflects the ELIDED content once eliding is active,
making `Layout.maximumWidth` self-referential and ratcheting the width down.
If that were true the title could not recover without something resetting it,
and it does: driven on the laptop rig with "Lightning search fixture" open, the
title elides to "Light…" at a 640 px window and renders IN FULL at both 1200
and 1707 — no reset, no reload, just a resize. A self-referential bound cannot
un-ratchet. `Math.ceil(implicitWidth)` is not the cause and that line should be
left alone.

**And the symptom did not reproduce either.** At 640 px the gap between the
elided title and the first header icon measured ~17 logical px, not the ~60 px
the original sweep recorded — a normal layout gap. The title ink matched (~54
px). The likeliest explanation for the difference is the ICON COUNT: the
original was measured on the Windows guest in an ENCRYPTED room, which carries
the padlock this one does not. So what is left of this item is "the title
elides at a 640 px window", which is a genuine squeeze — the header has about
277 logical px for an avatar, a title and four icons at that size — and not
obviously a defect. Re-open it only with a capture showing real empty space
beside an elided title, and say what the icon cluster held.

NOT TESTED in that sweep and worth covering: code blocks and wide tables (the
one attempt was void — shell quoting truncated the message before it reached
the composer, and the "clipped code block" it appeared to show was simply the
message the app had received), and the call stage's tile grid, which cannot be
judged on a machine that renders no video.

---

**THE SNAP'S STAGED-LIBRARY LIST AND ITS JOB'S APT LIST HAVE TO AGREE, AND
THEY DID NOT (2026-09-12).** A follow-up to the libSM fix, and it would have
turned that fix into a red build rather than a working snap.

`stage_unresolved_libs` copies from what `ldd` RESOLVES ON THE BUILD HOST. So a
family named in `build-snap.sh` that is not installed in the `build-snap` job's
image stages **nothing** — and the payload guard added in the same round then
fails the build. Loud rather than silent, which is the right failure, but still
a failure. `libsm6`/`libice6` were not in that job's apt list.

Found by simulating the staging against pipeline 198's real payload rather than
by reading: copy the named libraries into an unsquashed copy, re-run the guard,
and see what the NEXT layer asks for. That surfaced one more, and it is the
kind nobody guesses — **`libSM` needs `libuuid`**, so `libqxcb.so` would STILL
have failed to load with libSM and libICE both present.

Now in the job's apt list: `libsm6 libice6 libuuid1`, plus `libasound2t64`,
`libdrm2`, `libgbm1`, `libwayland-cursor0`, `libwayland-egl1`, `libfribidi0`,
`libthai0` — the set the widened plugin sweep reported once a window opened.
`libuuid.so.*`, `libfribidi.so.*` and `libthai.so.*` join the staged families.

**AND MY FIRST SWEEP OF THAT WAS A PROBE THAT COULD NOT FAIL.** I reported "no
fatal entry" from a tree into which I had hand-copied `libGL.so.1` and
`libgbm.so.1` — the exact two libraries the staging step turns out to be unable
to produce. The probe did not share the property under test, which §16 records
as a standing trap and which I walked into anyway. Caught in review by running
the REAL functions in the job's own pinned image against the PRISTINE payload.

Two things that run found, both since fixed:

* **`ubuntu:24.04` ships no `readelf`**, and the guard walks each plugin's
  NEEDED graph with it. Without `binutils` the walk yields nothing, every probe
  reports nothing missing, and the guard passes VACUOUSLY on a payload whose
  plugins cannot load — the same silent-absence shape it was written to end.
  `binutils` is now in that job's apt list.
* **`ldd` could not see a GStreamer plugin's own dependencies.** Qt's plugins
  carry `RUNPATH=$ORIGIN/../../lib`, so `ldd` walks into the AppDir and reaches
  libSM; a gst plugin carries `$ORIGIN` alone, so the walk stopped at
  `libgstgl-1.0.so.0 => not found` and never reached `libGL`/`libgbm`. Naming
  those families therefore staged NOTHING and `libgstopengl.so` stayed
  unloadable — meaning the snap kept reproducing the very software-renderer
  condition the call UI's new notice exists to explain. Staging now resolves
  with `LD_LIBRARY_PATH="$TREE/usr/lib"`; measured in the job's image against
  pipeline 198's payload, 10 staged before and 12 after, and the guard then
  passes with no fatal entry and no warnings.

Worth keeping beside that: the family list is not self-sufficient, it is
self-sufficient GIVEN THIS AppDir. `libICE` needs `libbsd`→`libmd`, `libthai`
needs `libdatrie`, `libgbm` needs `libexpat`, and all four resolve only because
linuxdeploy already bundled them.

**NOT TESTED: the resulting build.** None of this has run in CI. What the
review's docker runs establish is that the staging and the guard now behave
correctly on the real image against the real payload; a pipeline is still what
proves the job goes green.

---

**THE SOFTWARE-RENDERER NOTICE COVERS THE CALL UI ONLY.** `qml/CallStage.qml`
explains why no video appears during a call; `VideoPlayerCard.qml`,
`VideoViewerOverlay.qml` and `MessageComposerBar.qml`'s preview will still show
an unexplained empty rectangle on the same machine, because they draw video the
same way and nothing tells them either. Pre-existing and deliberately out of
scope for the round that added the notice — recorded so it is not later read as
closed. `app.softwareRenderer` is already available to all three.
