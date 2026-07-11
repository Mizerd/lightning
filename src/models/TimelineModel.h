#pragma once

#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QList>
#include <QStringList>
#include <QVariantList>

class MatrixClient;

class TimelineModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ eventCount NOTIFY countChanged)
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingTextChanged)
    Q_PROPERTY(bool canPaginate READ canPaginate NOTIFY paginationChanged)
    Q_PROPERTY(bool paginating READ paginating NOTIFY paginationChanged)
    // v0.5.7: last backward pagination failed; QML shows a Retry affordance.
    Q_PROPERTY(bool paginationFailed READ paginationFailed NOTIFY paginationChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
        TimestampRole,
        TypeRole,
        StatusRole,
        IsOwnRole,
        EditedRole,
        RedactedRole,
        ReplyToEventIdRole,
        ReplyToSenderRole,
        ReplyToPreviewRole,
        MediaMxcUrlRole,
        MediaHttpUrlRole,
        MediaThumbnailHttpUrlRole,
        MediaMimetypeRole,
        MediaFilenameRole,
        MediaSizeRole,
        MediaWidthRole,
        MediaHeightRole,
        IsImageRole,
        IsFileRole,
        ReactionsRole,
        // v0.4.1
        ThreadRootIdRole,        // Non-empty on thread replies.
        IsThreadRootRole,        // True if any event in this room has this as root.
        ThreadReplyCountRole,    // Number of visible replies for a thread root.
        // v0.5.0-prep+12: encryption metadata roles surface the four
        // flags C++ already carries on TimelineEvent so MessageDelegate
        // can style undecryptable rows without body-string matching.
        IsEncryptedRole,
        IsDecryptedRole,
        UndecryptableRole,
        ErrorKindRole,
        // v0.5.7: live SDK timeline roles.
        ItemIdRole,          // Stable SDK item id (survives in-place updates).
        IsLocalEchoRole,     // True until the remote echo reconciles.
        SendErrorRole,       // Coarse category when status == Failed.
        IsVirtualRole,       // Date divider / read marker / timeline start.
        // v0.5.9: media bridge + identity presentation.
        MediaKeyRole,             // Retrieval key for MatrixClient::fetchMedia.
        MediaSourceAvailableRole, // Bytes fetchable (incl. encrypted media).
        MediaThumbAvailableRole,  // Server-side thumbnail exists.
        SenderNameAmbiguousRole,  // Display name shared by 2+ members.
        SameSenderAsPreviousRole, // Consecutive-message grouping hint.
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    int eventCount() const { return static_cast<int>(m_events.size()); }

    QString typingText() const { return m_typingText; }
    bool canPaginate() const;
    bool paginating() const;
    bool paginationFailed() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void requestOlder();
    Q_INVOKABLE void markVisibleAsRead(int firstVisibleRow, int lastVisibleRow);
    Q_INVOKABLE QString ownUserId() const { return m_selfUserId; }

    // v0.5.7: retry a failed outgoing message (row must be a failed local
    // echo with a transaction id). Routed to the backend send queue; the
    // SDK re-attempts the same queued item, so no duplicate can appear.
    Q_INVOKABLE void retrySend(int row);

    // v0.5.9: image events currently loaded in this timeline, oldest
    // first, for the image viewer's previous/next navigation. Each entry:
    // {row, mediaKey, filename, sender, timestamp, mime, httpUrl}. Only
    // loaded rows — no history is fetched.
    Q_INVOKABLE QVariantList imageEntries() const;
    // All media events (images + files) currently loaded, oldest first,
    // for the Room Information "Media & Files" list. Adds isImage and
    // size to the imageEntries() shape.
    Q_INVOKABLE QVariantList mediaEntries() const;

Q_SIGNALS:
    void roomIdChanged();
    void countChanged();
    void typingTextChanged();
    void paginationChanged();

private Q_SLOTS:
    void onEventAppended(const QString &roomId, const TimelineEvent &event);
    void onEventReplaced(const QString &roomId,
                         const QString &oldEventId,
                         const TimelineEvent &newEvent);
    void onEventStatusChanged(const QString &roomId,
                              const QString &eventId,
                              TimelineEvent::Status status);
    void onEventEdited(const QString &roomId, const QString &eventId);
    void onEventRedacted(const QString &roomId, const QString &eventId);
    void onReactionsChanged(const QString &roomId, const QString &eventId);
    void onEventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void onTimelineReset(const QString &roomId);
    // v0.5.7 index-based diff application. Every index is validated
    // against the local copy; on mismatch the model self-heals by
    // reloading the backend's full timeline instead of corrupting state.
    void onEventInsertedAt(const QString &roomId, int index,
                           const TimelineEvent &event);
    void onEventChangedAt(const QString &roomId, int index,
                          const TimelineEvent &event);
    void onEventRemovedAt(const QString &roomId, int index);
    void onEventsTruncatedTo(const QString &roomId, int length);
    void onLoggedOut();
    void onTypingChanged(const QString &roomId);
    void onMembersChanged(const QString &roomId);
    void onPaginationStateChanged(const QString &roomId);

private:
    void reload();
    int rowForEventId(const QString &eventId) const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    QUrl mediaHttp(const QString &mxc) const;
    QUrl mediaThumbHttp(const QString &mxc, int w, int h) const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    QString m_selfUserId;
    QList<TimelineEvent> m_events;
    QString m_typingText;
};
