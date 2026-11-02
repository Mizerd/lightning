#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixClient;

class OpenRoomPermissions : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId NOTIFY changed)
    Q_PROPERTY(QString ownMembership READ ownMembership NOTIFY changed)
    Q_PROPERTY(bool sendKnown READ sendKnown NOTIFY changed)
    // False only when a known answer refuses.
    Q_PROPERTY(bool canSendMessages READ canSendMessages NOTIFY changed)
    Q_PROPERTY(QString sendBlockedReason READ sendBlockedReason NOTIFY changed)
    Q_PROPERTY(bool redactKnown READ redactKnown NOTIFY changed)
    Q_PROPERTY(bool canRedactOthers READ canRedactOthers NOTIFY changed)
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
    int openRoomEncryptionState() const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_op = 0;
    QString m_ownMembership;
    bool m_sendAnswered = false;
    bool m_canSendMessages = true;
    bool m_redactKnown = false;
    bool m_canRedactOthers = false;
    QVariantMap m_moderation;
    int m_encryptionState = 0;
};
