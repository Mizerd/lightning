# Timeline scrolling

**MOVED OUT OF CLAUDE.md §16 on 2026-09-18** at 141,409 characters — the sixth
move, for the reason all six happened: past roughly 140,000 that file's own
tail starts heading for a 150,000 cliff where it truncates SILENTLY and §§17-19
vanish from agent context. Nothing was deleted; the whole block is below
unchanged.

It was the block chosen because it is the single largest in §16 and the most
self-contained: five rounds, three reverted fixes, a refuted-hypothesis table
that is binding, and the measurements behind every one of them. READ IT BEFORE
TOUCHING TIMELINE SCROLLING — its refutations must not be re-proposed without
stating which claim was refuted and whether yours is the same claim.

**Timeline scrolling — read this whole block before touching it.** Five
rounds, three reverted fixes, several measurement errors of my own.

*Refuted hypotheses. Do not re-propose any of these.*

| Hypothesis | Refuted by |
|---|---|
| State events are the cost | state rows ~8x CHEAPER per notch than ordinary messages; page inserts flat (~6 ms) out to 1063 rows |
| GPU fill-rate at 4K | `render` = 1-4 ms on every slow frame |
| Clipping breaks batching | same capture — render is never the cost |
| De-layouting MessageDelegate's nested ColumnLayout/RowLayouts | `perf record` named `QQuickItemPrivate::transformChanged` at 19.2% of cycles and `polishItems` at 1.0%. Buried twice; do not revive |
| The anchoring machinery displaces readers | every anchor counter zero on every line of every live capture (`anchorCorrections=0 displacedFirings=0 prependFirings=0 unresolvedId=0 evictedNoInsert=0`) |
| Pagination teleport | did not reproduce 2026-08-12; structurally impossible now (positive-only guard, below) |
| `worstNotchMs` measures frame cost | it times the wheel HANDLER, never the frame: 0-2 ms live while frames cost 14 ms. It once wrongly retired the row-window idea |
| Offscreen per-notch cost transfers to hardware | offscreen uses the software rasterizer where `syncSceneGraph`/`updateDirtyNode` dominates: 10.65 ms/notch offscreen at 1000 rows vs 0-2 ms on the GPU |

**Offscreen perf numbers are not the user's numbers.** Any scale-with-N
result measured offscreen must be confirmed on hardware before anything
is built on it.

*Measured.* Frame cost tracks TOTAL instantiated rows, not what is on
screen. `QSG_RENDER_TIMING=1` on Rokas's GPU, pagination frames
excluded: ~108 rows = 3 ms median frame (1% over 16 ms); ~916 rows =
14 ms (46% over, polish 5.6 / render 8.3). Cost is CPU-side polish+sync
on the GUI thread; `render`/`swap` are negligible. Residual accepted:
~60-140 ms per pagination page (`perRowMs` 3-7, ~18-20 rows a page),
paced by `ReverseListProxyModel` at 3 ms per tick.

*Shipped 1 — never-laid-out empty `Text` items (`d1ddc2f`).* Every
`QQuickText` is BORN carrying `ItemObservesViewport`
(`QQuickTextPrivate::init`). The ONLY code that clears it is
`QQuickText::setText`, which opens with `if (d->text == n) return;` —
*before* its `setFlag(ItemObservesViewport, n.size() > 10000)` line, so
a binding that keeps producing the same empty string never clears it.
**Visibility is never consulted** (an earlier revision wrongly blamed
invisibility; correlated, not causal).
`QQuickItemPrivate::transformChanged` only switches off its per-subtree
walk once NO descendant observes the viewport, so a few such Labels per
row made Qt walk the whole instantiated tree on every `contentY` change:
3000 observers across 1000 rows. Fixed with seven `Loader`s in
MessageDelegate.qml and ThreadSummaryCard.qml (whose `timeLabel()`
returns `""` without an SDK timestamp, so the hazard lives inside a live
card too). Observers 3000 → 0; offscreen per-notch 33.89 → 10.39 ms at
n=1000. Felt improvement on a real desktop: **NOT TESTED**.
**GENERALIZE: in a per-row delegate, a `Label` whose text can be `""` in
the state it is created in belongs in a `Loader`** — including labels
reading message fields, which are ALL empty on a virtual
date-divider/read-marker row. Single most expensive QML mistake known
here. `timelineRowsCarryNoPermanentViewportObservers` walks the real
item tree and requires ZERO (139 on the pre-fix tree). Unmeasured
follow-up: `continuationTimestamp` churns one Label per row crossed on
hover; the alternative costs a text layout per row at load.

*Shipped 2 — decoded image size (`6ca9d99`).* `MediaImageProvider`
ignored `sourceSize` on every timeline image: rows ask for
`sourceSize.width: 640` with height 0 (the documented keep-the-aspect
idiom) and the provider gated on
`requestedSize.isValid() && !requestedSize.isEmpty()` — but
**`QSize::isEmpty()` is true when EITHER axis is below 1**, so a
width-only request read as "no size asked for" and decoded at FULL
resolution. Fixture: 3.84 Mpx → 0.27 Mpx. Upscaling is still honoured
when a shape is BAKED IN (an avatar mask rasterizes once) and refused
otherwise. **NOT TESTED** live.

*Shipped 3 — speculative media waits for a settle.* A live capture of
one 15 s upward gesture (442 wheel events, ~45 pagination pages, 19 →
813 rows) showed ~120 MB of video pulled because every row that merely
SWEPT THROUGH the on-screen band armed a full-payload prefetch, each
completion writing its temp file synchronously on the GUI thread. ONE
gate, `speculativeMediaAllowed: !userScrollActive`, consulted by the
video payload prefetch, the video POSTER path (which prefetches
internally via `videoPosterSource` → `prefetchPlayable`, so gating only
the obvious call site leaves half the traffic) and the audio card.
**Thumbnails are deliberately NOT gated**: small, and they are what the
reader is looking at.

*Shipped 4 — jump-to-live history trim (`f40da33`).* The un-virtualized
Column instantiates every paginated event permanently. **Lightning
implements no unloading of its own**: matrix-sdk 0.18 already does it —
`RoomEventCache::subscribe()` bumps a `subscriber_count`, and at zero
`auto_shrink_linked_chunk_task` calls `shrink_to_last_chunk()`; this
round adds only ORDERING. **`abort()` only REQUESTS cancellation**, so
the old task still owns the `Arc<Timeline>` holding the count up;
`await_event_cache_shrink` awaits that handle (a `Cancelled` join IS the
success signal), bounded so a slow task degrades to no-trim. **Never
`RoomEventCache::clear()`** — it wipes PERSISTED events, forcing even
the live tail to be refetched. Policy is a PURE predicate
(`AppController::historyTrimAllowed`) so every clause is testable: Rust
backend, room open, not mid-pagination, **no thread panel or Threads
view open**, >400 loaded rows. The thread clause is load-bearing twice:
a thread timeline holds its OWN event-cache subscriber (so the shrink
could not fire) and the reload would tear its live subscription out from
under an on-screen panel. ONE call site, contract-pinned: the FAR branch
of `goToLatest()` — wiring it to scrolling or pagination would reset a
reader's timeline out from under them — committing (`stickToBottom`,
`saveFollowingLatest`) ONLY on a real dispatch success. Deliberate side
effect: `onModelReset` now closes the row-anchored surfaces on EVERY
reset, including the same-room recovery reload. **LIVE-VALIDATED PASS**
(`cachedBefore= 1083 released= true reloadedItems= 19`). The payload
carries both `trimmed_from` and `trim_shrunk` so a timed-out wait cannot
look like a successful trim, and the baseline must be sampled BEFORE the
release or a fast shrink makes a genuine trim report `released=false`.
`await_event_cache_shrink` has NO automated coverage at any layer (no
mock-room harness in `rust/`).

*Shipped 5 — the row window (`b74b518`, made live by `7092eab`).*
`ReverseListProxyModel` carries a window: `windowSkip` (how many of the
NEWEST source rows are excluded) plus the exposed count, every
transition a single insert-or-remove at ONE end — never a reset, never a
mid-list renumbering. **`windowSkip == 0` is the only state in which
proxy row 0 is the live edge**, so the pane must return to 0 before the
reader can reach the bottom. Policy (`applyRowWindow`,
TimelinePane.qml): `windowRunwayRows` 220 below the reader (≈30
viewports vs a largest observed gesture of ≈7.5 — that runway is the
strand-prevention), `windowMarginRows` 120 above, never below
`windowMinRows` 320, move only for a change of 40+ rows. **Applied ONLY
from the scroll-settle timer** — no structural change mid-gesture, which
is what sank the reverted bounded retained window. With a window active
`atBottomEdge()` returns FALSE so follow-latest cannot latch onto a
false newest message. Correction on a newest-end release is ONE exact
write: sum the MEASURED heights of the released rows (the Column has no
spacing, so a plain sum is exact) and subtract from contentY. A deferred
`Qt.callLater` snap-by-anchor-id was tried and REMOVED — it runs BEFORE
the Column relayout, reads the anchor's stale y and clamps against the
new shorter content, dumping the reader at the top. Do not add a second
correction path alongside `maintainViewAnchor`. Silent failure modes:
- **A skip change renumbers every view row.** View-row helpers must
  subtract `rowWindowSkip` or every jump/search/anchor-restore resolves
  to "no such row" and does nothing. Four instances; none threw.
- `releasePendingRows()` must clear the WINDOW, not just the pacing cap
  (`releaseAll()` lifts `m_windowCap`, leaves `m_windowSkip`).
- Live-edge paths must RESTORE the live edge: `wheelMinY()` under a
  window is a synthetic newest edge, so `goToLatest()` glided to a
  message that was not the latest. It now refuses the glide while a
  window is held, and `settleAtLatest()` calls `releasePendingRows()`
  itself rather than trusting the trim's model reset.
- `revealNextChunk()` bounded its release loop on `sourceRowTotal()`
  instead of `revealTarget()` (`9adcdc9`): one tick released straight
  through the cap — 230 rows against a cap of 60. Only bites once the
  exposed count drops below the cap.
- **Thrash guard, thresholded on the ENTER band.** Trimming the OLDEST
  end shrinks `contentHeight` and so `wheelMaxY()`, and
  `distanceFromTop()` is `wheelMaxY() - contentY`, so a trim moves the
  reader's measured distance from the top with no visible movement while
  `applyRowWindow()` ends in `updateStickAndPaginate()` — dispatching a
  backfill that regrows what was released. TALL-VIEWPORT only (fixed
  ~4020 px kept margin vs a `2.5 * height` enter band): a test at 420 or
  1400 px passes on broken code, so the suite uses 2160 px. Thresholding
  on `nearTopExitDistance` OVER-fires — that is hysteresis for a reader
  already in the band; what dispatches is crossing INTO it.
- **The window's old edge re-exposes LOCAL rows; it does not ask the
  server**, or the window creates a stall where none existed. It rides
  the proxy's paced reveal (`extendWindowAtOldEnd`); a synchronous
  `setWindow(skip, count+120)` would build 120 delegates at once,
  360-840 ms. It deliberately does NOT consume `nearTopArmed`.
- Acceptance: `rowWindowBoundsRowsWithoutMovingTheReadersMessage`
  (900 → 376 rows, the reader's own event moves 0 px, bar 2 px, both
  directions) plus three review-driven cases including
  `rowWindowTrimNeverFeedsTheNearTopPaginationBand`.

*It shipped as a PERMANENT NO-OP and a live capture caught it.*
`userScrollActive: moving || wheelAnimating || scrollSettleTimer.running`
and `applyRowWindow()`'s only caller is `scrollSettleTimer.onTriggered`,
where that timer still reads as running — so `if (userScrollActive)
return` was UNSATISFIABLE at the one call site that exists. Fixed with
`viewportMotionActive` (`moving || wheelAnimating`); `userScrollActive`
is left alone because the speculative-media gate deliberately includes
the settle tail. **GENERALIZE: a policy test that invokes the policy
function directly proves nothing about whether production ever reaches
it.** The policy was covered six ways and the trigger not at all;
`wheelScrollingIntoHistoryEventuallyBoundsRowsThroughTheSettleTimer` now
drives real wheel notches and waits, calling nothing. The window had
ZERO observability, which is how it shipped unnoticed: the gesture trace
now carries `srcRows`, `winSkip`, `winApplies`, and `rows == srcRows`
with a deep reader, or `winApplies=0`, is the signature.

*The row window FIRES in production — first evidence, 2026-08-29.* A live
`LIGHTNING_SCROLL_TRACE=1` capture on the maintainer's desktop, scrolling
deep into a real room, produced `winApplies=1 winSkip=380 rows=255
srcRows=635` with `dContentH=-22111`. So the window does bound the
instantiated set on a real account, which had never been observed before —
`rows` really is much less than `srcRows`. What is STILL unproven is the
FRAME COST half: that capture carries `worstNotchMs` (0-1 ms throughout,
which times the handler and not the frame), not `QSG_RENDER_TIMING`, so it
says the mechanism engages and says nothing about what it saves. The judge
for that remains a `QSG_RENDER_TIMING` capture with `winApplies` > 0 and
median frame cost deep in history falling toward 3 ms.

*And the anchor machinery came back CLEAN in that same capture.* Eleven
gestures, up to 385 events each, 635 rows: `displacedApplied=0`,
`prependFirings=0`, `unresolvedId=0` and `evictedNoInsert=0` on every line.
`materializedMaxAbsDelta` was non-zero twice (84 and 221) — but
`activeDeferrals` EQUALS `materializedFirings` on every line and
`activeDeferredSum` equals the delta, so every one of those was deferred
and none reached an active gesture. The single `anchorCorrections=1` sits
on a gesture with `netY=0` and `stick=1`: an idle stuck-to-bottom restore,
which is the designed path. Non-zero counters are not automatically a
failure — read `activeDeferrals` beside them before concluding anything. The window
only acts when SETTLED, so it does not help during the long upward
scroll itself. Also open: which stall category (`row-reveal`,
`image-decode`, `timeline-diff`, `timeline-reset`) owns the logged
333/369/1062 ms GUI stalls — do not guess a fix before that line exists.
`writePlayableFile` still writes up to 32 MiB synchronously on the GUI
thread (unmeasured).

*The positive-only anchor guard: do not "fix" it.* The timeline is a
rotated `Flickable` + `Column` (`qml/TimelinePane.qml`), not a ListView:
`contentHeight` is the exact sum of real rows and `originY` can never
move. View row 0 is the NEWEST message at content y 0
(`sourceRowForViewRow = count-1-row`), so backward pagination lands at
HIGHER view rows and higher content y, past the reader; a prepend does
not change the anchor row's index or y and the displaced branch is never
reached. The only insertion that displaces a scrolled-up reader is a
LIVE message at view row 0 — a genuinely positive `grew` the guard
already applies. Three attempts failed here (two withdrawn in review;
the staging/freeze window `225c7b3` shipped, regressed and was removed
in `263268b`). A fourth needs a `LIGHTNING_SCROLL_TRACE=1` capture
naming a failure: a non-zero `displacedApplied`, `anchorCorrections` or
`materializedMaxAbsDelta`. All-zero lines are not evidence.
**AND THE SUITE'S ANCHOR FLAKE WAS ITS FIXTURE, NOT THIS MACHINERY
(2026-09-11, root-caused and FIXED).** Disabling `maintainViewAnchor()` fails
EIGHT cases while the three prepend/anchor ones PASS, so a MISSING correction
never failed them. And with the counters ON, a pass and a fail of one case
differ BEFORE the prepend under test: `offsetBefore=+334` (row y 723,
contentHeight 2231) vs `-389` (row y 0, 2115) — one row short, so the anchor
was captured on a different row. `!pagination()->busy()` is not "the timeline
stopped growing" — `ReverseListProxyModel` paces its reveal — so it captured
mid-growth. **Wait on the PRODUCER**: `revealIdle()` is the same condition the
proxy stops its own timer on. ~1 failure in 5 became 0 in 24 on a first
(heuristic) fix and 0 in 6 idle plus 0 in 6 under 24-way CPU load on this one.
Do not time the poller instead: `kRevealBudgetMs` is a per-tick WORK budget
and the interval is 16-250 ms and ADAPTIVE, while `qWaitFor` polls every
~10 ms, so "N quiet reads" silently stops meaning anything once rows get
expensive — which is the loaded machine a flake appears on.
**THE COUNTERS ARE GATED ON `LIGHTNING_SCROLL_TRACE`** — every `diag*`
increment is inside `if (scrollTrace)`, read once per controller — so a test
reading them without setting it gets zeros on any build. A first version of
this capture did that, and "every counter zero, so the machinery never ran"
was written here on it: a dead instrument, not a measurement. (`PrependFirings`
is 0 in a passing run too, so these three do not exercise that branch despite
their names.)

*Element (classic) was read for this and does NOT animate.*
`ScrollPanel.scrollToBottom()` is a bare `scrollTop = scrollHeight`;
`TimelinePanel.jumpToLiveTimeline()` builds a NEW `TimelineWindow` at
the live edge and DISCARDS everything paginated. Its height-based
unfilling (`UNPAGINATION_PADDING = 6000`,
`UNFILL_REQUEST_DEBOUNCE_MS = 200`, relative `scrollBy` from a tracked
node's `offsetTop`) works because DOM removal is nearly free — exactly
why Lightning's bounded-retained-window attempt was reverted (no felt
improvement, a follow-up cap made the app FREEZE, and its
width-invalidation injected anchor calls on every resize into the
machinery three fixes were reverted from). Incremental unfilling while
scrolling remains deliberately NOT done.

## The history fill, pagination and the row window

**MOVED OUT OF CLAUDE.md §16 on 2026-09-20** at 139,505 characters — the
seventh move, for the reason all seven happened. These blocks were already
about this file's subject; they were simply never here. Nothing was changed
in them.

**A ROOM OPEN COSTS THE HISTORY FILL'S PAGE COUNT TIMES ~400 MS, AND
`maxInvisibleFillRetries` IS THAT COUNT'S CEILING (measured 2026-09-05).**
`a5e64a6` raised the no-progress budget 8 -> 60; in a call room whose tail is
RTC membership churn that meant 9 fill pages on a first open (4.2 s) and 18 on
a re-open (11 s) — each page ~70 ms dispatch + network + 100-250 ms of ingest
and the fill's own timers. Reported as "ten seconds to load a room". It is 12
now, and a wheel towards older history on content too short to scroll asks for
the next page, so the reader is never stuck behind a collapsed group either.
The fill also decides how many rows a room holds after open, which IS the
scroll frame cost (~14 ms at 900 rows): a bigger budget makes scrolling worse
too. Detail in `docs/round-history.md`.

**PAGE DOUBLING WAS TRIED THE SAME DAY AND REFUTED BY THE SAME LOG.** Asking
for 100 events after an invisible page: Synapse answered a 100-event
/messages in 1.5-1.8 s (17 ms per event) against 110 ms for 20 (5.5 ms
per event), the fill overshot to ~600 rows, and the re-open went from 4 s
to 11 s. Do not re-propose bigger pages as the answer to the fill; fewer
pages is.

**MATRIXRTC MEMBERSHIP STATE IS FILTERED OUT OF EVERY TIMELINE AT THE SDK
(`lightning_event_filter`, rust/src/timeline.rs).** A call re-publishes one
`m.call.member` (msc3401 / msc4143 / stable) state event per participant per
MINUTE, so a room that hosts calls carries thousands; every one was a timeline
item — paginated twenty at a time, ingested as a hidden activity row,
instantiated as a delegate, counted by the fill. The maintainer's key
observation (2026-09-05): ONE room lagged and every other opened instantly.
Nothing on screen needs them (the "started a call" row is the notification
event; the call UI reads membership from room STATE). The controller's
no-progress budget is 12 empty pages (was 2) so the fill can walk a churn
run; the pane's row cap bounds what is inserted.

**THE FILL IS BOUNDED BY ROWS, NOT ONLY BY INVISIBLE PAGES
(`maxViewportFillRows`, 240).** The invisible-page budget cannot bound a room
whose every page adds a little height — a collapsed activity run growing by a
line per page, one visible message per twenty hidden ones — because each such
page counts as progress and resets it. Measured 2026-09-05: a re-open ran 32
pages and 600+ rows in 6.4 s with the page budget never tripping. Every row
is a delegate in the un-virtualized Column: that instantiation is the
"freezes for five seconds", and the same 600 rows are the one-second stall
when the row window releases them all at the live edge. 0.8.3 stopped at
eight pages, ~160 rows; the cap restores that scale and the reader's scroll
loads the rest a page at a time.

**THE FILL LOOP MUST FOLLOW THE CONTROLLER, NOT ITS RETRY TIMER.** Rows
land before `PaginationController` finishes a batch, so the fill check they
trigger finds `busy` true and waits on the 250 ms retry timer — once per
page. With every page served from the event cache in ~1 ms, that timer WAS
the room-open time (14 pages, 4.4 s, timestamped log 2026-09-05). The pane
re-checks the fill on the controller's `stateChanged` when it is idle.

**THE MEDIA BAND IS AN INDEX RANGE THE ROW LOADER ASSIGNS, exactly like
`rowOnScreen`, AND IT NEVER CLOSES ON THE NEWEST SIDE.** A picture loading
late in a row between the reader and the live edge grows below the reader
and moves them; rows older than the reader grow away from them. Only history
beyond 2.5 viewports waits. Its first cut compared the delegate's own `y` against
content-coordinate bounds — and inside the per-row Loader a delegate's y is
always 0, so every row was "in band" at the newest end (no saving) and no row
was deep in history (no picture ever loaded there). The pane computes
`mediaBandFirstRow/LastRow` with `viewRowAtContentY` at discrete moments and
the Loader sets `mediaInBand` on the delegate; the delegate's own default is
permissive for hosts without a band.

**A LOG LINE THAT CANNOT TELL "NOTHING HAPPENED" FROM "WE THREW EVERYTHING
AWAY" IS NOT A LOG LINE.** A room open made fourteen back-paginations that each
reported `added= 0`, and nothing anywhere could say whether the server returned
nothing or the timeline filter had discarded a full page — `paginate_backwards`
returns a bare `bool` and matrix-sdk-ui drops `BackPaginationOutcome.events` on
the line that tests it. Lightning's own filter is the only place that sees
every raw event AND knows why it said no; it counts now, and one instrumented
run answered it outright (`filterOffered= 240 droppedRtc= 240` — twelve pages,
100% MatrixRTC churn). **Four handling defects hid behind that silence**: a
room asserted its own emptiness after ONE empty page
(`m_initialHistoryHasSucceeded` had no `inserted > 0` test, so
`timelineEmptyState` rendered "No messages here yet" over full history); the
fill gave up at 8 because a page that adds NO rows spends
`maxViewportFillRetries`, never the `maxInvisibleFillRetries` the 2026-09-05
round raised for exactly this case; every empty page paid a 250 ms settle for
rows that could not arrive; and `requestNearTop()`'s redirect swallowed the
user's gesture once the fill had stopped. Detail in `docs/round-history.md`,
2026-09-15 (afternoon).

**AND A PAGE-SIZE ESCALATION IS NEARLY INERT — do not record it as the fix.**
`matrix-sdk`'s `load_more_events_backwards` returns ONE STORED CHUNK at a time
and never consults `batch_size`; that parameter only reaches the wire when the
walk hits a network gap. A filtered run is therefore local disk reads, which is
why the 250 ms settle dominated and not the fetch. (It is also NOT the page
doubling §16 refutes: that measured unconditional 100-event pages on rooms
whose pages ADD ROWS, and both harms it found need rows.) Related: the
sliding-sync room list runs at `DEFAULT_LIST_TIMELINE_LIMIT = 1` and any
`limited` response shrinks a room's cache to its last chunk, so `items= 0`
versus a healthy room's `items= 2` (one event plus its date divider) is that
residue, not an empty room.

**AND THE BOUND THAT REPLACED THAT SILENCE WAS WRONG BY EXACTLY ONE MESSAGE
(2026-09-16).** The fix above keyed "keep walking a filtered run" on
`eventCount() == 0`. That is a PROXY for the reader's actual condition — *is
the viewport full?* — and the maintainer's next report was the same room one
message later: "in this room only a single image loads and I have to scroll up
for anything else to appear." One loaded image made `eventCount()` non-zero, so
the room got the ordinary twelve. **A bound keyed on a proxy for the user's
condition is wrong by exactly the difference between them**; when the honest
criterion cannot be read where the decision is made, derive it and say so
rather than taking the nearest readable thing.

**The terminator was in QML, and no previous round had it in frame.** The log
showed NINE dispatches and stopped; neither controller bound can produce a nine
(both are twelve), and `TimelinePane.qml`'s `maxViewportFillRetries` is EIGHT,
plus one from `requestNearTop()`'s redirect, which does not spend the pane's
counter. That counter's real subject is "the dispatch went nowhere" — and a
page the filter emptied looks identical to it from QML (zero rows, zero pixels)
while meaning the opposite: the cursor walked twenty real events towards the
first message beyond the churn. **Two observations that are identical at the
point of measurement are not one event.** Filtered pages now spend their own
budget (`viewportFillEmptyPages` / 60, matching `kMaxFilteredRunStrikes`),
which is affordable precisely because a page that inserts nothing instantiates
no delegates; `maxViewportFillRows` (240) still bounds everything the fill puts
on screen.

**AND THE MOCK COULD NOT EXPRESS A FILTERED PAGE AT ALL, WHICH IS WHY IT
SHIPPED TWICE.** `setPaginationChunkForTest({})` falls through to three
synthetic events (`if (!m_paginationChunkOverride.isEmpty())`), so EVERY mock
page had always added rows — the one pagination shape that matters most here
had no reachable fixture at the QML layer. `setFilteredPaginationPagesForTest`
is that fixture now. GENERALISE: before concluding a defect is untestable,
check whether the harness can even REPRESENT the input. Detail in
`docs/round-history.md`, 2026-09-16.

## Touchpad input: a zero-pixel frame is not a notch (2026-09-23)

**Reported:** scrolling "feels quite bad" on the laptop (Flathub 0.9.9, KDE
Plasma 6.7 Wayland, Lunar Lake iGPU, touchpad with KDE `ScrollFactor=0.1`).

**Refuted first, with measurements on that laptop.** Software rendering:
`scene graph backend=opengl`, `Mesa Intel(R) Graphics (LNL)`, threaded render
loop. Frame cost: `QSG_RENDER_TIMING` during wheel motion 59.7 fps, frame
interval p50 16 / p99 18 ms, polish <= 6 ms, render thread <= 17 ms (the vsync
swap). Package format: the AppImage (Qt 6.8.2) shows the identical signature
to the Flatpak (Qt 6.11.2). Neither is the cause. Large-room frame cost was NOT
measured there (no long room on the test accounts).

**The cause is the input mapping.** Qt Wayland (identical code in 6.8.2 and
6.11.2, read from source) turns each finger-scroll frame into
`pixelDelta = round(delta + carried remainder)` AND `angleDelta = delta * 12`.
A slow finger, or any ScrollFactor below 1, sends sub-pixel frames: an 8 mm/1 s
swipe on this machine is `wl_pointer.axis` 0.121 px per frame, so most frames
arrive as `px=0 ang=±1`. The WheelHandler routed every `pixelDelta == 0` frame
to the NOTCH glide, i.e. `|ang|/120` of a notch (a third of the viewport),
animated — and the next `px=±1` frame cancelled it. Captured with
`LIGHTNING_SCROLL_TRACE=1` on the shipped Flatpak, gestures replayed through a
uinput clone of the touchpad (same bus/vendor/product/name, so KWin applies the
same ScrollFactor):

| Gesture | trace line | moved |
|---|---|---|
| brisk 20 mm / 300 ms | `events=34 pixel=34 angle=0` | 33 px |
| slow 8 mm / 1000 ms | `events=105 pixel=13 angle=92` | 312-342 px |

A slower, shorter swipe travelled ten times farther, in glide-stop-glide jerks.
**`angle > 0` beside `pixel > 0` in ONE gesture is the signature** to look for
in any future trace.

**Fix:** a frame with a scroll `phase` (continuous source) is pixel input even
when its pixelDelta is 0 — it moves nothing, and Qt's remainder carries it.
**`phase` is the discriminator, NOT `event.device.type`**: on a seat with
pointer gestures Qt Wayland labels a plain mouse WHEEL `PointerDevice.TouchPad`
as well (measured: a ydotool wheel arrives `dev=4 phase=0`). A wheel is always
`NoScrollPhase`, and so are X11 and Windows touchpads, which keep their path.
Also, Qt drops a finger frame outright when `angleDelta` rounds to 0
(`|delta| < 1/24 px`) — upstream behaviour, nothing here can recover it.
`ThreadPanel.qml` and `SmoothWheelArea.qml` carry the same routing.
