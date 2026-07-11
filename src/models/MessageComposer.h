#pragma once

#include "models/AttachmentQueueModel.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

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
    Q_PROPERTY(QString threadRootId READ threadRootId NOTIFY threadStateChanged)
    Q_PROPERTY(QString threadPreview READ threadPreview NOTIFY threadStateChanged)
    Q_PROPERTY(bool inThread READ inThread NOTIFY threadStateChanged)
    // v0.5.9: attachment tray (Rust backend; SDK send queue).
    Q_PROPERTY(AttachmentQueueModel* attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported NOTIFY roomIdChanged)

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
    QString threadRootId()      const { return m_threadRootId; }
    QString threadPreview()     const { return m_threadPreview; }
    bool isReplying() const { return !m_replyingToEventId.isEmpty(); }
    bool isEditing()  const { return !m_editingEventId.isEmpty(); }
    bool inThread()   const { return !m_threadRootId.isEmpty(); }

    bool canSend() const;

    Q_INVOKABLE void send();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview);
    Q_INVOKABLE void beginEdit(const QString &eventId,
                               const QString &currentBody);
    // v0.4.1: enter thread-reply mode. `preview` is a short body preview of
    // the thread root, used to render the composer chip. Cleared by
    // cancelReplyOrEdit() or by the next successful send().
    Q_INVOKABLE void beginThreadReply(const QString &rootEventId,
                                      const QString &preview);
    Q_INVOKABLE void cancelReplyOrEdit();
    Q_INVOKABLE void reactTo(const QString &targetEventId, const QString &key);
    Q_INVOKABLE void redact(const QString &eventId);
    Q_INVOKABLE void sendImageFromPath(const QString &localPath);
    Q_INVOKABLE void sendFileFromPath(const QString &localPath);

    // v0.5.9 attachment tray.
    AttachmentQueueModel *attachments() const { return m_attachments; }
    bool hasAttachments() const { return m_attachments && !m_attachments->isEmpty(); }
    bool attachmentsSupported() const;
    // Add a picked/dropped file; emits attachmentRejected(reason) when the
    // file fails validation (directory, unreadable, empty, over limit).
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    // Intercept Ctrl+V: returns true when the clipboard held an image or
    // local file URLs and they were queued as attachments; false lets the
    // text editor perform a normal text paste. Plain text that merely looks
    // like a path is never treated as a file.
    Q_INVOKABLE bool pasteFromClipboard();

Q_SIGNALS:
    void textChanged();
    void roomIdChanged();
    void canSendChanged();
    void replyStateChanged();
    void editStateChanged();
    void threadStateChanged();
    void attachmentsChanged();
    void attachmentRejected(const QString &reason);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void updateCanSend();
    void refreshTypingState();
    void stopTyping();
    void dispatchAttachments();

    MatrixClient *m_client = nullptr;
    AttachmentQueueModel *m_attachments = nullptr;
    QString m_text;
    QString m_roomId;
    QString m_replyingToEventId;
    QString m_replyingToSender;
    QString m_replyingToPreview;
    QString m_editingEventId;
    QString m_threadRootId;
    QString m_threadPreview;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    QTimer  m_typingRefresh;
};
