# Sync latency: how to measure it, and what is already known

Written for the reported defect *"messages come in Element and take about a
minute to come to Lightning, but sometimes."* Intermittent, never reproduced in
a harness, and therefore exactly the class of bug this repository has learned
not to guess at — three speculative scroll fixes were withdrawn or
shipped-and-regressed before the rule *instrument rather than guess* was
written down.

Nothing here claims the lag is fixed. One mechanism is identified by
inspection, its fingerprint is stated precisely, and the instrument that can
confirm or refute it is described.

## Enabling the trace

```sh
LIGHTNING_SYNC_TRACE=1 QT_LOGGING_RULES='lightning.sync.trace=true' \
  nix develop -c ./build-rust/matrix-client --backend=rust
```

`LIGHTNING_SYNC_TRACE=<ms>` (>= 100) overrides the 2000 ms slow threshold. One
variable drives **both** halves — the Rust stamp and the C++ tracer — so the
two can never disagree about whether they are recording.

Disabled, the whole facility is one relaxed atomic load per call site.

## What it records

Per event, `sdk -> bridge -> model -> ui` with the elapsed time between each
pair:

```
event id=41 room=9f2ac118 sdk->bridge=6ms bridge->model=1ms model->ui=12ms total=19ms
```

| Stage | Meaning |
|---|---|
| `sdk` | the Rust bridge enqueued the timeline diff (`crate::sync_trace_stamp_ms`) |
| `bridge` | the C++ poll drain took it off the queue |
| `model` | `TimelineModel` applied it — the row exists |
| `ui` | the GUI thread finished that event-loop iteration |

Two honesty notes. `model->ui` is a **proxy** for presentation, not a
frame-presented callback: it measures the delay before the GUI thread was free
again, which is the quantity that makes a message *feel* late. Per-frame timing
is `QSG_RENDER_TIMING`'s job. And a stage that was never observed reports `-1`,
never `0` — "we did not see this" and "it took no time" are different facts.

Plus sync-loop liveness, which is the line that matters most here:

```
SLOW sync gap=61240ms (a gap near 60000ms with no event journeys is the
sliding-sync dead-connection timeout: poll 30s + network 30s)
```

Recorded on a **drained event**, never on the poll timer — `pollRustEvents()`
runs every 100 ms whether or not the backend produced anything, so stamping it
there would measure the timer and report a healthy 100 ms gap straight through
a total outage.

## Privacy

Never message content, a body, a token, a key, a room id or a user id. A
journey carries an opaque correlation id and a **room key**: eight hex
characters of a SHA-256, enough to separate two rooms in one capture and
useless for identifying either — the same discipline the support-diagnostics
export uses. Encrypted rooms trace identically to unencrypted ones, because
nothing derived from the payload is recorded at all.

## The leading hypothesis, and why it is only that

matrix-sdk 0.18 issues the sliding-sync long poll with

```rust
RequestConfig::default()
    .timeout(self.inner.poll_timeout + self.inner.network_timeout)
    .retry_limit(3)
```

(`matrix-sdk-0.18.0/src/sliding_sync/mod.rs:531`), and both default to **30
seconds** (`builder.rs:54-55`). The request timeout is therefore **60 seconds**
— the reported duration, exactly.

The mechanism: a connection that dies *silently* — laptop sleep/wake, wifi
roam, NAT rebind, a server dropping the socket without a FIN — is
indistinguishable, from the client's side, from a server legitimately holding a
long poll open with nothing to report. So the client waits the full 60 s before
the request fails and the supervisor rebuilds. Anything Element delivered
during that window lands only when the new connection is up.

That explains every part of the report: the duration, the intermittency (it
needs a connection to die in a way that produces no packet), and why it is not
reproducible on demand.

**It is a hypothesis.** It has not been observed in a capture. The fingerprint
to look for is a `SLOW sync gap` near 60 000 ms with **no event journeys beside
it** — nothing was delivered during the wait. A gap with slow journeys in it is
a *different* problem (a backlog being applied), and the stage deltas say which.

**Lightning cannot currently shorten it.** `SyncServiceBuilder`
(`matrix-sdk-ui-0.18.0/src/sync_service.rs`) exposes `with_offline_mode`,
`with_share_pos`, `with_room_list_conn_id`, `with_room_list_timeline_limit` and
`with_parent_span` — and **neither timeout**. Changing it means patching a
pinned dependency, which is out of scope for a stabilization pass and would
need its own round.

Ruled out by inspection, so nobody re-checks them:

- the bridge poll timer is **100 ms** (`RustSdkMatrixClient.cpp`), not a minute;
- the supervisor's rebuild backoff is **3 s**, not a minute;
- the drain caps (64 soft / 256 hard per tick) bound work per tick, not
  delivery: 256 events per 100 ms is 2 560/s, so they cannot manufacture a
  60 s delay from any plausible backlog. A hydration-scale burst would show as
  slow `bridge->model` legs in the trace, which is the point of having them.

## If a capture shows something else

Read the stage deltas before proposing a fix:

| Signature | Where to look |
|---|---|
| `SLOW sync gap` ~60 s, no journeys | the SDK timeout above; needs an SDK-level change |
| `SLOW sync gap`, journeys present | a backlog arriving at once, not a stall |
| large `sdk->bridge` | the Rust event queue or the poll drain |
| large `bridge->model` | model application cost — correlate with `LIGHTNING_GUI_STALL_TRACE` |
| large `model->ui` | the GUI thread was busy; `QSG_RENDER_TIMING` next |
