#include "app/SavedMessagesController.h"

#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <utility>

namespace {
const QString kType = QStringLiteral("org.lightning_matrix.saved_messages");
constexpr int kMaxRefs = 100;
constexpr int kMaxBytes = 16 * 1024;
}

const QString &SavedMessagesController::eventType() { return kType; }

bool SavedMessagesController::supported() const
{ return m_client && m_client->supportsAccountPrefs(); }

SavedMessagesController::SavedMessagesController(QObject *parent) : QObject(parent)
{
    m_resolveTimeout.setSingleShot(true);
    m_resolveTimeout.setInterval(15000);
    connect(&m_resolveTimeout, &QTimer::timeout, this, [this] {
        // A refused or dropped event-source request must release the queue.
        for (Ref &ref : m_refs) {
            if (m_resolving.contains(key(ref.roomId, ref.eventId))) {
                ref.resolved = true;
                ref.available = false;
                ref.preview.clear();
            }
        }
        m_resolving.clear();
        emitChanged();
        pumpResolve();
    });
}

QString SavedMessagesController::key(const QString &roomId, const QString &eventId)
{ return roomId + QChar(0x1f) + eventId; }

bool SavedMessagesController::validRef(const Ref &ref)
{
    return ref.roomId.startsWith(QLatin1Char('!')) && ref.roomId.size() <= 255
        && !MatrixClient::isThreadTimelineId(ref.roomId)
        && !ref.roomId.contains(QChar(0x1f))
        && ref.eventId.startsWith(QLatin1Char('$')) && ref.eventId.size() <= 255
        && !ref.eventId.contains(QChar(0x1f))
        && (ref.rootId.isEmpty()
            || (ref.rootId.startsWith(QLatin1Char('$')) && ref.rootId.size() <= 255
                && !ref.rootId.contains(QChar(0x1f))))
        && ref.savedAtMs > 0;
}

int SavedMessagesController::indexOf(const QString &roomId, const QString &eventId) const
{
    for (int i = 0; i < m_refs.size(); ++i)
        if (m_refs.at(i).roomId == roomId && m_refs.at(i).eventId == eventId)
            return i;
    return -1;
}

void SavedMessagesController::emitChanged()
{
    ++m_revision;
    Q_EMIT changed();
}

void SavedMessagesController::reset()
{
    m_refs.clear();
    m_beforeWrite.clear();
    m_undo = {};
    m_undoValid = false;
    m_loaded = false;
    m_busy = false;
    m_saving = false;
    m_fetchingForWrite = false;
    m_pendingReads = 0;
    m_reloadOwed = false;
    m_error.clear();
    m_resolving.clear();
    m_attempted.clear();
    m_redacted.clear();
    m_resolveTimeout.stop();
    emitChanged();
}

void SavedMessagesController::setClient(MatrixClient *client)
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_userId.clear();
    reset();
    if (!client)
        return;
    connect(client, &MatrixClient::loggedOut, this, [this] {
        m_userId.clear();
        reset();
    });
    connect(client, &MatrixClient::loginSucceeded, this,
            [this](const QString &userId) {
        m_userId = userId;
        reset();
        refresh();
    });
    connect(client, &MatrixClient::initialSyncDoneChanged, this, [this] {
        if (m_client && m_client->initialSyncDone())
            refresh();
    });
    connect(client, &MatrixClient::accountPrefChanged, this,
            [this](const QString &type) {
        if (type != kType)
            return;
        if (m_busy) m_reloadOwed = true;
        else refresh();
    });
    connect(client, &MatrixClient::accountPrefRead, this,
            [this](const QString &type, bool present, const QVariantMap &content) {
        if (type != kType) return;
        if (m_pendingReads > 0) --m_pendingReads;
        if (m_busy && !m_fetchingForWrite) {
            if (m_pendingReads == 0) {
                m_fetchingForWrite = true;
                ++m_pendingReads;
                m_client->readAccountPref(kType);
            }
            return;
        }
        const QVariantMap current = present ? content : QVariantMap{};
        if (!m_fetchingForWrite) {
            if (!m_busy) receive(current, present);
            return;
        }
        m_fetchingForWrite = false;
        QVector<Ref> remote;
        if (!parseRefs(current, present, remote)) {
            failWrite(tr("Saved messages data is unavailable."));
            return;
        }
        // Apply only THIS operation's change to the fresh server list: what
        // it added (in m_refs, not in m_beforeWrite) and what it removed (the
        // reverse). Re-adding the whole local copy would resurrect an item
        // another device removed after our last read.
        QSet<QString> before;
        QSet<QString> desired;
        for (const Ref &ref : std::as_const(m_beforeWrite))
            before.insert(key(ref.roomId, ref.eventId));
        for (const Ref &ref : std::as_const(m_refs))
            desired.insert(key(ref.roomId, ref.eventId));
        QVector<Ref> merged;
        QSet<QString> seen;
        for (const Ref &ref : std::as_const(remote)) {
            const QString id = key(ref.roomId, ref.eventId);
            const bool locallyRemoved = before.contains(id) && !desired.contains(id);
            if (!locallyRemoved && !seen.contains(id)) {
                merged.append(ref);
                seen.insert(id);
            }
        }
        for (const Ref &ref : std::as_const(m_refs)) {
            const QString id = key(ref.roomId, ref.eventId);
            const bool locallyAdded = !before.contains(id);
            if (locallyAdded && !seen.contains(id)) {
                merged.append(ref);
                seen.insert(id);
            }
        }
        if (merged.size() > kMaxRefs) {
            failWrite(tr("Saved messages are full. Remove an older item first."));
            return;
        }
        std::sort(merged.begin(), merged.end(), [](const Ref &a, const Ref &b) {
            return a.savedAtMs > b.savedAtMs;
        });
        m_refs = std::move(merged);
        sendWrite();
    });
    connect(client, &MatrixClient::accountPrefReadFailed, this,
            [this](const QString &type, const QString &) {
        if (type != kType)
            return;
        if (m_pendingReads > 0) --m_pendingReads;
        if (m_busy && !m_fetchingForWrite) {
            if (m_pendingReads == 0) {
                m_fetchingForWrite = true;
                ++m_pendingReads;
                m_client->readAccountPref(kType);
            }
            return;
        }
        if (m_fetchingForWrite) {
            m_fetchingForWrite = false;
            failWrite(tr("Saved messages could not be updated. Retry."));
            return;
        }
        if (m_busy) return;
        m_error = tr("Saved messages could not be loaded. Retry.");
        emitChanged();
    });
    connect(client, &MatrixClient::accountPrefWritten, this,
            [this](const QString &type, bool ok, const QString &) {
        if (type != kType || !m_busy)
            return;
        m_busy = false;
        if (!ok) {
            m_refs = std::move(m_beforeWrite);
            m_undoValid = false;
            m_error = tr("Saved messages could not be updated. Retry.");
            Q_EMIT actionFailed(m_error);
        } else {
            m_error.clear();
            if (m_saving)
                Q_EMIT saveSucceeded();
        }
        m_saving = false;
        m_beforeWrite.clear();
        emitChanged();
        if (ok || m_reloadOwed) {
            m_reloadOwed = false;
            refresh();
        }
    });
    connect(client, &MatrixClient::eventSourceReceived, this,
            [this](const QString &roomId, const QString &eventId, bool ok,
                   const QString &json, const QVariantMap &) {
        receiveSource(roomId, eventId, ok, json);
    });
    connect(client, &MatrixClient::eventRedacted, this,
            [this](const QString &roomId, const QString &eventId) {
        const int i = indexOf(roomId, eventId);
        if (i < 0) return;
        Ref &ref = m_refs[i];
        ref.preview.clear();
        ref.sender.clear();
        ref.available = false;
        ref.resolved = true;
        const QString id = key(roomId, eventId);
        m_resolving.remove(id);
        if (m_resolving.isEmpty()) m_resolveTimeout.stop();
        m_attempted.insert(id);
        m_redacted.insert(id);
        emitChanged();
        pumpResolve();
    });
    if (client->isLoggedIn() && !client->currentUserId().isEmpty()) {
        m_userId = client->currentUserId();
        refresh();
    }
}

void SavedMessagesController::refresh()
{
    if (m_busy) { m_reloadOwed = true; return; }
    if (!m_client || !m_client->supportsAccountPrefs()
        || m_userId.isEmpty() || m_client->currentUserId() != m_userId)
        return;
    ++m_pendingReads;
    m_client->readAccountPref(kType);
    emitChanged();
}

void SavedMessagesController::receive(const QVariantMap &content, bool present)
{
    if (!m_client || m_userId.isEmpty()
        || m_client->currentUserId() != m_userId)
        return;
    QVector<Ref> next;
    if (!parseRefs(content, present, next)) {
        m_loaded = false;
        m_error = tr("Saved messages data is unavailable.");
        emitChanged();
        return;
    }
    m_refs = std::move(next);
    m_loaded = true;
    m_error.clear();
    m_resolving.clear();
    m_attempted.clear();
    for (Ref &ref : m_refs) {
        if (m_redacted.contains(key(ref.roomId, ref.eventId))) {
            ref.resolved = true;
            ref.available = false;
            m_attempted.insert(key(ref.roomId, ref.eventId));
        }
    }
    m_resolveTimeout.stop();
    emitChanged();
}

bool SavedMessagesController::parseRefs(const QVariantMap &content, bool present,
                                        QVector<Ref> &next) const
{
    // An existing but empty {} (another client clearing it) is an empty
    // list; no write can be built from it without the strict shape below.
    const QVariantList raw = content.value(QStringLiteral("entries")).toList();
    if (!content.isEmpty()
        && (content.size() != 2
            || !content.value(QStringLiteral("entries")).canConvert<QVariantList>()
            || content.value(QStringLiteral("version")).toInt() != 1
            || raw.size() > kMaxRefs)) {
        return false;
    }
    QSet<QString> seen;
    for (const QVariant &v : raw) {
        const QVariantMap item = v.toMap();
        if (item.size() != 4) {
            return false;
        }
        Ref ref;
        ref.roomId = item.value(QStringLiteral("room_id")).toString();
        ref.eventId = item.value(QStringLiteral("event_id")).toString();
        ref.rootId = item.value(QStringLiteral("thread_root_id")).toString();
        ref.savedAtMs = item.value(QStringLiteral("saved_at_ms")).toLongLong();
        if (!validRef(ref)) {
            return false;
        }
        const QString id = key(ref.roomId, ref.eventId);
        if (!seen.contains(id)) {
            next.append(ref);
            seen.insert(id);
        }
    }
    std::sort(next.begin(), next.end(), [](const Ref &a, const Ref &b) {
        return a.savedAtMs > b.savedAtMs;
    });
    return true;
}

bool SavedMessagesController::isSaved(const QString &roomId, const QString &eventId) const
{ return indexOf(roomId, eventId) >= 0; }

void SavedMessagesController::write()
{
    if (!m_client || m_userId.isEmpty()
        || m_client->currentUserId() != m_userId)
        return;
    m_busy = true;
    m_fetchingForWrite = m_pendingReads == 0;
    m_error.clear();
    emitChanged();
    if (m_fetchingForWrite) {
        ++m_pendingReads;
        m_client->readAccountPref(kType);
    }
}

void SavedMessagesController::failWrite(const QString &message)
{
    m_busy = false;
    m_saving = false;
    m_fetchingForWrite = false;
    m_refs = std::move(m_beforeWrite);
    m_beforeWrite.clear();
    m_undoValid = false;
    m_error = message;
    Q_EMIT actionFailed(m_error);
    emitChanged();
}

void SavedMessagesController::sendWrite()
{
    QVariantList entries;
    for (const Ref &ref : std::as_const(m_refs))
        entries.append(QVariantMap{
            { QStringLiteral("room_id"), ref.roomId },
            { QStringLiteral("event_id"), ref.eventId },
            { QStringLiteral("thread_root_id"), ref.rootId },
            { QStringLiteral("saved_at_ms"), ref.savedAtMs },
        });
    const QVariantMap content{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("entries"), entries },
    };
    if (QJsonDocument(QJsonObject::fromVariantMap(content))
            .toJson(QJsonDocument::Compact).size() > kMaxBytes) {
        failWrite(tr("Saved messages are full. Remove an older item first."));
        return;
    }
    m_client->writeAccountPref(kType, content);
}

void SavedMessagesController::save(const QString &roomId,
                                   const QString &eventId,
                                   const QString &threadRootId)
{
    if (!m_loaded || m_busy || isSaved(roomId, eventId)) return;
    Ref ref{roomId, eventId, threadRootId,
            QDateTime::currentMSecsSinceEpoch()};
    if (!validRef(ref)) return;
    if (m_refs.size() >= kMaxRefs) {
        m_error = tr("Saved messages are full. Remove an older item first.");
        Q_EMIT actionFailed(m_error);
        emitChanged();
        return;
    }
    m_beforeWrite = m_refs;
    m_refs.prepend(ref);
    m_saving = true;
    m_undoValid = false;
    write();
}

void SavedMessagesController::remove(const QString &roomId,
                                     const QString &eventId)
{
    if (!m_loaded || m_busy) return;
    const int i = indexOf(roomId, eventId);
    if (i < 0) return;
    m_beforeWrite = m_refs;
    m_undo = m_refs.takeAt(i);
    m_saving = false;
    m_undoValid = true;
    write();
}

void SavedMessagesController::undoRemove()
{
    if (!m_loaded || m_busy || !m_undoValid
        || isSaved(m_undo.roomId, m_undo.eventId)) return;
    m_beforeWrite = m_refs;
    m_refs.append(m_undo);
    m_saving = false;
    std::sort(m_refs.begin(), m_refs.end(), [](const Ref &a, const Ref &b) {
        return a.savedAtMs > b.savedAtMs;
    });
    m_undoValid = false;
    write();
}

void SavedMessagesController::setQuery(const QString &value)
{ if (value != m_query) { m_query = value; emitChanged(); } }
void SavedMessagesController::setRoomFilter(const QString &value)
{ if (value != m_roomFilter) { m_roomFilter = value; emitChanged(); } }
void SavedMessagesController::setTypeFilter(const QString &value)
{ if (value != m_typeFilter) { m_typeFilter = value; emitChanged(); } }

QVariantList SavedMessagesController::entries() const
{
    QVariantList out;
    for (const Ref &ref : m_refs) {
        const RoomInfo room = m_client ? m_client->roomInfo(ref.roomId) : RoomInfo{};
        const QString roomName = room.name.isEmpty() ? ref.roomId : room.name;
        if (!m_roomFilter.isEmpty() && ref.roomId != m_roomFilter) continue;
        if (m_typeFilter == QLatin1String("text")
            && ref.kind != QLatin1String("m.text")
            && ref.kind != QLatin1String("m.notice")
            && ref.kind != QLatin1String("m.emote")) continue;
        if (m_typeFilter == QLatin1String("media")
            && (ref.kind.isEmpty() || ref.kind == QLatin1String("m.text")
                || ref.kind == QLatin1String("m.notice")
                || ref.kind == QLatin1String("m.emote"))) continue;
        if (!m_query.isEmpty()
            && !roomName.contains(m_query, Qt::CaseInsensitive)
            && !ref.preview.contains(m_query, Qt::CaseInsensitive)
            && !ref.sender.contains(m_query, Qt::CaseInsensitive))
            continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"), ref.roomId },
            { QStringLiteral("roomName"), roomName },
            { QStringLiteral("eventId"), ref.eventId },
            { QStringLiteral("threadRootId"), ref.rootId },
            { QStringLiteral("savedAtMs"), ref.savedAtMs },
            { QStringLiteral("sender"), ref.sender },
            { QStringLiteral("senderName"),
              m_client && !ref.sender.isEmpty()
                  ? m_client->displayNameFor(ref.roomId, ref.sender) : QString() },
            { QStringLiteral("preview"), ref.preview },
            { QStringLiteral("kind"), ref.kind },
            { QStringLiteral("resolved"), ref.resolved },
            { QStringLiteral("available"), ref.available },
        });
    }
    return out;
}

QVariantList SavedMessagesController::rooms() const
{
    QVariantList out;
    QSet<QString> seen;
    for (const Ref &ref : m_refs) {
        if (seen.contains(ref.roomId)) continue;
        seen.insert(ref.roomId);
        const RoomInfo room = m_client ? m_client->roomInfo(ref.roomId) : RoomInfo{};
        out.append(QVariantMap{
            { QStringLiteral("roomId"), ref.roomId },
            { QStringLiteral("name"), room.name.isEmpty() ? ref.roomId : room.name },
        });
    }
    return out;
}

void SavedMessagesController::resolveVisible()
{
    pumpResolve();
}

void SavedMessagesController::pumpResolve()
{
    if (!m_client || !m_loaded || m_userId != m_client->currentUserId()) return;
    while (m_resolving.size() < 4) {
        const Ref *next = nullptr;
        for (const Ref &ref : std::as_const(m_refs)) {
            const QString id = key(ref.roomId, ref.eventId);
            if (!ref.resolved && !m_attempted.contains(id)
                && !m_redacted.contains(id)) {
                next = &ref;
                break;
            }
        }
        if (!next) break;
        const QString id = key(next->roomId, next->eventId);
        m_attempted.insert(id);
        m_resolving.insert(id);
        m_client->requestEventSource(next->roomId, next->eventId);
    }
    if (!m_resolving.isEmpty() && !m_resolveTimeout.isActive())
        m_resolveTimeout.start();
}

void SavedMessagesController::receiveSource(const QString &roomId,
                                            const QString &eventId, bool ok,
                                            const QString &json)
{
    const QString id = key(roomId, eventId);
    if (m_redacted.contains(id)) return;
    if (!m_resolving.remove(id)) return;
    const int i = indexOf(roomId, eventId);
    if (i >= 0) {
        Ref &ref = m_refs[i];
        ref.resolved = true;
        ref.available = false;
        ref.preview.clear();
        ref.sender.clear();
        ref.kind.clear();
        if (ok) {
            const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
            const QJsonObject content = object.value(QStringLiteral("content")).toObject();
            const bool redacted = object.value(QStringLiteral("unsigned"))
                .toObject().value(QStringLiteral("redacted_because")).isObject();
            if (!redacted && object.value(QStringLiteral("type")).toString()
                    == QLatin1String("m.sticker") && !content.isEmpty()) {
                ref.sender = object.value(QStringLiteral("sender")).toString();
                ref.kind = QStringLiteral("m.sticker");
                ref.preview = tr("Sticker");
                ref.available = true;
            }
            if (!redacted && object.value(QStringLiteral("type")).toString()
                    == QLatin1String("m.room.message") && !content.isEmpty()) {
                const QString msgtype = content.value(QStringLiteral("msgtype")).toString();
                ref.sender = object.value(QStringLiteral("sender")).toString();
                ref.kind = msgtype;
                ref.available = true;
                if (msgtype == QLatin1String("m.image")) ref.preview = tr("Image");
                else if (msgtype == QLatin1String("m.video")) ref.preview = tr("Video");
                else if (msgtype == QLatin1String("m.audio")) ref.preview = tr("Audio");
                else if (msgtype == QLatin1String("m.file")) ref.preview = tr("File");
                else ref.preview = content.value(QStringLiteral("body"))
                    .toString().simplified().left(140);
            }
        }
        emitChanged();
    }
    if (m_resolving.isEmpty()) m_resolveTimeout.stop();
    pumpResolve();
}

void SavedMessagesController::open(const QString &roomId,
                                   const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i < 0) return;
    const Ref &ref = m_refs.at(i);
    Q_EMIT openRequested(ref.roomId, ref.eventId, ref.rootId);
}

void SavedMessagesController::openRoot(const QString &roomId,
                                       const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i < 0 || m_refs.at(i).rootId.isEmpty()) return;
    Q_EMIT openRequested(roomId, m_refs.at(i).rootId, QString());
}

void SavedMessagesController::openRoom(const QString &roomId)
{
    for (const Ref &ref : std::as_const(m_refs)) {
        if (ref.roomId == roomId) {
            Q_EMIT openRequested(roomId, QString(), QString());
            return;
        }
    }
}
