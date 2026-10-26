#pragma once

#include <functional>

#include "media/ImageSendPrep.h"
#include "models/AttachmentQueueModel.h"
#include "models/MentionTokenizer.h"
#include "models/SlashCommands.h"

#include <QList>
#include <QObject>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QTemporaryDir>
#include <memory>
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
    /// The text of the message being edited, as it was when the edit began
    /// (the banner quotes it, C46 F07). Meaningful only while isEditing.
    Q_PROPERTY(QString editOriginalText READ editOriginalText NOTIFY editStateChanged)
    Q_PROPERTY(bool isReplying READ isReplying NOTIFY replyStateChanged)
    Q_PROPERTY(bool isEditing READ isEditing NOTIFY editStateChanged)
    Q_PROPERTY(QString threadRootId READ threadRootId NOTIFY threadStateChanged)
    Q_PROPERTY(QString threadPreview READ threadPreview NOTIFY threadStateChanged)
    Q_PROPERTY(bool inThread READ inThread NOTIFY threadStateChanged)
    // Inline thread style: while the room timeline is filtered to one thread,
    // everything this composer sends goes into that thread through the SDK
    // thread path (CLAUDE.md §8), never as an ordinary room send. Persistent
    // until cleared (unlike the one-shot beginThreadReply), so it survives
    // every send and reply. Set by AppController from the timeline filter.
    Q_PROPERTY(QString threadTargetRootId READ threadTargetRootId
                   WRITE setThreadTargetRootId NOTIFY threadStateChanged)
    // The thread the NEXT send lands in, or empty for the room: the one-shot
    // thread reply, else the thread of the message being replied to (inline
    // style shows thread replies in the room timeline), else the target above.
    Q_PROPERTY(QString sendThreadRootId READ sendThreadRootId
                   NOTIFY threadStateChanged)
    // Attachment tray (Rust backend; SDK send queue).
    Q_PROPERTY(AttachmentQueueModel* attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported NOTIFY roomIdChanged)
    // Composer policy pushed in from QML (qml/Main.qml): text typed alongside
    // an attachment is sent as that attachment's caption, one event instead of
    // two. Default false.
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption NOTIFY sendTextAsCaptionChanged)
    // Photo metadata policy pushed in from QML (qml/Main.qml): "location"
    // (default) removes GPS from images before upload, "all" removes every
    // Exif / XMP / text block, "keep" sends the file as is. Applied to every
    // image that leaves the tray (picker, paste, drop); never to GIF (the GIF
    // provider path does not use the tray).
    Q_PROPERTY(QString imageMetadataMode READ imageMetadataMode
                   WRITE setImageMetadataMode NOTIFY imageMetadataModeChanged)
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
    // True only when commandError is a refused slash command, i.e. when
    // sendBypassingCommands can get past it. The size refusal sets the same
    // commandError and sending the same body as text is refused again.
    Q_PROPERTY(bool commandErrorBypassable READ commandErrorBypassable
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
    QString editOriginalText() const { return m_editSeedText; }
    bool isReplying() const { return !m_replyingToEventId.isEmpty(); }
    bool isEditing()  const { return !m_editingEventId.isEmpty(); }
    bool inThread()   const { return !m_threadRootId.isEmpty(); }
    QString threadTargetRootId() const { return m_threadTargetRootId; }
    // Retargets the composer at one thread of the current room (the inline
    // thread focus) or back at the room. Each conversation keeps its own
    // draft: the outgoing one is saved under its own key and the incoming
    // one restored, so room text never sends into a thread and thread A's
    // text never appears in thread B.
    void setThreadTargetRootId(const QString &rootEventId);
    // The DraftStore key of the conversation this composer writes to: the
    // room id, or MatrixClient::threadTimelineId(room, root) while targeting a
    // thread -- the same key the thread panel uses, so a thread's draft
    // follows it between the panel and the main column. Never a wire id.
    QString draftKey() const;
    QString sendThreadRootId() const;

    bool canSend() const;

    bool sendTextAsCaption() const { return m_sendTextAsCaption; }
    void setSendTextAsCaption(bool on);

    QString imageMetadataMode() const { return m_imageMetadataMode; }
    void setImageMetadataMode(const QString &mode);

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
    // Called once per CHOSEN custom image actually sent (a successful use for
    // the custom recents; never a highlight or a cancelled draft).
    void setCustomEmojiUseRecorder(std::function<void(const QVariantMap &)> record)
    {
        m_customEmojiUseRecorder = std::move(record);
    }
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
    // QML knows where the caret is. Returns [{shortcode, url, packName}] rows
    // (custom, `kind` omitted) merged with [{kind: "unicode", emoji, name,
    // shortcode}] rows (standard emoji, from EmojiCatalog), most recently
    // used first within each source, capped at 12 total, or empty when the
    // caret is not in a completable `:token` (including inside a URL or code
    // span). Custom packs keep their existing one-character threshold; a
    // Unicode match needs at least two, so a bare ":D" or ":)" being typed as
    // an emoticon does not pop a Unicode-emoji menu.
    Q_INVOKABLE QVariantList emojiCompletionsAt(int cursorPos) const;
    /// Replace the `:token` under the caret with `:shortcode: ` and return
    /// the new cursor position, or -1 when there was nothing to replace.
    Q_INVOKABLE int acceptEmojiCompletionAt(int cursorPos,
                                            const QString &shortcode);
    /// As acceptEmojiCompletionAt for a CUSTOM image row (findEmoticons'
    /// shape), and records the exact image chosen (url, description, key)
    /// against the inserted `:shortcode:` range. Sending uses that image, not
    /// whichever pack happens to own the shortcode at send time; an edit
    /// inside the range turns it back into literal text.
    Q_INVOKABLE int acceptEmoticonCompletionAt(int cursorPos,
                                               const QVariantMap &entry);
    /// Bind the `:key:` text already at `start` in the composer text to the
    /// image `entry` names (the emoji palette inserts the text, then binds).
    /// False, and nothing recorded, when that text is not there.
    Q_INVOKABLE bool bindEmoticonAt(int start, const QVariantMap &entry);
    /// The live selections, [{text, url, body, key, start, length}], for
    /// presentation and tests. Never persisted to disk.
    Q_INVOKABLE QVariantList selectedEmoticons() const;
    /// Replace the `:token` under the caret with `emoji ` (the character
    /// itself, already skin-tone adjusted by the caller) and return the new
    /// cursor position, or -1 when there was nothing to replace. Records the
    /// use for recents.
    Q_INVOKABLE int acceptUnicodeEmojiCompletionAt(int cursorPos,
                                                   const QString &emoji);
    // The same completion and auto-convert over ANY editor's text, for a
    // composer that keeps its own text (the thread panel). They return the
    // edit to make rather than making it, as {start, length, replacement,
    // cursor}, or an empty map: the caller replaces [start, start+length) in
    // its editor (plain or rich, both share the plain-text coordinates).
    Q_INVOKABLE QVariantList emojiCompletionsForText(const QString &text,
                                                     int cursorPos) const;
    Q_INVOKABLE QVariantMap emojiCompletionEdit(const QString &text,
                                                int cursorPos,
                                                const QVariantMap &entry);
    Q_INVOKABLE QVariantMap shortcodeAutoConvertEdit(const QString &text,
                                                     int cursorPos);
    // The send-time emoji pass every send makes: remaining `:alias:` Unicode
    // shortcodes on the markdown lane, and installed-pack `:shortcode:`s into
    // bodySpec's emoticon map (MSC2545). Shared with the thread composer.
    void applyOutgoingEmoji(QString &body, QVariantMap &bodySpec) const;
    /// Supplies the candidates. Set by AppController from the installed packs;
    /// without it completion is never offered.
    void setEmoticonSearch(
        std::function<QVariantList(const QString &prefix, int limit)> search);
    /// Supplies standard-emoji completion candidates (EmojiCatalog); without
    /// it only custom packs are offered.
    void setUnicodeEmojiSearch(
        std::function<QVariantList(const QString &prefix, int limit)> search);
    /// Records a Unicode emoji's use (recents), separate from the custom-pack
    /// resolver: MessageComposer has no direct EmojiCatalog dependency.
    void setUnicodeEmojiUseRecorder(
        std::function<void(const QString &emoji)> record);
    /// Exact shortcode/alias -> emoji (EmojiCatalog::emojiForShortcode), used
    /// by the `:shortcode:` auto-convert below. Distinct from
    /// setUnicodeEmojiSearch, which is the fuzzy popup search.
    void setUnicodeShortcodeResolver(
        std::function<QString(const QString &code)> resolve);

    // ── `:shortcode:` auto-convert (Slack/Discord-style) ─────────────────
    //
    // Called by QML right after a keystroke lands in the text. When the
    // character immediately before `cursorPos` is a space and the run before
    // it is a complete `:alias:` naming a Unicode emoji — and not a custom
    // pack shortcode of the same name, which keeps today's `:shortcode: `
    // literal-until-send behaviour — replaces it with the emoji and returns
    // the new cursor position; otherwise returns -1 and leaves the text
    // alone. A no-op while disabled (Settings) or with no Unicode resolver.
    Q_INVOKABLE int maybeAutoConvertShortcodeBeforeCursor(int cursorPos);
    /// Settings -> composer: auto-convert a completed `:alias:` while typing
    /// and once more at send for a trailing one with no following space.
    /// Default true; a custom pack shortcode is never affected by this flag.
    void setEmojiAutoConvertEnabled(bool enabled);
    bool emojiAutoConvertEnabled() const { return m_emojiAutoConvert; }
    Q_INVOKABLE void clear();
    // `threadRootId`: the replied-to message's own thread, when it is a thread
    // reply shown in the room timeline (inline thread style). The reply is then
    // sent into that thread as an in-thread rich reply.
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview,
                                const QString &mediaKey = QString(),
                                const QString &threadRootId = QString());
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
    // Drop only the reply target (chip and event id), leaving the typed text,
    // its mentions and any edit/thread mode alone. For a send that carried the
    // reply but is not a text send: a voice message or a picked GIF.
    Q_INVOKABLE void clearReplyTarget();
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
    // A drop: its URLs plus the FileTransfer portal key a sandboxed drop
    // carries ("" when none). Resolved to readable files first (see
    // AttachmentQueueModel::resolveDroppedUrls), then queued like addAttachment.
    // A portal answer that arrives after a room switch is dropped.
    Q_INVOKABLE void addDroppedAttachments(const QList<QUrl> &urls,
                                           const QString &portalKey);
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
    bool commandErrorBypassable() const
    {
        return !m_commandError.isEmpty() && m_commandErrorBypassable;
    }
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
    void imageMetadataModeChanged();
    // Result of removeEdits(), for the room it was issued in. Counts only.
    void editsRemoved(const QString &eventId, bool ok, int removed,
                      int failed, bool truncated);
    void attachmentRejected(const QString &reason);
    // A drop finished resolving and its files were offered to the queue
    // (synchronously, or later when the portal answered). The bar focuses the
    // editor on it so Return sends.
    void droppedAttachmentsResolved();

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
    // The "or send" half of shortcode auto-convert: a `:alias:` still literal
    // (typically because it ended the message with no trailing space)
    // converts here, under the same code-span and custom-pack-shadow rules as
    // maybeAutoConvertShortcodeBeforeCursor. A no-op without a resolver.
    QString convertUnicodeShortcodesForSend(const QString &text) const;
    void setCommandError(const QString &error, bool bypassable = false);
    // stopTyping + cancelReplyOrEdit + clear: the tail shared by every
    // successful send and content-sending command.
    void finishSuccessfulSend();
    // True (with commandError set) when the message would exceed the event
    // size limit; nothing is sent and the text stays.
    bool refuseIfTooLarge(const QString &body, const QString &html);
    static qsizetype serializedContentBytes(const QString &body,
                                            const QString &html, bool edit);
    void dispatchAttachments();
    // Dispatches the next requested row in tray order, only while none is in
    // flight.
    void pumpAttachments();
    // One entry, once dispatchable (a video waits for its poster).
    void dispatchAttachment(int row);
    // Attach `body` to one queued attachment as its caption; false means it
    // must be sent as its own message.
    bool takeTextAsCaption(const QString &body, const QStringList &mentionIds);

    MatrixClient *m_client = nullptr;
    std::function<QString(const QString &)> m_emoticonResolver;
    std::function<void(const QVariantMap &)> m_customEmojiUseRecorder;
    std::function<QVariantList(const QString &, int)> m_emoticonSearch;
    std::function<QVariantList(const QString &, int)> m_unicodeEmojiSearch;
    std::function<void(const QString &)> m_unicodeEmojiUseRecorder;
    std::function<QString(const QString &)> m_unicodeShortcodeResolver;
    bool m_emojiAutoConvert = true;
    AttachmentQueueModel *m_attachments = nullptr;
    bool m_sendTextAsCaption = false;
    QString m_imageMetadataMode = QStringLiteral("location");
    /// Applies the photo-metadata policy; owns the stripped copies.
    ImageSendPrep m_imagePrep;
    QString m_text;
    QString m_roomId;
    // Bumped on sign-out and client change: a portal answer to a drop made in
    // an earlier session must never stage a file in this one (§9).
    quint64 m_dropGeneration = 0;
    QString m_replyingToEventId;
    QString m_replyingToSender;
    QString m_replyingToPreview;
    QString m_replyingToMediaKey;
    QString m_editingEventId;
    /// The timeline holding `m_editingEventId` (a composite for a thread-panel
    /// edit). Empty means this composer's room.
    QString m_editingTimelineId;
    /// The text beginEdit() seeded the box with; an edit saved with this text
    /// unchanged sends nothing (C46 F06).
    QString m_editSeedText;
    QString m_editSeedHtml;
    /// Where an edit must be sent: the recorded timeline, or this composer's
    /// room when none was recorded.
    QString editTargetTimelineId() const
    {
        return m_editingTimelineId.isEmpty() ? m_roomId : m_editingTimelineId;
    }
    QString m_threadRootId;
    QString m_threadTargetRootId;
    // The thread of the message being replied to (see beginReply).
    QString m_replyThreadRootId;
    QString m_threadPreview;
    QList<mention::MentionRef> m_mentionRefs;
    // Custom images the user explicitly chose, by the `:shortcode:` range they
    // occupy in m_text. Kept in memory only (per room while switching rooms,
    // dropped at logout), so an encrypted-room draft's selections never reach
    // disk.
    struct EmoticonRef {
        QString displayText;  // ":shortcode:"
        QString url;
        QString body;
        QString key;
        int start = 0;
        int length = 0;
    };
    QList<EmoticonRef> m_emoticonRefs;
    QHash<QString, QList<EmoticonRef>> m_emoticonRefsByRoom;
    // Thread composer choices from its completion popup, ":code:" ->
    // {url, body}; mutable because the send pass spends them.
    mutable QVariantMap m_threadEmoticonChoices;
    // The custom images an edited message already shows, recovered from its
    // sanitized html (`<img data-mx-emoticon src=mxc alt title>`, in order)
    // onto their `:shortcode:` ranges in the edit text, so an edit keeps the
    // images it was sent with rather than today's same-name ones.
    static QList<EmoticonRef> emoticonRefsFromHtml(const QString &text,
                                                   const QString &sanitizedHtml);
    static QList<EmoticonRef> shiftEmoticonRefs(const QList<EmoticonRef> &refs,
                                                const QString &oldText,
                                                const QString &newText);
    bool customShortcodeExists(const QString &code) const;
    // ":shortcode:" -> {url, body} for the live selections.
    QVariantMap selectedEmoticonMap() const;
    void applyOutgoingEmojiWith(QString &body, QVariantMap &bodySpec,
                                const QVariantMap &selected) const;
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
    // Whether m_commandError is a refused slash command (see
    // commandErrorBypassable), as opposed to the size refusal.
    bool    m_commandErrorBypassable = false;
    QTimer  m_typingRefresh;

    // Drafts. The debounce is stopped before every room change and the save
    // reads the current room, so a stale timer never writes under another
    // room's key. Edit mode never saves: beginEdit flushes the pre-edit
    // draft first, and leaving edit mode (cancel or a sent edit) restores it.
    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
};
