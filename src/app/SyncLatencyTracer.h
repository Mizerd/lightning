// Opt-in sync-latency tracing (LIGHTNING_SYNC_TRACE), for intermittent
// delivery lag that cannot be reproduced in a harness.
//
// Traces one event's journey with per-stage deltas:
//
//   sdk        the Rust bridge enqueued a timeline change for this room
//   bridge     the C++ poll drain took it off the queue
//   model      TimelineModel applied it (the row exists)
//   ui         the delegate for that row was realised on screen
//
// A line is logged when the journey completes or a stage exceeds the slow
// threshold. Gaps between sync responses are traced too: sliding sync's long
// poll times out after poll + network timeout (30 s + 30 s in matrix-sdk 0.18,
// not configurable through SyncService), so a silently dead connection shows
// up as a ~60 s gap.
//
// Privacy: no content, tokens, keys or Matrix ids; only a per-journey
// correlation id and a truncated hash of the room id.
//
// Disabled cost: one relaxed atomic load per call site.
//
// Enabling: LIGHTNING_SYNC_TRACE=1 (slow threshold 2000 ms) or
// LIGHTNING_SYNC_TRACE=<ms> with <ms> >= 100 to override it.
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace synctrace {

// Stable for the process lifetime.
bool enabled();

// A stage outstanding longer than this is reported as a stall.
int slowThresholdMs();

// Mint a correlation id and record the SDK stage. `roomId` is stored only as a
// hash. Returns 0 when disabled; every other entry point ignores id 0.
// `sdkEpochMs` is the Rust side's wall-clock enqueue stamp (0 = use now).
quint64 beginEvent(const QString &roomId, qint64 sdkEpochMs = 0);

// Unknown ids are ignored.
void noteBridge(quint64 id);
void noteModel(quint64 id);
// Terminal: logs the whole journey with per-stage deltas.
void noteUi(quint64 id);

// Logs the gap since the previous sync response when it exceeds the slow
// threshold.
void noteSyncResponse();
// `state` must be a literal, never server text.
void noteSyncState(const char *state);

// Test accessors. Counts and durations only.
int completedJourneys();
int reportedStalls();
qint64 lastJourneyTotalMs();
void resetForTest(int thresholdMsOverride);

} // namespace synctrace
