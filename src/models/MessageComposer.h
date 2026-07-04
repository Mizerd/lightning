#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class MatrixClient;

class MessageComposer : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool canSend READ canSend NOTIFY canSendChanged)
    Q_PROPERTY(QString replyingToEventId READ replyingToEventId NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyingToSender READ replyingToSender NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyingToPreview READ replyingToPreview NOTIFY replyStateChanged)
    Q_PROPERTY(QString editingEventId READ editingEventId NOTIFY editStateChanged)
    Q_PROPERTY(bool isReplying READ isReplying NOTIFY replyStateChanged)
    Q_PROPERTY(bool isEditing READ isEditing NOTIFY editStateChanged)

public:
    explicit MessageComposer(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString text() const { return m_text; }
    void setText(const QString &t);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &r);

    QString replyingToEventId() const { return m_replyingToEventId; }
    QString replyingToSender()  const { return m_replyingToSender; }
    QString replyingToPreview() const { return m_replyingToPreview; }
    QString editingEventId()    const { return m_editingEventId; }
    bool isReplying() const { return !m_replyingToEventId.isEmpty(); }
    bool isEditing()  const { return !m_editingEventId.isEmpty(); }

    bool canSend() const;

    Q_INVOKABLE void send();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview);
    Q_INVOKABLE void beginEdit(const QString &eventId,
                               const QString &currentBody);
    Q_INVOKABLE void cancelReplyOrEdit();
    Q_INVOKABLE void reactTo(const QString &targetEventId, const QString &key);
    Q_INVOKABLE void redact(const QString &eventId);
    Q_INVOKABLE void sendImageFromPath(const QString &localPath);
    Q_INVOKABLE void sendFileFromPath(const QString &localPath);

Q_SIGNALS:
    void textChanged();
    void roomIdChanged();
    void canSendChanged();
    void replyStateChanged();
    void editStateChanged();

private:
    void updateCanSend();
    void refreshTypingState();
    void stopTyping();

    MatrixClient *m_client = nullptr;
    QString m_text;
    QString m_roomId;
    QString m_replyingToEventId;
    QString m_replyingToSender;
    QString m_replyingToPreview;
    QString m_editingEventId;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    QTimer  m_typingRefresh;
};
