# Round history

Moved out of `CLAUDE.md` §16 on 2026-09-03: that file had reached 150,397
characters against a 150,000 limit and was being truncated, silently dropping
its own tail — sections 17 to 19 — from agent context, exactly as §7 was moved
to `docs/feature-contracts.md` on 2026-08-28. Nothing was deleted; the whole
block is below unchanged.

This is a LESSON INDEX, not an inventory, and it is organised by THEME rather
than chronology. Source and `git log` remain authoritative. The standing
warnings, the live-validation record and the open-items inventory all stay in
`CLAUDE.md` §16 — only this block moved.

By THEME, not chronology, and reduced to rules, refutations, deliberate
decisions, measured numbers and live status. Features are §7; the caps
contract, the refutation rule and the probe rule are in the standing warnings.

#### 2026-09-11, the thread edit live, and a reply count that counted rows

**THE FOURTH THREAD DEFECT IS LIVE-VALIDATED: PASS.** Driven through the GUI
on a real homeserver with the fixture account, on a build carrying the fix.
Sent a root, opened its thread, sent a reply, then right-clicked that reply IN
THE THREAD PANEL and chose Edit. The edit loads into the ROOM composer — which
is the whole reason this defect existed, and the screenshot shows it plainly:
"Editing message" sits in the room's composer at the bottom left while the
message being edited lives in the thread panel on the right. Submitting it
applied the edit: the thread panel row reads the new body with the `edited`
marker, and the room's own thread summary card updated to match. No error, and
no `edit_rejected` anywhere in the session log. Before the fix this path
failed every time with "The edit could not be applied."

Worth keeping for the next GUI round: the message context menu SURVIVES a
`shot_pid` capture, so the earlier "never screenshot between opening a menu
and clicking it" rule is about spectacle's interactive mode, not about every
capture. And the menu publishes its own shortcuts — `T` for Reply in thread,
`E` for Edit — which are a steadier target than a measured offset. `ydotool
key` takes KEYCODES (`28:1 28:0` for Return), not key names; `keypid $PID
Return` types nothing and reports success.

**AND THE SAME WINDOW SHOWED TWO DIFFERENT REPLY COUNTS.** Found while doing
the above, not reported: the thread panel's "N replies" divider said **3**
beside a thread holding **2** replies, while the room's summary card for the
same thread said **2**. The card is right — it reads the SDK's `num_replies`.
The panel computed `app.thread.model.count - 1`, subtracting the thread ROOT
and nothing else, and `count` is the ROW count: date dividers, the read marker
and the timeline-start row are all rows. One date divider was the whole
discrepancy, and a thread spanning several days drifts by one per day. The
divider's own visibility gate (`count > 1`) had the same flaw, so a thread
with no replies at all could show the divider once it crossed a day boundary.
`TimelineModel::realCount` states the distinction where it only has to be
stated once — `count` counts rows, `realCount` counts events — and
LIVE-VALIDATED PASS on the same thread that produced the report: the divider
reads 2 beside the card's 2, with the date divider still present. That PASS belonged to the
`realCount - 1` revision; the shipped `ThreadController::replyCount` is
LIVE-VALIDATED separately below.

Review round 8 then found that `realCount - 1` was only two thirds of a fix,
and both remaining holes are ones QML cannot even see, so the answer moved to
`ThreadController::replyCount`. The `- 1` assumed the root IS a row, which
`rootInfo()` itself documents is not always true — it falls back to the ROOM
timeline while the thread snapshot is arriving — so in that window it
UNDERCOUNTED instead. And a loaded count can never agree with the card for a
long thread at all: the thread timeline is windowed and paginates lazily, so
it would disagree by LENGTH rather than by date dividers. The controller
prefers the SDK's `num_replies`, falls back to the loaded count, and
subtracts the root only when the root is really a row. One trap inside that:
`ThreadReplyCountRole` answers 0 rather than -1 when the SDK summary is
absent, so through the ROLE "the server says none", "no summary yet" and
"nothing indexed" are one value. A first revision took `qMax(role, loaded)` to
dodge that, and round 9 showed the ambiguity does not exist one layer down:
`TimelineEvent::threadReplyCount` is -1 in exactly the unknown case, so the
preference is expressible rather than approximated — and `qMax` would have
pinned the count HIGH when replies are redacted and a stale `num_replies`
outlives them. The root is also looked up in the ROOM timeline when it is not
a row of the thread model, which is the very case the SDK's number exists for.

**AND PREFERRING THE SERVER'S NUMBER OUTRIGHT FAILED ON SCREEN, WHICH IS THE
ONLY REASON IT WAS CAUGHT.** Every unit test passed and two review rounds had
approved the shape. Driving it through a real thread panel: sending a third
reply left the divider reading "2 replies" above three visible ones, and 45
seconds later it still did. A thread summary is stale LOW as readily as high,
and low is the direction a reader can see — the label sits directly above the
rows it counts. So the loaded replies are a FLOOR and the SDK's number covers
only what lies beyond the loaded window: `qMax(known, loaded)`.

That is the shape review round 9 talked me OUT of, on the argument that a
stale-high `num_replies` outliving redacted replies would pin the count up.
Checked rather than traded off: `onEventRedacted` sets `redacted` and removes
nothing, and a redacted event is not virtual, so `realEventCount` does not
fall on a redaction and the hazard does not arise from that direction. What
can still happen is the server decrementing `num_replies` while the redacted
row remains, leaving the divider one above the room's card — recorded in the
code as the accepted cost, because the card describes the thread from outside
while this label describes the list underneath it.
LIVE-VALIDATED PASS on the shipped code: three replies read "3 replies", and a
fourth sent with the panel open moved it to "4" with no reopen — which
exercises the announcement path below as well.
GENERALISE: a number rendered immediately above the things it counts is
checkable by eye, and that is the check to run. Two review rounds and a full
unit suite passed the version that contradicted itself on screen.

**AND THE PROPERTY DID NOT ANNOUNCE THE ONE ARRIVAL IT EXISTS FOR.** The SDK
summary lands as an in-place Set on the root row, and `onEventChangedAt` emits
`countChanged` only when a row's virtualness flips — so wiring
`replyCountChanged` to `countChanged` and `stateChanged` alone meant the
divider kept the loaded count until some unrelated insert happened to fire.
It listens to `dataChanged` now, filtered to the root row, and every source
goes through a recompute that emits only when the ANSWER moves, so one Set per
receipt cannot become one re-render per receipt. The FILTER's own first draft
resolved the root's row to test the range — and `rowForStableId` is an
unconditional linear scan that does not use the `rowIndex` hash, so that put
an O(n) lookup in front of a free range test on every SDK Set, which is
receipt frequency. This file records paying for exactly that shape twice
before. It compares event ids over the announced range instead (one row, in
practice), and the room-length fallback scan runs only when the thread model
does not hold the root at all.
GENERALISE: order a filter's tests by cost, cheapest first — and check what
the "cheap" lookup actually does before believing it is cheap.
Mutation-proven both ways: reverting to the row count minus one fails
`replyCountIsRepliesNotRows`, and dropping the `dataChanged` connection or
preferring the loaded count fails
`theSdkSummaryWinsAndItsArrivalIsAnnounced`.

The same review caught the signal half. `realCount` was published with
`NOTIFY countChanged`, and `onEventChangedAt` is the one `m_events` mutator
that deliberately does not emit it — correctly, because an in-place Set
cannot change the ROW count. It can change the REAL count, by replacing a
virtual row with a real one or the reverse, so anything bound to `realCount`
kept the old number. It emits now when virtualness flips.
GENERALISE: reusing an existing NOTIFY for a new property means inheriting
every place that signal is deliberately NOT emitted. The restart that check needed also confirms the
thread edit reached the SERVER rather than a local echo — the edited body and
its `edited` marker came back from a cold start.
GENERALISE: a label that says how many MESSAGES there are must never be
derived from a row count, in any view that synthesises rows. Lightning
synthesises three kinds.

#### 2026-09-11 (evening), a share volume nobody saved, and a hypothesis the instrument refuted

**THE REPORT WAS "PARTICIPANT VOLUME IS NOT SAVED"; THE PARTICIPANT VOLUME WAS
FINE.** Tested before assuming, twice: the value reaches DISK (asserted against
the ini file itself, because reading it back through the same QSettings would
be answered from the cache) and it survives a fresh controller. What was never
saved is the SHARE volume — `m_shareVolumes` is a per-process QHash keyed by
share id, and shares render as their own tiles, so "another person's volume"
covers that control too. It failed twice over: across a restart, and WITHIN one
call, because a share that stops and restarts returns under a NEW share id, so
the level applied to exactly one share and then evaporated. Now stored under
the OWNER's user id, in its own namespace — turning down someone's noisy game
share is not the same wish as turning down their voice, and one key for both
would make each control silently move the other.
GENERALISE: when a report names a control, check the NEIGHBOURING controls that
look the same to the user. The reporter describes what they see, not which
class handles it.

**AND MY FIRST TESTS FOR IT PASSED ON BROKEN CODE — the fourth time today.**
The test config file outlives the process, so a value written by an earlier
successful run was still there, and the mutated build read THAT. Both tests now
reset to the neutral point and assert the fixture is clean before trusting
anything after it. A store that outlives the process is shared mutable state
between test RUNS, which is the same hazard as sharing it between cases.

**THE OPEN "sending… until a room switch" DEFECT: THE LEADING HYPOTHESIS IS
REFUTED BY MEASUREMENT.** The reconciliation watcher was built to separate two
explanations, and it did. Two cut/restore cycles against a real homeserver
through a throttled CONNECT proxy, on a second machine so nothing touched the
maintainer's desktop:

    in_flight=1 queued=1 orphaned=0     during the outage AND after restore
    orphaned>0 ever:            0
    missed-echo warnings ever:  0

**AND THE CONCLUSION I FIRST DREW FROM THAT WAS WRONG — the same one-way
instrument error as the scroll counters, in a new dress.** I wrote that the
lagged-broadcast hypothesis "has no support". It does not have support, but
this run cannot be what withholds it: THE TWO HYPOTHESES DIFFER ONLY IN THE
DEFECTIVE STATE. A slow link and a lost terminal update look identical until
the timeline is stuck at `sending…` while the server already has the event,
and that state never occurred here — both messages sent and the trace went
quiet on its own, which is the send queue working. During a genuine outage a
queued message really has not been sent, so `queued=1` is the ONLY reading the
instrument could have produced. I measured the healthy regime and concluded
about the defective one.

What this run does establish, and all it establishes: the recipe no longer
reproduces the defect on this build, across two cut/restore cycles. With zero
samples of the defective state, the lagged-broadcast hypothesis is neither
supported nor refuted — and the instrument is now in place to settle it the
first time the defect IS seen.

Two gaps against the recipe this file recorded, stated so the next run closes
them: it says "throttle, cut, send, restore, RETRY, and watch the row against
the ROOM-LIST PREVIEW", and this run did neither the retry step nor the
room-list cross-check. The preview is what established "the server already has
it" in the original sighting, so it is the only thing that identifies the
defective state at all.
GENERALISE, third time today: before reading a measurement, ask what the
instrument would print in the case you are trying to rule out. If the answer
is "the same thing", the measurement is not evidence about that case.

**LIVE-VALIDATED PASS, same session:** the thread root card now follows an
in-place change. Editing a thread ROOT from the room timeline with the panel
open updated the card to the new body without a reopen — the defect fixed
earlier today, seen working rather than argued.

**AND THE SECOND MACHINE IS THE REAL UNLOCK HERE.** GUI tests now run on a
laptop over SSH, so they neither touch the maintainer's desktop nor wait for
him to be away. What that took, all of it non-obvious:
- `systemd-run --user --unit=…` and NOT `setsid`/`nohup`: an app started from
  an SSH session lives in that session's scope and logind kills it on
  disconnect. It died twice, silently, with a clean log ending at window
  placement.
- `QT_QPA_PLATFORM=wayland`: an SSH session has `DISPLAY` but no Xauthority, so
  Qt tried XWayland and aborted on `xcb_connection_has_error`. Forcing Wayland
  also exercises the shell integration this release staged.
- KWin scripting over qdbus works fine from SSH once
  `DBUS_SESSION_BUS_ADDRESS` is exported, and that is what gives geometry, the
  focus guard and the window list.
- `ydotool` and `magick` need not be installed: `nix shell nixpkgs#ydotool`
  covers input, and screenshots can be cropped on the other machine.
- The uinput device needs ~12 s after `ydotoold` starts before the compositor
  routes to it. [[ydotoold-keyboard-dies-silently]] says "restart the daemon";
  a restart alone is not enough.

#### 2026-09-11 (later), four defects an analyst found by looking for known shapes

A read-only pass whose brief was "look for a test that cannot fail, a
diagnostic that reports a constant, a comment that contradicts the code beside
it, a NOTIFY some mutator does not emit — this file records several of each
shipping, treat that as the base rate". It found one of each. That brief is
reusable, and the yield says the base rate is real.

**THE THREAD PANEL'S ROOT CARD WAS A SNAPSHOT WITH NO IN-PLACE TRIGGER.**
`ThreadPanel` copies `app.thread.rootInfo()` into `rootData` and refreshed it
on exactly three things: a lifecycle change, `model.countChanged`, and
`Component.onCompleted`. The card renders the root's body, its `redacted` and
`undecryptable` states, and its sender's name and avatar — and every one of
those arrives as an IN-PLACE SET, which changes no row count. So a thread root
that arrived undecryptable and decrypted later kept "Unable to decrypt this
message" on screen above replies that had decrypted fine, and editing or
redacting the root while the panel was open left the original body. §9 is
explicit that a late key updates the event in place with no restart and no
room switch; the card broke that. `ThreadController` already identified a
root-row `dataChanged` for the reply count, so it now emits `rootInfoChanged`
from the same place and the panel refreshes on it. Predates this week's
countChanged work — `onEventChangedAt` never emitted countChanged at all
before — so it is not a regression from it.
GENERALISE: this is the reply-count NOTIFY defect one layer up, found by
asking "what else in this file reads a snapshot?" after fixing the first.

**AN OVERFLOW ANNOUNCED THAT THE STREAM BROKE AND REPAIRED NOTHING.** The
Rust→C++ queue drops its OLDEST entries at `EVENT_QUEUE_CAP` and injects one
`queue_overflow` marker; the handler logged it, emitted a banner and returned.
What the queue carries is POSITIONAL — timeline diffs at an index, room-list
index diffs — so after a drop every later op addresses a vector that never
received the earlier ones, and only SOME of that is detectable: an
out-of-range index is caught by `DiffOutcome::Invalid`, but a dropped `Set` (a
send-state update, a decryption, an edit) or a dropped insert followed by
in-range ops passes every bounds check in silence, and nothing in the payload
carries a sequence number that would reveal the gap. It now re-snapshots with
the two primitives this file already uses for DETECTED damage — resync the
room list, reload the open room — both idempotent, and the producer injects at
most one marker per episode so it cannot chase its own tail.
GENERALISE: "we told the user it broke" is not error handling when the
program is the only party that can repair it.

**A NOTIFY NOTHING EMITTED, BESIDE A COMMENT ASSERTING IT DID.**
`CustomThemeStore`'s `roles` is declared `NOTIFY rolesChanged` under a comment
saying labels "are translated, so this is re-read on a language change rather
than being CONSTANT". `rolesChanged` was emitted nowhere in the tree — a
mechanical sweep of every `Q_PROPERTY` NOTIFY in `src/` found it the only one
neither emitted nor forwarded. `roles()` builds its labels with `tr()`, and
QML's `engine.retranslate()` does not reach strings a C++ model has already
turned into data, so the theme editor kept the old language until reopened.
The store already holds the `SettingsManager` and already wires
`sessionChanged`; it wires `languageChanged` now, which makes the comment
true rather than weakening it.

**AND A CONTRACT SCAN THAT PASSES IF ITS SUBJECT IS RENAMED.**
`theChannelsPresenterDrawsNoSecondGrouping` read a QML file through a helper
that answers an EMPTY string when the file cannot be opened, then asserted
only that the text does NOT contain something. Rename or move the file and the
case goes green — the one change most likely to break the contract it pins.
Its neighbour one line away already had the `!isEmpty()` guard.
GENERALISE: a purely NEGATIVE assertion over text that might not have loaded
is vacuous by construction. Assert you read something first.

**Three doc claims were also false and have been corrected**, all of the same
kind: an "accepted follow-up" or a "STILL NOT SEEN" that a later round closed
without going back to amend. The cost is an agent re-doing shipped work or
hunting a surface that has been found — CLAUDE.md's local-search paragraph
still said the find bar had never been reached from the GUI, and pointed at
the room-header magnifier, which is the wrong surface.
GENERALISE: when a round closes something an earlier entry lists as open, the
edit to the earlier entry is part of closing it.

#### 2026-09-11, review round 6, and three anchor tests that proved nothing

**A GENERATION GUARD IS ONLY A GUARD IF IT NAMES THE RIGHT COUNTER.** The
thread-edit fix shipped a failure report that could never fire. `edit()`
resolves its timeline through `thread_timeline_for` in the thread lane, which
returns `thread.thread_gen`; the binding was NAMED `room_gen` and handed to
`is_current`, which compares against `self.room_gen`. They are independent
counters and `open_thread` bumps only the thread one, so the test was
unconditionally false and a REJECTED thread edit said nothing at all — the
silent no-op the whole four-defect family exists to remove, reintroduced in
the reporting half of its own fix. `toggle_reaction` had already split the
two; `edit` now does the same. The name is what hid it, so the regression
scan asserts the pairing AND refuses the old name.
GENERALISE: when two generation counters exist, a variable holding "the"
generation is a bug waiting for a reader — name it for its lane.

**AND THE SCAN THAT PINS IT WAS ITSELF WRITTEN WRONG FIRST.** It bounded the
window with `split(marker).next()`, which on a marker that has MOVED returns
the whole remainder without failing — and the rest of `timeline.rs` is full
of `thread_current` call sites, so the scan would have passed on an `edit()`
that had none. Its other assertion, `body.contains("if in_thread {")`, was
already satisfied twice over by branches `edit()` had before this round. Both
replaced: the bound is asserted before use, and the assertions name the exact
pairing — ARM BY ARM, because two independent `contains` checks survive the
likeliest mutation of all, swapping the two arms, which restores the defect
verbatim with both substrings still present. Review round 7 found that hole;
the scan now isolates each arm and fails on the swap, on a negated condition,
and on a collapse to one lane.

**THREE ANCHOR TESTS PASS WITH `maintainViewAnchor()` DISABLED OUTRIGHT.**
Measured, module rebuilt: an `if (true) return` at the top of that function
fails EIGHT cases in `timeline-pane-qml` — the `diag*` family,
`displacedBranchDoesNotFireWhileAnchorDelegateAlive` and
`anchorDelegateSurvivesDistantScrollNeverEvictedFallback` — and
`topEdgePrependKeepsReaderOnTheSameRowMidGesture`,
`nearTopControllerDrivenBatchesCompensateImmediatelyNotChained` and
`viewportFillRunCompensatesEveryBatchImmediately` all PASS. Read that
narrowly, as review round 7 insisted: disabling the function models a MISSING
correction, never a WRONG one, so what it rules out is only that these three
fail for want of a correction. An OVER-firing `maintainViewAnchor()` — one
that corrects a prepend the positive-only guard says needs none — would move
exactly the quantity they measure, so they do guard something real.

**AND THE FIRST VERSION OF THE COUNTER CAPTURE WAS A DEAD INSTRUMENT, WHICH I
DREW A CONCLUSION FROM.** Every `diag*` increment in `TimelinePane.qml` sits
inside `if (scrollTrace)`, and `scrollTrace` reads
`app.timelineScroll.scrollTraceEnabled` — CONSTANT, set once per controller
from `LIGHTNING_SCROLL_TRACE`. The eight cases that read counters all
`qputenv` it; the three anchor cases did not. So the snapshot printed seven
zeros on a correct build, a broken build and any build, and I wrote "every
counter zero, so the machinery never ran" into CLAUDE.md as the reason not to
attempt a fourth anchor fix. Caught in review round 8.
GENERALISE, and this repo already had the sentence for it one file away — *a
diagnostic that reports a constant is worse than one that reports nothing*. A
new reader of a gated counter must prove the gate is OPEN before reading
anything into the value; the three cases now assert
`scrollTraceEnabled()` before the snapshot can run.

**AND THEIR FAILURE TEXT COULD NEVER PRINT.** All three used one QTRY whose
lambda returned false for two unrelated reasons — the anchor's row not BUILT
yet, or the reader having MOVED — and a failing `QTRY_VERIFY` RETURNS from
the test function, so the carefully worded `QVERIFY2` beneath it, the only
place the offsets were ever named, was unreachable. Every failure these three
have ever produced read `returned FALSE ()`. Split: `QTest::qWaitFor` answers
with a bool, the wait covers the row's CONSTRUCTION alone with a generous
budget, and the 2 px bound is then read ONCE with no grace period — strictly
stricter than before, since these cases are named for compensation being
IMMEDIATE and a converge-until-true loop is the one thing that could let a
deferred correction pass them. Cutting the old combined wait to 1 ms showed
all three still passed, so no case was relying on the grace.
GENERALISE: a wait whose predicate can be false for two reasons reports
neither. Wait for the precondition, assert the invariant.

Round 7 found the other half of that: reading the offset the instant the row
appears races QQuickBasePositioner, which assigns `y` in a POLISH pass, so a
correct build could fail. Exactly ONE layout flush is allowed now and the
report says whether it was needed — a single pass inside one batch is not the
chaining these cases are named against, and `!nearTopRunActive()` beside each
call is what guards that. The identity of the measured row is checked too,
because a view row is `count - 1 - sourceRow` MINUS `rowWindowSkip` and any
momentary disagreement resolves to somebody else's delegate; reporting "the
reader moved" for that would accuse the anchor machinery of the mapping's
mistake.

**AND WITH THE INSTRUMENT LIVE, THE FLAKE WAS THE FIXTURE — ROOT-CAUSED AND
FIXED.** Turning the trace on made the counters say something, and a pass/fail
control on the same case said the rest. Same build, same case:

    pass  offsetBefore=+334  row y=723  contentHeight=2231  MaterializedMaxAbsDelta=0
    fail  offsetBefore=-389  row y=0    contentHeight=2115  MaterializedMaxAbsDelta=819

The divergence is in `offsetBefore` — BEFORE the prepend the test performs.
The failing run is one row short and captures its anchor on a different row
(content y 0 rather than 723), and the 819 the machinery measures is the 839
the test then sees. In both runs `ActiveDeferrals == MaterializedFirings`,
which by this file's own reading rule means every firing was deferred; the
machinery behaves identically in a pass and a fail.

The cause is the fixture's readiness check. It waited for
`!controller.pagination()->busy()`, which is NOT "the timeline stopped
growing": `ReverseListProxyModel` paces its reveal at 3 ms a tick, so rows
keep arriving after the controller reports idle, and `positionAtTopEdge()` +
`captureViewAnchor()` sometimes ran mid-growth and picked whichever row was
there. Waiting for `contentHeight` to hold still across three consecutive
reads took the three cases from roughly one failure in five to **24
consecutive clean runs** (p ~= 0.005 under the old rate).
GENERALISE: "the producer says it is idle" is not "the derived view has
stopped changing" whenever anything between them is PACED. Wait on the
quantity you are about to measure.

So the recorded `timeline-pane-qml` anchor flake is closed, and it was never
the anchor machinery. Note what remains untouched: the positive-only guard
still stands on its own evidence, and §16's bar for a fourth anchor fix is
unchanged.

**A "FIX" TO THE MOCK'S COMPOSITE HANDLING WAS A REGRESSION, AND THE COMMENT
I WROTE FOR IT ASSERTED THE OPPOSITE OF THE CODE.** `MockMatrixClient::
findEvent` was changed to reduce a §8 composite to its room, on the stated
premise that the mock "has no separate thread timeline". It has one:
`rebuildOpenThreadTimeline()` stores `m_timelines[composite]`, a list of
COPIES, and `closeThread()` removes the key — so a composite resolves for
exactly as long as a thread panel is open, which is the only window in which
a composite reaches `findEvent` at all. The reduction therefore mutated the
ROOM copy while `TimelineModel::onEventEdited` and `onReactionsChanged`
re-read `client->timeline(m_roomId)` with `m_roomId` being the composite, got
the untouched thread copy, and wrote the PRE-EDIT body back over the row: an
edit that worked would have begun showing the old text. Caught in review
round 7 and reverted the same day.
Nothing failed while it was in, because no test opened a thread on the mock
and then edited through it; `anEditThroughTheOpenThreadLandsInTheThreadsOwnList`
is that test now and it fails on the reverted-away version.
GENERALISE, and this is the part that stings: the comment was written from
the premise rather than from the code, and it was long and confident enough
to look researched. A comment asserting what a collaborator's code does NOT
do is a claim to verify at that code, not at the call site.

**AND A HARNESS FLAG MUST BE NAMED FOR WHAT IT DOES.** `--kbps` throttled
KiB/s, per direction, per tunnel — overstating the rate by eight and hiding
that two tunnels get twice the budget. Renamed `--kbytes`.

**PROCESS NOTE, MY OWN.** The `timeline-pane-qml` failures that opened this
round were produced by running the two CTest trees and a three-run flake
measurement CONCURRENTLY — the exact thing §18 forbids and which the file
already records as having caused flakes twice. Serialized, everything is
green: 203/203, 201/201, 93/93 idle and 93/93 again under 24-way CPU load,
and the case alone 10/10. A measurement taken under a condition the guide
forbids is not evidence.

#### 2026-09-11, the live tests that were "impossible", and the fourth thread defect

**A LOCAL CONNECT PROXY TURNS TWO UNTESTABLE THINGS INTO ORDINARY TESTS.** Two
validations had been recorded as out of reach: the send-queue wedge needs a
mid-send NETWORK FAILURE, and the upload-progress percentage needs an upload
slow enough to sample (21 MB goes out in under two seconds on this LAN). Both
were framed as "cannot be staged for one app without root". That framing was
wrong. reqwest — which matrix-sdk uses — honours `HTTPS_PROXY`, so pointing
ONLY Lightning at a local CONNECT tunnel puts that one app's network under
test control while the machine's own networking is untouched. The tunnel
copies bytes and never inspects TLS. `scripts/test-netproxy.py`: `--kbytes` to
throttle, `touch <ctl>/cut` to drop every tunnel and refuse new ones.

**LIVE PASS — the upload-progress percentage.** Throttled to 120 KB/s, a 21 MB
attachment reported `sending… 13%`, then `20%`, then `43%`, then `66%`, with a
DETERMINATE bar advancing. That is `enable_upload_progress(true)` working:
before that opt-in `report_media_upload_progress` defaulted to false, so
`EventSendState::NotSentYet` carried no progress, `UploadProgressRole` stayed
-1, the bar was permanently indeterminate and the `"%1 • sending… %2%"` string
was unreachable. Both sides of that seam were unit-tested with synthetic
values, which is exactly why nothing caught that the SDK was never asked to
produce them.

**LIVE PASS — `cancel_too_late` says the true thing.** Cancelling that upload
at 66% produced `category= "cancel_too_late"` and the status bar read *"That
message had already been sent, so it could not be cancelled."* Before this
round every cancel category fell through to *"Message could not be sent. You
can retry from the message's Retry action."* — wrong twice over for that case,
since the message DID reach the server and there is nothing to retry.

**LIVE PASS — Retry recovers a wedged room.** With the tunnel cut, a send
failed and the row went to `failed · Retry · Cancel` — which is matrix-sdk
having disabled that room's whole send queue (`locally_enabled.store(false)`,
send_queue/mod.rs:1012). Restoring the tunnel and clicking Retry moved it to
`sending…` and the server got it. **A wedged item is skipped by
`peek_next_to_send` even on an enabled queue, so Retry is the ONLY thing that
can recover one** — which makes this a clean test of the fix rather than of
the sync-recovery edge. Before it, `unwedge()` woke a loop that immediately
re-parked on the still-false flag.

**OPEN, AND NEWLY REPRODUCIBLE — a local echo can stay at "sending…" after the
server already has the event.** In the same session, after the outage and
retry, both the retried message and the interrupted attachment sat at
`sending…` / `failed` in the open timeline for over five minutes while the
ROOM LIST preview already showed them. Switching away and back resolved both
at once: the attachment had in fact completed at 09:04 and the text message
had sent. So the sends were fine and the open timeline's echo state was stale.
WHAT IS NOT ESTABLISHED is the cause. It is consistent with the per-room
`broadcast::channel(32)` dropping a terminal update — matrix-sdk-ui warns
`missed {n} local echoes, ignoring those missed` and does NOT resync, unlike
every event-cache stream in that same file — but it is equally consistent with
the throttled link simply not having delivered the remote echoes yet. Do not
quote a cause; the recipe now exists to find one: throttle to ~120 KB/s, cut,
send, restore, retry, and watch the row against the room-list preview.

**AND THE FOURTH THREAD DEFECT, fixed.** Editing a thread reply failed every
time, for the same reason reacting to one did, and redacting one did, and
retrying one did: `Timeline::edit` resolves against the timeline's own items
and the live room timeline hides threaded events. Edit was the last one left
because it was the only one whose caller had no thread identity to pass — the
thread panel's Edit routes through the ROOM composer. `beginEdit` now takes
the timeline that holds the event, the composer remembers it, and the
composite is decomposed at the FFI boundary exactly as `toggleReaction` does.
**GENERALISE, because this is now four for four: any operation that addresses
an event through `Timeline` must be issued on the timeline that HOLDS it, and
in this client that means every such call needs the thread root threaded
through to it. Before adding a fifth, check the caller can supply one.**

#### 2026-09-10 (night), live GUI validation above 0.9.4

**2026-09-10, the first GUI validation of anything above 0.9.4.** Driven by
automation on a throwaway fixture account (`@lightningtest2`, isolated XDG
profile; the maintainer's own account, store and crypto were never touched —
every launch was checked against `/proc/<pid>/environ` first).

- **PASS — the room mirror is retired, and only for the room you LEFT.**
  `timeline mirror retired room=… rows= 107 -> 60`, once per switch away from
  a 107-row room, never for the room being entered. This is the half of
  `fe2160f` that had NO coverage at any layer: `RustRoomRegistryTest` proves
  `trimToBackgroundBound` as a pure function, and the whole risk was the call
  sites' ordering against `m_timelineTracker.request()`. It needed a room with
  more than 60 rows, so the first attempt on the untouched fixture logged
  nothing at all — an inconclusive run, not a pass.
- **PASS — local search, driven from the GUI for the first time ever.** §16 had
  recorded the surface as unreached. It is the find bar's **History** scope
  (Ctrl+F), which offers `Indexed` and `Server`; Indexed reported "Searching 89
  messages Lightning has indexed, including encrypted ones". Scrolling the
  result list ran CONTINUOUSLY from the newest match to the oldest with no
  snap-back, so `103ab1f`'s "load more" extended the page and then stopped
  offering more. Under the old code each redundant page reset `contentY` to 0,
  so reaching the last row by scrolling was impossible — the scroll itself is
  the assertion.
- **PASS — the Appearance theme cards** render and the selection ring sits on
  the active theme (all four featured cards, plus the eight in More themes).
- **PASS, with a behavioural note — 70 rapid sends all landed.** They drained
  over ~3 MINUTES and the UI showed "sending…" on the tail the whole time. Not
  a defect and not the tracked-task change failing: Synapse rate-limits at
  `rc_message` 0.2/s by default and the burst went out at ~1.5/s. What DID
  change in `5d52126` is that sends now serialize through matrix-sdk's
  per-room send queue on the shared runtime, where before each send got its
  own throwaway current-thread runtime and they raced. Correct — the old shape
  hammered a rate-limited server — but slower, and the long "sending…" tail is
  now the honest observable. **The evidence that settled it was the ROOM LIST
  preview reading `zqxjfixture message 70`**: the echoes were accurate, the
  messages were genuinely still in flight.

NOT covered by any of that: two-account behaviour (the custom-theme leak of
`13a3afc`, the credential states of `e478d6f`/`efd2009`/`69dc232`), thread-panel
identities (`1297856`), the sticker grid (`73aa50e`), and leaving a Space
(`fe2160f`'s rail half). Do not promote those.

**Harness note.** `scratchpad/ui.sh` was rebuilt this session and two things
had changed under it. `gdbus` is not installed on this host — KWin scripting
goes through `qdbus`. And `ydotool mousemove -a` is UNUSABLE here: it put
(400,300) at (2000,0) and then parked every later request at (1,1), because
its absolute axis range does not correspond to this 5120x1440 logical desktop.
RELATIVE moves are exact, so `moveto` aims by delta from `workspace.cursorPos`
and VERIFIES, correcting up to three times. ydotool 1.0.4 also has no wheel
command at all, so a Flickable is scrolled by a real press-move-release drag
with intermediate points (`pdrag`) — a single jump reads as a click.

#### 2026-09-10 (later), four items that were degrading every session

Small round, four unrelated fixes, and three of them turned on the same kind
of mistake: a value that was really a STATE being written as a CONSTANT.

**CLAUDE.md HIT ITS LIMIT AGAIN, AND THE RULE NOW BINDS THE ROUND IN HAND.**
Writing the previous round's entry into §2 the old way took the file to
148,643 characters against a hard 150,000 that truncates SILENTLY, dropping
§§17-19 — the completion-report requirements and the multi-agent protocol —
out of every agent's context. The file's own rule (past ~140,000, move a
section to `docs/` and leave a pointer) had been sitting there being read as
advice for some future editor. Third move: §2's release inventory and its
operational traps are now `docs/release-operations.md`, chosen because they
are needed ONLY during release, packaging or pipeline work — the same
criterion that moved §7 and the round history. 137,759 after. **GENERALISE:
a round's own record belongs in `docs/round-history.md` with a pointer of
three or four lines; if adding yours crosses 140,000, move a section out in
the SAME commit rather than leaving the next session to find the tail gone.**

**THE APPIMAGE'S MISSING `gst-plugin-scanner`, AND WHY "DROP THE Q_OS_MACOS
GUARD" WAS THE WRONG FIX.** 0.9.4 printed `External plugin loader failed` at
every launch: GStreamer builds its registry by dlopen'ing candidates in a
separate helper process, the path to that helper is compiled into
libgstreamer, and it names the BUILD IMAGE. The obvious repair — un-guard the
macOS call to `applyBundledScannerPath()` — would have repointed the deb, the
rpm and the Flatpak too, and those use a system or runtime GStreamer whose
compiled-in path is CORRECT. The AppImage is a third lane and already has a
shape: the AppRun hook exports the variables and the app merely NOTICES them,
exactly as it does for the plugin path. It has to be that way here for a
structural reason as well as a stylistic one — the helper is staged at
`usr/libexec/gstreamer-1.0/`, not beside the binary at `usr/bin/`, so the
macOS "beside the executable" rule cannot reach it. **The scanner variable
also differs from its plugin-path sibling in one way that matters: it names
ONE EXECUTABLE, not a colon-joined list, so the rule refuses a list.**

**AND ITS TWIN GUARD ALMOST DID NOT LEARN.** The hook preserves every variable
it overrides as `APPIMAGE_ORIGINAL_<NAME>` and `UrlLauncher` hands them back
to anything Lightning spawns, because a child that keeps them looks inside an
AppImage mount that may already be gone. `UrlLauncher` carried
`GST_PLUGIN_SCANNER` but not the versioned `GST_PLUGIN_SCANNER_1_0` — the one
GStreamer consults FIRST. Fifth occurrence of "twin guards must learn
together" in this project, so the two lists are now bound by a test that
DERIVES the names from the hook's own source: adding a variable to the hook
covers itself. `validate-appimage.sh` asserts both halves, the executable file
AND the hook line that exports it, because either alone is a silent no-op.

**A PANIC MAY PRINT WHERE, NEVER WHAT.** §6 forbids logging decrypted message
bodies, and Rust's DEFAULT panic hook prints the payload to stderr before
`catch_unwind` ever runs. That is a carrier this crate has actually had: a
`str` slice panic's payload QUOTES the string it was slicing, and 0.9.4 fixed
two byte-offset slices that were slicing message BODIES. It does not reach
`--log-file` (which mirrors Qt's message handler, not the process's stderr),
but it reaches a terminal, a journal, and any log a user is asked to attach.
`install_panic_hook()` prints location and thread only, and **the property is
held by the SIGNATURE — `panic_report_line()` takes no payload, so it cannot
leak one and no filter has to stay correct.** It deliberately does not chain
to the previous hook, because the previous hook is the one printing the
payload.

**AND IT MUST NEVER BE INSTALLED IN THE TEST PROFILE.** `assert_eq!` reports
through the panic hook, and four cases in this crate call `mx_rust_create` —
so installing it there would withhold the message of every ASSERTION FAILURE
in the rest of the binary, turning a readable diff into "Rust panic at
lib.rs:9001". The leak being guarded is a leak to a USER's terminal; `cargo
test` output is a developer's own screen. `cfg!` rather than `#[cfg]`, so the
body stays compiled and type-checked in both profiles.

**A CONSTANT WEARING A PREDICATE'S CLOTHES, ONE LAYER DOWN.**
`InsecureFallbackSecretStore::lastReadFailed()` returned
`m_substitutedForNative || m_lastReadFailed`, and the first term is set at
construction and never cleared — so on any build with a native backend
compiled in whose daemon is not running it was PERMANENTLY true. Every Linux
package carries `HAVE_LIBSECRET`, so that is an ordinary machine with no
keyring daemon or no session bus: every token reads back perfectly from the
fallback INI and the app was told, forever, that it could not read its own
sign-ins. `secretBackendUnavailable()` was stuck true, and
`AccountManager::needsSignIn()` short-circuits on it, so a genuinely EXPIRED
sign-in could never be reported as needing one. **The distinction is one line:
substitution means the native store may hold something this one cannot see,
which makes a MISS ambiguous and says nothing about a value already in hand.**
A miss under substitution stays inconclusive on purpose (§6), and a backing
file that cannot be read outranks both.

**HARNESS NOTE.** The first draft of the three store cases failed for a reason
that had nothing to do with the code: `InsecureFallbackSecretStore`
default-constructs `QSettings`, which resolves its file from the ORGANIZATION
and APPLICATION names, and a test binary has neither — so every read returned
a status error and the store reported failure. Same family as every other
entry under "harness bugs masquerade as findings": before believing a new
fixture, ask what would make it fail for the wrong reason.

#### 2026-09-10, the post-0.9.4 audit debt: nine defects the audits found and the release did not take

Nothing in this round was reported by a user. Every item is an audit finding
that 0.9.4 shipped without, and the ones worth keeping are the ones where the
SHAPE repeats.

**A PER-ACCOUNT CACHE THAT OUTLIVES ITS ACCOUNT IS A DATA-LOSS GUARD, NOT A
FRESHNESS NICETY — third occurrence.** 0.9.4 fixed the hidden-image list and
the ignore list; `CustomThemeStore` is the same defect a third time. The cache
is filled once and returned forever, and a save writes the whole CACHED list
back — so account B's Appearance page listed A's themes, and B's first edit
persisted A's themes over B's record. Permanently, silently. The signal to
invalidate on is `SettingsManager::sessionChanged`, not `loggedOut`: it fires
AFTER the active account id has moved, so a read triggered by it resolves the
INCOMING account, and it covers the switch, an account being ADDED, and the
sign-out of the active one. `RailLayoutStore` had already recorded that
reasoning at its own `connect()`. **GENERALISE: grep for every cache keyed by
nothing whose data is account-scoped; the bug is not that it goes stale, it is
that a write-back persists the wrong account's data.**

**§6's "no readable access token is not no account" had TWO more violations,
and they formed a closed loop with wrong advice at both ends.** With the
keyring locked or the session bus unavailable, every credential read comes
back empty: `switchToAccount` refused with "That account's sign-in has
expired. Sign in to it again.", and `onLoggedOut` skipped every remaining
account and dropped the user to a login form. The advice cannot be followed —
the password login it asks for is bounced by `passwordLoginBlockReason` as
`ExistingStoreNeedsRestore`, back to the switch that just refused.
`signInStateFor()` answers in three states. **The ordering is the whole
finding, and review caught it:** a SUCCESSFUL READ must be tested FIRST and is
the only evidence of `Usable`. Inferring it from "not signed out and the
backend seems fine" locks out an entire shipped configuration — when a native
backend is compiled in but probes unavailable, `SecretStore` substitutes the
insecure fallback, whose `lastReadFailed()` is `m_substitutedForNative ||
m_lastReadFailed` and therefore PERMANENTLY true; every account would classify
`Unreadable` and every switch would be refused on a machine whose tokens read
back perfectly, with advice to unlock a keyring that does not exist. That is
the no-session-bus Linux case §16 already records real users running.
`Unreadable` still REFUSES the switch, deliberately — the switch detaches the
running session first and the restore needs the token that cannot be read — but
it now names the failure and gives the action that can work.

**AN INTEREST SET RECORDS AN OUTSTANDING FETCH, SO NOTHING MAY ENTER IT ON A
PATH THAT DISPATCHES NONE.** `MediaBridge::animatedSource` filled
`m_animatedWanted` and `m_animatedDemanded` ABOVE two early exits — the failure
block and the cache hit that writes the file itself — and neither exit drains
them, because only `onMediaReady` drains and no completion was coming. A key
stranded in `m_animatedWanted` makes `cancelPlayable()` early-return FOREVER:
the queue purge, the in-flight abort and the playable writer's cancel never
run, which is the multi-hundred-MB transfer for a card that is gone that the
cancel exists to stop. `playableSource`'s failure branch had the rule written
down already. Two invariants survive the move: a DEMANDING caller is still
owed a terminal answer when cached bytes turn out not to be an animation, and
`mxcAnimatedSource` still demands nothing.

**"WAS THE PAGE FULL?" MUST BE ASKED AGAINST WHAT THIS REQUEST ASKED FOR.**
Local search paging has no cursor: "load more" re-runs the query with a BIGGER
limit and replaces the rows. The exhaustion test compared against the constant
`kLocalPage`, so every page after the first tested 100 (or 150, or 200) rows
against 50. Once the index held 50 matches, the short page that PROVES
exhaustion read as a full one, `canLoadMore` stayed true forever, and the
list's `onAtYEndChanged` kept firing `loadMore` — each redundant page replacing
the rows inside `begin/endResetModel`, which drops `contentY` to 0 and
re-satisfies `atYEnd`. A spin, not a stall. Count RAW results, before the
filters: they run on this side, so a filtered count says nothing about what
the index had left.

**THE §8 COMPOSITE TIMELINE ID SILENTLY MISSES EVERY ROOM-KEYED LOOKUP.** A
thread model's `m_roomId` is `room ␟ thread ␟ root`, because that is what
addresses the timeline in the backend's diff stream. Nothing keyed by a ROOM is
keyed by it — not the member cache behind `displayNameFor()`/`avatarMxcFor()`,
not `membersChanged` — and `displayNameFor()` answers the bare user id for an
id it cannot find, which every caller reads as "unresolved". So a thread panel
showed localparts and letter avatars for everyone whose name was not already
carried on the event; mention pills fell back to the localpart AND asked
`UserProfileResolver` for a `/profile` they did not need; and
`onMembersChanged` had NEVER ONCE RUN for a thread panel in the whole life of
the feature. **The failure mode is the lesson: the composite is a valid
QString and every lookup returns a plausible-looking answer, so nothing throws
and nothing logs.** `m_realRoomId` is resolved once beside `m_roomId`;
`typingUsersFor()` deliberately keeps the composite so that it and
`onTypingChanged`'s guard still agree.

**A Qt MESSAGE HANDLER IS CALLED FROM ARBITRARY THREADS, AND THIS ONE OWNS A
QFile.** Two writers are unconditional and already shipped — the GUI-stall
watchdog logs from a raw `std::thread`, `PlayableWriteWorker` from its own
`QThread`, both at default-on levels — and neither `QFile` nor `QTextStream` is
thread-safe. So the capture recipe this project ASKS TESTERS TO RUN
(`LIGHTNING_GUI_STALL_TRACE` together with `--log-file`) was precisely the
racing configuration, and the one artifact we ask for could come back
interleaved. The `g_logFile` pointer is now published AND read under the same
mutex; the previous handler is chained OUTSIDE it, because it is arbitrary code
and holding ours across it would invent a lock ordering for no gain.

**A TAG SCANNER THAT SEARCHES FORWARD FROM EVERY `<` IS QUADRATIC ON REMOTE
INPUT — and the first two fixes proposed for it were not fixes.** Message
bodies arrive from any room. Measured on a hostile-shaped body: 24,626 ms of
GUI thread. My first proposal handled a `</` run with no `>` present; the
reviewing agent showed a `</` run WITH a distant `>` is equally quadratic, so
the fix has to be a cursor that never moves backwards, not a special case.
Both shapes are now pinned by tests, and the malformed shapes are pinned by
their exact text output so a "faster" rewrite cannot quietly change what the
reader sees.

**SEVEN FFI ENTRY POINTS SPAWNED UNTRACKED THREADS HOLDING AN `Arc<Client>`.**
`send_text`, `probe_encrypted_send`, `recover_backup`, `reload_timeline`,
`rename_device`, `backup_action`, `backup_progress`. `shutdown_managed_tasks`
could neither wait for them nor report one that died: a send in flight at
logout kept the client alive past the point the store was closed, and a panic
inside any of them was swallowed with no event. They are tracked now, and the
shutdown budget is stated ONCE and bound at COMPILE TIME — `const _: () =
assert!(SHUTDOWN_WORST_CASE_MS + SHUTDOWN_DESTROY_RESERVE_MS <=
STORE_CLOSE_BUDGET_MS, ...)` — so an edit that makes the declared worst case
unreachable fails the build with an explanation instead of timing out in the
field. `rust/src/sfu.rs`'s leave-flush timeout is derived from
`SHUTDOWN_ACTION_JOIN_MS` the same way rather than being a second independent
number.

**A SPACE THE USER HAS LEFT STAYED ON THE RAIL UNTIL THE NEXT SIGN-IN**, and
the fix had to be narrowed twice. `space_list_reset` is a COMPLETE list built
from `joined_space_rooms()`, so absence IS the fact — but only for a JOINED
space: an INVITED one is absent by construction, and erasing on absence alone
would create the invite row from the room payload and destroy it again on the
next space list, so a Space invitation could never be seen or accepted. (My
own fixture caught that one, with the wrong membership string: the parser
wants `"invited"`, not `"invite"`.) And nothing may leave `rooms` while
`order` still names it — `order` is addressed BY INDEX by every room-list diff,
and removing an indexed entry from the map alone is the shape of the
wrong-room deletion this project has already shipped once. Spaces are never
appended to `order`, so that guard is normally vacuous; it is there so a
future producer that indexes one keeps its retirement with the diffs that own
it. The Space exemptions in `applyIndexReset()`/`applySnapshot()` are
untouched and must stay: the ROOM LIST producer never mentions Spaces, so
absence from ITS reset is not evidence.

**THE C++ EVENT MIRROR OF AN OPENED ROOM WAS NEVER REDUCED AGAIN.**
`appendBounded()` bounds the ring for a room the user has never opened; opening
one REPLACES that ring with the SDK snapshot and grows it with every diff — the
viewport fill alone reaches 600-900 rows — and closing the room did not shrink
it, switching rooms did not (the generation tracker forgets the old room
without touching anything keyed by it), and only a sign-out's wholesale clear
ever freed any of it. A session that visited fifty rooms kept every event of
all fifty, while matrix-sdk's own `shrink_to_last_chunk` had already released
Rust's copy. Trimming back to the background cap rather than discarding is the
point: that cap is exactly what the room would hold had it never been opened,
so it restores the documented invariant instead of inventing a second one, and
it keeps the instant pre-snapshot render on re-open.

**HARNESS LESSON, and it cost two hours of measurement.** `timeline-pane-qml`
failed once in a full run and twice in three re-runs, on two DIFFERENT anchor
cases (`topEdgePrependKeepsReaderOnTheSameRowMidGesture` and
`nearTopControllerDrivenBatchesCompensateImmediatelyNotChained`). Different
cases failing on different runs is the signature of timing, not of a defect —
a regression fails the same case every time. The cheap decisive check is not a
rate comparison but a proof that the diff CANNOT REACH the case: `qml/` was
byte-identical to `main` (the anchor logic under test), the fixture sets
`formattedBody` ZERO times (so the tokenizer rewrite cannot move a row
height), and it contains no U+001F (so every `m_realRoomId` reduction is a
literal identity operation there). Also: running a single test FUNCTION
directly is NOT a smaller version of the suite — one QML engine is shared
across cases, and the same function failed 6 of 8 times alone on a tree whose
full-suite runs were 1 pass and 2 single-case failures. And every one of these
numbers was taken with a game at 222% CPU on one of the three recorded
load-sensitive suites.

**Accepted follow-ups from this round's review, none blocking.** (1) On a
machine where a native secret backend is compiled in but unavailable,
`SecretStore` substitutes the insecure fallback and `lastReadFailed()` is
permanently true — so an account whose token really IS gone classifies
`Unreadable` and is told to unlock a keyring that does not exist. **DONE —
closed later the same day by `efd2009`: `InsecureFallbackSecretStore` splits
`lastReadFailed()` (the real per-read failure) from `missesAreInconclusive()`
(structural, and it never softens), which is exactly the split named here.** (2) `MessageHtmlTest` now carries two wall-clock
assertions (complexity is the property under test and there is no branch to
assert on; the headroom is 24x and documented in-source) — so a
`message-html` failure in a full run should be re-run alone before it is read
as a regression, exactly like the three suites §16 already tracks. (3)
**Rust's DEFAULT panic hook still writes the payload to stderr**, and no
`std::panic::set_hook` is installed anywhere in the crate. **DONE — closed
later the same day by `581ba4e`: `install_panic_hook()` runs at the top of
`mx_rust_create` and `panic_report_line()` keeps the location and drops the
payload. Left here because the reasoning below is still the reasoning.** That
matters
because a `str` slice panic prints the string it was slicing, and the two such
panics fixed in 0.9.4 were slicing message BODIES. It does NOT reach
`--log-file` (that mirrors Qt's message handler, not the process's stderr),
but it reaches a terminal or the journal. `CatchPanic` keeps the payload out
of the event and out of every line Lightning logs, which is the half this
round owns; closing the other half means a hook that keeps the location and
drops the payload, and that changes crash diagnostics process-wide.

**One refutation to keep.** Review proposed guarding `retireAbsentSpaces()`
against an EMPTY `present`, on the theory that a transiently short space list
would now erase where it used to blank. Refused with a counter-case: a user
who leaves their ONLY Space produces a legitimately empty payload, so that
guard would skip exactly the retirement the function exists for.
`enqueue_spaces()` does emit an empty array, so the shape is reachable — but
the transient has never been observed and an entry erased by one is re-created
by the next space list or by `room_snapshot`'s walk of `client.rooms()`. The
reasoning is recorded at the function so it is not re-proposed.

#### 2026-09-10, the 0.9.4 round: two user reports, five audits, and a sweep

**The two user reports are the ones worth reading.** Both were reported the
same day in the project's own Matrix room, and in both cases the report's own
framing was wrong in a way that mattered.

- **OAuth sign-in refused outright by continuwuity.** Two independent
  reporters, one on the flake and one on the AppImage, so not a local setup.
  The message names the rule: `invalid_client_metadata: HTTP redirect URIs for
  native applications do not need to specify a port`. We registered the
  loopback callback WITH the ephemeral port we were listening on. RFC 8252
  §7.3 says a native client takes an ephemeral port at request time and the
  server MUST accept any port, so pinning one is invalid metadata.
  **GENERALISE: this had nothing to do with our OAuth code being wrong and
  everything to do with WHICH SERVER we had ever tested against.**
  continuwuity's `client_metadata.rs` refuses `uri.port().is_some()` outright;
  MAS validates the same field in a Rego policy that checks the scheme and the
  loopback host and NEVER LOOKS AT THE PORT. MAS is the only server this flow
  has ever been live-validated against (§2), which is exactly why a pinned
  port survived. Both then strip the port at authorization time, so
  register-portless / request-with-port is the shape both accept, and
  matrix-sdk 0.18 supports it — `OAuthAuthCodeUrlBuilder` takes the request
  URI as its own argument and never reads it back out of the metadata. The
  fix retries once, on the RFC 7591 error CODE and never on prose.
- **"Clicking a notification sends me to a broken room."** Reported as a
  regression between 0.9.2 and 0.9.3. **It was not a regression, and saying so
  was the finding.** `qml/Main.qml` did `app.currentRoomId = roomId` — the
  property WRITE — where every other navigation calls `openRoom()`, and
  `openRoom()` is the only caller of `openRoomTimeline()`, which is the only
  caller of `mx_rust_timeline_open`. So a room entered from a notification
  never got a live SDK timeline: the navigation succeeds, the header and
  composer switch, and the timeline shows only the bounded background sync
  mirror with `paginationReady()` false forever. **AND IT IS STICKY** —
  `openRoom()`'s `alreadyOpen` guard skips the SDK open for a room this path
  has already made current, so clicking the same room in the list afterwards
  repairs nothing. The line is unchanged since 0.6.0; what changed was
  REACHABILITY, because a build without QtDBus had no notifications at all
  until the tray balloon shipped in 0.9.1. GENERALISE: when a reporter dates a
  regression, check whether the PATH is new rather than assuming the CODE is.

**The §18 review earned its keep twice, and the second time is the lesson.**
One non-author reviewer, two passes, six findings, all fixed. First pass
found a regression *the fix itself introduced*: gating the notification emit
on a non-empty room id killed ROOMLESS notifications on Linux, the one
platform where that path always worked — `qml/Main.qml` calls
`raiseIntoView()` BEFORE it checks the room id, and "wants to verify a
session. Open Lightning to review it." has no room by design. Second pass
found that the regression test written for the *first* correction passed on
the very form the first pass had rejected: the composite was built from a
room that was already open, so "reduced and reopened" and "did nothing at
all" were the same observation. **GENERALISE: a fixture that starts in the
state the fix produces cannot discriminate. Start from the state the DEFECT
produces.**

**Live GUI sweep, on two instances against real accounts.** Nine passes, and
three of them are firsts:

- The Home **"Your spaces" strip renders**. It could never have rendered on
  any account: `spacesSummary()` filtered `m_rooms`, and `passesScopeFilter`'s
  first line drops exactly `isSpace && Joined`, which is the predicate
  spacesSummary requires. Dead since it was written.
- The find bar's **source strip and coverage line**, both recorded in §16 as
  never having been observed from the GUI.
- **A redaction removes the row from the local plaintext index, and a forced
  re-index does not bring it back.** The coverage line is the evidence and it
  is the cleanest number this round produced: 55 messages indexed, 56 after
  indexing the new one, 55 after the redaction, still 55 after re-indexing.
  Before this round `SearchIndex::remove_event` had NO CALLER on the real
  backend at all — `eventRedacted` is emitted only by the mock and the legacy
  HTTP client — so §6's single named obligation for the search-index
  exception was simply not met.
- Leaving one room removes **exactly** that room, every other row intact, no
  `malformed diff rejected` line. That is the id-checked positional removal.
- Escape closes Settings with the room info panel open, and still closes the
  info panel afterwards — the two-enabled-Shortcuts ambiguity fixed, and the
  new `app.currentScreen === 1` guard proven not to over-fire.

**A fixture that cannot discriminate is not a pass.** The sweep plan for this
round explicitly marked which cases could not fail on this fixture (the
room-list storm on a small account, the state-bound case on a two-member
room, the account-switch stall on tiny accounts) and required them to be
reported as "cannot discriminate" rather than PASS. The room fixture was
built out to 38 rooms and 2 Spaces specifically so the room-list cases could
bite.

#### 2026-09-09, the membership read a call cannot trust (GitHub issue #10)

Reported by an outside user, with a full call log, after two earlier rounds on
the same issue had each fixed something real and not fixed the report: a
two-party encrypted call in which the reporter's own participant list fell
**2 -> 1 -> 0** while the peer was demonstrably still in the call. Every
symptom is that one list: the peer drawn as a question mark (no membership, so
no name and no avatar), `media key distributed index= 3 targets= 0
sfuPeers= 0`, and 1002 frames that decrypted followed by frames that could
not, because the peer had rotated their key to a target set that no longer
contained this device.

- **THE STORE-FIRST GUARD DID NOT IMPLEMENT ITS OWN DOCUMENTED CONTRACT.**
  `read_membership_events` asks the state store first and the homeserver only
  when the store has nothing usable, and its comment defines usable as "at
  least one membership that is neither retracted nor expired". The code tested
  `content != {}`. So a membership left behind by an unclean exit — which the
  publish path logs as a known consequence of a homeserver without MSC4140,
  "an unclean exit will leave this membership until it expires" — answered
  "the store is fine" for as long as the ghost survived, and the network read
  that exists to cover a lagging store could never run. It now parses the
  event and checks the deadline, which is the same parse the session read
  itself performs, so the two can no longer disagree.
- **AND "THE STORE HAS SOMEBODY LIVE IN IT" IS NOT EVIDENCE THAT IT HAS
  EVERYBODY.** That is the half the corrected predicate still cannot cover,
  and the reporter's log shows it in the very first read of the call:
  `sfu joined others= 1` and a media key received from the peer, against
  `session read room participants= 1`. Our own membership echoing back
  through sync is enough to satisfy any store-first rule, and from that moment
  the peer's absence from the store is invisible. So the SFU is now allowed
  to contradict the store: `RtcController::refreshFromServer()` re-reads the
  room's state from the homeserver, and `SfuCallController` calls it when the
  SFU reports a participant that no membership accounts for. The SFU only
  lists a participant who authenticated as a real Matrix identity, so that
  condition is evidence about OUR view, not about theirs. Before it existed
  the key lane had no route back at all: `reconcileKeyLane()` re-ran the same
  resolution every tick against the same wrong answer for the length of the
  call. (This closes the deferred F6 item — "key reconcile never re-issues
  the membership read".)
- **The two views are MERGED, not chosen.** `/state` is a snapshot that can
  predate our own publish by a round trip, and the store is where our own echo
  lands first; equally, the store can hold an event the server has already
  replaced. Newest `origin_server_ts` per state key wins, so a retraction is
  an ordinary newer event and an older stored copy can never resurrect a
  participant who left.
- **The forced read backs off, because some participants can never be named.**
  A client publishing a membership format this build cannot parse would
  otherwise cost a `/state` request every ten seconds for the length of the
  call. The gap doubles per consecutive forced read that changed nothing, to a
  five-minute ceiling, and resets the moment a read comes back different —
  which is the proof that asking again can help.
- **The read now says where its answer came from.** `session read room
  participants= N source= "store"|"server"|"server-none"|"store-fallback"
  rawEvents= R`. Counts and one fixed word, no ids. A participant list missing
  somebody who is demonstrably in the call is the hardest thing to diagnose in
  this lane, and until this line existed "the store is stale" and "nobody is
  really published in this room" produced identical logs — which is why this
  issue took three rounds.
- **NOT live-validated, and one thing is still unexplained.** The list reaching
  **0**, which includes this device's own membership, cannot be produced by
  the expiry arithmetic: a refresh writes `expires = (now - created_ts) +
  period`, so our own deadline is always a period into the future. Whatever
  emptied it emptied the read, not the clock. The new `source=`/`rawEvents=`
  line is what will name it in the next report; do not guess a second fix
  before that line exists.

#### 2026-09-08, the post-0.9.3 audit round (four read-only audits, then the fixes)

Four independent read-only audits — MatrixRTC join and keys, GStreamer object
lifetime, sync and the timeline, crypto/storage isolation — followed by fixes
under strict single-file ownership. Every claim below was traced to `file:line`
before anything was changed, and the headline ones were re-verified against
`HEAD` by the orchestrator rather than taken on the auditor's word.

- **TWO PRODUCERS WROTE ONE INDEX BASE, and the recovery WAS the storm.** The
  SDK's diffs address a vector from `entries_with_dynamic_adapters`, which
  starts at 20 rooms and grows in batches; the snapshot came from
  `client.rooms()` — the whole state store, different order, Spaces included —
  and both were handed to one C++ handler that rebuilt the ordered list. A
  dozen ordinary actions (mark read, favourite, accept an invite, leave a
  room) therefore replaced the index base, the next `set{index}` addressed a
  different room, was rejected, and the rejection called resync, which
  re-emitted the same snapshot. That is the recorded "room_list malformed diff
  rejected" storm, self-sustaining, and `4185a92` could not have fixed it: it
  touched only the C++ side. GENERALISE: **a positional diff and a snapshot
  are not interchangeable inputs**; only the producer that owns the index
  space may re-establish it, which is what the timeline path has always done
  by re-opening the SDK timeline.
- **And the storm was the benign branch.** `remove{index}`, `pop_front` and
  `pop_back` deleted `m_roomOrder.at(index)` with NO id check, so a drifted
  index silently removed a room the SDK never named. The SDK's `VectorDiff`
  carries no id for those, but the PRODUCER knows one — it now stamps it, and
  a mismatch rejects instead of deleting.
- **The classic-sync fallback died permanently on the first network error,
  and matrix-sdk's defaults are why.** `sync_with_callback` cannot return
  `Ok(())`, so any error ends the loop and nothing re-invokes it. NEW, read
  out of the linked SDK: `RequestConfig::default()` has `retry_limit: None`,
  and the native http client treats a network failure with no retry limit as
  PERMANENT — matrix-sdk-ui works around exactly this in its own sync service
  with `retry_limit(5)`. Two seconds of Wi-Fi killed sync for the session.
- **`full_state(true)` was set once and never cleared**, and `sync_loop_helper`
  mutates only the token, so every 30-second incremental sync asked the server
  to serialise the complete state of every joined room. It bought nothing even
  on the first request, which carries no `since`.
- **A promise change function read its own context after freeing it**, in four
  handlers of the 1:1 engine and three of the SFU engine: `ctx->…` below
  `gst_promise_unref`, where the destroy notify deletes it. On webrtcbin's
  error-reply path our ref is the last one. The SFU engine's equivalents
  survived only by returning early on exactly the branch an error reply takes
  — luck, not design. GENERALISE: **hoist every read of a promise-owned
  context above the unref**; upstream's own example does.
- **Neither SFU bus was ever drained.** The sync handler returned
  `GST_BUS_PASS` on every path and nothing pops those queues, so every message
  of a whole call accumulated — and a `STATE_CHANGED` holds a ref on the
  element that posted it, keeping every torn-down bin alive. The same defect
  had already been found and fixed TWICE in this subsystem
  (`GstCallMediaBackend`'s "DROP after inspection", `ShareAudioSources`' bus
  flush); this was the third bus and the only one still leaking.
- **A deferred teardown could outlive the engine.** The IDLE-probe teardown
  unparents the bin and only then NULLs it, so between those lines the bin is
  orphaned and running where `destroyPeer` cannot reach it, and nothing waited
  for that work — a crypto probe firing in that window reads raw pointers into
  a destroyed engine. The teardowns are counted now and `stop()` waits with a
  bounded budget. The comment that asserted the opposite invariant was itself
  part of the defect.
- **The insecure-to-keyring migration wrote credentials under a mangled key.**
  Group names fold `/` and `\` to `_`, and the loop passed the folded GROUP
  NAME back as the user id; the read-back compared against the same folded key
  so it always "succeeded", and the plaintext was deleted. The comment
  justified it by asserting a Matrix user id never contains `/` — the
  localpart grammar includes it. It also removed the whole secrets group after
  migrating one key, taking the refresh token and OAuth client id with it.
- **"Remove account" on the ACTIVE account was a sign-out.** The active branch
  returned early and delegated to logout, leaving the account directory —
  whose name is the Matrix localpart — its cache and any divergent second
  store root on disk, while the same button on a background account deleted
  every one of them.
- **The Spaces rail followed the user into the next account**: its layout
  (Space room ids, user-typed folder names) was written to a device-global key
  as well as a scoped one, never reset on a switch, and never removed by
  account removal. `loggedOut` alone was not enough to fix it and the test
  found that: the signal fires BEFORE the active account moves, so the store
  re-cached the outgoing account.
- **The store's databases were chmod'd before they existed.** The calls sit
  either side of `RustClient::new`, which builds a runtime and opens nothing;
  the sqlite store is created later in `build_client`. So the run that CREATED
  a store left it at the umask and only a later launch corrected it — while
  the comment asserted the opposite.
- **Eight SFU failure categories arrived as one sentence**, including the two
  that name a configuration mistake, and a 404 from the call's own JWT service
  was reported as "Calling isn't available on this homeserver" — the focus
  comes from the oldest membership and is usually somebody else's
  infrastructure. GENERALISE: **a closed set on one side of an FFI and a bare
  `default:` on the other is how these drift**; the test that pins every
  category against the fallback AND against each other is the cheap fix.
- **A definitive "no MatrixRTC here" was reported as "couldn't check".**
  Discovery maps a 404 well-known to `unsupported` and the comment says in as
  many words that this is an answer, not a failure — then reported
  `server_answered: category.is_empty()`, which is false for exactly that
  case. The retry predicate is the negation of that flag, so discovery re-ran
  on every room change for the whole session against a constant.
- **Leaving while a membership publish was in flight left a ghost.** Teardown
  zeroed the op id, so the answer was discarded unread — no log, no
  retraction, and no cancellation of the delayed retraction that publish had
  armed. Three guards were needed for the fix and all three are non-obvious:
  the event id (not `ok`) says whether a membership exists; a refusal that
  created nothing retracts nothing; and being back in the same room retracts
  nothing, because the state key is per device and this call's own publish
  already replaced it.
- **The device picker was decoration on the lane that carries calls.** Camera,
  microphone and speaker selections reached only the legacy 1:1 engine; the
  SFU engine built its capture from a bare element name with no device
  property. GENERALISE: **Qt and GStreamer enumerate devices through different
  subsystems, so their ids are not one namespace** — measured here,
  `gst-device-monitor-1.0 Audio/Source` answers `pipewiresrc target-object=68`
  for a device Qt names in PulseAudio form. Resolution must be identity first,
  display name second, and the id's own shape only when the monitor cannot
  answer; ambiguity resolves to nothing, because opening the wrong camera is
  worse than opening the default.
- **A test's timeout can fail a DIFFERENT component.** Raising this suite's
  QTRY budget from 2 s to 10 s made a failing run of the anchor case stack its
  waits to ~34 s — past the 30 s pagination watchdog added in the same round —
  so the watchdog fired inside the test and the run then failed for a second,
  unrelated reason. 4 s is twice the measured overshoot and keeps the watchdog
  out of reach. GENERALISE: **a test budget has a ceiling as well as a floor,
  set by whatever production timer is shorter than it.**
- **A flake rate measured on a busy machine is not a measurement.** The same
  anchor case ran 3/10, 4/10, 6/10 and 11/12 across batches that differed only
  in machine load, and an A/B that looked decisive (8/8 vs 5/8) was load
  confounded. Interleave A/B/A, and quiet the machine, before calling anything
  a regression.
- **An attached audio file carried no duration** because `attachment_info`
  hard-coded `duration: None` for audio AND video; video only looked right
  because `sendVideo` has a separate path. The decoder that grabs video
  posters already knows the length and was throwing it away with the frame an
  audio file does not have. `duration: 0` is a claim, not an absence — it
  renders as the 0:00 the fix removes.

#### 2026-09-05, the second evening on 0.9.0 (mentions, tray, profile card, room load)

- **A `QGuiApplication` cannot host Qt's XEmbed tray icon.** `QSystemTrayIcon`
  uses a D-Bus StatusNotifierItem when a watcher exists and a `QWidget`-based
  XEmbed icon otherwise; the second aborted a 0.9.0 process on NixOS under an
  XEmbed-only bar the moment the tray setting was switched on. The process is
  a `QApplication` now. The reporter's "works on 0.8.3" is most likely a
  desktop difference, not a code one — the tray code had not changed since
  0.7.5.
- **Mention pills fell to the localpart too early.** The sanitizer replaced
  the anchor text with the member snapshot's answer or the localpart; a user
  the snapshot could not name (not in the room, or never loaded) rendered as
  a bare username and opened a blank profile card. Order now: room member
  name, the global profile (`UserProfileResolver`, one `/profile` ask per
  user per session), localpart. The popover asks the same resolver when the
  roster has nothing. Honouring the sender's own anchor label was tried and
  REFUSED in the same round: it is the spoof the localpart fallback exists to
  prevent ("@admin" linking to an attacker), and Element ignores it as well.
- **Member hydration was a full re-render.** `onMembersChanged` dropped every
  cached body and announced the body roles on every row — one relayout of the
  whole room after each first open. Each render now records the (user id,
  name) pairs it resolved; hydration and profile answers re-announce only the
  rows whose pair changed. Proven by `memberHydrationRerendersOnlyRowsWhoseMentionsChanged`
  (one body announcement, for the one row that mentions the renamed member).
- **The media band closed on the newest side and moved the reader.** Its
  first working cut gated rows on both sides of the viewport, so a picture in
  a row between the reader and the live edge loaded late, grew, and pushed
  the reader ("teleports me around"). Rows older than the reader grow away
  from them; the band is open all the way to the newest row now and only
  history beyond 2.5 viewports waits.
- **All media rows fetched at open.** The un-virtualized Column instantiates
  every row, and each image row asked the bridge on `Component.onCompleted` —
  in an encrypted room without server thumbnails that is a FULL download per
  image. A band gates the ask now: an INDEX RANGE the pane computes at
  discrete moments and the row Loader assigns to the delegate, the same shape
  as `rowOnScreen`. The first cut compared the delegate's own y with
  content bounds, and inside the per-row Loader that y is always 0 — no
  saving at the newest end, no pictures deep in history. Caught by the
  maintainer's live run the same evening.
- **Only the call room was slow — and why.** The maintainer's observation
  that every other room opened instantly pointed at the room, not the client:
  its history is MatrixRTC membership churn, one state event per participant
  per minute of every call, thousands of them, each a timeline item to
  paginate, ingest, instantiate and count. They are filtered out of every
  timeline at the SDK now (`lightning_event_filter`); the pagination
  controller tolerates twelve consecutive empty pages so the fill can walk
  through a run of them.
- **The room-open cost, measured.** A timestamped log put a first open of a
  call room at 9 history pages / 4.2 s and a re-open at 18 / 11 s; each page
  is ~70 ms dispatch + network (or ~1 ms from the event cache) + 100-250 ms
  of ingest and fill-loop timers. A second log, after the budget change,
  showed 14 pages in 4.4 s with every page served from the cache in ~1 ms:
  the remaining cost was the fill loop itself — rows land before the
  controller finishes the batch, the fill check they trigger finds it busy
  and waits on the 250 ms retry timer, once per page. The fill now re-checks
  the moment the controller goes idle. A third log then showed the other
  half: a re-open ran 32 pages and 600+ rows in 6.4 s, the invisible-page
  budget never tripping because every page added a little height, and the
  instantiation of those rows was the freeze. The fill is capped at 240 rows
  now (`maxViewportFillRows`), which is the scale 0.8.3 stopped at. `a5e64a6` had raised the invisible-page
  budget from 8 to 60 so a collapsed run never blocks older history; it is
  12 now, and a wheel towards older history on content too short to scroll
  requests the next page, which keeps that promise without paying for it on
  every open. REFUTED by the same log: doubling the page size after an
  invisible page — Synapse took 1.5-1.8 s per 100-event page (17 ms/event
  vs 5.5 at 20), the fill overshot to ~600 rows, and the re-open went from
  4 s to 11 s. The event cache keeps one page after a room closes
  (`shrink_to_last_chunk`), so a re-open pays the fill again, from the store.
- **Emoji in packaged builds.** The SAS verification labels and every surface
  that never names a face (tooltips) drew monochrome or missing emoji on the
  AppImage's Qt 6.8. The verification labels name `app.emojiFontFamily`; the
  application default font now carries the emoji face as its second family
  (`FontManager::withEmojiFallback`), so unnamed surfaces inherit it.
- **The profile card.** The banner drew over the popover's border and met its
  rounded corner at a notch: inset by the border width, radius reduced to
  match. Its corners were then reported as a staircase: `MultiEffect`'s mask
  defaults (threshold 0, spread 0) are a hard step on the mask's alpha, so
  the mask rectangle's own antialiased corner pixels came out fully opaque;
  `qquickmultieffect.cpp` derives the low-side ramp as
  `[(t-1)(1+s)+1, t(1+s)]` for threshold `t` and spread `s` — threshold 0
  with spread 1 ramps over [-1, 0] and made the empty corners OPAQUE (square
  corners), threshold 1 with spread 1 ramps over [1, 2] and hid the whole
  banner (an empty box), both seen the same evening; the ramp that maps mask
  alpha 0 -> 0 and 1 -> 1 is `maskThresholdMin: 0.5` with
  `maskSpreadAtMin: 1.0`. Read the source, not the docs, for a shader's
  arithmetic. The gradient Canvas paints antialiased now too. The membership chip was content-sized while the row's other chips
  shared `uniformHeight`. The avatar's presence tooltip opened on top of the
  avatar; it now sits to its right. The Message button's glyph read as
  pixelated: `Icon` renders natively (hinted, device-pixel) instead of through
  the distance field.
- **Hover bar Edit, popout share fill.** The bar gained Edit before More under
  the context menu's own `canEditEvent` gate; the popout gained "fill this
  window with the share" (a Repeater over the share model showing only the
  spotlighted share, the tile grid stepping aside), dropped when the share
  ends.
- **The share surface.** Reported from the popout's fill mode and the main
  stage alike: "no border, just a stream thrown in there", and the owner
  nameplate covering the picture. The tile's own 1px edge sat at the window
  edge and the aspect-fit video left bare strips beside it. A frame now
  follows the painted picture — computed from the frame size, because
  `VideoOutput.contentRect` did not describe it on the live build — and the
  tile's own edge steps aside while the picture shows, or there were two
  frames (reported the same evening). On a big surface the nameplate fades
  after three idle seconds the way the full-screen controls do.
- **Two read receipts for one reader.** Receipt hosting (a call row hosting
  the bodiless rows after it) drew the same avatar on the call row and on the
  message after it: the backend still carried the reader's receipt on the
  older row. A reader has one position now — the newest row carrying them
  wins — and the host they left is announced so it drops them.
  The first cut of that announcement read the model's cached receipt index
  as "before" — and every handler invalidates that index ahead of the call,
  so the snapshot was always empty and no old host was ever announced. The
  regression test failed on it in the full run, which is the whole argument
  for a test that fails on the old code. The handlers capture the positions
  ahead of their mutation now, keyed by EVENT ID because an insert or a
  removal shifts every row index, and the announcement covers the host a
  reader left, the host they arrived at, and the neighbour above the row
  (a span can split or merge there).
- **The selection circle never filled.** `rowSelected` called
  `app.forward.isSelected()`, a plain function Qt cannot observe, so the
  binding ran once and never again: the footer counted "1 message(s)
  selected" while every circle stayed empty. The same lesson as the
  CallStage `indexOfShare` note — a binding on a plain call needs a
  NOTIFYing property in it — so it names `selectedCount`, which changes on
  every toggle. The hovered row's time also drew over the circle (the same
  gutter); it yields while selecting, and the circle is solid so it reads
  over an avatar.
- **"Home" read "Ho…", and every room name carried a stray ellipsis.** The
  header title's `Layout.maximumWidth` was its own `implicitWidth`, a
  FRACTION (53.28 px for "Home"), and a Layout hands an item an integer
  width — 53 — so the label elided by a quarter of a pixel. Measured
  offscreen with a replica of the header (width 53, `truncated` true;
  ceiled: 54, false). `Math.ceil` on the bound. GENERALISE: a Layout bound
  derived from a text's implicit size must be rounded UP.
- **The hang-up button left the voice bar.** A non-fill Text in a Layout is
  fixed at its own width, so at the navigation column's floor "Voice
  connected" kept its width and the row overflowed through the bar's edge.
  The text column is shrinkable and the status text elides; the buttons
  never move.
- **A popped-out share came back as a grey box.** The popout's click-to-fill
  tiles lived in a Repeater gated on the fill flag alone, not on the window
  showing, so they survived a pop-in unattached — the main stage's tiles had
  claimed the sinks back — and the next pop-out showed the same tiles with
  no frames until a restart. Gated on `root.visible` like the grid Loader
  beside it, which is the router's one-owner rule applied to the second
  surface; the reader's fill choice itself is kept.
- **Direct messages at Home.** "add people dms to home page too, so they are
  listed under rooms but keep a separate people dms tab too": Home lists the
  joined DMs again as a Direct Messages group after Rooms (its own collapse
  key), the DM invites stay in the tab, a Space view still never carries a
  DM, and the People chip is offered at Home too, where it narrows the view
  to that group.
- **The mention popup followed the sentence.** After a rich-mode pill was
  inserted the composer kept scanning back to the pill's own "@" — the
  markdown editor records each inserted mention as a ref and refuses a
  token overlapping one, the rich editor recorded nothing — so every
  keystroke reopened the popup matching "SpongeMan as a true profes…"
  against nobody. Two fixes: the rich editor refuses a token that starts
  inside a matrix.to anchor, and the tokenizer ends a token at its SECOND
  space (one keeps two-word names completable; Element allows none).
- **Selection mode followed the reader into the next room.** A selection
  belongs to the room it was started in; the controller cancels it on a
  room change now (`switchingRoomsEndsAMessageSelection`).
- **"1 message(s) selected".** Thirteen English plural entries in the
  catalog were still empty, so `%n` strings shipped their source text; they
  carry both forms now.
- **A star for favourites in Channels.** Favourites rise to the top of their
  group and nothing said why. The row draws a star between the name and the
  call glyph: filled while favourite, an empty outline on hover, a click
  toggles the same `m.favourite` tag the menu writes. Drawn on a Canvas
  rather than a glyph, because the bundled Material subset is instanced at
  FILL 0 and has only the outline.
- **The Home tile's badge counts the DMs it lists.** With the Direct
  Messages group at Home, the rail's Home badge adds the People total to the
  unparented rooms' — a room in two views is counted by both tiles, as a
  room under two Spaces already was — and the attribution tests say so.
- **Notifications on Windows were nothing at all, and a read KDE
  notification stayed in the history.** Delivery had one path, the
  freedesktop daemon over D-Bus; a build without QtDBus logged a line and
  showed nothing. The tray icon's balloon is Qt's only other delivery
  (Windows: a toast in the Action Centre; macOS: a user notification) and
  it needs a VISIBLE icon, so on those platforms the icon follows the
  notifications setting as well as close-to-tray, the manager keeps the one
  payload the balloon carries and a click routes to its room. And the
  read-withdrawal (`closeRoomNotifications`) never reached KDE's history
  because an EXPIRED popup (freedesktop reason 1) forgot its payload —
  expired stays withdrawable now, dismissed and closed are forgotten. NOT
  LIVE-TESTED on any platform: Windows and macOS need a packaged build, KDE
  needs a read on the maintainer's desktop. Withdrawing a shown balloon on
  Windows or macOS is not possible through Qt; that needs the native toast
  APIs, recorded as the follow-up.
- **Favourites rank again.** "favoriting a room should raise it to the top
  in channels mode" and "no favorite tab exists in classic mode". Channels
  sorts a favourite first within its group; Classic gives favourites a rank
  and a "Favourites" header between the invites and the feed, which REVERSES
  the 2026-08 decision that a star should not buy rank (a starred room
  frozen above live traffic) — the maintainer asked for Element's shape, and
  the retired `favouritesBoundaryRoomId` stays retired because the header
  now separates the groups.
- **The row context menu stayed put on scroll.** Opened at a point computed
  once in the overlay, so a scroll moved the row out from under it and it sat
  "chilling in the middle of the screen" (2026-09-06). The pane holds the one
  open menu and closes it on any content movement; the thread panel keeps the
  old behaviour.
- **A read room's KDE notification never went away.** `refreshTrayUnread`
  also withdraws the notifications of rooms that are no longer unread, but it
  returned early whenever the tray icon was off — the default — so the
  withdrawal never ran. It is gated on the client now, not the tray; only the
  badge write needs the icon.
- **A refused call join said nothing.** The controller has carried the
  reason ("You don't have permission to join this call.") since the MatrixRTC
  round, and nothing was connected to its `callFailed` signal — a member
  without the power level for the membership state pressed Join, the prompt
  vanished, and the banner went on offering Join. It reaches the status bar
  now, like every other error. Found on the 2026-09-06 GUI pass.
- **A changed name colour now propagates on a timer, not only on a
  re-render.** The read-driven refresh (5 min -> 20 s) only fires when a name
  re-renders; a sweep re-asks the recently read names every 20 s on its own,
  so a colour someone changed reaches everyone within the window
  ("in max 15-30 seconds") without anything having to repaint first.
- **Minimising popped the call out.** The automatic picture-in-picture
  (desktop-integration round) fired on every minimise and every close to the
  tray, and it shipped ON. A window appearing on its own for a reader who
  only wanted the app out of the way is not what anyone asked for: the
  setting is OFF by default now (`calls/pictureInPicture`), the manual
  pop-out from the call bar is unchanged, and a reader who wants the old
  behaviour has the checkbox under Calls.
- **The SDK event-cache panic.** `failed to remove an event: InvalidItemIndex`
  is an `expect()` inside matrix-sdk 0.18's event cache; with
  `panic = "abort"` it took the process down. Both profiles unwind now, so the
  SDK's own `TaskMonitor` contains it: that task may die and the room stop
  updating until a restart, which is the honest trade against a crash. Not
  fixed upstream; not reproduced here.

#### Capture, encoding and the media pipeline

- **Caps evidence.** `WindowCaptureSrc` fixated 1920x1080 against a 3840x2100
  window: half-row shear, top quarter only — under 1920 wide the stride matched
  and only bottom rows were lost. `gdiscreencapsrc` reports FIXED caps, so the
  restricted structure drops out: that is why a MONITOR share got 3840x2160 and
  a WINDOW share did not. With PAR opened downstream and the source's fixate
  silent, 3840x2100 and 3840x2160 ERROR while 1557x1213 passes carrying
  1/2147483647 — every window over the ceiling would have published NOTHING.
  The element fixates 1/1 itself AND a FIXED 1/1 capsfilter sits before the
  source, because `avfvideosrc` declares no PAR and `pipewiresrc` is untestable
  outside a portal.
- **PARs VP8 discards** — videoscale's "keeps the aspect ratio" is false; a
  libwebrtc receiver draws the literal size. 3840x2100 -> 36/35; 1920x1200 ->
  9/10 (11% stretch); 3440x1440 -> 43/32 (34% squash); `ximagesrc` -> 2/1.
  Unreported for months because the maintainer's monitors are 16:9.
- **`videorate` clocks from SEGMENT START, not the first buffer's PTS**, and
  capture sources stamp pipeline RUNNING TIME, so a source started mid-call
  back-fills 30 duplicates per second of CALL AGE — one picture at full rate,
  counters healthy. That was the "camera does not work". First PTS 0/10/174 s
  -> 27/327/5247 out; `skip-to-first=true` -> 27 always. Lightning's own
  element stamps from ZERO and must KEEP that.
- **`videorate` also HOLDS the first buffer until a second arrives**, and a
  PipeWire capture delivers ON DAMAGE, so the wait is "until the screen moves".
  REFUTED here: `skip-to-first` does nothing; `max-duplication-time` keeps the
  hold AND starves the encoder below 30 fps. `keepalive-time=100` re-pushes the
  held buffer, is NOT `min-buffers` renamed, and 100 ms is DIAGNOSTIC (a dead
  capture reports ~10/s vs a live one's up-to-30/s). **UNRECONCILED: open items
  record `keepalive-time=100` as having KILLED the capture.**
- **`min-buffers=8` was an unmeasured guess that made things worse and was
  banned; ban RE-SCOPED 2026-08-28** — it could not negotiate against a
  compositor offering at most 4 buffers on PipeWire >= 1.6, so "no frame
  arrived" was an allocation failure, not proof the property is forbidden.
  `min-buffers=1` is REQUIRED.
- **A counter downstream of `videorate` cannot prove the capture is alive** —
  it repeats the last picture, so a dead capture still encodes, encrypts and
  sends at full rate. Count the capture's own buffers, before it.
- **A desktop capture is VARIABLE RATE** — PipeWire negotiates `framerate=0/1`
  at native size (3840x2160 BGRA), and a range including `0/1` leaves `vp8enc`
  no rate to plan against. Pin `30/1`; sizes stay ranges, being ceilings.
- **`rtpvp8pay` parses the VP8 bitstream and cannot payload an encrypted
  frame** — libwebrtc takes the descriptor from the encoder as METADATA, hence
  `RtpVp8Payloader`, which reads nothing.
- **element-call mints a 16-byte media key, livekit-client 32** — requiring 32
  rejected every Element key for its LENGTH, so an Element peer was inaudible
  while our own media reached them.
- **Identify a received track by TRACK SID from the msid, never a `mid`** — a
  `TrackInfo.mid` belongs to the PUBLISHER's connection.
- **A capture that ends itself must be heard** — closing a shared window
  answers EOS and nothing listened, leaving the track declared and the far end
  frozen. It retires through Stop's path, which sets the transceiver INACTIVE.
- **Verify against something that is not Lightning** — two Lightning clients
  agree on streams a libwebrtc receiver rejects; `livekit-cli` (pion) and an
  independent frame-crypto implementation each refuted a confident wrong
  theory. Full table: `docs/matrixrtc.md`.

#### The v0.9.0 round (Task B), 2026-09-05

Ten features in one round. What is worth keeping is not what they do — that is
`docs/feature-contracts.md` — but what each one refused to do, and what my own
mistakes cost.

- **A rule enforced on one of N paths is not enforced.** Read-receipt privacy
  has THREE Rust send paths (in-room, mark-a-room-read, the thread panel). The
  mode is stored ONCE on the bridge and all three read it; the two that build
  `Receipts` share one `receipts_for_mode`. The same shape drove the pack CRUD
  (one `PackEdit` enum, one writer, two stores) — four near-identical async
  functions is how three of them end up correct.
- **A notification card outlives the account that raised it.** The user can
  switch accounts or sign out while it is on screen. Acting on it under the
  current account would mark another account's room read, or reply from the
  wrong identity — and it would SUCCEED, so nothing would report it. Every
  payload carries the account it was raised for.
- **`inline-reply` arrives in TWO parts on some daemons** (ActionInvoked, then
  NotificationReplied), so the payload must SURVIVE the first. Dropping it
  there fails only on the desktop nobody tested.
- **One sink per track, last attach wins** decided the picture-in-picture
  design. PiP and full screen are mutually exclusive IN CallStageState rather
  than by convention, its surfaces are built only while the window shows, and
  its tiles are Repeaters over live models — a `get(row)` snapshot taken
  before a share's track key arrives never attaches a sink (CallStage learned
  this first).
- **Both of Discord's call keys were already taken** — Ctrl+Shift+M is
  `room.markRead`, Ctrl+Shift+D is in the reserved table — and
  `everySeededDefaultAvoidsTheHardCodedKeys` caught the second. The gate
  works; use free keys and let people rebind.
- **matrix-sdk-ui sanitises INCOMING html with a hard-coded const** and strips
  `data-mx-emoticon`; read the RAW event's `formatted_body` instead, and call
  that AFTER `fill_message_content` or its guard returns early and the feature
  silently does nothing. **Allowing that attribute DISABLES ruma's img-src
  scheme check** — its loop returns on the first attribute with no scheme
  rules — so mxc-only is enforced by our own strip, and the `<img>` is REBUILT
  from validated parts rather than passed through.
- **MSC4108's blocker was our own recorded decision, not the SDK.** The
  new-device direction needs the OAuth device-code grant, which `oauth.rs`
  deliberately does not request (with a test saying so). Only the
  already-signed-in direction shipped. `login_with_qr_code` takes its OWN
  `ClientRegistrationData`, so the other direction can be added later without
  touching the ordinary flow's metadata or that test.
- **MSC4153 is TWO knobs and one setting.** Setting one alone gives an
  asymmetric client. `CrossSignedOrLegacy`, never `CrossSigned` — the strict
  variant refuses legacy Megolm sessions and would turn existing history into
  UTDs the moment someone enabled a privacy setting. Builder-only in 0.18, so
  restart-to-apply, said plainly.
- **`Recommendation` must cross as a STRING.** ruma models it with one known
  variant plus `_Custom`, so Mjolnir's legacy `org.matrix.mjolnir.ban` lands
  in `_Custom` and an enum comparison reads a real ban list as EMPTY. The
  legacy `org.matrix.mjolnir.rule.*` type names matter for the same reason.
- **A removed policy rule is an empty content object**, and must not parse as
  a rule with an empty entity — which matches nothing under a careful matcher
  and EVERYTHING under a careless one.
- **A desktop "share my location" is "paste a link", so it was removed the
  day it was built.** The first version had latitude/longitude fields
  because a desktop has no better input; the maintainer's reaction was
  "why would I not just send the map link directly?", and the honest answer
  is that they would. Wrapping the link in an `m.location` bought a native
  pin on phones and cost a dialog, a menu item and a code path — not worth
  it. RECEIVING stayed, because that is interop with the phones people
  actually use. Generalise: before building an input for a desktop, ask what
  the user would type into it and whether the composer already accepts that.
- **An unreadable coordinate must be ABSENT, not 0,0.** Zero is a real spot in
  the Atlantic; a UI reading it draws a confident link to the wrong place. And
  0,0 is itself a real place, so the flag exists rather than a magic-value
  check. No embedded map, because tiles put every reader's IP at a tile
  server; the OSM link is built from parsed NUMBERS and `UrlLauncher`'s
  allowlist is untouched.

**Four defects of my own that the existing gates caught, and one they did
not.** Caught: `AppTheme.surfaceRaised` (theme-tokens); `place` and `edit`
absent from the icon-font SUBSET — Icon.qml answers an unknown name with an
empty string, so a wrong name is a silently BLANK glyph, not tofu; and
`QAbstractListModel` has NO `count` in QML — a ListView supplies one, the
model does not — so `model.count` reached `qsTr()` as a plural argument and
three "no QML warnings" suites failed on it.

NOT caught by anything: `app.copyToClipboard` does not exist, and
MediaBrowserRow's "Copy link" called it. It would have failed the first time
anyone used the menu item. Found only by writing the same affordance a second
time. **A QML call to a non-existent `app.` method is invisible until the code
path runs** — which is the gap `tests/QmlComponentLoadTest.cpp` was added to
narrow, and does not close: the load gate proves a component instantiates, not
that its handlers work.

**Two contract tests were failing on anchors an earlier commit in the same
session had removed** (`id: mediaList`, "Media & Files"). Both FAILED rather
than silently covering nothing, which is the guard working — and is why a
scoped scan needs a bound whose absence breaks it.

#### Voice-call constraints that must not soften

Contract in `docs/voice-calls.md`. Inbound call/party ids are sender-chosen
text: bounded in Rust, never logged; remotely triggered work is BOUNDED and
idempotent; ignored senders drop before any state change or send; backlog
suppression defaults CLOSED. SDP transport is OPT-IN end to end, bounded
128 KiB, in the single-shot memory-only `calls::SdpStore` (cap 8, wiped on
sign-out/detach/teardown/reset), never on CallSignal, never logged, never in
QML. TURN comes from `/voip/turnServer` only — credentials cross once,
engine-only, never logged, no third-party STUN. `startVoiceCallButton` is
contract-enforced 1:1-DM-only (a legacy invite rings every room member) and
`enabled: false`, contract-pinned so re-enabling is a decision, because no
answered call has been live-validated. Session-identity tokens ride every
GStreamer callback so a reused engine cannot attribute a closed call's queued
event to the next, and registration sits behind an explicit
`enableCallMediaEngine()` so the test fleet never gains an engine it did not
ask for. Pre-answer candidate buffering and RFC 3264 answer-side Opus pt reuse
came from reading GStreamer sources; `m.call.negotiate` is deliberately
unhandled. Suites: `call-controller` 35, `call-ring-policy` 10,
`call-ui-contract` 6, `call-media-loopback` (SKIPs without plugins),
`calls::tests` 10 — loopback proves the engine, not the network.

#### Packaging, platforms and toolchains

- **`GST_PLUGIN_PATH` is read DURING `gst_init`, once**, and two backends each
  ran their own `gst_init_check` while only one set the bundled path — so a
  package with 25 correct plugins and zero unresolved symbols refused every
  call, and every check passed it, because they proved the payload's SHAPE and
  none proved the app could FIND it. One entry point now does path-then-init
  and both backends are BANNED from `gst_init`. GENERALISE: a feature assembled
  at package time needs a check that runs the SHIPPED artifact and asks whether
  it works. Windows and macOS had shipped for months with no media engine, and
  the honest refusal kept anyone from suspecting packaging.
- **Fedora's mingw GStreamer is a trap** — `gstreamer1-plugins-bad-free` ships
  `libgstwebrtc-1.0-0.dll` (the LIBRARY), not the `webrtcbin` PLUGIN, and no
  nice/srtp/opus/vpx. A `.pc` file and a DLL of the right name are not the
  element; use the upstream MinGW SDK.
- **Capture elements AND their property names are per-platform** —
  `v4l2src`/`pipewiresrc` Linux-only, Windows `ksvideosrc`/`gdiscreencapsrc`,
  macOS `avfvideosrc` (± `capture-screen=true`); `gdiscreencapsrc` takes
  `monitor`/`cursor` where `d3d11screencapturesrc` takes `monitor-index`/
  `show-cursor`, and `gst_parse_launch` fails outright on an unknown property.
  Read them from the shipped plugin's own help strings.
- **A UCRT/msvcrt CRT split, and the probe that cannot see it** — mingw-w64's
  `wchar.h` makes `mbstate_t` a struct under `_UCRT` and an `int` otherwise, so
  `libgstd3d11`/`libgstmediafoundation` import a `std::codecvt` symbol absent
  from the staged libstdc++ (12018 exports; that symbol 0 times, the msvcrt
  spelling once). Windows fails a missing NORMAL import at LoadLibrary — **but
  Wine loads the module anyway**, so a Wine element probe passes a feature dead
  on its target platform. Only a symbol-level walk over the staged closure sees
  it (ZERO unresolved across the 24 shipped plugins). A CRT CHOICE, not version
  drift: a GStreamer bump will not fix it.
- **macOS codesign refuses a plain directory of dylibs inside the bundle** —
  the working shape is a SYMLINK from `Contents/MacOS/gstreamer-1.0` to
  `../PlugIns/gstreamer-plugins`. `macdeployqt` also rewrites the app's
  GStreamer glib/gobject/intl deps into `Contents/Frameworks` out of HOMEBREW,
  splitting the GObject type system, so validation asserts every GStreamer
  library the executable loads is the staged copy.
- **Compile-checking a `Q_OS_WIN`-only TU**:
  `x86_64-w64-mingw32-g++-posix -fsyntax-only` in `debian:13.6-slim` against
  Linux Qt/GStreamer headers plus a stub for `QtGui/qwindowdefs_win.h`, leaving
  three glib LP64/LLP64 `static_assert`s. **Prove it reached the end of the
  file** with a probe TU plus a deliberate undeclared identifier — "no errors"
  can mean "gave up early". nixpkgs' `pkgsCross.mingwW64` gcc does NOT work: it
  wants `mcfgthread/gthr.h`.
- **Windows update paths (fixed in 0.7.3).** MSI failed with **1619** because
  msiexec has its own argument parser and rejects Qt's forward-slash path (`/`
  errored, `\` installed). The portable swap renamed the install DIRECTORY
  while the running helper and its mapped Qt DLLs lived inside it — now
  entry-by-entry, since renaming in-use FILES is permitted on Windows while
  deleting them is not, so a stale backup directory must be cleared or update
  #2 fails. AppImage relaunched the MOUNTED binary, not the `.AppImage` it
  replaced; the app icon was passed only as a theme NAME, which resolves in an
  installed deb/rpm and nowhere else.

#### QML, layout and bindings

- **A window-level `Shortcut` in a kept-alive screen is armed forever.**
  Keeping SettingsScreen built between opens (for the 428 ms it cost to
  rebuild) left its Escape and Ctrl+, Shortcuts live on the main screen: a
  Shortcut is matched by window, never by its item's visibility, and two
  enabled Shortcuts on one sequence make Qt fire NEITHER — Escape would have
  stopped closing the info panel. Caught in review, not by the live pass
  (which never pressed Escape after opening Settings). Gate every Shortcut
  in a kept-alive component on `root.visible`; the regression case presses a
  real key on the window after a real open and close.
- **`Component.onCompleted` in a kept-alive screen runs once at launch.**
  Backup progress and profile banners were requested there; kept alive, the
  figures froze at their launch values. Re-ask on `visible`.
- **A RowLayout of non-fill children reports their SUM as its MINIMUM, and a
  minimum propagates up through every enclosing Layout.** The room-info tab
  strip's natural width became the panel's floor, TimelinePane's row honoured
  it, and the panel was pushed off the window's right edge instead of being
  narrowed — so `fitWidth`'s `overflowing` ("implicitWidth > width") could
  never come true, because width was never allowed below implicitWidth.
  Measured live at the 260 px floor: close button, Save buttons and last tab
  outside the frame. Fix: `Layout.minimumWidth: 0` on the strip. GENERALISE:
  a compacting control inside a Layout must zero its own minimum or it will
  resize its host rather than itself.
- **A property declared inside an anonymous child is not on `root`, and
  nothing says so.** `tabsWrap`/`tabModel` were added at the ColumnLayout's
  indentation; `root.tabModel` then read as `undefined` with NO warning
  (member access on a QObject never throws, only a bare id does), the
  Repeater got an undefined model, the strip's implicit size went to 0 and
  the whole strip vanished with a clean log. A live test driving the real
  widths (`roomInfoTabsWrapIntoTwoRowsWhenThePanelIsNarrow`) found it in one
  run; the contract scans could not.
- **A Loader whose `active` follows the current screen rebuilds the screen
  on every open.** Settings was 428 ms of GUI-thread block per open
  (`LIGHTNING_GUI_STALL_TRACE=100`), reported as "takes like a second". Keep
  the first build (`warm`), hide on close, pre-build asynchronously at idle,
  and route anything `Component.onCompleted` used to consume through a
  signal — the handler runs once now.
- **A nohup'd launch logs NOTHING to its redirect file on a journald
  system**: Qt sends messages to journald when stderr is not a terminal.
  `QT_FORCE_STDERR_LOGGING=1` (or `--log-file`) is required, or a stall
  capture reads as "no stalls" — which is exactly how the first Settings
  timing came back empty.
- **A 5px transparent grab band shows the wrong surface through it**, and the
  rule centred inside it is then NOT on the boundary. Measured at the
  room-list seam: sidebar `#1B242F`, then THREE native pixels of the window
  ground `#0D1117`, then the rule, then the timeline — the boundary sat three
  pixels before the line meant to mark it, and the sliver read as a gap
  between the panels. Two instances, the shell `SplitView` handle and the
  Room Information resizer. Fix is two parts and the second matters as much:
  paint the band as the surface on ONE side, and anchor the rule to that
  edge — centred, the colour changes in one place and the rule is drawn in
  another. Sample pixels across a seam rather than eyeballing it; the
  numbers name the defect instantly.
- **`Layout.fillWidth` DEFAULTS TO TRUE for a nested layout.** A composite
  control whose root is a `RowLayout` takes a share of its host row unless
  the host says otherwise. Bit twice in one round: `SegmentedControl` in the
  find bar (a ~350 px dead gap at 1600 px wide, scaling with the window), and
  `roomHeaderActions`, where the band's `clip: true` turned the lost width
  into CUT ICONS — the People glyph sliced in half and the Room information
  button gone entirely.
- **A Label with `elide` and no `fillWidth` does not elide.** It sits at its
  implicit width and refuses to shrink, and whatever is beside it pays. The
  room header's identity column could not give up space for that reason, so
  the shortfall landed on the action icons. Third occurrence of this shape.
- **A `color` compared to a hex STRING with `===` is never true.** The
  selected name-colour swatch showed no ring for exactly this. `String(c)`
  first.
- **`SmoothWheelArea` is a non-visual handler and has no `anchors`.**
  `anchors.fill: parent` on one is a LOAD-TIME error that took
  RoomInfoPanel, TimelinePane, MainScreen and Main down together and left the
  app exiting silently. Same family as `font.families`; qmlformat cannot see
  either, and only loading the component does.

- **`Layout.fillWidth` DEFAULTS TO TRUE for a nested layout**, so a composite
  control whose root is a `RowLayout` takes a share of its host row's surplus
  unless the host says otherwise. `SegmentedControl` is exactly that, and its
  own trailing filler — which exists to keep the segments packed — turns the
  stolen width into DEAD SPACE. In the find bar that put ~350 px between
  "History" and the search field at 1600 px wide, and ~130 px at 1100 px:
  **it scales with the window, which is what identifies it.** Invisible in
  every host where nothing follows the control, which is why most hosts never
  noticed and why the fix belongs on the host that has two things after it,
  not on the shared control's default. Found by a screenshot; no test saw it.
- **Two producers, one consumer, two spellings of the same key.**
  `MessageSearchController` is fed by SERVER search (`senderDisplayName`) and
  by the LOCAL index (`senderName`), and reads only the first — so every
  local result reached the find bar with an empty sender, in the list and in
  the row's Accessible name alike. The delegate still BUILT, because the role
  exists on the model; it is the payload key underneath that missed, which is
  the half a `required property` cannot defend (contrast the Qt 6.8
  `roleNames` entry, where the role itself is absent and the delegate count
  goes to zero). GENERALISE: when a controller has more than one producer,
  the key names are a contract between them — assert a VALUE from each
  producer, not that the role exists.

- **A `Popup` with `CloseOnPressOutside` closes on the PRESS, and the button
  that opens it is outside it.** So the icon that opens a picker could never
  close it: the press dismissed the panel and the button's own `onClicked` —
  which arrives on the RELEASE — opened it again, one gesture, close then open.
  Reported as the panel blinking and staying. `if (picker.opened) close()`
  cannot fix it, because the popup layer runs first and `opened` already reads
  false; MEASURED, not assumed — the mutation run pinned the ordering. What
  identifies the gesture is that a panel of that kind was dismissed a moment
  ago and the very next thing is a click on its own button. Callers that are
  not that button (menu entries, demo hooks) must say so, or a displaced menu
  action becomes a toggle that does nothing.
- **`QWindow::show()` FORCES the NORMAL state**, so calling it on a window
  already on screen and MAXIMIZED un-maximizes it — and if `onVisibilityChanged`
  persists the maximized flag, the user's window preference is rewritten as a
  side effect. Reported as "clicking a notification in the bell menu minimizes
  Lightning": the frame snapping back to its small remembered size is what that
  looks like. The tray path had learned this and used `visible = true`; the
  notification path had not, which is the general shape — ONE helper for
  "bring the window forward", never two call sites each deciding. Hidden (back
  from the tray) restores through `visible`; Minimized restores the visibility
  the window last had ON SCREEN, tracked in QML because
  `SettingsManager::initialWindowMaximized` is CONSTANT and cannot answer it
  mid-session; a window already on screen has its visibility left alone.
- **Two wheel branches must agree about SIGN, and one of them reached the
  controller by a different door.** `TimelineScrollController::wheelTargetY()`
  negates its argument INTERNALLY ("angleDelta.y > 0 == wheel up == toward the
  top"), which is right for an ordinary Flickable; the rotated timeline cancels
  that by passing `-angleDelta`, so two negations leave `+(angle/120)*per`. The
  smooth-OFF branch went through `notchDistance()`, which negates nothing, and
  then negated once. Turning smooth scrolling off reversed the mouse wheel.
  GENERALISE: when one code path reaches a helper that applies a convention and
  a second reaches a different helper that does not, the convention is the
  thing to assert on — the regression test compares the two settings rather
  than pinning an absolute direction, because the absolute one is a property of
  the rotation.

- **An imperative write to a bound property destroys the binding** — five media
  cache handlers assigned `Image.source` directly, so the first image that
  loaded was the last that Image showed. Use a `resolveTick` the binding READS
  and the handler bumps (an unused local does create the dependency in Qt
  6.11); with an intermediate `readonly property` it must live in THAT binding
  or it is a silent no-op; key handlers on the cache key.
- **A Popup does NOT consume a press landing on it** — `blockInput()` is FALSE
  when `popupItem == item`, so `modal: true` blocks OUTSIDE presses only; the
  2026-08-18 emoji fix assumed the opposite and was INERT. Sink with an
  all-buttons `MouseArea` in `background:`, never with `z`.
- **`visible: running` on a shared busy indicator is a permanent latch** —
  hosts use `running: visible`, together they cycle, and `visible` is EFFECTIVE
  visibility, so one created under a hidden ancestor latches off silently. A
  component owns its animation; the HOST owns visibility.
- **A defaulted C++ parameter QML must pass fails silently** — `setMentionStyle`
  gained `linkColor`, nothing passed it, and every URL and non-self mention
  rendered in the accent for a round. Pin the arity in a test.
- **In a Qt Quick Layout a child's size constraint may only read a width the
  layout does not compute** — `Layout.maximumWidth: parent.width * 0.7` under
  its RowLayout, and a segment sized against `bubble.width` which in Bubbles
  mode IS the segments' own implicit width, were the whole binding-loop log.
- **An invisible `MenuSeparator` still reserves its height** — QQuickMenu's
  ListView honours each item's height, and MenuSeparator's comes from
  contentItem plus padding regardless of `visible`.
- **A JS array bound to a ListView is a model RESET on every change**, so a
  reorder cannot animate and the delegate holding a live drag is destroyed by
  any refresh: if rows must MOVE, the model must be able to say so. A model
  early-returning on identical rows also announces nothing when only your
  per-row PRESENTATION FLAGS changed, so whoever clears such a flag announces it.
- **`QObject::findChild` cannot reach a `Repeater`'s delegates** — proven with
  a CONSTANT objectName absent from a full `findChildren` dump, ruling out a
  failed binding. Walk `childItems()`.
- **A change handler can run BEFORE the bindings depending on the same
  property** — `onTabChanged` read a binding on `tab`, got the tab being LEFT,
  and moved the selection into the tab just left.
- **Window geometry must be restored in BINDINGS, not `Component.onCompleted`**
  — Qt shows the window during `componentComplete()`, which runs first, so the
  user watches it jump. Read through a CONSTANT property, because a notifying
  one feeds the save back into the binding that produced it. A size below the
  window's minimum is REFUSED on write, since Qt reports transient 0x0/1x1
  while a window is shown, hidden to tray or restored from minimized; only the
  WINDOWED state is stored, maximized as its own flag; `QWindow::show()` forces
  NORMAL, so restoring from the tray sets `visible = true`. `QScreen` stays OUT
  of `SettingsManager` (~20 test targets link it against `Qt6::Core` alone), so
  the still-on-screen test is a BAND along the top of the frame in
  AppController — a window spanned across two monitors is not refused.
- **A guard suppressing a signal for a whole gesture needs something firing at
  the END of it** — `onWidthChanged: if (!SplitView.view.resizing) save()`
  never fired again, because the RELEASE moves nothing.
- **`Qt.quit()` is a REQUEST** — QGuiApplication closes every top-level window
  first and ignores the quit if one refuses, so close-to-tray's
  `close.accepted = false` ate Ctrl+Q, the only way out of that mode. Still
  `Qt.quit()`, not `Qt.exit()`: teardown and apply-on-quit hang off
  `aboutToQuit`.
- **A per-row Loader's item parented to `Overlay.overlay` keeps the Loader as
  its destruction owner**, so delegate churn dereferenced a dangling pointer;
  the `detailsDialogComponent` precedent does NOT transfer, a Dialog being a
  Popup that owns its overlay lifetime. Fixed with ONE shared action bar into
  which rows publish only PRIMITIVES, never a QObject reference;
  `forceReleaseActionBar` exists because the ordinary release refuses while the
  pointer is on the bar — right for a live row, wrong for a dying one.
- **Delegates reach the timeline pane only through their `timelineView`** (the
  rotated Flickable), so a pane-root `openReceiptList` was silently swallowed
  by the delegate's existence guard. Such entry points must be
  property-functions ON the Flickable.
- **The layout faults were one shape: a fixed band in a viewport that got
  smaller**, biting Windows at 125-150% scaling and not Linux, since every
  number is unchanged and two thirds as many fit. `CallHeaderBar` declared no
  `implicitWidth`, so its control row laid out at width 0; the spotlight
  strip's flat 96 px made it the bigger half of a short stage; the call panel's
  flat 45% floor bought the header, the dock and ten pixels of picture. The
  floor now asks the STAGE for `minimumUsefulHeight`, and overlay controls are
  ABSENT rather than squeezed.
- **The hidden-image contract is GEOMETRY, not visibility** — a text row in
  place of a 360x270 picture jumps every message above it, so the placeholder
  fills the media box and contributes no implicit size. An `Image` whose
  `visible` is false still holds its decoded pixmap (clear the SOURCE), and an
  `AnimatedImage` behind an opaque placeholder keeps decoding for nobody.
- **`QScreen::geometry()` is device-independent** — a 4K display at 125% listed
  as 3072x1728, a LABEL defect and not a share defect. Resolve a display by
  DEVICE NAME: Qt's screen order, `EnumDisplayMonitors`' order and
  `gdiscreencapsrc`'s `monitor` index are three unrelated enumerations. A
  Chromium window's caption is the TAB's title, so the owning application comes
  from the executable's VERSIONINFO.
- **The bundled Material Symbols font is a SUBSET** — an unmapped name renders
  as tofu and regenerating needs the network, so pick from the mapped set;
  `IconChromeTest` catches it. A brand mark in the raw accent reads as a status
  light, so `AppTheme.wordmarkBolt` keeps Storm's yellow and blends toward the
  header's secondary ink elsewhere.
- **Colour: measure before believing the symptom.** "Needs more colour" was
  SEPARATION — Storm is the most saturated shell (Lab chroma 27.1 vs Moss Light
  0.8), but every surface step was below 1.25:1 and four elevation roles were
  one literal. Hard ceiling: dark identity inks must clear 4.5:1 on four
  surfaces, capping them at luminance 0.0757, which four 1.25 rungs reach
  exactly. Contrast is NOT sufficient for identity colours — nine sender inks
  were really seven (closest pair dE 5.6/7.4) with every one passing AA — and
  an ink used as the base of its own 14% chip fill is checked against THAT.

#### Timeline, scrolling and navigation

- **The scroll teleport was FOUR paths and the reported one was not the obvious
  one.** (1) A CONVERGENCE-based landing budget re-armed forever, because
  `count`/`layoutRowsAtLastPass` change constantly during a scroll; now an
  absolute ~2 s ceiling. (2) **The actual "about 10 seconds" is a scroll-anchor
  RESTORE** — up to `kMaxNavigationBatches` (8) REAL network paginations before
  `targetLocated`, and cancelling in the VIEW cannot help because the landing
  does not exist yet when the reader starts scrolling; hence
  `PaginationController::cancelNavigation()` from `noteReaderTookControl()`.
  (3) Middle-click autoscroll left `userScrollActive` FALSE for the whole
  gesture (it writes contentY directly, so `moving` stays false), so
  `maintainViewAnchor()` took its IDLE branch and ABSOLUTELY restored contentY;
  pre-existing since v0.7.4. (4) Keyboard paging retired nothing. GENERALISE: a
  convergence budget needs an absolute ceiling, and the reader taking the view
  must reach EVERY layer that can move it.
- **After a model reset `contentHeight` still reads the OUTGOING content's
  height** (old delegates linger until deferred destruction), and
  `contentHeight >= height-1` is degenerately true while `height == 0`
  pre-layout, so the hydration gate opened early. Fixed with
  `presentationGeometryStale` plus a `height > 0` guard; both stale suites then
  went green (`timeline-hydration-qml` 8/0, `timeline-pane-qml` 63/0).
- **`SmoothWheelArea` may use only ScrollTuning's STATELESS `notchDistance()`**
  — `wheelTargetY()` mutates controller state owned by the timeline's
  anchoring. Its `parent as Flickable` was NULL in nine panes, leaving the
  shared area inert; the contract test LISTS unconverted panes.
- **State-flood scroll death is still NOT reproduced.** The proxy-suppression
  fix sketched in its commit message was deliberately NOT shipped — it would be
  a fourth speculative scroll change. The blocker is a real capture: a high
  `worstNotchMs` beside a high `stateRows`. Confirmed inefficiency: a collapsed
  state group drawing ONE summary line still instantiates a delegate per member.
- **GUI stall tracing** (`LIGHTNING_GUI_STALL_TRACE`, `src/app/GuiStallTracer`;
  default 250 ms, env value >= 50 overrides): one line per stall, coarse
  RAII-scope category, literal strings only, never content. `stalltrace::Scope`
  writes a single GLOBAL category, so it is inert off the GUI thread — a
  confidently wrong category is worse than `unattributed`.
- **The rail's drop gesture never once grouped, through THREE rules and two
  rounds that each believed they had fixed it.** All three shared one shape: *a
  reading that moves things while the user is still aiming.* Retired, do not
  re-propose: (a) "the middle 24 px of a row is the group zone" — reaching that
  middle means crossing the near edge first, which reorders, so the row under
  the pointer becomes the DRAGGED entry, never a group target; (b) a 320 ms
  dwell plus a 12 px dead zone; (c) `updateDrag(row, !dwellTimer.running)`,
  where `running` is TRUE for the whole 250 ms the dwell is served, so the
  second sample reordered and then stopped the dwell it was waiting for. The
  rule now is Discord's: the TILE is the group target, the GAP between tiles is
  the reorder target, nothing moves while the pointer is on a tile, and there
  is NO dwell because the geometry carries what the dwell stood in for.
  `updateDrag` was REMOVED rather than shimmed for three exclusive verbs
  `hoverGroup`/`hoverGap`/`clearDropTarget`, and the reorder destination
  derives from a GAP index with the `g > dragRow ? g - length : g` conversion
  the row-index version never had — separately why a one-row hover oscillated.
  **§7's rail paragraph still describes the 250 ms dwell as a second guard;
  this entry is the later record.** GENERALISE (third time): fifteen model
  cases passed through every broken rule because they hand the model a state
  production could not produce. `RailDragQmlTest` drives real mouse events at
  tile centres from real delegate geometry and asserts on the STORE; all six
  cases FAILED on the reverted tree.

#### Models, backends and derived data
- **A STORE-ONLY READ ANSWERS "NOTHING" FOREVER, and every unit test passes.**
  Widgets shipped `Room::get_state_events`, which reads the state store and
  never the network. Widget state reaches that store only if sliding sync asked
  for it in `required_state`, and matrix-sdk-ui 0.18's
  `RoomListService::subscribe_to_rooms` takes room ids ONLY — there is no API
  to extend the list. So the answer was empty for every room, always. Eleven
  unit tests were green; the first live run against a real homeserver reported
  `widgets found: 0` in a room that had four. Same shape as
  `m.room.pinned_events`, same answer as `banner.rs`: store first, one raw
  `/state` request second, on demand. GENERALISE: for any state type sliding
  sync does not carry, "the store said nothing" and "the room has none" are the
  same observable, and only a homeserver can tell them apart.
- **`RoomEventCache::events()` is the IN-MEMORY chunk, not the store.** The
  local search index's first backfill paginated N times and then read the
  events — collecting one page's worth however far back it went, because
  Lightning's own jump-to-live trim shrinks that chunk back to roughly one
  page. Reading after EVERY page is what makes the coverage real, and holding
  ONE cache handle across the loop is what stops the chunk shrinking under the
  walk. Measured in production conditions: the sweep wrote 0 rows and the
  interleaved deep index wrote 33 in the same run.
- **FTS5 is a COMPILE-TIME option of SQLite and is not on by default.**
  sqlite.org: disabled by default for the canonical source tree, enabled for
  the amalgamation's configure script. A distro that forgets `--enable-fts5`
  ships without it silently — the same "graceful absence and silent success are
  identical" shape as the AppImage's missing `libgstopengl`. matrix-sdk's
  `bundled-sqlite` makes it a build-time constant on all six platforms
  (libsqlite3-sys sets `-DSQLITE_ENABLE_FTS5` explicitly) and raises the
  feature floor to 3.50.2; on the system path the floor is Debian's and
  flatpak's 3.46.1. It also means the C++ side must NOT link `SQLite::SQLite3`
  as well, or two SQLite implementations end up in one process and which one
  answers is whichever the linker resolved first, silently, per symbol.
- **`unicode61` cannot segment CJK, so a Chinese search matches NOTHING.**
  Chinese has no spaces between words, so a sentence becomes one token. The
  trigram tokenizer matches substrings in every script at the cost of a hard
  three-character minimum — a visible limit beats an invisible one, and
  `remove_diacritics 2` on a trigram table folds case and accents on both the
  stored text AND the query, so no second folded column is needed. Measured on
  3.50.2, and the alternative is pinned by a test so the choice stays defended.

- **TWO ANSWERS FROM THE SAME SERVER, and the client picked the one nobody
  can see.** The Activity bell counted 25 highlights while no room showed
  unread. The bell's seed is `GET /notifications?only=highlight`, whose
  per-notification `read` flag said false; the room list uses
  `highlight_count` (`max(num_unread_mentions, sync highlight_count)`), which
  said nothing was unread. Neither side was "the bug" — the client believed
  the invisible one. Rule: when two server-sourced answers describe the same
  thing, the surface the user is looking at wins, and the badge is reconciled
  against it (per room, the newest N rows stay unseen where N is that room's
  count). A per-notification flag is also not durable state: the earlier fix
  assumed a read receipt would set it and the report proved it did not.

- **The mock backend being RIGHT is how a backend defect survives** —
  `RoomInfo::childRoomIds` is contractually a Space's DIRECT children and was
  that on the mock and HTTP backends, while the Rust backend filled it from
  `descendants` (the transitive closure), so Channels listed a subspace's rooms
  twice and fifteen model tests passed against the mock throughout. GENERALISE:
  when a field's contract is enforced only by the testable backend, the others
  are undefended. Fixed by reading each Space's own `m.space.child` with the
  spec comparator (`order` first, room id tiebreak, empty-`via` skipped).
- **A design where every view is the same list narrowed by a scope cannot
  express a tab** — Channels collapsed every non-`!` scope to `""`, so the rail
  had one way to say anything that was not a Space and DMs had to ride inside
  EVERY view to stay reachable. Keeping the selection VERBATIM and CLASSIFYING
  it made three real views possible. GENERALISE: when a fix must make every
  surface carry something so it stays reachable, what is missing is a PLACE for
  it to be.
- **A layout that becomes the other layout is not a layout** — Channels scoped
  itself to the active Space, so at Home the host rendered Classic and the user
  silently got the layout they had not chosen. The fix removed the premise: no
  `spaceId` at all, and rooms from the CLIENT rather than the Space-scoped,
  chip-filtered `RoomListModel`.
- **A DM is never scoped by a Space, in any filter** — Matrix gives no way for
  a DM to be a Space's child, and a scoped Space dropped the account-wide
  "Rooms" group, the only place a DM could live. The column can now say a
  filter matched nothing (`matchCount`) without claiming the ACCOUNT is empty.
- **`level = parentSpaceIds.isEmpty() ? 0 : 1` is a two-level approximation
  that looks like a hierarchy** — a three-deep tree rendered as a flat pair of
  indents. Real depth is a breadth-first walk with assign-once semantics, which
  is also what makes it cycle-safe and stable under multiple parents; a Space
  the walk never reaches becomes a ROOT rather than being dropped.
- **Announce only what you actually learned** — `DirectAvatarResolver` cached a
  profile answer only when it carried a NON-EMPTY avatar but announced EVERY
  answer, and its owner rebuilds on that signal and re-resolves, so every
  avatar-less peer and every 404 ran rebuild -> fetch -> answer -> rebuild
  forever: one `/profile` and one full rebuild per round trip, per peer. That
  was the slow account switch, a switch clearing the caches and re-arming it,
  and the comment claiming "this cannot feed itself" was false for the two
  commonest answers. Fixed by caching the NEGATIVE and announcing only a face
  learned; the rebuild is coalesced per event-loop turn and resolves children
  against the map it already built, not `directChildRoomsDetailed`, which
  materialised the whole room list and a fresh hash PER SPACE. No test saw it:
  the fixture's `fetchUserProfile()` returns 0, and the resolver skips its
  pending bookkeeping on op 0.
- **A derivation living privately in one model will be wrong in the next** —
  `RoomInfo::avatarUrl` is empty for most DMs, so the Channels column drew
  initials beside a Home strip showing real faces. A late answer must run a
  `rebuild()`, not a bare `dataChanged`: the rows hold a SNAPSHOT.
- **A room-list indicator must not be allowed to ask** — `read_membership_events`
  falls back to a full `/state` whenever the store holds no live membership,
  the normal state of every idle room, so a self-refreshing call glyph would
  issue one `/state` PER ROOM per rebuild. `RoomCallGlyph` reads only what the
  controller knows, `app.rtc.refresh` is banned by contract test, and the
  honest cost is that a call in a room nothing has poked shows nothing.
- **When a row stops being a `StateChange`, grep every branch testing for
  one** — a new `TimelineEvent::CallEvent` silently un-suppressed call events
  in `NotificationManager` (an EMPTY notification per call) and in the Rust
  backend's activity test (blanking the room-list preview).
- **A collapsed folder cannot be reported on, so it must not be written over** —
  `applyArrangement` takes the whole arrangement in one write and a folder LEFT
  OUT keeps its members; without that, a drag past one would empty it.
- **Per-row state cannot live in the delegate** — a timeline row is destroyed
  the moment it leaves the cache buffer. `MediaVisibilityStore` keys by media
  identity, bounded at 4096, and the cap releases the OLDEST rather than
  refusing the newest: refusing to hide what the user just asked to hide is the
  worse failure.
- **A `json!` past serde_json's macro recursion limit is a compile error naming
  no key** — it points at the macro, not the addition. Hoist any nested object
  into its own `let` first.
- **"Mark as read" was a silent no-op for any room but the open one** —
  `markRoomRead` walked the client's timeline, which on the Rust backend holds
  only the ACTIVE room. `mx_rust_mark_room_read` takes the target from
  `Room::latest_event()` and sends the public receipt AND `m.fully_read`.

#### Matrix protocol, privacy and lifecycle decisions

- **Read the reference implementation; do not infer a wire format.** Raised
  hands: three things would have been wrong by inference — the target is the
  sender's OWN `m.call.member` STATE event, not a timeline message (that scopes
  a hand to one call, since rejoining publishes a new membership); the key is
  TWO code points (U+1F590 + U+FE0F, visually identical to the one-code-point
  form in every editor, so the test asserts the seven UTF-8 bytes); and the
  sender must OWN the membership they annotate, or one user could raise
  everybody's hand. A redaction names only what it removed, so "whose hand went
  down" comes from a locally held `reaction id -> identity` map.
- **Message forwarding** re-sends a NEW, unrelated event with NO relation (no
  Matrix forward primitive), so a forwarded thread reply lands as an ordinary
  message. **Media is RE-UPLOADED, never mxc-copied** — the target's members
  may not be entitled to the source mxc under authenticated media, and an
  encrypted source's `file` block carries per-event keys that must not be
  planted in a room that never negotiated them. Filename and MIME are
  re-originated and sanitized: leaf-only filename, type from MAGIC BYTES — NOT
  `QImageReader::format()` (plugin-backed; WebP lives in qtimageformats, which
  the packaged fleet need not carry) and NOT `gif::validateRasterBytes` (whose
  4096 px / 25 MiB caps would refuse a 5K screenshot). Review caught three
  defects: every image forward would have written decrypted bytes into the
  saved-media store; forwarding to any room but the OPEN one failed 100% of the
  time; a server refusal after dispatch was SILENT.
- **Sliding sync delivers `m.room.pinned_events` ONLY inside a room
  SUBSCRIPTION's required state** — the open room is THE one subscription,
  replacing the previous set, and `stop_sync_and_wait` forgets it so a later
  account cannot inherit it. Relatedly, opening a room notified for its own
  backlog, which arrives as live appends while `roomVisibleAtLatest` is false.
- **Server search covers UNENCRYPTED rooms only, and every surface says so** —
  the server cannot search ciphertext, so in an encrypted room the
  loaded-timeline find is the only search and the find bar offers no History
  segment. The only content sent is the typed term.
- **UIA scrubbing is transit hygiene, never a guarantee** — buffers are zeroed
  best-effort, but on the success path the String moves into ruma's
  `uiaa::Password`, which serializes and drops it without zeroing. A real 401
  surfaces sanitized stage NAMES only, the current device is guarded out of
  per-device sign-out, and **OAuth/MAS accounts have NO password stage**, so
  their buttons open the account-management URL, never a fake prompt.
- **The ignore list is the SDK's read-modify-write of `m.ignored_user_list`,
  never a Lightning-local database** — the SDK clears the whole event cache on
  a list change (timelines reset and refetch; expected), and `senderIsIgnored`
  closes the notification race before the server stops sending. Report is
  `Room::report_content` (requires Joined); `report_room` (MSC4151) and
  `report_user` (absent from the SDK) are deliberately NOT offered, and the
  message menu uses the real room id, never the thread composite.
- **Drafts: encrypted rooms are memory-only, and an UNKNOWN encryption state
  fails closed to memory.** Unencrypted rooms persist account-scoped (LRU 256);
  saves are 1 s debounced, and the debounce is STOPPED before every room/thread
  change with the save reading the still-current key.
- **Smaller protocol decisions.** A refused `get_room_preview` still resolves,
  so Join stays offered; knock withdrawal is a Knocked-state `Room::leave`,
  because the normal leave path filters to Joined; the `/hierarchy`-backed list
  is bounded to 10 pages / 200 rows; `restricted_denied` is classified
  separately, never presented as plain invite-only.
  `mx_rust_set_space_child_suggested` reads the CURRENT `m.space.child`,
  preserves via/order, flips only `suggested`, REFUSES a non-child (empty-via
  included) and never promotes one, and "Suggested" shows only when the
  hierarchy KNOWS. `mediaDownloadUrl`/`mediaThumbnailUrl` were the last surface
  handing unauthenticated `/media/v3` links to the browser and now return empty
  on the Rust backend. A `%n` source string renders its "(s)" literally without
  a loaded translation, so "Seen by N people" is branched explicitly, and
  `tsMs` 0 renders nothing rather than a fabricated time.
- **Rail / Space Home** — a SINGLE tap on a real Space opens Space Home (which
  REPLACES the chat view), there is deliberately NO double-tap, and the ONLY
  expansion trigger is the chevron disc, whose band is excluded from the tile's
  tap. `openSpaceHome` is ordered teardown-first, activation-last because the
  loader instantiates SYNCHRONOUSLY and its handlers point RoomInfoController
  at the Space, and the old order wiped the canInvite/canManageSpaceChildren
  gates afterwards. `spaceJoined` drill-in had been an UNFILTERED listener.
- **A keyed dedup must service ALL claimants** — a saved-media star and a Copy
  image racing on the same uncached image left the star stranded forever. Both
  fetch through MediaBridge with pending-key discipline and magic sniffing (SVG
  refused). Reply-to-image thumbnails register the embedded reply event's media
  under the reply target's event id; the media KEY crosses the FFI, never bytes.
- **Presence is a bounded poll because Sliding Sync delivers NO presence
  events** (MSC4186 has no presence extension): one batch per round (raw ruma
  `get_presence`, <= 40 users, 10 s no-retry timeout so sign-out's task join
  cannot stall), 30 s rounds with rotation past the cap. Transient failures
  KEEP the last known state, forbidden/not_found erase it, and two consecutive
  all-forbidden batches of at least two distinct users each latch "server has
  presence disabled" for the session — a single user's 403 never latches.
  **Unknown renders NOTHING, never a fabricated offline.** Own presence is
  gated by the application-wide `sharePresence` (default ON, global not
  per-account; disabling publishes ONE final offline).
- **Login button naming follows Element classic's actual strings** — both
  "Continue in browser" and "Sign in with SSO" open a browser, so naming the
  MECHANISM told the user nothing; what differs is which authority
  authenticates them. Element's order is `["oauthNativeFlow",
  "m.login.password", "m.login.sso"]`, SSO is primary only when there is no
  password flow, and the homeserver host on the browser button derives from
  what the USER typed, because a server must not choose the words on
  Lightning's own button. **RETIRED 2026-09-02: the i18n catalogs are now
  REFRESHED EVERY ROUND.** They used to be deliberately left alone (~27
  strings behind, and `lupdate` rewrote all 10 files warning "Removed plural
  forms as the target language has less forms"). PR #7 added an eleventh
  language, refreshed catalogs and a CTest gate
  (`catalogsMatchTheCurrentSource`) that extracts the live source with
  `lupdate` and diffs it against every catalog, so a round that adds a
  `qsTr()` and skips the refresh now FAILS `localization`. The refresh loop is
  in `docs/localization.md`. The plural-damage risk is real and unchanged:
  count `<numerusform>` after every refresh — 6 forms for `ar`, 3 for
  `ru`/`lt`, 2 for most, 1 for `zh_CN`/`id`.
- **MediaBridge request priorities** (0 explicit playback/save, 1
  avatars/thumbnails, 2 full static, 3 speculative GIF prefetch), two slots
  reserved for interactive classes, a 15 s starvation bound, temp-file pinning
  while a QMediaPlayer holds the file, queued-speculative dropping on room
  switch, byte-sniff rejection of A/V containers on thumbnail-class results,
  offscreen player reclamation (45 s audio, 90 s video). An SDK receipt MOVE
  arrives as adjacent Set diffs, so the poll drain must not split the pair
  across 100 ms ticks. libpipewire was made resolvable in the dev shell so Qt
  Multimedia uses native PipeWire, not the PulseAudio fallback a captured FLAC
  crash aborted in. Receipt-loss mechanisms Lightning cannot fix without
  patching matrix-sdk-ui 0.18: `docs/receipt-semantics.md`.

#### Testing and harness discipline

- **A gate that PARSES what it is supposed to defend keeps passing after the
  thing moves.** `ThemeTokensTest` read the sender-name ink tables out of
  AppTheme.qml as text. When `userColor()` stopped reading those tables and
  started deriving, the test went on validating dead data — green forever,
  defending nothing. A gate over a derivation has to CALL the derivation.
- **A gate that names only some of the presets defends only those.** The same
  case covered seven of eleven themes, and Storm was not among them. That
  omission is exactly how a derivation which rendered Storm's names pure
  black got past a fully green suite; only putting it on screen showed it.
  Enumerate every preset, or the gate is a sample.
- **Every suite can pass while the feature is broken in the app**, when the
  suites all call the C++ and the break is in the QML that reaches it.
  Deleting two dead colour tables also removed the `_nameGrounds` property
  added beside them; `userColor()` then threw a ReferenceError on every call
  and every name rendered black. Nothing in ctest touches that path.
- **`ydotool` types a backtick as a DEAD KEY.** `` `inline code` `` arrived at
  the server as `ìnline code`, and it looked exactly like the renderer
  failing to style `<code>`. Read the event off the server before blaming the
  client for what a harness typed.
- **Cropping past the edge of a screenshot skews every coordinate derived
  from it.** A crop 640 px wide starting 580 px from the right edge silently
  rescales, and clicks computed from that render land a whole tab off. It
  looked like a tab refusing to switch. Keep crops inside the image.

- **`nearTopControllerDrivenBatchesCompensateImmediatelyNotChained` flakes
  ALONE**, not only under `-j8`. Measured 2026-09-03 on identical code with
  nothing touching the timeline: one failure and two passes in three
  consecutive isolated runs, and three DIFFERENT cases of `timeline-pane-qml`
  failed across four full runs the same day. §16's load-sensitive note already
  covers the suite; this widens it — a lower `-j` is not a reliable workaround,
  so re-run the case before reading a failure as a scroll regression.

- **A case that flips a shared setting must RESTORE BEFORE IT ASSERTS.** Test
  binaries here have no `XDG_CONFIG_HOME` isolation, so every case in one
  binary shares a QSettings file — and an assertion that fires while a setting
  is flipped leaves it flipped ON DISK for every later case and every later
  run. Twice in one session: a `smoothScrolling` case failed during its own
  mutation check and the next full run of `timeline-pane-qml` failed
  `realWheelEventEngagesControllerAndLeavesFollowLatest`, on correct code; a
  `hiddenComposerButtons` case did the same and took three `composer-qml` cases
  down with it. The shape that works is MEASURE, RESTORE, THEN ASSERT — collect
  the observations in a loop, put the setting back, and only then compare. The
  second failure is the expensive one, because it indicts an unrelated area.
- **Retiring a test is part of changing the design it pinned.** Merging the
  GIF and sticker buttons deleted the mono "GIF" keycap a 2026-08-21 audit had
  fixed, so `gifKeycapMatchesItsBorderlessGlyphRow` could not survive as
  written. It was REPOINTED, not deleted: the case now asserts the chip is gone
  and that the button replacing it matches the row's geometry, so the original
  defect still cannot come back.

- **A policy test that invokes the policy directly proves nothing about whether
  production ever reaches it** — recorded three times: the row window shipped
  as a permanent no-op, and the rail drop passed fifteen model cases through
  two successive broken rules. **A regression test that does not fail on the
  old code is decoration**; prove it against the unfixed tree.
- **Mutation-check every new sweep and give it a `found > 0` guard.**
  `everyRuntimeChosenIconNameIsMapped`'s C++ half tried to pattern-match its
  call sites, matched NONE of them, and passed on a deliberately broken tree;
  fixed by moving the names into a `kIcon…` block the sweep finds by prefix and
  BANNING the literal form. The Channels suite was checked the same way against
  two mutations of the FIXED tree (a Space view carrying the DM group again: 4
  failures; a Home repeating every Space: 2).
- **Anchor a source scan on the EXPRESSION, never on a fixed window after a
  name** — fourth occurrence. A case read 700 chars after `function
  clampCallPanelHeight`, and the explanatory comment inside pushed the code to
  offset 1016, so it failed on the FIXED tree. Mutation-check both halves.
- **A negated character class matches newlines** — a comment stripper using
  `(?m)\s//[^"']*$` let `[^"']*` cross newlines, so a trailing `//` comment
  consumed every following line until one ended in a quote, silently weakening
  **every scan in that file positioned after a trailing comment**. GENERALISE:
  a "strip comments" regex is a parser; assert something you KNOW is present
  and watch it fail.
- **An offscreen pixel is evidence only once every animation touching it has
  finished.** Four rounds of probes "proved" the Channels column marked the
  wrong row; every reading came from a `--demo-capture` at the default 1400 ms
  settle, and the rows' 90 ms `Behavior on color` had not advanced, so the grab
  held each row's CREATION-time colour. At 6000 ms every row was correct and
  always had been: NOTHING was wrong with the code, and one speculative fix was
  made on that false reading and reverted. A property probe rendered into a
  LABEL can disagree with the pixel for exactly this reason, which is what
  makes the contradiction diagnosable.
- **Suspect the harness first when a measurement indicts something distant** —
  `startSync()` returns silently before login completes, publishing before
  `Connected` puts no track on the wire, and sampling the SFU before a share
  starts looks like a forwarding failure.
- **Ask an agent what it OBSERVED, not what it concluded**, before writing its
  conclusion into a commit message. A "d3d11 and mediafoundation cannot load"
  finding was a static symbol comparison presented as an observed load failure;
  re-run, both plugins loaded under Wine. The DECISION survived (the absent
  import is real and Wine cannot adjudicate it); the reason did not.
- **Six of seven test failures in one round were bad tests, not bad code** — a
  ban regex matching a token named in a COMMENT, an icon regex matching `State
  { name: }`, three fixed-window source scans defeated by added comments, a
  click helper that never scrolled (Qt DROPS a press outside the window), and a
  reflow guard measuring scene coordinates so a scroll read as a reflow. Ask
  what an assertion meant to measure before deciding who is wrong, and repoint
  it with teeth rather than deleting it. `qmlformat` over `qml/*.qml` is a
  seconds-long parse gate worth running before any build.
- **`QAbstractSocket::waitForReadyRead()` cannot work against a server on the
  SAME thread** — blocking the caller is what stops the listener accepting.
  `SsoCallbackTest::deliver()` ended in `waitForReadyRead(3000)`, so each of
  eleven deliveries burned the full bound: **34.5 s of a 34.5 s suite**, and
  **1.4 s** with the wait removed. Pumping the loop in the helper is WORSE —
  the server then answers before the caller arms its `QSignalSpy`, and
  `QSignalSpy::wait()` waits for a NEW signal, so seven cases fail.
- **Contract-suite duplication detection is mechanical** — extract every
  `contains(QStringLiteral("…"))` needle per suite and rank suite PAIRS by
  intersection. One GIF-picker case had grown to 190 lines, 170 of them
  internals, with **26 of its 41 needles asserted again** in a second suite.
  Left alone deliberately: 23 suites each declare their own `MatrixClient`
  subclass with ~13 identical `override {}` stubs (~300 lines; a shared double
  would be a 23-file change).

#### Performance, disk and logging

- **The first `QVideoSink` in a process costs ~931 ms** (lazy Qt Multimedia
  init including a hardware-decoder probe that fails without VAAPI); the same
  extraction on a worker thread is 1 ms, and the per-frame theory was WRONG —
  `toImage()` is 0.24 ms. Two traps: a plain `moveToThread` leaves a MEMBER
  `QTimer` on the creating thread where Qt refuses to start it, silently
  disarming the 6 s watchdog (make it a CHILD); and the reply becomes QUEUED,
  so `disconnect()` no longer reliably cancels one already posted — session
  isolation keys on `m_posterExtracting`, not on the connection.
  `warmMultimediaBackend()` pre-pays the init off-thread for the first inline
  PLAYBACK, whose sink QML builds on the GUI thread and cannot move.
- **A log line that fires per CALLER does not belong in a default-on category;
  only state transitions do.** `avatarSource()` was the only one of five
  `alreadyPending()` branches that logged, and that branch is reached once per
  caller — O(callers), unbounded in a list. Twelve per-request lines moved to
  `lightning.media.trace`, with one counts-only burst summary once activity
  goes quiet. Separately `Avatar.qml` called the bridge from three triggers per
  instance, one of which (`onSizeChanged`) could not change the request at all,
  because `avatarSource` opens with `Q_UNUSED(size)`.
- **Where the disk goes.** A debug `libmatrix_client_rust.a` is **2.1 GB** and
  every one of the ~146 test binaries links it: `lightning-matrix` alone is 906 MB
  in `build-rust` against 124 MB in `build`, and the test binaries total
  **35 GB** there versus 5.1 GB in the non-Rust tree. With the two
  `incremental` caches (25 GB and 13 GB) the repo was 157 GB; those are pure
  caches, costing only the next build's incremental state. `nix store gc` freed
  **63 GB** — pin the dev shell FIRST (`nix develop --profile <path> -c true`,
  registering a root under `/nix/var/nix/gcroots/auto/`) or the GC takes the
  whole Qt/Rust toolchain with it. `split-debuginfo = "unpacked"` in
  `[profile.dev]` is **NOT applied**: a build-config decision for Rokas, and
  `[profile.release]` (which packaging uses) is unaffected either way.

#### Live status for these rounds

**NOT TESTED** live, and do not promote any of them: the Sable-parity round
(three Channels views, member column, call glyph); the 2026-08-19
design-deficit pass (CTest 134/134 both trees is a build result, not a GUI
one); the Element-parity round (`space-child-suggest` 4,
`element-parity-contract` 5); discovery / search / UIA / moderation / drafts;
pins / power levels / join rule / alias (real `m.room.pinned_events` round
trips, Element interop, a homeserver accepting or refusing a write, alias
publication, and the on-screen look of any of it); Matrix presence; the
2026-08-11 media/UX round; the tester report #2 round on Windows; any call
PLACED from a Windows or macOS package; an ANSWERED legacy 1:1 call.
**Live-validated**: MatrixRTC audio, camera and screen share both directions
against Element, and the Windows camera and window share on a packaged build —
CLAUDE.md §16's "Live validation: what Rokas has actually confirmed"
carries the full confirmed list, and stayed there.

