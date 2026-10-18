#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixClient;

// The OPEN room's own posting facts: our membership, whether we may post an
// ordinary message, and whether we may remove another member's event. They
// gate the composer's posting block and moderator Delete.
//
// Independent of RoomInfoController, which describes whichever room the info
// panel, member popover or Space settings last inspected: a gate reading only
// that controller never acted in a room whose info had not been opened.
//
// - Read from local SDK state (MatrixClient::requestRoomOwnPermissions, no
//   network request), on every open, on every member or power-level poke
//   for the open room, and when the open room's encryption state changes (the
//   room list's encrypted/encryptionKnown pair, read on roomsChanged): which
//   event type a post needs depends on it.
// - Facts are per room id: switching rooms clears them before the next answer
//   lands, and an answer for another room, or for a superseded request, is
//   dropped. Sign-out and account switch close the room, which clears them.
// - The answer also carries kick, ban, unban, invite, topic and name for the
//   slash-command hints (commandFacts), under the same unknown rule.
// - Unknown is permissive for posting and not offered for moderation, exactly
//   as RoomInfoController reads the same answers. The server enforces both.
class OpenRoomPermissions : public QObject
{
    Q_OBJECT

    // The room these facts describe; follows AppController::currentRoomId.
    Q_PROPERTY(QString roomId READ roomId NOTIFY changed)
    // "joined", "invited", "left", "knocked", "banned", or empty (unknown).
    Q_PROPERTY(QString ownMembership READ ownMembership NOTIFY changed)
    // Whether an answer says anything about posting (a not-joined membership
    // or a may-post answer). False means: ask someone else, or allow.
    Q_PROPERTY(bool sendKnown READ sendKnown NOTIFY changed)
    // False only when a known answer refuses.
    Q_PROPERTY(bool canSendMessages READ canSendMessages NOTIFY changed)
    // Why canSendMessages is false, translated; empty when it is true.
    Q_PROPERTY(QString sendBlockedReason READ sendBlockedReason NOTIFY changed)
    // Whether an answer says anything about redacting others.
    Q_PROPERTY(bool redactKnown READ redactKnown NOTIFY changed)
    // True only when a known answer grants it.
    Q_PROPERTY(bool canRedactOthers READ canRedactOthers NOTIFY changed)
    // The slash-command facts the answer carried, by hint key ("kick", "ban",
    // "unban", "invite", "topic", "roomname") to a bool. A key is present only
    // when known: a real answer, or every key false for a known membership
    // other than "joined". Absent means: ask someone else, or allow.
    Q_PROPERTY(QVariantMap commandFacts READ commandFacts NOTIFY changed)

public:
    explicit OpenRoomPermissions(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setRoomId(const QString &roomId);

    QString roomId() const { return m_roomId; }
    QString ownMembership() const { return m_ownMembership; }
    bool sendKnown() const;
    bool canSendMessages() const;
    QString sendBlockedReason() const;
    bool redactKnown() const { return m_redactKnown; }
    bool canRedactOthers() const { return m_redactKnown && m_canRedactOthers; }
    QVariantMap commandFacts() const;

signals:
    void changed();

private:
    void request();
    void clearFacts();
    void onReceived(quint64 opId, const QString &roomId,
                    const QVariantMap &facts);
    void onRoomPoked(const QString &roomId);
    void onRoomsChanged();
    // 0 when the open room is not in the room list, else 1 | encrypted << 1 |
    // encryptionKnown << 2: the part of the room row a re-read depends on.
    int openRoomEncryptionState() const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    // The newest request for m_roomId; an older answer is superseded.
    quint64 m_op = 0;
    QString m_ownMembership;
    bool m_sendAnswered = false;
    bool m_canSendMessages = true;
    bool m_redactKnown = false;
    bool m_canRedactOthers = false;
    // Hint key to the SDK's answer, only for the keys it gave.
    QVariantMap m_moderation;
    // openRoomEncryptionState() as of the last read, so only a change re-reads.
    int m_encryptionState = 0;
};
