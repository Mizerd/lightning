#include "matrix/CppHttpMatrixClient.h"

#include "app/SettingsManager.h"
#include "matrix/MediaHelpers.h"
#include "storage/CacheStore.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

Q_LOGGING_CATEGORY(lcHttp, "matrix.http")

namespace {

QString sanitizeHomeserver(const QString &raw)
{
    QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    if (!s.startsWith(QLatin1String("http://")) &&
        !s.startsWith(QLatin1String("https://"))) {
        s = QLatin1String("https://") + s;
    }
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

QString extractMatrixError(const QByteArray &body, const QString &fallback)
{
    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return fallback;
    const auto obj = doc.object();
    const QString err = obj.value(QStringLiteral("error")).toString();
    if (err.isEmpty())
        return fallback;
    const QString code = obj.value(QStringLiteral("errcode")).toString();
    if (!code.isEmpty())
        return QStringLiteral("%1 (%2)").arg(err, code);
    return err;
}

QString percentPath(const QString &segment)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(segment));
}

} // namespace

CppHttpMatrixClient::CppHttpMatrixClient(SettingsManager *settings, QObject *parent)
    : MatrixClient(parent)
    , m_settings(settings)
    , m_nam(new QNetworkAccessManager(this))
{
    m_syncRetryTimer.setSingleShot(true);
    QObject::connect(&m_syncRetryTimer, &QTimer::timeout, this, [this] {
        if (m_syncActive)
            startNextSync();
    });
}

CppHttpMatrixClient::~CppHttpMatrixClient() = default;

QUrl CppHttpMatrixClient::endpoint(const QString &path) const
{
    return QUrl(m_homeserver + QLatin1String("/_matrix/client/v3") + path);
}

QUrl CppHttpMatrixClient::mediaEndpoint(const QString &path) const
{
    return QUrl(m_homeserver + QLatin1String("/_matrix/media/v3") + path);
}

void CppHttpMatrixClient::applyBearer(QNetworkRequest &request) const
{
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + m_accessToken.toUtf8());
    }
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", "matrix-client/0.3");
}

void CppHttpMatrixClient::setState(ConnectionState s)
{
    if (m_state == s)
        return;
    m_state = s;
    Q_EMIT connectionStateChanged(m_state);
}

// -------- session lifecycle --------

void CppHttpMatrixClient::openCacheFor(const QString &userId)
{
    m_cache = std::make_unique<CacheStore>(this);
    if (!m_cache->openFor(userId)) {
        qCWarning(lcHttp) << "cache open failed; running without cache";
        m_cache.reset();
    }
}

void CppHttpMatrixClient::closeAndClearCache()
{
    if (m_cache) {
        m_cache->clearAll();
        m_cache->close();
        m_cache.reset();
    }
}

int CppHttpMatrixClient::loadCachedState()
{
    if (!m_cache)
        return 0;
    const auto rooms = m_cache->loadRooms();
    int visibleRoomCount = 0;
    for (const auto &r : rooms) {
        RoomInfo copy = r;
        copy.members = m_cache->loadMembers(r.id);
        m_rooms.insert(r.id, copy);
        m_timelines[r.id] = m_cache->loadTimeline(r.id);
        if (!copy.isSpace)
            ++visibleRoomCount;
    }
    if (!rooms.isEmpty())
        Q_EMIT roomsChanged();
    for (const auto &r : rooms)
        Q_EMIT timelineReset(r.id);
    qCInfo(lcHttp) << "cache restore: rooms=" << rooms.size()
                   << "visible_rooms=" << visibleRoomCount
                   << "sync_token_len=" << m_syncToken.size();
    return visibleRoomCount;
}

void CppHttpMatrixClient::login(const QString &homeserver,
                                const QString &user,
                                const QString &password)
{
    const QString hs = sanitizeHomeserver(homeserver);
    if (hs.isEmpty()) {
        Q_EMIT loginFailed(tr("Homeserver URL is empty or invalid."));
        return;
    }
    if (user.trimmed().isEmpty()) {
        Q_EMIT loginFailed(tr("Username is required."));
        return;
    }
    m_homeserver = hs;
    setState(Connecting);

    const QJsonObject identifier{
        { QStringLiteral("type"), QStringLiteral("m.id.user") },
        { QStringLiteral("user"), user.trimmed() },
    };
    const QJsonObject body{
        { QStringLiteral("type"), QStringLiteral("m.login.password") },
        { QStringLiteral("identifier"), identifier },
        { QStringLiteral("password"), password },
        { QStringLiteral("initial_device_display_name"),
          QStringLiteral("Native Matrix Client") },
    };

    QNetworkRequest req(endpoint(QStringLiteral("/login")));
    applyBearer(req);

    QNetworkReply *reply = m_nam->post(
        req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = extractMatrixError(
                reply->readAll(),
                status == 403 ? tr("Invalid username or password.")
                              : reply->errorString());
            qCWarning(lcHttp) << "login failed" << status;
            m_homeserver.clear();
            setState(Error);
            Q_EMIT loginFailed(msg);
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            m_homeserver.clear();
            setState(Error);
            Q_EMIT loginFailed(tr("Malformed login response from homeserver."));
            return;
        }
        const auto obj = doc.object();
        m_accessToken = obj.value(QStringLiteral("access_token")).toString();
        m_userId      = obj.value(QStringLiteral("user_id")).toString();
        m_deviceId    = obj.value(QStringLiteral("device_id")).toString();
        if (m_accessToken.isEmpty() || m_userId.isEmpty()) {
            m_homeserver.clear();
            m_accessToken.clear();
            m_userId.clear();
            m_deviceId.clear();
            setState(Error);
            Q_EMIT loginFailed(tr("Login response missing required fields."));
            return;
        }
        m_loggedIn = true;
        m_syncToken.clear();
        m_rooms.clear();
        m_timelines.clear();
        m_pendingSends.clear();
        if (m_initialSyncDone) {
            m_initialSyncDone = false;
            Q_EMIT initialSyncDoneChanged();
        }
        if (m_settings) {
            m_settings->saveSession(m_homeserver, m_userId, m_deviceId, m_accessToken);
            // A fresh login starts from a new since-less sync. Do not let a
            // previous run's resume token survive for the same MXID.
            m_settings->setSyncToken({});
        }
        // Fresh session gets a fresh cache — different user must not see the
        // previous user's history.
        if (m_cache) { m_cache->clearAll(); m_cache->close(); m_cache.reset(); }
        openCacheFor(m_userId);
        setState(Disconnected);
        qCInfo(lcHttp) << "login ok for" << m_userId;
        Q_EMIT loginSucceeded(m_userId);
    });
}

bool CppHttpMatrixClient::restoreSession()
{
    if (!m_settings || !m_settings->hasSession())
        return false;

    m_homeserver  = sanitizeHomeserver(m_settings->homeserverUrl());
    m_userId      = m_settings->userId();
    m_deviceId    = m_settings->deviceId();
    m_accessToken = m_settings->accessToken();
    m_syncToken   = m_settings->syncToken();

    if (m_homeserver.isEmpty() || m_userId.isEmpty() || m_accessToken.isEmpty()) {
        clearLocalSession(true);
        return false;
    }

    setState(Connecting);
    openCacheFor(m_userId);
    const int cachedVisibleRoomCount = loadCachedState();
    if (cachedVisibleRoomCount == 0 && !m_syncToken.isEmpty()) {
        // A /sync since token is only useful together with the local state
        // that token advances from. If the SQLite cache is empty, missing,
        // or only contains Space rooms that are hidden from the visible room
        // list, an incremental sync can correctly return no joined-room
        // objects and leave the app looking empty. Force a since-less
        // initial sync so the server sends a full room snapshot again.
        qCWarning(lcHttp)
            << "discarding stored sync token because cache has no visible rooms;"
            << "forcing initial sync";
        m_syncToken.clear();
        m_settings->setSyncToken({});
    }
    doWhoami();
    return true;
}

void CppHttpMatrixClient::doWhoami()
{
    QNetworkRequest req(endpoint(QStringLiteral("/account/whoami")));
    applyBearer(req);
    QNetworkReply *reply = m_nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            QString msg = (status == 401)
                ? tr("Session expired — please sign in again.")
                : extractMatrixError(reply->readAll(), reply->errorString());
            qCWarning(lcHttp) << "whoami failed" << status;
            clearLocalSession(true);
            Q_EMIT loginFailed(msg);
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject() ||
            !doc.object().contains(QStringLiteral("user_id"))) {
            clearLocalSession(true);
            Q_EMIT loginFailed(tr("Invalid /whoami response."));
            return;
        }
        m_userId  = doc.object().value(QStringLiteral("user_id")).toString();
        m_loggedIn = true;
        setState(Disconnected);
        qCInfo(lcHttp) << "session restored for" << m_userId;
        Q_EMIT loginSucceeded(m_userId);
    });
}

void CppHttpMatrixClient::logout()
{
    if (!m_loggedIn || m_accessToken.isEmpty() || m_homeserver.isEmpty()) {
        clearLocalSession(true);
        Q_EMIT loggedOut();
        return;
    }
    stopSync();
    QNetworkRequest req(endpoint(QStringLiteral("/logout")));
    applyBearer(req);
    QNetworkReply *reply = m_nam->post(req, QByteArray("{}"));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            qCWarning(lcHttp) << "logout returned error, clearing locally anyway";
        clearLocalSession(true);
        Q_EMIT loggedOut();
    });
}

void CppHttpMatrixClient::clearLocalSession(bool clearPersisted)
{
    stopSync();
    m_loggedIn = false;
    m_accessToken.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_syncToken.clear();
    m_homeserver.clear();
    m_rooms.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    m_lastReceiptSent.clear();
    if (m_initialSyncDone) {
        m_initialSyncDone = false;
        Q_EMIT initialSyncDoneChanged();
    }
    if (clearPersisted) {
        if (m_settings) m_settings->clearSession();
        closeAndClearCache();
    }
    Q_EMIT roomsChanged();
    setState(Disconnected);
}

// -------- sync loop --------

void CppHttpMatrixClient::startSync()
{
    if (!m_loggedIn || m_syncActive)
        return;
    m_syncActive = true;
    setState(Syncing);
    startNextSync();
}

void CppHttpMatrixClient::stopSync()
{
    m_syncActive = false;
    m_syncRetryTimer.stop();
    if (m_syncReply) {
        m_syncReply->abort();
        m_syncReply = nullptr;
    }
    if (m_state == Syncing)
        setState(Disconnected);
}

void CppHttpMatrixClient::startNextSync()
{
    if (!m_syncActive || !m_loggedIn)
        return;

    // v0.4.6 — the initial sync (no since token) uses timeout=0 so the
    // server returns current state immediately instead of long-polling.
    // Follow-up syncs long-poll with timeout=30000. Matches the Matrix
    // spec recommendation and makes "still loading rooms" feel snappy
    // instead of dead.
    const bool initial = m_syncToken.isEmpty();

    QUrl url = endpoint(QStringLiteral("/sync"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("timeout"),
                   initial ? QStringLiteral("0") : QStringLiteral("30000"));
    if (initial) {
        // Redundant for a spec-compliant since-less sync, but harmless and
        // useful for homeservers that are conservative about initial state.
        q.addQueryItem(QStringLiteral("full_state"), QStringLiteral("true"));
    } else {
        q.addQueryItem(QStringLiteral("since"), m_syncToken);
    }
    url.setQuery(q);

    QNetworkRequest req(url);
    applyBearer(req);
    // Long-poll: allow 30s server-side timeout + response streaming.
    // 60s hard cap avoids indefinite hangs on stalled TLS connections but
    // is comfortably above the 30s the server may hold the connection.
    req.setTransferTimeout(initial ? 30000 : 60000);

    qCInfo(lcHttp) << "sync request:"
                   << (initial ? "INITIAL (timeout=0)"
                               : "continuation (timeout=30000)")
                   << "url=" << url.toString(QUrl::RemoveQuery)
                              + (initial ? QStringLiteral("?timeout=0&full_state=true")
                                         : QStringLiteral("?timeout=30000&since=<redacted>"));

    m_syncReply = m_nam->get(req);
    QPointer<QNetworkReply> guard = m_syncReply;
    QObject::connect(m_syncReply.data(), &QNetworkReply::finished, this,
                     [this, guard, initial] {
        if (!guard)
            return;
        QNetworkReply *reply = guard.data();
        reply->deleteLater();
        m_syncReply = nullptr;

        if (reply->error() == QNetworkReply::OperationCanceledError)
            return;
        if (!m_syncActive)
            return;

        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 401) {
                qCWarning(lcHttp) << "sync 401 — session expired";
                Q_EMIT errorOccurred(tr("Session expired during sync."));
                clearLocalSession(true);
                Q_EMIT loggedOut();
                return;
            }
            qCWarning(lcHttp) << "sync error status=" << status
                              << "net=" << reply->errorString();
            Q_EMIT errorOccurred(tr("Sync error (%1): %2")
                                 .arg(status)
                                 .arg(reply->errorString()));
            m_syncRetryTimer.start(m_syncBackoffMs);
            return;
        }

        const QByteArray body = reply->readAll();
        const auto doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            qCWarning(lcHttp) << "sync malformed body, size=" << body.size();
            Q_EMIT errorOccurred(tr("Malformed /sync response."));
            m_syncRetryTimer.start(m_syncBackoffMs);
            return;
        }
        qCInfo(lcHttp) << "sync response ok" << (initial ? "(initial)" : "(continuation)")
                       << "status=" << status
                       << "size=" << body.size();
        handleSyncResponse(doc.object());
        startNextSync();
    });
}

void CppHttpMatrixClient::handleSyncResponse(const QJsonObject &obj)
{
    const QString nextBatch = obj.value(QStringLiteral("next_batch")).toString();
    if (!nextBatch.isEmpty()) {
        m_syncToken = nextBatch;
        if (m_settings)
            m_settings->setSyncToken(m_syncToken);
    }
    const auto roomsObj = obj.value(QStringLiteral("rooms")).toObject();
    const auto joined = roomsObj.value(QStringLiteral("join")).toObject();

    qCInfo(lcHttp) << "sync parse: joined=" << joined.size()
                   << "invited=" << roomsObj.value(QStringLiteral("invite")).toObject().size()
                   << "left=" << roomsObj.value(QStringLiteral("leave")).toObject().size()
                   << "next_batch_len=" << nextBatch.size();

    if (!joined.isEmpty())
        processJoinedRooms(joined);

    // v0.4.6: mark initial sync complete after the first response is
    // parsed, even if `join` was empty (the account may legitimately be
    // in zero joined rooms). QML uses this to switch from "Loading
    // rooms…" to "No joined rooms" / "No rooms in this Space".
    if (!m_initialSyncDone) {
        m_initialSyncDone = true;
        qCInfo(lcHttp) << "initial sync complete; rooms in memory ="
                       << m_rooms.size();
        Q_EMIT initialSyncDoneChanged();
        // Emit roomsChanged unconditionally on first response so any
        // model already bound to the client picks up the "0 rooms" case
        // as a real state, not the pre-sync default.
        Q_EMIT roomsChanged();
    }
}

void CppHttpMatrixClient::processStateEvent(RoomInfo &room, const QJsonObject &ev)
{
    const QString type = ev.value(QStringLiteral("type")).toString();
    const auto content = ev.value(QStringLiteral("content")).toObject();
    if (type == QLatin1String("m.room.name")) {
        const auto n = content.value(QStringLiteral("name")).toString();
        if (!n.isEmpty())
            room.name = n;
    } else if (type == QLatin1String("m.room.topic")) {
        room.topic = content.value(QStringLiteral("topic")).toString();
    } else if (type == QLatin1String("m.room.avatar")) {
        room.avatarUrl = content.value(QStringLiteral("url")).toString();
    } else if (type == QLatin1String("m.room.encryption")) {
        room.encrypted = true;
    } else if (type == QLatin1String("m.room.canonical_alias")) {
        if (room.name.isEmpty()) {
            const auto alias = content.value(QStringLiteral("alias")).toString();
            if (!alias.isEmpty())
                room.name = alias;
        }
    } else if (type == QLatin1String("m.room.member")) {
        const QString membership = content.value(QStringLiteral("membership")).toString();
        const QString stateKey = ev.value(QStringLiteral("state_key")).toString();
        if (stateKey.isEmpty()) return;
        if (membership == QLatin1String("leave") ||
            membership == QLatin1String("ban")) {
            room.members.remove(stateKey);
            if (m_cache) m_cache->saveMember(room.id,
                                             MemberInfo{ stateKey, {}, {} });
            return;
        }
        MemberInfo m;
        m.userId       = stateKey;
        m.displayName  = content.value(QStringLiteral("displayname")).toString();
        m.avatarMxcUrl = content.value(QStringLiteral("avatar_url")).toString();
        room.members.insert(stateKey, m);
        if (m_cache) m_cache->saveMember(room.id, m);
    } else if (type == QLatin1String("m.room.create")) {
        // v0.4.2: Spaces. A room is a Space when its creation content
        // declares type == "m.space". This event only fires once per room
        // so we set the flag idempotently.
        const QString roomType = content.value(QStringLiteral("type")).toString();
        room.isSpace = (roomType == QLatin1String("m.space"));
    } else if (type == QLatin1String("m.space.child")) {
        // v0.4.2: Space hierarchy edges. The state_key is the child room
        // id. An active edge carries a non-empty `via` array with servers
        // that can be used to join. Removing the edge produces a state
        // event whose content is either empty {} or missing `via`.
        const QString childRoomId = ev.value(QStringLiteral("state_key")).toString();
        if (childRoomId.isEmpty()) return;
        const auto via = content.value(QStringLiteral("via")).toArray();
        const bool active = !via.isEmpty();
        if (active) {
            if (!room.childRoomIds.contains(childRoomId))
                room.childRoomIds.append(childRoomId);
        } else {
            room.childRoomIds.removeAll(childRoomId);
        }
        // NB: we intentionally do NOT set `room.spaceId` on the *child*
        // here — spaceId is a primary-parent hint and SpaceManager builds
        // membership from Space.childRoomIds, so this stays consistent
        // even when a child room appears in multiple Spaces.
    }
}

void CppHttpMatrixClient::applyEdit(const QString &roomId,
                                     const QString &targetEventId,
                                     const QString &newBody)
{
    auto tlIt = m_timelines.find(roomId);
    if (tlIt == m_timelines.end()) return;
    for (auto &e : *tlIt) {
        if (e.eventId == targetEventId) {
            e.body   = newBody;
            e.edited = true;
            if (m_cache) m_cache->updateEvent(e);
            Q_EMIT eventEdited(roomId, targetEventId);
            return;
        }
    }
}

void CppHttpMatrixClient::applyRedaction(const QString &roomId,
                                          const QString &redactedEventId)
{
    auto tlIt = m_timelines.find(roomId);
    if (tlIt == m_timelines.end()) return;

    // A redaction may target a reaction. In that case we want to remove it
    // from its parent's reactions list.
    QString parentEventId;
    QString reactionKey;
    bool wasMineReaction = false;
    for (auto &e : *tlIt) {
        // Scan reactions on every event; if we find one whose myEventId
        // matches, we know the redacted event is a reaction.
        for (const auto &r : e.reactions) {
            if (r.myEventId == redactedEventId) {
                parentEventId  = e.eventId;
                reactionKey    = r.key;
                wasMineReaction = true;
                break;
            }
        }
        if (!parentEventId.isEmpty()) break;
    }
    if (!parentEventId.isEmpty()) {
        for (auto &e : *tlIt) {
            if (e.eventId != parentEventId) continue;
            for (auto &r : e.reactions) {
                if (r.key == reactionKey) {
                    r.count = qMax(0, r.count - 1);
                    if (wasMineReaction) {
                        r.byMe = false;
                        r.myEventId.clear();
                    }
                    break;
                }
            }
            QList<Reaction> filtered;
            filtered.reserve(e.reactions.size());
            for (const auto &r : e.reactions)
                if (r.count > 0) filtered.append(r);
            e.reactions = filtered;
            if (m_cache) m_cache->updateEvent(e);
            Q_EMIT reactionsChanged(roomId, parentEventId);
            return;
        }
    }

    // Otherwise redact the message event itself.
    for (auto &e : *tlIt) {
        if (e.eventId == redactedEventId) {
            e.redacted = true;
            e.body.clear();
            if (m_cache) m_cache->markEventRedacted(redactedEventId);
            Q_EMIT eventRedacted(roomId, redactedEventId);
            return;
        }
    }
}

void CppHttpMatrixClient::applyReactionEvent(const QString &roomId,
                                              const QString &targetEventId,
                                              const QString &key,
                                              const QString &sender,
                                              const QString &reactionEventId)
{
    auto tlIt = m_timelines.find(roomId);
    if (tlIt == m_timelines.end()) return;
    for (auto &e : *tlIt) {
        if (e.eventId != targetEventId) continue;
        Reaction *slot = nullptr;
        for (auto &r : e.reactions) {
            if (r.key == key) { slot = &r; break; }
        }
        if (!slot) {
            Reaction r;
            r.key = key;
            e.reactions.append(r);
            slot = &e.reactions.last();
        }
        slot->count += 1;
        if (sender == m_userId) {
            slot->byMe = true;
            slot->myEventId = reactionEventId;
        }
        if (m_cache) m_cache->updateEvent(e);
        Q_EMIT reactionsChanged(roomId, targetEventId);
        return;
    }
}

void CppHttpMatrixClient::enrichReplyPreview(TimelineEvent &e) const
{
    if (e.replyToEventId.isEmpty()) return;
    const auto tlIt = m_timelines.constFind(e.roomId);
    if (tlIt == m_timelines.constEnd()) return;
    for (const auto &candidate : *tlIt) {
        if (candidate.eventId == e.replyToEventId) {
            const auto roomIt = m_rooms.constFind(e.roomId);
            const QString name = (roomIt != m_rooms.constEnd())
                ? cachedDisplayName(*roomIt, candidate.sender)
                : candidate.sender;
            e.replyToSender  = name;
            e.replyToPreview = matrix::media::previewSnippet(candidate.body);
            return;
        }
    }
}

QString CppHttpMatrixClient::cachedDisplayName(const RoomInfo &room,
                                                const QString &userId) const
{
    const auto it = room.members.constFind(userId);
    if (it != room.members.constEnd() && !it->displayName.isEmpty())
        return it->displayName;
    return userId;
}

void CppHttpMatrixClient::processTimelineEvent(const QString &roomId,
                                                const QJsonObject &ev)
{
    const QString type = ev.value(QStringLiteral("type")).toString();

    // Redactions are handled as a special top-level type in Matrix.
    if (type == QLatin1String("m.room.redaction")) {
        const QString target = ev.value(QStringLiteral("redacts")).toString();
        applyRedaction(roomId, target);
        return;
    }

    // Reactions are annotations, not message content — apply and stop.
    if (type == QLatin1String("m.reaction")) {
        const auto content = ev.value(QStringLiteral("content")).toObject();
        const auto rel = content.value(QStringLiteral("m.relates_to")).toObject();
        if (rel.value(QStringLiteral("rel_type")).toString()
                != QLatin1String("m.annotation")) {
            return;
        }
        const QString target = rel.value(QStringLiteral("event_id")).toString();
        const QString key    = rel.value(QStringLiteral("key")).toString();
        const QString sender = ev.value(QStringLiteral("sender")).toString();
        const QString rxId   = ev.value(QStringLiteral("event_id")).toString();
        if (target.isEmpty() || key.isEmpty()) return;
        applyReactionEvent(roomId, target, key, sender, rxId);
        return;
    }

    if (type != QLatin1String("m.room.message") &&
        type != QLatin1String("m.room.encrypted")) {
        return;
    }

    const auto content = ev.value(QStringLiteral("content")).toObject();
    const auto relatesTo = content.value(QStringLiteral("m.relates_to")).toObject();
    const QString relType = relatesTo.value(QStringLiteral("rel_type")).toString();

    // Edits: apply and suppress the wrapper event from the visible timeline.
    if (relType == QLatin1String("m.replace")) {
        const QString target  = relatesTo.value(QStringLiteral("event_id")).toString();
        const auto newContent = content.value(QStringLiteral("m.new_content")).toObject();
        const QString newBody = newContent.value(QStringLiteral("body")).toString();
        if (!target.isEmpty())
            applyEdit(roomId, target, newBody);
        return;
    }

    TimelineEvent te;
    te.eventId    = ev.value(QStringLiteral("event_id")).toString();
    te.roomId     = roomId;
    te.sender     = ev.value(QStringLiteral("sender")).toString();
    te.timestamp  = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(ev.value(QStringLiteral("origin_server_ts")).toDouble()),
        QTimeZone::UTC);
    te.status     = TimelineEvent::Sent;

    const auto roomIt = m_rooms.constFind(roomId);
    te.senderDisplayName = (roomIt != m_rooms.constEnd())
        ? cachedDisplayName(*roomIt, te.sender)
        : te.sender;

    if (type == QLatin1String("m.room.encrypted")) {
        te.body = tr("[encrypted message - E2EE not implemented yet]");
        te.type = TimelineEvent::Notice;
    } else {
        const QString msgtype = content.value(QStringLiteral("msgtype")).toString();
        te.body = content.value(QStringLiteral("body")).toString();
        if (msgtype == QLatin1String("m.text")) {
            te.type = TimelineEvent::TextMessage;
        } else if (msgtype == QLatin1String("m.notice")) {
            te.type = TimelineEvent::Notice;
        } else if (msgtype == QLatin1String("m.emote")) {
            te.type = TimelineEvent::Emote;
        } else if (msgtype == QLatin1String("m.image") ||
                   msgtype == QLatin1String("m.file")) {
            const bool isImage = (msgtype == QLatin1String("m.image"));
            te.type = isImage ? TimelineEvent::Image : TimelineEvent::File;

            const bool encFile = content.contains(QStringLiteral("file"));
            if (encFile) {
                // Encrypted media envelope. We don't decrypt yet — show a
                // placeholder rather than trying to fetch and rendering junk.
                te.body = tr("[encrypted media - E2EE not implemented yet]");
                te.mediaMxcUrl.clear();
            } else {
                te.mediaMxcUrl   = content.value(QStringLiteral("url")).toString();
                te.mediaFilename = te.body;
                const auto info = content.value(QStringLiteral("info")).toObject();
                te.mediaMimetype = info.value(QStringLiteral("mimetype")).toString();
                te.mediaSize     = static_cast<qint64>(
                    info.value(QStringLiteral("size")).toDouble());
                te.mediaWidth    = info.value(QStringLiteral("w")).toInt();
                te.mediaHeight   = info.value(QStringLiteral("h")).toInt();
                te.mediaThumbnailMxcUrl = info
                    .value(QStringLiteral("thumbnail_url")).toString();
            }
        } else {
            return; // unsupported msgtype
        }

        // v0.4.4: thread relation. When rel_type == "m.thread" the
        // referenced event_id is the thread root. This is set even when
        // the message *also* carries m.in_reply_to (which it does when
        // is_falling_back is true — that's how non-thread-aware clients
        // still see it as a reply). Populate both fields; QML gives the
        // "in thread" badge priority via TimelineModel.
        if (relType == QLatin1String("m.thread")) {
            te.threadRootId = relatesTo.value(QStringLiteral("event_id")).toString();
        }

        // Reply metadata (via m.in_reply_to). Populated for both plain
        // replies and thread fallbacks.
        const auto inReply = relatesTo.value(QStringLiteral("m.in_reply_to")).toObject();
        te.replyToEventId = inReply.value(QStringLiteral("event_id")).toString();
        // If this is a thread reply, hide the plain-reply preview strip in
        // QML — the thread badge already tells the user the context. The
        // spec-compliant fallback reply id is still available in
        // replyToEventId for anything that needs it.
        if (!te.threadRootId.isEmpty()) {
            te.replyToEventId.clear();
        }
        enrichReplyPreview(te);
    }

    // Dedup our own echoes.
    const auto unsignedObj = ev.value(QStringLiteral("unsigned")).toObject();
    const QString txnId = unsignedObj.value(QStringLiteral("transaction_id")).toString();
    if (!txnId.isEmpty() && m_pendingSends.contains(txnId)) {
        m_pendingSends.remove(txnId);
        return;
    }

    m_timelines[roomId].append(te);
    if (m_cache) m_cache->appendEvent(te);
    Q_EMIT eventAppended(roomId, te);
}

void CppHttpMatrixClient::processEphemeral(const QString &roomId,
                                            const QJsonObject &ephemeral)
{
    const auto events = ephemeral.value(QStringLiteral("events")).toArray();
    for (const auto &v : events) {
        const auto ev = v.toObject();
        if (ev.value(QStringLiteral("type")).toString() != QLatin1String("m.typing"))
            continue;
        const auto users = ev.value(QStringLiteral("content"))
                             .toObject()
                             .value(QStringLiteral("user_ids"))
                             .toArray();
        QStringList list;
        list.reserve(users.size());
        for (const auto &u : users) {
            const QString id = u.toString();
            if (id != m_userId)
                list.append(id);
        }
        auto it = m_rooms.find(roomId);
        if (it != m_rooms.end()) {
            it->typingUserIds = list;
            Q_EMIT typingChanged(roomId);
        }
    }
}

void CppHttpMatrixClient::processJoinedRooms(const QJsonObject &joined)
{
    bool structureChanged = false;

    for (auto it = joined.constBegin(); it != joined.constEnd(); ++it) {
        const QString roomId = it.key();
        const QJsonObject roomObj = it.value().toObject();

        RoomInfo room = m_rooms.value(roomId);
        const bool isNew = room.id.isEmpty();
        room.id = roomId;

        const auto stateEvents = roomObj.value(QStringLiteral("state"))
                                         .toObject()
                                         .value(QStringLiteral("events"))
                                         .toArray();
        for (const auto &v : stateEvents)
            processStateEvent(room, v.toObject());

        const auto timelineObj = roomObj.value(QStringLiteral("timeline")).toObject();
        const auto timelineEvents = timelineObj.value(QStringLiteral("events")).toArray();

        // State events can also appear in the timeline for this batch.
        for (const auto &v : timelineEvents) {
            const auto e = v.toObject();
            const QString t = e.value(QStringLiteral("type")).toString();
            if (t == QLatin1String("m.room.name") ||
                t == QLatin1String("m.room.topic") ||
                t == QLatin1String("m.room.avatar") ||
                t == QLatin1String("m.room.encryption") ||
                t == QLatin1String("m.room.canonical_alias") ||
                t == QLatin1String("m.room.member") ||
                // v0.4.2: Spaces state can arrive in either bucket.
                t == QLatin1String("m.room.create") ||
                t == QLatin1String("m.space.child")) {
                processStateEvent(room, e);
            }
        }

        // Pagination token for backfill.
        const QString prev = timelineObj.value(QStringLiteral("prev_batch")).toString();
        if (!prev.isEmpty())
            room.prevBatchToken = prev;

        const auto notif = roomObj.value(QStringLiteral("unread_notifications"))
                                  .toObject();
        if (notif.contains(QStringLiteral("notification_count"))) {
            room.unreadCount = notif.value(
                QStringLiteral("notification_count")).toInt();
        }

        if (room.name.isEmpty())
            room.name = roomId;

        // Emit membership change if members updated.
        const auto oldRoom = m_rooms.value(roomId);
        const bool membersChangedFlag = oldRoom.members.size() != room.members.size();

        m_rooms.insert(roomId, room);

        for (const auto &v : timelineEvents)
            processTimelineEvent(roomId, v.toObject());

        // Refresh preview from newest event.
        const auto &tl = m_timelines[roomId];
        if (!tl.isEmpty()) {
            const auto &last = tl.last();
            RoomInfo &stored = m_rooms[roomId];
            stored.lastMessagePreview =
                matrix::media::previewSnippet(last.body.isEmpty()
                    ? (last.redacted ? tr("[message deleted]")
                                     : last.mediaFilename)
                    : last.body);
            stored.lastActivity = last.timestamp;
        }

        // Ephemeral: typing.
        processEphemeral(roomId,
                         roomObj.value(QStringLiteral("ephemeral")).toObject());

        if (m_cache) m_cache->saveRoom(m_rooms[roomId]);

        if (isNew)
            structureChanged = true;
        else
            Q_EMIT roomUpdated(roomId);
        if (membersChangedFlag)
            Q_EMIT membersChanged(roomId);
    }

    if (structureChanged)
        Q_EMIT roomsChanged();
}

// -------- queries --------

QList<RoomInfo> CppHttpMatrixClient::rooms() const
{
    QList<RoomInfo> list;
    list.reserve(m_rooms.size());
    for (const auto &r : m_rooms)
        list.append(r);
    std::sort(list.begin(), list.end(),
              [](const RoomInfo &a, const RoomInfo &b) {
        if (a.lastActivity.isValid() && b.lastActivity.isValid())
            return a.lastActivity > b.lastActivity;
        if (a.lastActivity.isValid()) return true;
        if (b.lastActivity.isValid()) return false;
        return a.name.toLower() < b.name.toLower();
    });
    return list;
}

QList<TimelineEvent> CppHttpMatrixClient::timeline(const QString &roomId) const
{
    return m_timelines.value(roomId);
}

QString CppHttpMatrixClient::displayNameFor(const QString &roomId,
                                             const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd())
        return userId;
    return cachedDisplayName(*it, userId);
}

QString CppHttpMatrixClient::avatarMxcFor(const QString &roomId,
                                           const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd()) return {};
    const auto mIt = it->members.constFind(userId);
    if (mIt == it->members.constEnd()) return {};
    return mIt->avatarMxcUrl;
}

QStringList CppHttpMatrixClient::typingUsersFor(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd()) return {};
    return it->typingUserIds;
}

QUrl CppHttpMatrixClient::mediaDownloadUrl(const QString &mxcUrl) const
{
    return matrix::media::downloadUrl(m_homeserver, mxcUrl);
}

QUrl CppHttpMatrixClient::mediaThumbnailUrl(const QString &mxcUrl,
                                             int width, int height,
                                             bool crop) const
{
    return matrix::media::thumbnailUrl(m_homeserver, mxcUrl, width, height, crop);
}

// -------- send / edit / redact / react --------

QString CppHttpMatrixClient::nextTxnId()
{
    return QStringLiteral("m%1.%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++m_txnCounter);
}

bool CppHttpMatrixClient::isRoomEncrypted(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    return it != m_rooms.constEnd() && it->encrypted;
}

TimelineEvent CppHttpMatrixClient::buildOwnEcho(const QString &roomId,
                                                 const QString &body,
                                                 TimelineEvent::Type type) const
{
    TimelineEvent e;
    e.roomId            = roomId;
    e.sender            = m_userId;
    e.senderDisplayName = QStringLiteral("You");
    e.body              = body;
    e.timestamp         = QDateTime::currentDateTimeUtc();
    e.type              = type;
    e.status            = TimelineEvent::Sending;
    return e;
}

// Common PUT /rooms/{id}/send/{type}/{txnId} helper. If echoEventId is
// non-empty, tracks it in m_pendingSends and updates its status on response.
void CppHttpMatrixClient::putSendJson(const QString &roomId,
                                       const QString &type,
                                       const QJsonObject &content,
                                       const QString &echoEventId,
                                       const QString &debugLabel)
{
    const QString txnId = nextTxnId();
    if (!echoEventId.isEmpty())
        m_pendingSends.insert(txnId, qMakePair(roomId, echoEventId));

    QUrl url = endpoint(QStringLiteral("/rooms/%1/send/%2/%3")
                             .arg(percentPath(roomId),
                                  percentPath(type),
                                  percentPath(txnId)));
    QNetworkRequest req(url);
    applyBearer(req);
    QNetworkReply *reply = m_nam->put(
        req, QJsonDocument(content).toJson(QJsonDocument::Compact));

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, roomId, echoEventId, txnId, debugLabel] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = extractMatrixError(reply->readAll(),
                                                    reply->errorString());
            qCWarning(lcHttp) << debugLabel << "failed:" << msg;
            m_pendingSends.remove(txnId);
            if (!echoEventId.isEmpty()) {
                for (auto &ev : m_timelines[roomId]) {
                    if (ev.eventId == echoEventId) {
                        ev.status = TimelineEvent::Failed;
                        if (m_cache) m_cache->updateEvent(ev);
                        Q_EMIT eventStatusChanged(roomId, echoEventId,
                                                   TimelineEvent::Failed);
                        break;
                    }
                }
            }
            Q_EMIT errorOccurred(tr("Send failed: %1").arg(msg));
            return;
        }
        if (echoEventId.isEmpty())
            return;

        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const QString realEventId = doc.isObject()
            ? doc.object().value(QStringLiteral("event_id")).toString()
            : QString();

        for (int i = 0; i < m_timelines[roomId].size(); ++i) {
            auto &ev = m_timelines[roomId][i];
            if (ev.eventId != echoEventId)
                continue;
            const QString oldId = ev.eventId;
            if (!realEventId.isEmpty()) {
                ev.eventId = realEventId;
                if (m_pendingSends.contains(txnId))
                    m_pendingSends[txnId] = qMakePair(roomId, realEventId);
                if (m_cache) m_cache->replaceEventId(oldId, ev);
                ev.status = TimelineEvent::Sent;
                Q_EMIT eventReplaced(roomId, oldId, ev);
            } else {
                ev.status = TimelineEvent::Sent;
                if (m_cache) m_cache->updateEvent(ev);
                Q_EMIT eventStatusChanged(roomId, oldId, TimelineEvent::Sent);
            }
            break;
        }
    });
}

void CppHttpMatrixClient::sendTextMessage(const QString &roomId, const QString &body)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId)); return;
    }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    TimelineEvent echo = buildOwnEcho(roomId, body, TimelineEvent::TextMessage);
    echo.eventId = QLatin1String("local:") + nextTxnId();
    m_timelines[roomId].append(echo);
    if (m_cache) m_cache->appendEvent(echo);
    Q_EMIT eventAppended(roomId, echo);

    const QJsonObject content{
        { QStringLiteral("msgtype"), QStringLiteral("m.text") },
        { QStringLiteral("body"),    body },
    };
    putSendJson(roomId, QStringLiteral("m.room.message"),
                content, echo.eventId, QStringLiteral("send-text"));
}

void CppHttpMatrixClient::sendReply(const QString &roomId,
                                     const QString &replyToEventId,
                                     const QString &body)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId)); return;
    }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }

    TimelineEvent echo = buildOwnEcho(roomId, body, TimelineEvent::TextMessage);
    echo.eventId       = QLatin1String("local:") + nextTxnId();
    echo.replyToEventId = replyToEventId;
    enrichReplyPreview(echo);
    m_timelines[roomId].append(echo);
    if (m_cache) m_cache->appendEvent(echo);
    Q_EMIT eventAppended(roomId, echo);

    const QJsonObject relates{
        { QStringLiteral("m.in_reply_to"),
          QJsonObject{ { QStringLiteral("event_id"), replyToEventId } } },
    };
    const QJsonObject content{
        { QStringLiteral("msgtype"),      QStringLiteral("m.text") },
        { QStringLiteral("body"),         body },
        { QStringLiteral("m.relates_to"), relates },
    };
    putSendJson(roomId, QStringLiteral("m.room.message"),
                content, echo.eventId, QStringLiteral("send-reply"));
}

void CppHttpMatrixClient::sendThreadReply(const QString &roomId,
                                           const QString &threadRootEventId,
                                           const QString &body)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId)); return;
    }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    if (threadRootEventId.isEmpty()) {
        Q_EMIT errorOccurred(tr("Cannot send thread reply: no thread root."));
        return;
    }

    // v0.4.4: real thread relation (spec: m.thread rel_type).
    // Content carries m.relates_to =
    //   { rel_type: "m.thread",
    //     event_id: <root>,
    //     is_falling_back: true,
    //     m.in_reply_to: { event_id: <latest-in-thread-or-root> } }
    //
    // is_falling_back + m.in_reply_to make non-thread-aware clients render
    // the reply as a normal in-reply-to chain. If we already have a local
    // event whose threadRootId matches, use the newest server-confirmed one
    // as the fallback target; otherwise fall back to the root itself.
    QString fallbackReplyTarget = threadRootEventId;
    {
        const auto tlIt = m_timelines.constFind(roomId);
        if (tlIt != m_timelines.constEnd()) {
            for (auto it = tlIt->crbegin(); it != tlIt->crend(); ++it) {
                if (it->threadRootId == threadRootEventId
                    && !it->eventId.startsWith(QLatin1String("local:"))) {
                    fallbackReplyTarget = it->eventId;
                    break;
                }
            }
        }
    }

    TimelineEvent echo = buildOwnEcho(roomId, body, TimelineEvent::TextMessage);
    echo.eventId       = QLatin1String("local:") + nextTxnId();
    echo.threadRootId  = threadRootEventId;
    // Do NOT set replyToEventId on the echo — the "in thread" chip in QML
    // is what the user asked for; adding a reply preview strip on top of
    // that duplicates the UI. Once the real event round-trips through
    // /sync we still parse the fallback m.in_reply_to for spec-compliance
    // with non-thread-aware clients, but the local echo stays clean.
    m_timelines[roomId].append(echo);
    if (m_cache) m_cache->appendEvent(echo);
    Q_EMIT eventAppended(roomId, echo);

    const QJsonObject relates{
        { QStringLiteral("rel_type"),        QStringLiteral("m.thread") },
        { QStringLiteral("event_id"),        threadRootEventId },
        { QStringLiteral("is_falling_back"), true },
        { QStringLiteral("m.in_reply_to"),
          QJsonObject{ { QStringLiteral("event_id"), fallbackReplyTarget } } },
    };
    const QJsonObject content{
        { QStringLiteral("msgtype"),      QStringLiteral("m.text") },
        { QStringLiteral("body"),         body },
        { QStringLiteral("m.relates_to"), relates },
    };
    putSendJson(roomId, QStringLiteral("m.room.message"),
                content, echo.eventId, QStringLiteral("send-thread"));
}

void CppHttpMatrixClient::editMessage(const QString &roomId,
                                       const QString &targetEventId,
                                       const QString &newBody)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    // Apply the edit optimistically to the local echo.
    applyEdit(roomId, targetEventId, newBody);

    const QJsonObject newContent{
        { QStringLiteral("msgtype"), QStringLiteral("m.text") },
        { QStringLiteral("body"),    newBody },
    };
    const QJsonObject relates{
        { QStringLiteral("rel_type"), QStringLiteral("m.replace") },
        { QStringLiteral("event_id"), targetEventId },
    };
    // Per spec: fallback body prefixed with "* ".
    const QJsonObject content{
        { QStringLiteral("msgtype"),        QStringLiteral("m.text") },
        { QStringLiteral("body"),           QStringLiteral("* ") + newBody },
        { QStringLiteral("m.new_content"),  newContent },
        { QStringLiteral("m.relates_to"),   relates },
    };
    putSendJson(roomId, QStringLiteral("m.room.message"),
                content, {}, QStringLiteral("send-edit"));
}

void CppHttpMatrixClient::redactByHttp(const QString &roomId,
                                        const QString &eventId,
                                        const QString &reason)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    const QString txnId = nextTxnId();

    QUrl url = endpoint(QStringLiteral("/rooms/%1/redact/%2/%3")
                             .arg(percentPath(roomId),
                                  percentPath(eventId),
                                  percentPath(txnId)));
    QNetworkRequest req(url);
    applyBearer(req);
    QJsonObject body;
    if (!reason.isEmpty()) body.insert(QStringLiteral("reason"), reason);
    QNetworkReply *reply = m_nam->put(
        req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(tr("Redaction failed: %1").arg(
                extractMatrixError(reply->readAll(), reply->errorString())));
        }
        // Actual event mutation arrives via /sync as m.room.redaction.
    });
}

void CppHttpMatrixClient::redactEvent(const QString &roomId,
                                       const QString &eventId,
                                       const QString &reason)
{
    redactByHttp(roomId, eventId, reason);
}

void CppHttpMatrixClient::toggleReaction(const QString &roomId,
                                          const QString &targetEventId,
                                          const QString &key)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    // If I've already reacted with this key on this target, remove it (redact
    // my reaction event).
    const auto tlIt = m_timelines.constFind(roomId);
    if (tlIt != m_timelines.constEnd()) {
        for (const auto &e : *tlIt) {
            if (e.eventId != targetEventId) continue;
            for (const auto &r : e.reactions) {
                if (r.key == key && r.byMe && !r.myEventId.isEmpty()) {
                    redactByHttp(roomId, r.myEventId, {});
                    return;
                }
            }
            break;
        }
    }
    // Otherwise send a new reaction.
    const QJsonObject relates{
        { QStringLiteral("rel_type"), QStringLiteral("m.annotation") },
        { QStringLiteral("event_id"), targetEventId },
        { QStringLiteral("key"),      key },
    };
    const QJsonObject content{
        { QStringLiteral("m.relates_to"), relates },
    };
    putSendJson(roomId, QStringLiteral("m.reaction"),
                content, {}, QStringLiteral("send-reaction"));
}

void CppHttpMatrixClient::sendTyping(const QString &roomId,
                                      bool isTyping,
                                      int timeoutMs)
{
    if (!m_loggedIn || m_userId.isEmpty() || m_homeserver.isEmpty())
        return;
    QUrl url = endpoint(QStringLiteral("/rooms/%1/typing/%2")
                             .arg(percentPath(roomId), percentPath(m_userId)));
    QNetworkRequest req(url);
    applyBearer(req);
    QJsonObject body{ { QStringLiteral("typing"), isTyping } };
    if (isTyping)
        body.insert(QStringLiteral("timeout"), timeoutMs);
    QNetworkReply *reply = m_nam->put(
        req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     &QNetworkReply::deleteLater);
}

void CppHttpMatrixClient::sendReadReceipt(const QString &roomId,
                                           const QString &eventId)
{
    if (!m_loggedIn || eventId.isEmpty()) return;
    if (m_lastReceiptSent.value(roomId) == eventId) return; // debounce
    m_lastReceiptSent[roomId] = eventId;

    QUrl url = endpoint(QStringLiteral("/rooms/%1/receipt/m.read/%2")
                             .arg(percentPath(roomId), percentPath(eventId)));
    QNetworkRequest req(url);
    applyBearer(req);
    QNetworkReply *reply = m_nam->post(req, QByteArray("{}"));
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     &QNetworkReply::deleteLater);
}

// -------- media send --------

void CppHttpMatrixClient::sendImage(const QString &roomId, const QString &localPath)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId)); return;
    }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send media to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    QFile *file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(tr("Cannot read %1").arg(localPath));
        delete file;
        return;
    }
    const QFileInfo fi(localPath);
    const QString mime = matrix::media::mimetypeForFile(localPath);
    const QString fileName = fi.fileName();

    // Optimistic local echo.
    TimelineEvent echo = buildOwnEcho(roomId, fileName, TimelineEvent::Image);
    echo.eventId       = QLatin1String("local:") + nextTxnId();
    echo.mediaFilename = fileName;
    echo.mediaMimetype = mime;
    echo.mediaSize     = fi.size();
    // Try to read dimensions cheaply (image only).
    QImage tmp(localPath);
    if (!tmp.isNull()) {
        echo.mediaWidth  = tmp.width();
        echo.mediaHeight = tmp.height();
    }
    m_timelines[roomId].append(echo);
    if (m_cache) m_cache->appendEvent(echo);
    Q_EMIT eventAppended(roomId, echo);

    QUrl uploadUrl = mediaEndpoint(QStringLiteral("/upload"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("filename"), fileName);
    uploadUrl.setQuery(q);
    QNetworkRequest req(uploadUrl);
    if (!m_accessToken.isEmpty()) {
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") + m_accessToken.toUtf8());
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, mime.toUtf8());
    req.setRawHeader("User-Agent", "matrix-client/0.3");

    const QString echoEventId = echo.eventId;
    QNetworkReply *reply = m_nam->post(req, file);
    file->setParent(reply); // deleted with the reply

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, roomId, echoEventId, fileName, mime,
                      width = echo.mediaWidth, height = echo.mediaHeight,
                      size = echo.mediaSize] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            for (auto &ev : m_timelines[roomId]) {
                if (ev.eventId == echoEventId) {
                    ev.status = TimelineEvent::Failed;
                    if (m_cache) m_cache->updateEvent(ev);
                    Q_EMIT eventStatusChanged(roomId, echoEventId,
                                               TimelineEvent::Failed);
                    break;
                }
            }
            Q_EMIT errorOccurred(tr("Upload failed: %1").arg(
                extractMatrixError(reply->readAll(), reply->errorString())));
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const QString mxc = doc.object().value(QStringLiteral("content_uri")).toString();
        if (mxc.isEmpty()) {
            Q_EMIT errorOccurred(tr("Upload response missing content_uri."));
            return;
        }
        for (auto &ev : m_timelines[roomId]) {
            if (ev.eventId == echoEventId) {
                ev.mediaMxcUrl = mxc;
                if (m_cache) m_cache->updateEvent(ev);
                break;
            }
        }
        // Now send the message event referencing the mxc URI.
        QJsonObject info{
            { QStringLiteral("mimetype"), mime },
            { QStringLiteral("size"),     static_cast<qint64>(size) },
        };
        if (width > 0)  info.insert(QStringLiteral("w"), width);
        if (height > 0) info.insert(QStringLiteral("h"), height);
        const QJsonObject content{
            { QStringLiteral("msgtype"), QStringLiteral("m.image") },
            { QStringLiteral("body"),    fileName },
            { QStringLiteral("url"),     mxc },
            { QStringLiteral("info"),    info },
        };
        putSendJson(roomId, QStringLiteral("m.room.message"),
                    content, echoEventId, QStringLiteral("send-image"));
    });
}

void CppHttpMatrixClient::sendFile(const QString &roomId, const QString &localPath)
{
    if (!m_loggedIn) { Q_EMIT errorOccurred(tr("Not signed in.")); return; }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId)); return;
    }
    if (isRoomEncrypted(roomId)) {
        Q_EMIT errorOccurred(tr(
            "Cannot send media to encrypted rooms yet: E2EE backend is not implemented."));
        return;
    }
    QFile *file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(tr("Cannot read %1").arg(localPath));
        delete file;
        return;
    }
    const QFileInfo fi(localPath);
    const QString mime = matrix::media::mimetypeForFile(localPath);
    const QString fileName = fi.fileName();

    TimelineEvent echo = buildOwnEcho(roomId, fileName, TimelineEvent::File);
    echo.eventId       = QLatin1String("local:") + nextTxnId();
    echo.mediaFilename = fileName;
    echo.mediaMimetype = mime;
    echo.mediaSize     = fi.size();
    m_timelines[roomId].append(echo);
    if (m_cache) m_cache->appendEvent(echo);
    Q_EMIT eventAppended(roomId, echo);

    QUrl uploadUrl = mediaEndpoint(QStringLiteral("/upload"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("filename"), fileName);
    uploadUrl.setQuery(q);
    QNetworkRequest req(uploadUrl);
    if (!m_accessToken.isEmpty()) {
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") + m_accessToken.toUtf8());
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, mime.toUtf8());
    req.setRawHeader("User-Agent", "matrix-client/0.3");

    const QString echoEventId = echo.eventId;
    QNetworkReply *reply = m_nam->post(req, file);
    file->setParent(reply);

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, roomId, echoEventId, fileName, mime,
                      size = echo.mediaSize] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            for (auto &ev : m_timelines[roomId]) {
                if (ev.eventId == echoEventId) {
                    ev.status = TimelineEvent::Failed;
                    if (m_cache) m_cache->updateEvent(ev);
                    Q_EMIT eventStatusChanged(roomId, echoEventId,
                                               TimelineEvent::Failed);
                    break;
                }
            }
            Q_EMIT errorOccurred(tr("Upload failed: %1").arg(
                extractMatrixError(reply->readAll(), reply->errorString())));
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const QString mxc = doc.object().value(QStringLiteral("content_uri")).toString();
        if (mxc.isEmpty()) {
            Q_EMIT errorOccurred(tr("Upload response missing content_uri."));
            return;
        }
        for (auto &ev : m_timelines[roomId]) {
            if (ev.eventId == echoEventId) {
                ev.mediaMxcUrl = mxc;
                if (m_cache) m_cache->updateEvent(ev);
                break;
            }
        }
        const QJsonObject info{
            { QStringLiteral("mimetype"), mime },
            { QStringLiteral("size"),     static_cast<qint64>(size) },
        };
        const QJsonObject content{
            { QStringLiteral("msgtype"), QStringLiteral("m.file") },
            { QStringLiteral("body"),    fileName },
            { QStringLiteral("url"),     mxc },
            { QStringLiteral("info"),    info },
        };
        putSendJson(roomId, QStringLiteral("m.room.message"),
                    content, echoEventId, QStringLiteral("send-file"));
    });
}

// -------- pagination --------

void CppHttpMatrixClient::loadOlderMessages(const QString &roomId)
{
    if (!m_loggedIn) return;
    if (m_paginating.contains(roomId)) return;
    const auto roomIt = m_rooms.constFind(roomId);
    if (roomIt == m_rooms.constEnd()) return;
    if (roomIt->paginationExhausted) return;
    if (roomIt->prevBatchToken.isEmpty()) return;

    m_paginating.insert(roomId);
    Q_EMIT paginationStateChanged(roomId);

    QUrl url = endpoint(QStringLiteral("/rooms/%1/messages")
                             .arg(percentPath(roomId)));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("dir"),   QStringLiteral("b"));
    q.addQueryItem(QStringLiteral("from"),  roomIt->prevBatchToken);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    url.setQuery(q);

    QNetworkRequest req(url);
    applyBearer(req);
    QNetworkReply *reply = m_nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, roomId] {
        reply->deleteLater();
        m_paginating.remove(roomId);
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcHttp) << "pagination failed";
            Q_EMIT errorOccurred(tr("Failed to load more history."));
            Q_EMIT paginationStateChanged(roomId);
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            Q_EMIT paginationStateChanged(roomId);
            return;
        }
        const auto obj = doc.object();
        const QString endToken = obj.value(QStringLiteral("end")).toString();
        const auto chunk = obj.value(QStringLiteral("chunk")).toArray();

        auto rIt = m_rooms.find(roomId);
        if (rIt == m_rooms.end()) {
            Q_EMIT paginationStateChanged(roomId);
            return;
        }
        if (endToken.isEmpty() || chunk.isEmpty()) {
            rIt->paginationExhausted = true;
        }
        if (!endToken.isEmpty())
            rIt->prevBatchToken = endToken;
        if (m_cache) m_cache->saveRoom(*rIt);

        // /messages returns events in reverse chronological order when dir=b.
        // Convert to chronological order, dedup against what we already have.
        QList<TimelineEvent> prepended;
        QSet<QString> existingIds;
        for (const auto &e : m_timelines[roomId])
            existingIds.insert(e.eventId);

        // Walk from oldest to newest.
        for (auto it = chunk.end(); it != chunk.begin();) {
            --it;
            const auto ev = it->toObject();
            const QString type = ev.value(QStringLiteral("type")).toString();
            if (type != QLatin1String("m.room.message") &&
                type != QLatin1String("m.room.encrypted"))
                continue;
            // Skip edit-wrapper events; those need their target already loaded,
            // and pagination isn't the right moment to reconcile them.
            const auto content = ev.value(QStringLiteral("content")).toObject();
            const auto rel = content.value(QStringLiteral("m.relates_to")).toObject();
            if (rel.value(QStringLiteral("rel_type")).toString()
                    == QLatin1String("m.replace"))
                continue;

            TimelineEvent te;
            te.eventId    = ev.value(QStringLiteral("event_id")).toString();
            if (existingIds.contains(te.eventId)) continue;
            te.roomId     = roomId;
            te.sender     = ev.value(QStringLiteral("sender")).toString();
            te.timestamp  = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(ev.value(QStringLiteral("origin_server_ts")).toDouble()),
                QTimeZone::UTC);
            te.status = TimelineEvent::Sent;
            te.senderDisplayName = cachedDisplayName(*rIt, te.sender);

            if (type == QLatin1String("m.room.encrypted")) {
                te.body = tr("[encrypted message - E2EE not implemented yet]");
                te.type = TimelineEvent::Notice;
            } else {
                const QString msgtype = content.value(QStringLiteral("msgtype")).toString();
                te.body = content.value(QStringLiteral("body")).toString();
                if (msgtype == QLatin1String("m.text"))       te.type = TimelineEvent::TextMessage;
                else if (msgtype == QLatin1String("m.notice")) te.type = TimelineEvent::Notice;
                else if (msgtype == QLatin1String("m.emote"))  te.type = TimelineEvent::Emote;
                else if (msgtype == QLatin1String("m.image") ||
                         msgtype == QLatin1String("m.file")) {
                    const bool isImage = (msgtype == QLatin1String("m.image"));
                    te.type = isImage ? TimelineEvent::Image : TimelineEvent::File;
                    if (!content.contains(QStringLiteral("file"))) {
                        te.mediaMxcUrl   = content.value(QStringLiteral("url")).toString();
                        te.mediaFilename = te.body;
                        const auto info = content.value(QStringLiteral("info")).toObject();
                        te.mediaMimetype = info.value(QStringLiteral("mimetype")).toString();
                        te.mediaSize     = static_cast<qint64>(
                            info.value(QStringLiteral("size")).toDouble());
                        te.mediaWidth    = info.value(QStringLiteral("w")).toInt();
                        te.mediaHeight   = info.value(QStringLiteral("h")).toInt();
                    } else {
                        te.body = tr("[encrypted media - E2EE not implemented yet]");
                    }
                } else {
                    continue;
                }
                // v0.4.4: thread relation on backfilled events.
                if (rel.value(QStringLiteral("rel_type")).toString()
                        == QLatin1String("m.thread")) {
                    te.threadRootId = rel.value(QStringLiteral("event_id")).toString();
                }
                const auto inReply = rel.value(QStringLiteral("m.in_reply_to")).toObject();
                te.replyToEventId = inReply.value(QStringLiteral("event_id")).toString();
                if (!te.threadRootId.isEmpty()) {
                    // Same UX rule as /sync: thread events use the thread
                    // badge; the plain-reply preview strip would be noise.
                    te.replyToEventId.clear();
                }
            }
            existingIds.insert(te.eventId);
            prepended.append(te);
        }

        // Prepend to in-memory timeline in chronological order.
        if (!prepended.isEmpty()) {
            auto &tl = m_timelines[roomId];
            for (int i = prepended.size() - 1; i >= 0; --i)
                tl.prepend(prepended.at(i));
            if (m_cache) {
                for (const auto &e : prepended)
                    m_cache->appendEvent(e);
            }
            // Now that older events are loaded, backfill reply previews.
            for (auto &e : m_timelines[roomId]) {
                if (!e.replyToEventId.isEmpty() && e.replyToPreview.isEmpty())
                    enrichReplyPreview(e);
            }
            Q_EMIT eventsPrepended(roomId, prepended);
        }
        Q_EMIT paginationStateChanged(roomId);
    });
}

bool CppHttpMatrixClient::canPaginate(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd()) return false;
    return !it->paginationExhausted && !it->prevBatchToken.isEmpty();
}

bool CppHttpMatrixClient::paginating(const QString &roomId) const
{
    return m_paginating.contains(roomId);
}
