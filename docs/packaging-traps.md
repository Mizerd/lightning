# Packaging toolchains and builder images

**MOVED OUT OF `CLAUDE.md` §16 on 2026-09-19**, at 141,532 characters against
that file's hard 150,000 limit — the SIXTH such move, and made for the reason
all six happened: past roughly 140,000 the guide's own tail starts heading for
a cliff where it truncates SILENTLY and §§17-19 vanish from agent context.
Nothing was deleted; the whole run is below unchanged.

It was the block chosen because §16 is a LESSON INDEX for the code, and these
are lessons about the machines and images that BUILD the code — a reader
touching the timeline or the rail never needs them, and a reader touching
packaging needs all of them at once.

**READ THIS BEFORE** rebuilding a builder image, changing a required-plugin
list, pinning a toolchain version, or diagnosing a packaging job that hangs.

**A LIBRARY IS NOT ITS LOADABLE MODULE, AND `appstreamcli compose` NEEDS THE
MODULE.** 0.9.9 lost FIVE pipelines in a row (246-250) to `build-flatpak`,
each reporting the same two hints and no filename:
`E: filters-but-no-output` and `E: file-read-error`. flatpak-builder runs
`appstreamcli compose` in its cleanup phase and that RASTERISES the
component's icon; the release commit moved the flatpak renames from the
manifest's `post-install` into CMake, and the new block renames
`data/icons/lightning.svg` to the app id — so compose began picking the
SCALABLE icon, which gdk-pixbuf reads through a loader MODULE. On Debian that
module is `librsvg2-common`; `librsvg2-2`, the library, was present all along
through somebody's Depends, and `--no-install-recommends` is why the module
was not. The hint report, which the job never printed, names it exactly:
`fname: //share/icons/hicolor/scalable/apps/org.lightning_matrix.Lightning.svg`,
`msg: Unrecognized image file format`.

Two hypotheses were spent on it first and both are refuted here, so that
nobody re-derives them: it is NOT the metainfo's `<screenshots>` block
(flatpak-builder passes no mirror flag, so compose never fetches a screenshot
— compose succeeds with the block intact and the tag absent), and it is NOT
runner-specific (the pinned CI image carries flatpak-builder 1.4.4, appstream
1.0.5 and flatpak 1.16.6, which are the laptop rig's versions to the digit).
The rig passed because `org.flatpak.Builder` plus the KDE SDK bring the loader
in: it shared the versions under test and not the property under test.

**The method that settled it in one session is worth copying.** Stage the
tree CMake installs (`share/applications`, `share/metainfo`, `share/icons`)
by hand, run `docker run <the image the job pins BY DIGEST>` with the job's
exact apt line, and run `appstreamcli compose` with `--hints-dir` — which is
what turns two nameless hints into a filename and a message. Then change one
variable at a time: delete the SVG (Success), install `librsvg2-common`
(Success, and five icon-cache sizes appear), restore the screenshots
(Success). No pipeline, about four minutes.

`build-flatpak.sh` now runs `appstream_can_read_scalable_icon` before the
build: it composes a synthetic one-component unit around the REAL
`data/icons/lightning.svg` and dies naming the package, in about a second
instead of sixteen minutes. Note for anyone editing it — the synthetic
desktop entry MUST carry a `Categories` key, because compose rejects a
desktop application without one and the probe then fails in every
environment, healthy or not. `test-pipeline-config.py` pins the package by
name and asserts the preflight is CALLED.

This is the fourth costume of one failure here — `libgstsctp.dll` that
webrtcbin loads for itself, the AppImage's Qt TLS and Wayland plugins, NSS's
runtime-path `libsoftokn3`, now gdk-pixbuf's SVG loader. In every one the
library was present, every ELF walk was clean, and the feature was silently
gone. Full account in `docs/round-history.md`, 2026-09-22.

**THE WINDOWS BUILDER IMAGE IS BUILT BY HAND UNDER A FIXED TAG, AND A
DOCKERFILE CHANGE ALONE CHANGES NOTHING.** Pipeline 172 compiled and linked
Windows and then died in `stage-windows-runtime.py` on "required GStreamer
plugin is missing from the builder image: libgstjpeg.dll" — the plugin was
added to `packaging/windows/Dockerfile` AND to the required list on
2026-09-02, but the image on the runner host (`...-v5`, 10.195.35.2) is
rebuilt only by the four-step operator procedure in lightning-deploy
`docs/windows-runner-operations.md`, which never ran. Deploy `37a4dd1` makes
the plugin OPTIONAL (staged when present, a WARNING when not) because the
app's camera chain cannot negotiate `image/jpeg` yet, so it would be staged
and never loaded.

**THAT OPERATOR STEP IS DONE, 2026-09-12 — AND ATTEMPTING IT FOUND THAT THE
DOCKERFILE HAD BEEN UNBUILDABLE FOR TEN DAYS.** Its verify stage asserts the
staged plugin count with a literal (`= 27`) and the install loop above it
stages 28: `libgstjpeg.dll` was added to the loop on 2026-09-02 and the number
was not bumped with it. Nobody could have found out, because the image is
built by hand and was not rebuilt in that window — so the very step this
paragraph asked for could not have succeeded if anyone had tried. It fails
with NO diagnostic, because a bare `test` in an `&&` chain prints nothing.
GENERALISE: a hand-built artefact's recipe is only as true as its last build;
"the change is committed" is not "the change works".

**AND IT CAUGHT ME AGAIN ON 2026-09-16, IN THE OPPOSITE DIRECTION.** A review
asked for `libgstlevel.dll` to be staged on Windows — correctly, since the
capture level meter is the diagnostic that tells a live microphone from a dead
one and Windows is where the "nobody can hear me" reports come from. Adding it
to the REQUIRED list killed `build-windows` in pipeline 224: the hand-built
image does not carry it, and the required list is only ever as true as the
image's last build. It is `OPTIONAL_GSTREAMER_PLUGINS` now, exactly as
libgstjpeg was, with the promotion procedure written at the declaration. **A
required-plugin entry and an image rebuild are ONE change, and the entry is
the half that must come second.**

**FEDORA WITHDREW EVERY 6.11.1 QT PACKAGE THIS IMAGE PINS, SO THE RECIPE COULD
NOT BE REALISED AT ALL (2026-09-16).** Not a version anyone wanted to move: a
v7 build died on `No match for argument:
qt6-qtshadertools-devel-6.11.1-1.fc44.x86_64`, and it had got as far as step 7
of 11 only because layers 1-10 came from that host's cache. The pins are 6.11.2
now, so **the Qt shipped to Windows users moves with the next package**. A sweep
of all 31 pinned NVRs found 23 available and 8 gone, the 8 being exactly the Qt
set — so do not widen it by assumption. **AND VALIDATE THE PROBE BEFORE
BELIEVING IT**: the first sweep used a dnf5 argument that does not exist, every
query returned empty, and all 31 looked withdrawn. A probe that answers "absent"
for everything is a broken probe until it has answered "present" for something.
Release numbers are not uniform across a Qt set (`qtmultimedia` was `-2`, the
rest `-1`), and `download.qt.io` without `--location` hands you a 306-byte
mirror page that hashes cleanly and is not the tarball.

**A DOWNLOAD WITH NO TIMEOUT CAN HANG A BUILD FOREVER — AND A SLOW ONE LOOKS
IDENTICAL (2026-09-16).** A v7 attempt sat SIXTEEN MINUTES at near-zero CPU with
nothing in the log on a `curl` to gstreamer.freedesktop.org, and I recorded that
as a stall. **It may not have been**: that installer is 960 MB, a later run
pulled it at ~560 kB/s, and 28 minutes of silence at no CPU is what a healthy
fetch of it looks like. The first attempt was killed without measuring bytes, so
the diagnosis was a guess dressed as a finding. Every download there now carries
`--connect-timeout 30 --speed-limit 10000 --speed-time 60 --retry 3`, which is
right either way; the sha256 checks are untouched. **The measurement that
actually distinguishes them is the file**: `stat -c %s
/proc/<curl-pid>/root/<path>` twice, fifteen seconds apart — zero delta is a
stall, anything else is slow. Load average tells you a step is not COMPUTING; it
does not tell you the step is not WORKING.

Builder `...-v6` is now built on 10.195.35.2 (image `sha256:5c628d4b`, 28
plugins, `jpegenc` and `jpegdec` both in `libgstjpeg.dll`), the host's
`config.toml` points at it with v5 kept in `allowed_images` for rollback, the
runner verifies, and `windows-package-test` is green on it (pipeline 205).
`libgstjpeg.dll` is back on the REQUIRED list and `jpegdec` is in the probed
element set — staging the DLL is not the same claim as the element
registering, which is the distinction that shipped Windows for months with
`libgstsctp-1.0-0.dll` present and `sctpenc` missing. Do not remove the v5
image; removing it is what makes the rollback impossible.

**THE 0.9.0 APPIMAGE SHIPPED WITHOUT QT'S TLS BACKEND AND WITHOUT THE
WAYLAND SHELL INTEGRATION, and nothing could have caught it.**
linuxdeploy-plugin-qt deploys neither directory unless `EXTRA_QT_PLUGINS`
names it, so it ran under XWayland (a screen share captures a black root
window) and every QNetworkAccessManager https request failed — the update
manifest and download included, so **a 0.9.0 AppImage will never offer the
next release by itself**; Matrix traffic was unaffected (rustls). Deploy
`1b773c2` names the four directories and `validate-appimage.sh` asserts the
files, because graceful fallback and silent absence are the same observable
until something asserts the payload — the fourth time this shape has bitten
(sctp, ximagesrc, opengl, now this). Detail in `docs/round-history.md`.

**A FLAG TESTED OVER A CACHE HIT WAS NEVER TESTED (2026-09-19).**
`flatpak-builder` caches per stage, and `cleanup` is the stage that runs
`appstreamcli compose`. A round added `--compose-url-policy=full` to a rebuild
whose log says `Cache hit for cleanup, skipping`, saw the catalogue unchanged,
and recorded the flag as REFUTED — over a stage that never executed. On a cold
cache it changes the output completely: the catalogue goes from
`media_baseurl=` plus relative paths to absolute `https://dl.flathub.org/media/`
URLs, and both `appstream-external-screenshot-url` and
`appstream-remote-icon-not-mirrored` disappear. Two days of "the linter is
wrong" were one missing flag. The packaging cousin of "a job that exists and
looks right is not a job that has run": **check the build log for the cache line
before believing any conclusion about a cached stage.**
Corollary already paid for twice here: **do not hand-roll `flatpak-builder`
flags.** `flathub-build`, inside the `org.flatpak.Builder` flatpak, is what
Flathub's own buildbot runs and it passes both
`--mirror-screenshots-url=https://dl.flathub.org/media` and
`--compose-url-policy=full`. The tracked driver is
`packaging-ci/scripts/flathub-presubmission-lint.sh`; it calls that wrapper,
never its own argument list, and warns on the cache line.

**THE LICENCE TEXT TRAVELS WITH THE BINARIES, AND "THE ONE PROJECT A REVIEW
NAMED" IS NOT THE SCOPE (2026-09-19).** A 2026-09-17 round shipped
gst-plugins-good's LGPL text on Windows, the AppImage and the snap, and called
itself RESOLVED.

**THE FIRST VERSION OF THIS ENTRY WAS WRONG ABOUT THE APPIMAGE and the way it
was wrong is the lesson.** It said the payload had licence text for one
project out of ~150. It does not: linuxdeploy deploys Debian copyright files
by design, and the published 0.9.8 AppImage already carries **235
`usr/share/doc/<pkg>/copyright` files, 4.95 MB**, libavcodec61 and
libqt6core6t64 among them. The claim came from a probe searching `*licen*`
and `COPYING*` — and Debian names the file `copyright`, so it found two where
`-name copyright` finds 235. *A probe that answers "absent" for everything is
a broken probe until it has answered "present" for something*, and it was
believed because its answer was the one being looked for. The real AppImage
gap is **ten packages** hand-staged past linuxdeploy's excludelist.

The macOS half needed no correction and is the serious one: that bundle
carried **no licence file at all** across 2,066 files, **including
Lightning's own GPL-3, which §4 of that licence requires to accompany the
program**. A self-contained format's licence
obligation is a property of the PAYLOAD, so derive it from the payload:
`build-appimage.sh` now maps every bundled object to its Debian package through
`/var/lib/dpkg/info/*.list` (plus the two packages the job `dpkg-deb -x`s into
`/opt` rather than installing — `kimg_jxl.so` is in no dpkg file list) and
copies `/usr/share/doc/<pkg>/copyright`. 242 packages, 5.2 MB, in a 136 MB
image. A hand-written list would go stale the first time linuxdeploy's ELF walk
pulled in one more library, and nothing would say so.
And check what CLASS of licence it is before assuming the duty: Qt Multimedia's
ffmpeg plugin drags Debian's GPL-enabled libavcodec and with it x264, x265,
xvidcore, dvdnav, dvdread, gme and openmpt — **GPL-2-or-later, not LGPL**, so
the source obligation is GPL §3's. Options and tradeoffs in
`docs/open-items.md`; that one is the maintainer's call.
