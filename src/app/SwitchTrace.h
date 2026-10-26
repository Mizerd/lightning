// Opt-in account-switch tracing (LIGHTNING_SWITCH_TRACE), Phase 0 of the
// concurrent live accounts work: it measures where today's switch time goes
// before anything is changed to make it faster.
//
// One line per switch, category lightning.switchtrace, at info:
//
//   switch-trace id=N path=cold outcome=ok from=<slug> to=<slug>
//     s1..s11=<ms|-1> s6_runs=K
//     rust_discovery_ms rust_store_open_ms rust_store_opens
//     rust_fallback_build_ms rust_offline_fallback rust_restore_ms
//     rust_resume_unsent_ms rust_restore_total_ms restore_poll_ms
//     reset_rooms rust_sync_build_ms rust_since_start_sync_ms
//     rust_reset_serialize_ms reset_poll_ms reset_parse_ms model_ms
//     first_frame_ms room_interactive_ms logged_in_flips
//
// (written on one physical line). -1 means "not measured".
//
// Steps (design §7.1). Durations ACCUMULATE: a step that runs twice reports
// the sum, and S6 also reports how often it ran.
//   S1  signInStateFor() keyring read
//   S2  leave the call, setCurrentRoomId("") / RoomInfo reset
//   S3  detachSession(): clearLocalState and the loggedOut fan-out
//   S4  setActiveAccountUserId(): the settings signals and the sync. S5 (the
//       accountsChanged -> accounts() keyring reads) is NOT separable from an
//       AppController stamp in Phase 0: it runs synchronously inside S4's
//       signal fan-out, so it is FOLDED INTO S4 and S5 always reports -1. No
//       extra keyring reads are added to measure it.
//   S6  clearCrossAccountCaches(); runs twice per switch (switch and
//       onLoginSucceeded)
//   S7  RustSdkMatrixClient::restoreSession() up to the mx_rust_restore
//       dispatch
//   S8  S7's end -> the login_ok / login_failed handler (restore thread plus
//       the poll that delivers it). The Rust-side split is in rust_*.
//   S9  startSync() -> the first room list reset handled on the GUI thread
//   S10 onLoginSucceeded(), whole body
//   S11 the first room opened after login (nothing reopens one
//       automatically today, so it is the user's first open) -> its
//       timeline first stops hydrating. Begun at most once per trace.
//
// Two targets, kept separate (design R2-H6), both in ms since begin():
//   first_frame_ms       the first frame swapped after the room list has rows
//   room_interactive_ms  the first room opened after login has filled (S11's
//                        end)
//
// Target 2 is recorded only from a fill, never from something that merely
// LOOKS like one. Its signal, activeRoomHydrating, is QML's
// `timeline.visible && !presentationReady && Window.active`, so it also goes
// false when the window loses focus or the chat shell is hidden (Settings).
// AppController ignores a false edge unless the main window is active and the
// main screen is showing, and a room change before the fill abandons the
// target (abandonRoomInteractive()). Either way the trace then closes Partial
// with room_interactive_ms=-1 rather than a wrong number: a focus change
// during the measured fill under-reports, it never mis-reports. Two things
// still count as "filled" and are the view's own definition of it: the
// presentation guard's 2.5 s safety valve, and initialContentSettled on a
// room too short to fill the viewport.
//
// A trace closes Ok when both targets are marked, Partial 30 s after
// markLoginSucceeded() if they are not, Refused / Failed when the switch is
// refused or fails, and Superseded when another begin() opens over it.
//
// Privacy (§6): accounts appear only as safeUserSlug(); no room, event or
// device ids, no tokens, no bodies. Disabled cost: one cached bool per call.
//
// GUI thread only. Not a QObject.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace lightning::switchtrace {

enum class Step : quint8 { S1 = 1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11 };
enum class Path : quint8 { Cold, Adopt, Warm, HostThenAdopt, RetireThenCold, Startup };
enum class Outcome : quint8 { Ok, Partial, Refused, Failed, Superseded };

constexpr int kStepCount = 11;
// A trace not complete this long after markLoginSucceeded() closes Partial.
constexpr qint64 kDeadlineMs = 30000;

// LIGHTNING_SWITCH_TRACE set and, trimmed and lowercased, not one of "", "0",
// "false", "off", "no". Read once per process (the Rust side's rule too).
bool enabled();
// enabled() && a trace is open. A trace past its deadline is closed here.
bool active();
// Opens a trace. No-op unless enabled(). An open trace is first finished as
// Superseded.
void begin(const QString &fromUserId, const QString &toUserId, Path path = Path::Cold);
// T0 (master prompt Part 4 §5): the moment a switch to `toUserId` was asked
// for, before any live-account release, queueing behind a running switch or
// keyring read. The next begin() for the same target reports the lead from
// it (`intent_lead_ms`) and the first correct frame measured from it
// (`first_frame_from_intent_ms`), so the total never restarts at a later
// barrier. The earliest pending intent for a target is kept (a re-entry of
// the same switch does not move it); an intent for another target replaces
// it (the newest click wins); one older than kIntentMaxAgeMs is ignored.
void noteIntent(const QString &toUserId);
constexpr qint64 kIntentMaxAgeMs = 120000;
// Durations ACCUMULATE; begin/end nest per step (only the outermost pair
// counts); an unmatched end is ignored. No-op unless active(). S11 only
// begins once markLoginSucceeded() has run, and only once per trace.
void stepBegin(Step);
void stepEnd(Step);

class StepScope { // RAII stepBegin/stepEnd; no-op when !active() at entry
public:
    explicit StepScope(Step s);
    ~StepScope();
    StepScope(const StepScope &) = delete;
    StepScope &operator=(const StepScope &) = delete;

private:
    Step m_step;
    bool m_on;
};

// The login_ok / login_failed "trace" object (WP0-R §6.2). receivedWallMs =
// QDateTime::currentMSecsSinceEpoch() at handler entry. Ends S8 (S8 = S7 end
// -> this call, C++-observed). The first call per trace is recorded.
void noteRustRestore(const QJsonObject &rustTrace, qint64 receivedWallMs);
// The first room_list_reset (or classic "rooms") after startSync. Records only
// the first per trace. Ends S9. rustTrace may be empty (classic path, or the
// Rust trace off).
void noteFirstReset(int rooms, qint64 parseMs, qint64 modelMs,
                    const QJsonObject &rustTrace, qint64 receivedWallMs);
// Target 1: room list populated, the first frame after it. Only after
// markLoginSucceeded(); the first call per trace is recorded.
void markFirstFrame();
// Target 2: the first room opened after login has filled (= S11's end).
// Ignored unless S11 is open (so only after markLoginSucceeded() and the
// first room's open) and not abandoned; the first call per trace is recorded.
void markRoomInteractive();
// The room S11 measures was left before it filled (another room opened, or
// none): target 2 is given up for this trace, S11 stays -1, and the trace
// closes Partial at its deadline. No-op unless S11 is open.
void abandonRoomInteractive();
// Probe: logs each transition (whether or not a trace is open), counts flips
// within an open trace. A repeat of the last value is ignored.
void noteLoggedIn(bool loggedIn);
// Starts the 30 s deadline. Writes nothing by itself.
void markLoginSucceeded();
// active() and markLoginSucceeded() has not run yet: the switch or restore
// the trace measures is still waiting on its login. A login failure after
// that belongs to something else (an add-account attempt, say) and must not
// relabel a switch that succeeded.
bool awaitingLogin();
// Writes the one line and closes. No-op when no trace is open.
void finish(Outcome);

// ---- pure, for tests ----
// The §1 rule on a raw value ("" for unset). enabled() applies it once to the
// environment.
bool enabledFromEnvValue(const QByteArray &raw);
struct Record {
    quint64 id = 0;
    Path path = Path::Cold;
    Outcome outcome = Outcome::Ok;
    QString fromSlug;          // safeUserSlug(from), "" when none
    QString toSlug;
    qint64 steps[kStepCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int s6Runs = 0;
    // Rust restore (login_ok / login_failed "trace")
    qint64 rustDiscoveryMs = -1;
    qint64 rustStoreOpenMs = -1;
    qint64 rustStoreOpens = -1;
    qint64 rustFallbackBuildMs = -1;
    int rustOfflineFallback = -1;  // -1 unknown, 0 false, 1 true
    qint64 rustRestoreMs = -1;
    qint64 rustResumeUnsentMs = -1;
    qint64 rustRestoreTotalMs = -1;
    qint64 restorePollMs = -1;     // Rust enqueue (wall) -> C++ handler (wall)
    // First room list reset
    qint64 resetRooms = -1;
    qint64 rustSyncBuildMs = -1;
    qint64 rustSinceStartSyncMs = -1;
    qint64 rustResetSerializeMs = -1;
    qint64 resetPollMs = -1;
    qint64 resetParseMs = -1;
    qint64 modelMs = -1;
    // Targets, ms since begin()
    qint64 firstFrameMs = -1;
    qint64 roomInteractiveMs = -1;
    int loggedInFlips = 0;
    qint64 intentLeadMs = -1;  // T0 (noteIntent) -> begin()

    qint64 step(Step s) const { return steps[int(s) - 1]; }
};

QString formatLine(const Record &);
// The open trace's record; with none open, the last one written (a default
// Record when there was none).
Record currentRecordForTest();
void setEnabledForTest(bool);
// Monotonic clock in ms. nullptr restores the QElapsedTimer clock.
void setClockForTest(std::function<qint64()> monotonicMs);
// Closes any trace WITHOUT writing it, forgets the last record, the loggedIn
// probe's last value and the id counter. Keeps the enabled override and the
// clock.
void resetForTest();

} // namespace lightning::switchtrace
