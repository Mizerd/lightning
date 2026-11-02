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
    Q_PROPERTY(QString sendThreadRootId READ sendThreadRootId
                   NOTIFY threadStateChanged)
    Q_PROPERTY(AttachmentQueueModel* attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported NOTIFY roomIdChanged)
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption NOTIFY sendTextAsCaptionChanged)
    // Photo metadata policy pushed in from QML (qml/Main.qml): "location"
    // (default) removes GPS from images before upload, "all" removes every
    // Exif / XMP / text block, "keep" sends the file as is. Applied to every
    // image that leaves the tray (picker, paste, drop); never to GIF (the GIF
    // provider path does not use the tray).
    Q_PROPERTY(QString imageMetadataMode READ imageMetadataMode
                   WRITE setImageMetadataMode NOTIFY imageMetadataModeChanged)
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)
    Q_PROPERTY(QString commandError READ commandError
                   NOTIFY commandErrorChanged)
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
    QString editOriginalText() const { return m_editPreview; }
    bool isReplying() const { return !m_replyingToEventId.isEmpty(); }
    bool isEditing()  const { return !m_editingEventId.isEmpty(); }
    bool inThread()   const { return !m_threadRootId.isEmpty(); }
    QString threadTargetRootId() const { return m_threadTargetRootId; }
    void setThreadTargetRootId(const QString &rootEventId);
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
    Q_INVOKABLE void sendBypassingCommands();
    Q_INVOKABLE QVariantMap composedMessage() const;

    void setCustomEmojiUseRecorder(std::function<void(const QVariantMap &)> record)
    {
        m_customEmojiUseRecorder = std::move(record);
    }
    void setEmoticonResolver(
        std::function<QString(const QString &shortcode)> resolve);
    Q_INVOKABLE void sendPrepared(const QString &body, const QString &html,
                                  const QStringList &mentionUserIds);
    Q_INVOKABLE int acceptCommandCompletion(const QString &name);

    Q_INVOKABLE QVariantList emojiCompletionsAt(int cursorPos) const;
    Q_INVOKABLE int acceptEmojiCompletionAt(int cursorPos,
                                            const QString &shortcode);
    Q_INVOKABLE int acceptEmoticonCompletionAt(int cursorPos,
                                               const QVariantMap &entry);
    Q_INVOKABLE bool bindEmoticonAt(int start, const QVariantMap &entry);
    Q_INVOKABLE QVariantList selectedEmoticons() const;
    Q_INVOKABLE int acceptUnicodeEmojiCompletionAt(int cursorPos,
                                                   const QString &emoji);
    Q_INVOKABLE QVariantList emojiCompletionsForText(const QString &text,
                                                     int cursorPos) const;
    Q_INVOKABLE QVariantMap emojiCompletionEdit(const QString &text,
                                                int cursorPos,
                                                const QVariantMap &entry);
    Q_INVOKABLE QVariantMap shortcodeAutoConvertEdit(const QString &text,
                                                     int cursorPos);
    void applyOutgoingEmoji(QString &body, QVariantMap &bodySpec) const;
    void setEmoticonSearch(
        std::function<QVariantList(const QString &prefix, int limit)> search);
    void setUnicodeEmojiSearch(
        std::function<QVariantList(const QString &prefix, int limit)> search);
    void setUnicodeEmojiUseRecorder(
        std::function<void(const QString &emoji)> record);
    void setUnicodeShortcodeResolver(
        std::function<QString(const QString &code)> resolve);

    Q_INVOKABLE int maybeAutoConvertShortcodeBeforeCursor(int cursorPos);
    void setEmojiAutoConvertEnabled(bool enabled);
    bool emojiAutoConvertEnabled() const { return m_emojiAutoConvert; }
    Q_INVOKABLE void clear();
    Q_INVOKABLE void beginReply(const QString &eventId,
                                const QString &sender,
                                const QString &preview,
                                const QString &mediaKey = QString(),
                                const QString &threadRootId = QString());
    Q_INVOKABLE void beginEdit(const QString &eventId,
                               const QString &currentBody,
                               const QString &sanitizedHtml = QString(),
                               const QString &timelineId = QString());
    Q_INVOKABLE void beginThreadReply(const QString &rootEventId,
                                      const QString &preview);
    Q_INVOKABLE void cancelReplyOrEdit();
    Q_INVOKABLE void clearReplyTarget();
    Q_INVOKABLE void reactTo(const QString &targetEventId, const QString &key);
    Q_INVOKABLE void redact(const QString &eventId);
    Q_INVOKABLE bool canRemoveEdits() const;
    Q_INVOKABLE void removeEdits(const QString &eventId);

    Q_INVOKABLE QVariantMap mentionTokenAt(const QString &text,
                                           int cursorPos) const;
    Q_INVOKABLE int insertMention(const QString &userId,
                                  const QString &displayName,
                                  int tokenStart, int cursorPos);

    Q_INVOKABLE bool pollsSupported() const;
    Q_INVOKABLE void votePoll(const QString &pollEventId,
                              const QStringList &answerIds,
                              const QString &threadRootId = QString());
    Q_INVOKABLE void endPoll(const QString &pollEventId,
                             const QString &threadRootId = QString());
    Q_INVOKABLE void createPoll(const QString &question,
                                const QStringList &answers,
                                bool undisclosed,
                                int maxSelections);
    Q_INVOKABLE void sendImageFromPath(const QString &localPath);
    Q_INVOKABLE void sendFileFromPath(const QString &localPath);
    Q_INVOKABLE void sendVoiceMessage(const QString &localPath,
                                      const QString &mime,
                                      qreal durationMs,
                                      const QVariantList &waveform);

    AttachmentQueueModel *attachments() const { return m_attachments; }
    bool hasAttachments() const { return m_attachments && !m_attachments->isEmpty(); }
    bool attachmentsSupported() const;
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    Q_INVOKABLE void addDroppedAttachments(const QList<QUrl> &urls,
                                           const QString &portalKey);
    Q_INVOKABLE bool pasteFromClipboard();

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
    void composerModeToggleRequested();
    void displayNameChangeRequested(const QString &name);
    void replyStateChanged();
    void editStateChanged();
    void threadStateChanged();
    void attachmentsChanged();
    void sendTextAsCaptionChanged();
    void imageMetadataModeChanged();
    void editsRemoved(const QString &eventId, bool ok, int removed,
                      int failed, bool truncated);
    void attachmentRejected(const QString &reason);
    void droppedAttachmentsResolved();
    void ownMessageSent();

public:
    void setTypingNotificationsEnabled(bool enabled);
    bool typingNotificationsEnabled() const { return m_typingEnabled; }

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void updateCanSend();
    void refreshTypingState();
    void stopTyping();
    void sendInternal(bool allowCommands);
    void sendComposed(const QString &body, const QStringList &mentionIds,
                      const QVariantMap &bodySpec);
    bool executeCommand(const SlashCommands::Parse &parsed,
                        const QStringList &mentionIds);
    QString convertUnicodeShortcodesForSend(const QString &text) const;
    void setCommandError(const QString &error, bool bypassable = false);
    void finishSuccessfulSend();
    bool refuseIfTooLarge(const QString &body, const QString &html);
    static qsizetype serializedContentBytes(const QString &body,
                                            const QString &html, bool edit);
    void dispatchAttachments();
    void pumpAttachments();
    void dispatchAttachment(int row);
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
    QString m_editingTimelineId;
    QString m_editSeedText;
    QString m_editSeedHtml;
    QString m_editPreview;
    QString editTargetTimelineId() const
    {
        return m_editingTimelineId.isEmpty() ? m_roomId : m_editingTimelineId;
    }
    QString m_threadRootId;
    QString m_threadTargetRootId;
    QString m_replyThreadRootId;
    QString m_threadPreview;
    QList<mention::MentionRef> m_mentionRefs;
    // Custom images the user explicitly chose, by the `:shortcode:` range they
    // occupy in m_text. Kept in memory only (per room while switching rooms,
    // dropped at logout), so an encrypted-room draft's selections never reach
    // disk.
    struct EmoticonRef {
        QString displayText;
        QString url;
        QString body;
        QString key;
        int start = 0;
        int length = 0;
    };
    QList<EmoticonRef> m_emoticonRefs;
    QHash<QString, QList<EmoticonRef>> m_emoticonRefsByRoom;
    mutable QVariantMap m_threadEmoticonChoices;
    static QList<EmoticonRef> emoticonRefsFromHtml(const QString &text,
                                                   const QString &sanitizedHtml);
    static QList<EmoticonRef> shiftEmoticonRefs(const QList<EmoticonRef> &refs,
                                                const QString &oldText,
                                                const QString &newText);
    bool customShortcodeExists(const QString &code) const;
    QVariantMap selectedEmoticonMap() const;
    void applyOutgoingEmojiWith(QString &body, QVariantMap &bodySpec,
                                const QVariantMap &selected) const;
    QString m_commandError;
    QVariantMap m_commandPermissions;
    struct VoiceOp {
        QString localPath;
        QString roomId;
    };
    QHash<quint64, VoiceOp> m_voiceOps;
    bool    m_canSend = false;
    bool    m_typingActive = false;
    bool    m_typingEnabled = true;
    bool    m_commandErrorBypassable = false;
    QTimer  m_typingRefresh;

    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
};
