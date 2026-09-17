# Live validation: what Rokas has actually confirmed

## 2026-09-17 — the 0.9.8 AppImage camera on the MJPG chain, which no release has shipped

**PASS, and this one is a release gate rather than a nicety.** 0.9.8 is the
FIRST shipped AppImage to carry `libgstjpeg`, so `jpegCameraChainAvailable()`
now succeeds and its camera takes the **MJPG** chain where every previous
release took the raw one. The camera evidence gathered earlier tonight was
measured on 0.9.7 — evidence about a *different chain* — and does not transfer.
The code comment on that decision is explicit about the risk: the ladder falls
back on a description that fails to PARSE, and cannot catch one that parses and
then fails to NEGOTIATE, "which is what an `image/jpeg` capsfilter in front of a
source offering only raw produces — a camera that is simply dead".

Measured on the pipeline-228 artifact (`Lightning-0.9.8+git20260916.a12088f`),
in a real encrypted call:

```
camera chain= mjpg (jpeg elements present )
capture negotiated caps= image/jpeg
capture delivered frames count= 10
frames encrypted stream= "" video= true count= 500
```

| | stage region |
|---|---|
| camera ON (MJPG) | `mean=0.817 stddev=0.198` |
| camera OFF | `mean=0.913 stddev=0.077` |

RMSE on/off **0.209**. So the new chain negotiates, delivers, encodes, and the
stage region CHANGES when the camera is turned on.

**"AND RENDERS" IS WITHDRAWN — 2026-09-17, on review.** A changed region is not
a picture. ON is DARKER and noisier than OFF, which is also the signature of
the dark tile with a crossed-camera glyph that `docs/open-items.md` records as
the no-picture state, and of a black rectangle, and of any layout change. The
method cannot tell those apart from a camera image.

**And "method is identical to the 0.9.7 run" was false in the direction that
matters.** The 0.9.7 entry below ALSO recorded the sensor itself
(`mean=0.115 stddev=0.209`, a real contrasty scene) as a cross-check; this run
recorded no sensor reading at all, so it is strictly weaker. Worth noting the
0.9.7 pair does not correlate either — sensor `mean=0.115` against a rendered
region of `mean=0.574`, five times brighter than its own source, which is
evidence AGAINST that render claim rather than for it.

What this entry does carry, and it is the release gate it was run for: the
MJPG chain that 0.9.8 newly takes builds, negotiates `image/jpeg`, delivers
frames and encodes them, and the application does not fall back or fail. The
picture itself is NOT TESTED on either chain.

## 2026-09-17 — THE WINDOWS CAMERA RENDERS, on both chains, near AND far

**PASS, and it REFUTES the hypothesis I was about to spend the morning on.**
Published **0.9.7 Windows portable**, the same package and the same guest that
produced the failing session on 2026-09-16, in a real call against the
published Linux AppImage.

The experiment needed no new build. The application decides whether a camera
takes the compressed chain by BUILDING `videotestsrc ! jpegenc ! <entry> !
fakesink` once per process, so moving `libgstjpeg.dll` out of the package's
plugin directory (and deleting GStreamer's cached registry beside it) forces
the raw entry — the one every release before 0.9.8 used.

| self-view tile, same crop | mean | stddev | log |
|---|---|---|---|
| camera OFF | 0.934 | 0.043 | — |
| RAW chain | 0.605 | 0.119 | `camera chain= raw (jpeg elements absent )`, `video/x-raw YUY2 1920x1080 5/1` |
| MJPG chain | 0.683 | 0.128 | `camera chain= mjpg`, `image/jpeg 1920x1080 30/1` |

**Both chains drew a picture** — a recognisable photograph of the room the
webcam faces, not merely a region that changed: three times the variance of
the off-state, and the same scene visible in both captures. `publish first
encoded frame screenShare= false` on both.

**AND THE FAR END GOT IT.** The Linux AppImage in the same call logged
`received track attributed= true ... kind= "video"`, a running receive chain,
and `frames decrypted ... video= true count= 1000 dropped= 0` starting at the
second the Windows camera was switched on, with ZERO
`video frames decrypted but NOT rendered` warnings in its whole log.

**So "the camera does not display on Windows" DOES NOT REPRODUCE**, and
"MJPG is the cause" is refuted by an A/B in one session with one package.
Nothing in the code changed between the failing session and this one, so this
does not say the defect was fixed — it says the defect is not a property of
the package, the chain, or the camera, and whatever condition produced it on
2026-09-16 is not present today. `docs/open-items.md` carries what that leaves.

**What the same run DOES quantify: the raw chain's rate ceiling.** It
negotiated **5 fps** at 1080p where the compressed chain negotiated 30 — which
is the bandwidth ceiling `libgstjpeg` exists to remove, measured on the
platform the "Windows camera runs at 10 fps" item was filed against.

## 2026-09-17 — THE FALSE DECRYPTION BADGE, CAUGHT LIVE, and the shape of it

**The defect the maintainer reported, reproduced and measured** on published
0.9.7 binaries: a red warning marker on a participant's tile in a call where
everything worked. The receiving client's log says exactly what raised it:

```
decrypt failed stream= "PA_iya5jz6bhMZG" video= false count= 1  passed= 5074
decrypt failed stream= "PA_iya5jz6bhMZG" video= true  count= 1  passed= 2759
decrypt failed stream= "PA_iya5jz6bhMZG" video= false count= 10 passed= 5074
```

**Every occurrence in that log has the same shape**, across two days and five
different stream ids: a burst of about ten undecryptable frames within ~160 ms
of a NEW sender's stream appearing, and then nothing — no `count= 50`, no
`count= 100`, ever. Against 5074 and 103203 frames that passed.

That is the sizing evidence for the fix. Badging on the FIRST failure (0.9.7's
behaviour) raises a warning every time a peer joins. A rate over a 100-frame
window sees 10%, which is nowhere near the 90% it raises at, while a genuine
key mismatch is 100% of every window. And the shape — a burst at the start of
a stream, then clean for ever — is frames arriving before that sender's key is
installed, which is `no-key-for-index` and NOT "the two ends hold different
keys", the sentence the 0.9.7 log prints there. Both are fixed in the tree.

## 2026-09-17 — THE LEVEL METER LOGS FROM A PACKAGE AT LAST (and -350 dBFS is correct)

**PASS.** A published package logs a microphone level and raises the silence
warning — the 0.9.7 promise no shipped package could keep, and the claim
0.9.8's notes are built on.

The 0.9.8 Windows portable, unpacked in the guest, session migrated from the
0.9.7 portable's own data directory (so no password was typed anywhere — see
the three recorded incidents), in a real call:

```
microphone level peak= -90  dBFS
microphone level peak= 0    dBFS
microphone level peak= -350 dBFS
...
THE MICROPHONE IS CAPTURING NOTHING: peak has stayed at or below -60 dBFS
for 10 s while unmuted
```

The element registers, the meter reports, and the silence warning fires. The
guest's only microphone is an RDP endpoint with nothing playing into it, so
the capture really was silent and the warning really was correct.

**AND -350 dBFS IS THE RIGHT ANSWER, WHICH I GOT WRONG FIRST.** 16-bit audio
floors near -96, so -350 looked impossible, and it matches the value the bus
handler starts each message at — so it was read as "the parser failed" and a
guard was written to ignore any reading that low. Then it was measured:

```
gst-launch-1.0 audiotestsrc wave=silence ! level
  -> peak=(GValueArray)< -349.99999992181608 >
```

That is the ELEMENT'S OWN FLOOR for true digital silence, which is exactly the
condition the silence warning exists for. The guard would have made that
warning unreachable for a genuinely dead microphone while leaving it working
for a merely quiet room — the precise inversion of the feature. It was
withdrawn before it shipped, and the starting value was -350 on purpose all
along.

**GENERALISE: a value that looks impossible for the SIGNAL may be exactly what
the INSTRUMENT emits at its limit.** Ask the instrument before calling its
output a bug. What survives in the tree is a fallback for a GstValueArray
spelling no runtime has been seen using, labelled as such, plus a line that
names the type once if a third ever appears.

## 2026-09-17 — VOICE DELAY ON REAL WINDOWS, from the shipped 0.9.8 package

**PASS, and this is the platform the maintainer made mandatory.** Not Wine,
not a rig, not a differential against an RDP path that drifts 250 ms between
identical runs — the shipped `Lightning-0.9.8-...-windows-x86_64-portable.zip`
unpacked in the Windows 11 guest and asked directly. **The guest has no audio
hardware at all**, which is exactly why every acoustic attempt at this number
failed, and exactly why this one does not care.

```
Lightning 0.9.8 / GStreamer 1.28.5 / Windows 11 guest

SHIPPED queue max-size-buffers=4 leaky=downstream
    peak while starved  40 ms   at realtime   40 ms   free  0 ms
SHIPPED queue max-size-time=100000000 leaky=downstream
    peak while starved 100 ms   at realtime  100 ms   free 10 ms
CONTROL queue
    peak while starved 1000 ms  at realtime 1000 ms   free  0 ms

RESULT: PASS    exit code 0
```

**The same numbers as Linux, to the millisecond on the shipped queues and on
the control**, from a different GStreamer (1.28.5 against the dev shell's
1.26.11) on a different operating system. The control holds a full second and
is STILL holding it once its consumer is back at real time — which is all a
live encoder ever gets — while the shipped bound gives back everything above
100 ms.

Two platforms of the three now, with Windows among them. How it was run, for
whoever repeats it: a batch job dropped on the host share and picked up by a
loop running inside the guest, so no GUI, no clicking and no screenshot is in
the path.

## 2026-09-17 — THE QUEUE PROPERTY, MEASURED WITH A CONTROL: 900 ms, and it is permanent

**PASS on Linux, from `--call-queue-selftest`.** This is the entry that
replaces the retracted half of the acoustic run below. It measures the property
the voice-delay claim is ABOUT — a live queue does not accumulate latency it
cannot give back — rather than an end-to-end number that depends on a rig.

Dev-shell build, GStreamer 1.26.11, one command, about thirty seconds:

| queue | peak while starved | consumer back at REAL TIME | consumer let run free |
|---|---|---|---|
| `queue max-size-buffers=4 leaky=downstream` | 40 ms | 40 ms | 0 ms |
| `queue max-size-time=100000000 leaky=downstream` | 100 ms | 90 ms | 0 ms |
| **CONTROL** `queue` (GStreamer's default) | **1000 ms** | **1000 ms** | 0 ms |

**The third column is the whole result.** A live source produces one second of
audio per second, so a consumer that merely KEEPS UP can never give back what
it fell behind by — and a plain `queue` is still holding its full second there.
That 900 ms difference is what this project has asserted in a source comment
since 2026-09-16 and had never demonstrated. The fourth column is the consumer
running FASTER than real time, which nothing in a call can do; a queue that
only drains in that column has shown nothing, which is why the column exists.

Three properties make this evidence rather than decoration, and each answers a
specific objection raised against the acoustic run:

* **it tests the string production builds.** The queue specifications are read
  out of what `videoPipelineDescription()` returns, not written out again
  beside it. `docs/round-history.md` records three separate occasions where a
  test composed something *resembling* what production composes and passed on
  a feature that had never worked.
* **it starves the CONSUMER.** The defect is the encoder falling behind the
  capture, a relative rate difference. `SIGSTOP` — the acoustic run's method —
  freezes producer and consumer TOGETHER, so no backlog can form by that
  mechanism at all and a flat result is consistent with the fix working and
  with nothing ever entering the queue. `identity sleep-time` slows one end.
* **it carries its own negative control**, in the same run on the same machine.

NOT COVERED, and said plainly. This is a PROPERTY of the queues, not a
mouth-to-ear latency: it does not measure the network, the SFU, the jitter
buffer or the far end, and it cannot replace an acoustic number for "how long
until the other person hears me". The receive-side queues are built inside a
member function and cannot be read out here, so the command covers the two
publish-path specs; the source sweep `everyLiveQueueIsBoundedAndLeaky` covers
all seven statically. And the run above is a DEV-SHELL BUILD — the same command
against Windows, macOS and Linux PACKAGES is what the maintainer's three-
platform bar asks for, and is recorded separately when those artifacts exist.

## 2026-09-17 — VOICE DELAY on Linux: a number, and NOT a proof of the queue fix

**PASS for "the delay is about a quarter of a second and does not grow after a
stall". NOT TESTED for "the queue fix is what makes that true"** — read the
retraction below before quoting either. Until now the only evidence that 0.9.7
fixed the one-second send delay was the maintainer's ear ("delay is good now,
almost instant"). This is a number, from the published AppImage, on two
accounts in an encrypted call.

| phase | one-way voice delay |
|---|---|
| baseline | **265.2 ms** |
| after freezing the SENDER for 1.5 s | **268.1 ms** |
| delta | **+2.9 ms** |

**THE STALL IS THE EXPERIMENT; THE TONE IS ONLY THE RULER.** A default
GStreamer `queue` is `max-size-time=1s leaky=no`: it adds nothing until
something downstream falls behind, and then it fills and never drains. So a
latency measurement on an idle machine reads the SAME on the fixed and unfixed
trees, and a voice-delay check without an induced stall is decoration by this
project's own standard. The sender is frozen with `SIGSTOP` and released with
`SIGCONT` — chosen over a code hook precisely because it needs nothing from the
build, so the same script discriminates between two SHIPPED binaries.

**AND THAT PARAGRAPH IS A DESIGN INTENTION, NOT A RESULT — RETRACTED
2026-09-17 after an independent review.** It used to end "on the unfixed tree
the delta would be roughly +1000 ms and permanent", which is a PREDICTION in
the conditional mood sitting inside a section headed *measured*. **The unfixed
binary was never run.** §18 of the guide says a regression test that does not
fail on the old code is decoration, and that applies to a measurement exactly
as it does to a test.

**Worse, the mechanism and the method may not meet.** The defect is the
ENCODER falling behind the CAPTURE — a relative rate difference. `SIGSTOP`
freezes the whole process, so the producer and the consumer stop TOGETHER and
no backlog can form by the named mechanism; any backlog would have to come
from the audio server's own record buffer draining on resume, and that was
never measured. **A delta of +2.9 ms is equally consistent with "the fix
works" and with "nothing ever entered the queue."**

Two more limits on this number, for whoever quotes it next. It was taken on
the **0.9.7** AppImage, which captures through `pipewiresrc`; 0.9.8 ships
`pulsesrc`, so it is a measurement of a chain the next release does not have.
And the harness — `DelayProbe`, the burst generator, the onset detector —
exists in neither this repository nor the vault, which is the same failure
§2 records for `verify-release.sh`, on a detector that was already wrong once
in this very entry.

What would settle it, in order of cost: log the queue's own
`current-level-time`, or run with `GST_DEBUG=queue_dataflow:5` and look for
the element saying it is full and leaking — an internal property with a direct
readout, needing no acoustics, no sound card and therefore no RDP, which is
also how it could be answered on Windows. Then run the identical script
against a published PRE-FIX binary as a negative control.

**How the number is taken, because latency measurements lie when two clocks are
compared.** A single sink, `DelayProbe`, receives BOTH a mirror of the burst as
it enters the near microphone AND whatever the far client plays out. One
recording therefore holds the same 150 ms burst twice and the gap between them
is the delay. No wall clock is trusted and no two clocks are ever compared.

**The budget, so the 265 ms is readable rather than alarming:** ~100 ms is
`webrtcbin`'s receive jitter buffer, which is deliberate (`latency=100`); ~43 ms
is the harness itself, two `pw-loopback` hops at a 1024/48000 quantum (recorded
with the run, because an unpinned quantum moves this number by tens of ms);
Opus frame plus lookahead is ~26 ms; the capture queue's own ceiling is 100 ms
worst case and ~0 typical.

**AND THE FIRST NUMBER THIS RIG PRODUCED WAS THE INSTRUMENT, NOT THE CALL.** It
returned exactly 300.0 ms — which was the refractory period in my own onset
detector. A burst still above threshold when that timer expired was counted a
second time. **A result equal to a constant inside the instrument is the
instrument.** Detection is a rising edge through a high threshold after falling
below a low one now, which cannot manufacture an onset, and the same recording
then read 227.7 ms.

**The 227.7 ms above and the 265.2 ms headline are different runs, not a
discrepancy**: 227.7 ms was the re-read of the recording that had produced the
detector artefact, minutes earlier and before the rig was re-pinned at a
1024/48000 quantum. The headline is the later run whose budget is itemised
here. Neither is repeated enough to carry a ± figure, and the same rig on
Windows produced stall deltas of +1.1, -43, +21 and +39 ms — a -43 ms negative
delta means the noise floor of this instrument exceeds ±40 ms somewhere, and
it has not been characterised.

NOT COVERED: this is Linux to Linux. Per the maintainer, a delay claim needs
three platforms with Windows mandatory; the other two are recorded separately.

## 2026-09-16 night — the camera DOES render, on Linux, measured not eyeballed

**PASS for the render path, and it refutes the explanation I gave for Windows.**
Published 0.9.7 AppImage on the laptop, in a call, camera toggled on and off,
with the call stage measured rather than looked at (the webcam faces a real
room):

| | stage region |
|---|---|
| camera ON | `mean=0.574 stddev=0.159 max=1` |
| camera OFF | `mean=0.925 stddev=0.053 max=1` |

RMSE between the two captures is **0.356**. With the camera on, something
varied and mid-tone is painted where the flat background is otherwise; with it
off, the region is flat. The sensor itself measured `mean=0.115 stddev=0.209`
at the same time — a real, contrasty scene. So **Lightning's camera capture and
local render path work**, and the earlier "the camera renders nowhere" was not a
property of the client.

Method note, because the first version of this measurement was wrong twice: the
sensor reading must be a LATE frame (`-frames:v 30`), not `-frames:v 1`, which
catches a UVC device before auto-exposure converges and reads near-zero on a
perfectly good camera. And the tile is compared by pixel statistics rather than
viewed, because it is a camera pointed at the maintainer's room.

WHAT THIS DOES NOT COVER: **Windows**. The guest published 5000 camera frames
and drew the no-picture state at both ends, and this run does not explain that —
it only removes "the client cannot render a camera" as the reason. The in-guest
check (`docs/open-items.md`) is still what settles Windows.

Also confirmed here, which is the other half of a finding I had only half
stated: this AppImage logs `camera MJPG chain unavailable, cameras will use the
raw entry: no element "jpegenc"`. The published 0.9.7 AppImage is missing
libgstjpeg as well as libgstlevel, so its camera cannot reach 720p30 on USB 2.0.
Both are fixed on `main` by `8e744c7` and neither is in a release.

## 2026-09-16 night — the level meter reaches a Windows PACKAGE, asked of the registry

**PASS, and on the claim rather than the colour.** Builder image v7 was built,
deployed and verified, and only then did `stage-windows-runtime.py` move
`libgstlevel.dll` into the required list and add `"level"` to
`GSTREAMER_ELEMENTS`. Pipeline **227** (`windows-package-test`, non-publishing:
`BUILD_FORMATS=none`, `PUBLISH_PACKAGES=false`, variables confirmed present)
built a Windows package from that tree and its Wine probe reported:

```
bundled GStreamer registered all 42 required elements under Wine
```

42 is the size of `GSTREAMER_ELEMENTS` with `"level"` in it, so the probe asked
the SHIPPED GStreamer's real registry for that element and got it. **Staging a
DLL is not this claim** — the Dockerfile's own `level:libgstlevel` symbol probe
CANNOT fail, because unlike `sctpenc:libgstsctp` the element and the plugin
share a name and the string sits in `.rdata` either way. This is the check that
distinguishes them, and it is the one that caught `libgstsctp-1.0-0.dll` being
present for months while `sctpenc` was missing.

WHAT THIS DOES NOT COVER: it proves the element REGISTERS in a package. It does
not prove a call made from that package logs `level= true` and raises the
silence badge — that needs a real call from a published artifact, and it is the
thing 0.9.7's notes promised and could not keep.

## 2026-09-16 — Windows 0.9.7 and Linux 0.9.7 call each other, both ways

**PASS, measured.** The same `lt-windows` guest running the published Windows
portable, against the published **`Lightning-0.9.7-x86_64.AppImage`** on the
laptop (KDE/Wayland, PipeWire), two accounts, encrypted `calltest`. The Linux
client STARTED the call and the Windows client joined it.

| | Windows -> Linux | Linux -> Windows |
|---|---|---|
| audio | PASS, tone ratio **315:1** | PASS, tone ratio **1,126,362:1** |
| screen share | PASS, rendered in Lightning | PASS, rendered in Lightning |

The same Goertzel tone method as the Sable and Element runs below, with
`pw-link -l` printed first to prove that the guest's microphone
(`WinTestMic` -> `FreeRDP:input`) and the Linux client's (`LightningTestMic` ->
`AppRun.wrapped:input`) are separate nodes that nothing else touches. Both
logs agree at the media layer: Windows `rtp packets handed to webrtcbin
count= 500` and `frames decrypted ... video= true count= 500 dropped= 0`;
Linux `frames decrypted ... video= true count= 3000` and `video= false
count= 8000`. Both ends tore down cleanly on quit (`teardown state= 6
error= "<none>"`).

This is the cell the Sable and Element runs left open: Lightning to Lightning
ACROSS PLATFORMS, which had never been exercised. What it does NOT cover is
unchanged from the entry below — the guest has no sound card, so the audio
rides RDP; the GPU share chain cannot run in the VM; macOS is still untested.

## 2026-09-16 — the WINDOWS package calls Sable and Element Web, both ways

**PASS, measured, not reported.** The published **0.9.7 Windows portable**
(`Lightning-0.9.7-bc5dcd5-windows-x86_64-portable.zip`, the release bytes, not
a local build) running in the `lt-windows` guest on the laptop, in the
encrypted `calltest` room, against **Sable 1.21.0** (`app.sable.moe`) and
**Element Web** (Element Call, not the legacy 1:1 path):

| | Windows -> peer | peer -> Windows |
|---|---|---|
| Sable, audio | PASS, tone ratio **142:1** | PASS, tone ratio **130,224:1** |
| Sable, screen share | PASS, rendered in Sable | PASS, rendered in Lightning |
| Element Web, audio | PASS, tone ratio **3,635:1** | PASS, tone ratio **3,147,784:1** |
| Element Web, screen share | PASS, rendered in Element | PASS, rendered in Lightning |

Each audio direction is a 440+880 Hz tone through a Goertzel detector against a
1 kHz control, played into ONE endpoint's microphone and recorded at the
OTHER's speaker, with `pw-link -l` printed in the same run to prove which node
fed which. The Windows client both STARTED a call Sable joined and JOINED one
Element started.

What the guest's own log shows:

```
call media engine built in: yes            (GStreamer 1.28.5, bundled)
publishing microphone: valve drop= false device-channels= 0 dsp= true level= false
sfu published our track kind= microphone sid= "TR_AMypAPvCXf8Lcq"
rtp packets handed to webrtcbin video= false count= 1500
frames decrypted stream= "PA_LkjTZjF8Tfwa" video= false count= 2500 dropped= 0
screen share falling back to the CPU: the GL chain is present but cannot run on this machine
publish first encoded frame screenShare= true afterPublishMs= 83 firstCaptureMs= 34
frames encrypted stream= "" video= true count= 500 dropped= 0
share audio published perApplication= false
camera chain= mjpg (jpeg elements present )
```

**THE GUEST HAS NO SOUND CARD AT ALL, AND THAT IS WHY THIS TEST EXISTS IN THIS
SHAPE.** `qemu-system-x86_64` runs with `-nodefaults` and no `-audiodev`; the
only USB device passed through is a UVC webcam with no audio interface
(`bInterfaceClass` 0e on every one of its five interfaces). Windows reported
zero `Win32_SoundDevice` and zero `AudioEndpoint`, so Lightning had nothing to
open. The audio path is **RDP**: `xfreerdp /sound:sys:pulse /microphone:sys:pulse`
into the guest gives the session two `Remote Audio` endpoints, and the client's
`PULSE_SOURCE`/`PULSE_SINK` pin them to dedicated PipeWire nodes
(`WinTestMic` in, `TestIn` out) that no other endpoint touches. Recreating the
container to add an audio device was deliberately NOT done: `lt-windows` shares
a host with the maintainer's hands-off WinApps guest.

WHAT THIS DOES NOT COVER:

* `level= false`: the published Windows build has no `libgstlevel.dll`, so the
  capture-side level meter is absent there and the mic-silence badge cannot
  fire. That is the optional-plugin entry waiting on a builder-image rebuild,
  not a defect in this run. The RTP pad probe carried the proof instead.
* The GPU share chain cannot run in this VM (no working GL), so the CPU
  fallback is what was measured. A Windows host with a real GPU is NOT TESTED.
* macOS remains NOT TESTED.
* **The camera is NOT TESTED, and the "it was the sensor" explanation is
  WITHDRAWN.** It captured and encoded cleanly — `frames encrypted video= true
  count= 5000 dropped= 0`, a steady 30 fps, `camera chain= mjpg` — and neither
  Element nor Lightning's own self-view drew a picture. I measured the webcam
  off the host afterwards (`/dev/video0` mean=1.9e-07) and called it a dark
  sensor. **That does not hold.** Zoomed, the "You" tile is the tile's
  dark-grey BACKGROUND with a centred crossed-camera glyph — the no-picture
  state, not a video surface full of dark pixels, which is what a dark sensor
  would paint. The measurement was also taken on a different OS through a
  different driver after the guest released the device, and `ffmpeg -frames:v 1`
  grabs a UVC device's FIRST frame, before auto-exposure converges. The earlier
  Windows round settled the same symptom from INSIDE the guest — a lit picture,
  and three stills of a static scene hashing differently — and that check was
  available to me and I did not use it.

  The camera from this guest is therefore **OPEN, not explained**; what settles
  it is in `docs/open-items.md`. Audio and screen share are unaffected.
## 2026-09-16 — calls audible both ways, and the send latency gone

**PASS, on the maintainer's desktop, Lightning (source build) <-> Element Web
(Brave), encrypted room, MatrixRTC via the LiveKit focus.**

What he confirmed, in his words: "there was sound in element this time, i heard
myself"; then, after the queue fix, "delay is good now, its almoast instant".
Element -> Lightning audio was working throughout.

What the run's own log shows, which is why this is a PASS and not a report:

```
publishing microphone: valve drop= false device-channels= 4 dsp= true level= true
sfu published our track kind= microphone sid= "TR_AMSmcx8UZqzsri"
microphone level peak= -3 dBFS
frames encrypted stream= "" video= false count= 1000 dropped= 0
rtp packets handed to webrtcbin video= false count= 500
```

WHAT THIS DOES NOT COVER, and none of it may be promoted without its own run:

* ONE device, ONE platform: a Roland Rubix44 on PipeWire on NixOS. The
  multi-input fix is scoped to `pipewiresrc` precisely because nothing else
  was measured. Windows and macOS are NOT TESTED, and on Windows packages the
  level meter did not even exist until this round staged `libgstlevel.dll`.
* Lightning <-> Lightning and Lightning <-> any other MatrixRTC client
  (Element Desktop, Element X, Sable) are NOT TESTED. Sable was read rather
  than run: it carries `livekit-client`, `matrix-js-sdk` and `msc3401`
  references, so it should interoperate, and that is code reading, not a test.
* The latency improvement is the maintainer's ear, before and after, not a
  measured figure. The `queue` default it fixes IS measured (1 s, non-leaky).

## 2026-09-16 — the Flathub build makes real calls (PASS, send direction)

**Rokas, from the sandboxed Flathub build on the Fedora 44 laptop (Wayland),
to Element X on his phone:** he heard himself on the phone, and when he shared
his screen the video arrived on the phone.

So, from a build produced by `flathub-build` out of the submission manifest —
not a dev build, not a package we assemble ourselves:

- **Audio, Lightning -> Element X: PASS**
- **Screen share, Lightning -> Element X: PASS**

That is the one thing the two `flatpak-builder-lint` runs could not answer: the
sandbox + portal call path. It also exercises `--filesystem=xdg-run/pipewire-0`,
which is the one permission a Flathub reviewer is most likely to question, and
shows it is doing its job.

**What this does NOT cover, and must not be read as covering:** the RETURN
direction. He reported hearing himself and seeing his own share arrive; he did
not report receiving the phone's audio or camera. Element X -> Lightning over
the Flatpak remains **NOT TESTED**. (Receive is the direction that has broken
before and been invisible for months — see the sctp lesson — so it does not
inherit a pass from send.)

Environment: Fedora 44, KDE/Wayland, `org.lightning_matrix.Lightning` built
from tag v0.9.6 / `e177135` via the GitHub mirror.


**MOVED OUT OF `CLAUDE.md` §16 on 2026-09-15**, at 139,949 characters against
that file's 150,000 hard limit — the fifth move, and for the reason all five
happened: past roughly 140,000 the file's own TAIL heads for a cliff where it
truncates SILENTLY and §§17-19 vanish from agent context. §16 says outright
that it is a LESSON INDEX and not an inventory; a chronological record of what
has been confirmed live is an inventory, so it was the section to go.

This is the record of what has actually been exercised against real
homeservers, real hardware and real peers — newest first. Nothing in it is
inferred from a passing build or a green pipeline. Read it before promoting
anything to tested, and read `open-items.md` beside it for what has NOT been.

### Live validation: what Rokas has actually confirmed

**2026-09-16 — THE MATRIXRTC MEDIA-KEY REJOIN DEFECT: FAIL BEFORE, PASS AFTER,
against Element Web in a real encrypted room.** This is the only evidence that
can fail on the old code for that change, and it is therefore load-bearing
rather than bookkeeping.

The maintainer reported his screen share reaching Element while nothing came
back. Three cases were run with Element (`@lightningtest`) staying in the call
throughout and Lightning (`@lightningtest2`) restarted around it:

| case | before the fix | after |
|---|---|---|
| fresh join, Element already in call | PASS — `media key received index= 1` | PASS |
| clean hang-up then rejoin | PASS — `membership retracted attempts= 1`, key index 3 | PASS |
| **`kill -9` then rejoin** | **FAIL — no `media key received` line at all, `frames dropped: no key … count= 500`, never recovered** | **PASS — `index= 4`/`7`, `frames … DECRYPT correctly`, `dropped= 0`, and the Element→Lightning screen-share VIDEO decrypts too** |

Case 3 is the maintainer's own run, line for line.

**Structural proof from room state, not just from logs:** the case-3 rejoin
wrote an event carrying `created_ts: 1789505720103` — the KILLED session's join
time — where the pre-kill event had no `created_ts` in its content at all. Same
`createdTs()` before and after, so matrix-js-sdk's `RTCEncryptionManager` saw
no new joiner and sent no key.

**Refresh behaviour verified preserved on the wire:** the join writes no
`created_ts`, the 60 s refresh writes one inherited from that join with the
deadline walking forward — so `expires_for_refresh` and oldest-membership focus
selection are untouched.

**Also measured, not inferred:** the homeserver answers a delayed-event arm
with `400 M_UNKNOWN` / `org.matrix.msc4140.errcode: M_MAX_DELAY_UNSUPPORTED`
and does not apply the body. That text matched no branch of
`classify_room_error` and fell into the `network` catch-all, which is why a log
line asserted a transport problem on a server that had published
`msc4140: false`. Now `delayed_unsupported`, and publishes dropped from two
state events per refresh to one.

**NOT covered:** a peer other than Element; more than two participants; and
receiver-side recovery, which is deliberately not implemented.

**2026-09-16 — DESKTOP GUI SWEEP, on an isolated profile: no regressions.**
Run on the maintainer's own desktop while he slept, with `XDG_DATA_HOME`
redirected to a scratch directory — isolation proven from the store-path log
line before login, and his real profile's mtimes unchanged afterwards.

PASS: reply quotes resolving; forwarding offering every room OUTSIDE the Space
while the conversation list was correctly narrowed to it; the message toolbar
reachable on a one-line row with a read receipt (overflow actually CLICKED and
the menu opened); offline restore going Boot → **Main** with the cached list
and "Offline — retrying"; and the call-events room of Priority 2 below.

**The call-events room, which is the one the maintainer asked for by name:**
3 real messages plus 35 `m.call.member` events. On open the failure shape DID
occur — `items= 0` and a first page whose 21 events were all discarded as churn
— **and was handled correctly**: the fill kept walking instead of asserting
emptiness. Subscription to settled **754 ms**, all three messages rendered, the
churn collapsed into "9 room updates", and no "No messages here yet."

PARTIAL: local search — the server-side path returned results with no false
empty-index claim, but the LOCAL encrypted index was NOT TESTED, because the
only encrypted room on that fresh device was entirely "Waiting for keys…" and
so had nothing to index.

**2026-09-15 (night) — THE AUTOMATIC KEY-RECOVERY PATH, END TO END: PASS.**
On the COMMITTED code (`e72d97d`, which contains `8f472c2`), nothing patched.
Two screenshots of the same two rows: **"Waiting for keys…"** before, and
**"FOXTROT 006 second session" / "GOLF 007 second session"** after, with **no
Retry press, no passphrase, and no interaction with the client between the two
frames.** Verified twice. (`~/lt-e2ee/CLEAN-BEFORE-s.png`, `CLEAN-AFTER2-s.png`
on the laptop.)

The precondition that defeated the previous round was finally built: device
`GIYKFBQTEJ` had backups enabled **from its own store** — `Backup state changed
from Unknown to Enabled` with no `Downloading` pass and no passphrase entered
in the measured run — while one session sat in the account's backup and not in
that device's crypto store. Opening the room logged `auto key recovery
"started" sessions= 1` -> `"no_keys_found"`; the key was then restored to the
backup and a timeline diff produced from OUTSIDE the client (a reaction over the
raw API), giving `"started" sessions= 1` -> `"ok" sessions= 1` and the rows
rendering their text.

**WHY THE PRECONDITION IS HARD, and this is worth keeping:** matrix-sdk runs a
full `Downloading` pass whenever backups are enabled, so **entering a recovery
key downloads the whole backup** and cannot leave a gap. The gap needs a key
that enters the backup AFTER that pass. That is why two rounds could not build
it by the obvious route.

**Observations, not findings:** the recovery is **~200 ms** on a warm path, so
the intermediate state is effectively invisible — it could only be photographed
by making the backup temporarily lack the key. And nothing polls, as designed:
after `no_keys_found` the rows stayed unresolved indefinitely until a diff
arrived. A reaction on an already-decryptable row did not re-trigger one; on an
undecryptable row it did. That may be the 30 s backoff rather than row identity;
the two were not separated.

**NOT covered:** Element interoperability, key recovery across a room switch,
and the thread-timeline hook (the room hook is what was exercised).

**2026-09-15 (night) — THE AUTOMATIC KEY-RECOVERY PATH: THE MECHANISM FIRES
AND IS BOUNDED (PASS). THE END-TO-END CLAIM IS NOT TESTED.** Two fresh devices
of a throwaway account on the laptop, driven against a real homeserver.

**CAVEAT ON WHAT WAS TESTED:** the build was the working-tree version as it
stood when the run began (463 changed lines), not the committed `8f472c2`
(769 lines, a refined superset with the same constants and the same log
vocabulary). The evidence below applies strictly to that predecessor.

**FIRES — PASS.** On a DM full of "Waiting for keys…" rows the new path ran:
`auto key recovery "started" sessions= 3`, and matrix-sdk's own
`retry_decryption_for_events` spans appear 200 ms later, so the pass really did
call `timeline.retry_decryption` rather than merely log. Two review fixes
confirmed live: `no_keys_found` was reported (not `failed`) when the backup
genuinely lacked the keys, and skips arrived under the separate
`backup_download_skipped` / `auto_key_recovery` kinds, so a skip never touched
`m_download` — which is H1.

**BOUNDED — PASS.** 9 lines on one device over ~13 min, 8 on another over ~7
min. **The backoff is visible in the log**: three sessions attempted at
17:04:49 were not retried until 17:12:51 — an eight-minute gap with the room
open and the rows on screen — then exactly once each. The
`skipped_no_backup_key` branch produced one line, not one per diff. Clients
that never opened an undecryptable room produced **zero** lines: nothing polls.

**END TO END — NOT TESTED IN THIS RUN. Achieved later the same night; see the
entry above.** The blocker below was real but was NOT what it looked like —
see `docs/open-items.md`: it was four Lightning instances sharing one device id
and racing for its to-device queue, caused by a harness `pgrep` idiom that
matched the wrapper instead of the app, so every `kill` left the app alive.
Neither the homeserver nor Lightning was at fault.

**The run's own account of it:**
The precondition needs a key that is IN the backup and NOT yet downloaded, and
no device of that account could obtain a Megolm key by to-device at all.
Crypto-store inspection (identifiers only) found the sender's store holding
only the sessions it had always had, while account 1's SDK log shows the keys
dispatched: *"All m.room_key … were sent out, marking session as shared"*. So
the keys left the sender and never arrived, and nothing could put one into the
backup that the observer lacked. **Pre-existing and unrelated to the change** —
it was true before anything was touched. Five separate attempts at the
precondition, none stretched into a pass.

**NO REGRESSIONS — PASS.** Zero QML warnings, TypeErrors or binding loops
across five client logs; normal E2EE messaging worked throughout.

**What remains unproven no matter how carefully the code reads:** that a key
arriving in backup after a room's first pass is picked up without a gesture;
that the import produces an in-place row update rather than needing a room
switch; and that the emitted vocabulary describes what a real capture shows —
the instrument has never been read in anger.

**2026-09-15 (evening) — THE 0.9.6 GUI SWEEP ON THE LAPTOP: 8 of 8 PASS, no
regressions.** Two instances of a build of `ba2a7eb` on KDE/Wayland, throwaway
accounts, purpose-built fixture rooms. What each item actually proves:

- **The row's right rail — PASS, and MEASURED rather than eyeballed.** Edit and
  the overflow button were CLICKED and opened on a short one-line own row
  carrying a live read receipt, in Modern, Compact AND Bubbles, plus on a ~21px
  continuation row. The reserve was proven ACTIVE, not incidentally clear: the
  action bar's right edge sits at x=1340.7 with a receipt present and x=1362.7
  without — a shift of exactly 22 px = `receiptRow.width` (18) + `spacingXS`
  (4). Without it the bar lands 2.7 px inside the avatar band. **Reduced
  strength, stated:** two accounts yield at most ONE receipt avatar, so the
  reporter's four-avatar pile was not reproduced; the mechanism was, and the
  measurement scales to the ~47 px the repo test asserts.
- **Bubbles sender header inside its bubble — PASS.** No 1-px-pinned header.
- **Facepile no longer clipping an own bubble's corner — PASS.**
- **A reply resolves its target — PASS on two cases, and attributed.** The
  homeserver was first confirmed NOT to bundle the target (`unsigned` carries
  only `age` and `membership`), then a purpose-built room put 60 filler
  messages between target and reply and a COLD-STARTED client still rendered
  the quote. `git grep -c fetch_details_for_event` is 0 before `e0b6b8d` and 3
  at HEAD.
- **Forwarding outside a Space — PASS, decisively.** Inside a Space whose room
  list was correctly narrowed to its 4 children, the picker offered five rooms
  from outside it, and a message was actually forwarded to one.
- **A call-activity room does not claim to be empty — PASS, with the log.** A
  fixture room with 34 `m.call.member` events as its tail: `items= 0`, a first
  page with `added= 0` — the exact shape that used to latch the empty state —
  and six history lines rendered with no "No messages here yet.", settled
  **201 ms** after the subscription started. The new counters named the cause
  in one line on a warm re-open: `filterOffered= 262 droppedRtc= 145`.
- **Local search — PASS.** "Searching 24 messages Lightning has indexed,
  including encrypted ones." above three real hits.
- **Offline restore — PASS, confirming the desktop result on a second
  machine.** With every proxy pointed at a closed port: Boot → **Main** in
  3.3 s, not Login; complete cached room list; "Offline — retrying" from the
  first frame; and an ENCRYPTED DM rendering its decrypted history off the disk
  with no server.

**NOT covered:** the four-avatar receipt pile; anything about the 0.9.6 version
bump (the build reports 0.9.5 because the bump was still uncommitted); and the
automatic key recovery, which is a separate live test.

**2026-09-15 (evening) — THE WINDOWS CAMERA DELIVERS ~29.8 fps SUSTAINED, AND
THE SELF-VIEW SHOWS IT: PASS.** On the laptop's Windows guest, released 0.9.5
portable, with the laptop's physical privacy shutter OPEN — which is the whole
point of this entry, because the same round measured a rock-steady 10.00 fps a
few hours earlier with it closed and those numbers meant nothing. Windows
itself had said so: *"Your camera is reporting that it is blocked or turned off
by a switch."*

Three runs from the `capture delivered frames count=` pad probe on `capsrc` —
what the DEVICE emits, not a negotiated caps string: **29.797 / 29.812 / 29.809
fps** over 285 s, 218 s and 151 s, flat in every 500-frame bucket
(16.76-16.79 s each). `camera chain= mjpg`, `image/jpeg 1920x1080 30/1`,
`firstCaptureMs= 514-535`. The negotiated and delivered rates AGREE, which is
the thing that had never been true before. Against 0.9.4's raw chain on the
same guest and sensor (`YUY2 1920x1080 framerate=5/1`), the MJPG work is doing
exactly what it was built to do.

The self-view tile shows the live camera image, not the crossed-camera
placeholder the previous round saw — that observation was the shuttered sensor
and is WITHDRAWN. No `SfuMediaEngine.cpp` change is indicated.

**What this does NOT cover:** it is a QEMU `usb-host` passthrough, not bare
metal, so it does not measure the host's USB 2.0 bus, and the raw-YUY2
bandwidth ceiling the original 10 fps theory named still needs physical
hardware. It is 0.9.5, so the camera-preference fix on `main` is NOT TESTED
here. And a second peer receiving those frames was not part of it.

**2026-09-15 — OFFLINE RESTORE, LIVE: PASS, and the room list, an encrypted
DM's decrypted history and the local search index all came off the disk.**
Automation-driven on the maintainer's desktop with two throwaway accounts, not
Rokas. With `HTTPS_PROXY` pointed at a closed port so every outbound request is
refused at once, the client goes Boot -> Main and **not to the login page**,
logs `session restored from the local store`, shows the complete cached room
list, opens an ENCRYPTED DM and renders its history, and reads
**"Offline — retrying"** from the first frame (the `setState` override; before
it the same session said "Loading rooms…" over a list that was already
complete). The find bar's History scope answered `Searching 12 messages
Lightning has indexed, including encrypted ones.` with three hits, with no
server at all — which was the specific ask. In the same session the three
message layouts were audited against a real account and four collision defects
found and fixed; see `docs/round-history.md`, 2026-09-15. NOT covered: a real
homeserver outage (this is a refused proxy), and the first offline start of an
account that has never signed in on this build, which still fails by design.

**2026-09-13 (night) — A SHARED WINDOW THAT NEVER REPAINTS NOW PUBLISHES:
PASS, and it published NOTHING before.** Automation-driven on the laptop, same
static window and same peer either side of the fix. A PipeWire screencast
delivers ON DAMAGE and `videorate` emits nothing until a SECOND buffer
arrives, so a window that does not repaint gave `capture delivered frames
count= 1`, no `publish first encoded frame` line at all, and a far end stuck on
"Waiting for the picture" indefinitely. §16's `videorate` block described this
as a WAIT ("~1 s to 10 s"); for a still window it is unbounded. Fixed in
`5abcc81` by handing videorate the second buffer it is waiting for — measured
after: `keep-alive … quietMs= 506` then `afterPublishMs= 595`, and the far end
RENDERS the window. A moving share is unchanged (141 ms, keep-alive fires
zero times). A GAP event joins the do-not-retry list (0 buffers out), and the
injected PTS must come from the SAMPLED BUFFER, never the pipeline clock —
`LightningWindowCaptureSrc` and `ximagesrc` are zero-based, and the
running-time version was measured in review at 2612 buffers from one
injection and a permanently dead share. Full account in
`docs/round-history.md`, 2026-09-12 (night).

**2026-09-13 (evening) — THE SNAP COULD NEVER CARRY CALL MEDIA, AND THE CAUSE
IS NSS. Sixth occurrence of "a library loads its own plugins", and the first to
reach a shipped lane.** Debian builds **`libsrtp2` against NSS, not OpenSSL**.
`ldd` names libnss3/libnspr4/libnssutil3/libplc4/libplds4, the ELF walk bundles
all five, every payload check passes — and NSS does no crypto itself: it
**dlopens `libsoftokn3.so`**, which dlopens `libfreebl3.so`, from a path
derived at RUNTIME. Nothing staged them. Unconfined the host's NSS is found and
this is invisible; under strict confinement `/usr` is core24's, which has NO
NSS, so libsrtp returns `init_fail` (err 5), `srtpenc` posts "Could not
initialize SRTP encoder", the publisher dies and the subscriber never gets a
receive pad. **The AppImage carries the identical gap** and is one NSS-less
host away from the same failure. Fixed by staging the four NSS modules beside
`libnss3.so` in `build-appimage.sh`, asserted BY NAME in both validators.
**LIVE-VALIDATED PASS on the artefact, 2026-09-13**: pipeline 214's snap,
installed over the signed-in revision, produced `received track attributed=
true`, `a receive chain is RUNNING`, and 3000 frames in the clear BOTH ways,
with ZERO srtp errors and zero pipeline errors. `received track` had never
appeared on this lane once. The snap carries call media for the first time.

FOUR HYPOTHESES WERE KILLED FIRST and must not be re-proposed: a missing
GStreamer plugin (29 staged, env points at them); the publisher's bus error
tearing the call down (`handleBusMessage` deliberately never calls `failed()`);
a Matrix-level failure (membership, media key, SDP and ICE all verified good);
and OpenSSL provider loading (`OPENSSL_CONF=/dev/null` changed nothing). What
settled it was `GST_DEBUG=dtls*:6,srtpenc:5` INSIDE the confinement: DTLS
COMPLETES and hands srtpenc a correct 30-byte aes-128-icm key, and libsrtp
fails to initialise with it.

**AND DO NOT PUSH TO `main` WHILE A PIPELINE IS RUNNING.** `resolve-source`
pins the SHA; every later job re-clones `main` and refuses a different commit
(`error: source ref moved`). Pipeline 211 lost two jobs to exactly that. That
is the guard working — trigger, then hold.

**2026-09-13 — THE FIVE-MINUTE MATRIXRTC MEMBERSHIP EXPIRY IS CLOSED: PASS,
and it is the headline of a full packaged-flatpak GUI sweep.** A two-party call
between the packaged flatpak and an AppImage was held **nineteen and a half
minutes**, ~4x `MEMBERSHIP_EXPIRY_NO_DELAYED_MS`, and at the end both clients
still read `session read room participants= 2` with both `frames in the clear
in` counters climbing and no `frames dropped: no key in` anywhere. That is
`expires_for_refresh()` working: before it, the membership died a fixed five
minutes after the JOIN however often it was refreshed, lopsidedly (still heard,
hearing nobody). Automation-driven, not Rokas.

Everything else PASS in the same sweep, all on the confined flatpak: screen
share through the portal — `remote_fd= true` on the sharer, and the RECEIVING
client's stage tile drew the sharer's desktop, captured from the AppImage's own
window on the default RHI backend, NOT the sharer's self-view and not
`QT_QUICK_BACKEND=software` (the distinction §16's WITHDRAWN 1 exists for);
share audio; **call audio FRAMES both directions** (clear-frame counters, in
and out — audibility NOT TESTED, nobody listened); **group power control** (Member -> Moderator
-> Member, real `m.room.power_levels`); threads (panel, reply, summary card,
and §8 held); the Ctrl+Shift+K command palette EXECUTING an action; a real
freedesktop notification with Reply/Mark as read; the updater (installation
type "Flatpak", check reaches the server); local message search INCLUDING an
encrypted room, whose index sits inside the flatpak's own data dir; token AND crypto-store persistence
across a restart; and §6's rule live — relaunched with no session bus, the
unreadable secret store did NOT read as a missing account.

Both of the desktop notification's ACTIONS were pressed, not merely shown:
**Mark as read** moved the window caption from `(1 unread)` to clean and sent
two real read receipts, and the toast's inline **Reply** put a message into an
ENCRYPTED room, decrypted on the peer, from a client whose window was never
focused. And the call stage was checked against a known state: two tiles with
the right names, and a crossed-microphone badge that appeared on the PEER's
tile and not the local one when the peer pressed Ctrl+Shift+U.

FOUR defects found and fixed (Activity Center rows baking raw ids in for the
session plus its silent reconcile consequence; Updates contradicting itself;
the Space Home row running off a narrow pane; an edited thread root pushing
its summary card off the bubble), plus a fifth that could NOT be fixed here —
the snap is not signed in — **one claim WITHDRAWN before it was acted on** (shortcuts are NOT dead under a menu — Lightning's menus are
in-scene popups, not `xdg_popup`s), and two non-defects recorded so nobody
"fixes" them (Lithuanian date dividers are `LC_TIME`; the three-hour timestamp
gap is the container's missing TZ). **The SNAP could not be swept — it is not
signed in**, and so is recovery/key-backup setup, which would put a generated
recovery key in a screenshot. Detail in `docs/round-history.md`, 2026-09-13
(afternoon).


**2026-09-12 (evening) — THE WINDOWS CAMERA'S 10-FPS CEILING IS CLOSED: PASS.**
On the laptop's Windows guest with its USB webcam passed through, the portable
build that carries `libgstjpeg.dll` negotiates `image/jpeg 1920x1080 @ 30/1`
where the released 0.9.4 negotiated `video/x-raw YUY2 1920x1080 @ 5/1` —
`camera chain= mjpg (jpeg elements present)`, 500 frames delivered and
climbing, a real picture on screen, and `firstCaptureMs= 424` against the
794-811 ms the raw path cost. Same guest, same camera; the only variable is
that the shipped package finally contains the plugin. The packaging half of
that is builder image v6 plus the plugin being REQUIRED again and `jpegdec`
being probed against the extracted package under Wine.


**2026-09-12 (afternoon) — THE WINDOWS "NO VIDEO" DEFECT IS CLOSED, on a
PICTURE: PASS.** Automation-driven on the laptop's Windows 11 guest, not
Rokas. With the GL probe choosing `Direct3D11` the guest RENDERS a remote
screen share — the receiving tile carries the sending machine's live desktop —
where the same guest, same share and same clear-frame counters previously
advanced past 1000 against an empty rectangle. The renderer was the only
variable, and the defect was our own probe, never packaging. In the same
session: the WINDOWS TRAY-BALLOON notification displays and its CLICK routes to
the right room, both live for the first time. And FOUR convincing "defects" on
that guest — no call banner, no Join, no room-list glyph, a `?` facepile —
were its CLOCK, seven hours ahead in UTC, which made every `m.call.member`
expiry read as past. Check `date -u` on the host against a guest log's own `Z`
stamps before believing anything about call state on a VM. Detail in
`docs/round-history.md` and `docs/open-items.md`.


**2026-09-11 (evening) — CALLS AND PER-PARTICIPANT VOLUME, LIVE ON TWO
CLIENTS: PASS — AND TWO OF THIS ENTRY'S ORIGINAL CLAIMS WERE WITHDRAWN ON
2026-09-12.** What stands: a two-party call with clear-frame counters
advancing both ways on both clients; 0% muting; 200% amplifying. Driven by
`scripts/gui-suite-calls.sh`, which is now the tracked form of those checks —
every assertion on an engine log line or a value on disk, never on a picture.
**NOT TESTED: audibility** (nobody listened; what is proven is that the value
reaches a real GStreamer `volume` element).

WITHDRAWN 1 — "a screen share the other client RENDERS". The suite asserts
`frames in the clear in`, and `SfuMediaEngine.cpp` says in its own comment
that the crypto counters climb either way, so a tile that never attached its
sink is indistinguishable from working video. It is worse than insufficient:
MEASURED 2026-09-12 on one Linux client, one call, `QT_QUICK_BACKEND=software`
the only change, that counter climbed past 500 **against an empty rectangle**.
Qt Quick's software adaptation has no node type for video at all
(`QSGSoftwareRenderableNode::NodeType` lists rectangles, glyphs, images and
nine-patches; `QSGVideoNode` is none of them, and the path is RHI-only while
the software context has no RHI). The honest claim is: frames reach the other
client's decryptor in the clear. **That a picture was drawn is NOT asserted by
any automated check we have**, and on a host with no usable GL it is false.

WITHDRAWN 2 — "the value surviving a `systemctl --user restart`". The restart
persistence was proven ON DISK ONLY. `21f4a1a`, a later commit, records that
THE STORED VOLUME NEVER REACHED THE ENGINE — "the slider read 200% and nothing
had reached the audio graph" — so the restarted client could not have applied
it at the time of that run. Fixed in `21f4a1a`; **NOT TESTED since**. The round also fixed a bell that
HID REAL UNREADS and carries the generalised lesson — a consumer of derived
state must listen to the writer of that state, not to the signal named after
the same noun. Detail in `docs/round-history.md`, 2026-09-11 (evening).

**2026-09-11 — the send path, live, with `scripts/test-netproxy.py` instead of
root.** reqwest honours `HTTPS_PROXY`, so one app's network becomes
controllable: throttle it and an upload is samplable, cut it and that app
alone goes offline. PASSES: the upload percentage advances on a determinate
bar; `cancel_too_late` says the message had already been sent; and Retry
recovers a room whose send queue matrix-sdk had disabled. STILL OPEN: a local
echo can sit at "sending…" after the server has the event, resolving only on a
timeline reload — cause NOT established, and NOT reproducible on demand (two
cut/restore cycles on 2026-09-11 evening did not trigger it). **Run the
capture with `LIGHTNING_SEND_TRACE=1`** as well as `LIGHTNING_RUST_LOG=1`: it
reconciles the timeline's in-flight items against `SendQueue::local_echoes()`
and prints `queued=0 … ORPHANED` exactly when a terminal update was lost,
which is the one thing that separates this from a slow link. Read
`docs/round-history.md`, 2026-09-11, before theorising — it records which
conclusions the evidence does and does not support.

**2026-09-11 — THE THREAD ROOT CARD FOLLOWS AN IN-PLACE CHANGE: PASS.**
Editing a thread ROOT from the room timeline with the panel open updates the
card without a reopen. It renders the root from a SNAPSHOT refreshed only on a
lifecycle change and on countChanged, and a late decryption, an edit, a
redaction and a sender-name resolution all arrive as in-place Sets that change
no row count — so an undecryptable root stayed undecryptable on screen above
replies that had decrypted fine. Driven on a LAPTOP over SSH, which is how GUI
tests run now without touching the maintainer's desktop.

**2026-09-11 — THE THREAD EDIT IS LIVE-VALIDATED: PASS.** The fourth and last
of the "address the event on the timeline that HOLDS it" family, driven
through a real thread panel against a real homeserver: the edit applies, the
row carries the `edited` marker and the room's summary card follows — and the
new body survives a restart, so it is the server's copy and not an echo.
Found in the same session: the panel's "N replies" divider read the ROW
count, so date dividers inflated it — it said 3 beside two replies where the
room card correctly said 2. The shipped fix is
`ThreadController::replyCount` and it is **LIVE-VALIDATED PASS** on the real
backend: three replies read "3 replies", and a fourth sent with the panel
open moved it to "4 replies" without a reopen. Getting there caught one more
defect the same way — preferring the SDK's `num_replies` outright showed "2
replies" above THREE visible ones and had not corrected itself 45 s later,
because a thread summary is stale LOW as readily as high. The loaded replies
are a floor now; the SDK's number covers only what lies beyond the window. Two harness facts: the message context menu
survives a `shot_pid` capture (the no-capture-mid-menu rule is about
spectacle's interactive mode) and publishes its own shortcuts, `T` and `E`.
`ydotool key` needs KEYCODES — `28:1 28:0` for Return; a key NAME types
nothing and reports success.

**2026-09-10 — the first GUI validation of anything above 0.9.4, and it was
AUTOMATION-driven on a throwaway fixture account, not Rokas.** Four PASSes,
detailed in `docs/round-history.md` under 2026-09-10 (night): the room mirror
is retired only for the room you LEFT (`rows= 107 -> 60`); local search was
driven from the GUI for the FIRST TIME (it is the find bar's History scope,
Ctrl+F) and its "load more" now walks to the last row instead of spinning; the
Appearance theme cards render with the ring on the active theme; and 70 rapid
sends all landed, drained over ~3 minutes by Synapse's `rc_message` limit
rather than by any defect. Two-account behaviour, thread-panel identities, the
sticker grid and leaving a Space remain NOT TESTED — do not promote them.

**2026-08-30 — THE GPU SCREEN-SHARE SCALE PATH WORKS ON FOUR ENVIRONMENTS,
AND TWO GPU VENDORS.** `screen share scaling on the GPU` confirmed on: NixOS
from source (NVIDIA), a packaged Windows portable build (NVIDIA), the Fedora
RPM (Intel), and the Flatpak (Intel) — the last two on a laptop, with share
audio and a real two-participant call carrying a distributed media key. It is
now the DEFAULT everywhere rather than opt-in.

The Windows numbers are the ones that justify it: a game held 225 of 240 fps
while sharing, and the capture fed the encoder 1:1 (1000 delivered, 1000
encrypted) where the CPU path at 60 fps delivered ~500 against ~1500 — i.e.
`videorate` tripling every real frame, so two thirds of the encode and
encryption was the same picture.

`gstreamer initialised bundled= false` is CORRECT for the RPM and the Flatpak
(system and runtime GStreamer respectively); only the AppImage bundles.

**AND THE APPIMAGE SHIPPED WITHOUT THE PLUGIN, which nothing could have
caught.** The 0.8.2 AppImage logged `screen share falling back to the CPU:
GStreamer element "glupload" is not available in this build` — `libgstopengl`
was simply not in `GST_REQUIRED_PLUGINS`. The engine probes for the element
and degrades, so a missing plugin can never fail a build or a call: GRACEFUL
FALLBACK AND SILENT ABSENCE ARE THE SAME OBSERVABLE unless something asserts
the payload. Third time this shape has bitten — sctp on Windows, ximagesrc,
now opengl — and `validate-appimage.sh` now names it, as it already named
those two. The same gap existed on Windows in a SECOND list:
packaging/windows/Dockerfile stages into the builder SYSROOT,
stage-windows-runtime.py stages into the shipped ZIP, and updating one is not
updating the other.

NOT COVERED: whether the GPU path survives on a machine with no usable GL —
the new `gpuShareChainUsable()` pre-flight is written for that case and has
never been observed declining. macOS is untested entirely.

**2026-08-29 — SCREEN SHARE AUDIO REACHES ELEMENT, on Linux.** Confirmed on
a real desktop into an ENCRYPTED room: Element hears what the sharing
computer is playing. First time share audio has ever left this client.

The log carries the whole path — `share audio published`, then
`negotiation needed: offering 2 track(s)` and an answer at **3 sections**
(the SFU accepting the added track), then TWO independent
`frames encrypted video=false` counters running side by side, which is the
microphone and the share audio as two separately encrypted Opus tracks.

NOT covered by that confirmation: Windows (the capture element differs and
has only been verified to EXIST and to carry a `loopback` Boolean, by
running the shipped SDK's own gst-inspect under Wine — Wine answers
metadata questions, not whether WASAPI captures); the RECEIVE direction,
i.e. whether Lightning plays share audio someone else sends; and whether a
receiver handles two audio tracks from one participant.

Seen in the same capture and NOT diagnosed: `frames dropped: no key in
video=false` climbing on the receive side, with `sfu joined others=2`
against `media key distributed targets=1`. The leading explanation is a
GHOST MEMBERSHIP from the evening's repeated Ctrl+C exits — the log says
`no MSC4140 delayed retraction armed — an unclean exit will leave this
membership until it expires` — which is the mechanism recorded under
"media key targets=0". Not established, and not attributable to the share
audio change either way.

**2026-08-27 — THE WINDOWS CAMERA AND THE WINDOWS SCREEN SHARE BOTH WORK.**
Confirmed on a real packaged Windows build (project 7 pipeline 135, a
NON-PUBLISHING snapshot from `9f829a3`): the camera sends live video instead
of one frozen frame, and screen sharing works. Earlier the same day, from the
same tester on the pipeline 134 artifact: selecting a MONITOR works with two
monitors attached, and a window share of File Explorer is correct.

CONFIRMED LATER THE SAME DAY, on the pipeline 136 build: the camera works,
and a WINDOW share is correct — right aspect ratio, and resizing the window
mid-share does not break it. So the three headline defects of this lane are
all closed on Windows.

WHAT IT STILL DOES NOT COVER: closing a shared window while sharing, the
picker's grid rework, the call-UI layout fixes, and anything at all on macOS.
Do not promote those to tested.

STILL WRONG at the time of that confirmation, reported with a screenshot and
fixed afterwards in `008ccfd` (so ITSELF not yet re-validated): with a share
running, dragging the call panel small collapsed the picture to a sliver and
the spotlight's overlay controls drew across its top edge in half.

**2026-08-26 (later still) — RAISED HANDS INTEROPERATE WITH ELEMENT.**
Confirmed working on a real desktop: a hand raised in Lightning is seen in
Element and vice versa. The wire format was read out of element-call's own
source rather than guessed at (§16), and it was right first time — which is
the whole argument for reading the reference implementation.

Two things from the same session log, neither of them the feature under test:

* **The screen-share startup hold is 77 ms**, measured rather than reported:
  `publish first encoded frame screenShare=true afterPublishMs=135
  firstCaptureMs=58 rateStageHoldMs=77`. The open defect below describes
  "5-10 s previously, 1-2 s on a restart" — this is the first NUMBER anyone
  has had for it, and it says the `videorate` hold is no longer the cost on
  this machine. It is ONE capture on ONE desktop and the cause is unchanged;
  it is evidence about that share, not a fix.
* **Switching accounts mid-call stranded the membership** (`retraction could
  not be dispatched`), because the teardown ran after the Rust client was
  released. Fixed the same day; live re-validation NOT TESTED.

**2026-08-26 (later) — screen share STOP and RESTART, against Element.**
Confirmed working on a real desktop: stopping a share genuinely stops it —
the far end's tile clears instead of freezing on a grey box — and starting a
new share afterwards works, replacing rather than landing beside the old one.
First share near-instant, restart 1-2 s. Microphone loudness (the `webrtcdsp`
AGC) and the per-participant volume curve were confirmed in the same session.

This lane took FOUR rounds and the first three each fixed something real
without fixing the report, which is the lesson worth keeping:

1. `unpublish()` deadlocked the GUI thread against its own streaming thread
   (core dump: `gst_pad_set_active` wanting the stream lock, the encoder
   thread holding it in `do_probe_callbacks`). Fixed with an IDLE pad probe
   plus `gst_element_call_async`.
2. That probe then never fired, because a pad pushing into a webrtcbin that
   is not draining never becomes idle — so the teardown did not deadlock, it
   simply never ran. A leak wearing a fix's clothes. Instrumented: "probe
   installed", silence for three seconds, "probe fired" during teardown.
3. Releasing the request pad dropped the msid and left the section
   `a=sendrecv` — the far end still told it was being sent to, with nothing
   behind it. THAT was the grey box.
4. Setting the transceiver direction to INACTIVE is what the far end obeys.
   Section count must stay stable across renegotiation (an m= section may
   never be removed), so `a=inactive` is the only correct shape, not merely
   the tidy one.

Checked rather than assumed at step 4: livekit-protocol 0.7.12 has NO
unpublish verb for media tracks — SignalRequest carries AddTrack, Mute and an
UnpublishDataTrackRequest for DATA only. Renegotiation is the mechanism, which
is why a MUTE could never do the job: a mute removes nothing, so the stopped
track stayed in the participant list and was rendered forever.

GENERALISE: Lightning's own self-view is tee'd off the CAPTURE, upstream of
encryption and of the SFU entirely. It looking correct says nothing whatever
about what any other client receives, and it looked correct through all four
rounds. When a share is reported broken remotely and fine locally, the
preview is not evidence.

**2026-08-26 — the largest live-validation event this project has had.** Rokas
tested and confirmed WORKING, on a real desktop against real homeservers:

1. **The rail's drag, including drop-to-make-a-folder.** This is the headline:
   the gesture was structurally unreachable through THREE rules and two rounds
   that each believed they had fixed it. It works. Also confirmed: the
   folder-name dialog, the auto-scroll near the rail's ends, and a drop
   BETWEEN tiles reordering rather than grouping.
2. **Space settings and the rail's Space menu — every write.** Name, topic,
   avatar, join rule, canonical alias, the full power-level matrix, Publish to
   Directory, Local Addresses, Mark as read, Mute, Invite, Copy/Share link.
   Every one of those is a real state event and none had ever been sent.
3. **Keyboard shortcuts**, including the design's load-bearing case: Ctrl+B is
   Bold inside the message box and still toggles the room list everywhere
   else, rebinding, and the conflict refusal.
4. **Multi-account against real homeservers** — switching, encrypted-room
   decryption after a switch, notification routing, restart restoration,
   scoped removal. (The switch's 3-5 s FREEZE is a separate open defect below;
   the behaviour is correct, the latency is not.)
5. **Element interoperability** per `docs/element-interop-checklist.md`:
   encrypted both directions, threads, voice messages, video posters,
   reactions, pins, edits, redactions, the key-recovery cycle, and QR and SAS
   verification against Element.
6. **Notifications** — thread replies, server push-rule modes including
   "follow account default", and the retry after reconnect.
7. **The Channels column, the 2026-08-21 UI round across all 11 themes,** and
   the smaller items: hide/show an image without moving the timeline, read
   receipts, presence dots, saving GIFs, the compact link-preview consent box,
   reduced motion, the 24-hour clock, attachment captions.

WHAT THAT CONFIRMATION IS AND IS NOT. It is Rokas exercising each feature and
reporting it works — the only evidence that has ever counted here. It is not a
claim that every FAILURE branch was reached: a write that the server REFUSES,
a power level a homeserver rejects, and a reconnect retry all need a server
that says no, and those paths remain unexercised. Do not re-list the seven
areas above as untested; do not upgrade their failure branches to tested
either.
