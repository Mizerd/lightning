#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "matrix/MatrixClient.h"

class SettingsManager;

// v0.7.x Matrix presence (design-handoff follow-up).
//
// Sliding Sync delivers NO presence events, so presence is a bounded
// polling loop, and this class owns the entire policy: WHO is polled
// (exactly the users some visible surface has watch()ed — DM rows, the
// open People list, an open profile popover), HOW OFTEN (one bounded batch
// per round, plus a short debounced burst when a new unknown user appears),
// and WHEN TO STOP (a homeserver that answers forbidden for everyone has
// presence disabled; polling it forever would be noise).
//
// Honesty rules, matching the receipt/facepile precedents:
//   - Unknown is rendered as NOTHING. A failed lookup, a not-yet-looked-up
//     user, an unsupported backend and a presence-disabled server are all
//     indistinguishable "no indicator" — never fabricated offline.
//   - A transient network failure keeps the last known state rather than
//     erasing it; only an authoritative answer replaces an answer.
//   - stateFor()/infoFor() are pure reads, safe in QML bindings; re-read
//     on revisionChanged.
//
// Own-presence publication is the other half: a periodic keep-alive PUT
// (servers expire presence quickly) of online — or unavailable once the
// application has been in the background for a while — gated by the
// application-wide "share presence" privacy setting (global like the
// link-preview switches, not per-account). Disabling the setting
// publishes one final offline so the account does not linger online.
class PresenceManager : public QObject
{
    Q_OBJECT

    // Backend capability alone: true whenever the client CAN do presence.
    // This is what gates the Settings publication card — the card must
    // never disappear while publication can still run (review M1), so it
    // deliberately ignores the read-side refusal latch below.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // supported AND the server has not refused presence reads; QML uses it
    // only to skip watch() bookkeeping — an inactive manager already
    // answers "" for every user.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // Bumped whenever any cached presence changes. Bindings reference it to
    // re-evaluate stateFor()/infoFor().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // Bumped when a session ends (sign-out / account switch). PresenceDot
    // re-registers its watch on this edge, because clearSession() drops
    // the watched set (review M2: one account's watch list must never be
    // polled against the next account's homeserver).
    Q_PROPERTY(int sessionEpoch READ sessionEpoch NOTIFY sessionEpochChanged)
    // The ONLY two conditions under which the client actually KNOWS that
    // presence will not be answered: the backend cannot do presence at
    // all, or this session's server refused it for every user (the latch
    // below). Everything else — an unanswered lookup, a user we have not
    // polled yet, a transient failure — stays UNKNOWN, and unknown must
    // keep rendering nothing. This property exists so the profile popover
    // can say "Presence unavailable" for the two honest cases without
    // QML having to infer them from `supported && !active`, which reads
    // like a coincidence and would silently acquire a third meaning the
    // day either property gains a condition.
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    // v0.9 (phase 10): the account's own status. `ownStatusText` is what is
    // published as the spec presence status_msg (emoji first, as ordinary
    // characters, so every client sees it); `ownStatusExpiresAtMs` is 0 for
    // no expiry, else a wall-clock ms timestamp after which the status is
    // cleared — on the timer while running, on the next start otherwise.
    // Expiry is a LIGHTNING convenience (it does not federate): the
    // published text simply disappears when the deadline passes.
    Q_PROPERTY(QString ownStatusText READ ownStatusText NOTIFY ownStatusChanged)
    Q_PROPERTY(QString ownStatusEmoji READ ownStatusEmoji NOTIFY ownStatusChanged)
    Q_PROPERTY(qint64 ownStatusExpiresAtMs READ ownStatusExpiresAtMs
                   NOTIFY ownStatusChanged)

public:
    explicit PresenceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSettings(SettingsManager *settings);

    bool supported() const;
    bool active() const;
    bool unavailable() const;
    int revision() const { return m_revision; }
    int sessionEpoch() const { return m_sessionEpoch; }

    // Ref-counted visibility: a delegate watches on creation and unwatches
    // on destruction. Watching is idempotent per caller and cheap; only
    // watched users are ever polled.
    Q_INVOKABLE void watch(const QString &userId);
    Q_INVOKABLE void unwatch(const QString &userId);
    // A newly received remote event is a freshness hint, never a presence
    // assertion. Re-poll a watched sender promptly; only the server answer
    // may change the rendered state.
    void noteActivity(const QString &userId);
    // A live typing notification about a WATCHED user. It is the one
    // present-tense, server-forwarded fact this client receives about
    // somebody else, and it CONTRADICTS a cached "offline": a homeserver
    // with presence switched off answers 200 with "offline" for everybody
    // rather than refusing, so the refusal latch never fires and the dot is
    // confidently wrong forever (the same defect ownPublishedState() already
    // records for the local user, which reached nobody else).
    //
    // The contradicted claim is WITHDRAWN, never replaced. The state becomes
    // unknown, and unknown renders nothing — promoting it to "online" would
    // be the same fabrication in the other direction, since typing proves
    // activity and presence is a state the server owns. Only "offline" is
    // withdrawn: "unavailable" is a soft idle heuristic and someone typing
    // while marked away is ordinary, not a contradiction.
    void noteTyping(const QString &userId);

    // "online" / "unavailable" / "offline", or "" when unknown.
    Q_INVOKABLE QString stateFor(const QString &userId) const;
    // { state, currentlyActive, lastActiveAgoMs } with the age adjusted to
    // NOW (the server age plus time since the answer arrived), so QML can
    // format "last active …" without its own clock bookkeeping.
    // lastActiveAgoMs is -1 when the server sent none. Empty map when
    // unknown.
    Q_INVOKABLE QVariantMap infoFor(const QString &userId) const;
    // A peer's status text as the last poll reported it ("" = none/unknown).
    Q_INVOKABLE QString statusMessageFor(const QString &userId) const;
    QString ownStatusText() const;
    QString ownStatusEmoji() const { return m_ownStatusEmoji; }
    qint64 ownStatusExpiresAtMs() const { return m_ownStatusExpiresAtMs; }
    Q_INVOKABLE void setOwnStatus(const QString &emoji, const QString &text,
                                  qint64 expiresAtMs);
    Q_INVOKABLE void clearOwnStatus();

    // Test/embedding seam; the ctor also tracks QGuiApplication state when
    // one exists.
    void setApplicationActive(bool active);
    // Test seams: the real idle threshold is minutes and the real
    // keep-alive interval is 4 minutes — the idle/publish contracts are
    // untestable at those scales (review L7).
    void setIdleThresholdForTest(qint64 ms) { m_idleAfterMs = ms; }
    void setPublishIntervalForTest(int ms)
    {
        m_publishJitter = false;   // deterministic cadence for the suite
        m_publishTimer.setInterval(ms);
    }
    void setMinPublishGapForTest(int ms) { m_minPublishGapMs = ms; }
    /// How many own-presence PUTs were sent and how many the server
    /// rejected, SINCE THE PROCESS STARTED — deliberately monotonic and
    /// deliberately NOT reset by `clearSession()`, a sign-out or an account
    /// switch. A rate is a difference between two readings, and a counter
    /// that resets destroys the history the reading is for.
    ///
    /// Read them together with the fact that they span sessions; an earlier
    /// draft of this comment said "since this session started", which
    /// nothing in the implementation has ever made true.
    ///
    /// They exist because "is presence working" was unanswerable of a live
    /// session: the rejection RATE is the thing that matters and it was
    /// visible only by counting log lines by hand after the fact. The
    /// ATTEMPT count matters just as much — see the note at
    /// `kRetryAfterFloorMs` about the offered rate, which is the number
    /// that would settle what is actually driving the rejections.
    Q_INVOKABLE qint64 publishAttempts() const { return m_publishAttempts; }
    Q_INVOKABLE qint64 publishRejections() const { return m_publishRejections; }
    /// How many retries deep the current rejection chain is, 0 when none is
    /// armed. A COUNT, not a duration — the first version of this comment
    /// said milliseconds, which is a different member's sentence.
    ///
    /// It exists because the chain is otherwise observable only through
    /// TIMING, and timing is what made three assertions in this suite
    /// vacuous: `m_publishRetryTimer` is one single-shot timer and
    /// `start()` RESTARTS it, so at most one retry is ever pending and the
    /// publish count in any window is insensitive to the cap. Assert the
    /// chain, not the consequence.
    int retryChainForTest() const { return m_retryChain; }
    /// The cap the chain plateaus at. Readable so the suite asserts the
    /// REAL bound instead of mirroring a literal that would quietly go
    /// stale the day somebody tunes it — the value is the contract, and a
    /// test carrying its own copy is a test that stops testing.
    static constexpr int maxRetryChain() { return kMaxRetryChain; }
    // The real typing-evidence window is 35 s; that it EXPIRES is untestable
    // at that scale. Deliberately not reset by clearSession() — it is a
    // harness value, not session state.
    void setTypingEvidenceWindowForTest(qint64 ms) { m_typingWindowMs = ms; }

Q_SIGNALS:
    void supportedChanged();
    void activeChanged();
    void unavailableChanged();
    void ownStatusChanged();
    void revisionChanged();
    void sessionEpochChanged();

private:
    struct Entry {
        QString state;
        bool currentlyActive = false;
        qint64 lastActiveAgoMs = -1;
        qint64 receivedAtMs = 0;
        QString statusMsg;
    };

    // One polling round every 30 s keeps a visible dot honest without
    // meaningfully loading the server (each round is at most
    // kBatchCap GETs for users that are actually on screen).
    static constexpr int kPollIntervalMs = 30000;
    // Newly watched unknown users are answered quickly (a popover should
    // not wait half a minute), but debounced so a People list materializing
    // 30 delegates asks once, not 30 times.
    static constexpr int kBurstDelayMs = 400;
    static constexpr qint64 kFreshWatchMs = 10000;
    // Mirrors PRESENCE_BATCH_CAP in rust/src/presence.rs.
    static constexpr int kBatchCap = 40;
    // MEASURED, and the number this replaced was assumed. The comment here
    // used to read "servers expire presence after a few minutes without
    // activity" and set the keep-alive to four minutes on that basis. An
    // interop audit on 2026-09-19 measured the real figure against this
    // project's own Synapse: a published "online" survives between 33 and
    // 63 seconds, which is Synapse's `SYNC_ONLINE_TIMEOUT` (30 s after the
    // last sync activity) plus its activity granularity. So the keep-alive
    // was FOUR TIMES SLOWER than the expiry, and the account read OFFLINE to
    // everybody else for about three quarters of every live session —
    // measured from a second account querying the server, not inferred.
    //
    // AND THE PUT IS THE ONLY LEVER WE HAVE. A client normally stays online
    // because its /sync carries `set_presence`; Lightning syncs through
    // simplified sliding sync, which has no such parameter, so nothing about
    // syncing tells this server we are here. Verified in `rust/src/presence.rs`
    // — `set_presence::v3` is the only call that touches presence.
    //
    // 25 s therefore, strictly inside the 30 s floor. That is one small PUT
    // per 25 s for a live session, which is the cost of the protocol here;
    // Synapse's `rc_presence` default (0.1/s sustained) allows it with room
    // to spare.
    static constexpr int kPublishIntervalMs = 25 * 1000;
    // NO TWO IDENTICAL PUBLISHES INSIDE THIS WINDOW. `handleConnectionState`
    // forces a publish on every edge into Syncing, and a session start flaps
    // `starting -> offline -> retrying -> starting -> running`, so two PUTs
    // went out within ~3 s of launch; Synapse's `rc_presence` burst is 1, it
    // rejected the second, and the Rust side sends with `.disable_retry()` —
    // so the reported `own-presence publish failed: "rate_limited"` cost the
    // whole first keep-alive window. Only an UNCHANGED state is dropped: a
    // real state change still publishes immediately.
    static constexpr int kMinPublishGapMs = 10 * 1000;
    // How far EARLIER than the interval a KEEP-ALIVE tick may land, so the
    // effective period is 21-25 s and the 33 s floor the interval was
    // measured against is never approached from below. The RETRY path adds
    // it instead of subtracting, because there the point is to spread
    // several rejected devices apart rather than to stay under a floor;
    // this sentence used to read "subtracted, never added" and stopped
    // being true of one of its two users.
    static constexpr int kPublishJitterMs = 4 * 1000;
    // ── A REJECTED PUBLISH IS NOT A PUBLISH, AND WAITING A FULL PERIOD
    //    AFTER ONE IS HOW AN ACCOUNT GOES DARK ────────────────────────────
    //
    // `rc_presence` is per USER and Synapse's default is one accepted PUT
    // per ten seconds with a burst of ONE. Jitter spreads a user's devices
    // apart but does NOT reduce their aggregate rate, and the aggregate is
    // what the limiter counts — so with several devices signed in, most
    // ticks are rejected. Measured live against this project's own Synapse:
    // 62% of publishes rejected over 33 minutes, and a run of TWENTY-NINE
    // consecutive rejections — about ELEVEN MINUTES in which the account
    // read offline to everyone while the process was running and healthy,
    // against a server expiry of 33 to 63 seconds.
    //
    // The old handler logged the category and returned, under a comment
    // saying "the next keep-alive tick retries anyway". That is only true
    // if the next tick is not ALSO rejected, and for 29 ticks it was.
    //
    // So a rejection arms a SHORT one-shot retry instead of ceding the
    // period. The server usually says when it will accept; the hint is
    // bounded here because a homeserver is free to answer with a number
    // that would park the keep-alive for an hour, and because a hint of 0
    // must not become a busy loop.
    // **AND THE MEASUREMENT IS NOT FULLY EXPLAINED — do not record this as
    // the whole cause.** One device at 25 s offers 0.04 PUT/s against a
    // 0.1/s limit, 2.5x UNDER. Four devices offer 0.16/s, which predicts
    // ~37% rejection, not 62%. That number needs either six or seven
    // concurrent publishers or an offered rate well above one per 25 s from
    // a single client — and there IS such a path: the Syncing-edge publish
    // is gap-limited to one per 10 s, which is exactly the limiter's own
    // rate, so a flapping connection alone can sustain ~50% rejection with
    // one device. If that is what was happening, this retry treats a
    // symptom and multiplies a flap's traffic by five.
    //
    // **NARROWED 2026-09-19, and it is the many-devices arm.** Two
    // measurements, taken minutes apart on the same machine:
    //
    //   * a single client on a fixture account, with the publish trace on:
    //     8 attempts over 165 s, steady intervals of 23/24/24 s, 2.91
    //     attempts/min converging on ~2.6 — and **ZERO rejections**. So the
    //     period does what it says and one well-behaved client is 2.3x under
    //     the limit.
    //   * the client on the account that was FAILING, over the same window:
    //     rejections at 24, 24, 25, 25 and 23 s — **every tick, 100%** — from
    //     ONE process offering that same ~0.042 PUT/s.
    //
    // A client 2.4x under the limit cannot be rejected by its own traffic, so
    // the budget was being spent by OTHER publishers on that account. That is
    // the deduction the ratio could not support and these two readings do.
    //
    // It also says what the failure SHAPE is: with several devices the ones
    // that lose the race are starved indefinitely, because ceding a whole
    // period after a rejection means never competing for the next token —
    // which is the 29-consecutive-rejection run, and exactly what the retry
    // below is for. The flap arm is NOT ruled out as a second cause; it is
    // no longer needed to explain what was seen.
    //
    // `publishAttempts()` and the `presence-publish` trace line are what made
    // this answerable; use them rather than the rejection ratio, which cannot
    // tell the two causes apart.
    static constexpr int kRetryAfterFloorMs = 1500;
    // Plus up to kPublishJitterMs, because the jitter is added AFTER this
    // clamp — the effective ceiling is 24 s, still well inside the 33 s
    // expiry floor. And the ceiling's real job is not a hostile server: it
    // is CLIENT CLOCK SKEW. A `RetryAfter::DateTime` hint is converted
    // against our own clock, and this project has already lost a round to a
    // guest whose clock was seven hours ahead — without this bound that
    // machine would park its keep-alive for seven hours.
    static constexpr int kRetryAfterCeilingMs = 20 * 1000;
    // What to wait when the server rejected us without saying when. Under
    // the floor of the expiry window, so a blind retry still lands inside
    // the life of the last accepted publish.
    static constexpr int kRetryAfterUnknownMs = 4 * 1000;
    // Bounded so a server that rejects everything cannot turn the
    // keep-alive into a tight loop: after this many retries in a row the
    // chain gives up and waits for the ordinary tick.
    static constexpr int kMaxRetryChain = 4;
    // The app being CONTINUOUSLY in the background this long reads as
    // "idle" — measured from the moment focus was lost (review H2: an
    // earlier draft measured from the moment focus was GAINED, so any
    // session focused longer than this published Away the instant the
    // user switched windows).
    static constexpr qint64 kIdleAfterMs = 10 * 60 * 1000;
    // Two consecutive all-forbidden batches latch "this server has
    // presence disabled" for the rest of the session. A batch only counts
    // when it carries at least this many distinct users (review L1: a
    // single user's 403 — a federation edge, an invited-not-joined member
    // — must not blind presence for everyone).
    static constexpr int kForbiddenLatchThreshold = 2;
    static constexpr int kForbiddenLatchMinBatch = 2;
    // How long one typing notification keeps contradicting a cached
    // "offline". Long enough to cover a poll round (30 s) plus its answer,
    // short enough that a stale contradiction cannot outlive the typing it
    // came from by much. Withholding is the conservative direction — it
    // renders nothing — so erring slightly long costs an absent dot, never
    // a wrong one.
    static constexpr qint64 kTypingEvidenceMs = 35000;

    void pollRound(const char *kind, const QStringList &userIds);
    // Opt-in diagnostic (env LIGHTNING_PRESENCE_TRACE, read ONCE at
    // construction — the LIGHTNING_SCROLL_TRACE pattern). One bounded line
    // per polling round decision; applyBatch emits the matching answer
    // line itself. Counts and literal tags only: never a user id, never a
    // display name, never a list. `reason` is always a string literal.
    void traceRound(const char *kind, const char *reason, int batch,
                    quint64 opId) const;
    void scheduledPollRound();
    void burstRound();
    void applyBatch(quint64 opId, const QVariantList &entries);
    // `afterRejection` skips the kMinPublishGapMs window, and that is the
    // whole point of the flag: that window exists to stop a DUPLICATE PUT
    // of a state the server already accepted, and after a rejection the
    // server accepted nothing. Without the bypass every retry inside ten
    // seconds would be dropped by the guard that was written for the
    // opposite situation.
    void publishTick(bool force, bool afterRejection = false);
    void onPublishRejected(const QString &category, qint64 retryAfterMs);
    void handleConnectionState(MatrixClient::ConnectionState state);
    void clearSession();
    int desiredOwnState() const;
    bool publishEnabled() const;
    // v0.9 own status.
    void loadOwnStatusIfNeeded();
    void persistOwnStatus();
    void armStatusExpiry();
    bool m_ownStatusLoaded = false;
    bool m_statusTimerWired = false;
    QString m_ownStatusEmoji;
    QString m_ownStatusPlainText;
    qint64 m_ownStatusExpiresAtMs = 0;
    QTimer m_statusExpiryTimer;
    // "online" / "unavailable" / "offline" for the local user, or "" when
    // this client is not publishing and therefore does not know.
    QString ownPublishedState() const;
    bool isOwnUser(const QString &userId) const;
    // True while live typing evidence contradicts what the cache holds for
    // this user. Pure read: stateFor()/infoFor() are QML-binding safe.
    bool typingContradicts(const QString &userId) const;
    // Drops expired evidence and ANNOUNCES it. applyBatch only bumps the
    // revision when a polled VALUE changes, and on a presence-disabled
    // server the cached value is "offline" throughout — so without this the
    // withheld dot would never come back until an unrelated update happened
    // along.
    void pruneTypingEvidence();

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;

    QHash<QString, int> m_watched;
    QSet<QString> m_burstPending;
    QHash<QString, Entry> m_cache;
    QSet<quint64> m_inFlight;
    QStringList m_pollOrder;
    int m_pollCursor = 0;

    // userId -> m_clock time of the most recent typing notification.
    // Bounded by the WATCHED set (nothing else is ever recorded), which is
    // bounded by what is on screen.
    QHash<QString, qint64> m_typingSince;

    QTimer m_pollTimer;
    QTimer m_burstTimer;
    QTimer m_publishTimer;
    // One-shot, armed only by a rejection. Separate from m_publishTimer so
    // the ordinary cadence is never disturbed by a retry.
    QTimer m_publishRetryTimer;
    int m_retryChain = 0;
    // Diagnostics, because "is presence working" was unanswerable of a live
    // session: the rejection RATE is the thing that matters and it was
    // visible only by counting log lines by hand after the fact.
    qint64 m_publishAttempts = 0;
    qint64 m_publishRejections = 0;
    QTimer m_typingTimer;
    QElapsedTimer m_clock;
    // m_clock time of the last PUT we actually sent, for kMinPublishGapMs.
    qint64 m_lastPublishAtMs = -1;
    int m_minPublishGapMs = kMinPublishGapMs;
    // TWO CLIENTS OF ONE ACCOUNT MUST NOT KEEP ALIGNING. Every running client
    // publishes on the same 25 s period, so a user with a desktop and a
    // laptop puts ~4.8 PUT/min on one account and Synapse's `rc_presence`
    // (burst 1) starts answering 429 — measured at the time as 3 of 38 PUTs
    // on an account with four sessions open. Spreading each client's next
    // tick over a window makes a collision transient instead of periodic.
    //
    // **AND THAT 8% WAS NOT THE WHOLE STORY.** A later live audit measured
    // 62% rejected over 33 minutes on the same homeserver, with a run of 29
    // consecutive rejections. Jitter fixes collisions when the account's
    // AGGREGATE rate is under the limit — burst 1 means simultaneous
    // arrivals collide even at a legal rate — and does nothing at all when
    // the aggregate is over it, because the limiter counts per USER. See
    // `kRetryAfterFloorMs` for what was done about that, and for the part
    // of the measurement that is still unexplained.
    bool m_publishJitter = true;

    quint64 m_nextOpId = 1;
    int m_revision = 0;
    int m_sessionEpoch = 0;
    int m_forbiddenBatches = 0;
    bool m_serverRefused = false;
    // Read once at construction so a test can enable the trace per
    // instance; a function-static would freeze the first value for the
    // whole process.
    bool m_traceEnabled = false;
    bool m_appActive = true;
    // When focus was LOST (only meaningful while m_appActive is false).
    qint64 m_inactiveSinceMs = 0;
    qint64 m_idleAfterMs = kIdleAfterMs;
    qint64 m_typingWindowMs = kTypingEvidenceMs;
    int m_lastPublished = -1;
    // The user disabled sharing while the session was not live; the final
    // offline is owed and flushed on the next Syncing edge (review M3).
    bool m_pendingFinalOffline = false;
    bool m_syncing = false;
};
