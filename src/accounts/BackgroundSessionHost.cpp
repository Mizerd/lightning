#include "accounts/BackgroundSessionHost.h"

#include "app/SettingsManager.h"
#include "crypto/E2eeDiagnostics.h"
#include "matrix/RoomInfo.h"
#include "matrix/RoomUnreadRule.h"
#include "matrix/RustTimelineIngest.h"
#include "matrix/TimelineEvent.h"
#include "models/AccountActivityModel.h"
#include "notifications/NotificationContextBuilder.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(lcBgHost, "lightning.accounts.host")

namespace lightning::accounts {

namespace {

// mx_rust_bg_take counts-tuple flags (contract §3.2).
constexpr int kFlagHasUnread = 1;
constexpr int kFlagMarkedUnread = 2;
constexpr int kFlagThreadsOnly = 4;
constexpr int kFlagMentionInThreads = 8;
constexpr int kFlagEncrypted = 16;
constexpr int kFlagEncryptionKnown = 32;
constexpr int kFlagLowerBound = 64;

QString localpartOf(const QString &userId)
{
    if (!userId.startsWith(QLatin1Char('@')))
        return userId;
    const qsizetype colon = userId.indexOf(QLatin1Char(':'));
    return userId.mid(1, colon > 1 ? colon - 1 : -1);
}

// The queue types the host acts on (contract §3.5, plus the verification
// flow's end). Anything else is counted and dropped WITHOUT being parsed: a
// queue copy of a sticky Session type is read from the sticky slot instead
// (review L9). A body can quote one of these words, which costs one parse and
// nothing else: the type field decides below.
bool mayBeHandled(const QString &raw)
{
    static const QLatin1String kHandled[] = {
        QLatin1String("\"timeline_event\""),
        QLatin1String("\"event_decrypted\""),
        QLatin1String("\"event_redacted\""),
        QLatin1String("\"reaction_event\""),
        QLatin1String("\"verification_request_received\""),
        QLatin1String("\"verification_cancelled\""),
        QLatin1String("\"queue_overflow\""),
        QLatin1String("\"token_trace\""),
    };
    for (const QLatin1String &type : kHandled) {
        if (raw.contains(type))
            return true;
    }
    return false;
}

QJsonObject parseObject(const QByteArray &json)
{
    if (json.isEmpty() || json.startsWith("error: "))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isObject() ? doc.object() : QJsonObject();
}

} // namespace

BackgroundSessionHost::BackgroundSessionHost(Deps deps, Identity identity, void *handle,
                                             quint64 storeSerial, quint64 epoch,
                                             QObject *parent)
    : QObject(parent)
    , m_deps(std::move(deps))
    , m_identity(std::move(identity))
    , m_handle(handle)
    , m_storeSerial(storeSerial)
    , m_epoch(epoch)
{
    if (!m_deps.monotonicMs) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        m_deps.monotonicMs = [clock] { return clock->elapsed(); };
    }
}

BackgroundSessionHost::~BackgroundSessionHost() = default;

qint64 BackgroundSessionHost::now() const
{
    return m_deps.monotonicMs();
}

void *BackgroundSessionHost::takeHandle()
{
    m_phase = Phase::Stopping;
    m_catchUp.reset();
    return std::exchange(m_handle, nullptr);
}

void BackgroundSessionHost::beginCatchUp()
{
    m_catchUp.begin(now(), m_seenSessionVersion);
}

bool BackgroundSessionHost::drainOnce()
{
    if (!m_handle || !m_deps.api)
        return false;
    BgStatus status;
    if (!m_deps.api->bgPoll(m_handle, &status))
        return false;
    m_status = status;
    bool worked = false;
    if (m_catchUp.active())
        m_catchUp.sample(now(), status.notableUpdatesTotal);

    // 1. Badges: at most one take, only when something changed. Phase 1 asks
    //    for counts only (max_rows 0).
    if (status.dirty) {
        applyTake(m_deps.api->bgTake(m_handle, 0));
        worked = true;
    }
    // 2. Session state, only when its version moved.
    if (status.sessionStateVersion != m_seenSessionVersion) {
        m_seenSessionVersion = status.sessionStateVersion;
        applySessionState(m_deps.api->bgTakeSessionState(m_handle));
        worked = true;
        // A handler (login failed, signed out) may have handed the handle on.
        if (!m_handle)
            return true;
    }
    // 3. The queue: Notify events and the few Session ones the host handles.
    if (status.queuedEvents > 0) {
        for (int i = 0; i < kMaxEventsPerPass && m_handle; ++i) {
            const QString raw = m_deps.api->pollEvent(m_handle);
            if (raw.isEmpty() || raw.startsWith(QLatin1String("error: ")))
                break;
            handleQueued(raw);
        }
        worked = true;
        if (!m_handle)
            return true;
    }
    // 4. Custody: only when the token generation moved, or on revocation.
    if (takeCustodyIfMoved(status)) {
        worked = true;
        if (!m_handle)
            return true;
    }
    if (m_catchUp.active()
        && m_catchUp.complete(now(), m_initialSyncDone,
                              m_roomListState == QLatin1String("running"),
                              m_seenSessionVersion)) {
        m_catchUp.reset();
        Q_EMIT catchUpComplete();
    }
    return worked;
}

bool BackgroundSessionHost::takeCustodyIfMoved(const BgStatus &status)
{
    if (status.tokenGeneration == m_seenTokenGeneration && !(status.revoked && !m_revokedSeen))
        return false;
    m_seenTokenGeneration = status.tokenGeneration;
    if (status.revoked)
        m_revokedSeen = true;
    const CustodySnapshot custody = m_deps.api->takeCustody(m_handle);
    if (m_deps.onCustody)
        m_deps.onCustody(this, custody);
    return true;
}

void BackgroundSessionHost::drainTokensOnly()
{
    if (!m_handle || !m_deps.api)
        return;
    BgStatus status;
    if (!m_deps.api->bgPoll(m_handle, &status))
        return;
    m_status = status;
    takeCustodyIfMoved(status);
}

void BackgroundSessionHost::applyTake(const QByteArray &json)
{
    const QJsonObject take = parseObject(json);
    if (take.isEmpty())
        return;
    // A baseline or a rescan is a fresh whole picture: re-read the modes too.
    if (take.value(QStringLiteral("baseline")).toBool(false)
        || take.value(QStringLiteral("rescan")).toBool(false))
        m_modes.clear();
    // The WHOLE table every time: a room absent from it is zero.
    QHash<QString, RoomCounts> rooms;
    for (const QJsonValue &value : take.value(QStringLiteral("counts")).toArray()) {
        const QJsonArray tuple = value.toArray();
        if (tuple.size() < 4)
            continue;
        const QString roomId = tuple.at(0).toString();
        if (roomId.isEmpty())
            continue;
        rooms.insert(roomId, RoomCounts{tuple.at(1).toInt(0), tuple.at(2).toInt(0),
                                        tuple.at(3).toInt(0)});
    }
    m_rooms = std::move(rooms);
    m_invites = take.value(QStringLiteral("invites")).toInt(0);
    recomputeSummary(false);
}

void BackgroundSessionHost::recomputeSummary(bool force)
{
    if (m_quiet)
        return;
    int unreadRooms = 0;
    int mentions = 0;
    bool lowerBound = false;
    QSet<QString> unreadNow;
    for (auto it = m_rooms.cbegin(); it != m_rooms.cend(); ++it) {
        RoomInfo room;
        room.id = it.key();
        room.membership = RoomInfo::Joined;
        room.unreadCount = it->unread;
        room.highlightCount = it->highlight;
        room.hasUnreadMessages = (it->flags & kFlagHasUnread) != 0;
        room.markedUnread = (it->flags & kFlagMarkedUnread) != 0;
        room.unreadThreadsOnly = (it->flags & kFlagThreadsOnly) != 0;
        room.mentionInThreads = (it->flags & kFlagMentionInThreads) != 0;
        room.encrypted = (it->flags & kFlagEncrypted) != 0;
        room.encryptionKnown = (it->flags & kFlagEncryptionKnown) != 0;
        // The foreground's rule, with THIS account's mode for the room. The
        // mode is a settings read, so only for a room with something unread.
        if (!roomunread::rawUnread(room))
            continue;
        const int mode = modeFor(room.id);
        if (!roomunread::countsAsUnread(room, mode))
            continue;
        ++unreadRooms;
        unreadNow.insert(room.id);
        mentions += roomunread::mentionCount(room, mode);
        if (it->flags & kFlagLowerBound)
            lowerBound = true;
    }
    // Read elsewhere withdraws this account's cards for that room; a room
    // becoming unread arms the withdrawal (NotificationManager's rule).
    if (m_deps.notifications) {
        for (const QString &roomId : std::as_const(m_unreadRooms)) {
            if (!unreadNow.contains(roomId))
                m_deps.notifications->observeRoomUnreadFor(m_identity.userId, roomId, false);
        }
        for (const QString &roomId : std::as_const(unreadNow)) {
            if (!m_unreadRooms.contains(roomId))
                m_deps.notifications->observeRoomUnreadFor(m_identity.userId, roomId, true);
        }
    }
    m_unreadRooms = std::move(unreadNow);
    const bool same = m_haveSummary && unreadRooms == m_lastUnreadRooms
        && mentions == m_lastMentions && m_invites == m_lastInvites
        && lowerBound == m_lastLowerBound;
    if (same && !force)
        return;
    m_haveSummary = true;
    m_lastUnreadRooms = unreadRooms;
    m_lastMentions = mentions;
    m_lastInvites = m_invites;
    m_lastLowerBound = lowerBound;
    if (m_deps.activity) {
        AccountActivityModel::Summary summary;
        summary.unreadRooms = unreadRooms;
        summary.mentions = mentions;
        summary.invites = m_invites;
        summary.mentionsLowerBound = lowerBound;
        m_deps.activity->setSummary(m_identity.userId, m_epoch, summary);
    }
    Q_EMIT summaryUpdated();
}

int BackgroundSessionHost::modeFor(const QString &roomId)
{
    const auto it = m_modes.constFind(roomId);
    if (it != m_modes.cend())
        return it.value();
    const int mode = m_deps.settings
        ? m_deps.settings->roomNotificationModeFor(m_identity.userId, roomId)
        : roomunread::kModeAllMessages;
    m_modes.insert(roomId, mode);
    return mode;
}

void BackgroundSessionHost::refreshModes(const QString &roomId)
{
    if (roomId.isEmpty())
        m_modes.clear();
    else
        m_modes.remove(roomId);
    if (m_handle && m_deps.api && m_deps.settings) {
        QStringList rules =
            m_deps.settings->locallyDecidedNotificationRoomsFor(m_identity.userId);
        rules.sort();
        if (rules != m_localRules) {
            m_localRules = rules;
            m_deps.api->bgSetLocalRules(m_handle, rules);
        }
    }
    recomputeSummary(false);
}

bool BackgroundSessionHost::changed(const QString &type, const QJsonObject &payload)
{
    const auto it = m_sticky.constFind(type);
    if (it != m_sticky.cend() && it.value() == payload)
        return false;
    m_sticky.insert(type, payload);
    return true;
}

void BackgroundSessionHost::applySessionState(const QByteArray &json)
{
    const QJsonObject state = parseObject(json).value(QStringLiteral("state")).toObject();
    if (state.isEmpty())
        return;

    const auto has = [&state](const char *type) {
        return state.contains(QLatin1String(type));
    };
    const auto payload = [&state](const char *type) {
        return state.value(QLatin1String(type)).toObject();
    };

    if (has("initial_sync_done") && changed(QStringLiteral("initial_sync_done"),
                                            payload("initial_sync_done")))
        m_initialSyncDone = true;
    if (has("ignored_users_list")) {
        const QJsonObject list = payload("ignored_users_list");
        if (changed(QStringLiteral("ignored_users_list"), list)
            && list.value(QStringLiteral("ok")).toBool(true)) {
            m_ignored.clear();
            for (const QJsonValue &user : list.value(QStringLiteral("users")).toArray())
                m_ignored.insert(user.toString());
        }
    }
    if (has("ignored_users_changed")) {
        const QJsonObject list = payload("ignored_users_changed");
        if (changed(QStringLiteral("ignored_users_changed"), list)) {
            m_ignored.clear();
            for (const QJsonValue &user : list.value(QStringLiteral("users")).toArray())
                m_ignored.insert(user.toString());
        }
    }
    if (has("room_list_sync_state")) {
        const QJsonObject rl = payload("room_list_sync_state");
        if (changed(QStringLiteral("room_list_sync_state"), rl)) {
            m_roomListState = rl.value(QStringLiteral("state")).toString();
            if (m_phase == Phase::Running && m_roomListState == QLatin1String("offline"))
                Q_EMIT connectionLost();
        }
    }
    if (has("sync_error")) {
        const QJsonObject error = payload("sync_error");
        if (changed(QStringLiteral("sync_error"), error)
            && error.value(QStringLiteral("rate_limited")).toBool(false)) {
            Q_EMIT rateLimited(
                qint64(error.value(QStringLiteral("retry_after_ms")).toDouble(0)));
        }
    }

    // The login result last: its handlers can hand the handle on.
    if (m_phase == Phase::Starting && has("login_ok")
        && changed(QStringLiteral("login_ok"), payload("login_ok"))) {
        m_phase = Phase::Running;
        // Start syncing at once: the catch-up semaphore was taken before the
        // handle was created.
        m_deps.api->startSync(m_handle);
        qCInfo(lcBgHost) << "background account signed in" << "acct=" << m_identity.slug;
        Q_EMIT loginSucceeded();
    } else if (m_phase == Phase::Starting && has("login_failed")
               && changed(QStringLiteral("login_failed"), payload("login_failed"))) {
        const QJsonObject failed = payload("login_failed");
        FailureKind kind = FailureKind::Transient;
        if (failed.value(QStringLiteral("stage")).toString() == QLatin1String("store")) {
            const QString reason = failed.value(QStringLiteral("reason")).toString();
            if (reason == QLatin1String("store_in_use"))
                kind = FailureKind::StoreInUse;
            else if (reason == QLatin1String("crypto_account_missing")
                     || reason == QLatin1String("crypto_account_mismatch"))
                kind = FailureKind::EncryptionDataMissing;
        }
        // The reason word only; the message can quote the server.
        qCInfo(lcBgHost) << "background account could not start" << "acct=" << m_identity.slug
                         << "reason=" << failed.value(QStringLiteral("reason")).toString();
        Q_EMIT loginFailed(kind);
        if (!m_handle)
            return;
    }
    if (has("logged_out") && changed(QStringLiteral("logged_out"), payload("logged_out"))) {
        // No result word is not a success (review L12): the pool maps only
        // "ok" and "already_invalid" to a server sign-out that happened.
        Q_EMIT loggedOut(payload("logged_out").value(QStringLiteral("result")).toString());
    }
}

void BackgroundSessionHost::handleQueued(const QString &raw)
{
    if (m_quiet || !mayBeHandled(raw)) {
        ++m_droppedEvents;
        return;
    }
    const QJsonObject event = parseObject(raw.toUtf8());
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("timeline_event")) {
        handleTimelineEvent(event, false);
        return;
    }
    if (type == QLatin1String("event_decrypted")) {
        handleTimelineEvent(event, true);
        return;
    }
    if (type == QLatin1String("event_redacted")) {
        const QString redacts = event.value(QStringLiteral("redacts")).toString();
        if (!redacts.isEmpty() && m_deps.notifications)
            m_deps.notifications->withdrawEvent(redacts);
        return;
    }
    if (type == QLatin1String("reaction_event")) {
        // Reactions raise no desktop card today (the Activity Center is the
        // foreground's, and its background inbox is Phase 3).
        return;
    }
    if (type == QLatin1String("verification_request_received")) {
        // Never parked, never auto-cancelled (design §9.2): a labelled card,
        // and the request stays pending in the SDK for when the account is
        // opened. The card says nothing but that.
        const QString key = verificationCardKey(event.value(QStringLiteral("flow_id")).toString());
        // Notifications off is off for every account (§18 round 1, L5).
        if (m_deps.settings && !m_deps.settings->notificationsEnabled())
            return;
        if (m_deps.notifications && !m_verificationCards.contains(key)) {
            m_verificationCards.insert(key);
            // The label is a display name from the server: isolated (FSI ..
            // PDI) so its direction cannot reorder the title; and the card is
            // stamped for THIS account, so "Open this account" opens it.
            m_deps.notifications->showGeneric(
                tr("Verification request · %1")
                    .arg(QChar(0x2068) + m_identity.label + QChar(0x2069)),
                tr("Open this account to answer it."), QString(), QString(), true, key,
                m_identity.userId);
        }
        return;
    }
    if (type == QLatin1String("verification_cancelled")) {
        // The other side gave up (or the request timed out): the card would
        // only send the user to a request that no longer exists (review L10).
        const QString key = verificationCardKey(event.value(QStringLiteral("flow_id")).toString());
        if (m_verificationCards.remove(key) && m_deps.notifications)
            m_deps.notifications->withdrawGeneric(key);
        return;
    }
    if (type == QLatin1String("token_trace")) {
        // Rust enqueues it only under LIGHTNING_TOKEN_TRACE: a rotation's
        // generation, its wait's outcome and milliseconds, never a token
        // (sessiontokens.rs). Logged so a background rotation's answer path
        // can be seen working (24a485c5 for live accounts).
        qCInfo(lcBgHost).noquote()
            << "token trace acct=" + m_identity.slug
            << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
        return;
    }
    if (type == QLatin1String("queue_overflow")) {
        // Badges do not ride the queue (the aggregator's dirty flag), so only
        // notify-worthy events can have been lost.
        qCWarning(lcBgHost) << "a background account's event queue overflowed"
                            << "acct=" << m_identity.slug;
        return;
    }
    // Every other Session type is read from the sticky slot; a queue copy is
    // counted and dropped.
    ++m_droppedEvents;
}

QString BackgroundSessionHost::verificationCardKey(const QString &flowId) const
{
    return QStringLiteral("bg-verify:") + m_identity.slug + QLatin1Char(':')
        + matrix::e2ee::redactId(flowId);
}

void BackgroundSessionHost::handleTimelineEvent(const QJsonObject &event, bool decrypted)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    TimelineEvent timelineEvent = matrix::rust_timeline::syncEventFromPayload(
        event.value(QStringLiteral("event")).toObject(), roomId);
    if (roomId.isEmpty() || timelineEvent.eventId.isEmpty())
        return;
    if (decrypted && timelineEvent.undecryptable)
        return;
    timelineEvent.roomId = roomId;
    const QJsonObject bg = event.value(QStringLiteral("bg")).toObject();
    // Rust resolved the name from the room's member list; unresolved, the
    // localpart, never another account's member cache.
    const QString senderName = bg.value(QStringLiteral("sender_display_name")).toString();
    timelineEvent.senderDisplayName =
        senderName.isEmpty() ? localpartOf(timelineEvent.sender) : senderName;
    if (timelineEvent.undecryptable && timelineEvent.body.isEmpty()) {
        // The foreground's own words for it, so the translation is shared.
        timelineEvent.body =
            QCoreApplication::translate("RustSdkMatrixClient", "[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
    }
    // An event_decrypted for an id this host was handed as an undecryptable
    // timeline_event replaces that card. One Rust forwarded only once its
    // decrypted facts passed the filter (contract §3.5, review WP1-R N2) was
    // never seen here: it is a NEW candidate, and processDecrypted() would
    // drop it for not being a live UTD.
    bool lateForwarded = false;
    if (decrypted) {
        lateForwarded = m_forwardedUtds.remove(timelineEvent.eventId) == 0;
    } else if (timelineEvent.undecryptable) {
        rememberForwardedUtd(timelineEvent.eventId);
    }
    const NotificationManager::Context context = contextFor(roomId, bg, timelineEvent);
    m_lastContext = context;
    ++m_notifyEvents;
    if (lateForwarded)
        ++m_lateDecryptedEvents;
    if (!m_deps.notifications)
        return;
    if (decrypted && !lateForwarded)
        m_deps.notifications->processDecrypted(timelineEvent, context);
    else
        m_deps.notifications->processEvent(timelineEvent, context);
}

void BackgroundSessionHost::rememberForwardedUtd(const QString &eventId)
{
    // Bounded like NotificationManager's own memory of live UTDs: past ten
    // minutes a decryption no longer notifies anyway.
    constexpr qint64 kMemoryMs = 10 * 60 * 1000;
    constexpr int kMaxRemembered = 512;
    const qint64 at = now();
    for (auto it = m_forwardedUtds.begin(); it != m_forwardedUtds.end();) {
        if (at - it.value() > kMemoryMs)
            it = m_forwardedUtds.erase(it);
        else
            ++it;
    }
    if (m_forwardedUtds.size() >= kMaxRemembered) {
        auto oldest = m_forwardedUtds.begin();
        for (auto it = m_forwardedUtds.begin(); it != m_forwardedUtds.end(); ++it) {
            if (it.value() < oldest.value())
                oldest = it;
        }
        m_forwardedUtds.erase(oldest);
    }
    m_forwardedUtds.insert(eventId, at);
}

NotificationManager::Context BackgroundSessionHost::contextFor(const QString &roomId,
                                                               const QJsonObject &bg,
                                                               const TimelineEvent &event)
{
    lightning::notifications::NotificationContextInputs in;
    const QString &uid = m_identity.userId;
    in.accountUserId = uid;
    in.accountLabel = m_identity.label;
    in.selfUserId = uid;
    in.roomId = roomId;
    in.roomInfoName = bg.value(QStringLiteral("room_name")).toString();
    in.roomIsDirect = bg.value(QStringLiteral("is_direct")).toBool(false);
    // Never fetched through the foreground's media bridge, which would use
    // another account's token for this room (R1-H7).
    in.avatarMxc = QString();
    in.avatarColorKey = roomId;
    if (m_deps.settings) {
        in.themeId = int(m_deps.settings->theme());
        in.roomMode = m_deps.settings->roomNotificationModeFor(uid, roomId);
        in.localModeUnsynced = m_deps.settings->roomNotificationModeDecidesLocallyFor(uid, roomId);
        // The room's encryption as the last take knew it; an unlisted room is
        // "unknown", and the stricter preview level wins, as the foreground's
        // noticePreviewFor() does.
        const auto facts = m_rooms.constFind(roomId);
        const bool known = facts != m_rooms.cend();
        in.previewMode = m_deps.settings->effectiveNotificationPreview(
            known && (facts->flags & kFlagEncrypted),
            known && (facts->flags & kFlagEncryptionKnown));
        in.notificationsEnabled = m_deps.settings->notificationsEnabled();
        in.soundMode = m_deps.settings->notificationSound();
    }
    in.eventId = event.eventId;
    in.eventThreadRootId = event.threadRootId;
    in.fromThread = false;
    // Nothing of a background account is on screen.
    in.currentRoomId = QString();
    in.activeRoomAtLatest = false;
    in.activeRoomHydrating = false;
    in.threadPanelPresent = false;
    in.initialSyncComplete = m_initialSyncDone;
    in.senderIsIgnored = m_ignored.contains(event.sender);
    in.senderMemberName = event.senderDisplayName;
    in.senderProfileName = QString();
    return lightning::notifications::NotificationContextBuilder::build(in);
}

} // namespace lightning::accounts
