# Tester report, 2026-09-06 — the 0.9.3 list

From a user running the Nix flake build (which they confirm now works). None
of these is flake-specific as far as they can tell. Recorded verbatim in
substance, with a first-pass reading of the code where I have one; nothing
here is diagnosed until it is measured, and each item says which it is.

## 1. Stickers do not animate

An animated GIF sent as a sticker does not animate in Lightning. The same
sticker, sent FROM Lightning, animates in Element when viewed there — so the
event and its media are fine and this is our render path.

**Strong lead, from the code, not yet measured.** The delegate animates only
when it believes the media is a GIF, and that belief has exactly one source:

    readonly property bool isGif:
        (model.mediaMimetype || "").toLowerCase() === "image/gif"

An `m.image` attachment reliably carries a mimetype. A STICKER does not:
`MsgLikeKind::Sticker` in rust/src/timeline.rs only emits `media_mimetype`
`if let Some(mime) = &info.mimetype`, and MSC2545 makes that field optional
-- a fact this project already recorded when an absent sticker mimetype let
SVG bytes reach the image decoder. So a sticker whose sender omitted the
mimetype has an empty one here, `isGif` is false, the still `Image` is drawn
instead of the `AnimatedImage`, and it never animates. Element animating the
same sticker fits: it does not need us to have been told the type.

To confirm, look at one such event's `info` and check whether `mimetype` is
present. If it is absent, the fix is to stop trusting the sender's metadata
for this and ask the bytes -- the media bridge already sniffs them, because
that is how it refuses SVG. A GIF is unambiguous from its first six bytes.

Note the same reasoning covers WebP and APNG stickers, which have never
animated either and nobody has reported.

## 2. Media in room information always opens the first item

Room information, Media tab, click any picture and the viewer opens the LAST
one (reported as "always shows the first" and "always opens the last" in the
same breath — either way, it opens the wrong item, and the index is not
reaching the viewer).

**DIAGNOSED, from the code.** `ImageViewerOverlay.openFor(mediaKey, httpUrl)`
builds its list from `app.timeline.imageEntries()` — the images in the
LOADED TIMELINE — and searches it for the key it was handed. The Media tab is
`MediaHistoryModel`, a separate walk back through the room's history that
deliberately reaches media the timeline has never loaded. So a picture from
months ago is not in `imageEntries()` at all, the search fails, and:

    if (currentIndex === -1 && entries.length > 0)
        currentIndex = entries.length - 1

it falls back to the last entry of the wrong list. That is the reported
"always opens the same one", and it will be the newest loaded image rather
than the one clicked.

The fallback is the bug: it turns "I could not find what you asked for" into
"here is something else". The fix is for the viewer to accept the list it
should page through, so a Media-tab click browses the media history, and for
a miss to open the requested item alone rather than substituting a neighbour.
Worth checking the pinned and search paths for the same fallback.

## 3. Screen share audio echoes the whole system back

Sharing with sound captures the entire system output, Lightning's own call
audio included, so the other participants hear themselves.

This one is a design gap rather than a slip: the capture takes the default
sink monitor. What it needs is to exclude our own playback stream from the
captured mix. PipeWire can express that; the work is choosing the right
mechanism and proving it on a real call.

## 4. Bridge tags appear only on direct messages

Bridged rooms show which platform they bridge to (Discord, Messenger) but
only for DMs, and not at all on one end-to-bridge-encrypted Messenger chat.

The tester is unsure whether this is intended. Worth establishing what the
bridge actually advertises before changing anything: it may be that the room
carries no such state and the DM path infers it from something the room path
cannot see.

## 5. A joiner cannot be heard, and their avatar does not load

The tester starts a call, someone joins, and they cannot hear them; the
joiner's profile picture also fails to load. They believe the other person
COULD hear them. They intend to re-test and suspect their own configuration.

The one-directional shape plus a failing avatar is interesting: an avatar is
plain media over HTTPS and has nothing to do with the call, so two unrelated
subsystems failing together points at something shared underneath. Needs
their log before anything else.

## Context worth keeping

They tried the AppImages under NixOS `appimage-run`: 0.9.1 crashed when they
clicked the tray icon to show the window, and 0.9.0 worked for a while and
then stopped responding to the tray icon. They note other AppImages fail for
them under `appimage-run` too, so they suspect NixOS.

Both AppImage symptoms are plausibly already addressed in 0.9.2 — the Wayland
client library that made 0.9.1 abort during graphics setup (GitHub issue #9),
and the tray requiring a full widget application. Neither is confirmed for
this reporter's case, and the tray-click crash specifically has never been
reproduced here. Ask them to re-test on 0.9.2 before treating it as open.
