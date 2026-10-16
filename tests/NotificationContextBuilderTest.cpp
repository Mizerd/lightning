// NotificationContextBuilder is AppController::notificationContextFor()
// factored out (live accounts, Phase 0). It must be behaviour-identical: the
// golden case runs a VERBATIM copy of the old body (an oracle reading the
// same fake state the old code read through m_client, m_settings,
// m_roomList, m_thread and m_moderation) against the builder, over a table
// that exercises every rule, and compares the whole Context with operator==.
//
// The builder is fed by gather(), which is a hand-written COPY of
// AppController's gathering over the fake world, not the real one: the
// controller needs a whole application to construct. So the golden case
// proves the RULES moved intact, and says nothing about the real gathering.
// That half is appControllerGathersEveryInputFromItsOwnSource, which pins
// every `in.<field> = <expr>;` in the real notificationContextFor to the
// source the old body read for it, and requires every input field except the
// account to be assigned exactly once. A swapped right-hand side there would
// pass the golden case and change when notifications are suppressed.

#include "notifications/NotificationContextBuilder.h"

#include "matrix/TimelineEvent.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>
#include <QtTest>

#include <optional>

using lightning::notifications::NotificationContextBuilder;
using lightning::notifications::NotificationContextInputs;

namespace {

// What the old body read, per room, through its collaborators.
struct FakeRoomInfo {
    QString name;
    bool isDirect = false;
};
struct FakeThread {
    QString root;
    bool atLatest = false;
    QString rootEventId() const { return root; }
    bool viewerAtLatest() const { return atLatest; }
};
struct FakeModeration {
    QSet<QString> ignored;
    bool isIgnored(const QString &userId) const { return ignored.contains(userId); }
};
struct World {
    QString self;
    QHash<QString, FakeRoomInfo> rooms;
    QHash<QString, QVariantMap> rows;
    int theme = 0;
    QHash<QString, int> modes;
    QHash<QString, bool> decidesLocally;
    QHash<QString, int> preview;
    bool notificationsEnabled = true;
    QString currentRoomId;
    std::optional<FakeThread> thread;
    bool activeRoomAtLatest = false;
    bool activeRoomHydrating = false;
    bool initialSyncDone = true;
    int sound = 1;
    std::optional<FakeModeration> moderation;

    FakeRoomInfo roomInfo(const QString &id) const { return rooms.value(id); }
};

// ---- ORACLE: AppController::notificationContextFor at 9177fc58, verbatim
// except for the receivers (m_x-> becomes a read of the fake world). Do not
// "tidy" it: its value is that it is the old code.
NotificationManager::Context oldContextFor(const World &w, const QString &roomId,
                                           const TimelineEvent &event,
                                           bool fromThread)
{
    const FakeThread *m_thread = w.thread ? &*w.thread : nullptr;
    const FakeModeration *m_moderation = w.moderation ? &*w.moderation : nullptr;
    const QString &m_currentRoomId = w.currentRoomId;
    const bool m_activeRoomAtLatest = w.activeRoomAtLatest;
    const bool m_activeRoomHydrating = w.activeRoomHydrating;

    NotificationManager::Context context;
    context.selfUserId = w.self;
    // Targeted lookup; rooms() deep-copies the whole list per event.
    const FakeRoomInfo info = w.roomInfo(roomId);
    context.roomName = info.name.isEmpty() ? roomId : info.name;
    context.roomIsDirect = info.isDirect;
    const QVariantMap notifyRoomRow = w.rows.value(roomId);
    context.avatarMxc =
        notifyRoomRow.value(QStringLiteral("avatarUrl")).toString();
    // Initials colour and theme from the same row the room list uses, so the
    // two never disagree about an identity.
    context.avatarColorKey =
        notifyRoomRow.value(QStringLiteral("identityColorKey")).toString();
    context.themeId = int(w.theme);
    context.roomMode = static_cast<NotificationManager::RoomMode>(
        w.modes.value(roomId));
    // The server does not have this device's choice (a failed write, or a
    // device-only legacy mode); the local mode then decides alone.
    context.localModeUnsynced = w.decidesLocally.value(roomId);
    // Encrypted rooms may withhold more; a room not listed yet resolves like
    // unknown encryption (the stricter level).
    context.previewMode =
        static_cast<NotificationManager::PreviewMode>(w.preview.value(roomId, 1));
    context.notificationsEnabled = w.notificationsEnabled;
    const bool threaded = fromThread
        || (!event.threadRootId.isEmpty()
            && event.threadRootId != event.eventId);
    if (threaded) {
        context.roomVisibleAtLatest = roomId == m_currentRoomId && m_thread
            && m_thread->rootEventId() == event.threadRootId
            && m_thread->viewerAtLatest();
    } else {
        context.roomVisibleAtLatest =
            roomId == m_currentRoomId && m_activeRoomAtLatest;
    }
    // The open room's backlog arrives as live appends while its view is still
    // hydrating.
    context.roomHydrating = roomId == m_currentRoomId && m_activeRoomHydrating;
    // Initial-sync backlog is history and must not re-notify each launch.
    context.initialSyncComplete = w.initialSyncDone;
    context.soundMode = static_cast<NotificationManager::SoundMode>(w.sound);
    // Events from an ignored user already in flight must not notify.
    context.senderIsIgnored =
        m_moderation && m_moderation->isIgnored(event.sender);
    return context;
}

// A COPY of the new AppController's gathering, over the same world (the
// real one is pinned by appControllerGathersEveryInputFromItsOwnSource). The
// thread getters are const, so reading them eagerly (no short-circuit) is
// safe.
NotificationContextInputs gather(const World &w, const QString &roomId,
                                 const TimelineEvent &event, bool fromThread)
{
    NotificationContextInputs in;
    in.selfUserId = w.self;
    in.roomId = roomId;
    const FakeRoomInfo info = w.roomInfo(roomId);
    in.roomInfoName = info.name;
    in.roomIsDirect = info.isDirect;
    const QVariantMap row = w.rows.value(roomId);
    in.avatarMxc = row.value(QStringLiteral("avatarUrl")).toString();
    in.avatarColorKey = row.value(QStringLiteral("identityColorKey")).toString();
    in.themeId = w.theme;
    in.roomMode = w.modes.value(roomId);
    in.localModeUnsynced = w.decidesLocally.value(roomId);
    in.previewMode = w.preview.value(roomId, 1);
    in.notificationsEnabled = w.notificationsEnabled;
    in.eventId = event.eventId;
    in.eventThreadRootId = event.threadRootId;
    in.fromThread = fromThread;
    in.currentRoomId = w.currentRoomId;
    in.activeRoomAtLatest = w.activeRoomAtLatest;
    in.activeRoomHydrating = w.activeRoomHydrating;
    in.threadPanelPresent = w.thread.has_value();
    in.threadPanelRootId = w.thread ? w.thread->rootEventId() : QString();
    in.threadPanelAtLatest = w.thread && w.thread->viewerAtLatest();
    in.initialSyncComplete = w.initialSyncDone;
    in.soundMode = w.sound;
    in.senderIsIgnored = w.moderation && w.moderation->isIgnored(event.sender);
    return in;
}

QString describe(const NotificationManager::Context &c)
{
    return QStringLiteral("name=%1 direct=%2 mode=%3 unsynced=%4 preview=%5 "
                          "enabled=%6 visible=%7 hydrating=%8 initial=%9 "
                          "sound=%10 ignored=%11 avatar=%12 key=%13 theme=%14 "
                          "acct=%15 label=%16")
        .arg(c.roomName)
        .arg(int(c.roomIsDirect))
        .arg(int(c.roomMode))
        .arg(int(c.localModeUnsynced))
        .arg(int(c.previewMode))
        .arg(int(c.notificationsEnabled))
        .arg(int(c.roomVisibleAtLatest))
        .arg(int(c.roomHydrating))
        .arg(int(c.initialSyncComplete))
        .arg(int(c.soundMode))
        .arg(int(c.senderIsIgnored))
        .arg(c.avatarMxc, c.avatarColorKey)
        .arg(c.themeId)
        .arg(c.accountUserId, c.accountLabel);
}

const QString kRoomA = QStringLiteral("!alpha:example.org");
const QString kRoomB = QStringLiteral("!beta:example.org");
const QString kSender = QStringLiteral("@sender:example.org");

World baseWorld()
{
    World w;
    w.self = QStringLiteral("@me:example.org");
    w.rooms.insert(kRoomA, {QStringLiteral("Alpha"), true});
    // kRoomB has an empty name: the room id stands in for it.
    w.rooms.insert(kRoomB, {QString(), false});
    w.rows.insert(kRoomA,
                  {{QStringLiteral("avatarUrl"), QStringLiteral("mxc://x/a")},
                   {QStringLiteral("identityColorKey"), QStringLiteral("ka")}});
    // kRoomB has no row at all.
    return w;
}

} // namespace

class NotificationContextBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theBuilderMatchesTheOldBodyOverEveryRule()
    {
        const QStringList roots = {QString(), QStringLiteral("$e"),
                                   QStringLiteral("$r1"), QStringLiteral("$r2")};
        const QList<std::optional<FakeThread>> panels = {
            std::nullopt,
            FakeThread{QStringLiteral("$r1"), true},
            FakeThread{QStringLiteral("$r1"), false},
            FakeThread{QStringLiteral("$r2"), true},
            FakeThread{QString(), true},
        };
        const QStringList currents = {kRoomA, kRoomB, QString()};

        int compared = 0;
        int visible = 0, visibleThreaded = 0, hydrating = 0, namedById = 0,
            ignored = 0;
        QSet<int> modesSeen, previewsSeen, soundsSeen;
        for (const QString &roomId : {kRoomA, kRoomB})
        for (const QString &current : currents)
        for (const bool fromThread : {false, true})
        for (const QString &root : roots)
        for (const std::optional<FakeThread> &panel : panels)
        for (const bool atLatest : {false, true})
        for (const bool hydr : {false, true}) {
            World w = baseWorld();
            w.currentRoomId = current;
            w.thread = panel;
            w.activeRoomAtLatest = atLatest;
            w.activeRoomHydrating = hydr;
            // The scalars vary with the index so every value is covered
            // without multiplying the table.
            const int n = compared;
            w.theme = n % 11;
            w.modes.insert(roomId, n % 4);
            w.decidesLocally.insert(roomId, (n / 4) % 2 == 1);
            w.preview.insert(roomId, n % 3);
            w.notificationsEnabled = (n / 3) % 2 == 0;
            w.initialSyncDone = (n / 5) % 2 == 0;
            w.sound = (n / 2) % 3;
            if (n % 3 == 1)
                w.moderation = FakeModeration{{kSender}};
            else if (n % 3 == 2)
                w.moderation = FakeModeration{{QStringLiteral("@other:x")}};

            TimelineEvent event;
            event.eventId = QStringLiteral("$e");
            event.threadRootId = root;
            event.sender = kSender;

            const NotificationManager::Context oracle =
                oldContextFor(w, roomId, event, fromThread);
            const NotificationManager::Context built =
                NotificationContextBuilder::build(
                    gather(w, roomId, event, fromThread));
            QVERIFY2(built == oracle,
                     qPrintable(QStringLiteral("case %1\n  old: %2\n  new: %3")
                                    .arg(n)
                                    .arg(describe(oracle), describe(built))));
            ++compared;
            visible += oracle.roomVisibleAtLatest ? 1 : 0;
            const bool threaded =
                fromThread || (!root.isEmpty() && root != event.eventId);
            visibleThreaded += (oracle.roomVisibleAtLatest && threaded) ? 1 : 0;
            hydrating += oracle.roomHydrating ? 1 : 0;
            namedById += oracle.roomName == roomId ? 1 : 0;
            ignored += oracle.senderIsIgnored ? 1 : 0;
            modesSeen.insert(int(oracle.roomMode));
            previewsSeen.insert(int(oracle.previewMode));
            soundsSeen.insert(int(oracle.soundMode));
        }
        // Assert what VARIED, not how often the loop ran: each rule must
        // have been decided both ways somewhere in the table.
        QCOMPARE(compared, 2 * 3 * 2 * 4 * 5 * 2 * 2);
        QVERIFY(visible > 0 && visible < compared);
        QVERIFY(visibleThreaded > 0);
        QVERIFY(hydrating > 0 && hydrating < compared);
        QVERIFY(namedById > 0 && namedById < compared);
        QVERIFY(ignored > 0 && ignored < compared);
        QCOMPARE(modesSeen.size(), 4);
        QCOMPARE(previewsSeen.size(), 3);
        QCOMPARE(soundsSeen.size(), 3);
    }

    // The rows the old rules turn on, spelled out: a regression here names
    // the rule instead of a table index.
    void theThreadAndRoomRulesByName()
    {
        World w = baseWorld();
        w.currentRoomId = kRoomA;
        w.activeRoomAtLatest = true;
        w.thread = FakeThread{QStringLiteral("$root"), false};
        TimelineEvent ev;
        ev.eventId = QStringLiteral("$e");
        ev.sender = kSender;

        // A main-timeline message in the open room at the latest: visible.
        QVERIFY(NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                    .roomVisibleAtLatest);
        // The thread root itself (threadRootId == eventId) is a main-timeline
        // event, not a threaded one.
        ev.threadRootId = ev.eventId;
        QVERIFY(NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                    .roomVisibleAtLatest);
        // A reply in a thread: the main timeline's position says nothing; the
        // panel shows that thread but is not at its latest reply.
        ev.threadRootId = QStringLiteral("$root");
        QVERIFY(!NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                     .roomVisibleAtLatest);
        w.thread->atLatest = true;
        QVERIFY(NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                    .roomVisibleAtLatest);
        // No panel at all.
        w.thread.reset();
        QVERIFY(!NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                     .roomVisibleAtLatest);
        // Hydrating only counts for the open room.
        w.activeRoomHydrating = true;
        QVERIFY(NotificationContextBuilder::build(gather(w, kRoomA, ev, false))
                    .roomHydrating);
        QVERIFY(!NotificationContextBuilder::build(gather(w, kRoomB, ev, false))
                     .roomHydrating);
        // An unnamed room is titled by its id.
        QCOMPARE(NotificationContextBuilder::build(gather(w, kRoomB, ev, false))
                     .roomName,
                 kRoomB);
    }

    // Phase 0 gathers no account (foreground = today's stamping); a host will.
    void theAccountIsCopiedAndEmptyByDefault()
    {
        World w = baseWorld();
        TimelineEvent ev;
        ev.eventId = QStringLiteral("$e");
        NotificationContextInputs in = gather(w, kRoomA, ev, false);
        NotificationManager::Context c = NotificationContextBuilder::build(in);
        QVERIFY(c.accountUserId.isEmpty());
        QVERIFY(c.accountLabel.isEmpty());
        QVERIFY(c == oldContextFor(w, kRoomA, ev, false));

        in.accountUserId = QStringLiteral("@b:example.org");
        in.accountLabel = QStringLiteral("Work");
        c = NotificationContextBuilder::build(in);
        QCOMPARE(c.accountUserId, QStringLiteral("@b:example.org"));
        QCOMPARE(c.accountLabel, QStringLiteral("Work"));
        // And that is the only difference.
        c.accountUserId.clear();
        c.accountLabel.clear();
        QVERIFY(c == oldContextFor(w, kRoomA, ev, false));
    }

    // ---- AppController's side of the factoring (source scans: the
    // controller needs a whole application to construct). Each asserts the
    // extent it reads is real before asserting anything about it.

    // notificationContextFor gathers and delegates; the rules are not
    // duplicated there, and the foreground's account stays empty (today's
    // stamping) in Phase 0. The gathering itself is pinned field by field:
    // gather() above is only a copy of it.
    void appControllerGathersEveryInputFromItsOwnSource()
    {
#ifndef LIGHTNING_SOURCE_DIR
        QSKIP("LIGHTNING_SOURCE_DIR not defined for this target");
#else
        const auto read = [](const char *rel) {
            QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR) + QLatin1String(rel));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                               : QString();
        };
        // Line comments go, so prose can neither satisfy nor break a pin.
        const auto code = [](const QString &text) {
            QStringList lines = text.split(QLatin1Char('\n'));
            for (QString &line : lines) {
                const int c = line.indexOf(QLatin1String("//"));
                if (c >= 0)
                    line.truncate(c);
            }
            return lines.join(QLatin1Char('\n'));
        };

        const QString src = read("/src/app/AppController.cpp");
        QVERIFY2(!src.isEmpty(), "AppController.cpp unreadable");
        const int at = src.indexOf(QStringLiteral(
            "NotificationManager::Context AppController::notificationContextFor("));
        QVERIFY2(at >= 0, "notificationContextFor is gone");
        const int end = src.indexOf(QStringLiteral("\n}\n"), at);
        QVERIFY(end > at);
        const QString body = code(src.mid(at, end - at));
        const QString flat = body.simplified();

        // The input fields, from the struct itself, so a field added later
        // and never gathered fails here.
        const QString header =
            code(read("/src/notifications/NotificationContextBuilder.h"));
        const int open = header.indexOf(QStringLiteral("struct NotificationContextInputs {"));
        const int close = header.indexOf(QStringLiteral("\n};"), open);
        QVERIFY2(open >= 0 && close > open, "NotificationContextInputs not found");
        QSet<QString> fields;
        const QRegularExpression decl(
            QStringLiteral(R"re(^\s*(?:QString|bool|int)\s+(\w+)\b)re"),
            QRegularExpression::MultilineOption);
        for (auto it = decl.globalMatch(header.mid(open, close - open)); it.hasNext();)
            fields.insert(it.next().captured(1));

        // Field -> the source the OLD body read for it (whitespace folded).
        const QHash<QString, QString> expected = {
            {QStringLiteral("selfUserId"), QStringLiteral("m_client->currentUserId()")},
            {QStringLiteral("roomId"), QStringLiteral("roomId")},
            {QStringLiteral("roomInfoName"), QStringLiteral("info.name")},
            {QStringLiteral("roomIsDirect"), QStringLiteral("info.isDirect")},
            {QStringLiteral("avatarMxc"), QStringLiteral(
                 "notifyRoomRow.value(QStringLiteral(\"avatarUrl\")).toString()")},
            {QStringLiteral("avatarColorKey"), QStringLiteral(
                 "notifyRoomRow.value(QStringLiteral(\"identityColorKey\")).toString()")},
            {QStringLiteral("themeId"), QStringLiteral("int(m_settings->theme())")},
            {QStringLiteral("roomMode"), QStringLiteral(
                 "m_settings->roomNotificationModeFor(uid, roomId)")},
            {QStringLiteral("localModeUnsynced"), QStringLiteral(
                 "m_settings->roomNotificationModeDecidesLocallyFor(uid, roomId)")},
            {QStringLiteral("previewMode"), QStringLiteral("noticePreviewFor(roomId)")},
            {QStringLiteral("notificationsEnabled"),
             QStringLiteral("m_settings->notificationsEnabled()")},
            {QStringLiteral("eventId"), QStringLiteral("event.eventId")},
            {QStringLiteral("eventThreadRootId"), QStringLiteral("event.threadRootId")},
            {QStringLiteral("fromThread"), QStringLiteral("fromThread")},
            {QStringLiteral("currentRoomId"), QStringLiteral("m_currentRoomId")},
            {QStringLiteral("activeRoomAtLatest"), QStringLiteral("m_activeRoomAtLatest")},
            {QStringLiteral("activeRoomHydrating"), QStringLiteral("m_activeRoomHydrating")},
            {QStringLiteral("threadPanelPresent"), QStringLiteral("m_thread != nullptr")},
            {QStringLiteral("threadPanelRootId"), QStringLiteral(
                 "m_thread ? m_thread->rootEventId() : QString()")},
            {QStringLiteral("threadPanelAtLatest"), QStringLiteral(
                 "m_thread && m_thread->viewerAtLatest()")},
            {QStringLiteral("initialSyncComplete"),
             QStringLiteral("m_client->initialSyncDone()")},
            {QStringLiteral("soundMode"), QStringLiteral("m_settings->notificationSound()")},
            {QStringLiteral("senderIsIgnored"), QStringLiteral(
                 "m_moderation && m_moderation->isIgnored(event.sender)")},
            // New with the sender label (2026-10-16); the old body had no
            // equivalent, so these are pinned to their intended sources.
            {QStringLiteral("senderMemberName"), QStringLiteral(
                 "m_client->displayNameFor(roomId, event.sender)")},
            {QStringLiteral("senderProfileName"), QStringLiteral(
                 "m_userProfiles ? m_userProfiles->profile(event.sender).displayName : QString()")},
        };
        const QSet<QString> account = {QStringLiteral("accountUserId"),
                                       QStringLiteral("accountLabel")};
        // The table covers the struct exactly: every field but the account.
        QSet<QString> tableAndAccount(expected.keyBegin(), expected.keyEnd());
        tableAndAccount.unite(account);
        QVERIFY2(tableAndAccount == fields,
                 "NotificationContextInputs and this table disagree; a new "
                 "field needs its source pinned here");

        // Every assignment in the real body.
        QHash<QString, QStringList> assigned;
        const QRegularExpression assign(
            QStringLiteral(R"re(\bin\.(\w+)\s*=(?!=)([^;]*);)re"));
        int assignments = 0;
        for (auto it = assign.globalMatch(body); it.hasNext();) {
            const auto m = it.next();
            assigned[m.captured(1)] << m.captured(2).simplified();
            ++assignments;
        }
        // Nothing touches `in` except those assignments and the build call
        // (a compound assignment or a mutating call would slip past them).
        int touches = 0;
        for (auto it = QRegularExpression(QStringLiteral(R"re(\bin\.)re"))
                           .globalMatch(body);
             it.hasNext(); it.next())
            ++touches;
        QCOMPARE(touches, assignments);
        QCOMPARE(assignments, expected.size());
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const QStringList rhs = assigned.value(it.key());
            QVERIFY2(rhs.size() == 1,
                     qPrintable(QStringLiteral("in.%1 assigned %2 times")
                                    .arg(it.key()).arg(rhs.size())));
            QVERIFY2(rhs.first() == it.value(),
                     qPrintable(QStringLiteral("in.%1 = %2; expected %3")
                                    .arg(it.key(), rhs.first(), it.value())));
        }
        // Phase 0 stamps the foreground's context with no account.
        for (const QString &a : account)
            QVERIFY2(!assigned.contains(a),
                     "Phase 0 stamps the foreground's context with an account");

        // The locals those right-hand sides read, and where they come from.
        for (const char *local : {
                 "const QString uid = m_settings->activeAccountUserId();",
                 "const RoomInfo info = m_client->roomInfo(roomId);",
                 "const QVariantMap notifyRoomRow = m_roomList->findRoom(roomId);"})
            QVERIFY2(flat.contains(QLatin1String(local)), local);
        QCOMPARE(flat.count(QStringLiteral(" uid =")), 1);
        QCOMPARE(flat.count(QStringLiteral(" info =")), 1);
        QCOMPARE(flat.count(QStringLiteral(" notifyRoomRow =")), 1);

        QVERIFY2(flat.contains(QStringLiteral(
                     "return lightning::notifications::NotificationContextBuilder::build(in);")),
                 "notificationContextFor no longer goes through the builder");
        QVERIFY2(!body.contains(QStringLiteral("roomVisibleAtLatest"))
                     && !body.contains(QStringLiteral("roomHydrating")),
                 "a context rule is duplicated in AppController");
#endif
    }

    // Both foreground clears are exactly today's clear, and nothing still
    // calls the old name from AppController.
    void appControllerClearsTheForegroundTableOnly()
    {
#ifndef LIGHTNING_SOURCE_DIR
        QSKIP("LIGHTNING_SOURCE_DIR not defined for this target");
#else
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        QCOMPARE(src.count(QStringLiteral("m_notifications->clearPendingFor(QString())")), 2);
        QCOMPARE(src.count(QStringLiteral("m_notifications->clearPending()")), 0);
        // The open-for-account signal is connected; the three-argument one
        // still routes.
        QVERIFY(src.contains(QStringLiteral("&NotificationManager::openRequestedForAccount")));
        QVERIFY(src.contains(QStringLiteral("&NotificationManager::openRequested, this")));
#endif
    }

    // The client keeps a raw pointer to the proof map, so the map must be
    // destroyed AFTER the client: members die in reverse declaration order.
    void theRevocationProofsAreDeclaredBeforeTheClient()
    {
#ifndef LIGHTNING_SOURCE_DIR
        QSKIP("LIGHTNING_SOURCE_DIR not defined for this target");
#else
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/src/app/AppController.h"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString header = QString::fromUtf8(f.readAll());
        const int proofs = header.indexOf(QStringLiteral(
            "lightning::accounts::RevocationProofs m_revocationProofs;"));
        const int client = header.indexOf(QStringLiteral(
            "std::unique_ptr<MatrixClient> m_client;"));
        QVERIFY2(proofs >= 0, "AppController no longer owns the proof map");
        QVERIFY2(client >= 0, "m_client's declaration moved; re-point this scan");
        QVERIFY2(proofs < client,
                 "the proof map is destroyed before the client that points at it");
#endif
    }

    // The builder is what a background host will call, so it must not reach
    // foreground state: NotificationManager.h is its only project include.
    void theBuilderIncludesNothingButNotificationManager()
    {
#ifndef LIGHTNING_SOURCE_DIR
        QSKIP("LIGHTNING_SOURCE_DIR not defined for this target");
#else
        const QRegularExpression include(
            // An ordinary literal: moc misparses a raw string holding "#include".
            QStringLiteral("^\\s*#\\s*include\\s*\"([^\"]+)\""),
            QRegularExpression::MultilineOption);
        int scanned = 0;
        for (const char *rel : {"/src/notifications/NotificationContextBuilder.h",
                                "/src/notifications/NotificationContextBuilder.cpp"}) {
            QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR) + QLatin1String(rel));
            QVERIFY2(f.open(QIODevice::ReadOnly), rel);
            const QString text = QString::fromUtf8(f.readAll());
            auto it = include.globalMatch(text);
            while (it.hasNext()) {
                const QString path = it.next().captured(1);
                QVERIFY2(path == QLatin1String("notifications/NotificationManager.h")
                             || path == QLatin1String(
                                 "notifications/NotificationContextBuilder.h"),
                         qPrintable(QStringLiteral("%1 includes %2")
                                        .arg(QLatin1String(rel), path)));
            }
            ++scanned;
        }
        QCOMPARE(scanned, 2);
#endif
    }
};

QTEST_GUILESS_MAIN(NotificationContextBuilderTest)
#include "NotificationContextBuilderTest.moc"
