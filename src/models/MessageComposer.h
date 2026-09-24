#pragma once

#include <functional>

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
    // Attachment tray (Rust backend; SDK send queue).
    Q_PROPERTY(AttachmentQueueModel* attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported NOTIFY roomIdChanged)
    // Composer policy pushed in from QML (qml/Main.qml): text typed alongside
    // an attachment is sent as that attachment's caption, one event instead of
    // two. Default false.
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption NOTIFY sendTextAsCaptionChanged)
    // Current mention ranges as [{start, length}] for the composer's chip
    // highlighter and atomic-delete key handling. Derived from the semantic
    // refs; re-announced on every text change (the refs re-anchor there).
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)
    // Slash commands. `commandError` is a non-destructive refusal (unknown
    // command or missing arguments): the draft stays and the QML bar offers
    // sendBypassingCommands. Cleared by any text change, room change or
    // successful send. `commandCompletions` is the popup model while a command
    // word is typed: [{name, argsHint, description, enabled}], where `enabled`
    // reflects commandPermissions, a courtesy hint from QML; the server
    // enforces.
    Q_PROPERTY(QString commandError READ commandError
                   NOTIFY commandErrorChanged)
    Q_PROPERTY(QVariantList commandCompletions READ commandCompletions
                   NOTIFY commandCompletionsChanged)
    Q_PROPERTY(QVariantMap commandPermissions READ commandPermissions
                   WRITE setCommandPermissions NOTIFY commandPermissionsChanged)

public:
    explicit MessageComposer(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Injected by AppController. Without a store the composer wipes text on a
    // room switch.
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

    // Highlight ranges, each checked against the text it claims to cover.
    // MentionHighlighter paints whatever offsets it gets, and a ref can go
    // stale (beginEdit, draft restore, edits in front of a mention), so a range
    // is offered only while its slice still equals the mention's display text.
    // Presentation only: sending reads m_mentionRefs directly.
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
    // "Send as a message" on the unknown-command bar: the same send with
    // command parsing skipped.
    Q_INVOKABLE void sendBypassingCommands();
    // Scheduled send: the message the composer would send now — {body,
    // mentionIds, bodySpec (empty = markdown), roomId, threadRootId,
    // replyToEventId} — without sending it. The caller clears the composer.
    Q_INVOKABLE QVariantMap composedMessage() const;

    /// How a `:shortcode:` becomes an inline custom emoji: returns the pack
    /// image's `mxc://` URI, or "" to leave the shortcode as literal text.
    void setEmoticonResolver(
        std::function<QString(const QString &shortcode)> resolve);
    // Rich composer: send a pre-composed (plainBody, html, mentions) triple
    // derived by RichComposition from one QTextDocument. Routing and the send
    // tail match the markdown path. Text-as-caption does not apply: a caption
    // is plain text.
    Q_INVOKABLE void sendPrepared(const QString &body, const QString &html,
                                  const QStringList &mentionUserIds);
    // Replace the command word being typed with the chosen completion ("/ki" ->
    // "/kick "). Returns the new cursor position.
    Q_INVOKABLE int acceptCommandCompletion(const QString &name);

    // ── Inline custom emoji completion (MSC2545) ─────────────────────────
    //
    // Cursor-driven rather than a property: a shortcode can be anywhere, and
    // QML knows where the caret is. Returns [{shortcode, url, packName}], most
    // recently used first, or empty when the caret is not in a completable
    // `:token` (including inside a URL or code span).
    Q_INVOKABLE QVariantList emojiCompletionsAt(int cursorPos) const;
    /// Replace the `:token` under the caret with `:shortcode: ` and return
    /// the new cursor position, or -1 when there was nothing to replace.
    Q_INVOKABLE int acceptEmojiCompletionAt(int cursorPos,
                                            const QString &shortcode);
    /// Supplies the candidates. Set by AppController from the installed packs;
    /// without it completion is never offered.
    void setEmoticonSearch(
        std::function<QVariantList(const QString &prefix, int limit)> search);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview,
                                const QString &mediaKey = QString());
    // sanitizedHtml (optional): the event's sanitized formatted body, used to
    // recover mention refs when the plain body carries display text.
    // `timelineId` names the timeline holding the event: the room id from the
    // main timeline, the composite id from a thread panel. Empty means this
    // composer's room. Needed because matrix-sdk resolves an edit against the
    // timeline's own items, so a thread reply must be edited through its
    // thread.
    Q_INVOKABLE void beginEdit(const QString &eventId,
                               const QString &currentBody,
                               const QString &sanitizedHtml = QString(),
                               const QString &timelineId = QString());
    // Enter thread-reply mode. `preview` is a short preview of the thread root
    // for the composer chip. Cleared by cancelReplyOrEdit() or the next send().
    Q_INVOKABLE void beginThreadReply(const QString &rootEventId,
                                      const QString &preview);
    Q_INVOKABLE void cancelReplyOrEdit();
    Q_INVOKABLE void reactTo(const QString &targetEventId, const QString &key);
    Q_INVOKABLE void redact(const QString &eventId);
    // Redact the m.replace events on one of the user's own messages so it
    // returns to its original text. Backend-gated.
    Q_INVOKABLE bool canRemoveEdits() const;
    Q_INVOKABLE void removeEdits(const QString &eventId);

    // Outgoing @-mentions. `mentionTokenAt` reports the active @-token at the
    // cursor ({active, start, query}); a cursor over an inserted mention
    // reports inactive. `insertMention` replaces the token with "@DisplayName
    // ", records the range and returns the new cursor. Sending expands recorded
    // ranges into matrix.to links.
    Q_INVOKABLE QVariantMap mentionTokenAt(const QString &text,
                                           int cursorPos) const;
    Q_INVOKABLE int insertMention(const QString &userId,
                                  const QString &displayName,
                                  int tokenStart, int cursorPos);

    // MSC3381 poll actions on the current room. `threadRootId` is set when the
    // acting delegate is in the thread panel, so the backend routes through the
    // thread timeline. Aggregation and permissions stay SDK/server-side.
    Q_INVOKABLE bool pollsSupported() const;
    Q_INVOKABLE void votePoll(const QString &pollEventId,
                              const QStringList &answerIds,
                              const QString &threadRootId = QString());
    Q_INVOKABLE void endPoll(const QString &pollEventId,
                             const QString &threadRootId = QString());
    // Creates the poll in the composer's context: the open thread in
    // thread-reply mode, else the room timeline.
    Q_INVOKABLE void createPoll(const QString &question,
                                const QStringList &answers,
                                bool undisclosed,
                                int maxSelections);
    Q_INVOKABLE void sendImageFromPath(const QString &localPath);
    Q_INVOKABLE void sendFileFromPath(const QString &localPath);
    // MSC3245 voice message from VoiceRecorder's output. Waveform entries are
    // 0..=100; failure surfaces via attachmentRejected.
    Q_INVOKABLE void sendVoiceMessage(const QString &localPath,
                                      const QString &mime,
                                      qreal durationMs,
                                      const QVariantList &waveform);

    // Attachment tray.
    AttachmentQueueModel *attachments() const { return m_attachments; }
    bool hasAttachments() const { return m_attachments && !m_attachments->isEmpty(); }
    bool attachmentsSupported() const;
    // Add a picked/dropped file; emits attachmentRejected(reason) when the
    // file fails validation (directory, unreadable, empty, over limit).
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    // Ctrl+V: returns true when the clipboard held an image or local file URLs
    // that were queued; false lets the editor paste text. Text that looks like
    // a path is never treated as a file.
    Q_INVOKABLE bool pasteFromClipboard();

    // Formatting toolbar: markdown wrap/unwrap over the selection and the
    // active flags for the chips. Pure text transforms; see MarkdownFormat.
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
    // /markdown: the composer mode is owned by QML/settings; this only asks.
    void composerModeToggleRequested();
    // /nick: display-name op bookkeeping is AppController's
    // (submitOwnDisplayName); this only asks.
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

public:
    /// Whether "… is typing" leaves this device (Settings -> Privacy). A seam
    /// rather than a SettingsManager dependency. Turning it off while a notice
    /// is live sends the stop immediately instead of waiting for the server
    /// timeout.
    void setTypingNotificationsEnabled(bool enabled);
    bool typingNotificationsEnabled() const { return m_typingEnabled; }

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void updateCanSend();
    void refreshTypingState();
    void stopTyping();
    // The one send implementation behind send()/sendBypassingCommands().
    void sendInternal(bool allowCommands);
    // Route one body through the current context (thread / reply / room) with a
    // body spec; an empty spec means markdown.
    void sendComposed(const QString &body, const QStringList &mentionIds,
                      const QVariantMap &bodySpec);
    // Execute a parsed slash command. Returns true when it was handled (or
    // refused with commandError) and the ordinary send must not run.
    bool executeCommand(const SlashCommands::Parse &parsed,
                        const QStringList &mentionIds);
    void setCommandError(const QString &error);
    // stopTyping + cancelReplyOrEdit + clear: the tail shared by every
    // successful send and content-sending command.
    void finishSuccessfulSend();
    void dispatchAttachments();
    // One entry, once dispatchable (a video waits for its poster).
    void dispatchAttachment(int row);
    // Attach `body` to one queued attachment as its caption; false means it
    // must be sent as its own message.
    bool takeTextAsCaption(const QString &body, const QStringList &mentionIds);

    MatrixClient *m_client = nullptr;
    std::function<QString(const QString &)> m_emoticonResolver;
    std::function<QVariantList(const QString &, int)> m_emoticonSearch;
    AttachmentQueueModel *m_attachments = nullptr;
    bool m_sendTextAsCaption = false;
    QString m_text;
    QString m_roomId;
    QString m_replyingToEventId;
    QString m_replyingToSender;
    QString m_replyingToPreview;
    QString m_replyingToMediaKey;
    QString m_editingEventId;
    /// The timeline holding `m_editingEventId` (a composite for a thread-panel
    /// edit). Empty means this composer's room.
    QString m_editingTimelineId;
    /// Where an edit must be sent: the recorded timeline, or this composer's
    /// room when none was recorded.
    QString editTargetTimelineId() const
    {
        return m_editingTimelineId.isEmpty() ? m_roomId : m_editingTimelineId;
    }
    QString m_threadRootId;
    QString m_threadPreview;
    QList<mention::MentionRef> m_mentionRefs;
    QString m_commandError;
    QVariantMap m_commandPermissions;
    // Voice send ops in flight, each with the recording file it owns and its
    // room. The file is deleted when the op resolves (the SDK copies the bytes
    // at queue time), but a failure is only surfaced while the composer still
    // shows that room. Cleanup is unconditional; reporting is scoped.
    struct VoiceOp {
        QString localPath;
        QString roomId;
    };
    QHash<quint64, VoiceOp> m_voiceOps;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    bool    m_typingEnabled = true;
    QTimer  m_typingRefresh;

    // Drafts. The debounce is stopped before every room change and the save
    // reads the current room, so a stale timer never writes under another
    // room's key. Edit mode never saves.
    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
};
