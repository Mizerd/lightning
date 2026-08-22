// Opt-in sync-latency tracing (LIGHTNING_SYNC_TRACE).
//
// The reported defect this exists for: "messages come in Element and take
// about a minute to come to Lightning, but sometimes." Intermittent, never
// reproduced in a harness, and therefore exactly the class of bug this
// repository has learned not to guess at — three speculative scroll fixes were
// withdrawn or shipped-and-regressed before the rule "instrument rather than
// guess" was written down. This is the shippable instrument.
//
// # What it measures
//
// One event's journey, stage by stage, with the elapsed time between stages:
//
//   sdk        the Rust bridge enqueued a timeline change for this room
//   bridge     the C++ poll drain took it off the queue
//   model      TimelineModel applied it (the row exists)
//   ui         the delegate for that row was realised on screen
//
// A line is emitted when the journey completes, or when a stage is still
// outstanding after the slow threshold — so a stall REPORTS ITSELF rather than
// only showing up as a missing line.
//
// Sync-loop health is traced on the same switch, because the leading
// hypothesis for the minute-long lag lives there rather than in the stages
// above: matrix-sdk 0.18's sliding sync issues its long poll with
// `RequestConfig::timeout(poll_timeout + network_timeout)`, and both default
// to 30 s (matrix-sdk-0.18.0/src/sliding_sync/builder.rs:54-55, mod.rs:531).
// A connection that dies SILENTLY — laptop sleep/wake, wifi roam, NAT rebind,
// a server dropping the socket without a FIN — is therefore not noticed for
// 60 seconds, which is the reported duration. matrix-sdk-ui's SyncService
// builder does not expose either timeout, so Lightning cannot shorten it
// without patching a pinned dependency. `syncPollGap()` records the interval
// between sync responses so a capture can confirm or refute that mechanism
// instead of leaving it a theory.
//
// # Privacy
//
// Never message content, never a body, never a token, never a key, never a
// room or user identifier. A traced event carries a CORRELATION ID — a short
// opaque number minted per journey — and a room KEY that is a truncated hash,
// not a room id. Encrypted rooms are traced identically to unencrypted ones
// because nothing derived from the payload is recorded at all.
//
// # Cost when disabled
//
// One relaxed atomic load per call site. `enabled()` is read once at startup;
// every entry point returns immediately when off, allocating nothing.
//
// Enabling: LIGHTNING_SYNC_TRACE=1 (slow threshold 2000 ms) or
// LIGHTNING_SYNC_TRACE=<ms> with <ms> >= 100 to override it.
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace synctrace {

// True when the environment enables tracing. Stable for the process lifetime.
bool enabled();

// The slow-stage threshold in milliseconds. A stage outstanding for longer
// than this is reported as a stall.
int slowThresholdMs();

// Mint a correlation id for one event's journey and record the SDK stage.
// `roomId` is hashed and truncated before it is stored; it never appears in
// the log. Returns 0 when tracing is disabled, and every other entry point
// ignores an id of 0 — so a disabled build carries no branches beyond that.
// `sdkEpochMs` is the millisecond wall-clock stamp the RUST side recorded
// when it enqueued the change (0 = unknown, use now). Supplying it is what
// makes the sdk->bridge leg a measurement rather than an assumption.
quint64 beginEvent(const QString &roomId, qint64 sdkEpochMs = 0);

// Stage transitions. Unknown ids are ignored (a journey that began before
// tracing was installed, or one already reported).
void noteBridge(quint64 id);
void noteModel(quint64 id);
// Terminal: logs the whole journey with per-stage deltas.
void noteUi(quint64 id);

// Sync-loop health, independent of any single event.
//
// Called when a sync response is observed. Logs the gap since the previous
// one when it exceeds the slow threshold — the signature of the 60 s
// dead-connection mechanism described above is a gap of ~60 s here with no
// corresponding event journey.
void noteSyncResponse();
// Called when the sync loop reports a state change worth correlating
// (offline, retrying, running). `state` must be a literal, never server text.
void noteSyncState(const char *state);

// Test accessors. Counts and durations only.
int completedJourneys();
int reportedStalls();
qint64 lastJourneyTotalMs();
void resetForTest(int thresholdMsOverride);

} // namespace synctrace
