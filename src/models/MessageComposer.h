#pragma once

#include "models/AttachmentQueueModel.h"
#include "models/MentionTokenizer.h"

#include <QList>
#include <QObject>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
    // Current mention ranges as [{start, length}] for the composer's chip
    // highlighter and atomic-delete key handling. Derived from the semantic
    // refs; re-announced on every text change (the refs re-anchor there).
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)

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

    QVariantList mentionRanges() const
    {
        QVariantList out;
        for (const mention::MentionRef &ref : m_mentionRefs) {
            out.append(QVariantMap{
                { QStringLiteral("start"), ref.start },
                { QStringLiteral("length"), ref.length },
            });
        }
        return out;
    }

    Q_INVOKABLE void send();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview);
    // sanitizedHtml (optional): the event's sanitized formatted body, used
    // to recover mention refs when the plain body carries display text
    // (sends after the plain-body reduction have no markdown to parse).
    Q_INVOKABLE void beginEdit(const QString &eventId,
                               const QString &currentBody,
                               const QString &sanitizedHtml = QString());
    // v0.4.1: enter thread-reply mode. `preview` is a short body preview of
    // the thread root, used to render the composer chip. Cleared by
    // cancelReplyOrEdit() or by the next successful send().
    Q_INVOKABLE void beginThreadReply(const QString &rootEventId,
                                      const QString &preview);
    Q_INVOKABLE void cancelReplyOrEdit();
    Q_INVOKABLE void reactTo(const QString &targetEventId, const QString &key);
    Q_INVOKABLE void redact(const QString &eventId);

    // v0.7 outgoing @-mentions. `mentionTokenAt` reports the active @-token at
    // the cursor ({active, start, query}); it is ref-aware, so a cursor sitting
    // over an already-inserted mention reports inactive (the pill is complete).
    // `insertMention` replaces that token with "@DisplayName ", records the
    // mention range, and returns the new cursor position. Send-time expansion
    // rewrites the recorded ranges into matrix.to markdown links.
    Q_INVOKABLE QVariantMap mentionTokenAt(const QString &text,
                                           int cursorPos) const;
    Q_INVOKABLE int insertMention(const QString &userId,
                                  const QString &displayName,
                                  int tokenStart, int cursorPos);

    // v0.7: MSC3381 poll actions on the current room. `threadRootId` is
    // non-empty when the acting delegate lives in the thread panel, so the
    // backend can route through the thread-focused timeline. Aggregation
    // and permission enforcement stay SDK/server-side.
    Q_INVOKABLE bool pollsSupported() const;
    Q_INVOKABLE void votePoll(const QString &pollEventId,
                              const QStringList &answerIds,
                              const QString &threadRootId = QString());
    Q_INVOKABLE void endPoll(const QString &pollEventId,
                             const QString &threadRootId = QString());
    // Creates the poll in the composer's current context: the open thread
    // when the composer is in thread-reply mode, else the room timeline.
    Q_INVOKABLE void createPoll(const QString &question,
                                const QStringList &answers,
                                bool undisclosed,
                                int maxSelections);
    Q_INVOKABLE void sendImageFromPath(const QString &localPath);
    Q_INVOKABLE void sendFileFromPath(const QString &localPath);
    // v0.7: MSC3245 voice message from VoiceRecorder's finalized output.
    // waveform entries are 0..=100; failure surfaces via
    // attachmentRejected exactly like tray attachments.
    Q_INVOKABLE void sendVoiceMessage(const QString &localPath,
                                      const QString &mime,
                                      qreal durationMs,
                                      const QVariantList &waveform);

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

    // Formatting toolbar (design shell): markdown wrap/unwrap over the
    // editor's selection, and the active-state flags for the toolbar chips.
    // Pure text transforms — see MarkdownFormat.
    Q_INVOKABLE QVariantMap toggleFormat(const QString &format,
                                         const QString &text,
                                         int selectionStart,
                                         int selectionEnd) const;
    Q_INVOKABLE QVariantMap formatState(const QString &text,
                                        int selectionStart,
                                        int selectionEnd) const;

Q_SIGNALS:
    void textChanged();
    void mentionRangesChanged();
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
    QList<mention::MentionRef> m_mentionRefs;
    // Voice send ops in flight, mapped to the recording file each one
    // owns: a failed queueing surfaces to the user (the tray model only
    // tracks its own entries), and the file is deleted when the op
    // resolves — the SDK reads the bytes into its queue at queueing time
    // and never re-reads the path.
    QHash<quint64, QString> m_voiceOps;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    QTimer  m_typingRefresh;
};
