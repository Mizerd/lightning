# Round history

## 2026-09-20 (afternoon) — a predicate I read correctly and a value I never checked

I shipped a false trust badge to the highest-stakes surface in the app, a
non-author review approved it, and the only thing that found it was running
the build against a real account.

### What went wrong

`fb9081b0` rebound four trust surfaces from `is_cross_signed_by_owner()` to
`Device::is_verified()`, on the reasoning that the first is merely "signed by
the owner's key" while the second is the SDK's own verdict. Both halves of
that sentence are true. The conclusion was wrong, because for OUR OWN device
`is_verified()` is a **constant**.

```rust
// matrix-sdk-crypto machine/mod.rs:352, creating the device from our Account
// We just created this device from our own Olm `Account`. Since we are the
// owners of the private keys of this device we can safely mark
// the device as verified.
device.set_trust_state(LocalTrust::Verified);
```

`is_verified()` is `is_locally_trusted() || is_cross_signing_trusted()`, and
the first term is set unconditionally at creation — and re-established from
the server on every `/keys/query` that returns our own keys unchanged
(`identities/manager.rs:251`), so it survives restores too. The flag is true
on every session, forever, and says nothing about whether anyone verified us.

Measured on `@lightningtest:matrix.smetonis.net`, fresh profile, a build of
the shipped commit: the current session badged green **"✓ Verified"** with
Master, Self-signing, User-signing and Secrets all **Missing**, the server
reporting **28 of 28 devices unsigned**, and the app's own log saying
`bootstrap phase idle -> unverified`. Every other device correctly read "Not
verified" — the constant pins the current row only, which is why the symptom
looked like one odd row rather than a systemic failure. And
`sessionVerificationNeeded()` keys on that same string, so the app **stopped
offering verification at the same moment it stopped reporting the problem**.

### The premise was inverted

The original report was "card green, list grey, same device". The cause was
the CARD — `cross_signed || verified`, whose second arm is the constant — so
it was permanently green. The list's `cross_signed` had been right all along.
Exactly one of five surfaces was wrong, and I changed the other four to the
broken flag.

### The authority was upstream the whole time

matrix-sdk answers this exact question for itself in
`encryption/mod.rs:2063`: `Encryption::verification_state()` calls
`is_cross_signed_by_owner()` on the own device and deliberately NOT
`is_verified()`, and `VerificationState::Verified` is documented "it has been
signed by its user identity". The correction is byte-for-byte what upstream
does. **There was a published answer and neither of us looked for it.**

### THE LESSON

**Reading a predicate's definition is not knowing its value. Grep for every
writer of every term.** Two of us read `is_verified()`'s definition correctly
and neither searched for `set_trust_state`. A flag that is a constant for the
subject you are asking about looks exactly like a working flag in every unit
test, in source review, and in an SDK-source trace — the reviewer even wrote
that the old expression "already fell through to `device_verified`", calling
the constant a fallback while holding it.

Sibling of the three occurrences §16 already carries of "grep for the CALLER,
not just the definition". This is the same failure one level down: grep for
the WRITER, not just the declaration.

Corollary that cost the extra round: **when the change is to a label, ask what
the label reads on a machine, not what the predicate means in the abstract.**
Every automated test passed at every stage. A single launch against a real
account answered it in one screenshot.

### Also fixed in the correction, and not an over-revert

The all-devices rollup loses `|| d.verified`, which existed BEFORE the bad
round. For the current session that disjunct came from the same object as
`get_own_device()`, so it was constant-true and the "N DEVICES" chain step
could never be failed by the session you are sitting in.

### Left open deliberately

For rows that are NOT the current session, `is_verified()` genuinely is the
better flag — it also catches a device verified by SAS without cross-signing —
which needs `isCurrent ? crossSigned : verified`. Upstream gives no precedent
for the non-current case, and first-principles reasoning about it is exactly
what failed twice here, so it needs its own matrix first. Recorded in
`docs/open-items.md`, along with consuming
`client.encryption().verification_state()` instead of recomputing it.

### And a doc that told the next agent not to look

`fb9081b0` also committed an open-items entry asserting the regression's
symptom was "the fix working. **Do not chase it.**" CLAUDE.md sends agents to
that file before claiming anything is fixed. The correction withdraws it in
place rather than deleting it, because the withdrawal is the useful artefact.
**A confident "not a bug" note is worse than no note**, and it is written by
exactly the person least able to see the problem.

## 2026-09-20 — "Ctrl+A doesn't work" was the highlight, and a word bound to the wrong fact

Twelve commits across five surfaces, all from one GUI audit round plus two
reports from Rokas. The theme running through them: **a correct value
rendered invisibly, or a correct refusal delivered in the wrong place**, and
in four cases my own first measurement said the thing worked.

### "Ctrl+A didn't work in the text fields, Ctrl+V was fine"

Ctrl+A worked. The selection was invisible. `AppTextField.qml` set
`selectionColor: storm ? AppTheme.stormSelection : AppTheme.accentSoft`, and
both arms are wrong:

* `accentSoft` is a **tile fill**, not a selection colour, and only three of
  the eleven palettes define it — the same three whose default happens to be
  the least readable behind text;
* `stormSelection` is not defined by most palettes either and falls through
  to `hover`, which by construction is a few L\* from the field's own
  background.

Fixed to `AppTheme.selectedHover`, with a new `ThemeTokensTest` case,
`theTextSelectionIsVisibleOnEveryTheme`, asserting an **8.0 ΔL\*** floor
against the field background on all eleven and asserting the COUNT of
palettes it checked (the derived-from-token-names trick, so a twelfth theme
cannot slip past it).

**AND I FIRST REPORTED THIS "NOT REPRODUCED".** I measured it in Storm —
the one theme where `stormSelection` resolves to something visible. A
theme-dependent claim measured in one theme is a claim about that theme.
Measure it in the theme the reporter is running, or in all of them.

### A word bound to the wrong fact, three times on the Sessions page

Not fixed in this round — it is a trust label and §18 wants a non-author
review — but the analysis is done and lives in the round's notes.

matrix-sdk-crypto 0.18.0 exposes three different facts and Lightning ships
two of them to QML:

```rust
// device.rs:757
is_verified() = is_locally_trusted() || is_cross_signing_trusted(..)
// device.rs:293 — merely SIGNED by the owner's key. Does NOT require
// that we have verified that owner identity.
is_cross_signed_by_owner()
```

The device-list chip, both filter chips and the all-devices rollup key on
`crossSigned`; `verified` — the SDK's own verdict — is delivered to QML and
read in **one line of the whole file**. So the strongest word is bound to
the weakest fact, and it is wrong in both directions: a locally verified
device reads "Not verified" (what Rokas saw, contradicting the Cross-signing
card 590px above on the same page), and a device signed by an owner identity
this session has never verified can read a green "Verified" with the
`verified_user` icon. §6: trust labels come from SDK state.

**GENERALISE: when a bridge carries two flags whose names are near-synonyms
in English but not in the SDK, grep which one the UI actually reads.** Both
existed and were correct; only the binding was wrong.

### Two fixes of mine that shipped a regression, and one attribution I refuted

* `645d876f` let the call control row shrink and **installed a one-way
  door**: the compaction latch had no release path, so one narrow moment
  removed the controls for the rest of the call. `aea639c7` makes the latch
  ask the row for its own natural width
  (`callHeaderRow.width - (callHeaderRow.implicitWidth - implicitWidth)`),
  as a **function, not a property** — a property there is a binding loop.
* `9b07f825` left `qml-binding-contract` **red at HEAD for an hour** because
  I committed without running it. Three of its contracts asserted whole
  LINES of QML; they assert expressions now.
* I attributed the account switcher opening off-screen to `18b56dd8` and
  **refuted my own attribution** by checking the old code out and
  reproducing it there. The cause is that `mapFromItem()` is not reactive —
  a scene position cached before layout stays stale for ever. Fixed by
  dropping the cached `_windowTopLocalY` and the `Math.max` guard built on
  it, and positioning from the parent directly.

### Settings cards were the colour of the page on ten of eleven themes

`SettingsCard` painted `stormCanvas`, which **is** the page background, so
every card boundary vanished except on the one theme where the two tokens
happen to differ. Moved to `stormPanel`, with two nested elements to
`stormInset` so the hierarchy survives, and a `ThemeTokensTest` case naming
the raised plane. Same family as the selection colour: a semantically wrong
token that renders correctly on whichever theme you happen to test.

### The rail round: three geometric defects no source scan can see

* **The account tooltip was drawn on the avatar.** Not a tile defect — Qt
  Basic's ToolTip is `x: (parent.width - implicitWidth) / 2`, *centred on
  its anchor*, so an anchor at the rail's right edge only works while the
  tip is NARROWER than the anchor. All six anchors carried a flat
  `scaled(150)`; the account id is the widest string the rail shows (258px
  live), so 54+px spilled back over the rail. `9b6120fd` had moved the
  anchors' POSITION and never their WIDTH, which is exactly why it fixed six
  targets and left this one.
* **The group drop ring painted under the avatar.** It was `border.width` on
  the tile Rectangle, and a Qt border paints INSIDE the bounds, beneath the
  `Avatar { anchors.fill: parent }`. Zero accent pixels ever reached the
  screen. Now a CHILD outset by its own stroke — child rather than sibling
  because a drop target also scales to 1.08, and a sibling anchored to the
  unscaled bounds gets swallowed by the growth.
* **"Every tile shares one axis" was false at 5 of 9 rail widths**, on
  parity: `round((railTileSize - rowTileSize)/2)` is exact only when the two
  sizes agree in parity. Both derived sizes now round the half difference
  and double it.

### A refusal delivered at the wrong end of the run

`RailEntryModel::legalGap()` correctly refuses to land a top-level entry
inside a subspace run — and answered every such refusal by walking UP to the
slot in front of the run's owner, however far below the pointer was. A Space
released at y=560 inside an open folder's block was offered the slot at
y 169..234, **seven rows and ~360px above the pointer**, and the release
confirmed it. "Where the tile sits is where it lands" is this gesture's
stated contract.

It now walks both ways and takes the nearer boundary. It cannot oscillate,
and the reason is worth keeping because two earlier readings of this gesture
died of exactly that: `hoverGap()` MOVES the block to the slot it resolves,
and a dragged top-level row is neither a `hierarchyChild` nor a folder
member, so after a downward snap the block itself terminates the downward
walk, and the same pointer then lands in `hoverGap()`'s own no-op window.

**GENERALISE: a correct refusal still owes the user a destination near where
they aimed.** Refusing and then snapping somewhere arbitrary reads as a bug
even though the refusal is right.

### The room-list header drew outside its own panel

Card floor 120 + three fixed 30px actions + margins = 258, against a
`SplitView.minimumWidth` of 200 the user can drag to. A RowLayout that
cannot shrink draws past its anchored edge and **layouts do not clip**, so
the compass button was six pixels over at 245 and simply absent at 200. Now
a `GridLayout` stacking to one column below a threshold DERIVED from the
pieces, no term of which depends on the width the row is given — so it
cannot loop and cannot go stale when a fourth action lands.

### Harness notes from the round

* **A concurrent `ar` corrupted `liblightning-app-testlib.a`** on a quiet
  tree: the archive listed all 304 members with a bad index, and two link
  attempts failed with `undefined reference to typeinfo for MatrixClient`.
  `ninja -t clean lightning-app-testlib` then rebuild. Worth knowing while
  several agents share one build tree — §18's one-builder rule is about
  `.ninja_deps`, and this is a second artefact with the same exposure.
* **`qmlformat -i` reformats the whole file** to a style this repo does not
  use — 455 lines changed for a 50-line edit. `qmlformat -n` is safe as a
  parse check; `-i` is not safe on this tree.
* `TesterReportFixesTest` scans a **2200-character window** after an `id:`
  and asserts three item ids fall inside it. An unrelated insertion above
  pushed one out and turned the suite red. Same family as every other
  assertion in this file that goes stale on a byte offset.

## 2026-09-19 (late) — the account switcher, and a presence fix whose own tests could not fail

Six commits after the rail. Two reported defects, and two rounds of review
finding my work wrong in ways the tests were built not to notice.

### The account switcher scrolled with one account, and a probe was why

Reported as "you can even scroll about with one account since it doesn't
fit". The list sized its viewport from an off-layout PROBE of `IdentityCard`
times the model count — a real workaround for a real deadlock (`contentHeight`
stays 0 until delegates exist, and delegates only instantiate inside a
nonzero viewport) — and the probe declared `metaText` and `connected` but NOT
`trustCompleted` or `e2eeReady`. The active card renders a TrustMeter the
probe never had: **136 px measured against a real 159, 23 px short at every
account count, including one.**

**A PROBE IS ONLY AS GOOD AS ITS RESEMBLANCE, AND NOTHING ASSERTED THAT.**
The defect is invisible in the default harness because the mock backend
reports no crypto state, so the probe and the card agree there exactly —
which is why every test passed over it for as long as it existed. It was
reproduced by forcing the crypto state a real rust install reports onto a
copy: one account, with the word TRUST sliced in half by the viewport edge,
the maintainer's screenshot precisely.

The fix removes the CLASS: the menu owns one `rowH` and hands it to every
delegate, so `contentHeight == count x rowH` and the viewport is a multiple
of the same number. Both probes deleted. No property added to a row later
can make them disagree. Cards became rows on the product's existing person
ladder; the active row is marked three ways at the SAME height instead of
being three times taller. 488 px to 323 at four accounts.

**And an ink designed to be unreadable was carrying the line that
disambiguates two accounts.** The inactive MXID used `stormTextFaint`, which
falls back to `textDisabled` outside Storm — the one role the readability
table deliberately refuses to grade, because it is SUPPOSED to be low
contrast. On the light popover that is **1.60:1**, and the MXID is the only
thing separating two accounts sharing a display name, which is exactly the
maintainer's own pair. Now 5.12:1 light, 6.75:1 Storm.

**The trust meter and E2EE badge were a property of one row pretending to be
a column** — the SDK reports crypto state only for the client it is attached
to, which the old code's own comment said — so they moved to one status
strip below the list. Per-row meta is structurally impossible now rather
than forbidden by convention.

### A status line that always said "fine", over a fault that read as benign

"Remove that idle 1 spaces under the accounts... i dont even realize what
its for." Both halves earned it, for opposite reasons.

The space count is trivia in a switcher. But **"Idle" is AppController's word
for DISCONNECTED-WHILE-LOGGED-IN** — so that line was reporting a real fault,
in a word that reads as benign, beside a number that reads as noise. The
fault was unreadable because of the company it kept. The strip now speaks
only when the connection is NOT healthy: "Connected" on a working client is
the same noise as a warning that fires on a stock theme, and a line that
always says fine teaches people not to read it, and then it cannot say not
fine.

### Presence was still broken, and the earlier fix could not have reached it

A live audit against this project's own Synapse: Element Web showed the
account **"Offline for 29m" while the process had been running 51 minutes**.
From the client's own log over 33 minutes — 53 `rate_limited` rejections,
~62% of attempts, and a run of **29 consecutive rejections, about eleven
minutes** with no successful publish against a 33-63 s expiry.

**`rc_presence` IS PER USER, AND JITTER DOES NOT REDUCE AN AGGREGATE.** The
morning's fix made two devices stop colliding, which is real, and cannot
help an account whose TOTAL offered rate is over the limit — the limiter
counts per user, not per device. And the handler treated a rejection as
fire-and-forget under a comment reading "the next keep-alive tick retries
anyway", true only if the next tick is not also rejected.

Rust now extracts `retry_after_ms` from `M_LIMIT_EXCEEDED` instead of
flattening the error to a word, and a rejection arms a bounded, backed-off,
jittered retry that bypasses `kMinPublishGapMs` — that window suppresses a
duplicate PUT of an ACCEPTED state, and after a rejection there is nothing
to suppress.

**AND THE MEASUREMENT IS STILL NOT FULLY EXPLAINED.** One device at 25 s
offers 0.04 PUT/s against a 0.1/s limit, 2.5x under; four devices offer
0.16/s, predicting ~37% rejection, not 62%. That needs six or seven
publishers — or ONE device with a flapping connection, because the
Syncing-edge publish is gap-limited to one per 10 s, which is exactly the
limiter's own rate. Those two causes need opposite fixes and the ratio
cannot tell them apart.

**AND THEN IT WAS NARROWED, WITH THE INSTRUMENT THAT HAD JUST BEEN ADDED.**
The publish path had never been traced — only the READ path was — so a live
session could be asked how many publishes were REJECTED (those log) and never
how many were SENT. One trace line under the existing opt-in flag closed
that, and two measurements minutes apart settled it:

* a single client on a fixture account: 8 attempts over 165 s, steady
  intervals of 23/24/24 s, 2.91 attempts/min converging on ~2.6, and **ZERO
  rejections**. The period does exactly what it claims and one well-behaved
  client sits 2.3x under the limit.
* the client on the account that was failing, over the same minutes:
  rejections at 24, 24, 25, 25 and 23 s — **every tick, 100%** — from ONE
  process offering that same ~0.042 PUT/s.

A client 2.4x under the limit cannot be rejected by its own traffic. The
budget was being spent by OTHER publishers on that account, which is the
deduction the ratio alone could not support and these two readings do. It
also names the failure SHAPE: with several devices, the ones that lose the
race are starved indefinitely, because ceding a whole period after a
rejection means never competing for the next token — the 29-rejection run,
and precisely what the retry is for. The flap arm is not refuted; it is no
longer needed to explain what was seen.

**GENERALISE: the ratio was unfalsifiable and the RATE was not.** Two
hypotheses that a rejection count cannot separate are separated instantly by
counting the other side of the same event. When a measurement cannot
distinguish its own alternatives, instrument the denominator.

### Three assertions that could not fail, in one review

The review of that fix returned CHANGES_REQUESTED with six must-fix items,
and the three worst were tests:

* **The chain bound was asserted by counting publishes.** `m_publishRetryTimer`
  is ONE single-shot timer and `start()` RESTARTS it, so at most one retry is
  ever pending however many rejections arrive — the publish count in any
  window is insensitive to the cap. Removing `kMaxRetryChain` entirely, and
  the backoff with it, both left the assertion green. It was decoration over
  the one property that makes a retry safe against a rate limiter.
* **The floor assertion ran with no event-loop iteration** between the emit
  and the check, so the timer could not have fired for ANY interval, zero
  included. A `qWait(300)` was the whole fix.
* **The unretryable-category case waited 1200 ms** where deleting the guard
  arms 4000, so it passed on the broken code.

All three now assert `retryChainForTest()` directly — **and that accessor had
been written and never called**, with a comment describing a different member
("bounded in milliseconds" for something returning a count). The seam that
fixes all three vacuous tests was added in the same commit that made them
vacuous. Same shape as `refreshIndexStats()`, `fetch_details_for_event` and
the unregistered `ShortcutRegistryTest.cpp`.

**GENERALISE: when a test asserts a CONSEQUENCE rather than the state, ask
what else produces the same consequence.** A restartable one-shot timer
produces "at most one publish per window" whether or not the cap exists, so
the count could never see the thing under test.

Three more from the same review: `clearSession()` stopped neither the retry
timer nor the chain, so an account switch could fire a retry armed for the
PREVIOUS account against the new one, un-gapped — the dispatcher's generation
filter stops a stale EVENT but cannot stop a timer a live one already armed.
An ordinary tick reset the chain without cancelling the armed retry. And the
counters' comment said "since this session started" where nothing resets
them.

### A generator gate that had been red since the morning

`localization` compares the tracked catalogs against every `qsTr` in the
tree, and it had been RED since the theme-editor and rail-depth commits —
fourteen missing strings. Proportional validation had been run for each of
those commits and it did not include this suite, which is exactly the gap it
exists to catch: new user-visible strings were the one thing those commits
were full of. Resynced twice, each time in its own commit on a CLEAN tree,
because `update-translations` rewrites all eleven catalogs and §4 forbids
running a project-wide generator over someone else's uncommitted work.

## 2026-09-19 (evening) — the rail people can turn off, and three of my own claims that were not true

Four commits on top of the morning's eleven. Two features and two rounds of
undoing my own work, one of which an independent review had to find.

### The Spaces rail can be the plain list it used to be

`0eb37934`. The tinted-region rail is a large change to a surface that is on
screen every second the app is open, and nobody looking at it asked for it.
Appearance now carries **Spaces rail depth**: Regions, which is today's rail
and stays the default, or **Classic** — a plain top-level Space list, which
is what this client drew through 0.9.8 and what Element draws.

The first cut was wrong about what "like 0.9.8" meant. It restored 0.9.8's
*paint*: no tinted regions, no chevron plates, one tile size, and the
step-in indent (7 for a filed Space, 6 per level to a depth of 2, capped at
14) that the regions replaced. It was measured on screen and the step-in was
exactly right — top-level +0, level 1 +6, level 2 and deeper +12. Then the
maintainer said what he actually wanted: "just copy 1:1 what element does, no
fancy expanding spaces or rooms just a plain top level space list." Which is
**simpler** than what had been built, and made the whole indent machinery
dead code, because nothing is nested any more.

**CLASSIC IS A MODEL STATE, NOT A PAINT STATE, and that is the design.**
`RailEntryModel::setFlat` stops the hierarchy walk, so a subspace is not in
the row list at all, and `expandable`/`expanded` go false with it.
Hiding those rows in QML instead would have left them in the list that the
drag arithmetic, the group bands and every drop target index into — all of
them measuring rows nobody can see. Same family as the slice-and-splice
lesson: the thing that bites is not the pixels, it is everything that
counts rows.

What stays in QML is what the component draws ON TOP of those rows, and each
one follows from the rows rather than being a second opinion: the region
Repeater's `model` goes to 0 (one switch, and a model of 0 does not
instantiate rectangles to then hide them), the cap backdrop has no parent
region to be, the expander's plate has no rung to step up from, and the side
margin narrows back to what it was before the gutter grew to hold that
plate. Revealed rooms are the one thing the model cannot answer — they are
drawn from `railLayout`'s expansion state — so the rail declines to read it.

**The expansion state is KEPT, not cleared**, which is what makes the round
trip lossless: switching to Classic and back restored the full
DC -> S1 -> C1 chain with its revealed rooms, measured on screen and asserted
in the test. On Windows the same round trip is **bit-exact**, 0 differing
pixels of 55068.

**AND `std::clamp` IS THE WRONG TOOL FOR AN ENUM — my own test caught it
before it shipped.** The getter clamped the stored style to `[0, 1]`, and
`std::clamp(2, 0, 1)` is **1**: a value written by a newer build with a third
style would have landed this one on **Classic**, silently switching the rail
to a look the person never chose, as far from their real choice as the range
allows. A width can be clamped because 4000 and 260 are the same intent at
different magnitudes; an enum has no such ordering, and the honest answer to
a style this build does not know is the DEFAULT. Generalise: clamping is for
quantities, fallback is for names.

### A position floors, a total rounds, and they were never one clock

`d6d0ee25`, and it exists because an independent review found the commit
before it was wrong in the way it claimed to be right.

That commit changed `AudioPlayerCard.formatMs` from floor to round, justified
by "the video card next door already rounds". **It floors.** So do
`VideoControlBar`, `VoicePreviewBar` and both recording counters; only the
collapsed summary line and `MediaBrowserRow` rounded. So the two-clocks
disagreement the commit set out to fix was **untouched for video**, and the
audio card's own POSITION clock now ran half a second fast and would reach
the total before the audio ended. The function quoted to justify it,
`VideoPlayerCard.formatMs`, **has no caller anywhere in the tree** — the
"grep for the caller, not the definition" lesson, in the middle of a
justification.

The real defect is older and is why nobody could keep these consistent: a
POSITION and a TOTAL are different quantities and every player formatted them
with one function. At 25.7 s you have not reached 0:26, so a position floors.
25.7 s of audio IS 26 seconds to the nearest second — which is what the
summary line has always said — so a total rounds. `formatPosition` and
`formatDuration` in all three players now, `clockText` shared beneath them,
every call site moved to the one it means. The recording counters stay
floored and are named as out of scope: a counter running while you speak is a
position.

**GENERALISE: when two surfaces disagree and one of them is "obviously"
right, check that the one you are copying does what you think.** The whole
fix, its code comment, its test comment and its commit message were built on
a property of a neighbouring file that nobody read.

### Three more of my own claims the same review disproved

* **A doc comment spliced into the middle of another one steals its summary
  line.** The new `sticker_image_info` block was inserted after
  `/// Send one \`m.sticker\` to a room or a thread.`, so the HELPER was
  documented as the sending function and `send_sticker` had no summary at
  all. Two wrong doc comments from one insertion, both rendered by
  `cargo doc`, and it compiles perfectly. It also credited the
  no-decoder note to `add_to_user_pack_inner`, which does not carry it —
  `upload_to_user_pack` does.
* **A comment said "EIGHT of the nine checks that name `textPrimary`".** The
  table has EIGHT and Storm grades SEVEN. The GUI capture that produced the
  number had said eight; the prose said nine.
* **A badge that qualifies a pristine theme teaches people to ignore
  badges.** `|| readabilityUnchecked > 0` fires on the stock Storm base from
  the moment a theme is created, and in wide mode the permanent report
  column already carries the sentence. Scoped to compact mode, where the
  badge really is the only route. The TEXT change is what closed the
  original lie; the visibility clause only decides where it can be read.

And one test that could not fail: `everyCheckGradesTheColourItsRoleWouldEdit`
asserted the static alias table and GREPPED the QML for the call, but never
called `roleAliases()` — the one entry point the dialog uses. Stubbing that
delegation to `{}` left every assertion green while three role swatches paint
the muted colour.

### Windows: Regions is pixel-identical to Linux, and the theme nearly faked a failure

The rail was re-measured on the guest against a controlled Linux capture —
same account, same layout, same size, same Qt, only the renderer differing
(d3d11 vs opengl). All five rungs match in colour, x-extent and run length;
the notch at every junction carries the PARENT's rung on both platforms,
including a closing junction only Windows' viewport reached; an aligned diff
of the band columns found **0 differing pixels** in x 0..2 and **2 pixels, 1/255
in one channel** in x 72..77.

**The confound is worth more than the result.** The theme was not pinned, and
it resolved differently per platform — the guest had no `[ui]` section and
rendered a light theme where Linux rendered dark. A comparison taken before
pinning `theme=2` on both sides would have been a confident false FAIL about
a renderer difference that does not exist. **Pin every input a cross-platform
comparison does not mean to be testing.**

## 2026-09-19 — the keep-alive that was slower than the expiry, and a square corner chased twice

Eleven commits. Almost all of it came from the maintainer looking at the thing
and from one interop audit asking a second account what this one looked like
from outside. Two of the defects were mine, made earlier the same day.
Organised by lesson.

### A keep-alive slower than the expiry it exists to beat

The number it replaces was assumed and the constant's own comment said so:
"servers expire presence after a few minutes without activity", and the
keep-alive was set to **four minutes** on that basis. A second account
querying this project's own Synapse measured the real figure. It saw the
Lightning user online at 08:59, **OFFLINE for the next two and a half
minutes**, online again at 09:03 — a sawtooth, online for roughly a quarter of
a live, continuously syncing session. The server's window here is **33 to 63
seconds**: Synapse's `SYNC_ONLINE_TIMEOUT` plus its activity granularity.

**AND THE PUT IS THE ONLY LEVER.** A client normally stays online because its
`/sync` carries `set_presence`. Lightning syncs through simplified sliding
sync, which has no such parameter — verified in `rust/src/presence.rs`, where
`set_presence::v3` is the only call that touches presence at all. So nothing
about syncing told this server we were here, and the only thing that could was
the periodic PUT. It is 25 s now, strictly inside the floor.

**The `rate_limited` line in the maintainer's own logs was a second, separate
fault: a startup double publish.** `handleConnectionState` forces a publish on
every edge into Syncing, and a session start flaps `starting -> offline ->
retrying -> starting -> running`, so two identical PUTs go out about three
seconds apart. Synapse's `rc_presence` burst is 1 and refuses the second, and
the Rust side sends with `.disable_retry()` — so the first keep-alive window
was simply lost. An unchanged state is now dropped inside a 10 s window; a
real change never is. One existing case had asserted the duplicate this
removes, and now encodes the window instead of losing its intent.

**This is also what made the same day's rail badge dishonest.** `stateFor()`
answers for the own user from the locally published value, deliberately — a
server with presence disabled returns "offline" for everybody, and a user once
watched their own card read Offline. That value was right about what we sent
and wrong about what anyone saw. Fixing the cadence is what makes the two
agree.

### And the fix's own cost, measured rather than assumed

With 25 s in place the account stays online continuously: **15 consecutive
server samples over 6m55s with no gap**, against the old sawtooth of ~30-60 s
in every 240. But every client publishes on the same period, so one account
with several sessions puts **N x 2.4 PUT/min** on the server. With four
clients open, **3 of 38 PUTs came back HTTP 429** on the steady keep-alive —
not at session start. `rc_presence`'s burst of 1 does not tolerate that, and a
desktop plus a laptop is enough to reach it, which is ordinary use here.

Each client now re-arms **21-25 s** after publishing instead of exactly 25, so
two clients drift apart rather than aligning for ever. Jitter is subtracted
and never added, so the effective period stays inside the 33 s floor the
interval was measured against, and the test seam turns it off because a suite
wants a deterministic cadence. Stated plainly: **this is not a full answer** —
a 429 is still reachable if two clients happen to land together — but it turns
a periodic collision into a transient one, and a missed publish costs nothing,
because the next tick is 25 s away and the server holds the state for 33.

`presence-manager` 40/40, the new case mutation-proved against the unfixed
guard.

### A gate that could not tell "nothing yet" from "no session"

Reported from the maintainer's own launch log: ~60 MatrixRTC pokes on initial
sync collapsing into **TEN full `/state` requests**, every one returning
`participants= 0` with nothing in it but stale membership events.

`read_membership_events` had exactly two outcomes: answer from the store if it
holds a LIVE membership, otherwise ask the server. "Not live" is the ordinary
state of every idle room — **and an EMPTY store failed that same gate
identically**, so the cost was wider than the pokes: every room the user opens
paid for a `/state` too, through `setCurrentRoomId`, under a comment claiming
"a read is cheap (state store, no request)". It was not. That half never
appears in a poke trace, which is exactly why it stayed invisible while the
poke path was the thing being read.

The store read still happens on every poke; only the ESCALATION is gated. It
now needs an absence of live membership AND a reason to believe a session
exists: we are published in that room's call (the case the fallback was
written for — media-key targets), a MatrixRTC ring arrived within three
minutes, or the store's newest call-state signal is within fifteen. A per-room
backoff (15 s, doubling to 5 min) stops a ghost membership inside that horizon
from becoming the same storm in a smaller costume; a ring or our own publish
clears it. `refreshFromServer`'s forced lane is untouched.

**What it gives up, stated plainly:** a call already live in a room whose
membership state sync has never delivered shows no banner until sync delivers
it — opening the room subscribes it, so it self-corrects within a round trip.
A call STARTING is unaffected (a joining peer's membership is live, so the
store answers for free), and so are call teardown, our own call's key targets,
the incoming-call Answer gate and the SFU's unknown-participant path.

The new `source` words distinguish "we did not need to ask" from "we were not
allowed to ask" — `store-no-session` versus `store-cooling-{own,ring,recent}`
— because otherwise those are the same observable in a log. 10 new tests in
`rtc.rs`; cargo 459/459.

### A fix that shipped on reasoning alone, and the regression it caused

The maintainer photographed a square corner in the rail. My first answer was
`9474ae43`, and its own commit message says what was wrong with it: the
backdrop behind an over-cap child was a flat `radius: 0` whose comment claimed
it stood in "for a parent whose run CONTINUES through this row — that is the
only condition under which it exists", while its actual visibility is
`trueBandDepth > maxBandLayers` and says nothing about continuation. On the
LAST row of an over-cap run the parent ends exactly there, so a radius-0
rectangle leaves a hard square corner under the child's rounded one. That
reasoning is still sound as a reading of the invariant. **It was found by
re-reading the invariant after the photograph, NOT by reproducing the
photograph**, the commit said so outright, and it should not have shipped on
that.

`3ea7b320` undid it. That rectangle exists to be the PARENT's colour in the
notch a child's rounded corner opens at the cap. Giving it a radius rounds it
away from that notch, and what shows through instead is the **GRANDPARENT** —
a rung too light, under a hard full-width edge. Measured on a Windows guest at
the same junction before and after the change: before, the parent's rung
persists under the child's corner; after, the grandparent's does. A second
sweep on Linux found the same notch unfilled. If an end-of-run corner ever
does need rounding there, it needs a rectangle that is **square where the
child's corner is and rounded where the run stops** — one radius cannot be
both.

The same commit corrected a second thing of mine. The expander's plate stepped
two rungs UP the ladder and clamped at the ceiling, so on a row already at the
top rung it took its own band's colour: measured on Windows at depth 3 as
plate `#97B8A7` against band `#97B7A7`, **ΔL* 0.28**. The box the expander had
just been given disappeared exactly where the rail is busiest. It steps DOWN
when it runs out of ladder now, because what makes it read as a control is
that it **differs** from its background, not that it is lighter.
`rail-drag-qml` 17/17.

### The square corner was one wrong flag, and the view read it three ways

The real cause was never in the QML. `folderLast` is stamped by the STORE,
which knows a folder's top-level members only, and `appendSubspaces()` then
gives every nested row a hard-coded `false`. So the moment a folder's last
member is an EXPANDED Space, the flag sits on that Space instead of on the
last ROW of the block — and three separate visual defects came out of that one
row:

* the container squares its bottom at the block's true end;
* it overshoots by one `list.spacing` into the gap below;
* and it pinches mid-block at the row that wrongly holds the flag.

Measured on a capture: the container's bottom corner insets **0px where its
top insets 19**, at dpr 1.5 AND at 1.0, so it is not a rounding artefact of
one scale.

`refreshFolderRuns()` has always computed this correctly over every row — and
its only caller is the drag-preview path, so the ordinary refresh never ran
it. Same family as `refreshIndexStats()` and the unregistered test file: code
that exists, looks right, and is never reached. It is stamped in `applyRows()`
beside `stampGroupField` now, and it has to be **there rather than after**:
the bounds of a run are part of what makes two row sets the same picture, so a
reorder that only moves a run's end would otherwise compare equal and never
reach the view.

The regression case needs the last member EXPANDED to discriminate — with
everything collapsed the last member IS the last row and the broken code
agrees with the correct one, which is how this survived. Mutation-proved
against the unstamped tree. `rail-layout` 39/39.

### Depth had exactly one cue, and it was lightness

Reported as "hard to tell the layers apart". MEASURED before changing
anything, and the ladder was doing exactly one thing: rung to rung it moved
3.4 ΔL* while the CHROMA went **4.87 -> 4.63** — flat, and very slightly DOWN.
Every boundary in the rail was a brightness step and nothing else, because
`text` is a near-neutral and tinting a near-neutral rail toward it can only
produce greys.

Each rung's ink is now mixed toward the theme's own `accent` by a growing
fraction. **THE LIGHTNESS LADDER IS UNCHANGED**: the alphas are re-solved by
bisection so every rung lands on the same L* it did before, so the ceiling the
2026-09-18 round argued for does not move. What is added is a SECOND,
independent cue — chroma climbs 5.2 -> 16.5, and the perceptual distance
between adjacent regions goes from a flat 3.4 ΔE to **3.9 / 4.9 / 6.5** at the
depths where the rail is busiest.

The accent rather than a new colour, so it follows all eleven presets for
free. Kept small: the deepest region is `#222c44`, a muted navy, and the
selection language is untouched — that is a saturated 2px stroke plus an
`accentSoft` fill ON the tile, a different device from a wash three shades off
the rail's own ground. Measured on the render and not just modelled: the bands
come back L* 8.55 / 11.69 / 13.34 / 15.14 with chroma 5.64 -> 10.85 where it
used to be flat.

build-rust 210/210, build 201/201, `rail-drag-qml` 17/17 including the
eleven-theme ladder case. Live: **PASS** on Xvfb at 1430x902 dpr 1.5.

### A self badge that could not answer the one question it was for

Two reports, one badge: it did not show "the correct status", and it covered
the profile picture.

**It was a CONNECTION indicator** — green while the sync socket was up — and
it was the only self-status surface in the application, so "what am I showing
as to other people?" was the one question it could not answer. It shows real
Matrix presence now, through the same `PresenceDot` every other surface uses,
which is where the lifecycle already lives: the watch/unwatch bookkeeping,
unknown rendering NOTHING rather than a fabricated Offline, and offline drawn
as a hollow ring so the three states differ in FORM and not only in hue. The
rail was the one place hand-rolling a rival indicator.

**Connection is not lost, it moves to the tooltip** — the same argument
`PresenceDot` itself makes about `unavailable`: a dot has no room for prose,
so the only thing it can do with a second fact is paint another colour, which
is a fabricated indicator by another name. And because an unknown presence
renders nothing, the old connection dot stays as the FALLBACK for a server
with presence disabled, where otherwise the user's own tile would carry no
indicator at all.

**It covered the face because it was anchored to the tile's square bounding
box** at a 6.5% margin: that put its centre 15.4px from the centre of a disc
of radius 20, i.e. inside the avatar, so it sat on the picture. It is placed
by the geometry now — centre at `avatarR + dotR - ring` along the 45-degree
diagonal — so the state ink lands exactly at the disc's edge and only the
rail-coloured ring, whose whole job is separating the badge from what is under
it, overlaps at all.

**And the scrollbar is gone.** A 78px column of round tiles has no room for a
rail-length bar beside them, and what it drew was a hard grey line down the
one edge every region boundary meets. The wheel, a drag and the keyboard all
still scroll it.

`rail-drag-qml` 17/17, `qml-component-load` 32/32, `tester-report-2-contract`
8/8. Live: **PASS** on Xvfb at 1430x902 dpr 1.5 — badge tangent to the avatar
with a clean notch, no scrollbar, zero QML warnings.

### Collapsing an embed is `active: false`, and `visible: false` is the trap

Requested as "modern media, too much clutter — reduce all embeds into single
lines, with an expanding arrow or mouse over or keyboard shortcut". Off by
default; Settings › Appearance › Timeline.

**THE PROPERTY THAT MATTERS: collapsing sets `active: false` on the loader,
not `visible: false`.** Every media fetch in this delegate lives inside those
components, so a collapsed attachment is never instantiated and therefore
cannot fetch, decode or prefetch. A `visible: false` implementation would look
identical on screen and download everything; one of the ten new cases exists
purely to tell those two apart.

It covers the six blocks that dominate a room — image/GIF, sticker, video,
audio and voice, file, and a LOADED link preview — and deliberately does NOT
cover reply quotes, thread cards, polls or shared places, because each of
those IS the message rather than an attachment to it, and a poll collapsed to
one line hides the question and the vote. Nor the link preview's consent gate,
which is already one band and carries the reader's only control: putting a
click in front of a privacy decision is a worse trade than the pixels it
saves. Every exclusion is listed in the delegate beside the setting, so "not
covered" is a decision on the record rather than an omission.

The affordance is a visible chevron and the whole row is the target. Hover
alone is invisible at rest and unreachable by keyboard or touch; a shortcut
needs a "current message", which this timeline has no concept of — rows are
not a focus ring. So hover and keyboard come along as additions: the row takes
Tab focus, Space/Return toggle, Right/Left are directional. Expansion is
reversible and the summary line stays above the expanded embed, because a
one-way expand would mean the setting silently stopped applying to every row
the reader had ever opened. Filenames and hosts render as `PlainText`, and a
voice message deliberately drops its generated filename.

`collapsed-embeds-qml` 10/10 new, `qml-component-load` covers the new
component, catalogs regenerated for 18 new strings.

### Two facts about one file, and the string no catalog could reach

Both from a GUI pass over the feature above, and both visible as the setting
is toggled. **Audio collapsed to a bare filename** whenever its duration was
not known, while every other kind carried a second fact — and the uncollapsed
card sitting beside it said `0:00 • 281 KB`. It falls back to the size now.
**And the same file reported two different sizes**, `427 B` on the collapsed
line and `0.4 KB` on the card: the card computed its own, with no bytes tier,
and being built from bare `" KB"` / `" MB"` literals it was the one size
string in this file that no catalog could translate. Both use the one
formatter now.

### The theme maker learned to measure itself, and its review caught two things

Reported as "works, but it's very basic looking", with a request to test it on
the GUI including light and dark contrasts. So the editor now MEASURES.
`CustomThemeStore` gained a readability audit — WCAG contrast and CIE L*, in
C++ where the policy belongs — grading the pairs that decide whether a theme
is usable: body text on its surfaces, secondary text, a button's label on the
accent, text fields, hovered and selected rows, reaction pills. The dialog
around it is a three-column workspace: grouped roles with swatches and hex, a
filter, a picker with the theme's own colours offered as a lightness ladder, a
live readout that names each check in a sentence as you drag, a report panel,
and a header badge that says how many things are hard to read.

**A ONE-PIXEL SEAM WAS PAINTING A WHOLE COLUMN.** Its anchors were conditional
on `compact`, and the panel is built while the width is still 0 — so both
edges ended up anchored, the anchor system wrote `width` directly, and the
`width: 1` binding was gone for good. MEASURED as `#4C596D` across the entire
268px picker column where `editorPanel` is `#2A3140`, in both a light and a
dark base. The irony is the point: it put the readability panel's own text at
4.44:1 on a light base — the panel that enforces 4.5 failing it. Positioned
explicitly now, and re-measured on screen at `#2A3140`.

**AND THE AUDIT GRADED AN INK THE APPLICATION NEVER PAINTS.** The
reaction-pill check used `textPrimary`; a pill is painted with `reactionInk`,
which is `textSecondary`. Grading the brighter ink made the check strictly
LOOSER than reality — it could call a pill readable whose real label was not,
and it printed advice about a pixel that does not exist. Corrected, and
re-run: all eleven shipped presets still pass with the honest ink.

Three claims in comments were also wrong and are now right: the transfer
function's knee is WCAG's `0.03928` and not the sRGB spec's `0.04045`; the 3:1
bar on an accent button is a CALIBRATION decision — it is what caps the
accent's luminance, and Nordic 4.03 / Purple Dusk 3.62 sit under 4.5 by design
— and not the "large text" rule it claimed; and the import notice counted the
PREVIOUS theme, because the audit throttle's imperative assignment destroys
the binding it writes to, the same shape already recorded here for
`Image.source`.

**One test could not fail**: `lstar("#000000")` is 0 under any coefficient, so
L*'s linear segment was unpinned. A near-black case pins it, which is exactly
where a custom dark theme's checks land.

Accepted follow-ups, NOT done: a skipped check is invisible in the UI while
the badge still says "Readable"; four editable roles are never graded and that
is not recorded as deliberate; the live readout shows the ratio but not the
target; and the report rows are mouse-only.

`custom-theme` 27/27, build-rust 211/211, build 202/202. Live: **PASS** —
editor opened in a dark base, every column measured on the rendered pixels,
zero QML warnings.

### Three ways a run lied about itself

**A `ctest` killed mid-loop leaves its settings on disk.** `ComposerQmlTest`
shares one `QSettings` file across every case in the binary, and its
hidden-button case already measures first, restores, and asserts afterwards —
a discipline added on 2026-09-03 for exactly this. That defends against a
FAILING ASSERTION. It does not defend against the process being killed, and a
run killed mid-loop left a composer button hidden on disk, so three cases
failed the NEXT run on a tree that was fine. Diagnosed today, not fixed: read
a `composer-qml` failure against that file before reading it as a regression.

**A Windows job runner wedged on an inherited handle, and `cmd` said
nothing.** The `lt-windows` guest runs each job as `call j.bat >
C:\Users\tester\runner\job.out 2>&1`. One job launched `Lightning.exe` without
`start ""`, so the GUI app INHERITED the runner's redirected stdout handle and
then kept running. Every later iteration's redirect failed with *"The process
cannot access the file because it is being used by another process"* — **and a
failed redirect makes `cmd` SKIP the command entirely**, so `j.bat` never ran
at all. The stale local `job.out` could not be copied to the share either, and
only `echo %RC% > job.done` kept working: jobs looked like they ran, returned
an rc, and produced no output. This is NOT the old deleted-`job.out` trap;
nothing was deleted from the Linux side. The repaired runner uses a unique
local output file per iteration (`out_!RANDOM!!RANDOM!.txt`) so a leftover
long-lived child can never block the next job, and fixes `%RC%`, which was
expanded at block-parse time and so reported the PREVIOUS job's exit code. Two
rules are written into the runner itself: the guest owns both `job.out` and
`job.done` and the host deletes neither, and a job that starts a long-lived
GUI app must launch it with `start ""`.

**And four probes for it all ran through the channel they were testing.**
Before the handle was found, the hypothesis was that the share had gone
read-only or the guest's disk had filled, and four diagnostic jobs were
written to settle it: `net use`, `fsutil volume diskfree`, five distinct write
tests (a `cmd` redirect, `copy`, `[IO.File]::WriteAllText`, an overwrite of an
existing file, and `[IO.File]::Open`), then `ren` and `md`. Every one of them
was delivered as `job.bat` through the job runner — **the thing under test** —
so all four produced exactly the same nothing, and none of them said anything
about SMB. It was settled only by typing into the guest's Run box over RDP, a
channel that does not go through the runner: that listed the live `cmd.exe`
command lines and showed the wedged redirect. The repaired runner was
installed the same way and then proved with a nonce echoed back through it.

## 2026-09-19 (night) — a control placed by its box, and a ladder solved over a chain nobody draws

Four maintainer reports from one screenshot, one independent review that came
back `CHANGES_REQUESTED`, and a binding loop that had been in every launch log
for as long as anyone had been reading them. Organised by lesson.

### A change handler on a LOCAL binding runs inside a stranger's evaluation

`HomePane` logged `Binding loop detected for property "displayName"` on every
launch. Instrumented rather than guessed, and the order came back unambiguous:
`displayName` eval ENTER, then `activeUserIdChanged`, then the handler's write,
then the warning.

`displayName` falls back to the localpart, so it reads `activeUserId`.
`activeUserId` was itself a BINDING, and a QML binding is evaluated LAZILY — on
its first READ. That first read happened INSIDE `displayName`'s own evaluation,
so the id moved from `""` to the real one mid-binding, `onActiveUserIdChanged`
ran synchronously, and it wrote `activeAccount` — a dependency `displayName`
had already captured. Qt abandons an evaluation whose dependencies move under
it, so the greeting kept whatever the aborted pass left behind.

The rail's account tile and the Settings identity card were already right:
both drive their refresh from `Connections` on the manager's own signals and
read `app.accounts.activeUserId` DIRECTLY, with no local binding in between.
**GENERALISE: a change handler on a local binding fires during that binding's
first read — which is whenever some other binding happened to reach it first —
so what it writes is written inside a stranger's evaluation.**

The gate went into `QmlComponentLoadTest`, which had HomePane in neither its
covered list nor its recorded-exclusion list. A binding loop is a load-time
fact invisible to every source scan: the component loads, the root is non-null,
and a property silently keeps whatever the abandoned pass left. All 31 listed
components were already loop-free; the assertion is now general.

**And the data-driven case could not see this one**, which is why it has a
dedicated sibling: the mock login leaves no account record, so
`app.accounts.activeUserId` is `""` and a binding on it never changes value.
The focused case writes a real record with the secret store DETACHED (so it
puts no token in anyone's keyring) and removes it again on every exit path —
without that, its own leftovers made the run after it behave differently.

### "Clipping" was two things overlapping, and the reference was wrong

The maintainer marked a selected Space whose expander had lost its right arm.
Nothing was clipped. The active ring is drawn OUTSIDE its tile
(`anchors.margins: -4`), so a selected tile's visible edge is not
`tileColumnX` — and the chevron's gap was measured to the TILE. They occupied
the same pixels, and only on the one tile the user had just clicked, which is
why every capture taken while auditing that column looked fine.

**GENERALISE: when something is drawn outside its own bounds, every neighbour
budgeted against those bounds is budgeted against the wrong edge.** The same
literal was still next door — the tile's hover plate at `-3`, reaching further
out than the ring it shadows — and the revealed-room plate carried
`radius: 10`, concentric only at 100% scale. Both derive from one token now.

(An earlier version of this entry, and of the source comment, asserted that the
ring "won because it is declared later". It is not: the chevron is declared
after the ring and neither carries a `z`. The overlap was real and the
direction was narrated rather than measured — the same slip §16 already
records for this exact glyph.)

### The box never changed size; the INK did

"The chevrons are not centered in their box, and i dont like that the box
changes sizes between states." The plate was a constant 10x22. MEASURED at
dpr 1.5: `expand_more`'s ink is 12x6 device px and `chevron_right`'s is 7x12 —
**the two glyphs' ink boxes are transposes of each other**, so a tall narrow
pill holds a wide flat mark in one state and a tall thin one in the other, and
the pair reads as two different boxes. A SQUARE plate sized from the wider ink
is the only shape that looks the same around both.

Both glyphs also sit high in their own box, by different amounts (1.33 and
0.67 of the rise as measured in the square plate). That is the vertical half
of the rule this file already carried for x: **place the INK, never the box.**

### A ladder solved over a chain that is never drawn

The maintainer asked for more contrast between nesting levels. The first fix
re-solved the region tone ladder for an even 2.4 ΔL* per rung — and the
independent review computed what that did to the boundaries a reader actually
sees:

| drawn boundary | before | after the "fix" |
|---|---|---|
| rail -> depth 1 | 3.17 | **4.85** |
| depth 1 -> 2 | 2.99 | **2.30** |
| depth 2 -> 3 | 3.02 | **2.47** |

Rung 0 is the FOLDER container; hierarchy regions index the ladder from 1. So
the chain a reader sees is `rail -> r1 -> r2 -> r3`, and solving rungs 0-4 as
one even chain solved a ladder **whose first link is invisible** — making the
loudest boundary 53% louder and every inner one ~20% fainter, which is the
exact opposite of the report. **GENERALISE: solve for the sequence that is
PAINTED, not the sequence the data structure happens to hold.**

Rungs 1-4 are now solved for an even 3.4 ΔL* along the drawn chain, and rung 0
separately as the folder step. The sRGB half of the lesson stands: equal alpha
steps are NOT equal lightness steps, and the two ladders are not even the same
shape — against a light rail the solved alphas come out nearly even, because
at L* 85 the curve is locally straight.

### The suite that certified eleven themes was measuring one

`theRegionLadderIsEvenOnEveryTheme` set `settings.theme` 1..11 and counted to
eleven. But `AppTheme.mode` is written only by a `Binding` in `Main.qml`, which
that suite never loads — so `mode` stayed 0, `effectiveTheme` stayed on the
system default, and **eleven iterations measured ONE palette while the case
passed**. It writes `mode` on the singleton now and asserts the palettes are
DISTINCT. Mutation-proved: with the write removed it reports "11 presets were
selected but only 1 distinct palettes came back". Same family as every other
count assertion in §16 — a check that can come back silently short is the
defect, not its symptom.

### A resting slab is not a hover state

The expander's plate was first filled with `AppTheme.hover`. Measured, that is
L* 19.75 on the depth-1 region — above the ladder's deliberate ceiling (16.99)
AND above the room list beside it (13.83). One per expandable Space would have
rebuilt exactly what the ceiling round measured and removed: the rail as the
brightest vertical band in the window. It steps the rail's own ladder now, two
rungs above whatever it sits on, which follows every theme by construction.

### And the pgrep trap caught me twice in one night

`pkill -f "<pattern>"` where the pattern appears in the command line running it
kills the shell itself. Two background runs died with exit 144 this way — once
a build-and-test run, once an SSH tunnel — both times because the pattern was
written literally in the same command. The lesson was already in CLAUDE.md for
`pgrep`; it applies to every `-f` match, including the one you are typing.

## 2026-09-18 (evening) — the rail redesign, and the six defects that read correctly as source

Eleven commits, driven almost entirely by the maintainer looking at the thing
and one design agent after another measuring it. The record below is organised
by LESSON rather than by commit, because the same shape of mistake produced
most of the defects.

### The one mistake that looked like three bugs

A slice-and-splice edit removed a block from the rail's delegate and took four
lines above it with it: `expansionCol` lost its `visible`, `y`, `width` and
`spacing`. What followed:

* **no Space revealed any room ANYWHERE**, because a Column with no width lays
  out nothing;
* `visible` defaulted to TRUE, so the delegate added `expansionCol.height + 2`
  to every row while `rowBand()` — which the drag's pointer arithmetic
  accumulates — did not;
* so **a drop landed a row and a half from the pointer** and silently made a
  folder out of a Space the user was never pointing at. Measured by a GUI
  agent: the drop-target ring lit 56-103px below the cursor.

A full CTest run passed throughout, because the mock fixture reveals no rooms,
so that Column is empty there and the divergence is exactly zero.
**GENERALISE: when a slice-and-splice edit removes a block, diff what it
ACTUALLY removed — the boundary you searched for is not the boundary you
meant.**

### Two numbers that must move together, three times

Every remaining defect in this round was the same shape, and the bindings read
correctly in all of them:

* The cap seam moves a child's BAND down; the tile drawn on that band was
  still positioned from the ROW's top, so on every row that opens a seam the
  tile was drawn through the top edge of its own region. Its chevron, its
  revealed rooms and its tooltip anchor had it too. Reported as "blue DL looks
  very bad, the whole region".
* `Avatar.size` is the mask's permille denominator. A nested tile rendered at
  41px with a mask baked from 48 came out at r/size 0.356 against the 0.30
  every other tier uses — MORE round than the band containing it, the classic
  wrong-nesting read. The `Rectangle` underneath it was already correct, which
  is what made the two disagree.
* A delegate height that `rowBand()` does not mirror is a mis-drop, and this
  file now records it twice.

There is a geometric case for the whole class now: every tile must be inside
the region that holds it, on every row, in x and in y. It fails on the unfixed
tree naming the row and both edges.

### Rounding only reads where there is background behind the corner

Asked for by name — "round the shapes more around the subspaces" — the band
radii went to 14/12/10, still exactly concentric (a rounded rectangle inset by
N inside another is concentric only when its radius is smaller by exactly N).
A critique then measured what that bought: of 24 corners in a deep run, FOUR
met open rail and the other twenty met a sibling, where a corner curving in
immediately meets one curving out. **Rounder shapes, twenty pinches.** Every
region boundary spends 8px now, and the three different junction treatments it
measured collapse to one.

And a rounded corner shows what is drawn BEHIND it, which is how the last
defect of the round worked: behind an over-cap child sits the GRANDPARENT's
band, two tone steps away. The backdrop that exists to fill that notch was
gated on the SEAM, and the seam is gated on the row above already being at the
cap — so in the common case it was never drawn. **The seam and the backdrop
are different questions**: one asks "is there air here", the other asks "what
colour is behind this child's corners".

Swept all 1272 rows of the rail for others. Three came back and all three were
false positives, confirmed pixel by pixel: the chevron glyph's antialiased edge
landing on a rung's exact value, and a correctly rounded bottom corner whose
arc reads 38 -> 36 -> 34 -> 32.

### A rail is chrome, and this one was the brightest thing in the window

The tone ladder was derived from `text` at escalating alpha with no ceiling and
reached L*62 in a theme whose base is L*6 — **5.25:1 brighter than the
room-list column beside it**, where in Discord, Slack, Element and Linear the
leftmost rail is the DARKEST surface. A light slab behind a run of tiles is
also the universal language of SELECTION, so the feature was speaking the wrong
verb; and nine of sixteen tiles measured under 2:1 against the band behind
them, which is structural rather than fixture luck, because a ramp sweeping
L*21..62 must cross most of the generated avatar palette.

**The first fix capped the ladder's internal ratio and left its anchor alone**,
so a second critique found the rail still brightest and caught a number this
file had reported wrong: rail-to-deepest was quoted as 2.13:1, which was
rail-to-rung-THREE. There are four rungs. It was 2.82:1, and rung three sat at
L*31.3 against the chat pane's SELECTED ROW at L*31.1 — one value doing duty
as both "selected" and "depth 3" in one window.

The step is ΔL* 3 now, which is Discord's adjacent-panel step. Measured:
rail-to-deepest 9.08 ΔL* where it was 34.0, and +0.21 from the room list where
it was +25.1.

**AND ONE ALPHA LADDER CANNOT SERVE BOTH DIRECTIONS.** Measured across all
eleven presets: the dark ones landed at 1.40-1.53 per boundary and the LIGHT
ones at 1.22-1.36 on the same mix steps. That is the sRGB transfer curve, not a
palette problem — equal 8-bit steps are far smaller luminance steps near white
than near black.

### The rail's own centre, and what "too wide" actually was

"Icons are way too big, and top and bottom ui is not centered and stuck to the
right side." An audit measured three disagreeing centre lines at once, all
visible in the rail's bottom 200px — the tile column, the Home divider and the
bottom separator — and the separator was the one that was CORRECT, which is
exactly why the cog and avatar beside it looked wrong.

"Too big" settles from the product's own ladder in one screenshot: a room-list
avatar is 21, a "Jump back in" avatar 33, the WELCOME HERO PORTRAIT 57 — and
the rail tile was 59. A persistent navigation chip was larger than the hero.

The same audit found six things nobody had reported: a footer height that was a
literal 48 against a tile that had become 59, so the "+" clipped; a presence dot
anchored to the square BOUNDING BOX of a circular avatar, putting two thirds of
it off the disc; an alert badge anchored to the 59px button rather than the
23px glyph; RAW unscaled glyph literals at 0.37 of their tile where Material and
Discord use 0.50; a Home divider flush with its tile's left edge and 20% short
on the right; and a rail whose top inset was 18 against a bottom of 12.

**AND `AppTheme.scaled()` IS A FONT METRIC SIZING GEOMETRY.** `scaled(px) =
round(px * textScale * uiFontOptical)`, so changing the UI font from Manrope
(1.00) to Source Sans 3 (1.13) moves the rail tile 56 -> 63 and the rail 88 ->
99 with no user intent involved. That is why the maintainer's capture read
59/92 where the source said 56/88. NOT addressed — it would move geometry
across the whole app and belongs in its own round.

### Arranging what the rail shows, at every level

Subspaces and revealed rooms are draggable now, by the same mechanism the top
level has always used: a LOCAL order in `RailLayoutStore`, additive in the
stored JSON so an older layout loads as Matrix's own order. The refusal used to
read "Matrix owns this row's position" — true of the SERVER's order and never
of the rail's.

What Matrix still owns is the SHAPE: `legalGap()` clamps a subspace to
boundaries between its own parent's children, and `hoverGroup()` refuses to
file one into a rail folder.

Three things this cost, all of them worth keeping:

* **A drag that cannot GROUP has no "do nothing" reading.** `readingAt()`
  answers "on a tile" or "in a gap", and a tile means group — so a subspace
  drag was inert over most of the column's height. For those drags a tile is a
  POSITION now.
* **A handler that lives on an item its own side effect rebuilds cannot finish
  what it starts.** The room reorder's first handler sat on each row;
  instrumented, it activated with the right index and never deactivated,
  because setting the preview changes the Repeater's model and destroys it. It
  is on the column now.
* **An activity re-sort looks exactly like a successful reorder.** A capture
  was read as the room drag working when the tap underneath had opened the room
  and bumped its `lastActivity`. It was reported here as working, and it was
  not; it is live-verified now, including across a restart.

### Clicking a Space in the Home pane now shows it

It set the active Space and changed nothing a reader could see, because the
rail draws a row for a subspace only while its whole ancestor chain is
expanded. `RailEntryModel::revealSpace()` opens the chain OUTERMOST-FIRST —
each write rebuilds the rail, so the order matters — opens the Space itself and
asks the rail to scroll to the row.

### And a folder painted over its own tree

The folder container was drawn at `z: -2` while the hierarchy regions live at
-22..-17, in a colour those regions already used, so a Space tree filed into a
folder was painted flat: measured, exactly ONE region tint in the whole rail on
a three-deep tree with four open chevrons proving the app knew it was a tree.
Its margins were a RAW 6 against a scaled ladder too, so the container came out
NARROWER than what it contains. A folder and a hierarchy region say the same
thing; they are one device now.

The folder path also still had the per-level indent everything else had lost —
`tileIndent` moved a filed Space 7.5px off the shared axis, in a file whose
comment says hierarchy depth is not an offset, twice, in two copies of the same
paragraph.


## 2026-09-18 — the GUI pass that turned four "needs looking at" items into four defects, and then found eleven more

The 2026-09-17 round landed five fixes and recorded that none of them had been
looked at by a person. The pass ran, on a real build, on a fixture account in
an isolated profile, against a six-level Spaces hierarchy built for it through
the client-server API. Four of the five verified. It also found four things
nobody had asked about, three of them in the code that round had just
written.

### The rail's chevron opened and revealed nothing

Reported as **"a room did not appear under its space until Lightning
restarted"**, and filed as sync staleness with a discriminating test written
for it: join or leave a room while the tree is wrong, which forces a room-list
diff and re-emits the spaces. **The test was run and REFUTED the
hypothesis** — the tree stayed wrong. So did the structural reading behind it:
`m.space.child` is in the sliding-sync room list's `required_state`, every
room in a response gets a notable update, and `enqueue_spaces` is a GLOBAL
recompute fired by any diff for any room.

It was never sync. Space Home listed the rooms the whole time. `revealed`
reached the expansion state through `app.railLayout.spaceExpanded(id)` — a
Q_INVOKABLE, which records NO binding dependency — so it never re-evaluated
when that state changed. Expanding a Space that HAS subspaces inserts model
rows, the delegate is rebuilt, and the reveal recomputes as a side effect; a
LEAF inserts none, so only its `expanded` role changed. The chevron reads that
role and flipped open. The rooms did not appear until something unrelated
rebuilt the rail — another Space's toggle, or a restart.

Which means `143abb07` gave every Discord-style category a chevron that could
be opened and still revealed nothing: the same user report, one step further
in. **GENERALISE: a binding that reaches state through a function call is not
bound to it.** Third time in this tree — `root.info` in Space settings and
`refreshIndexStats()` are the others.

### The image viewer had no keyboard at all

`Popup.focus` defaults to false and this one never set it, so the overlay
never became the active focus item: Left/Right, Up/Down/Space, the +/-/0/F
zoom keys and Escape were all dead. `contentItem: FocusScope { focus: true }`
cannot rescue that. VideoViewerOverlay, written to the same pattern, has
always set it.

Escape is the half that matters. The round that took click-to-close off the
picture justified it with "closing is still instant everywhere else — the
scrim, Escape, the close button", and Escape was not one of them.

Found by driving it: the next ARROW advanced the counter 1 -> 2 -> 3 while
Right, Down, Space, plus and Escape all left it at "3 of 5", with the window's
X input focus confirmed and Ctrl+K opening the jump dialog in the same session
to prove keys reached the application.

### Clicking a thumbnail closed the viewer

The strip is the one piece of viewer chrome built out of bare TapHandlers. On
the default `DragThreshold` policy a TapHandler takes only a PASSIVE grab, so
the scrim's close handler fired on the same press: the picture was selected
and the viewer shut underneath it.

**AND THE COMMIT THAT FIXED IT SAID THE WRONG THING ABOUT WHY.** It recorded
that `gesturePolicy: TapHandler.WithinBounds` does NOT help and reached for an
`AbstractButton` instead. That "measurement" was taken through a broken
fixture: the strip carries `visible: opacity > 0` behind a 180ms fade, so the
synthesized click was landing on the scrim and closing the viewer for a reason
that had nothing to do with the policy — the same failure, a different cause.
With the case waiting for the fade, `WithinBounds` is provably the whole fix,
and removing it from either strip handler fails both cases. The code is back
to two TapHandlers with the policy set.

That also resolves a contradiction an audit found the next morning: the
commit's "a TapHandler never suppresses a handler on an ancestor" would have
made `imageTap` — the click-to-zoom on the picture itself — impossible, and
it has always worked. It works BECAUSE it asks for that policy.

The two surfaces a reviewer would try both worked — the toolbar (a Control)
and the picture — which is how a strip full of policy-less handlers survived.

### And a one-way-audio defect, in an unencrypted room

Two instances, one room with no `m.room.encryption` at all. The caller started
the call from an in-room surface and correctly published in the clear. The
answerer answered from the global incoming-call card, logged
`join begin encrypted= true`, required encryption inbound, and dropped every
frame — `frames dropped: no key ... count= 500`, never one `frames decrypted`.
In an ENCRYPTED room the same two instances carried audio both ways.

`RtcController::roomEncrypted()` read a map with exactly two writers,
`AppController::startCall()` and `setCurrentRoomId()`. It was filled only for
a room the user had OPENED or called FROM. Three of the four surfaces that
reach `join()` are in-room and happened to satisfy that; the fourth is an
overlay that opens nothing. **Grep for the caller, not the definition** — the
fourth occurrence of that shape here.

A second, independent bug sat in the same read: both writers passed
`!known || encrypted`, fabricating a KNOWN "encrypted" out of an UNKNOWN room,
and `setRoomEncrypted`'s downgrade guard — right in itself, encryption cannot
be removed in Matrix — then latched it for the session.

The record is a tri-state now and it is PULLED: `roomEncrypted()` asks an
installed resolver, falling back to the stored value, with Unknown still
failing CLOSED. A resolver "yes" is remembered so the irreversibility guard
covers every room the client has seen encrypted, not only the pushed ones. The
writers record only a KNOWN answer. Pulling removes the class rather than the
instance: a surface added tomorrow cannot forget to push, because there is
nothing to push.

The drop diagnostic now names both causes. It said "the sender's key never
reached this device", which is one of them and was not this one; a whole
evening went into key distribution on the strength of that sentence. The
opposite asymmetry — a peer ENCRYPTING into a call we believe is clear is
passed through and counted as flowing — is recorded in `docs/open-items.md`
rather than fixed: it fails in the safe direction, and telling it from real
cleartext needs a windowed frame-crypto trailer test.

### What the pass verified

Star/bell clear on a muted favourite; the viewer's click-to-zoom, pointer
anchoring, chrome-hide, pinned close and **both** empty bands closing; six
levels of rail hierarchy with every child under its own parent; the rail's
width stops at 90 / 100 / 140 % with the rendered pixel width equal to the
stored value every time, the narrowest surviving three restarts and the scale
round trip returning to where it started; the code block's scrollbar clear of
the widest line on three fixtures including one overflowing both axes; the
banner resolving to the deployed key against a real federated user with two
DIFFERENT values set; and a real incoming call whose D-Bus `Notify` carries
`accept`/`Join` beside `decline`.

Panning stops dead on release — three captures over 1.5 s, byte-identical.

**AND THE ONE THING THAT PASS RECORDED AS BROKEN WAS NOT.** It said horizontal
wheel does not pan a zoomed image, on the strength of buttons 6/7 moving
nothing on a real build plus the Qt fact that `WheelHandler.orientation`
defaults to `Qt::Vertical`. The fact is true and the conclusion was wrong: a
zoomed picture makes the Flickable `interactive`, and QQuickFlickable handles
the wheel itself, so that axis was covered all along. The live observation was
`contentX` already sitting at the end of a 96px range at 1.44x — **"no change"
read as "no effect", for the third time in one session.**

Re-measured on the same rig with the picture zoomed 4.3x and dragged to the
middle of its range first: six notches of button 7 moved the image's right
edge from x=1877 to x=1493, and six of button 6 moved it back. **~64px a
notch, both directions, on real hardware.**

A second handler was written, tested and REVERTED.
`oneWheelEventMovesEachAxisExactlyOnce` is what survives, and it asserts the
property that second handler would have broken rather than the one that was
never broken: a DIAGONAL event is ONE event carrying both deltas, and the
handler pair moved contentX 80 where the contract is 40.

The horizontal-only assertion is deliberately absent, and why is worth
keeping: a synthesized click on `imageTap` — which holds an exclusive grab,
that being exactly why the picture zooms without the scrim closing the viewer
— leaves that grab behind when the popup closes, and the Flickable never sees
another wheel event for the life of the process. Bisected to that one case; a
flush click and a pointer move both failed to clear it.

### And then the rail, which scaled half of itself

The chevron fix left an obvious complaint: the expanders sat 20, 23, 26, 29
and 32 pixels from the tiles they expand — measured off a screenshot, ink at
`y=151 x 14..17` against a tile left edge of 36, and drifting further at every
level, because the glyph was anchored to the RAIL and the tile to its indent.
Anchoring the glyph to its own tile made the gap ~8px and CONSTANT at every
depth, which is what `theExpanderSitsTheSameDistanceFromItsTileAtEveryLevel`
now asserts: equal gaps, the fixture required to actually nest, and the
expander close to its tile rather than parked against the rail edge.

Fixing that exposed the real defect. `143abb07` had scaled the rail's WIDTH
stops, its indent step and its minimum — and not the tile inside them. So at
140% the rail was 152px wide around a 40px tile: measured on the DC row as
`56 #C8DBCD / 38 #1E68B8 / 56 #C8DBCD`, identical ink to the 100% reading in a
rail 40px wider. Scaling the tile then exposed the rest, because nothing else
in the file scaled either: a 56px Space tile above 28px room tiles, a 40px
settings cog and a 40px account avatar, reading as three unrelated controls
stacked on one another.

**Two defects fell out of doing it rather than being the point of it, and
both are general.**

`Avatar.size` is not decoration. The provider bakes the rounded-square mask
into the bitmap as `radius * 1000 / size` PERMILLE of its edge, so a `size`
that no longer matches the rendered item rounds a real picture's corners by
the ratio of the two: `size: 40` under a 56px tile gave every Space with an
avatar a corner 40% too round. **An Avatar's `size` must be the edge it is
actually drawn at, even when `anchors.fill` decides the geometry.**

**An explicit `width` on a ColumnLayout child is a one-shot value, not a
binding the layout follows.** The account tile was written `width:
root.railTileSize` and stayed 40px at every interface size while the settings
cog directly above it — already an `implicitWidth` — scaled correctly. A
layout takes an aligned item's PREFERRED size, which falls back to whatever
`width` happened to be at the first pass and then never looks again. Caught by
the test, not by reading.

Measured on the running client at both sizes, by pixel scan: Space tile 40 ->
56, revealed room tile 28 -> 39 (0.7 x 56), account avatar 40 -> 56, and
nothing moves at 100%. `everyRailChipFollowsTheInterfaceSize` asserts RATIOS
— a pinned "56 at 140%" is a number someone edits to match the build — and it
reads the VISUAL tree, because `findChild` cannot reach a Repeater's delegates
and the whole expansion column is invisible to it.

### Six more bindings that asked once, and one file that already knew better

Same class as the chevron, found by listing every value-returning
`Q_INVOKABLE` in `src/` and grepping QML for bindings on the mutable-sounding
ones. Most hits were inside imperative refresh functions and fine. Six were
not.

`CallPipWindow.qml` resolved every row through `indexOfShare` /
`indexOfIdentity` and read no count — while `CallStage.qml`, which carries the
same two lookups, reads `shareCount`/`peopleCount` and explains why in a
comment. **The same two lines, copied, one of them right.** The expensive one
was `fillShareShown`: "fill the window with this share" is dropped by
`onFillShareShownChanged` when the share ends, and the signal could never
fire.

The "Follow this list" checkbox carried two defects in four lines: `checked`
bound to `isSubscribed()` where the controller exposes a NOTIFYING
`subscriptions` list returning exactly the same answer, and a user toggle
ASSIGNS `checked`, which destroys the binding — so after one click the box
showed the click and not the store, including when the asynchronous write
failed.

Also: the viewer's thumbnail strip asked `mediaSource()` once per picture with
no tick to re-ask on; the Home pane greeted the user by the name
`accounts.account()` returned when the pane was built; Send Later promised
durable storage for a room encrypted after the dialog instance was created,
which is a promise the feature cannot keep (an encrypted room's scheduled
message is held in memory and discarded on close).

**Two of the six could not be reproduced live, and are recorded as fixes
without a repro rather than as live-validated.** The thumbnail strip renders
identically on the fixed and the UNFIXED build, because the fixture room is
small enough that every row's bytes are already cached before the viewer
opens. And the bubble tap's missing exclusion for the action bar — eighth
instance of that shape in `MessageDelegate.qml` — is real by construction (the
bar is a plain Rectangle; its 2px padding and the 2px gaps between its buttons
reach the bubble beneath) but no stable click could demonstrate it.

**That second one cost an hour to an unvalidated probe.** The discriminator
counted bar-surface pixels in a 300x60 crop that also contained the composer's
top edge, so "7462 -> 0" was read as the bar unpinning when it was the whole
layout shifting. Re-cropped to the bar alone it reads 3081 pinned, 0
unpinned, and round-trips — **and the first thing the corrected probe showed
was that the point which "proved" the defect proves nothing.** Validate the
instrument against both states before believing either.

### The text-size slider moved the rail and said it did not

Its caption read "Interface chrome and icons keep their size", which stopped
being true when the rail gained scaled stops and finished being true when the
tiles followed. Reverting was not on the table — stops-per-scale is what was
asked for and was verified at 90/100/140% in this same pass — so the words
changed, and they now name Interface zoom (`QT_SCALE_FACTOR`, read once at
startup) as the genuinely different thing that scales everything.
`AppTheme.textScale`'s own comment carried the same claim: `scaled()` has ~250
call sites and a couple of dozen are geometry.

### And the stops only followed the scale until somebody dragged the rail

Found by measuring after the caption change: live at 140%, the rail was 100px,
and 100 is not a stop there (95/104/120/136/152). It lands half an indent step
short and draws one nesting level fewer than the grid exists to guarantee.

`SplitView.preferredWidth` IS bound to `snapWidth(settings.spacesRailWidth)`,
and `snapWidth` reads the scaled stops, so it should follow on its own.
SplitView writes that property itself while dragging and the saver wrote it
back on release as a NUMBER, which leaves it unbound for the session. **Every
earlier check passed because every earlier check restarted the app**, which
re-created the binding.

Putting the binding back created the other half: a live binding re-evaluates
on every size change, and each re-evaluation reached the saver and REWROTE the
stored width, so 112 at 100% came back as 100 after a round trip through 140%
— the user's choice creeping a stop narrower each time. The saver persists
only after a real divider drag now. Measured end to end with no restart
anywhere in it: `112/112/40` at 100%, `104/112/56` live at 140%, `112/112/40`
live back at 100%.

**GENERALISE, and it is the day's most repeated lesson: an imperative write to
a bound property is a one-way door unless it is put back.** Three instances in
one day — the follow checkbox, the rail's width, and (in the other direction)
the account tile that was never a binding at all.

### The rail draws the hierarchy as a tree

Asked for in those words — connect the Spaces and their subspaces with lines
like a tree — with two conditions attached: the chevrons must not break, and
"make the ui super clear, since this can confuse users".

Every row now carries a vertical for each ancestor that still has a branch
below it, an elbow that turns right and touches its own icon, and a descender
leaving the bottom-left of its tile when its children are showing.

**The shapes come from the ROW ORDER, not from the Space graph**
(`RailEntryModel::stampTreeGuides`, called from `applyRows`, the one chokepoint
every row set passes through). That is deliberate and it is what makes dragging
work: the preview rows ARE the model, so the tree shown while rearranging is
the tree the release will produce. It was confirmed live — a root Space dragged
from the top of the rail to the bottom carried its entire expanded subtree, two
levels of subspaces and their revealed rooms, and the tree redrew in the new
position with every elbow and corner intact.

Two values per row. `treeLastChild` picks the corner over the tee.
`treeGuides` is one bool per ancestor column: a line is drawn there when the
ancestor ONE LEVEL DEEPER has a later sibling. **The plausible wrong rule is
"draw when the next row is deeper", and it is right on a shallow tree** — it
only diverges once a LAST child has children of its own, where it puts a line
in a column whose branch has already ended. The fixture carries a third level
specifically to separate them; without it both rules answer the same and the
case passes on either.

#### The indent step went 6 to 18, and that is the whole cost

Written at 6 first, because 6 was the existing indent and changing it moves
every width stop. The lines drew, and the maintainer's reply was exact: "make
it bend to the right too on the bottom and connect to the icons". At 6 the
horizontal into each tile is three pixels. At 12 it is nine, and the chevron —
which sits ON the elbow and interrupts the line it opens — covered all of it,
so the tree looked like plain verticals again. 18 leaves 15px of horizontal,
about half of it clear of the glyph.

`widthForLevels` is tile + 2·n·step, so the stops moved from
68/76/88/100/112 to 76/112/148/184/220/256 at 100%. **The rail that showed six
levels of indent at 112px now shows two.** Depth became something the rail has
to be widened for, which is what forced the second half of the feature.

#### Too deep to draw: the rail dives

`tileIndent` has always clamped at `indentBudget`, so past the budget every row
was drawn at the SAME indent as its own parent — two tiles side by side
claiming to be siblings when one contains the other. **The tree made that
worse before it made it better**: the columns were computed from the UNCLAMPED
level, so a column landed to the right of a tile that had stopped moving and
the elbow came out with negative width and vanished. Both are clamped together
now (`drawnTreeLevel`).

Past the clamp the rail dives: the ancestor that brings the deepest row back
inside the budget becomes the trunk, its subtree is drawn from there with the
whole budget available again, and a chip at the top names the Space to come
back to. **It happens by itself.** Offering the dive and leaving the rest piled
up was the first version and it was the wrong half — the pile is the confusing
state, and a reader has no way to know those two rows are a parent and a child.

Leaving is one click and it COLLAPSES UNTIL THE TREE FITS. Collapsing only the
Space being left is not enough and the live build showed it: the depth that
forced the dive can come from a SIBLING branch, so the tree was still too deep,
the automatic dive fired again, and the chip visibly did nothing.

The chip names its DESTINATION, not where the reader already is. The focused
Space is drawn as the trunk directly below it, so naming that said the same
thing twice and left the useful fact unsaid.

#### Three details that only a magnified capture could show

None of them is visible at 100%, and none is reachable by a test:

* **The line ran straight through the chevron.** Reported as the chevron
  clipping, and that is exactly what it looks like. It sits on a rail-coloured
  disc now, which is how a file tree draws a twisty over its guide.
* **A 2px line left a 2px notch at the corner** — the vertical stopped at the
  tile's middle instead of reaching the far edge of the horizontal it turns
  into.
* **The descender started at the tile's BOUNDING BOX**, which on a rounded tile
  is below where the icon actually ends, leaving a gap between the icon and the
  line leaving it. It starts a corner radius higher, behind the tile.

And one more from a 700% capture: the horizontal was positioned AT the tile's
middle rather than centred on it, so a 2px line hung one pixel below the
chevron it runs out of.

#### The audit, and what it cost the chevron

Nine captures — 100% and 140%, widest and narrowest stop, dived and not, plus
300% magnifications — were handed to a read-only reviewer that measured them
pixel by pixel. Six findings were real and are fixed in `b3f5a3b2`: an expanded
Space at the depth limit drew the COLLAPSED twisty (the dive test was off by
one, and the dive reused the glyph for "closed"); a Space whose children are
all ROOMS drew a descender that ran 8px past the last room and stopped five
pixels above the next SIBLING's tile IN THAT TILE'S OWN COLUMN, so the eye read
one line connecting a Space to its sibling; a root's chevron stood in a column
that does not exist, floating in a whole indent step of blank rail, and at 140%
on a narrow dived rail that column falls OUTSIDE the rail so the glyph was
clipped by the window edge with one pixel showing; the elbow read as three
fragments because a 12px mask was punched for six pixels of ink; the deepest
tile shared an edge with the pane divider on three different widths; and the
hover halo painted over the last three pixels of the elbow.

**And then the chevron lost its argument twice over.** A separate glyph for
"this one dives" was tried as `chevron_right` (indistinguishable from
"collapsed") and as an arrow ("the arrows are not it"). It turns out to need no
glyph: expanding a Space too deep to draw already re-bases the rail by itself,
and the chip says where you are. So a chevron means one thing again, open or
closed.

**The chevron is also hover-only now, and that is what made the tree look
right.** It has to interrupt the line it sits on to be legible, so every
EXPANDED row was carrying a permanent break in its own corner. At rest the tree
is unbroken — the line descends, turns, and touches its tile on every row, with
nothing in the way — and pointing at a row puts the control exactly where that
row's corner is. Whether a Space is open was never the chevron's job: its
children are either drawn beneath it or they are not, and that is a far larger
signal than a six-pixel tick.

#### The indent was the mistake, and four shipping clients say so

"They keep sticking out more and more and create like a wave pattern.
Flatten the subspaces." A per-level step walks a 40px tile off its own axis
inside a rail that starts at 68px and then walks it back — the column ends up
with no baseline anywhere, and the tiles come in two sizes as well, so there is
no edge left for the eye to use. **And it is a WIDE-rail phenomenon**, which is
the perverse part: widening the rail is what buys the wave.

Two agents went out, one to find what shipped elsewhere and one to design.
They arrived at the same place from opposite directions, and the research half
is worth keeping because it settles the question rather than arguing it:

* **Element** renders no nesting AT ALL while its panel is narrow. `isCollapsed`
  returns true for every item and no child `SpaceTreeLevel` is constructed; its
  own end-to-end test is titled "should render subspaces in the space panel
  only when expanded" and asserts the child is not visible. Pressing the
  chevron in the 68px rail calls `onExpand` and **widens the entire panel
  first**. Expanded, it indents 16px per level with NO connector lines — there
  is not one `border-left` or `::before` rule in `_SpacePanel.pcss`.
* **Nheko**, the only other Qt client keeping a deep tree in a narrow rail,
  multiplies its indent by ZERO when collapsed:
  `anchors.leftMargin: paddingMedium + (collapsed ? 0 : lineSpacing * depth)`.
  No lines either.
* **Discord** gives a foldered server no horizontal offset whatsoever — every
  item is the full rail width and centre-justified, and `.folderGuildsList` has
  `overflow: hidden` and nothing else. The grouping is a tinted pill behind the
  run. Nesting past one level is unrepresentable in its wire format: a
  `GuildFolder` holds guild ids and folders are a flat list.
* **VS Code** draws the line between the two cases in one codebase: its 48px
  activity bar expresses no hierarchy and is not resizable (`minimumWidth` and
  `maximumWidth` are the same expression), while the tree lives in a side bar
  with `minimumWidth = 170`. Apple's HIG says outright: no more than two levels
  in a sidebar, and it never describes an icon-only one.

**No product draws connector or guide lines in a rail.** Element draws them
only in its full-width hierarchy explorer, at `left: 6px` inside a 12px step.

So the tiles stop moving. Every one sits at `tileColumnX`, at every depth,
forever, and the depth is carried by LANES in a fixed gutter to their left —
a ruler against a stationary origin instead of a ramp with none. `laneRegion`
is what is left once the tile, its elbow run and the edge margin are paid for;
`lanePitch` divides it. **The elbow's run is reserved BEFORE the lanes**, so
the connector into each icon is a constant width at every depth and can never
be squeezed back to the three pixels that produced "make it bend to the right
too on the bottom and connect to the icons".

It also makes the rail better where it is actually used, because depth had been
paid for twice — once by the tile and once by the line. At the 68px default the
gutter affords THREE levels where the indent afforded one; at 112px it affords
six where the indent afforded three. The stops become 68/76/86/96/106/116 and
stop there: past six lanes the gutter does not grow, so a wider rail would buy
nothing and is not offered.

#### The chevron, on its third home

In the gutter it was either indistinguishable from "collapsed" (as
`chevron_right` for the dive) or a stray mark (as an arrow), and on the elbow
it had to break the line it sat on to be legible. Hover-only fixed the line and
produced "I don't see how to collapse it" within the hour.

It is a notch straddling each tile's bottom-left corner now: permanent, at the
same place on every tile at every depth, and it never touches a line — so
`jointHalf` and the split vertical it needed are gone and the elbow is one
clean corner again. **The first attempt put it fully inside the tile and it
covered the initials**, which in an icon rail are the only thing identifying a
Space; centred on the corner it reads as a notch taken out of the tile.

Two tests were INVERTED rather than deleted, and that is the honest move when a
requirement turns out to be the defect.
`theExpanderSitsTheSameDistanceFromItsTileAtEveryLevel` required each level's
tile to sit further right than the one above; it is
`nestingMovesTheTreeAndNotTheTiles` now and requires them EQUAL, plus the
expander keeping one place on the tile it belongs to — the other half of the
same report, which was that the chevrons were unevenly distanced.
`railIndentsNestedSpaces` in the tester-report contract required a per-level
centre offset and now forbids one.

#### The tooltip fix that the obvious reading would have broken

The audit also measured the rail's tooltip covering 15 of the 28 pixels of the
tile ABOVE the one being pointed at. The obvious fix — declare a ToolTip per
row so it can be positioned — would have stepped around a security control:
`Main.qml` hardens the ONE shared `ToolTip` instance to plain text, and a Space
name is remote text. What moves instead is the ANCHOR. Qt centres an attached
tooltip on the item it is attached to and places it above, so the rail's
tooltips attach to an invisible 1px item at the rail's right edge hanging BELOW
the row: centred there they clear the rail entirely, and "above the anchor"
puts them beside the row they describe. **Before reaching for a different
mechanism, check what the current one is carrying.**

#### A misreading the instrument corrected

A later audit capture at 140% appeared to show the deepest tile clamped onto
its parent's indent — the exact failure the dive exists to prevent — and the
pixel measurement agreed: two tiles at left=200. A `console.log` in
`autoDiveIfNeeded` answered it in one restart: `deepest=4 drawable=4`. The two
tiles are SIBLINGS at level 4, and the rail was right to decline. **The rooms
revealed under a Space are not model rows**, so counting depth off a screenshot
by eye counts the wrong things; the model's own numbers are the only ones worth
reading.

### And then the lines came out again, three hours after they went in

The tree above shipped, was shown, and was answered with **"it looks way to
wavy, and the entire left side is wasted"**, then **"like flatten the
subspaces, they keep sticking out more and more and create like a wave
pattern"**, and finally **"it looks bad"**. A research agent and a design
agent were sent out on the maintainer's instruction; a third audited the
result.

**FOUR SHIPPING CLIENTS WERE READ AND NONE OF THEM DRAWS A LINE IN A RAIL.**
Element hides nesting entirely while the panel is narrow — its own e2e test
asserts that — and widens the whole panel when a chevron is pressed. Nheko
multiplies its indent by ZERO when collapsed. Discord gives a foldered server
no horizontal offset at all and tints a pill behind the run; nesting is not
even expressible in its wire format. VS Code's 48px activity bar has
`minimumWidth === maximumWidth` and expresses no hierarchy whatsoever. Two of
those are products that shipped a tree in a rail, looked at it, and took the
indent out.

The audit then measured what the lines cost here: **59% of a 116px rail spent
on line-work and void**, with depth encoded as the LENGTH of a horizontal
rule — 10px per level at the widest stop and FOUR at the default — and lanes
that began in empty background, never attaching to the parent they stood for.

So there are no lines. Containment is a tinted REGION behind the run, which
costs no horizontal space, is what Discord does, and is what this rail's own
folders had been doing since they shipped. Depth past the first step is
carried by SIZE — Space, subspace, room — which is Element's answer and is
also free, and the tint saturates after two steps so a deep tree does not
walk towards black. Everything that existed to pay for the indent went with
it: lanes, elbows, the width STOPS (each bought one more level of it) and the
auto-dive, which existed only because a rail could run out of depth.

#### Five defects, four found by measuring and one of them mine

**`bandTop` and `bandBottom` were never DECLARED on the delegate.** QML hands
back `undefined` for an unknown property on a delegate and says nothing, so
for several hours the field's corner-squaring children were `!undefined` —
every run square at both ends — and the gap separating one group from the
next was never added. Nothing in a capture said so, because a square-ended
tint still reads as a tint. It was caught by a geometric case comparing
`rowTop()` against the delegates. **Same family as the unregistered test file
and `refreshIndexStats()` with no caller: code that exists, looks right, and
is never reached.**

**`rowBand()` returned one constant for every row but the first**, which was
exactly true while every tile was `railTileSize` and stopped being true the
moment a nested tile became a step smaller and a group's last row started
carrying the gap below it. `rowTop()` is what EVERY drop decision is made
against — it is derived rather than read off the delegates precisely because
the move and displaced transitions interpolate `y` for 140ms — so the error
was 6px per nested row and 8px per group ABOVE the pointer: nothing on a
shallow rail, a slot off on a deep one. Every band derives from its own row
now, the divider included, so `firstRowBand` has nothing left to sample.
**A derived quantity has to be re-derived when the thing it describes
changes, and nothing will tell you it has not been.**

**`ListView.spacing` left a seam down the middle of every group.** A per-row
rectangle that stops at its own delegate cannot cover the 4px of rail
background between consecutive ones. Found by sampling a vertical line of
pixels out of a capture — 61, 61, 61, then 14, 14, 14, 14, then 61 again —
not by looking at it.

**A revealed-room run floated on bare rail while a nested-Space run sat on a
tint**, though both are the same statement about the same tile. The rooms are
drawn inside their owner's delegate rather than as model rows, so they carry
no `bandStep` and have to take their owner's, one step deeper.

**And one that was not a defect at all, which is the mistake.** The gutter's
width was a literal with nothing tying it to the expander it holds, and the
first version of this entry said the chevron was laid out at x = -2 and
clipped by the rail's edge. It was not. That came from eyeballing a 2x upscale
and assuming an `Icon` is as wide as the `size` it is given; an Icon sizes by
FONT PIXEL SIZE and a chevron's advance is 7.2px of the 12 it asked for, so
the arithmetic was out by the difference. The real finding is smaller and
still worth fixing: the glyph sat 2.8px from the rail's outer edge and 4px
from its own tile, closer to the window frame than to the thing it belongs
to. **ASK THE INSTRUMENT, not the arithmetic** — the second time that lesson
has been paid for here, after `level`'s -350 dBFS floor.

#### Two regression cases that passed on broken code first

`theGroupFieldFollowsTheRowsRatherThanTheGraph` was written against a fixture
of one child per level, and `<` versus `<=` gives the same answer everywhere
except at a SIBLING boundary — which that fixture never contains. It passes on
the wrong rule. The fixture now has three adjacent leaf siblings.

`everyRowTopMatchesTheRowThatIsActuallyThere` was written against two roots
each with one nested child, and passed on a constant band by arithmetic
accident: a nested tile is 8px shorter and a group's last row is 8px taller,
so a nested LAST row is exactly the constant at every width. A three-level
chain has a nested row that is not last, and that one cannot cancel.
**A fixture that cannot contain the distinction is not a test of it**, and
both of these were only found by running them against the unfixed tree.

#### What was verified live

On an isolated Xvfb display against the `lightningtest` fixture account, on
the real Rust build: nested Spaces and revealed rooms both sitting on one
continuous field, a drag-reorder of a Space across a group with the field
drawn from the preview's own rows, and the whole rail at 140% text size. The
field is deliberately NOT hidden during a drag — the lanes it replaces were —
because `stampGroupField` runs inside `applyRows`, so what a reader sees
mid-gesture is what the release will produce.


## 2026-09-17 (second review round) — the sweep that read one file, and three verdicts that could not fail

The same reviewer, a second time, on the work the first round produced. It
returned CHANGES_REQUESTED again and it was right to.

### The fix that claimed more than it did

`161b04c`'s subject was "every queue on a live path" and its sweep read ONE
FILE of three. Four more bare queues were shipping: the share-audio publish
path (whose SIBLING forty lines up was bounded), and the whole 1:1 lane in
`GstCallMediaBackend` — capture and both receive queues — which is the SAME
defect 0.9.7 fixed in the SFU lane, in a backend installed in every WebRTC
build. Eleven queues, not seven. **A sweep is only as wide as what it reads,
and a count assertion over too few files is a confident wrong answer.** The
sweep reads all three media sources now with a per-file count.

The claim had also been copied into four places — the commit message, the
test's own comment, CLAUDE.md §2, the release notes — and into the self-test
TRANSCRIPT, which every package validator prints into its log. A false claim
shipped inside an artifact.

### And one queue should not have been made leaky at all

The video RECEIVE queue holds RTP PACKETS in front of the depayloader.
Leaking there does not drop a frame, it corrupts the VP8 bitstream — and the
drop is DOWNSTREAM of webrtcbin, which therefore sees no loss and sends no PLI
(nothing in this tree sends one). Its latency protection already existed two
elements on, at `appsink max-buffers=1 drop=true`, which discards a late frame
after decoding it intact. Bound kept, leak removed, and the sweep exempts
exactly that shape and nothing else.

### Three things that could not fail

**The self-test reported PASS when its control never ran.** `ok` was only set
false by a SHIPPED probe. So a control that failed to start left the verdict
at PASS — and if the starvation mechanism itself ever stopped working, every
row would read low, shipped and control alike, and the run would report PASS
having demonstrated nothing. The control is the entire evidentiary basis of
the measurement and it was printed, never asserted. It must now hold ≥500 ms.

**A probe that measured nothing printed a fabricated FAIL.** Outside the dev
shell all three rows read `could not run: no element "audiotestsrc"` and the
verdict read "a live queue was still holding a backlog after its consumer had
caught up" — a specific measured fact that never happened, in a release log.
Three outcomes now, on an exact `VERDICT:` line: pass, fail, **unmeasurable**,
and unmeasurable DIES where fail only warns.

**The `jpegenc` ratchet was a raw-text grep.** Deleting the tuple entry fails
it; moving the quoted name into a COMMENT passes green while the Wine probe
silently stops asking. AST-parsed now, with the count asserted — 43 had lived
only in a commit message.

### The gate that would have caught its own defect one release late

`test-flathub-manifest-pin.py`'s tag-window clause keyed on
`newest == pending`, which holds from the moment a release is tagged until the
version is bumped for the next one — the whole inter-release life of the
repository. The drift it was written for (manifest at v0.9.6 while the tree
was 0.9.7) IS that state. And its `git tag --list` had no `check=True`, so a
broken git was indistinguishable from a tagless clone: measured with a shim
exiting 128, `all 5 checks passed`, rc=0, over a pin carrying any sha.

### The licence gap was never Windows-only

Recorded as a property of the Windows package. The AppImage stages ELEVEN
gst-plugins-good binaries and staged no licence text at all — a grep for
licen/COPYING/LICENSE across that script and its validator returned one hit, a
comment about HEVC — and the snap repacks the AppImage. And the blocker was an
assumption: "sourcing the text" had been read as needing a 960 MB builder
image rebuild. It needed a file in this repository.

### What it measured, in the end

Pipeline 230 finished **14/14**, every Linux format asked of the artifact its
own job had just built. Seven environments, five GStreamer versions, two
operating systems: shipped queues at 40 and 90-100 ms, the GStreamer default
at **1000 ms and still 1000 ms** once its consumer is back at real time. The
numbers do not move. That is both the measurement and the answer to whether
wall-clock thresholds are safe as a gate on shared runners.

### Mistakes made in this round

**I "fixed" a value that was a measurement.** A package logged `microphone
level peak= -350 dBFS`; 16-bit audio floors near -96 and -350 is also the bus
handler's own starting value, so I guarded any reading that low out of the
silence detector. Then measured: `gst-launch-1.0 audiotestsrc wave=silence !
level` posts `-349.99999992181608`. It is the element's floor for digital
silence — the guard would have made "your microphone is capturing nothing"
unreachable for a genuinely dead microphone while leaving it working for a
quiet room. The suite already said so in a comment and I changed four fixtures
away from it. Withdrawn before shipping. **Ask the instrument before calling
its output a bug.**

**I hypothesised that MJPG was the Windows camera's cause and it was not.**
The A/B ran — move `libgstjpeg.dll` out of the package, no rebuild needed —
and the picture appeared on BOTH chains, at both ends. Recorded as NOT
REPRODUCED rather than fixed, with what to check instead of what to guess.

## 2026-09-17 — the review round: four claims withdrawn, six queues found, and voice delay made askable of a package

An independent read-only review of the 0.9.8 tree, asked for the handoff it
would normally get and told to be hard about it. It returned
CHANGES_REQUESTED. Everything below came out of it; nothing here was found by
using the application.

### What it found in the tree

**SIX OF THE SEVEN QUEUES IN THE MEDIA ENGINE WERE UNBOUNDED, in the file whose
comment says in capitals that a default queue holds one second.** 0.9.7 bounded
the audio capture queue after a report of ~1 s of extra delay in one direction,
and stopped there. Three of the six were live paths: the tee branch feeding
`vp8enc` (the element measured at 2.6 cores sustained on a 4K share, so the one
most able to fall behind, and it holds RAW frames — a second of 1280x720 RGBA
is ~110 MB), and BOTH receive queues, which are on the side where a listener
actually experiences delay. The measurement that "proved" the send-side fix
could not have seen them; it was taken on the audio send path. Fixed in
`161b04c`, with a SWEEP rather than seven assertions so the eighth queue cannot
be forgotten.

**THE BADGE FIX FROM THE NIGHT BEFORE HAD THE OPPOSITE HOLE.** `2b6f6ce`
replaced "badge on the first undecryptable frame" with "badge on fifty
consecutive failures", and a consecutive run is reset by ANY good frame — so a
stream delivering one usable frame in every fifty never badges at all. The
first version badged on one bad frame; the second could badge on none, on a
security-adjacent E2EE indicator. It is a failure RATE over a sliding window
now, raised at 90% and cleared at 25% so it cannot flap, as a PURE STRUCT on
SfuMediaEngine — because what made both wrong answers invisible is that every
existing test over that probe asserts LOG LINES, and the log lines did not
change in either revision (`76b640a`).

**AND THAT FIX ASSERTED A ROOT CAUSE ITS INSTRUMENT COULD NOT SEE.** Its commit
message and source comment both stated that the one bad frame in 3101 came from
a media-key rotation leaving frames in flight. The evidence was
`decrypt failed count= 1 passed= 3101`, and that line cannot identify a
rotation: `decryptFrame` returned an empty QByteArray for SIX distinct faults
and the probe saw only `isEmpty()`. The design argues against the stated cause
too — the cryptor keeps a 16-slot ring and rotation advances by one, so the
previous key is still installed. Claim withdrawn; `DecryptFailure` names which
of the six it was, with the key index, so the next live call answers it.

**A GATE WRITTEN THE NIGHT BEFORE WOULD HAVE FAILED ON RELEASE NIGHT, TWICE.**
`test-flathub-manifest-pin.py` landed after the release commit, so its FIRST
EVER EXECUTION would have been the release pipeline — the shape this guide
records three times, and the shape that killed 223 and 224. With no tags in the
clone (a shallow CI fetch) its predicate collapsed to `tag == pending` and a
correct pin failed. And after the tag landed, the allowed set became
{v0.9.8} while the manifest still said v0.9.7, so every pipeline failed until
the re-pin — a window that is structural, because a release commit cannot pin
its own sha (`ea4432e`).

**`jpegenc` was never probed in the Windows package** although the app decides
whether a camera takes the compressed path by BUILDING a chain through it
(`763493e`). Same shape as `libgstsctp-1.0-0.dll` present while `sctpenc` was
missing.

### Four claims withdrawn

Detail in `docs/live-validation.md` and `docs/releases/v0.9.8.md`; in short:
"and renders" on the 0.9.8 camera (a changed stage region is not a picture, and
ON was DARKER than OFF, which is also the no-picture signature); "the queue fix
proven on a shipped binary" (the unfixed binary was never run); "both packages
were checked by running them" (a Wine registry probe proves an element
REGISTERS); and "interoperability, measured" (twelve legs claimed, six carry a
number — a tone cannot measure a screen share).

### Two claims that got STRONGER on the same read

The Windows camera's encode half was being inferred from the capture counter,
which sits on `capsrc`'s src pad — upstream of `jpegdec`, the scaler and the
encoder — so it could only ever say what the CAMERA produced. The failing
session's own log carries the real line: `publish first encoded frame
screenShare= false afterPublishMs= 894 firstCaptureMs= 786`, on the publishing
bin's encoder pad, with the encrypt probe on that same pad then climbing past
6500 while the capture counter reached 2000 (the rate stage duplicating a
10 fps capture up to the pinned 30). So `jpegdec` — the newest and least
exercised element in that chain — is exonerated, and the search narrows onto
getting a decoded frame onto a surface.

### And the blocker was replaced rather than documented

**`--call-queue-selftest`** (`88c5668`). Voice delay had only ever been measured
acoustically, which needs two machines, a sound card and a rig — hence Linux
only, hence a Windows guest that could not be measured at all (no sound card;
RDP playback drifting 291 -> 545 ms, larger than the effect), hence a bar of
three platforms that could not be met that way.

One command asks the property directly, with no GUI, account, homeserver, sound
card or second machine, against a PACKAGE:

  SHIPPED queue max-size-buffers=4 leaky=downstream
      peak while starved 40 ms    at realtime 40 ms    free 0 ms
  SHIPPED queue max-size-time=100000000 leaky=downstream
      peak while starved 100 ms   at realtime 90 ms    free 0 ms
  CONTROL queue
      peak while starved 1000 ms  at realtime 1000 ms  free 0 ms

Three things make that evidence. The specs are read out of the description
`videoPipelineDescription()` produces, so it cannot test a string that merely
RESEMBLES production's — this file has shipped that mistake three times. It
STARVES THE CONSUMER with `identity sleep-time`, which is the defect's own
mechanism and the thing `SIGSTOP` cannot do (freezing stops producer and
consumer together, so no backlog forms and a flat result means nothing). And it
runs a plain `queue` beside each one as a CONTROL in the same run.

The phase that decides it is the third: the consumer is restored to REAL TIME,
which is all a live encoder ever gets. A live source makes one second of audio
per second, so a consumer that merely keeps up can never give back what it fell
behind by. **The default still holds its full second there.** That 900 ms is
the number this project has asserted in a source comment since 2026-09-16 and
never demonstrated.

### The Flathub repo lint ran for the first time, and what stopped it was a session bus

Three attempts produced nothing and the third was reported as "the build failed
to produce a repo". The cause: `flatpak-builder` inside the `org.flatpak.Builder`
sandbox resolves its sdk by running `flatpak info` ON THE HOST through the spawn
portal, which needs a session bus carrying `org.freedesktop.Flatpak`. The rig
container has none, so every build died at init on "Unable to find sdk
org.kde.Sdk version 6.11" — while `flatpak info org.kde.Sdk//6.11` in the same
shell printed the ref. `dbus-run-session` is the entire fix; D-Bus activates the
portal from `/usr/libexec/flatpak-portal` by itself.

REFUTED on the way, and recorded rather than dropped: that Debian's
flatpak-builder 1.4.4 and Flathub's 1.4.9 differ here. A full rebuild under
1.4.9 produced the same two errors.

Two smaller traps paid for: `cmd | tail` makes `$?` the status of tail (it
reported `builder rc=0` over a build that never started — use `PIPESTATUS`), and
the flatpak sandbox maps host uid 1000 to nobody, so a work directory owned by
the host user is read-only inside it.

### Mistakes made in this round

**`git checkout --` to undo a mutation test also discarded the real work in the
same file.** The header carrying `BlockedRunPolicy` was reverted to HEAD along
with a two-constant mutation, and had to be written again. Mutate a COPY, or
back the file up first — `cp` before, `cp` back after, never `git checkout`.

**A source sweep matched C++.** `everyLiveQueueIsBoundedAndLeaky` looked for
`\bqueue\s` and reported `queue = gst_bin_get_by_name(...)` as an unbounded
pipeline queue the moment the self-test gave the file a variable of that name.
A pipeline queue is followed by a pad separator or by one of its own
properties; the regex says so now.

## 2026-09-16 (afternoon) — the call nobody could hear, and four instruments that could not have found it

**Reported:** "calls used to work fine and dandy, now they stink — i hear myself
from element to lighting but not from ligthing to element." Live-validated FIXED
the same day: audio both ways, and the send latency down from ~1 s to
"almost instant".

**It took a whole day and most of that was spent in the wrong subsystem.** The
crypto path was searched first and exhaustively — key indices, `targets=`, the
adopt guard, resolved target devices, the Olm identity compared byte-for-byte
against the server, RED wrapping, the to-device payload shape, room power
levels, widget capabilities, membership visibility. Four real defects were found
and fixed on the way (`268afd9`, `4e40467`, `121ea36`, `13b6a57`, the cleartext
downgrade being the one that mattered) and **not one of them was the report**.

### What it actually was

* **A multi-input capture device lost 12.04 dB to its own downmix.** A Roland
  Rubix44 presents FOUR channels and publishes no channel-mask; the microphone
  is on input 1 and inputs 2-4 are empty. The chain asked for `channels=1` and
  that request propagated all the way back to the source, so PipeWire averaged
  four channels before a sample reached us. Measured with the maintainer
  speaking: input 1 at **-21 dBFS**, inputs 2/3/4 at -86/-67/-92, and the chain's
  output at **-33 dBFS** — 20*log10(1/4) to two decimal places. Element takes the
  input that has the audio, which is why the same device worked there, and why
  "same device works on element just fine, has to be app issue" was exactly
  right. Fixed with a pinned channel-count capsfilter (`channel-mask=0`,
  UNPOSITIONED) plus an `audioconvert` mix-matrix taking input 1. Stereo is
  deliberately untouched: a two-channel MICROPHONE is a microphone, and §16
  already records a Windows mic with signal in one channel only where averaging
  costs 6 dB and being quiet in both ears beats being absent from one.
* **A default `queue` holds ONE SECOND and never leaks it.** `max-size-time`
  defaults to 1000000000 with `leaky=no`, so a live capture whose encoder falls
  behind once fills it and the backlog becomes permanent latency for the rest of
  the call. Reported as ~1 s from Lightning to Element against ~0.2 s the other
  way — same SFU, same network, so the asymmetry was ours, and it matched the
  queue's capacity almost exactly. Now 100 ms, `leaky=downstream`.
* **Media keys that arrived before the call was active were discarded.** The peer
  already in the room sends its key the moment it sees our membership, which can
  be before our own SFU session reports active. Measured: THREE keys dropped per
  call, and nothing re-sends them. They are parked and replayed on join now,
  bounded to 8 and cleared on teardown — the same thing matrix-js-sdk does with
  `keysWithoutMatchingRTCMembership`.

### The instruments, which are the durable part

**SILENCE ENCRYPTS EXACTLY LIKE SPEECH.** `opusenc` turns a silent buffer into a
real frame, the encrypt probe authenticates it, the far end decrypts it. A call
capturing a dead device reports `frames encrypted ... count= 1500 dropped= 0`, a
green padlock and a connected transport while nobody can hear a word. There is
now a `level` meter in the capture chain logging `microphone level peak= N dBFS`
every 5 s whatever it says, a sustained-silence warning, and a badge in the call
header.

**AND `frames encrypted` WAS MEASURED IN THE WRONG PLACE — it was quoted as proof
of transmission for six hours and it is not.** The probe sits on the ENCODER's src
pad, upstream of the payloader, the capsfilter and webrtcbin. A second counter now
sits on the publishing bin's own src pad: `rtp packets handed to webrtcbin`, the
last point we own. GENERALISE: a counter upstream of the transport says what was
PRODUCED, never what was SENT.

**`sfuTrackPublished` HAD NO CONSUMER ANYWHERE IN THE TREE.** LiveKit answers
`AddTrackRequest` with the sid it assigned, `rust/src/sfu.rs` has emitted it since
the signalling round and `RustSdkMatrixClient` re-emitted it — and nothing
listened. So a track declared and never published was indistinguishable from one
carrying audio to everyone. It is logged now, with an 8 s warning when the
microphone track is never confirmed. Same family as `refreshIndexStats()` and
`fetch_details_for_event`: **grep for the CALLER, not the definition.**

**AND THE MUTE VALVE HAD NEVER BEEN LOGGED.** `drop=true` discards buffers before
the encoder, so a muted capture and a stalled one are the same silence in every
log this client writes — no level, no frames, no RTP, no error, while the SFU
still accepts the track and the transport still connects. One run produced
exactly that shape and it could not be told apart from a stall. Both say so now.

## 2026-09-16 — the room that loaded one message, and a bound that was wrong by exactly one

The 2026-09-15 round fixed a room that rendered *"No messages here yet"* over a
full history. This is the same defect one message later, and the fix it shipped
is what hid it: the reader's condition was encoded as `eventCount() == 0`, and
the maintainer's DM had a single image loaded above an otherwise blank
viewport.

> "In this room only a single image loads and I have to scroll up for anything
> else to appear."

### What the log already said, and what it did not

The instrumented run that the previous round's counters made possible:

```
timeline pagination requested reason= viewport_fill generation= 6
timeline pagination complete reached_start= false nextBatch= 20  filterOffered= 89  droppedRtc= 72
timeline pagination completed added= 0 signalled= 0 reached_start= false
... nine rounds, droppedRtc 72 -> 232, added= 0 every time, then it stops
```

Nine dispatches and then silence, with `reached_start` still false. Neither of
the two bounds that were raised for exactly this case can produce a nine:
`PaginationController`'s `m_maxFillRequests` and `kMaxNoProgressStrikes` are
both twelve. **Eight can**, and eight is `TimelinePane.qml`'s
`maxViewportFillRetries` — plus one dispatch from `requestNearTop()`'s early
redirect, which does not spend the pane's counter. The terminator was the QML
bound, and it was never in the frame of the previous round.

That deduction was then *measured*, not left as a reading: on the unfixed tree
`anEndlessFilteredRunStillStopsTheAutomaticFill` fails with
`fillStopped == false` after thirty seconds of unending filtered pages. The
controller never latches anything, because the pane stops asking at eight and
the controller's twelve are never reached.

### The root cause is one substitution, in three places

`maxViewportFillRetries` counts "pages that achieved nothing". A page the
timeline filter emptied looks exactly like one from QML — zero rows, zero
pixels — and is the opposite of one: the SDK walked twenty real events and the
pagination cursor moved twenty events closer to the first message beyond the
churn run. **Two indistinguishable observations, opposite meanings, one
counter.**

`PaginationController` had the same substitution in its two caps, where
`eventCount() == 0` stood in for "the reader has nothing to look at". Both are
proxies for the question the fill actually exists to answer — *is the viewport
full?* — and both are wrong by one message.

Three changes, and they are the same change:

* `PaginationController` counts pages that COMPLETED against the backend while
  inserting nothing and not reaching the start (`emptyFillPages`, monotonic per
  room so QML can compare it across attempts). Its strike bound applies to
  every such page rather than only to an empty timeline, and the constant is
  renamed `kMaxFilteredRunStrikes` — `kMaxEmptyTimelineStrikes` named the proxy
  that was the defect. `m_maxFillRequests` goes 12 -> 60 to match, and its own
  conditional raise is DELETED: the two numbers count the two halves of one
  event, and having the smaller one silently decide the allowance is how a
  bound ends up being set in a place nobody is reading.
* `TimelinePane.qml` gains the third kind of progress beside `grewHeight` and
  `grewRows`: a page that walked filtered history, spending
  `viewportFillEmptyPages` against `maxEmptyFillPages` (60, matching
  `kMaxFilteredRunStrikes`) instead of the eight meant for a dispatch that
  went nowhere. `maxViewportFillRetries` keeps its real subject — nothing
  happened at all — which is what it was written for.
* `MockMatrixClient` can express a fully filtered page at last
  (`setFilteredPaginationPagesForTest`). It could not before: an empty chunk
  override falls through to three synthetic events, so **every mock page had
  always added rows**, and the single most consequential pagination shape this
  project has had no reachable fixture at the QML layer. That is why the defect
  shipped twice.

60 is affordable where 12 was not, and for a reason the other two budgets
cannot claim: a page that inserts nothing instantiates no delegates, and a
fully filtered page skips the completion settle. The row cap
(`maxViewportFillRows`, 240) is untouched and still bounds everything the fill
puts on screen.

### Not gated on `lastPaginationFullyFiltered()`

The Rust backend can say outright that a page was offered events and the filter
ate all of them, and the completion-settle decision does read it. The bounds
deliberately do not. Its documented contract is "false is always the safe
answer" — safe when it costs a wait, not safe when it decides whether a bound
exists — and only one backend implements it, so gating on it would leave the
mock and HTTP backends holding the defect. `inserted == 0 && !hitStart` is the
condition that matters and every backend reports it.

### The first cut of that overruled a caller, and an existing test said so

`requestViewportFill()` originally kept its raise as
`qMax(m_maxFillRequests, kMaxFilteredRunStrikes)`. That silently overrules
`setMaxViewportFillRequests()` — a caller that deliberately NARROWS the budget
gets 60 anyway — and `fillBudgetBoundsConsecutiveUnproductiveFills` failed on
it immediately. The pre-existing code had the same `qMax`, and only escaped
because its branch needed an empty timeline that the test never had. Removing
the branch outright is both the fix and the simpler code: one cap, honoured as
configured.

### What was proven, and what was not

Fail-on-old was measured on the tree with the behaviour reverted and the new
API left in place, so the experiment isolates the decision from the plumbing.
All three new cases fail there:

* `aFilteredHistoryRunStillFillsTheViewport` — the user-visible one. One
  message, twenty filtered pages, real messages beyond them; asserts only that
  the viewport ends up full without anything scrolling it. Unfixed:
  `contentHeight` stuck under a 620 px viewport.
* `anEndlessFilteredRunStillStopsTheAutomaticFill` — the bound is larger, not
  absent.
* `aFilteredRunIsWalkedEvenWithAMessageAlreadyOnScreen` — the controller layer,
  with one row in a real `TimelineModel`.

`emptyBatchesStopAutomaticFillButNotUserRequests` moved 12 -> 60 and
`consecutiveEmptyViewportFillsStillStopTheLoop` read `< 30` against a bound of
12; both now derive it from the header instead of restating it, so neither can
go stale the next time the number moves.

**A live Matrix room was NOT exercised.** The verification laptop has no route
to the homeserver — its WireGuard default route is dead and the wifi it is on
carries LAN only — so no account could be signed in at all.

### GENERALISE

**A bound keyed on a proxy for the user's condition is wrong by exactly the
difference between them.** `eventCount() == 0` is not "the viewport is blank",
and one loaded message is the whole gap. When the honest criterion cannot be
read where the decision is made, say so and derive it, rather than picking the
nearest readable thing.

**And two observations that are identical at the point of measurement are not
one event.** A dispatch that went nowhere and a page that walked twenty
filtered events are both "no rows, no pixels" in QML. The counter that cannot
tell them apart will eventually be spent by the wrong one — which is the same
lesson as the previous round's `added= 0`, one layer up: there, a log line that
could not distinguish two causes; here, a *budget* that could not.

## 2026-09-16 (early morning) — 0.9.6 published, and three defects that had never run

### 0.9.6 is out: pipeline 222, tag `v0.9.6` -> `e177135`

24 of 25 green. The one red job is `report-optional-assets`, and it is a bug in
the REPORTER, not in the release — see below. The anonymous verification bar
passed in full, all eleven package links, and **macOS is attached**, which
makes 222 the first real publishing run to prove the loopback relay that
0.9.5's 413 forced.

### Three things that were committed, looked right, and had never executed

A theme, not a coincidence, and each one failed the first time it was asked to
work:

1. **`report-optional-assets` died on `RELEASE_TAG: unbound variable`.** It
   called `gitlab_api_init` and never `release_contract_env`, the function that
   sets that variable; every sibling script pairs the two. It runs only in a
   PUBLISHING pipeline and no release happened between `64a1f6d` (which added
   it) and 0.9.6, so nothing could have found out. It failed in the first
   release it was written to protect — and what it protects is the macOS lane,
   the one whose absence a green pipeline does not otherwise report.
   `test-pipeline-config.py` now sweeps every packaging script that READS
   `RELEASE_TAG` and requires the call. Keyed on `RELEASE_TAG` and not
   `PACKAGE_VERSION` deliberately: `write-build-info.sh` assigns the latter
   itself, so that key would report a correct script as broken.

2. **The verification bar had been under-reporting by one link on every run
   ever.** `verify-release.sh` wrote its list with `"\n".join(...)` and read it
   with `while read -r u`, which drops a final unterminated line — so the last
   package link was never fetched. That is why CLAUDE.md said "nine package
   links" for a 0.9.5 that had ten. Fixed at both ends, and the real fix is the
   second one: the script now ASSERTS that the number of links it checked
   equals the number the release reports, because a count that can be silently
   short is the defect. The script now also lives in the vault instead of only
   in a session scratchpad.

3. **Media messages had never produced a notification or an Activity row.**
   `rust/src/lib.rs`'s live-sync handler matched `Text | Notice | Emote` and
   `_ => return`, so an image, video, voice message or file sent to a room with
   NO timeline open was dropped before it could be a row of any kind. Separately
   `RustSdkMatrixClient` kept a PRIVATE copy of the msgtype mapping that knew
   only `notice` and `emote`, so even a media row that did arrive was typed
   `TextMessage`. Everything downstream had handled media correctly for
   versions; only the two mappings in front of them had not. One shared
   `rowTypeForMsgtype()` now, promoted out of an anonymous namespace.

### The review that sent the media fix back, and why the redesign was better

The first cut routed every media body into `media_filename`, on the theory that
a media body is only a filename. **That is false for `m.location`**, whose body
is the sender's own words: `location.rs` keeps it as the BODY on purpose, and
`EventPreview::oneLineSummary`, `NotificationManager` and `ActivityModel` all
have no Location case and read `body`. It would have blanked a room-list line,
a desktop toast and an Activity row at once.

Reading the live producer then showed the contract was never "body XOR
filename" but BOTH, with `filename.unwrap_or(body)` for File — Element puts the
CAPTION in the body per MSC2530, so reading the body alone renames the
attachment — and plain `body` for image/video/audio. The sync payload matches
that arm for arm now. **GENERALISE: when a second producer is added for a field
that already has one, derive its rule from the existing producer rather than
from a theory about the data.**

Both halves carry a fail-on-old case and neither can cover the other:
`SyncMessageRowTest` composes its own payload so it cannot see a Rust-side
routing change, and the Rust module cannot reach the C++ mapping. Reverting
`typeFromString` fails 2 of 8 C++ cases; making `media_filename_for_kind`
answer `body` for `"location"` fails exactly 1 of 6 Rust cases.

### The room-list backstop landed for 0.9.7, with its honest framing intact

`c01bfa8`. It does NOT fix the reported stale-ordering symptom, and the
measurement that said so still stands: a room whose newest event is call churn
has no ordering producer at all, and whether it should be ordered by churn is a
product decision nobody has taken. What it does fix is a second, real mechanism
found by reading matrix-sdk 0.18.0 — three ways `Room::latest_event()` stops
moving while messages keep arriving — plus a fallback branch that could never
run, because `Room::latest_event_timestamp()` in matrix-sdk-base 0.18.0 IS
`latest_event_value.timestamp()`, the value already passed in.

## 2026-09-16 (night) — the media-key rejoin defect, and a fix that measurement sent back

### A call rejoined after a crash could never receive media, since v0.8.0

Reported as "screenshare worked from me to Element, but I didn't get anything
back". Every frame dropped for want of a key — audio and screen-share video —
and it never recovered.

`publish_membership` called `read_own_created_ts()` on EVERY publish, including
a fresh JOIN. On a homeserver without MSC4140 an unclean exit leaves the
membership until `expires`, so a rejoin inside that window found the ghost and
wrote **the previous session's `created_ts`** into the new event.
matrix-js-sdk's `RTCEncryptionManager` keys "who already holds my key" on
`(userId, deviceId, membershipTs)` where `membershipTs` is
`content.created_ts` — unchanged triple, so not a new joiner, so no key is
sent. **And Lightning never asks:** MatrixRTC has no key-request verb and
matrix-js-sdk only pushes, so the state is unrecoverable for the life of the
call.

LIVE before/after against Element Web, three cases: fresh join PASS, clean
rejoin PASS, `kill -9` rejoin **FAIL before** (zero keys, 500 dropped frames)
and **PASS after** (both media types, `dropped= 0`). Structural proof from room
state: the post-kill event carried the killed session's `created_ts` where the
pre-kill event had none.

**GENERALISE: a value inherited "to preserve continuity" must be scoped to the
thing whose continuity it represents.** `created_ts` represents THIS session's
join; reading it off the room meant reading a dead session's.

**The review found the fix surviving an account switch, which is the serious
direction** — resetting `created_ts` reorders oldest-membership focus selection
for everyone. `retract_membership` was the only clearer and four fallible
lookups (client, room, user id, device id) sat ahead of it, all of which
sign-out has already torn down; a session restore keeps the same device id, so
the stale mark still matched. Now forgotten by room prefix before anything
fallible runs, AND cleared wholesale in `shutdown_managed_tasks`, which every
teardown path runs. **GENERALISE: a process-global that decides what goes on
the wire must be cleared where the invariant ends, not where the tidy path
happens to pass.**

Two more from the same round, both measured rather than argued. The
`delayed_reason= "network"` a call log printed was asserting a transport
problem on a server that had published `msc4140: false`: the real answer is
`400 M_UNKNOWN` / `M_MAX_DELAY_UNSUPPORTED`, which matched no branch of
`classify_room_error` and fell into the catch-all. Now `delayed_unsupported`,
corroborated by `/versions` only AFTER an arm has failed, never latching on a
transient refusal and never on not knowing — and publishes dropped from two
state events per refresh to one. And **`sfu joined others=` counts SELF**,
which sent the first triage after a phantom third participant; relabelled.

### The GStreamer wall was never ours

56 `GstIntRange` CRITICALs before the first sync, again on every call join and
share. `gst-device-monitor-1.0` — a stock tool with no Lightning code in the
process — prints the same 28 pairs on the same machine. **That also refutes
what `docs/open-items.md` recorded as the suspect**, so nobody should hunt it
in `src/calls/` again. Collapsed, not suppressed: the first occurrence always
prints with an explanation of whose problem it is. Verified live on the
maintainer's own desktop — one line where there had been 56.

### `--log-file` created no file, on every platform

`Lightning --call-media-status --log-file out.txt` exited 0 and wrote nothing —
the one command a tester is told to run. `preflightParse()` is a single
left-to-right walk in which every terminating flag ends in `return r`, so a
`--log-file` standing AFTER one was never read. The status commands also PRINT
rather than log, so even in the right order the file held only its own header.
And on Windows `freopen("CONOUT$")` ran unconditionally, destroying an
inherited shell redirect. Confirmed fixed on real packaged Windows builds,
before and after.

### And the fix that measurement sent back

A round traced the stale room-list report to a `Text | Notice | Emote` filter
and built a sliding-lane recency harvest for it. Reviewed, mutation-tested,
monotonic. Then the desktop sweep measured it:

- **The old code did not reproduce the defect.** With the harvest disabled and
  the pre-fix fallback restored, an `m.image` into a closed room still moved
  that room to the top. The filter is real but feeds the OPEN room's timeline;
  the room list's stamp comes from `room_payload`, a separate producer that
  handles images fine.
- **The reported symptom is still present WITH the fix.** A room seeded with 3
  messages then 30 `m.call.member` events showed no time, no preview and
  bottom-of-list position, across a restart, until opened.

So it was held out of 0.9.6. **GENERALISE: a fix is not finished when it is
correct and reviewed; it is finished when something shows the defect happening
without it.** The real cause is a room whose newest event is churn having no
ordering producer at all — and stamping from churn would make an idle call
outrank a live conversation, which is a product decision rather than an
implementation one.

### Validation

Rust **430 passed, 0 failed, 5 ignored, 435 total**; `build-rust` CTest
**208/208**; non-Rust **204/204**; `WEBRTC=OFF` over every target rc=0. Six
independent review passes across the night's three workstreams, every finding
closed. Two tests of mine were decoration on the first attempt and were caught
by mutation, not by reading.

## 2026-09-15 (night) — "waiting for keys" fixed, and a review that caught me claiming the opposite

### The fix

Lightning had exactly ONE automatic route from a key in your backup to a
message decrypting, and it ran at most once per room per session. Two of the
three mechanisms CLAUDE.md §9 described do not exist:
`automatic-room-key-forwarding` is not a requested feature, so no
`m.room_key_request` has ever been sent in any version, and
`BackupDownloadStrategy::OneShot` installs neither the UTD handler nor the
`BackupDownloadTask`. What was left — `download_backup_keys_for_room` — was
deduplicated permanently, and `mx_rust_recover_from_backup` was the ONLY caller
of `clear_backup_attempt` in the tree. That is precisely why typing the
recovery passphrase cured it and waiting never did.

`recover_keys_for_utds` closes it, reusing the machinery the manual Retry
button already uses rather than adding SDK surface: a bounded per-session
`download_room_key`, then `retry_decryption` for exactly those sessions. It
fires on a timeline's initial snapshot and on every diff, for the ROOM and the
THREAD timelines, each scoped to its own generation (`RecoveryScope`) because
answering to the wrong one is how a late callback mutates the next timeline.

`mark_backup_attempt` stopped being a permanent `HashSet`. Each key records its
attempt count and time; a first try is always allowed, the wait then doubles
from 30 s to a 32 min ceiling, and after eight attempts — a little over an hour
— the key is left alone for the lifecycle. The first cut of that said five
attempts and passed the count straight into the backoff, which started the
schedule at 60 s and made the documented ceiling unreachable; a review measured
it, and `the_backup_retry_schedule_is_the_one_documented` now pins the series
so the comment and the code cannot drift apart again. **Nothing polls** — an attempt happens only when an
undecryptable row is actually in front of the user.

**`OneShot` STAYS, and re-proposing `AfterDecryptionFailure` needs to answer
this:** it was tried in v0.7 and reverted because it fetches one key per
freshly-failing event and left already-rendered history encrypted after
verification, while OneShot bulk-downloads everything when a session is
verified. The gap was never the strategy; it was that nothing re-ran after a
room's single pass.

### The review found a regression I had introduced, and my own prose asserted it was impossible

The instrumentation commit added `skipped_*` states on the `backup_download`
event kind and claimed — in the commit message, a source comment and
`docs/open-items.md` — that behaviour was "unchanged by construction, the model
compares that field only against `started` and `failed`".

**The comparisons are inert. The assignment is not.**
`CryptoBootstrapModel::applyEvent` assigns `m_download` unconditionally and does
not return, and `recompute()` reads anything that is not `"failed"` as
**Ready**. So a room that ran NO pass overwrote what a room that FAILED one had
recorded: fail a pass in room A (banner escalates, offers the recovery key),
switch to any already-attempted room, and the banner vanishes while history
stays unrestored. Every room switch after the first, since the pass is spawned
on every room open. §6: never report a cleanup as successful when it removed
nothing.

All 207 tests passed on it, because no test had ever fed a skip into that
model. Fixed on both sides — skips carry their own kind, and the
`backup_download` branch refuses a `skipped_` state regardless.

**GENERALISE: proving that nothing COMPARES a field is not proving that
nothing ASSIGNS it.** The commit message of `786e2ed` is pushed and immutable
and still carries the wrong claim; this entry is the correction.

### And the first regression test for it was decoration

Mutating the fix left the test passing. The dedicated `backup_download_skipped`
branch RETURNS before `recompute()`, so the assignment being mutated was inert
— the defect's real path was a skip flowing through `backup_download`, which
falls through. The test now sends that, and the mutation fails with
`Actual: Ready` against `Expected: ManualRecoveryRequired`. §18's rule earned
its place again: a regression test that does not fail on the old code is
decoration, and the only way to know is to try it.

### Pass 2 found the fix reproducing its own defect under a network outage

`download_room_key` reports "this key is not in your backup" and "we could not
reach your backup" IDENTICALLY — verified against the pinned SDK:
`Ok(false)` happens only when the store holds no decryption key or no backup
version, which `are_enabled()` has already excluded, so a 404, a 429 and a
dropped connection all arrive as `Err`. The first cut spent an attempt BEFORE
the request and reported every failure as `no_keys_found`. So a transient
outage while someone was reading an encrypted room burned the whole budget for
those sessions, logged a line that read as "nothing more to do", and left the
recovery passphrase as the only remedy — **the exact symptom this work
removes**, recreated by the work itself. It is also this round's own §16 lesson
in a second costume: a log line that cannot tell "nothing happened" from "we
threw everything away" is not a log line.

Now the error is classified. `not_found` is a definitive answer and spends an
attempt; anything else keeps `last` (so the backoff still throttles) and hands
the attempt back through `refund_backup_attempt`, and the pass emits `failed`
rather than `no_keys_found` so the two stay distinguishable in a capture.

Four more from the same pass, all fixed: the two early enqueues were not
lifecycle-guarded and the C++ bridge drops the lifecycle field entirely, so
Rust was the only gate and it was missing; `skipped_no_backup_key` was emitted
per diff with nothing able to throttle it (the gate returns before the backoff
is consulted) and is now latched once per lifecycle; `clear_backup_attempt`
removed only the bare room key, so the passphrase did not clear the
per-session entries it appeared to clear; and a pass now takes at most 32
sessions, because a `Reset` diff hands over the whole timeline.

### Pass 3: the H2 fix had two defects of its own, both proven by running it

**H3 — the refund pinned the counter, so the backoff could never leave its
floor.** `mark` took `attempts` 1 -> 2 and the refund put it back to 1, so the
backoff argument (`attempts - 1`) was always 0 and the wait was always 30 s,
for ever, with the budget never depleting. The reviewer measured it: twelve of
twelve cycles allowed. So the fix for "one outage exhausts the budget" had
created "one outage retries at the floor for ever" — against precisely the 429
that was asking us to slow down.

**Two counters now, and the refund is gone.** `tries` counts every attempt and
is never given back, so a sustained outage escalates 30 s -> 1 -> 2 -> ... ->
32 min as designed; `attempts` is the budget and only a DEFINITIVE answer
spends it. A permanent refusal spends the whole budget at once.

**H4 — the definitive/inconclusive split was a denylist, and the string
classifier could mis-file a transport error as definitive.** `category !=
"not_found"` made `unrecognized` (the server has no such endpoint) and
`forbidden` (we are not allowed) refundable and therefore retried for ever —
while `rtc.rs`'s `delayed_refusal_is_permanent` latches on exactly those, so
one half of this same round drew the distinction correctly and the other
inverted it. And `classify_room_error` matches SUBSTRINGS of the error's
Display, which for a transport failure carries the request URL — and that URL
ends in a 43-character base64 session id, so a dropped connection whose session
id happened to contain `404` was filed as definitive and spent a budget it had
not earned (~1.6e-4 per session: small, not zero, silent).

`classify_backup_error` now reads the STRUCTURED error
(`client_api_error_kind()`): `NotFound` is definitive, `Unrecognized` and
`Forbidden` are permanent refusals that stop the key outright, and everything
else including `None` (a transport error has no errcode at all) is
inconclusive. **GENERALISE: a user-facing error CATEGORY is the wrong input to
a control-flow decision about spending a budget — and a denylist there fails
open on every category nobody thought of.**

**AND MY REGRESSION TEST FOR THIS WAS DECORATION TWICE.** The first version
mutated clean because the branch it probed returned before `recompute()`. The
second computed the expected wait with `backup_attempt_backoff` and compared
the series — which tests the backoff function and says nothing about whether
the POLICY consults it, so mutating the policy to ignore `tries` left it
passing. The third asks only the policy: hold elapsed time at 45 s and walk
`tries` up, because 45 s satisfies the 30 s wait after one try and must not
satisfy the 60 s wait after two. Both halves now fail under mutation.
**The policy is a pure function (`backup_attempt_allowed`) for this reason** —
both defects were policy defects that no test could reach while the policy
lived inside a `Mutex` and an `Instant`.

Also from that pass: the `skipped_no_backup_key` "latch" was a backoff all
along and emits about eight times an hour, not once — the third comment this
round to describe a schedule the constants do not produce; the emit now carries
an `inconclusive` count, because a pass that downloaded 1 of 32 and was
rate-limited on 31 reported `ok count=1` and lost the distinction at the
session level.

### Pass 4: `are_enabled()` and `download_room_key` read DIFFERENT keys

The sharpest finding of the round, and it was hiding behind a comment of mine
that reasoned from a false premise. The `Ok(false)` arm was charged as a
definitive answer because "`are_enabled()` already excluded it". It does not:

- `are_enabled()` → `BackupMachine::enabled()` reads
  `backup_key: Arc<RwLock<Option<MegolmV1BackupKey>>>` — the **public upload
  key** (`matrix-sdk-crypto-0.18.0/src/backups/mod.rs:146`).
- `download_room_key` needs `BackupKeys::decryption_key` — the **private
  download key** (`store/types.rs:421`).

A verified device that uploads to a backup but has never had the recovery
passphrase entered on it has the first and not the second. `are_enabled()`
returns **true**, `download_room_key` returns **`Ok(false)` without sending a
request at all** — and that is not a race, it is the steady state of precisely
the person who reports "waiting for keys". It is ranked cause (2) in
`docs/open-items.md`.

So every pass returned `Ok(false)` for every session and charged it definitive:
eight passes and the whole room was exhausted for the lifetime, cured only by
the passphrase. **H2's symptom, third route, aimed at the exact population the
feature exists to serve** — and the emit called it `no_keys_found` ("your keys
are not in the backup") when the truth was "this device cannot read the
backup". The one state a capture most needs named was the one it hid.

`BackupOutcome::NoDecryptionKey` now spends nothing and carries its own emitted
state, ranked above `failed`, because it is the only outcome in this feature
with a remedy attached: enter your recovery key.

**GENERALISE: two SDK calls whose names both say "backup" can read two
different keys, and a guard only excludes what it actually reads.** Three of
this round's five findings came from a premise stated in a comment and never
checked against the source it described.

Also that pass: `PermanentRefusal` no longer parks a key at
`MAX_BACKUP_ATTEMPTS` (indistinguishable from a spent budget, and raising the
cap — which already went 5 → 8 this round — would have silently un-parked
every refused key); it uses `BACKUP_ATTEMPTS_STOPPED` instead. The
`skipped_no_backup_key` comment was wrong a second time, in the other
direction: nothing records an outcome for its key, so its budget is never
spent and only the backoff applies. That comment has now been wrong twice and
says so.

**And the wiring got its own test.** The policy was pure and well covered; the
call INTO it was not, and a review measured that swapping its two arguments
reproduces H3 exactly while every policy test still passes.
`mark_backup_attempt_at` takes the instant as a parameter so a test can ask
"and much later?" without sleeping, and
`what_an_attempt_established_decides_whether_it_cost_anything` drives the
registry itself. Mutation-checked twice: the argument swap fails it, and so
does making `NoDecryptionKey` spend the budget.

### The review's other findings, all fixed

- `"Empty on success"` on the new FFI signal was wrong: `rtc.rs` also leaves
  `delayed_category` empty when no arm was ATTEMPTED, which is the steady state
  once the permanent refusal latches — the very case the field exists to
  describe. The value is sticky, and the comment now says so.
- `m_delayedCategory` outlived its call, so a support log could attribute one
  room's refusal to the next room's call. Cleared with `m_delayId` at both
  reset sites.
- `test-metainfo-consistency.py`'s pre-tag fallback asked the FILESYSTEM. An
  untracked screenshot passes `is_file()` and no tag can contain it — and since
  §4 forbids `git add .`, that is a live near-miss of the defect the test was
  written for. It asks git now.
- Nothing checked that a screenshot ref names the CURRENT version, so a bumped
  release with unbumped refs would render the previous release's screenshots
  and pass. Asserted now.

The reviewer also re-traced the loosened device-id sanitiser adversarially and
found a defence the fix had not cited: `resolveActive()` returns a stored id
only when it matches a device `QMediaDevices` currently enumerates, which
closes the hand-edited-config vector on its own.

### Validation

Rust **423 passed, 0 failed, 5 ignored, 428 total**; `build-rust` CTest
**207/207**. **LIVE: NOT TESTED** — §9 and §12 require a real multi-device test
against a live backup before any of this may be called PASS, and the automated
coverage proves mechanics only.

## 2026-09-15 (evening) — the laptop rig, Flathub measured at last, and a field the bridge dropped

Two guests running at once on the laptop (10.195.174.169) with an agent on
each, plus a read-only audit of the standing "waiting for keys" report.

### Flathub: the maintainer was right, and the earlier audit was stale

A previous session reported that the Flathub manifest had never been built or
linted. The maintainer contradicted it from memory. **He was right.** The rig
is a `flathub-rig` container on the laptop and it holds a complete
`flatpak-builder --sandbox --repo=repo --install` run from 2026-09-11 (a
382,580-byte log), its driver script, the 23 MB bundle it produced, and the
app still installed. Bash history has no trace because the work ran over
non-interactive SSH — **a history grep returning nothing is not evidence of
absence**, and relaying a subagent's negative without checking it against the
maintainer's own records is how the wrong claim got written down twice.

Measured this round, on the rig, against `f7c6c3c`:

| gate | result |
|---|---|
| build, offline, KDE 6.11 runtime | **PASS** — cargo fully offline from `cargo-sources.json`, Rust from the SDK extension (1.98.1), `call media engine built in: yes`, `secret_store: libsecret` |
| `flatpak-builder-lint manifest` | **PASS**, no output |
| `flatpak-builder-lint builddir` / `repo` | **FAIL, 2 errors** — `metainfo-missing-screenshots`, `appstream-failed-validation` |
| `appstreamcli validate` (networked) | **FAIL** — 4 x `screenshot-image-not-found`, exit 3 |
| GUI in the Ubuntu guest | **PASS** — installs, launches, `Lightning 0.9.4`, both call engines, login screen read by OCR off the pixels |

Flathub documents both lint errors as ones whose *"exception is never
granted"*, so they are hard blocks.

### The cause is bigger than Flathub: 0.9.5 shipped four dead screenshot URLs

`73c87ef` added `docs/screenshots/flathub/` **and** the metainfo pointing at
them — with the URLs pinned to tag `v0.9.4`, where the files do not exist. It
shipped inside v0.9.5. Verified here offline: `git ls-tree v0.9.4 --
docs/screenshots/flathub/` is empty and `v0.9.5` lists all four.

So this is not a Flathub-only defect. **Every published 0.9.5 package — deb,
rpm, AppImage, snap — carries a metainfo whose four screenshot URLs 404**, and
GNOME Software and KDE Discover show no screenshots for those installs.

**Nothing could see it**, and the reason generalises: all three package
validators run `appstreamcli validate --no-net`
(`packaging-ci/scripts/validate-deb.sh:60`, `validate-rpm.sh:89`,
`packaging/rpm/lightning.spec:82`), and the network check is the only part of
appstreamcli that can see a dead URL.

Keeping `--no-net` is right — a package build should not fail because GitHub
is slow. So the new `packaging-ci/tests/test-metainfo-consistency.py` answers
the same question **offline**, with `git ls-tree`: a URL naming one of this
repository's own tags is checkable with no network at all, and it fails on the
commit that introduces the mistake instead of after a release is published. It
also calls `update-metainfo-release.sh`, which was written to catch the second
half of this, is correct, and had **no caller anywhere** — the recorded "a test
file can be committed and never registered", in its packaging costume. That
second half was live too: the newest `<release>` read 0.9.4 in a 0.9.5 tree,
so every listing showed the previous release's version and changelog.

FAIL-ON-OLD, run: reverting both defects fails **six** of its checks.

Fixed with it: the four URLs repointed to `v0.9.5`, a `0.9.5` release entry
added, and the Flathub manifest repinned from `v0.9.4`/`bcea599` to
`v0.9.5`/`8d5d0ca`. That last one matters beyond the lint — **v0.9.4 has
neither `CameraPortal` nor the `FLATPAK_ID`/`setDesktopFileName` fix**, so a
submission from the old pin would have shipped the very camera blocker the
Flatpak work existed to remove. `cargo-sources.json` needs no regeneration:
`git diff v0.9.5 HEAD -- rust/Cargo.lock` is empty.

Still open, and the maintainer's to do: the verified-app token
(`https://www.lightning-matrix.org/.well-known/org.flathub.VerifiedApps.txt`
is 404), and the submission PR itself — **Flathub's published requirements
forbid an AI agent opening or automating a submission PR or writing its commit
messages, descriptions or replies.** That one is his to write, by their rule.

### "Waiting for keys": the mechanism, established from the code

See `docs/open-items.md` for the full entry. The headline is that **two of the
three mechanisms CLAUDE.md §9's diagram named do not exist**:
`automatic-room-key-forwarding` is not a requested feature (so Lightning has
never sent an `m.room_key_request` on a decryption failure, in any version)
and `BackupDownloadStrategy::OneShot` installs neither the UTD handler nor the
`BackupDownloadTask`. The one automatic route left runs **at most once per
room per session**, and `mx_rust_recover_from_backup` is the only caller of
`clear_backup_attempt` in the tree — which is exactly why typing the
passphrase cures it and nothing else does. §9 and
`docs/feature-contracts.md` both claimed the absent mechanisms and are
corrected. NOT a 0.9.5 regression: the structure has been unchanged since
v0.6.3.

`download_backup_keys_for_room`'s three silent early returns now emit
`skipped_no_backup_key` / `skipped_already_attempted` / `skipped_bad_room_id`.
Behaviour is unchanged by construction — `CryptoBootstrapModel` compares that
field only against `started` and `failed` — which is also why the banner could
read **Ready** while rows sat on "Waiting for keys…".

### A field the Rust lane computes and the bridge dropped

`rtc_membership_published` carries six fields and
`RustSdkMatrixClient::handleRustEvent` read five: `delayed_category` was
computed in `rust/src/rtc.rs`, enqueued, and thrown away at the FFI. The cost
was diagnostic — every `delayed= false` in a call log was mute about whether
the homeserver has no MSC4140 endpoint (permanent, nothing to retry) or
refused this one write (transient, the next publish retries). Opposite
remedies, and issue #10's reporter had no way to tell them apart.

**Nothing could see it, for a reason worth keeping:** `SfuCallController`'s
tests drive a fake client that emits the signal *itself*, so they prove what
the controller does with a field and say nothing about whether the bridge ever
supplies one. `tests/RtcBridgePayloadTest.cpp` drives the real dispatcher with
the real payload through `handleRustEventForTest`. FAIL-ON-OLD, run: with the
field dropped again the bridge test reads an empty string, and with the
controller's record removed the controller test fails — both confirmed by
mutation, each caught by a different suite.

### The Windows guest: a preference that could never be stored

Choosing the webcam in Settings showed it selected and read back "System
default" on the next visit, on both builds. `sanitizedDeviceId()` in
`SettingsManager.cpp` refused any id containing a backslash — and a Windows
`QMediaDevices` id is a device path that BEGINS with two
(`\\?\usb#vid_322e&pid_233a&mi_00#…`). Every Windows camera and microphone
preference therefore stored as the empty string, and the empty string means
"system default". **The rule came from a GStreamer pipeline concern and its
own comment named PipeWire**, so it could only ever fire on the platform it
was not written for, and it fired on every id there.

Nothing interpolates these ids on Windows, which is what makes widening the
rule safe rather than a trade: `platformDeviceElement()` — the one site that
builds a `device="…"` description — returns early under
`Q_OS_WIN || Q_OS_MACOS` and carries its own refusal for `"`, `\` and `!` at
the point of use; the SFU engine never interpolates at all, parsing
`<element> name=micsrc` and setting the property on the parsed element
afterwards. The storage rule keeps its length bound and control-character
refusal, because the config file is hand-editable. FAIL-ON-OLD, run: with the
backslash refusal restored the stored id reads `""`.

**GENERALISE: a sanitiser that encodes a rule from the point of USE, applied
at the point of STORAGE, is a rule applied where it cannot know whether it is
needed** — and it will be wrong on the platform its author was not using. The
guard that matters was already in the right place; this one was a copy of it
that outlived its reason.

### What else the two guests established

Full detail in `docs/open-items.md`. The results worth naming here:

- **The Windows camera frame-rate numbers are unreadable, for a physical
  reason.** Measured 29.79 fps on one build and an exactly steady 10.00 fps on
  three later runs — and Windows Settings then reported the camera *"blocked
  or turned off by a switch"*. **The laptop's privacy shutter is closed**, and
  a rock-steady 10.00 fps is what a UVC sensor does in the dark. Neither
  number says anything about the 10 fps ceiling. The camera work itself is
  re-confirmed on the RELEASED 0.9.5: `camera chain= mjpg`, `image/jpeg
  1920x1080 30/1`.
- **The tray balloon's read-withdrawal is confirmed broken on Windows**, no
  longer merely predicted from the code: the toast survived the read receipt,
  a restart and a full in-app update. Display and click routing PASS again.
- **The Windows PORTABLE update path is LIVE-VALIDATED PASS**, 0.9.4 to the
  real 0.9.5, session and read state intact. MSI and installer remain NOT
  TESTED.
- **`--version` and `--call-media-status` print nothing on the packaged
  Windows build**, and `--log-file` writes no file there — so the
  shipped-artifact self-checks §16 and §10 rely on cannot be read from a
  Windows package today. Not fixed; a GUI-subsystem binary with no attached
  console explains the stdout half and not the missing file.

### Validation

Rust **423 passed, 0 failed, 5 ignored, 428 total**. `build-rust` CTest
**207/207 passed, 0 failed**. Non-Rust build rc=0. `pagination-controller` had
failed once in an earlier parallel run; it did **not** reproduce in four
attempts (three under 16-way load, one in the same parallel shape), so it is
recorded as an unreproduced flake and NOT as a cause.

## 2026-09-15 (afternoon) — the room that said it was empty, and three more user reports

Five agents audited message loading in parallel (three on Lightning, one on
how other matrix-rust-sdk clients do it, one on GitHub issue #10). Three
converged independently on the same mechanism, and the maintainer's own
instrumented run then settled it outright.

### `added= 0` was undiagnosable, and that was the first defect

The pagination log emitted one bit where it needed two numbers. "The room is
quiet" and "we fetched twenty events and discarded every one" were the same
line, and the count is not recoverable downstream: `paginate_backwards` returns
a bare `bool`, and matrix-sdk-ui throws `BackPaginationOutcome.events` away on
the line that tests it (`pagination.rs`). Lightning's own timeline filter is
the one place that sees every raw event AND knows why it said no, so it counts
now — cumulative and process-global, because the timeline ingests
asynchronously after the call returns and a per-batch delta would race it.

The maintainer's next run answered the question in one line:

```
filterOffered= 240  droppedSdk= 0  droppedRtc= 240
```

Twelve consecutive pages, **100% MatrixRTC membership churn** — about 300
`m.call.member` events between the room and its last real message.

**GENERALISE: a log line that cannot distinguish "nothing happened" from "we
threw everything away" is not a log line.** It took a maintainer report to find
this, and only because a second room in the same session opened instantly.

### Then four defects in how that was handled — none of them in the fetching

**A room asserted its own emptiness after ONE empty page.**
`m_initialHistoryHasSucceeded = true` had no `inserted > 0` test, so the first
fill settled the initial history, the pane's presentation gate opened, and
`timelineEmptyState` rendered *"No messages here yet. Start the
conversation."* over a room full of history. That is the report's own sentence.

**Fixing that made a latent redirect bite, and an existing test caught it.**
`requestNearTop()` hands an early gesture to the fill while the initial history
has not succeeded — and once the fill has STOPPED that swallows the gesture:
the fill refuses it and no near-top page is dispatched. It had been unreachable
only because an empty page used to set the flag.
`emptyBatchesStopAutomaticFillButNotUserRequests` went 13 → 12 and named it.

**The fill gave up at 8 pages, not the 12 that was deliberately configured.**
A filtered page adds no rows, so it spends `maxViewportFillRetries` (8) and
never touches `maxInvisibleFillRetries` (12) — the counter the 2026-09-05
round raised *for exactly this case*. The raise went into the counter that
filtered pages never reach, and nothing noticed because both numbers look
right in isolation.

**Every empty page paid a 250 ms settle for rows that could not arrive.**
Twelve of them is three seconds of the reported ten.

### Two things the cross-client agent found that are worth keeping

**The sliding-sync room list runs at `DEFAULT_LIST_TIMELINE_LIMIT = 1`**, and
any `limited` response with a prev_batch makes matrix-sdk shrink that room's
in-memory cache to its last chunk (`state.rs`). So a room opens with one or two
cached events — and `items= 0` when the filter drops them, against `items= 2`
for a healthy room (one event plus its date divider). Both appear side by side
in the maintainer's log. `subscribe_to_rooms` is what raises the room to 20,
and Lightning applied it AFTER building the timeline; it goes first now.

**And a page-size escalation is nearly inert, for a reason worth recording.**
It was added after a fully filtered page and looked like it was working —
`nextBatch= 180` — while `filterOffered` kept climbing by exactly 20. The cause
is that `load_more_events_backwards` returns ONE STORED CHUNK at a time and
never consults `batch_size`; that parameter only matters when the walk reaches
a network gap. Those pages were local disk reads, not round trips, which also
means the 250 ms settle was the dominant cost rather than the fetch. The
escalation is kept because it is the right ask at a real gap, but **it is not
the fix and must not be recorded as one.**

It is also NOT the page doubling §16 refutes: that refutation measured
unconditional 100-event pages against rooms whose pages ADD ROWS, and both
harms it found — a ~600-row overshoot and per-row ingest — need rows.

### Three other reports from the same day

**A reply quote could never resolve its own target.** `InReplyToDetails::event`
is a field on the REPLYING event, starts `Unavailable`, and matrix-sdk fills it
only when asked via `Timeline::fetch_details_for_event` — which
`grep -rn fetch_details_for_event rust/` showed had **never been called in this
repository**. So a quote was populated only when the homeserver happened to
bundle the replied-to event, and read "(original message not loaded)" for ever
otherwise. Reported as a quote failing on a message three rows above, on
screen; being on screen was never relevant.

**Forwarding was limited to the active Space.** Both pickers used
`app.roomList`, which is bound to SpaceManager. The reporter's own observation
named the mechanism precisely — *"if you arnt in any spaces you can forward
wherever you want"* — because an empty active Space disables the filter.

**GitHub issue #12 is confirmed, not hypothesised.** The reporter's log stops
at `publishTracks camera= false`, and `publishTracks` calls `publishAudio()` on
the next line, which is exactly where `monitorCandidates("Audio/Source")`
blocks the GUI thread. The 2026-09-14 fix addresses it both ways.

### And one that is fixed only in part, stated as such

`CallDeviceController`'s constructor is deliberately empty because touching
QMediaDevices initialises Qt Multimedia, which on a PipeWire desktop prints a
SPA parse error per device (`spaVisitChoice` — confirmed by walking the
binary's shared libraries to `libQt6Multimedia.so.6`, so the messages are Qt's
own parser and what is ours is waking it). `enableCallMediaEngine()` then
called `applyDevices()` unconditionally one line later, defeating it on every
launch. Now gated on a stored device preference.

**It did not stop the noise.** With no preference stored the gate skipped the
call and Qt Multimedia still came up before `voice-call media engine active`.
`runtimeAvailable()` is pure GStreamer probing and `setMediaBackend` touches no
devices, so the remaining trigger is unlocated. `applySfuDevices()` a few lines
below has the identical shape and is the first place to look.

## 2026-09-15 — the row's right rail had three things on it, and a live two-account GUI audit of all three layouts

A user reported the hover action bar buried under read-receipt avatars ("this
is a bit messy and hard to click on stuff", "might be because of my scaling").
It is not the scaling. Reproduced here in minutes on a real two-account rig,
and auditing the layouts for it turned up three more of the same shape.

### The rail

The hover action bar is `anchors.top/right` on `bubbleRow`, 3px above its top
edge. The read-receipt facepile paints UPWARD from `layout.y + layout.height`
at the row's own right margin. On a TALL row they are nowhere near each other,
which is why this went unnoticed for so long. On a SHORT one — a single line,
a continuation row, a compact timeline — the row's top and bottom are barely
30px apart and the two land on the same pixels. **The facepile wins**: both
carry `z: 3` and the strip is later in the document, so the buttons underneath
are not merely ugly, they cannot be pressed.

Measured on the unfixed tree by the new test: the bar ends at x=628 and the
pile starts at x=580. Forty-eight pixels of the bar — Edit and the overflow
menu — sit under the avatars.

`actionBarReceiptReserve` reserves the pile's width on the bar, and ONLY while
the two bands actually meet, so the bar keeps the row's corner everywhere it
can. It feeds one `rightMargin` on a Loader and nothing else, which is why
(unlike `receiptRailReserve` beside it) it can apply in Bubbles too.

### Three more, all found by auditing the layouts against a real account

**Bubbles: the sender header rendered OUTSIDE its bubble.** The header's cap
was `Math.max(1, bubble.width - 112)` — and in Bubbles the bubble is SIZED
FROM the column the header is in. That is a loop, and Qt resolves it with
whatever the bubble measured last: for a short body that is under 112, so
`Math.max(1, …)` pinned the header's contributed width to **one pixel**. The
bubble then sized itself to the body alone and the name and timestamp painted
on the timeline background beside it. The new test measures exactly that:
`the identity header measured 1px wide`.

The escape is the one `segmentCap` already uses a few hundred lines down:
derive from `bubbleRow`, which is fillWidth in `layout` and reports no
implicit width, so that end of the chain is inert.

**Bubbles: the facepile clipped an own bubble's corner.** The width cap
subtracts 40 for a rail and the PLACEMENT ignored it, right-aligning to
`parent.width`, so a short own bubble was pushed flush to the row edge where
the avatars are. `bubbleReceiptInset` insets the placement and narrows a wide
incoming bubble by the same amount. `receiptRow.width` is a chip count and
depends on nothing below the bubble, so this closes no loop.

**Local search said "Nothing is indexed yet." over three results it had just
returned.** `MessageSearchController::refreshIndexStats()` is a `Q_INVOKABLE`
with **no caller anywhere in the tree** — `grep -rn refreshIndexStats qml/ src/`
returns its declaration and its definition and nothing else. So
`indexedMessages` only ever moved when the five-minute sweep happened to fire
or the user pressed "Index this room"; on a freshly started client it was 0.
Same family as the unregistered test file and the gated counter in §16: code
that exists, looks right, and is never reached. The History scope asks for the
stats now, and the label refuses to claim an empty index while results are on
screen — a count of zero is not evidence when the number arrives on its own
signal, later.

### What the audit actually covered

Two instances, two throwaway accounts, one KDE/Wayland desktop, driven by
`scripts/gui-harness.sh`. Modern, Compact and Bubbles each rendered against a
real room with mixed own/other messages, short and wrapping bodies, group
headers, and live read receipts; the action bar was pinned on a receipt row in
each. Modern and Compact were otherwise clean. Bubbles carried both defects
above.

**AND THE OFFLINE RESTORE WAS LIVE-VALIDATED IN THE SAME RIG — PASS.** With
`HTTPS_PROXY` pointed at a closed port so every outbound request is refused at
once, the client goes `screen change 0 -> 3 -> 1` (Boot to Main), **not to the
login page**, logs `session restored from the local store — the homeserver
could not be reached`, shows the complete cached room list, opens an ENCRYPTED
DM and renders its decrypted history, and the footer reads
**"Matrix Rust SDK • Offline — retrying"** from the first frame. That last part
is the `setState` override from the review round working live: before it, the
same session read "Loading rooms…" over a list that was already complete.

Local search works in that state too, which was the specific ask: the find
bar's History scope answered `Searching 12 messages Lightning has indexed,
including encrypted ones.` with three hits from the encrypted room, with no
server at all.

### Harness notes worth keeping

**Tab, not coordinates, for a login form.** Three clicks aimed off a capture
all landed in the FIRST field, and because step two opened with Ctrl+A the
password was typed into the *homeserver* box in plain sight. Tab order is
Homeserver → User → Password and is exact. The password went in through
`ydotool type --file` on a 0600 file so it never appears in `ps`, and the
captures that caught it were deleted.

**A `shot_pid` capture resized to the window's LOGICAL width makes screenshot
coordinates directly clickable** (`magick … -resize 1400x` for a 1400px-wide
window at scale 1.5). That removes the native/logical conversion that the
harness header warns about, one arithmetic step at a time.

## 2026-09-14 — three user reports: a server that died, a share that ended the call, and a join that froze

Three reports against 0.9.5, none of them reproducible from the maintainer's
own desktop, and all three root-caused by reading — two of them proven with a
test that fails on the unfixed tree.

### 1. A dead homeserver put the user back on the LOGIN PAGE

Reported by Rokas, 2026-09-14: "my homeserver died due to a cloudflare issue
last night. Does lightning have to kick you to the login page — or could it
display the cached rooms, just you know not loading with an error message".
It did have to, and the reason is one builder call.

`build_client()` points every client at its homeserver with
`Client::builder().server_name_or_homeserver_url()`. That method is the right
one for a field a HUMAN typed — it is what `6e0f7e1` introduced for issue #5's
`.well-known` delegation — and it is two HTTP round trips: well-known
discovery, then a verification that whatever it settled on really is a
homeserver (matrix-sdk 0.18 `client/builder/homeserver_config.rs`,
`discover_homeserver_from_server_name_or_url`). **Every restore went through
it.** So with the server unreachable no `Client` could be constructed at all,
`restore_client` returned an error, `login_failed` was enqueued, and
AppController's own comment — "every restore failure path funnels through
loginFailed, which routes BootScreen to the genuine login form" — did the rest.

Nothing else about the session was in doubt. `restore_session()` itself only
reads the store. The rooms were on disk, the decrypted bodies were on disk, and
the FTS5 search index built over them was on disk.

**The fix is that a restore now has an offline path, and only a restore.**
`build_client_with()` takes a `HomeserverInput` of `Discover` (unchanged,
`server_name_or_homeserver_url`) or `Url` (`homeserver_url()`, which matrix-sdk
resolves with `Url::parse` and no network whatsoever). Every successful build
records what the SDK resolved in `lightning-homeserver-url` inside the
account's own store directory — inside it on purpose: it is a fact about that
store, and it is deleted with it. `build_client_for_restore()` tries discovery
FIRST on every restore, bounded at 10 s, and falls back to that record only
when the server did not answer.

**Discovery stays first, and that ordering is the contract.** A homeserver
that changes its `/.well-known` delegation must still be followed; skipping
discovery because a URL was recorded would freeze every existing install onto
whatever it resolved once. `a_reachable_homeserver_is_still_rediscovered_on_
every_restore` pins it.

**AND A TYPED STRING CAN NEVER BE PROMOTED TO A URL BY INSPECTION.** The
obvious shortcut — "if the stored homeserver parses as an http(s) URL, use
`homeserver_url()`" — is wrong and would break the largest homeserver there
is: a user types `https://matrix.org`, whose client API is at
`https://matrix-client.matrix.org`, and using the apex as the homeserver URL
gives a session that 404s on every request. Only a URL the SDK itself resolved
is safe, which is the whole reason the record exists.

MIGRATION, STATED HONESTLY: an account that has not signed in since this
build has nothing recorded, so its first offline start still fails exactly as
before. One successful online start fixes that permanently.

What the user sees afterwards: the room list is served from
`client.rooms_stream()`, which `restore_session` fills from the state store and
which `RoomList::entries()` returns verbatim — so cached rooms appear without
any server response. Both room-list empty states are already gated on
`count === 0`, so they correctly do not render over a populated list. The
connection state now starts at **Offline** rather than Disconnected when the
restore took the fallback (`session_restored_offline`), so the footer reads
"Offline — retrying" from the first frame instead of "Loading rooms…" over a
list that is complete and will never load anything. The sync supervisor retries
on its own.

NOT TESTED LIVE: no real account was opened against a dead homeserver. The
mechanism is covered by `a_restore_survives_the_homeserver_going_away`, which
starts a real loopback homeserver, builds a real SDK client with a real sqlite
store, KILLS the server, asserts that the old path (`build_client`) can no
longer build anything, and then asserts that the restore path can — that
failing assertion in the middle is the control, and it is what makes the case
evidence rather than decoration.

**AND SETTING THE STATE ONCE AT `login_ok` WAS NOT ENOUGH — caught in review,
after the first cut had already written the claim into this file as fact.**
`loginSucceeded` is a synchronous chain of DIRECT connections:
`RustSdkMatrixClient` -> `AuthManager` -> `AppController::onLoginSucceeded`,
which has no early return and ends in `m_client->startSync()`, which sets
`Syncing` unconditionally. No event-loop iteration separates any of it, so the
`Offline` set three lines earlier was overwritten before it could be rendered
— and the sync lane then reports "starting", which is `Syncing` again, which
AppController renders as "Loading rooms…". The state now lives in `setState`
itself: while a session has never reached its homeserver, `Syncing` reads as
`Offline`, released by the first `room_list_sync_state: running`, which is the
only event that proves a server answered.

**GENERALISE: a state set immediately before emitting a signal is not a state
the user sees.** Qt's default connection on one thread is direct, so the whole
downstream chain runs before the setter returns — and anything in it that
writes the same field wins. Set it where the field is written, or prove no
handler downstream touches it.

**A TRACK'S FAILURE ARRIVES BEFORE THE CALLER HAS FINISHED PUBLISHING IT, for
the same reason.** `publishShareAudio()` emits `failed()` synchronously, so
`onEngineFailed` re-entered `startScreenShare()` from inside that call — where
`m_shareAudioCid = audioCid` had not run yet. The new cleanup branch was
therefore a no-op on the one failure it was written for, and the controller
went on to record a cid and a `sfuAddTrack` declaration for a track with no
bin behind it; there is no remove-track verb, so that declaration would have
outlived the failure for the whole call, with every remote participant
carrying a `SCREEN_SHARE_AUDIO` track that could never produce a sample.
Before this round the teardown hid it by ending the session. The cid is
recorded BEFORE the publish now.

One harness fact worth keeping: **a `Client` with a sqlite store aborts the
process if it is dropped outside a tokio runtime.** deadpool's `SyncWrapper`
panics in its destructor, and a panic in a destructor during cleanup is a
non-unwinding abort — `signal: 6, SIGABRT`, no assertion message. Every
pre-existing case in `delegation_tests` uses an in-memory store and could
never meet it. Do all of it inside one `block_on`.

### 2. Sharing a screen WITH AUDIO ended the call — and per-application share audio had never worked at all

Reported by Seikm on a 0.9.5 flatpak: choosing a source to share kicks them
out of the call. Their log, in Portuguese:

```
share audio pipeline parse failed: referência inesperada "shareaudiomix"
engine failed category= "share_audio_failed" active= true
teardown state= 7 error= "A ligação terminou inesperadamente."
```

**TWO INDEPENDENT DEFECTS, and the second is the one that cost them the call.**

**The parse.** `publishShareAudio()` composed its bin inline as
`"%1 name=sharesrc ! queue ! …"`. That is valid only while `%1` is a single
element — and `mixedSourceDescription()`, the per-application capture, ENDS IN
A PAD REFERENCE (`shareaudiomix.`). GStreamer's grammar takes no assignment
after a reference, so the whole description was refused with
`unexpected reference "shareaudiomix" - ignoring`. Reproduced here in four
lines of C against the dev shell's own GStreamer before anything was changed.

So per-application share audio — the echo fix, the entire reason that file
exists — **could never have worked on any machine where the device monitor
starts**, which is every PipeWire desktop. The sink-monitor fallback worked,
which is why nobody noticed: a machine that took the fallback had share audio,
and a machine that qualified for the good path had none.

**GENERALISE, and this is the third time in this file for the same shape:**
a test that composes something *resembling* what production composes proves
nothing about production. `aShareAudioBranchParsesStandaloneTheWayTheDynamic
PathBuildsIt` appended `" ! fakesink"` to the mixed description and passed;
production appended `" name=sharesrc ! queue ! …"` and could not parse. The
composition is now ONE function, `shareaudio::encodedTrackDescription()`, and
the test parses exactly what the engine hands GStreamer. Every capture element
names itself (`name=sharesrc` is in the candidate table now, not appended), and
that is pinned too, because `handleBusMessage` recognises a device that will
not open by that name — which now also matches `shareapp*`, the
per-application sources, where there is no `sharesrc` at all.

**The teardown.** `onEngineFailed()` ended the CALL for every category it was
given. Share audio is an optional second track that had not started; killing
the session for it is the difference between a missing feature and a lost call,
and the user got "The call ended unexpectedly." — the generic fallback, because
neither `share_audio_failed` nor `share_audio_unavailable` had wording at all.
Both are now non-fatal, take the shape `onEnginePublishFailed` already uses
(turn the thing that failed off, say so, leave the call alone), and have
sentences that do not claim anything ended.

### 3. Joining a call froze the application (GitHub issue #12)

Reported by BroCraftPlus: Linux Mint 21.3, flatpak, joining any call in any
room freezes Lightning and needs a force quit; 0.9.3 is fine, 0.9.4 and 0.9.5
are not. **NOT REPRODUCED HERE**, so what follows is a hypothesis with a
mechanism — but the mechanism's shape is not in doubt and it does not belong
in the join path whatever a particular provider does with it.

`gst_device_monitor_start()` is SYNCHRONOUS, and each provider decides for
itself when it has an answer: the PulseAudio provider connects to the sound
server and then waits on its own mainloop until the initial device list
arrives, with no timeout anywhere in that path. `publishMicrophone()` calls it
on the **GUI thread** at every join. `git show v0.9.3:src/calls/SfuMediaEngine.cpp`
contains neither `monitorCandidates` nor `gst_device_monitor_start`; both
arrived in `7e6bcb6` with the device-preference work — exactly the boundary the
report names.

Two changes, and the first is the one that helps most people:

* **It is not asked at all when there is nothing to resolve.**
  `chooseCaptureElement()` returns nothing for an empty device id — but C++
  evaluates arguments first, so every join enumerated the machine's audio
  devices even for a user who has never opened the picker. The camera site has
  always been gated this way; the microphone site was not.
* **It can no longer hold the caller for ever.** The enumeration runs on a
  worker with a 2.5 s budget (MEASURED on a healthy PipeWire desktop here:
  218 ms for the first `Audio/Source`, 8 ms after, 4 ms for `Video/Source`).
  A blocked enumeration cannot be cancelled — GStreamer offers no such call —
  so the worker is abandoned rather than joined and the klass is LATCHED: a
  provider that hung once is never asked again this session. That bounds the
  damage at one stuck thread per klass per process instead of one per publish.
  Losing the answer costs the user their device PREFERENCE and leaves them the
  platform default, which is exactly what this function already returned when
  a monitor would not start.

No regression test: nothing here reproduces a hanging provider, and §18 is
explicit that a test which does not fail on the old code is decoration.

**REVIEW FOUND A SECOND UNBOUNDED ONE IN THE SAME PATH, and it is the older
of the two.** `perApplicationCaptureAvailable()` ends in the same
`gst_device_monitor_start()`, and its monitor installs **no filter at all**
(deliberately — a provider filter matches the PROVIDER's advertised classes,
and filtering on `Stream/Output/Audio` matches none, which once made the whole
feature inert). So it starts EVERY provider on the machine, PulseAudio's
included. It is read from `SfuCallController::shareAudioSupported()` and
`shareAudioExcludesOwnPlayback()`, both `CONSTANT` properties the call
header's share menu binds the moment `groupCall.active` flips true. It is
present in v0.9.3 unchanged, so it does NOT explain issue #12's version
boundary and does not compete with the diagnosis above — but leaving it would
have made "a call join must not block the GUI thread enumerating devices"
half true. Bounded the same way; giving up answers `false`, which is the
existing "no per-application capture" state.

Deliberately a second small implementation rather than a shared helper: the
engine's needs a per-klass latch across many calls, this one is a single
cached answer, and a template shared across two translation units to save
fifteen lines is worth less than both being readable on their own.

## 2026-09-13 (night) — the one feature no release has ever tested, audited by reading, and 0.9.5 cut on it

**RECOVERY AND KEY BACKUP HAD NEVER BEEN EXERCISED IN ANY RELEASE, AND IT
CANNOT BE**: §6 forbids logging or capturing recovery keys, and the setup flow
displays a freshly generated one. So it was read instead — QML to controller to
the FFI to matrix-sdk 0.18.0's own sources — and the audit found **six
defects, two of which could destroy an account's recovery**. `2eb38b1`.

The point worth carrying forward is not any one of them. It is that a feature
which cannot be driven is not thereby exempt from evidence: reading the code of
the SDK it delegates to is evidence, and it is the only kind available here.
Two of the six are only visible by reading matrix-sdk itself.

### The two that destroy recovery

**A successful setup left the button that destroys it on screen.** The Sessions
card gates its DESTRUCTIVE buttons on the crypto-health snapshot, and nothing
invalidated that snapshot after a backup action — its only callers are login,
first sync, verification-done and two explicit user actions. So after "Set up
recovery and backup" succeeded and showed the key, the card still read "No key
backup exists for this account" and "Secrets recoverable: Missing", and the
button was still enabled. Pressing it again is the *reasonable* response to a
screen that says it did not work — and `Recovery::enable()` calls
`create_secret_store()` **unconditionally**, so the key just written down stops
working, with none of the double confirmation `reset_key` carries.
`BackupController` now emits `cryptoHealthStale` on every outcome, **success or
failure**, because a failure can have changed server state too.

**"New recovery key" was offered on sessions that could not honour it.** It was
gated on `secretStorageAvailable`, which is SERVER truth — whether the account
has 4S at all — and is true on a session holding none of the secrets. `Reset`
calls `create_secret_store()` and fills the new store **only from what the
LOCAL olm machine can export**, so on a freshly signed-in unverified session it
repointed `m.secret_storage.default_key` at an EMPTY store: the old key opens
nothing, the new key opens nothing, and the cross-signing identity is no longer
recoverable from 4S by anyone. matrix-sdk's own source carries the matching
TODO. Now gated on this session actually holding what it would upload, and the
confirmation names what is lost.

### The other four

* **"Nothing was changed" was frequently false.** An `enable` that fails inside
  `create_secret_store()` has already created the backup version. Same
  conflation §6 names for cleanup — "target absent" and "reset completed" are
  different outcomes. The FFI genuinely cannot tell partial from total, so the
  copy no longer claims to know.
* **A failed `/devices` fetch rendered as "all devices verified, 3 of 3 checks
  complete".** The trust chain fell back to "this device is verified" on an
  EMPTY list, and the list is empty when the fetch FAILS as well as before it
  returns. §9: trust labels come from SDK state, and "the list did not load" is
  not the SDK saying verified. `sessionDevicesFailed` already existed and was
  rendered as a banner in the same pane; the binding simply never consulted it.
* **The card promised the recovery key "will not be shown again once you leave
  this card" and that was false** — Settings is a warm `Loader`, so switching
  section and back rendered it again. An ARMED destructive confirmation had the
  same lifetime bug from the same cause, turning a two-press contract into one
  press an hour later. Both are cleared together now.
* **The Home security banner sent two of its three phases to the wrong page.**
  All three routed to Sessions, but two of them say "Enter your recovery key"
  and that field is in Privacy & security. The file's own note records it was
  moved TO Sessions to fix a complaint — which fixed one phase and broke two.

**The verification flows came out CLEAN** and are the best-built part of the
feature: no trust promotion anywhere, every SDK callback flow-id scoped, `done`
sticky against a late cancel, and a Rust-side claim race that cancels our own
request rather than evicting somebody else's.

**FOUR MORE ARE KNOWN AND NOT FIXED**, recorded in `docs/open-items.md` and in
the release notes: an `enable` aborted by teardown after secret storage exists
but before the key is delivered still says nothing; `recover()` reports
"Recovery complete" when 4S held no backup key and nothing was restored; a
wrong recovery key shows an untranslated SDK error; and the restore panel can
wedge at "Restoring…" if the session ends mid-restore. **None can destroy a
key**, which is why they are follow-ups rather than blockers.

### The README had been a whole release behind, and nothing compared them

§14 lists CMake, Rust and the user agent as the synchronized locations. The
README is not on that list, and it was still advertising **0.9.3** while the
tree, the tag and the published packages were on 0.9.4 — every install command
in it naming a file the download page no longer served.

`tests/VersionConsistencyTest.cpp` (`a858223`) pins `project(VERSION)`,
`APP_VERSION_LABEL`, `rust/Cargo.toml`, the `matrix-client-rust` entry in
`rust/Cargo.lock` and the README against each other. It is deliberately a
CONSISTENCY check and never says what the version should BE — CMakeLists.txt is
authoritative for that.

Two things it had to get right to be worth having:

* **The lock file is matched through the package NAME.** `rand` in
  `rust/Cargo.lock` was coincidentally also `0.9.4`, so a value-shaped search
  would have passed on a lock file that had never been bumped.
* **The README quotes DEPENDENCY versions too** — Qt 6.8.2, GStreamer 1.26.2,
  the Ubuntu and Fedora Qt levels — and those must NOT move with a release. A
  first version flagged all four, which would have trained the next person to
  edit the test instead of the README. It counts a version as ours only if the
  line mentions Lightning or the version is backticked, with an `ours >= 5`
  floor so a reorganisation cannot make the scan vacuous.

### The website was rebuilt from the application's own design

Third repo (`lightning-website`), commit `e41656b`. The page read as generated
marketing — gradient-text hero, eight keyframe animations, 35 scroll reveals, a
marquee, a scroll progress bar — none of which exist anywhere in the client it
describes. `public/index.html` is now GENERATED by `tools/build-site.py` from
the app's own tokens, 1602 hand-written lines becoming 880 generated ones, and
`motion.js` is gone.

**Three defects, all found by RENDERING it, none visible to any check that
existed:**

* **Every Linux card showed the AppImage's `chmod` line.** `releases.js`
  rebuilds the cards by **cloning card zero**, so a card missing a
  `[data-lg-bind]` slot silently keeps card zero's text — and the baked HTML is
  perfect, because the miss only appears once the script runs. The first
  generator dropped `data-lg-bind="pkg.install"` from the Linux `<code>`. Same
  shape as the bug that once served the `.deb` from every Linux button.
* **Every Copy button was an empty rounded rectangle.** `.lg-copy` asked
  JetBrains Mono for `font-weight: 600`; the self-hosted subset carries
  400/500/700 only, and with `font-display: swap` there is no face to swap in,
  so the run paints as **nothing** while the DOM says "Copy" throughout. No DOM
  test can see that — **only a render can, and only by looking at it**.
* **A check added earlier the same session was deleted later in it.** The baked
  release-date invariant had been filed under the `motion.js` heading, and
  removing the motion layer took it out with it. **A check filed under an
  unrelated heading leaves with that heading.** It is back beside the version
  check it belongs with.

Covered now, each proved against the unfixed page first: `check.py` asserts
every card carries the slots `releases.js` rewrites and that each says what the
feed says, that every `font-weight` on the page has an `@font-face` to answer
it, and the baked date again. `tools/cards-test.js` is new — the jsdom test the
README had described for months and nobody had written — and it drives the real
`releases.js` over the real page on **both** passes, because they mask each
other: pass 2 sets every href by suffix and papers over a broken rebuild in
pass 1. `lightbox-test.js` counted `=== 6` copy buttons against a feed listing
five Linux packages; it counts off the feed now.

**GENERALISE: three of these four defects were invisible to every automated
check and visible immediately in a screenshot.** A page is a rendered artifact,
and the same rule that governs this project's GUI claims governs it — a suite
passing is not a picture.

### Headless screenshots lie about tall pages, and both engines lie differently

Worth writing down because two hours went into it. Chromium `--headless`
(old or `--headless=new`) with `--window-size=1400,8000` composites only the
initial viewport: everything below it comes back BLANK, and
`--virtual-time-budget` does not help because it is not a timing problem.
Scrolling with an injected `window.scrollTo` produces an all-black capture for
the same reason. **Firefox `--headless --screenshot --window-size=1400`
captures the true full page** (and needs `--profile <dir> --no-remote` if a
Firefox is already running, or it refuses).

And `loading="lazy"` images never arrive in EITHER engine's full-page capture —
the README already said so, and rediscovering it cost most of the two hours.
**Strip `loading="lazy"` into a copy before measuring anything below the fold.**

### Validation

`ctest` `build-rust`: **204 passed, 0 failed, 0 skipped, 204 total**. The
`-DLIGHTNING_ENABLE_WEBRTC=OFF` build over every target: `rc=0`. Website:
`check.py`, `check-assets.py --feed`, `cards-test.js` and `lightbox-test.js`
all pass. **Live validation of recovery and key backup: NOT TESTED**, for the
reason at the top of this entry, and that is how 0.9.5 ships it.

## 2026-09-13 (evening) — the snap could never carry call media, and the reason was NSS

**ROOT CAUSE, measured rather than reasoned about, and the SIXTH occurrence of
"a library loads its own plugins" — the first to reach a shipped lane.**

Debian builds **`libsrtp2` against NSS, not OpenSSL**. `ldd libsrtp2.so.1`
names libnss3, libnspr4, libnssutil3, libplc4 and libplds4; linuxdeploy's ELF
walk bundles all five and every payload assertion we have passes. But NSS does
no crypto itself: it **dlopens `libsoftokn3.so`** (the PKCS#11 softoken), which
in turn dlopens `libfreebl3.so`, from a directory derived at RUNTIME from
libnss3's own path. No NEEDED list mentions them, so nothing staged them.

Unconfined this is invisible — essentially every desktop Linux has NSS for its
browser and NSS finds the host's copy. Under **strict confinement `/usr` is the
BASE SNAP's and core24 has no NSS at all**, so there is nothing to fall back
to: libsrtp cannot initialise a cipher, `srtp_add_stream` returns
`init_fail` (err 5), `srtpenc` posts "Could not initialize SRTP encoder", the
publisher pipeline dies and the subscriber never gets a receive pad.

So the snap has **never** carried call media in either direction, while its
signalling, membership, media-key distribution, SDP and ICE were all correct.
That combination is exactly why it looked like anything but packaging.

### Four hypotheses tested and killed first

Each was the obvious next guess, and writing them down is the point:

| hypothesis | how it died |
|---|---|
| a missing GStreamer plugin (the Windows `libgstsctp` shape) | 29 plugins staged at `usr/lib/gstreamer-1.0/` and `GST_PLUGIN_SYSTEM_PATH_1_0` points at them |
| the publisher's bus error tearing the call down | `handleBusMessage` deliberately never calls `failed()` — it says so in its own comment — and the peers are separate `GstPipeline`s |
| a Matrix-level failure | membership publishes, the snap's media key ARRIVES and installs on the peer, the SDP answer is correct, both peers reach `ice-connection-state = 3` |
| OpenSSL provider loading (no `ossl-modules` in the payload) | relaunched with `OPENSSL_CONF=/dev/null`; `srtpenc` failed identically |

### What settled it

`GST_DEBUG=dtls*:6,srtpenc:5` **inside the confinement** (snapd passes it
through). The DTLS handshake COMPLETES — `dtlsdec: using agent with generated
cert`, then a correct 30-byte `aes-128-icm` key handed to srtpenc — and
libsrtp then fails to initialise *with that key*. `Failed to add stream to SRTP
encoder (err: 5)`. From there libsrtp2's own NEEDED list named NSS, and
`/proc/<pid>/maps` inside the sandbox confirmed the bundled libcrypto loaded
while no softoken existed anywhere on the system.

### The fix

`build-appimage.sh` stages `libsoftokn3`, `libfreebl3`, `libnssdbm3` and
`libnssckbi` beside `libnss3.so`, which is where upstream NSS ships them and
where NSS's own path derivation looks first. Both validators assert them BY
NAME, so a regression names itself instead of arriving as a silent lane — the
same shape as the xkb and fontconfig assertions and for the same reason: the
base snap cannot supply it, so the payload must. `libnss3` is pinned explicitly
in the build job rather than left to arrive as somebody's dependency, and that
pin is mutation-proved.

**The AppImage carries the identical gap** and is one NSS-less host away from
the same failure, which is why the staging lives in the script both formats
share.

**VERIFIED ON THE ARTEFACT — PASS, 2026-09-13 15:12 UTC.** Pipeline 214 built a
snap carrying all five modules (`validate-snap` asserts them by name and went
green), it was installed over the signed-in revision with `snap install
--dangerous` — which snapd treats as a refresh, so the account and secret
records came across and the session survived — and a two-party call with the
AppImage then produced, on the CONFINED snap:

```
received track attributed= true trackKey= "TR_AMAqt4vT6KWgQJ" fromPadMsid= true
call diagnosis: a receive chain is RUNNING for stream= "PA_LAHFDkbz8gqX" kind= "audio"
frames in the clear out   count= 3000
frames in the clear in    count= 3000
```

with **zero** `Could not initialize SRTP encoder` (two per join before) and
**zero** pipeline errors. `received track` had never once appeared on this
lane. The snap carries call media in BOTH directions for the first time in its
existence, and the fix working is what confirms the diagnosis.

Two notes on getting there, both costing a pipeline each and both avoidable:
the first attempt hard-coded `/usr/lib/x86_64-linux-gnu/nss`, which is where
Debian USED to keep these — `dpkg -L libnss3` on trixie puts them directly in
`/usr/lib/x86_64-linux-gnu`, and the guard caught it rather than shipping a
payload that staged nothing. The directory is derived from libnss3's own
location now, which is also how NSS finds them. And **scope the pipeline**:
`BUILD_FORMATS=appimage,snap` is 6 jobs against 14, and with one online runner
the difference is hours. Post it as a JSON body and CHECK
`/pipelines/<id>/variables` — form fields are silently ignored.

### One confounder, and one operational trap

The peer was still MUTED from the tile-badge test earlier in the day, which
made a first reading look like "nobody is transmitting". Unmuted it reached
4000 frames out while the snap stayed at zero.

And **do not push to `main` while a pipeline is running.** Pipeline 211 lost
`build-appimage` and `build-deb-ubuntu` to `error: source ref moved: expected
fdbf9ac…, resolved 8e4ed2a…`. That is `prepare-pinned-source.sh` working
exactly as designed — `resolve-source` pins the SHA and every later job
re-clones `main` and refuses to build a different commit — but it means any
push during a run fails everything after it. Trigger, then hold.


## 2026-09-13 (afternoon) — the packaged GUI sweep, and the five-minute call that did not drop

Driven on the laptop against the two fixture accounts: the packaged **flatpak**
(`org.lightning_matrix.Lightning`, 0.9.4, built 2026-09-12 22:11) signed in as
`lightningtest`, and a `0.9.4+git20260912.5abcc81` **AppImage** on the host
signed in as `lightningtest2`. Every claim below is a screenshot or an engine
log line; nothing is inferred from a build succeeding.

### The headline: `expires_for_refresh()` is LIVE-VALIDATED — PASS

A two-party MatrixRTC call between the flatpak and the AppImage was held from
10:38:50 to past 10:58 UTC — **nineteen and a half minutes, ~4x the five-minute
`MEMBERSHIP_EXPIRY_NO_DELAYED_MS` window**. At the end both clients still
reported `session read room participants= 2` and both `frames in the clear in`
counters were still climbing (57,500 on the flatpak). `frames dropped: no key
in` never appeared.

That is the defect the fix was written for: an RTC membership's deadline is
`created_ts + expires`, a refresh preserves `created_ts`, so re-writing the
same constant republished the SAME ABSOLUTE INSTANT and the participant died a
fixed five minutes after joining — lopsidedly, because peers rotated media keys
without them, so they could still be heard and could hear nobody. It had never
been exercised past five minutes on a real call. It has now.

### The user's own report, reproduced and then explained

**Every old call row in a room offered "Join" while a call was live.** Captured
on the flatpak: seven "started a call." rows from 21:49 through 10:02, each
with a Join button, alongside the genuinely live one at 10:38.
`CallEventDelegate.sessionLive` is room-scoped (`participantCount(roomId) > 0`),
so every row in the room binds identically. Fixed in `8406f33` and `2a70239`
(the newest-call-row cache and its in-place-Set hook); the flatpak under test
predates both, which is why the capture still shows it.

### Also PASS, all on the packaged flatpak under confinement

| what | evidence |
|---|---|
| screen share through the portal | SHARER: `screen share portal ready node= 137 remote_fd= true`, `capture negotiated 2560x1600`, `publish first encoded frame afterPublishMs= 163`. RECEIVER: the AppImage's OWN window, captured separately, drew the flatpak's desktop in its stage tile — NOT the sharer's self-view, and on the default RHI backend, not `QT_QUICK_BACKEND=software`. That distinction is what §16's WITHDRAWN 1 exists for |
| share audio | `share audio published perApplication= false`, with the honest output-monitor warning |
| call audio FRAMES, both directions — **not audibility** | `frames in the clear out` AND `in` both climbing; `a receive chain is RUNNING`. Nobody listened, and the flatpak's microphone volume was at 0% from an earlier round, so what is proven is that frames reach each peer's decryptor in the clear |
| group power control | Member -> Moderator -> Member, live, with the confirmation dialog; the member list regrouped under "Moderator — 1" and two `m.room.power_levels` events landed |
| threads | context-menu `T` opens the panel; a reply renders in the panel, the room shows a "1 reply" summary card, and §8 holds — the reply is NOT a standalone row in main |
| command palette | Ctrl+Shift+K, fuzzy match, and the action EXECUTES (theme 9 -> 10 on disk and on screen, back again) |
| notifications | a real freedesktop notification with the room avatar, Reply and Mark as read, for a message in a room the unfocused client was not looking at |
| the updater | installation type correctly "Flatpak"; a check reached the release server through confinement and reported up to date |
| local message search, INCLUDING an encrypted room | Ctrl+F -> History: "Searching 12 messages Lightning has indexed, including encrypted ones", and a hit on a phrase that exists ONLY in the encrypted room — which the server cannot search. §6's one sanctioned plaintext-on-disk exception, working inside the sandbox, with its index at `…/.var/app/org.lightning_matrix.Lightning/data/MatrixClient/matrix-client/<slug>/matrix-rust-sdk-store/lightning-search.sqlite3` — inside the flatpak's own data dir and deleted with the account |
| the notification's ACTIONS, both of them | **Mark as read**: the window caption went `(1 unread) Lightning 0.9.4` -> `Lightning 0.9.4` and the log shows two `read receipt sent` lines, so it sent real receipts rather than clearing a local flag. **Reply**: typed into the toast's inline field, `notification reply sent thread= false`, and the message landed DECRYPTED on the peer — from a client whose window was never focused |
| the call stage's tile grid, and a remote mute | two tiles, "You" and "lightningtest2", per-user identity colours; the peer pressed Ctrl+Shift+U and a crossed-microphone badge appeared on THAT tile and not on the local one |
| restart persistence | token AND crypto store: relaunch comes back signed in, rooms/spaces/theme restored, and the messages that decrypted before still decrypt |
| an unreadable secret store is not a missing account | relaunched WITHOUT `DBUS_SESSION_BUS_ADDRESS`: both account records survived and were offered under "Already on this device" — §6's rule, live |
| member list, call survives a room switch, Activity Center, Settings (all nine panes, all eleven themes listed) | captured |

### Six defects the sweep found

1. **Activity Center rows baked raw ids in for the session.** The seed is
   dispatched on the first `Syncing`, which is the connection coming up, not a
   room payload landing — so `roomInfo()` and `displayNameFor()` both answered
   nothing and the row kept `!abc:server` / `@bob:server` forever. Seen as an
   Activity row reading `@lightningtest2:matrix.smetonis.net` while the
   timeline two panes away read `lightningtest2`. Its second, silent
   consequence: `reconcileSeedAgainstRoomCounts()` skips unknown rooms, so a
   seed that beats the room list skips EVERY room and the whole
   bell-versus-room-list reconciliation does not run — the 0.8.4 defect, back.
   Both fixed; both hooks mutation-proved.
2. **Settings -> Updates contradicted itself**: "Last checked: 12 Sep 2026
   18:17" three lines above "Updates haven't been checked yet." Idle is not
   never-checked. Fixed.
3. **The Space Home action row ran off the pane.** Six buttons in a RowLayout,
   which does not wrap: at ~850 logical px "People (1)" lost its bracket and
   "Space settings" was off screen and unreachable. Now a Flow. Source-correct,
   but NOT instantiated by any test — the whole Space Home tree lives inside a
   `Component`, which `timeline-pane-qml` loads and never builds — so that is a
   parse-level claim only, and the wrapped row itself is NOT re-validated on a
   package either.
4. **An edited thread ROOT pushed its summary card off the bubble.** The
   card's Loader shares `metaRow` with the "edited" / "sending…" marker and
   was sized only by its own implicitWidth, so the marker took the left of the
   row and the card ran past the pane, clipping the last characters of its own
   "10:46". Now `Layout.fillWidth` with a `Layout.maximumWidth` of its natural
   width, so a row WITHOUT the marker is laid out exactly as before. Same
   honest status as the Flow above: source-correct, pinned by a source scan,
   and NOT re-validated on a package — no test instantiates that Loader in a
   thread-root state either.
5. **THE CONFINED SNAP CARRIES NO MEDIA IN EITHER DIRECTION — measured
   2026-09-13 16:30 once Rokas signed it in, and three hypotheses are now
   DEAD.** The snap itself is healthy everywhere else: it starts under strict
   confinement, renders on `backend=opengl software=0`, signs in, syncs, shows
   the whole room list and Spaces rail, raises a real desktop notification for
   an incoming call, joins the call and shows "Voice connected".

   What does NOT happen is any media at all. `frames in the clear` appears
   ZERO times in either direction across two join attempts, and `received
   track` never fires, so webrtcbin emits no receive pad. The first bus error
   is always `srtpenc*: Could not initialize SRTP encoder`, with
   `micsrc-actual-src-puls` and a `queue` "Internal data stream error"
   cascading after it.

   ELIMINATED, and each of these was the obvious next guess:
   - **Not a missing plugin.** The payload carries 29 GStreamer plugins at
     `/snap/lightning/x1/usr/lib/gstreamer-1.0/` — webrtc, srtp, dtls, nice,
     rtp, rtpmanager, opus, sctp, pulseaudio, audioconvert, volume — and
     `GST_PLUGIN_SYSTEM_PATH_1_0` points at them. This is NOT the Windows
     `libgstsctp.dll` shape.
   - **Not the publisher killing the call.** `handleBusMessage` deliberately
     never calls `failed()` on a bus ERROR (it says so in its own comment, and
     the reverted attempt is recorded there), so the publisher's death cannot
     tear the subscriber down — and the peers are separate `GstPipeline`s
     anyway.
   - **Not a Matrix-level failure.** Membership publishes, the snap's media
     key ARRIVES and installs on the peer, the subscriber's SDP answer is
     correct (`[0 mid=0 application] [1 mid=1 audio dir=recvonly]`), and both
     peer connections reach `ice-connection-state = 3`.

   CONFOUNDER FOUND AND REMOVED, which is why the first reading was wrong: the
   peer was still MUTED from the tile-badge test earlier in the round. Unmuted
   it reached `frames in the clear out count= 4000` while the snap stayed at
   zero, so "nobody was transmitting" is not the explanation.

   RIG LIMITS THAT BOUND WHAT THIS CAN PROVE, both real and both in the
   container rather than the package: there is **no audio daemon** (the Pulse
   socket the launcher bridges is a stale file with nothing listening, which
   is why `micsrc` errors), and **no portal at all**
   (`org.freedesktop.portal.Desktop was not provided by any .service files`),
   so screen share and camera are untestable here. Finishing this needs a snap
   container with `pipewire-pulse` and `xdg-desktop-portal`, or a real desktop
   session.

   **CAUSE ESTABLISHED THE SAME EVENING — see the NSS entry below, which is
   the sixth occurrence of "a library loads its own plugins".** Note 0.9.4 already ships a Snap
   (`Lightning 0.9.4 — Snap amd64` is one of its ten package links) and that
   one could not even start — so this is not a regression, and everything
   above 0.9.4 strictly improves it.
6. **The snap was not signed in for most of this round.** Its window sits on the
   login screen with no account record at all (`matrix-client.conf` carries a
   homeserver URL, window geometry and an update timestamp — no account
   section), and signing it in needs a password typed into the app, which is
   not something this session does. Its log also shows **no portal at all**
   (`org.freedesktop.portal.Desktop was not provided by any .service files`)
   and the insecure QSettings fallback, both of which are this container rig
   rather than the package.

### One claim WITHDRAWN, before it was acted on

"Call shortcuts are dead while a menu is open" was carried into this round as a
defect with a one-line fix. **It is refuted.** With the message context menu
open on the flatpak, Ctrl+Shift+K opened the command palette, and KWin reports
NO extra window for the client while that menu is up — Lightning's menus are
in-scene popups, not `xdg_popup`s, so the window never loses activation and
`Qt.WindowShortcut` keeps firing. Had it not been checked, a pointless
`Qt.ApplicationShortcut` change would have shipped. A NATIVE menu would still
be a separate window; nothing here uses one.

### Two things that looked like defects and are not

- **Date dividers in Lithuanian under an English UI.** The host sets
  `LANG=en_US.UTF-8` with `LC_TIME=lt_LT.UTF-8`. Qt honours exactly that split.
  `lightning_lt.ts` exists, which is what made it look like a translation
  loading failure. Do not "fix" it.
- **Timestamps three hours apart between the two clients.** The flatpak's
  container has no timezone configured (C.UTF-8, UTC); the host AppImage is
  EEST. Rig, not app.

### NOT TESTED, with the reason

- **Recovery and key backup setup, and therefore cross-user verification.**
  Neither fixture account has cross-signing or a backup (Sessions reports
  Master/Self-signing/User-signing and secret storage all "Missing"), so the
  encrypted room's history is undecryptable on both devices — correct Matrix
  behaviour, not a defect. Setting it up means putting a generated recovery key
  on screen and therefore in a capture, which §6 forbids. **Declined for
  CAPTURE reasons, not test reasons** — it is runnable capture-free by a human,
  or by automation with captures off, taking the evidence from the Sessions
  panel afterwards (the three cross-signing rows flipping from "Missing") and
  from the log. Do not read it as impossible.
- **Everything signed-in on the snap**, for the reason above.
- **Media send through the file chooser** — ATTEMPTED this round and blocked by
  the RIG, which is worth writing down because it looks exactly like an app
  defect. The flatpak has NO filesystem permission at all (`flatpak info
  --show-permissions` lists only `xdg-run/pipewire-0` and
  `xdg-config/kdeglobals:ro`), which is correct: every file reaches it through
  the document portal. The portal returned
  `/run/user/1000/doc/<id>/sweep-upload.png` and Qt refused it —
  `QML FileDialog: Cannot set ... as a selected file because it doesn't
  exist` — so `selectedFiles` was empty, nothing was attached, and the
  composer was left as it was. `/proc/mounts` says why: `/run/user/1000/doc`
  carries the real `fuse.portal` mount AND a **tmpfs mounted over the top of
  it**, so the document is genuinely unreachable inside the sandbox. Rig, not
  package.

  **OPEN ROBUSTNESS ITEM, and the three parts of it have different standing —
  do not let the caveat on the last two swallow the first.**
  (a) ESTABLISHED: the picker closed and the user was told NOTHING. No
  attachment, no notice, no change to the composer. That is a direct
  observation and does not depend on which branch ran.
  (b) NOT ESTABLISHED: the mechanism. `attachmentRejected` exists, so someone
  already decided this surface owes the user a word — but this failure lands
  before `addAttachment()` is reached, and whether `accepted` fires at all
  with an empty `selectedFiles` was not instrumented.
  (c) NOT ESTABLISHED: whether a HEALTHY sandbox can reach that branch at all.
  The trigger here was the rig fault above.
  §16's "graceful fallback and silent absence are the same observable" is the
  named lesson for (a), and one log line at the `onAccepted`/`selectedFiles`
  boundary settles all three — while the tmpfs is still there to reproduce it.

  Doing it at all needed care and the method is worth keeping: the portal's
  chooser browses the MAINTAINER'S OWN HOME, so nothing captured it. The path
  was typed blind into the Name field and verified by cropping a 46-pixel-tall
  strip of that one row, with the full-screen original deleted on the spot.
  Ctrl+L did NOT reach the location bar — the first attempt typed into the
  file list and left the Name field empty, which is exactly why the strip
  check happened before Enter was pressed.
- **Audibility** of any of it. Nobody listened; the flatpak's microphone volume
  was at 0% from an earlier test (reset to 100% at the end of this round).
- **A notification CLICK** (as opposed to its two action buttons, both of which
  were pressed and are PASS above) — clicking the toast body to raise the
  window and open the room was not exercised.
- **The speaker ring** on a call tile. Names and the mute badge were checked
  against a known state; who-is-talking was not.
- **The updater past the check**: a flatpak must REFUSE to self-install, and
  that refusal's wording was not exercised.
- **Element interoperability.** Both ends here were Lightning, so nothing in
  this round is an interop claim.

One rig note worth keeping: before sharing the screen, MINIMISE every
non-Lightning window first. The portal's picker still lists them with
thumbnails (minimising does not remove them from the picker), but the "Laptop
screen" capture then contains only Lightning windows, which is what makes a
whole-screen share safe to run and to look at on the far end.


## 2026-09-13 — the snap had never worked, and only a real snapd could say so

**The first time the Lightning snap was run the way a user runs it** — `snap
run` under a real snapd on Ubuntu 24.04, strict confinement, the manual
interfaces connected — **it failed three ways before it could show a window**,
and every structural check in CI was green throughout. That is the whole
lesson: `meta/snap.yaml` parsing, the payload audit and the launcher smoke run
all pass on a snap that cannot start.

Under strict confinement `/usr` is the BASE SNAP's, so anything the host has
is unreachable unless an interface bind-mounts it; and snapd remaps
`XDG_RUNTIME_DIR` to `$XDG_RUNTIME_DIR/snap.<name>`, so every socket a desktop
session leaves in the real runtime dir sits one level up.

| | symptom | cause |
|---|---|---|
| compositor | Qt: "no Qt platform plugin could be initialized", abort | relative `WAYLAND_DISPLAY` resolves inside the remapped runtime dir |
| graphics | `software=0` never reached; app warns video will not display | payload has glvnd DISPATCH stubs only; the vendor driver is dlopened and `ldd` never saw it; core24 has no Mesa |
| startup | SIGSEGV (exit 139) right after `window placement` | core24 carries no xkb keymaps and no fontconfig configuration |
| audio capture | `micsrc` "Connection refused", no microphone in the picker | PipeWire and Pulse sockets are one level up, same as the compositor |

Each was proved with a mutation both ways. The compositor: relative
`WAYLAND_DISPLAY` aborts, a `$XDG_RUNTIME_DIR/wayland-0 -> ../wayland-0`
symlink starts. The audio pair, counting `pa_context_connect() failed`: **1**
without the bridge, **0** with it. The crash was attributed to the missing
data rather than to the renderer by a control: the identical payload run
UNCONFINED with `QT_QUICK_BACKEND=software` ran fine for 35 s.

**A CAUSAL CLAIM THIS ENTRY ORIGINALLY MADE IS WITHDRAWN.** It said the
confined snap could send but not receive BECAUSE the microphone failure
cascaded into `srtpenc0: Could not initialize SRTP encoder` and took the
subscriber down with it. This repository's own code refutes that:
`ensurePeer()` calls `gst_pipeline_new()` PER PEER
(`SfuMediaEngine.cpp:966`, one `Peer` each for Publisher and Subscriber, each
with its own bus and its own teardown), so a capture failure in the
publisher's pipeline cannot by construction stop the subscriber's connection
reaching `connected`. And `srtpenc` failing to initialise is what it reports
with NO KEY — a symptom of a DTLS handshake that never completed, not a cause
of one. The arrow may point the other way.

What IS established: the sockets sit one level up, and bridging them fixes
`pa_context_connect()` (mutation-proved both directions) — that is microphone
capture and device enumeration, nothing more. **The confined snap's RECEIVE
path is NOT TESTED and its cause is NOT ESTABLISHED**; recreating the
container destroyed the snap's signed-in session before the fix could be
re-tested. Do not read the bridge as having fixed it.

**The fix**: the launcher bridges the compositor, PipeWire and Pulse sockets
into snapd's per-snap runtime dir; the build stages xkb keymaps and the
fontconfig configuration; graphics come from Canonical's `gpu-2404` content
snap, exec'ing through its provider wrapper when connected and falling back to
a direct exec (software renderer, with the app's own warning) when not.
Verified live: `scene graph backend=opengl software=0 platform=wayland` with a
rendered window, under real confinement.

**FONTS ARE DELIBERATELY NOT STAGED, and the first version got this wrong.**
fontconfig's `<dir>` entries are ABSOLUTE, so a font tree under `$SNAP` is on
no search path and is inert; snapd's `desktop` interface already bind-mounts
the host's `/usr/share/fonts` and `/var/cache/fontconfig`. What snapd does not
provide is `/etc/fonts` — so the configuration is staged and the glyphs are
not.

**And `cp -a` on `/etc/fonts` ships broken rules.** 22 of the 35 entries in
`conf.d` are absolute symlinks into `/usr/share/fontconfig/conf.avail`, which
is neither staged nor in core24: measured, `cp -a` → 22 dangling, `cp -aL` →
0 with all 35 resolving. The casualties were every generic-family alias, the
metric aliases, the hinting defaults and `70-no-bitmaps-except-emoji.conf` —
the rule §16's emoji lesson turns on. Present file, broken pointer: the same
shape as the defect the whole change exists to fix.

**Two traps in the guards themselves**, both found by measurement rather than
review. `cp -a src/. dst/` succeeds on an EMPTY source, so counting successful
copies lets an image that gains an empty `/etc/fonts` pass the build and fail
validation forty minutes later — the guards now assert `rules/evdev.xml`,
`fonts.conf`, a rule count and three named rules. And `find -xtype l` passes
VACUOUSLY when rules are ABSENT rather than dangling, which is exactly what
happens when `/usr/share/fontconfig` is missing at build time.

**Three of the six first-draft assertions passed on deliberately broken
input**, caught in review: `grep gpu-2404 meta/snap.yaml` matches the
template's own COMMENTS, so it passed with the entire plug stanza deleted;
`grep gpu-2404-provider-wrapper` matches the `GPU_WRAPPER=` assignment, so it
passed on a launcher that never execs through it; and the Wayland grep matched
the `[ ! -e ]` test guarding the symlink. They are now a parsed-YAML check and
greps for the `exec` and the `ln` themselves.

**`default-provider` is not auto-connection.** snapd's base declaration allows
content auto-connection only when the plug and slot publishers match;
mesa-2404 is Canonical's. So a store install pulls the provider in and leaves
it disconnected — software renderer, no call video. That, plus `camera`,
`audio-record` and `password-manager-service`, makes **four** manual
interfaces; the snap needs a store snap-declaration before it is fit to
publish. Recorded in `packaging-ci/docs/package-layout.md`.

**Blocker caught before it shipped**: the first draft staged fonts and
required three data sets, but the pinned builder image has neither `/etc/fonts`
nor `/usr/share/fonts` of its own — the guard would have turned `build-snap`
red on the next pipeline. `xkb-data` and `fontconfig-config` are now installed
by the job and pinned in `tests/test-pipeline-config.py`, the same way the
GStreamer dev packages are.

ACCEPTED FOLLOW-UP, not fixed here: the payload still stages
`libEGL/libGL/libgbm/libdrm` from the build image while the gpu-2404 wrapper
appends its own paths AFTER `$SNAP/usr/lib`. glvnd dispatch makes this work
(the vendor comes from `__EGL_VENDOR_LIBRARY_DIRS`), and the live run reported
`software=0` — but canonical/gpu-snap's own integration runs
`gpu-2404-cleanup` to remove provider-owned libraries, and not doing so leaves
a version-mismatch surface. Also unexercised under confinement: screen share
through the portal, and the file-chooser paths (the snap plugs no `home`).


## 2026-09-12 (night) — a shared window that never repaints published nothing

**LIVE-VALIDATED PASS**, on the laptop, same window and same peer before and
after — the only variable was the build.

**The defect.** Sharing a window that does not repaint (a file manager showing
a static list) published NO video at all. Measured on the packaged Debian deb:

```
capture negotiated caps= video/x-raw, format=BGRx, width=1140, height=869,
    framerate=(fraction)0/1, max-framerate=(fraction)60/1
capture delivered frames count= 1
```

and no `publish first encoded frame` line ever. The receiving client sat on
"Waiting for the picture" indefinitely. The same share switched to the whole
screen reached `afterPublishMs=269` and rendered, which is why this had never
been seen: every share anyone had tested was of something that moves.

So it is the §16 `videorate` hold, and **its worst case is not a wait, it is
never**: PipeWire delivers ON DAMAGE, videorate emits nothing until a SECOND
buffer arrives, and a still window never produces one. The block in
`videoRateStage()` described the wait ("~1 s busy desktop to 10 s still
desktop") and that description was incomplete rather than wrong.

**One new refutation.** A GAP event was the cheapest candidate — videorate
handles GAP at all — and it does nothing here. Measured in the dev shell, one
buffer and no EOS into the real rate stage, GStreamer 1.26.11:

```
nothing                            ->  0 buffers out
GAP every 100 ms for 2 s           ->  0 buffers out
re-push the last picture, 100 ms   -> 59 buffers out
```

Every entry already on that function's do-not-retry list fails for one reason:
none of them is a buffer. The fix is to hand videorate an actual second one.

**The fix** (`5abcc81`). A throttled probe on the rate stage's upstream peer
keeps a deep copy of the last frame, and a 200 ms timer chains it into
`videorate` whenever the capture has been quiet for >500 ms. Screen share
only. The copy is taken DOWNSTREAM of the scale stage (so ~3 MB five times a
second, not a 4K BGRA frame) and DEEP (so the source's pool buffer returns to
PipeWire immediately — holding one is how `min-buffers=8` and
`keepalive-time=100` each killed the capture outright).

**Two shapes were wrong first; both are recorded in-source so they are not
retried.**

1. `gst_pad_push()` from an IDLE probe on the upstream peer DEADLOCKS against
   itself. An IDLE probe is a BLOCKING probe, so the pad stays flagged blocked
   for the callback's duration and the push waits in `do_probe_callbacks` for
   a block only that callback can lift. The regression test caught it by
   hanging for 300 s; no review would have. `gst_pad_chain()` on videorate's
   sink pad, dispatched through `gst_element_call_async`, is the right shape.

2. **The injected PTS must come from the SAMPLED BUFFER, never the pipeline
   clock** — found in review, and this one would have shipped a worse defect
   than it fixed. The first version took `gst_element_get_current_running_time`
   on the stated premise that "the source stamps its buffers the same way".
   True of `pipewiresrc` and `gdiscreencapsrc`; FALSE of the other two share
   sources — `LightningWindowCaptureSrc` and `ximagesrc` are both deliberately
   zero-based, and this repository says so in two places. Measured by the
   reviewer at **2612 buffers out of ONE injection** at a 174 s call age,
   after which every real frame was behind `prevbuf` and dropped: a transient
   stall would have become a permanently dead share, on the two platforms that
   never had the defect being fixed. Anchoring on the sampled frame's own PTS
   plus elapsed wall time is timebase-agnostic and cancels pipeline latency.

**Live validation, 2026-09-13 00:40.** Same static Dolphin window, same two
clients, same machine:

| | before (`bdba9f0`) | after (`5abcc81`) |
|---|---|---|
| capture | `delivered frames count= 1` | `delivered frames count= 1` |
| keep-alive | — | `re-pushed the last picture count= 1 quietMs= 506` |
| publish | **no line, ever** | `first encoded frame afterPublishMs= 595` |
| far end | "Waiting for the picture", indefinitely | **the window, rendered and readable** |

No regression on a moving share: a full-screen share published in 141 ms
(`rateStageHoldMs= 77`, the ordinary hold) and the keep-alive fired **zero**
times, which is the whole design — it acts only when the capture goes quiet.

Tests: `theKeepAlivePtsNeverLeavesTheSourcesTimebase` (the arithmetic —
anchored to the sample, strictly increasing, refuses an invalid source PTS),
`aStillScreenStillPublishesAPicture` (the real rate stage, one buffer, no EOS;
the control asserts ZERO without the injection, which is the defect itself),
`theRealPublishDescriptionStillOffersAnInjectionPad` (both pads the feature
needs, against the REAL publish description). All mutation-checked. What is
NOT covered is stated in the test rather than implied: arming needs a live
publish. `sfu-media-engine-test` 73 passed, 0 failed, 0 skipped.
`-DLIGHTNING_ENABLE_WEBRTC=OFF` over every target: clean.


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

#### 2026-09-12 (afternoon), the sound/shortcuts round and what the review caught

**THE D3D11 VERDICT IS IN AND IT IS A PICTURE, NOT A COUNTER.** The Windows
guest, upgraded in place to the `Direct3D11` build, RENDERS a remote screen
share: the receiving tile carries the sending laptop's live desktop, complete
with the mirror recursion only a live feed produces. Same guest, same share,
the same `frames in the clear in … video= true` counters that this morning's
entry records advancing past 1000 against an empty rectangle — the renderer
was the only variable. The Windows "no video" defect is CLOSED and it was
never packaging: it was our own GL probe choosing Qt Quick's software
adaptation. Sender was the laptop's Linux client A, receiver the guest, which
is the split to reuse: the guest holds ONE portable install and its store
travels with the exe folder, so two clients there are one account, one device
and two processes on one sqlite store.

**AND FOUR CONVINCING DEFECTS ON THAT GUEST WERE ITS CLOCK.** Its timezone was
`Pacific Standard Time` against a UTC hardware clock, so it computed UTC SEVEN
HOURS IN THE FUTURE. An `m.call.member` expiry is `created_ts + expires`, so
every membership in the room read as expired: `RtcController` logged
`participants= 0 source= "server" rawEvents= 6` for a room with a live call,
and the client showed no call banner, no Join button, no room-list call glyph,
and drew the in-call facepile as `?`. All four look exactly like product
defects. `tzutil /s "UTC"` and the very next read was `participants= 1 source=
"store"` with all four correct. GENERALISE: before reading ANY call-state or
expiry behaviour off a VM guest, check `date -u` on the host against the guest
log's own `Z` stamps. Treat every Windows call-state observation made on that
guest before 2026-09-12 13:16 EEST as unreliable.

**WINDOWS TRAY NOTIFICATIONS ARE LIVE-VALIDATED AT LAST.** A real toast, with
the room avatar and "lightningtest in Design Review"; clicking it raises the
app from minimised and opens that room. `CLAUDE.md` §16 has recorded that path
as NOT LIVE-TESTED on Windows since the day it was written. Read-withdrawal of
a balloon (WinRT `ToastNotificationHistory.Remove`) and notification SOUND
remain untested.

**A SHORTCUT GATED ON A PREDICATE THAT CANNOT SAY "NOT RIGHT NOW".**
`AppController::canStartCall` is one line over `preferredCallLane()` — "does
this room have a lane" — and `SfuCallController::join` opens by tearing down
whatever call is running. A new Ctrl+Shift+C gated on `canStartCall` alone was
therefore live in every other RTC-capable room DURING a call, and would have
ended it. The timeline button never had the hole because it also asks
`!app.groupCall.active` and that the legacy lane is idle; two comments claimed
the key used the same predicate. **The predicate cannot be hoisted into C++**:
a `Q_INVOKABLE` has no `NOTIFY`, so a binding on it would never re-evaluate
when a call starts — stale in the dangerous direction. It lives in two files
and a contract case asserts they agree, DERIVING the clause list from the
button's own `visible:` expression (split on `&&` at paren depth zero) rather
than hand-keeping it — because a hand-kept list only sees a clause LEAVING the
button, and the button GAINING one is the same defect mirrored. Proved failing
in both directions. GENERALISE: when one predicate must exist twice, test that
the two agree, not that either one has particular text.

**A CONTROL THAT WROTE A SETTING AND NEVER READ IT.** `VideoPlayerCard.qml`
hard-coded `volume: 0.8` while its control bar reached the shared
`MediaVolumeControl`, whose slider calls `rememberVolume()`. So dragging a
video's volume stored the level globally and no video ever started at it —
the 2026-08-18 tester report a second time, because that fix landed on the
audio card and the case that pinned it NAMED ONE FILE. Invisible for months
because `mediaVolume` defaults to the same 0.8 the literal was. The
replacement sweep derives the surface list from the tree (`AudioOutput {`)
instead of naming files. Behaviour change worth a release note: anyone who
ever dragged a video's volume has been accumulating a value nothing read back,
and their videos now start at it.

**Ctrl+Alt IS AltGr ON WINDOWS AND ⌘⌥ ON macOS, and an audit scoped to
`i18n/` sees neither.** Two new rows shipped on Ctrl+Alt for one review cycle.
Windows delivers AltGr as Ctrl+Alt, so Ctrl+Alt+M fires while a German T1 user
types µ; and portable Ctrl/Alt map to Command/Option, so Ctrl+Alt+H shipped as
⌘⌥H — the system "Hide Others" Qt's own application menu installs. The set
that matters is the layouts users HAVE, not the catalogs we ship. Both moved
to Ctrl+Shift. The pre-existing `call.returnToCall` = Ctrl+Alt+A breaks both
rules and is now recorded as knowingly accepted rather than left unmentioned,
because moving a shipped default silently rebinds it for every install.
Ctrl+Shift+W was rejected for `call.leave` in the same pass: it collides with
nothing, but it is the reflex chord for CLOSE THE WINDOW and `CallHeaderBar`
shapes leaving as the one irreversible action on that bar.

**A HAND-KEPT COPY OF A LIST IS A LIST THAT DRIFTS.** `QuickSwitcher.qml`
builds its "Open <section>" rows from a local array that had fallen THREE
sections behind Settings' nav rows — `shortcuts` and `updates` had never been
in it. The one surface whose entire job is "type a name and land on it" could
not reach three of ten sections, and nothing could notice because every row it
does build works.

**AND A FLOOR UNDER AN EQUALITY TEST MUST BE THE REAL COUNT.** The case added
for that required "at least 8" matches on each side of a comparison that only
asks the two sets to be equal — so with ten sections, two could vanish from
BOTH files and it would still pass. Same shape as a sweep that matches
nothing: the guard has to be tight enough to be a guard.

**AND THE CLAIM WAS NOT ASSERTABLE UNTIL AN INSTRUMENT WAS ADDED FOR IT.**
`SfuMediaEngine::setMicrophoneGain` walked the published bins, set `volume` on
any `micvol` it found and SAID NOTHING, so whether the new control reached the
audio graph was unobservable — the identical state the per-participant volume
was in when a live run produced "the slider read 200% and nothing had reached
the audio graph". The menu's readout is computed from the slider's own value
and the stored number only proves the SETTING was written; neither is
evidence. `microphone gain applied: percent= N gst= F elements= K` plus a
"nowhere to land" warning is the twin of the line `e07a663` gave the other
path. GENERALISE: before testing a claim, ask what observation could
distinguish it from its failure — and if there is none, the first commit of
the round is the instrument, not the feature.

**Live status: the round is validated, on a packaged AppImage that bundles Qt
6.8.2.** Two real clients, a real homeserver, the pipeline-203 artifact. PASS:
the microphone level reaches a real `volume` element at 0%, 100% and 200%; a
`Slider` inside a `QQuickMenu` can be dragged at all; the >100% glyph swap,
clipping warning and Reset row all appear; Ctrl+Shift+C is INERT during a live
call **with a positive control**, because a dead key and a correctly-gated key
look identical; Ctrl+Shift+Y leaves; Ctrl+Shift+S opens the picker and
publishes nothing; and Ctrl+Shift+A / Ctrl+Shift+R work against a real unread
state — which the MOCK backend could not have answered, since it does not
clear unread badges even from its own context menu. That last point is worth
keeping on its own: **the control that proved the fixture was the limitation
was the pointer-driven path failing the same way.** NOT TESTED: audibility
(nobody listened), a slow or diagonal HUMAN drag, the account switch, restart
with a call, and the PiP window, which declares none of the call keys. The
full plan, with what would make each observation look like a pass while the
feature is broken, is the vault note "Lightning/Testing/Sound and shortcuts
round — live test plan".

#### 2026-09-12, the overnight live round: three defects the harness could not see

**A COUNTER THAT COUNTS FRAMES NOBODY CAN SEE.** The engine's
`frames in the clear in` climbs whenever frames are decrypted, which says
nothing about whether a picture was drawn — and on Qt Quick's software
adaptation nothing IS drawn, because `QSGSoftwareRenderableNode::NodeType` is a
closed list of rectangles, glyphs, images and nine-patches and Qt Multimedia's
`QSGVideoNode` is none of them (the path is RHI-only besides, and that context
has no RHI). Measured on Windows, then reproduced on Linux with
`QT_QUICK_BACKEND=software` as the ONLY variable: the counter passed 500
against an empty rectangle where the same client on the default backend
rendered the remote desktop perfectly. Every "screen share PASS" that rested on
that counter was withdrawn.

**AND THE FALLBACK THAT PUT WINDOWS THERE WAS OURS.** `src/main.cpp` probes for
OPENGL — correct for the AppImage/Wayland case it was written for — and then
forced `Software` on every platform. Qt documents Windows' default as Direct3D
11: no
OpenGL, WARP without a GPU, RHI-backed so video renders. So the packaging item
this was blamed on (`opengl32sw.dll` absent) was not the fix; the image has no
such DLL and Fedora's mingw Qt never ships one. GENERALISE: a probe for ONE
graphics API must not choose the fallback for platforms that have a different
native one.

**A MEMBERSHIP NAMING THIS DEVICE IS NOT EVIDENCE THE DEVICE IS IN A CALL.**
Three surfaces asked `app.rtc.ownDeviceInSession()` and treated it as "am I in
this call". A device id survives a restart, so an unclean exit left a ghost that
hid the only Join affordance for five minutes — from exactly the user who had
just been dropped. Confirmed by waiting: the banner returned by itself when the
ghost expired. The local call controller is the authority; the C++ declaration
now says so.

**A LAYOUT FLOOR DOES NOT CREATE ROOM.** `Layout.minimumWidth: implicitWidth`
on the call controls meant that once the row's minimums exceeded the panel the
RowLayout handed every child its minimum and OVERFLOWED — and the end of that
row is the hang-up button, so below ~1100 px a user could not leave a call. The
fix drives `compact` from available width, as a LATCH: `compact` changes
`implicitWidth`, so a plain `width < implicitWidth` binding oscillates. It
learns the expanded requirement while expanded and freezes it on the way in.

**A PROCESS-GLOBAL FLAG FOR A PER-SERVER PROPERTY.**
`DELAYED_EVENTS_REFUSED` was an `AtomicBool`; MSC4140 belongs to a HOMESERVER.
One account on an old Synapse disabled crash-safe call cleanup for every other
account until restart. Now keyed by `client.homeserver()`. In the same area, a
server that IGNORES `?org.matrix.msc4140.delay=` answers **200 with no
`delay_id`** — not a 404 — so a "latch only on a permanent refusal" rule that
matched on 404s alone never latched on the one server class GitHub #10 came
from, and would have armed a naked retraction every 60 s. Classified on
matrix-sdk's error VARIANT, never on message text.

**AND THE PACKAGING CHECKS TAUGHT THE SAME LESSON THREE TIMES.** A snap that
installs, runs `--version`, and cannot open a window (`libSM` unstaged, reached
through `libQt6XcbQpa`, with `libuuid` behind it). A guard written to catch that
which was a SILENT NO-OP in CI because ubuntu:24.04 ships no `readelf`. And a
staging step that could not see a GStreamer plugin's own dependencies, because
Qt's plugins carry `RUNPATH=$ORIGIN/../../lib` and gst plugins carry `$ORIGIN`
alone — so naming `libGL`/`libgbm` staged nothing and `libgstopengl` stayed
unloadable. Measured in the job's own pinned image against a real payload:
staged 10 before, 12 after, guard green.

**MY OWN PROBE COULD NOT FAIL.** I reported "no fatal entry" from a tree into
which I had hand-copied the two libraries the staging step cannot produce. §16
records that trap and I walked into it anyway; the review caught it by running
the real functions against the PRISTINE payload. Ask what would make a probe
pass on broken code BEFORE trusting it — every time, not just when it is
convenient.

#### 2026-09-11 (evening), four user reports, and a bell that hid real unreads

**THE SIGNAL THAT ANNOUNCES A CHANGE IS NOT NECESSARILY THE SIGNAL THAT
CARRIES IT.** This is the generalised form of the round's one serious defect
and it cost a review round to catch. `ActivityModel` needed to know when a
room had been read on ANY device, which the SDK expresses through four
`RoomInfo` fields (`unreadCount`, `highlightCount`, `hasUnreadMessages`,
`markedUnread`). The obvious trigger was `MatrixClient::roomUpdated` — a
per-room signal, named for the thing being watched. It is the one room signal
that never carries unread state: `RustSdkMatrixClient` emits `eventAppended`,
and then THIRTY-SIX LINES LATER IN THE SAME FUNCTION raises the room's
`lastActivity` to that event's timestamp and emits `roomUpdated`, touching no
unread field. Those four fields are written in exactly one place,
`rust_rooms::roomInfoFromJson`, reached only from the room PAYLOAD handlers —
and every one of those emits `roomsChanged`.

So the predicate read counters that still said "clear" from before the
message, against a `lastActivity` that had just been raised to that very
message — and the "nothing newer than lastActivity" refusal was satisfied to
the millisecond by the row's own timestamp. **The first mention in any
already-read room was marked seen on arrival.** The room badge would say 1
and the bell 0: the same disagreement the change existed to remove, pointing
the other way and worse, because this one hides something real.

**A CONSUMER OF DERIVED STATE MUST LISTEN TO THE WRITER OF THAT STATE**, not
to whatever signal is named after the same noun. The check is mechanical:
grep every emit site of the signal you chose and confirm each one writes the
field you read. Three of the four `roomUpdated` sites in `RustSdkMatrixClient`
are in the timeline-event path; none writes an unread field.

**And three tests structurally could not see it.** Each handed the model an
already-updated `RoomInfo` and then emitted — so each one tested the fix's
intent rather than production's ordering. The case that catches it ingests the
row FIRST, against the stale pre-message `RoomInfo` with `lastActivity`
already at the row's timestamp, and only then emits. Same family as the
recorded "a policy test that invokes the policy function directly proves
nothing about whether production ever reaches it".

**A BACKEND THAT NEVER WRITES A FIELD IS NOT ANSWERING "no".** The same
predicate collapsed onto `notification_count` alone on the mock and the
experimental HTTP backend, because `hasUnreadMessages` and `markedUnread` are
never assigned there — so "every unread signal is clear" was true BY OMISSION.
`MatrixClient::tracksRoomReadState()` is the honest gate: false by default,
true only where someone actually answers. Sibling of the recorded
[[boolean-cannot-say-unknown]] lesson, in the negative direction.

**A LANDING IS ANNOUNCED NOW, because an absence is not a proof.**
`SfuMediaEngine::setTrackVolume` logged `participant volume had nowhere to
land` on a miss and NOTHING on a hit, so "the per-participant slider is not
cosmetic" could only ever be argued from silence — and silence is also what a
control nobody calls produces. `participant volume applied: wanted=<element>
percent=<n> gst=<factor> elements=<count>` is written only after
`g_object_set(element, "volume", …)` succeeded on a real element, at qCInfo
(a tester running `*.debug=false` would otherwise turn its absence into a
confident false FAIL), rate-limited per key on a REAL change. A landing also
clears that key's warned flag — ABOVE the rate limit, or a torn-down and
rebuilt bin silences the next genuine outage forever.

**A THREAD NOTIFICATION'S ROOT IS CONTEXT, AND CONTEXT MAY NOT PAGINATE.**
Reported as a notification click that "started scrolling backwards, I had to
stop it when it got to august". `jumpToEvent()` is allowed to spend
`kMaxNavigationBatches` (8) real backward paginations hunting for its target,
which is right for a message the reader asked for. B023's repair pointed a
thread notification at the thread ROOT — which the room timeline genuinely can
hold — and missed the PRICE: the destination is the thread panel, already open
on the same turn, while a root can be arbitrarily old. `revealIfLoaded()` takes
the row when the timeline already has it and does nothing when it does not —
not a failure either, since posting the unavailable notice for context nobody
requested would be a second lie. An ordinary notification is unchanged.
NOT live-validated, and the honest limit is stated: an old `eventId` on an
ordinary notification reaches the same 8 pages BY DESIGN, and that path is
deliberately left alone, so this may not be the whole of what the reporter saw.

**THE RENDERER NOW SAYS WHICH BACKEND IT GOT.** `src/main.cpp` probes for a
usable OpenGL context and falls back to `QSGRendererInterface::Software`; its
qWarning was the only observable, so a log with no such line was
indistinguishable from a log whose line was never written, and
`setGraphicsApi()` is a REQUEST rather than an answer. One startup line now
reads `rendererInterface()` once `sceneGraphInitialized` fires:
`lightning: scene graph backend=<name> software=<0|1> platform=<x>
refreshHz=<r> dpr=<d>`. The refresh rate rides along because it is what makes
a reported frame rate legible — an overlay claiming thousands of frames a
second against a 60 Hz panel is either not measuring presentation or the swap
chain is not throttling, and those are different investigations. Written for
the open "bouncing up to 9999 fps" report; it is an INSTRUMENT, not a fix.

**`scripts/gui-suite-calls.sh` IS THE CALL/SHARE/VOLUME CHECKS AS A SUITE, and
running it found two defects in itself that reading it did not.** Every
assertion is on evidence the layout cannot fake — an engine log line or a
value on disk — and it refuses to act on a process whose command line does not
name an isolated throwaway profile, fails rather than clicking blind when KWin
declines the pinned geometry, and proves cgroup ownership before restarting a
systemd unit.
- It refused to start on the machine it was written for: `no ImageMagick`. The
  GUI host has spectacle and no ImageMagick at all. A crop tool is the wrong
  shape for a hard precondition when no assertion depends on a picture;
  captures fall back to an uncropped full screen and say so.
- Its `share` check passed on a step that never happened. Clicking the
  portal's source tile CONFIRMS on this rig, so the dialog was gone before the
  Enter ran, `keypid`'s focus guard correctly refused to type into whatever
  had focus, and the check passed on its real assertions anyway. It asks
  whether the picker is still there now, and names the path it took.

**LIVE-VALIDATED PASS, two clients on the laptop rig** (AppImage
0.9.4+git, profiles A and B, `matrix.smetonis.net`): a two-party call with
clear-frame counters advancing in both directions on both clients; a screen
share that B RENDERS (`frames in the clear in video= true`, first encoded
frame at 108-152 ms, GPU scaling); per-participant volume at 0% muting with
the popup reading `0%`; 200% reached by dragging (plain clicks do not move
that slider) with the tooltip documenting amplification; and the value
surviving a full `systemctl --user restart` — `callVolumes\<hash>=200` on disk
and the popup still reading 200% after. **NOT TESTED: audibility.** Nobody
listened; what is proven is that the value reaches a real GStreamer `volume`
element.

**NOT live-validated in this round:** the bell's cross-device clear (needs a
second device reading a room the bell holds a row for), the thread-notification
scroll, and the renderer line's non-software branch.

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

