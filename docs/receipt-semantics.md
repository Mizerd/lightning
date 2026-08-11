# Read-receipt semantics and known loss mechanisms

Status as of the 2026-08-11 media/UX round. This documents what the
"receipts disappear / swap between people" report can and cannot be, with
the code evidence, so a live capture can be matched against a mechanism
instead of guessed at.

## The pipeline (verified correct)

```text
matrix-sdk-ui timeline (owns receipts)
  -> EventTimelineItem::read_receipts()
  -> rust/src/timeline.rs read_by_json (newest-16 window + uncapped total)
  -> timeline_diff "set" JSON
  -> RustSdkMatrixClient poll drain (100 ms ticks)
  -> RustTimelineIngest (wholesale row replacement, no merge)
  -> TimelineModel ReadReceiptsRole (excludes self + row sender, sorts
     newest first, resolves names/avatars via the member cache)
  -> MessageDelegate readReceiptStrip (4 chips + "+N", identity-guarded)
```

Lightning-side invariants are pinned by
`TimelineModelDiffTest::twoReadersAdvanceIndependentlyWithoutLoss` (two
readers advancing through the exact adjacent Set pairs the SDK emits,
across pagination inserts and member hydration),
`RustTimelineIngestTest::readReceiptsMoveBetweenRowsViaSet`, and the
`TimelinePaneQmlTest` receipt-chip suites.

## Intended semantics that read as "disappearing"

A receipt move is `Set(old row)` then `Set(new row)` — the SDK associates
each user's receipt with the newest visible item at or before their read
position, so advancing legitimately REMOVES the chip from the older row.
Two consequences are by design (and match Element):

1. **Sending hides your receipt.** The SDK synthesizes an implicit receipt
   for every event's sender; the model excludes the row sender's own
   receipt. When user U sends a message, U's receipt moves onto U's own
   message and is therefore not rendered anywhere until U reads someone
   else's newer message. In a two-person room this looks exactly like "the
   chips swap between us".
2. **The 16-receipt window.** Rust serializes the newest 16 receipts per
   row plus an uncapped total; on a crowded row a specific person can be
   legitimately absent from the chips while counted in "+N".

## Lightning-side mitigation landed this round

The poll loop drained at most 64 events per 100 ms tick. A receipt move's
two Sets are adjacent in the queue; a tick boundary between them painted
the removal a full tick before the addition — a visible flicker
indistinguishable in a capture from a real loss. The drain now extends
past the soft cap while timeline diffs keep coming (hard cap 256), so one
structural transaction normally lands in one tick
(`RustSdkMatrixClient::pollRustEvents`). This is a bounded MITIGATION
established by code reading, not a reproduced-and-retested fix: the split
needs a ≥64-event tick (a burst condition), there is no harness that
drives the FFI poll loop, and a pair straddling the 256-event hard cap is
still split. It narrows one Lightning-side window; it does not claim to
be the cause of the live report.

## SDK-internal loss mechanisms (matrix-sdk-ui 0.18.0, confirmed in code,
## not fixable in Lightning without patching the pinned dependency)

Line references below are against the published matrix-sdk-ui 0.18.0
crates.io sources (`src/timeline/controller/read_receipts.rs`), which are
not vendored in this tree — re-verify against the local cargo registry
copy before relying on an exact line.

- `read_receipts.rs:476-505` — `add_new_receipt` silently drops the
  addition when the target item is not found in the remotes region, is a
  virtual row, or is a **local echo** (local items structurally cannot
  carry receipts). The removal has already been applied: the user's chip
  vanishes everywhere until their receipt next moves.
- `read_receipts.rs:758-763` — a visibility-change recomputation
  short-circuits on equal receipt COUNTS: one reader added and another
  removed in the same recomputation leaves the row's stale set rendered —
  a literal avatar-swap mechanism.
- `read_receipts.rs:417` — the mirror image: the removal can fail while
  the addition succeeds, rendering the same user on two rows.

To capture these live, run with:

```sh
RUST_LOG=matrix_sdk_ui::timeline::controller::read_receipts=debug \
  nix develop -c ./build-rust/matrix-client --backend=rust
```

and look for "inconsistent state: new event item for read receipt was not
found" / "received a read receipt for a local item". Lightning's own
count-only diagnostics ride the `matrix.receipts` logging category
(`QT_LOGGING_RULES="matrix.receipts.debug=true"`), logging receipts
before/after per Set — counts only, never user ids or bodies.

## Queue-overflow corruption (bounded, detectable)

The Rust event queue caps at 4096; overflow drops the OLDEST events and
posts a `queue_overflow` marker (surfaced as an error banner). Dropping a
Set pair's removal half yields a duplicate chip; dropping an
insert/remove desynchronizes the C++ index mirror until the room timeline
reopens. If duplicate avatars are ever reported, check for that banner
first.
