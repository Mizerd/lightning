#include "models/ScheduledSendController.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QUuid>

#include <algorithm>

namespace {
constexpr qint64 kMaxTimerMs = 24LL * 60 * 60 * 1000; // QTimer is 32-bit
constexpr int kMaxEntries = 64;
} // namespace

ScheduledSendController::ScheduledSendController(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { fireDue(); });
}

void ScheduledSendController::setSettings(SettingsManager *settings)
{
    m_settings = settings;
}

void ScheduledSendController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clearAll();
    m_serverScheduling = -1;
    Q_EMIT supportChanged();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::delayedEventsSupportReceived, this,
            [this](bool supported, bool) {
        const int next = supported ? 1 : 0;
        if (next == m_serverScheduling)
            return;
        m_serverScheduling = next;
        Q_EMIT supportChanged();
    });
    connect(m_client, &MatrixClient::scheduledSendFinished, this,
            [this](quint64 opId, const QString &, bool ok,
                   const QString &delayId, const QString &category) {
        for (Entry &e : m_entries) {
            if (e.op == 0 || e.op != opId)
                continue;
            e.op = 0;
            if (ok && !delayId.isEmpty()) {
                e.delayId = delayId;
                e.status = QStringLiteral("pending");
                e.error.clear();
                // Anything asked for while the schedule was in flight is
                // applied now that the server can be told about it.
                applyDeferredChanges(e);
            } else if (category == QLatin1String("encrypted_unsupported")) {
                // The server would hold the plaintext: fall back to the
                // local queue and say so.
                //
                // THIS VERDICT IS ALSO AN ENCRYPTION ANSWER, and it overrides
                // ours. `isVolatile` was decided at schedule() time from the
                // C++ RoomInfo cache; the Rust side consults the SDK's own
                // Room::encryption_state(), and this category is exactly the
                // case where the two disagreed. Taking only the routing half
                // of the answer and leaving the disk half meant §6's
                // "encrypted-room plaintext remains memory-only" was decided
                // by the weaker of two sources. persist() below rewrites the
                // whole list and skips volatile rows, so flipping the flag
                // here also REMOVES the row schedule() already wrote.
                e.isVolatile = true;
                becomeLocal(e);
                e.error = tr("Kept in Lightning instead: the server cannot "
                             "hold an encrypted message.");
                armTimer();
            } else {
                e.status = QStringLiteral("failed");
                e.error = tr("The server refused to schedule this message.");
                e.cancelRequested = false;
                e.resubmitAfterCancel = false;
            }
            persist();
            Q_EMIT pendingChanged();
            return;
        }
    });
    connect(m_client, &MatrixClient::scheduledUpdateFinished, this,
            [this](quint64 opId, const QString &, const QString &action,
                   bool ok, const QString &category) {
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.op == 0 || e.op != opId)
                continue;
            e.op = 0;
            if (action == QLatin1String("cancel")) {
                if (!ok) {
                    // NEVER resubmit after a failed cancel: the server may
                    // still hold (or already have sent) the original, and a
                    // replacement would deliver the message twice.
                    e.status = QStringLiteral("failed");
                    if (category == QLatin1String("not_found")) {
                        // Nothing left on the server to cancel later: a
                        // later Cancel is a plain local removal.
                        e.delayId.clear();
                        e.error = tr("The server no longer holds this message; it "
                                     "may already have been sent. Check the room "
                                     "before scheduling it again.");
                    } else {
                        e.error = tr("The server did not cancel this message.");
                    }
                    e.resubmitAfterCancel = false;
                    e.cancelRequested = false;
                    e.nextSendAtMs = -1;
                    e.hasNextText = false;
                } else if (e.resubmitAfterCancel) {
                    // The replacement, only now that the original is gone.
                    e.resubmitAfterCancel = false;
                    e.delayId.clear();
                    if (e.nextSendAtMs >= 0)
                        e.sendAtMs = e.nextSendAtMs;
                    if (e.hasNextText) {
                        e.body = e.nextBody;
                        e.html = e.nextHtml;
                    }
                    e.nextSendAtMs = -1;
                    e.hasNextText = false;
                    e.nextBody.clear();
                    e.nextHtml.clear();
                    e.status = QStringLiteral("pending");
                    e.error.clear();
                    submitServer(e);
                    if (e.mode == QLatin1String("local"))
                        armTimer();
                } else {
                    m_entries.removeAt(i);
                }
            } else if (action == QLatin1String("send")) {
                if (ok)
                    m_entries.removeAt(i);
                else {
                    e.status = QStringLiteral("failed");
                    e.error = tr("The server did not send this message.");
                }
            }
            persist();
            Q_EMIT pendingChanged();
            return;
        }
    });
    connect(m_client, &MatrixClient::roomSendFinished, this,
            [this](quint64 opId, const QString &, bool ok, const QString &) {
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.sendOp == 0 || e.sendOp != opId)
                continue;
            e.sendOp = 0;
            if (ok) {
                m_entries.removeAt(i);
            } else {
                // Kept, and said plainly: "Send now" retries it.
                e.status = QStringLiteral("failed");
                e.error = tr("The room did not accept this message. It was "
                             "not sent.");
            }
            persist();
            armTimer();
            Q_EMIT pendingChanged();
            return;
        }
    });
    connect(m_client, &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState state) {
        if (state == MatrixClient::Syncing) {
            load();
            probeSupport();
            fireDue();
        }
    });
    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        // The next account loads ITS queue; this one's memory copy goes.
        clearAll();
        m_serverScheduling = -1;
        Q_EMIT supportChanged();
    });
}

bool ScheduledSendController::connected() const
{
    return m_client && m_client->connectionState() == MatrixClient::Syncing;
}

void ScheduledSendController::clearAll()
{
    m_entries.clear();
    m_loaded = false;
    m_timer.stop();
    Q_EMIT pendingChanged();
}

QVariantMap ScheduledSendController::bodySpecFor(const Entry &e) const
{
    if (e.html.isEmpty())
        return {};
    return QVariantMap{
        { QStringLiteral("format"), QStringLiteral("html") },
        { QStringLiteral("html"), e.html },
    };
}

bool ScheduledSendController::roomIsEncrypted(const QString &roomId) const
{
    if (!m_client)
        return true; // fail closed
    const RoomInfo info = m_client->roomInfo(roomId);
    // Unknown encryption state fails closed, like the draft store.
    return info.encrypted || !info.encryptionKnown;
}

bool ScheduledSendController::wouldUseServer(const QString &roomId,
                                             const QString &threadRootId,
                                             const QString &replyToEventId) const
{
    return m_serverScheduling == 1 && !roomIsEncrypted(roomId)
        && threadRootId.isEmpty() && replyToEventId.isEmpty();
}

bool ScheduledSendController::busy(const Entry &e) const
{
    return e.op != 0 || e.sendOp != 0 || e.cancelRequested
        || e.status == QLatin1String("sending");
}

QVariantMap ScheduledSendController::toMap(const Entry &e) const
{
    QString roomName;
    if (m_client)
        roomName = m_client->roomInfo(e.roomId).name;
    return QVariantMap{
        { QStringLiteral("id"), e.id },
        { QStringLiteral("roomId"), e.roomId },
        { QStringLiteral("roomName"), roomName.isEmpty() ? e.roomId : roomName },
        { QStringLiteral("body"), e.body },
        { QStringLiteral("html"), e.html },
        { QStringLiteral("sendAtMs"), e.sendAtMs },
        { QStringLiteral("mode"), e.mode },
        { QStringLiteral("status"), e.status },
        { QStringLiteral("busy"), busy(e) },
        { QStringLiteral("error"), e.error },
        { QStringLiteral("volatile"), e.isVolatile },
        { QStringLiteral("delayId"), e.delayId },
        { QStringLiteral("threadRootId"), e.threadRootId },
        { QStringLiteral("replyToEventId"), e.replyToEventId },
    };
}

QVariantList ScheduledSendController::pending() const
{
    QList<Entry> sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry &a, const Entry &b) { return a.sendAtMs < b.sendAtMs; });
    QVariantList out;
    for (const Entry &e : sorted)
        out.append(toMap(e));
    return out;
}

int ScheduledSendController::pendingCount() const
{
    int n = 0;
    for (const Entry &e : m_entries)
        if (e.status == QLatin1String("pending"))
            ++n;
    return n;
}

QVariantList ScheduledSendController::pendingForRoom(const QString &roomId) const
{
    QVariantList out;
    for (const QVariant &v : pending())
        if (v.toMap().value(QStringLiteral("roomId")).toString() == roomId)
            out.append(v);
    return out;
}

ScheduledSendController::Entry *ScheduledSendController::find(const QString &id)
{
    for (Entry &e : m_entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

int ScheduledSendController::indexOf(const QString &id) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).id == id)
            return i;
    return -1;
}

void ScheduledSendController::probeSupport()
{
    if (m_client)
        m_client->probeDelayedEvents();
}

void ScheduledSendController::persist()
{
    if (!m_settings)
        return;
    QVariantList rows;
    for (const Entry &e : m_entries) {
        // Encrypted-room plaintext never touches disk.
        if (e.isVolatile)
            continue;
        QVariantMap m = toMap(e);
        m.remove(QStringLiteral("roomName"));
        m.remove(QStringLiteral("busy"));
        m.insert(QStringLiteral("mentionIds"), e.mentionIds);
        rows.append(m);
    }
    m_settings->setScheduledSends(rows);
}

void ScheduledSendController::load()
{
    if (m_loaded || !m_settings)
        return;
    m_loaded = true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QVariantList rows = m_settings->scheduledSends();
    bool changed = false;
    for (const QVariant &v : rows) {
        const QVariantMap m = v.toMap();
        Entry e;
        e.id = m.value(QStringLiteral("id")).toString();
        e.roomId = m.value(QStringLiteral("roomId")).toString();
        e.body = m.value(QStringLiteral("body")).toString();
        e.html = m.value(QStringLiteral("html")).toString();
        e.mentionIds = m.value(QStringLiteral("mentionIds")).toStringList();
        e.threadRootId = m.value(QStringLiteral("threadRootId")).toString();
        e.replyToEventId = m.value(QStringLiteral("replyToEventId")).toString();
        e.sendAtMs = m.value(QStringLiteral("sendAtMs")).toLongLong();
        e.mode = m.value(QStringLiteral("mode")).toString();
        e.status = m.value(QStringLiteral("status")).toString();
        e.error = m.value(QStringLiteral("error")).toString();
        e.delayId = m.value(QStringLiteral("delayId")).toString();
        if (e.id.isEmpty() || e.roomId.isEmpty() || e.body.isEmpty())
            continue;
        if (e.mode == QLatin1String("server")) {
            if (e.delayId.isEmpty()) {
                // Persisted before the server answered and never confirmed:
                // it cannot be cancelled and may or may not exist. Reported,
                // never re-submitted on a guess.
                if (e.status != QLatin1String("failed")) {
                    e.status = QStringLiteral("failed");
                    e.error = tr("Lightning closed before the server confirmed "
                                 "this message. Check the room before "
                                 "scheduling it again.");
                    changed = true;
                }
            } else if (e.sendAtMs + kServerRetireGraceMs < now) {
                // The server sent it at the deadline; there is nothing left
                // to hold or cancel.
                changed = true;
                continue;
            }
        } else if (e.status == QLatin1String("sending")) {
            // A row caught mid-dispatch by a crash is NOT re-fired: it is
            // reported so the user decides, never sent twice on a guess.
            e.status = QStringLiteral("failed");
            e.error = tr("Lightning closed while sending this. Check the room "
                         "before sending it again.");
            changed = true;
        }
        m_entries.append(e);
    }
    if (changed)
        persist();
    armTimer();
    Q_EMIT pendingChanged();
}

QString ScheduledSendController::schedule(const QVariantMap &message,
                                          qint64 sendAtMs)
{
    if (!m_client)
        return {};
    load();
    const QString roomId = message.value(QStringLiteral("roomId")).toString();
    const QString body = message.value(QStringLiteral("body")).toString().trimmed();
    if (roomId.isEmpty() || body.isEmpty())
        return {};
    if (sendAtMs <= QDateTime::currentMSecsSinceEpoch())
        return {};
    if (m_entries.size() >= kMaxEntries)
        return {};
    Entry e;
    e.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    e.roomId = roomId;
    e.body = body;
    e.html = message.value(QStringLiteral("html")).toString();
    e.mentionIds = message.value(QStringLiteral("mentionIds")).toStringList();
    e.threadRootId = message.value(QStringLiteral("threadRootId")).toString();
    e.replyToEventId = message.value(QStringLiteral("replyToEventId")).toString();
    e.sendAtMs = sendAtMs;
    e.isVolatile = roomIsEncrypted(roomId);
    e.status = QStringLiteral("pending");
    e.mode = wouldUseServer(roomId, e.threadRootId, e.replyToEventId)
        ? QStringLiteral("server")
        : QStringLiteral("local");
    m_entries.append(e);
    if (e.mode == QLatin1String("server"))
        submitServer(m_entries.last());
    persist();
    armTimer();
    Q_EMIT pendingChanged();
    return e.id;
}

void ScheduledSendController::becomeLocal(Entry &e)
{
    e.mode = QStringLiteral("local");
    e.status = QStringLiteral("pending");
    e.delayId.clear();
    e.op = 0;
    e.cancelRequested = false;
    e.resubmitAfterCancel = false;
    e.nextSendAtMs = -1;
    e.hasNextText = false;
    e.nextBody.clear();
    e.nextHtml.clear();
}

void ScheduledSendController::submitServer(Entry &e)
{
    const qint64 delay = e.sendAtMs - QDateTime::currentMSecsSinceEpoch();
    if (delay <= 0 || !m_client) {
        becomeLocal(e);
        return;
    }
    const quint64 op = m_client->scheduleMessage(e.roomId, e.body, bodySpecFor(e),
                                                 e.mentionIds, delay);
    if (op == 0) {
        becomeLocal(e);
        return;
    }
    e.op = op;
}

void ScheduledSendController::beginServerCancel(Entry &e)
{
    const quint64 op = m_client
        ? m_client->updateScheduledMessage(e.delayId, QStringLiteral("cancel"))
        : 0;
    if (op == 0) {
        // No way to reach the server: the entry stays as it is, honestly.
        e.status = QStringLiteral("failed");
        e.error = tr("The server could not be asked to cancel this message.");
        e.resubmitAfterCancel = false;
        e.cancelRequested = false;
        return;
    }
    e.op = op;
    e.status = QStringLiteral("sending"); // busy until the server answers
}

void ScheduledSendController::applyDeferredChanges(Entry &e)
{
    if (e.cancelRequested) {
        e.cancelRequested = false;
        e.resubmitAfterCancel = false;
        beginServerCancel(e);
        return;
    }
    if (e.nextSendAtMs >= 0 || e.hasNextText) {
        e.resubmitAfterCancel = true;
        beginServerCancel(e);
    }
}

void ScheduledSendController::cancel(const QString &id)
{
    Entry *e = find(id);
    if (!e)
        return;
    if (e->mode == QLatin1String("server")) {
        if (e->op != 0 && e->delayId.isEmpty()) {
            // The schedule itself is still in flight: cancel once the
            // server has told us what to cancel.
            e->cancelRequested = true;
            e->status = QStringLiteral("sending");
            Q_EMIT pendingChanged();
            return;
        }
        if (busy(*e))
            return;
        if (!e->delayId.isEmpty()) {
            e->resubmitAfterCancel = false;
            beginServerCancel(*e);
            persist();
            Q_EMIT pendingChanged();
            return;
        }
        // A failed server entry with nothing on the server: plain removal.
    } else if (e->sendOp != 0) {
        return; // a room send is in flight; the result decides
    }
    m_entries.removeIf([&id](const Entry &x) { return x.id == id; });
    persist();
    armTimer();
    Q_EMIT pendingChanged();
}

void ScheduledSendController::sendNow(const QString &id)
{
    Entry *e = find(id);
    if (!e || busy(*e))
        return;
    if (e->mode == QLatin1String("server") && !e->delayId.isEmpty() && m_client) {
        const quint64 op = m_client->updateScheduledMessage(e->delayId,
                                                            QStringLiteral("send"));
        if (op != 0) {
            e->op = op;
            e->status = QStringLiteral("sending");
            Q_EMIT pendingChanged();
            return;
        }
        // The server holds this message and we could not reach it. Falling
        // through to the local queue here would call becomeLocal(), which
        // CLEARS delayId — discarding the only handle to the server-held
        // event while the homeserver still sends it at its deadline. That is
        // the double delivery this class's own contract forbids, and it also
        // makes the server copy uncancellable. Refuse, keep the delay id, and
        // say so; the schedule is still live.
        e->error = tr("Lightning could not reach the server to send this now. "
                      "It is still scheduled.");
        Q_EMIT pendingChanged();
        return;
    }
    if (e->mode == QLatin1String("server"))
        becomeLocal(*e);
    e->status = QStringLiteral("pending");
    e->error.clear();
    dispatchLocal(*e);
}

void ScheduledSendController::reschedule(const QString &id, qint64 sendAtMs)
{
    Entry *e = find(id);
    if (!e || sendAtMs <= QDateTime::currentMSecsSinceEpoch())
        return;
    if (e->mode == QLatin1String("server")) {
        if (e->op != 0 && e->delayId.isEmpty()) {
            // Applied once the delay id arrives (cancel, then resubmit).
            e->nextSendAtMs = sendAtMs;
            Q_EMIT pendingChanged();
            return;
        }
        if (busy(*e))
            return;
        if (!e->delayId.isEmpty()) {
            // MSC4140's restart only re-arms the ORIGINAL timeout: a new
            // deadline is a cancel plus a fresh delayed event — and the
            // fresh one is created only once the cancel has succeeded.
            e->nextSendAtMs = sendAtMs;
            e->resubmitAfterCancel = true;
            beginServerCancel(*e);
            persist();
            Q_EMIT pendingChanged();
            return;
        }
        // A failed server entry holds nothing on the server: it becomes a
        // local entry, which the timer can actually fire.
        becomeLocal(*e);
    } else if (e->sendOp != 0) {
        return;
    }
    e->sendAtMs = sendAtMs;
    e->status = QStringLiteral("pending");
    e->error.clear();
    persist();
    armTimer();
    Q_EMIT pendingChanged();
}

void ScheduledSendController::updateText(const QString &id, const QString &body,
                                         const QString &html)
{
    Entry *e = find(id);
    if (!e || body.trimmed().isEmpty())
        return;
    const QString trimmed = body.trimmed();
    if (e->mode == QLatin1String("server")) {
        if (e->op != 0 && e->delayId.isEmpty()) {
            e->hasNextText = true;
            e->nextBody = trimmed;
            e->nextHtml = html;
            Q_EMIT pendingChanged();
            return;
        }
        if (busy(*e))
            return;
        if (!e->delayId.isEmpty()) {
            // The server holds the OLD content: replace the delayed event,
            // cancel first.
            e->hasNextText = true;
            e->nextBody = trimmed;
            e->nextHtml = html;
            e->resubmitAfterCancel = true;
            beginServerCancel(*e);
            persist();
            Q_EMIT pendingChanged();
            return;
        }
        becomeLocal(*e);
    } else if (e->sendOp != 0) {
        return;
    }
    e->body = trimmed;
    e->html = html;
    persist();
    Q_EMIT pendingChanged();
}

void ScheduledSendController::armTimer()
{
    m_timer.stop();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 earliest = -1;
    for (const Entry &e : m_entries) {
        qint64 at = -1;
        if (e.mode == QLatin1String("local") && e.status == QLatin1String("pending"))
            at = e.sendAtMs;
        else if (e.mode == QLatin1String("server") && !e.delayId.isEmpty()
                 && e.status == QLatin1String("pending"))
            at = e.sendAtMs + kServerRetireGraceMs; // retirement tick
        if (at < 0)
            continue;
        if (earliest < 0 || at < earliest)
            earliest = at;
    }
    if (earliest < 0)
        return;
    m_timer.start(static_cast<int>(qBound<qint64>(0, earliest - now, kMaxTimerMs)));
}

void ScheduledSendController::fireDue()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Server-held entries past their deadline were sent by the server;
    // retire them whether or not we are connected.
    bool retired = false;
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        const Entry &e = m_entries.at(i);
        if (e.mode == QLatin1String("server") && !e.delayId.isEmpty()
            && e.status == QLatin1String("pending") && e.op == 0
            && e.sendAtMs + kServerRetireGraceMs < now) {
            m_entries.removeAt(i);
            retired = true;
        }
    }
    if (retired) {
        persist();
        Q_EMIT pendingChanged();
    }
    if (!connected()) {
        // Not connected: nothing leaves; the connection handler retries.
        armTimer();
        return;
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.mode == QLatin1String("local") && e.status == QLatin1String("pending")
            && e.sendAtMs <= now)
            dispatchLocal(e);
    }
    armTimer();
}

void ScheduledSendController::dispatchLocal(Entry &e)
{
    if (!m_client || e.sendOp != 0)
        return;
    if (e.status != QLatin1String("pending") && e.status != QLatin1String("failed"))
        return;
    if (!connected()) {
        e.error = tr("Waiting for the connection.");
        Q_EMIT pendingChanged();
        return;
    }
    // Marked and persisted BEFORE the send: the one guard against a double
    // dispatch across a crash or a reconnect.
    e.status = QStringLiteral("sending");
    e.error.clear();
    persist();
    Q_EMIT pendingChanged();
    const QVariantMap spec = bodySpecFor(e);
    // The ROOM-level send works for any room, open or not, and answers on
    // roomSendFinished; the entry leaves only on the room's acceptance.
    const quint64 op = m_client->sendRoomMessage(e.roomId, e.body, spec, e.mentionIds,
                                                 e.replyToEventId, e.threadRootId);
    if (op != 0) {
        e.sendOp = op;
        return;
    }
    // A backend without the room-level send (mock / HTTP): the historical
    // timeline sends, which report nothing back.
    if (!e.threadRootId.isEmpty())
        m_client->sendThreadReplyTo(e.roomId, e.threadRootId, e.replyToEventId,
                                    e.body, e.mentionIds, spec);
    else if (!e.replyToEventId.isEmpty())
        m_client->sendReply(e.roomId, e.replyToEventId, e.body, e.mentionIds, spec);
    else
        m_client->sendTextMessage(e.roomId, e.body, e.mentionIds, spec);
    const QString id = e.id;
    m_entries.removeIf([&id](const Entry &x) { return x.id == id; });
    persist();
    Q_EMIT pendingChanged();
}
