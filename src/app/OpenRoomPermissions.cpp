#include "app/OpenRoomPermissions.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

namespace {
// Unknown membership is not "not joined": nothing has said so yet.
bool knownNotJoined(const QString &ownMembership)
{
    return !ownMembership.isEmpty()
        && ownMembership != QLatin1String("joined");
}
} // namespace

OpenRoomPermissions::OpenRoomPermissions(QObject *parent) : QObject(parent) {}

void OpenRoomPermissions::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    // Another client is another account's answers.
    m_op = 0;
    clearFacts();
    if (m_client) {
        connect(m_client, &MatrixClient::roomOwnPermissionsReceived, this,
                &OpenRoomPermissions::onReceived);
        // Membership and power-level changes both arrive on this poke.
        connect(m_client, &MatrixClient::roomMemberEventSeen, this,
                &OpenRoomPermissions::onRoomPoked);
        // The room row carries the open room's encryption state; an existing
        // signal, no new request.
        connect(m_client, &MatrixClient::roomsChanged, this,
                &OpenRoomPermissions::onRoomsChanged);
        connect(m_client, &MatrixClient::loggedOut, this,
                [this]() { setRoomId(QString()); });
    }
    request();
    Q_EMIT changed();
}

void OpenRoomPermissions::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // Never show one room's answer in another, not even until the next one
    // lands.
    m_op = 0;
    clearFacts();
    request();
    Q_EMIT changed();
}

bool OpenRoomPermissions::sendKnown() const
{
    return knownNotJoined(m_ownMembership) || m_sendAnswered;
}

bool OpenRoomPermissions::canSendMessages() const
{
    return !knownNotJoined(m_ownMembership)
        && (!m_sendAnswered || m_canSendMessages);
}

QString OpenRoomPermissions::sendBlockedReason() const
{
    // RoomInfoController's own sentences, under its context, so the composer
    // says the same words whichever answer it read and the catalogues gain
    // nothing to translate twice.
    if (knownNotJoined(m_ownMembership))
        return QCoreApplication::translate("RoomInfoController",
                                           "You are not in this room.");
    if (m_sendAnswered && !m_canSendMessages)
        return QCoreApplication::translate(
            "RoomInfoController",
            "You do not have permission to post in this room.");
    return {};
}

void OpenRoomPermissions::request()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    // This read answers for the room as it is now; later changes are measured
    // against it.
    m_encryptionState = openRoomEncryptionState();
    // A synchronous refusal (0) keeps the previous answer for this room.
    const quint64 opId = m_client->requestRoomOwnPermissions(m_roomId);
    if (opId != 0)
        m_op = opId;
}

void OpenRoomPermissions::clearFacts()
{
    m_ownMembership.clear();
    m_sendAnswered = false;
    m_canSendMessages = true;
    m_redactKnown = false;
    m_canRedactOthers = false;
    m_moderation.clear();
}

QVariantMap OpenRoomPermissions::commandFacts() const
{
    // Our membership is not "joined": none of the commands can work, whatever
    // the power levels say, the same reading as the posting block.
    if (!m_ownMembership.isEmpty() && m_ownMembership != QLatin1String("joined")) {
        QVariantMap none;
        for (const char *key :
             {"kick", "ban", "unban", "invite", "topic", "roomname"})
            none.insert(QLatin1String(key), false);
        return none;
    }
    return m_moderation;
}

void OpenRoomPermissions::onReceived(quint64 opId, const QString &roomId,
                                     const QVariantMap &facts)
{
    if (opId == 0 || opId != m_op || roomId != m_roomId)
        return; // another room, or a request a newer one superseded
    m_op = 0;
    // An absent key is unknown, never a refusal or a grant.
    const QString sendKey = QStringLiteral("canSendMessages");
    const QString redactKey = QStringLiteral("canRedactOthers");
    const QString membership =
        facts.value(QStringLiteral("ownMembership")).toString();
    const bool sendAnswered = facts.contains(sendKey);
    const bool canSend = !sendAnswered || facts.value(sendKey).toBool();
    const bool redactKnown = facts.contains(redactKey);
    const bool canRedact = redactKnown && facts.value(redactKey).toBool();
    static const struct { const char *key; const char *hint; } kModeration[] = {
        {"canKick", "kick"},         {"canBan", "ban"},
        {"canUnban", "unban"},       {"canInvite", "invite"},
        {"canEditTopic", "topic"},   {"canEditName", "roomname"},
    };
    QVariantMap moderation;
    for (const auto &m : kModeration) {
        if (facts.contains(QLatin1String(m.key)))
            moderation.insert(QLatin1String(m.hint),
                              facts.value(QLatin1String(m.key)).toBool());
    }
    // Every timeline row binds canRedactOthers: an answer that changes
    // nothing (most pokes) must not make them all re-evaluate.
    if (membership == m_ownMembership && sendAnswered == m_sendAnswered
        && canSend == m_canSendMessages && redactKnown == m_redactKnown
        && canRedact == m_canRedactOthers && moderation == m_moderation) {
        return;
    }
    m_ownMembership = membership;
    m_sendAnswered = sendAnswered;
    m_canSendMessages = canSend;
    m_redactKnown = redactKnown;
    m_canRedactOthers = canRedact;
    m_moderation = moderation;
    Q_EMIT changed();
}

void OpenRoomPermissions::onRoomPoked(const QString &roomId)
{
    // Only the open room is read; the answer replaces the current one, which
    // stays until it lands.
    if (!roomId.isEmpty() && roomId == m_roomId)
        request();
}

int OpenRoomPermissions::openRoomEncryptionState() const
{
    if (!m_client || m_roomId.isEmpty())
        return 0;
    // The per-room lookup: the SDK client answers from its index, not by
    // copying the whole list on every roomsChanged. An unknown room comes
    // back with an empty id.
    const RoomInfo room = m_client->roomInfo(m_roomId);
    if (room.id != m_roomId)
        return 0;
    return 1 | (room.encrypted ? 2 : 0) | (room.encryptionKnown ? 4 : 0);
}

void OpenRoomPermissions::onRoomsChanged()
{
    // Most room-list changes say nothing about this room's encryption: only a
    // change in it asks again. The answer replaces the current one, which
    // stays until it lands (unknown stays allowed, as before any answer).
    if (m_roomId.isEmpty() || openRoomEncryptionState() == m_encryptionState)
        return;
    request();
}
