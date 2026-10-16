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
//   network request), on every open and on every member or power-level poke
//   for the open room.
// - Facts are per room id: switching rooms clears them before the next answer
//   lands, and an answer for another room, or for a superseded request, is
//   dropped. Sign-out and account switch close the room, which clears them.
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

signals:
    void changed();

private:
    void request();
    void clearFacts();
    void onReceived(quint64 opId, const QString &roomId,
                    const QVariantMap &facts);
    void onRoomPoked(const QString &roomId);

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    // The newest request for m_roomId; an older answer is superseded.
    quint64 m_op = 0;
    QString m_ownMembership;
    bool m_sendAnswered = false;
    bool m_canSendMessages = true;
    bool m_redactKnown = false;
    bool m_canRedactOthers = false;
};
