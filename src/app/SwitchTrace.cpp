#include "app/SwitchTrace.h"

#include "storage/AppDataPaths.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QStringDecoder>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <utility>

Q_LOGGING_CATEGORY(lcSwitchTrace, "lightning.switchtrace")

namespace lightning::switchtrace {
namespace {

// The rule Rust's LIGHTNING_SYNC_TRACE / LIGHTNING_SWITCH_TRACE reader uses
// (lib.rs), so both halves of one trace switch on together.
bool envEnabled()
{
    return enabledFromEnvValue(qgetenv("LIGHTNING_SWITCH_TRACE"));
}

struct OpenTrace {
    Record rec;
    qint64 beginMs = 0;
    qint64 stepStart[kStepCount] = {};
    int depth[kStepCount] = {};
    bool s9Begun = false;
    bool s11Begun = false;
    qint64 s7EndMs = -1;
    bool loginSucceeded = false;
    qint64 loginSucceededMs = -1;
    bool restoreNoted = false;
    bool resetNoted = false;
};

struct State {
    int enabledOverride = -1; // -1 env, 0 off, 1 on
    std::function<qint64()> clock;
    std::optional<OpenTrace> open;
    Record last;
    quint64 nextId = 1;
    int lastLoggedIn = -1; // -1 never observed
};

State &state()
{
    static State s;
    return s;
}

qint64 nowMs()
{
    State &s = state();
    if (s.clock)
        return s.clock();
    static QElapsedTimer timer;
    if (!timer.isValid())
        timer.start();
    return timer.elapsed();
}

int indexOf(Step step)
{
    return std::clamp(int(step) - 1, 0, kStepCount - 1);
}

const char *pathName(Path p)
{
    switch (p) {
    case Path::Cold: return "cold";
    case Path::Adopt: return "adopt";
    case Path::Warm: return "warm";
    case Path::HostThenAdopt: return "host-then-adopt";
    case Path::RetireThenCold: return "retire-then-cold";
    case Path::Startup: return "startup";
    }
    return "unknown";
}

const char *outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::Partial: return "partial";
    case Outcome::Refused: return "refused";
    case Outcome::Failed: return "failed";
    case Outcome::Superseded: return "superseded";
    }
    return "unknown";
}

// A missing or non-numeric key reads as -1 ("not measured").
qint64 readMs(const QJsonObject &obj, const char *key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (!v.isDouble())
        return -1;
    return qint64(v.toDouble());
}

qint64 pollLatency(qint64 enqueuedWallMs, qint64 receivedWallMs)
{
    if (enqueuedWallMs < 0 || receivedWallMs <= 0)
        return -1;
    // Wall clocks on both sides; a step backwards reads as 0, not negative.
    return std::max<qint64>(0, receivedWallMs - enqueuedWallMs);
}

QString slugOrDash(const QString &slug)
{
    return slug.isEmpty() ? QStringLiteral("-") : slug;
}

void maybeComplete()
{
    State &s = state();
    if (s.open && s.open->rec.firstFrameMs >= 0
        && s.open->rec.roomInteractiveMs >= 0)
        finish(Outcome::Ok);
}

} // namespace

bool enabledFromEnvValue(const QByteArray &raw)
{
    // Exactly Rust's reading, so a pathological value cannot switch on only
    // one half: env::var fails (off) on bytes that are not UTF-8 and keeps a
    // leading BOM; str::trim strips Unicode whitespace; then ASCII lowercase.
    QStringDecoder utf8(QStringDecoder::Utf8,
                        QStringDecoder::Flag::Stateless
                            | QStringDecoder::Flag::ConvertInitialBom);
    QString v = utf8.decode(raw);
    if (utf8.hasError())
        return false;
    v = v.trimmed();
    for (QChar &c : v) {
        if (c.unicode() < 0x80)
            c = c.toLower();
    }
    return !(v.isEmpty() || v == QLatin1String("0")
             || v == QLatin1String("false") || v == QLatin1String("off")
             || v == QLatin1String("no"));
}

bool enabled()
{
    State &s = state();
    if (s.enabledOverride >= 0)
        return s.enabledOverride > 0;
    static const bool fromEnv = envEnabled();
    return fromEnv;
}

bool active()
{
    if (!enabled())
        return false;
    State &s = state();
    if (!s.open)
        return false;
    if (s.open->loginSucceeded
        && nowMs() - s.open->loginSucceededMs >= kDeadlineMs) {
        finish(Outcome::Partial);
        return false;
    }
    return true;
}

void begin(const QString &fromUserId, const QString &toUserId, Path path)
{
    if (!enabled())
        return;
    if (active())
        finish(Outcome::Superseded);
    State &s = state();
    OpenTrace t;
    t.rec.id = s.nextId++;
    t.rec.path = path;
    t.rec.fromSlug = matrix::app_data::safeUserSlug(fromUserId);
    t.rec.toSlug = matrix::app_data::safeUserSlug(toUserId);
    t.beginMs = nowMs();
    s.open = std::move(t);
}

void stepBegin(Step step)
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    const int i = indexOf(step);
    if (step == Step::S11) {
        if (!t.loginSucceeded || t.s11Begun)
            return;
        t.s11Begun = true;
    }
    if (step == Step::S9)
        t.s9Begun = true;
    if (t.depth[i]++ == 0) {
        t.stepStart[i] = nowMs();
        if (step == Step::S6)
            ++t.rec.s6Runs;
    }
}

void stepEnd(Step step)
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    const int i = indexOf(step);
    if (t.depth[i] == 0)
        return; // unmatched
    if (--t.depth[i] > 0)
        return; // still inside an outer begin of the same step
    const qint64 now = nowMs();
    const qint64 elapsed = std::max<qint64>(0, now - t.stepStart[i]);
    t.rec.steps[i] = std::max<qint64>(t.rec.steps[i], 0) + elapsed;
    if (step == Step::S7)
        t.s7EndMs = now;
}

StepScope::StepScope(Step s)
    : m_step(s)
    , m_on(active())
{
    if (m_on)
        stepBegin(m_step);
}

StepScope::~StepScope()
{
    if (m_on)
        stepEnd(m_step);
}

void noteRustRestore(const QJsonObject &rustTrace, qint64 receivedWallMs)
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    if (t.restoreNoted)
        return;
    t.restoreNoted = true;
    if (t.s7EndMs >= 0)
        t.rec.steps[indexOf(Step::S8)] =
            std::max<qint64>(0, nowMs() - t.s7EndMs);
    Record &r = t.rec;
    r.rustDiscoveryMs = readMs(rustTrace, "discovery_ms");
    r.rustStoreOpenMs = readMs(rustTrace, "store_open_ms");
    r.rustStoreOpens = readMs(rustTrace, "store_opens");
    r.rustFallbackBuildMs = readMs(rustTrace, "fallback_build_ms");
    const QJsonValue offline = rustTrace.value(QLatin1String("offline_fallback"));
    r.rustOfflineFallback = offline.isBool() ? (offline.toBool() ? 1 : 0) : -1;
    r.rustRestoreMs = readMs(rustTrace, "restore_ms");
    r.rustResumeUnsentMs = readMs(rustTrace, "resume_unsent_ms");
    r.rustRestoreTotalMs = readMs(rustTrace, "restore_total_ms");
    r.restorePollMs =
        pollLatency(readMs(rustTrace, "enq_wall_ms"), receivedWallMs);
}

void noteFirstReset(int rooms, qint64 parseMs, qint64 modelMs,
                    const QJsonObject &rustTrace, qint64 receivedWallMs)
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    // Before startSync the reset can only be the previous account's.
    if (t.resetNoted || !t.s9Begun)
        return;
    t.resetNoted = true;
    Record &r = t.rec;
    r.resetRooms = rooms;
    r.resetParseMs = parseMs;
    r.modelMs = modelMs;
    r.rustSinceStartSyncMs = readMs(rustTrace, "since_start_sync_ms");
    r.rustSyncBuildMs = readMs(rustTrace, "sync_service_build_ms");
    r.rustResetSerializeMs = readMs(rustTrace, "serialize_ms");
    r.resetPollMs =
        pollLatency(readMs(rustTrace, "enq_wall_ms"), receivedWallMs);
    stepEnd(Step::S9);
}

void markFirstFrame()
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    if (!t.loginSucceeded || t.rec.firstFrameMs >= 0)
        return;
    t.rec.firstFrameMs = std::max<qint64>(0, nowMs() - t.beginMs);
    maybeComplete();
}

void markRoomInteractive()
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    // S11 open implies the login (stepBegin gates it) and the first room's
    // open; an abandoned S11 is closed, so its room's later fill is ignored.
    if (t.depth[indexOf(Step::S11)] == 0 || t.rec.roomInteractiveMs >= 0)
        return;
    stepEnd(Step::S11);
    t.rec.roomInteractiveMs = std::max<qint64>(0, nowMs() - t.beginMs);
    maybeComplete();
}

void abandonRoomInteractive()
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    // Not a stepEnd: the time to a room the user left is not S11. s11Begun
    // stays set, so no later room starts it again.
    t.depth[indexOf(Step::S11)] = 0;
}

void noteLoggedIn(bool loggedIn)
{
    if (!enabled())
        return;
    State &s = state();
    const int value = loggedIn ? 1 : 0;
    if (s.lastLoggedIn == value)
        return;
    const bool firstObservation = s.lastLoggedIn < 0;
    s.lastLoggedIn = value;
    qint64 sinceBegin = -1;
    if (active()) {
        sinceBegin = std::max<qint64>(0, nowMs() - s.open->beginMs);
        if (!firstObservation)
            ++s.open->rec.loggedInFlips;
    }
    qCInfo(lcSwitchTrace).noquote()
        << QStringLiteral("switch-trace loggedIn=%1 t=%2")
               .arg(value)
               .arg(sinceBegin);
}

void markLoginSucceeded()
{
    if (!active())
        return;
    OpenTrace &t = *state().open;
    if (t.loginSucceeded)
        return; // the first success starts the deadline
    t.loginSucceeded = true;
    t.loginSucceededMs = nowMs();
    // The lazy check in active() needs a caller; this one closes a trace
    // nothing touches again. Keyed on the id, so it cannot close a newer one.
    if (QCoreApplication *app = QCoreApplication::instance()) {
        const quint64 id = t.rec.id;
        QTimer::singleShot(int(kDeadlineMs), Qt::PreciseTimer, app, [id] {
            State &s = state();
            if (s.open && s.open->rec.id == id)
                finish(Outcome::Partial);
        });
    }
}

bool awaitingLogin()
{
    return active() && !state().open->loginSucceeded;
}

void finish(Outcome outcome)
{
    State &s = state();
    if (!s.open)
        return;
    Record r = std::move(s.open->rec);
    s.open.reset();
    r.outcome = outcome;
    qCInfo(lcSwitchTrace).noquote() << formatLine(r);
    s.last = std::move(r);
}

QString formatLine(const Record &r)
{
    QStringList parts;
    parts.reserve(48);
    parts << QStringLiteral("switch-trace")
          << QStringLiteral("id=%1").arg(r.id)
          << QStringLiteral("path=%1").arg(QLatin1String(pathName(r.path)))
          << QStringLiteral("outcome=%1")
                 .arg(QLatin1String(outcomeName(r.outcome)))
          << QStringLiteral("from=%1").arg(slugOrDash(r.fromSlug))
          << QStringLiteral("to=%1").arg(slugOrDash(r.toSlug));
    for (int i = 0; i < kStepCount; ++i)
        parts << QStringLiteral("s%1=%2").arg(i + 1).arg(r.steps[i]);
    const auto field = [&parts](const char *name, qint64 value) {
        parts << QStringLiteral("%1=%2").arg(QLatin1String(name)).arg(value);
    };
    field("s6_runs", r.s6Runs);
    field("rust_discovery_ms", r.rustDiscoveryMs);
    field("rust_store_open_ms", r.rustStoreOpenMs);
    field("rust_store_opens", r.rustStoreOpens);
    field("rust_fallback_build_ms", r.rustFallbackBuildMs);
    field("rust_offline_fallback", r.rustOfflineFallback);
    field("rust_restore_ms", r.rustRestoreMs);
    field("rust_resume_unsent_ms", r.rustResumeUnsentMs);
    field("rust_restore_total_ms", r.rustRestoreTotalMs);
    field("restore_poll_ms", r.restorePollMs);
    field("reset_rooms", r.resetRooms);
    field("rust_sync_build_ms", r.rustSyncBuildMs);
    field("rust_since_start_sync_ms", r.rustSinceStartSyncMs);
    field("rust_reset_serialize_ms", r.rustResetSerializeMs);
    field("reset_poll_ms", r.resetPollMs);
    field("reset_parse_ms", r.resetParseMs);
    field("model_ms", r.modelMs);
    field("first_frame_ms", r.firstFrameMs);
    field("room_interactive_ms", r.roomInteractiveMs);
    field("logged_in_flips", r.loggedInFlips);
    return parts.join(QLatin1Char(' '));
}

Record currentRecordForTest()
{
    const State &s = state();
    return s.open ? s.open->rec : s.last;
}

void setEnabledForTest(bool on)
{
    state().enabledOverride = on ? 1 : 0;
}

void setClockForTest(std::function<qint64()> monotonicMs)
{
    state().clock = std::move(monotonicMs);
}

void resetForTest()
{
    State &s = state();
    s.open.reset();
    s.last = Record{};
    s.nextId = 1;
    s.lastLoggedIn = -1;
}

} // namespace lightning::switchtrace
