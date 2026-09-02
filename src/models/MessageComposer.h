#pragma once

#include "models/AttachmentQueueModel.h"
#include "models/MentionTokenizer.h"
#include "models/SlashCommands.h"

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
    Q_PROPERTY(QString replyingToMediaKey READ replyingToMediaKey NOTIFY replyStateChanged)
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
    // Composer policy, pushed in from QML (qml/Main.qml) the same way the
    // theme settings are pushed into AppTheme: text typed alongside an
    // attachment is sent as that attachment's CAPTION — one event instead of
    // two — rather than as a separate message. Default false, which is the
    // behaviour every release so far has had.
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption NOTIFY sendTextAsCaptionChanged)
    // Current mention ranges as [{start, length}] for the composer's chip
    // highlighter and atomic-delete key handling. Derived from the semantic
    // refs; re-announced on every text change (the refs re-anchor there).
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)
    // v0.9 slash commands. `commandError` is a non-destructive refusal (an
    // unknown command or missing arguments): the draft stays, the message is
    // not sent, and the QML bar shows the text with a "send as a message"
    // affordance (sendBypassingCommands). Cleared by any text change, a room
    // change, or a successful send. `commandCompletions` is the popup model
    // while the command word is being typed: [{name, argsHint, description,
    // enabled}] — `enabled` reflects commandPermissions, a courtesy hint
    // pushed in from QML (RoomInfoController's can* booleans); the server
    // stays the enforcer.
    Q_PROPERTY(QString commandError READ commandError
                   NOTIFY commandErrorChanged)
    Q_PROPERTY(QVariantList commandCompletions READ commandCompletions
                   NOTIFY commandCompletionsChanged)
    Q_PROPERTY(QVariantMap commandPermissions READ commandPermissions
                   WRITE setCommandPermissions NOTIFY commandPermissionsChanged)

public:
    explicit MessageComposer(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // v0.7.x drafts: injected by AppController. Optional — without a store
    // the composer behaves exactly as before (wipe on switch).
    void setDraftStore(class DraftStore *store) { m_drafts = store; }

    QString text() const { return m_text; }
    void setText(const QString &t);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &r);

    QString replyingToEventId() const { return m_replyingToEventId; }
    QString replyingToSender()  const { return m_replyingToSender; }
    QString replyingToPreview() const { return m_replyingToPreview; }
    QString replyingToMediaKey() const { return m_replyingToMediaKey; }
    QString editingEventId()    const { return m_editingEventId; }
    QString threadRootId()      const { return m_threadRootId; }
    QString threadPreview()     const { return m_threadPreview; }
    bool isReplying() const { return !m_replyingToEventId.isEmpty(); }
    bool isEditing()  const { return !m_editingEventId.isEmpty(); }
    bool inThread()   const { return !m_threadRootId.isEmpty(); }

    bool canSend() const;

    bool sendTextAsCaption() const { return m_sendTextAsCaption; }
    void setSendTextAsCaption(bool on);

    // HIGHLIGHT ranges, and every one is checked against the text it claims to
    // cover before it is handed out.
    //
    // MentionHighlighter paints whatever offsets it is given, so a ref whose
    // start/length no longer line up with the composer's text colours an
    // arbitrary run of words in the accent -- reported against an edit as "its
    // only part blue". A ref can go stale legitimately (the text is replaced
    // wholesale by beginEdit or a draft restore, or the user edits in front of
    // a mention), and no producer can be sure its offsets survive that.
    //
    // So this fails closed the way the draft restore already does: a range is
    // offered only while the slice it names is still exactly the mention's own
    // display text. Dropping a stale highlight is invisible; painting the wrong
    // words is not. This is presentation only -- the send path reads
    // m_mentionRefs directly, so nothing here can change what is sent.
    QVariantList mentionRanges() const
    {
        QVariantList out;
        for (const mention::MentionRef &ref : m_mentionRefs) {
            if (ref.start < 0 || ref.length <= 0)
                continue;
            if (ref.start + ref.length > m_text.length())
                continue;
            if (m_text.mid(ref.start, ref.length) != ref.displayText)
                continue;
            out.append(QVariantMap{
                { QStringLiteral("start"), ref.start },
                { QStringLiteral("length"), ref.length },
            });
        }
        return out;
    }

    Q_INVOKABLE void send();
    // "Send as a message" on the unknown-command error bar: the same send,
    // with command parsing skipped, so "/typo hello" can still be posted
    // deliberately.
    Q_INVOKABLE void sendBypassingCommands();
    // v0.9 scheduled send (phase 11): the message the composer WOULD send
    // now — {body, mentionIds, bodySpec(empty = markdown), roomId,
    // threadRootId, replyToEventId} — without sending it. The scheduler
    // takes the snapshot and the composer is cleared by the caller.
    Q_INVOKABLE QVariantMap composedMessage() const;
    // v0.9 rich composer: send a PRE-COMPOSED (plainBody, html, mentions)
    // triple — both bodies derived from one QTextDocument by
    // RichComposition, handed over by RichComposerBridge. Context routing
    // (edit / thread / reply / room) and the send tail are identical to the
    // markdown path. Attachments dispatch alongside; the text-as-caption
    // convenience deliberately does NOT apply to a formatted message (a
    // caption is plain text).
    Q_INVOKABLE void sendPrepared(const QString &body, const QString &html,
                                  const QStringList &mentionUserIds);
    // Replace the in-progress command word with the chosen completion
    // ("/ki" -> "/kick "). Returns the new cursor position.
    Q_INVOKABLE int acceptCommandCompletion(const QString &name);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview,
                                const QString &mediaKey = QString());
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
    // 2026-08-18 tester request ("add function remove all edits"): redact the
    // m.replace events attached to one of the user's own messages so it
    // returns to its original text. Backend-gated — the relations are only
    // reachable through the SDK.
    Q_INVOKABLE bool canRemoveEdits() const;
    Q_INVOKABLE void removeEdits(const QString &eventId);

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

    QString commandError() const { return m_commandError; }
    QVariantList commandCompletions() const;
    QVariantMap commandPermissions() const { return m_commandPermissions; }
    void setCommandPermissions(const QVariantMap &permissions);

Q_SIGNALS:
    void textChanged();
    void mentionRangesChanged();
    void roomIdChanged();
    void canSendChanged();
    void commandErrorChanged();
    void commandCompletionsChanged();
    void commandPermissionsChanged();
    // /markdown: the composer MODE is presentation state owned by QML/
    // settings, so the command only asks; nothing here flips it.
    void composerModeToggleRequested();
    // /nick: display-name changes carry op-id bookkeeping owned by
    // AppController (submitOwnDisplayName); the command only asks.
    void displayNameChangeRequested(const QString &name);
    void replyStateChanged();
    void editStateChanged();
    void threadStateChanged();
    void attachmentsChanged();
    void sendTextAsCaptionChanged();
    // Result of removeEdits(), for the room it was issued in. Counts only.
    void editsRemoved(const QString &eventId, bool ok, int removed,
                      int failed, bool truncated);
    void attachmentRejected(const QString &reason);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void updateCanSend();
    void refreshTypingState();
    void stopTyping();
    // The one send implementation behind send()/sendBypassingCommands().
    void sendInternal(bool allowCommands);
    // Route one composed body through the current context (thread / reply /
    // room) with a v0.9 body spec; empty spec = the markdown path.
    void sendComposed(const QString &body, const QStringList &mentionIds,
                      const QVariantMap &bodySpec);
    // Execute a parsed slash command. Returns true when the command was
    // handled (successfully or with a commandError) and the ordinary text
    // send must not run.
    bool executeCommand(const SlashCommands::Parse &parsed,
                        const QStringList &mentionIds);
    void setCommandError(const QString &error);
    // stopTyping + cancelReplyOrEdit + clear, the tail every successful
    // send and content-sending command shares.
    void finishSuccessfulSend();
    void dispatchAttachments();
    // One entry, once it is dispatchable (a video waits for its poster).
    void dispatchAttachment(int row);
    // Attaches `body` to ONE queued attachment as its caption and reports
    // whether an attachment took it. False means the text still has to be
    // sent as its own message.
    bool takeTextAsCaption(const QString &body, const QStringList &mentionIds);

    MatrixClient *m_client = nullptr;
    AttachmentQueueModel *m_attachments = nullptr;
    bool m_sendTextAsCaption = false;
    QString m_text;
    QString m_roomId;
    QString m_replyingToEventId;
    QString m_replyingToSender;
    QString m_replyingToPreview;
    QString m_replyingToMediaKey;
    QString m_editingEventId;
    QString m_threadRootId;
    QString m_threadPreview;
    QList<mention::MentionRef> m_mentionRefs;
    QString m_commandError;
    QVariantMap m_commandPermissions;
    // Voice send ops in flight. Each carries the recording file it owns AND
    // the room it was sent to. The file is deleted when the op resolves (the
    // SDK reads the bytes into its queue at queueing time and never re-reads
    // the path), but the FAILURE is only surfaced when the composer is still
    // showing that room: an upload that fails after the user has moved on
    // must not appear over an unrelated conversation. Cleanup is
    // unconditional, reporting is scoped — the two are deliberately not the
    // same decision.
    struct VoiceOp {
        QString localPath;
        QString roomId;
    };
    QHash<quint64, VoiceOp> m_voiceOps;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    QTimer  m_typingRefresh;

    // v0.7.x drafts. The debounce is stopped BEFORE every room change and
    // the save reads the CURRENT room, so a stale timer can never write
    // one room's text under another's key. Edit mode never saves — the
    // text then is the edited event's body, not a draft.
    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
};
