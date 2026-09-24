#include "app/PinnedMessagesController.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

namespace {
// The raw SDK error never reaches QML.
QString messageForCategory(const QString &category)
{
    if (category == QLatin1String("forbidden")) {
        return QCoreApplication::translate(
            "PinnedMessagesController",
            "You do not have permission to change pinned messages in this "
            "room.");
    }
    if (category == QLatin1String("rate_limited")) {
        return QCoreApplication::translate(
            "PinnedMessagesController",
            "The server is rate limiting this action. Try again shortly.");
    }
    if (category == QLatin1String("not_found")) {
        return QCoreApplication::translate("PinnedMessagesController",
                                           "That message could not be found.");
    }
    if (category == QLatin1String("invalid")) {
        return QCoreApplication::translate("PinnedMessagesController",
                                           "The server rejected that change.");
    }
    return QCoreApplication::translate(
        "PinnedMessagesController",
        "Could not reach the server. Try again.");
}
} // namespace

PinnedMessagesController::PinnedMessagesController(QObject *parent)
    : QObject(parent)
{
}

void PinnedMessagesController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clearSnapshot();
    m_remoteProbed.clear();
    if (!m_client) {
        Q_EMIT supportedChanged();
        return;
    }

    connect(m_client, &MatrixClient::pinnedReceived, this,
            [this](quint64 opId, const QString &roomId,
                   const QVariantMap &snapshot) {
        // Match op id and room so a stale snapshot never repaints this one.
        if (opId == 0 || opId != m_fetchOp || roomId != m_roomId)
            return;
        m_fetchOp = 0;
        // Taken before the early return so a failed read cannot swallow it.
        const bool owed = m_refreshOwed;
        m_refreshOwed = false;
        if (!snapshot.value(QStringLiteral("ok")).toBool()) {
            // A failed read keeps the last known list.
            emitStateChanged();
            if (owed)
                fetch(/*allowRemote=*/false);
            return;
        }
        m_entries = snapshot.value(QStringLiteral("entries")).toList();
        m_ids = snapshot.value(QStringLiteral("ids")).toStringList();
        m_idSet = QSet<QString>(m_ids.cbegin(), m_ids.cend());
        m_total = snapshot.value(QStringLiteral("total")).toInt();
        m_truncated = snapshot.value(QStringLiteral("truncated")).toBool();
        m_canPin = snapshot.value(QStringLiteral("canPin")).toBool();
        emitStateChanged();
        if (owed)
            fetch(/*allowRemote=*/false);
    });

    connect(m_client, &MatrixClient::pinChangeFinished, this,
            [this](quint64 opId, const QString &roomId, const QString &eventId,
                   bool pin, bool ok, bool changed, const QString &category) {
        Q_UNUSED(changed);
        if (opId == 0 || opId != m_writeOp)
            return;
        m_writeOp = 0;
        const QString targetRoom = m_writeRoomId;
        m_writeRoomId.clear();
        m_writeEventId.clear();
        m_error = ok ? QString() : messageForCategory(category);
        emitStateChanged();
        Q_EMIT pinActionFinished(roomId, eventId, pin, ok, m_error);
        // Re-read either way, unless the user has moved to another room.
        if (targetRoom == m_roomId && !m_roomId.isEmpty())
            fetch(/*allowRemote=*/false);
    });

    // Remote change: re-read, so remote and local pins share one path.
    connect(m_client, &MatrixClient::pinnedEventsChanged, this,
            [this](const QString &roomId) {
        if (roomId == m_roomId && !m_roomId.isEmpty())
            fetch(/*allowRemote=*/false);
    });

    // One account's pins must never be shown under another's.
    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        clearSnapshot();
        m_remoteProbed.clear();
    });

    Q_EMIT supportedChanged();
    if (!m_roomId.isEmpty())
        refresh();
}

void PinnedMessagesController::emitStateChanged()
{
    ++m_revision;
    Q_EMIT stateChanged();
}

bool PinnedMessagesController::supported() const
{
    return m_client && m_client->supportsPinnedMessages();
}

void PinnedMessagesController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // In-flight answers for the previous room are rejected by the checks
    // above.
    clearSnapshot();
    Q_EMIT roomIdChanged();
    if (!m_roomId.isEmpty())
        refresh();
}

void PinnedMessagesController::clearSnapshot()
{
    m_fetchOp = 0;
    m_refreshOwed = false;
    m_writeOp = 0;
    m_writeRoomId.clear();
    m_writeEventId.clear();
    m_entries.clear();
    m_ids.clear();
    m_idSet.clear();
    m_total = 0;
    m_truncated = false;
    m_canPin = false;
    m_error.clear();
    emitStateChanged();
}

void PinnedMessagesController::refresh()
{
    // The /state fallback runs once per room per session.
    const bool allowRemote =
        !m_roomId.isEmpty() && !m_remoteProbed.contains(m_roomId);
    fetch(allowRemote);
}

void PinnedMessagesController::fetch(bool allowRemote)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    // One read at a time; a request made meanwhile is re-issued afterwards.
    if (m_fetchOp != 0) {
        m_refreshOwed = true;
        return;
    }
    const quint64 opId = m_client->requestPinnedMessages(m_roomId, allowRemote);
    if (opId == 0) {
        // Synchronous refusal: do not mark the room probed, so it is retried.
        return;
    }
    m_fetchOp = opId;
    if (allowRemote)
        m_remoteProbed.insert(m_roomId);
    emitStateChanged();
}

bool PinnedMessagesController::isPinned(const QString &eventId) const
{
    return !eventId.isEmpty() && m_idSet.contains(eventId);
}

bool PinnedMessagesController::canTogglePin(const QString &eventId,
                                            bool pin) const
{
    if (!supported() || eventId.isEmpty() || m_roomId.isEmpty())
        return false;
    if (!m_canPin || m_writeOp != 0)
        return false;
    return isPinned(eventId) != pin;
}

void PinnedMessagesController::pin(const QString &eventId)
{
    setPinned(eventId, true);
}

void PinnedMessagesController::unpin(const QString &eventId)
{
    setPinned(eventId, false);
}

void PinnedMessagesController::setPinned(const QString &eventId, bool pin)
{
    // Re-check: permissions can change between menu open and click.
    if (!canTogglePin(eventId, pin))
        return;
    const quint64 opId = m_client->setEventPinned(m_roomId, eventId, pin);
    if (opId == 0) {
        m_error = messageForCategory(QStringLiteral("network"));
        emitStateChanged();
        Q_EMIT pinActionFinished(m_roomId, eventId, pin, false, m_error);
        return;
    }
    m_writeOp = opId;
    m_writeRoomId = m_roomId;
    m_writeEventId = eventId;
    m_error.clear();
    emitStateChanged();
}
