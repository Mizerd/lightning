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
