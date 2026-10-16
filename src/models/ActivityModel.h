#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class MatrixClient;

// The global Activity Center.
//
// One account-wide list of things addressed to the user: mentions (@user
// and @room), replies to their messages, replies in threads they started or
// joined, reactions to their messages, room invites, and keyword highlights.
// Fed incrementally from the same `eventAppended` tap NotificationManager
// uses, plus the invite diff and a reaction lane; it never scans timelines.
//
// Seen state is its own: per-entry marks, plus a per-account "seen up to"
// marker that covers what earlier sessions already showed. Opening the list
// sends no receipt.
//
// PRIVACY: previews can be decrypted plaintext. Entries are memory-only and
// die with the session; only the seen marker, the legacy keyword list and the
// ids (never names or text) of pending invites persist, through the `Store`
// callbacks. Previews are never logged.
class ActivityModel : public QAbstractListModel
{
    Q_OBJECT
    // Rows after the filter.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Entries not yet seen, filter-independent — the badge.
    Q_PROPERTY(int unseenCount READ unseenCount NOTIFY unseenCountChanged)
    // "all" | "mentions" | "replies" | "threads" | "reactions" | "invites"
    // | "keywords"
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Case-insensitive whole-word highlights (bounded; see kMaxKeywords).
    Q_PROPERTY(QStringList keywords READ keywords WRITE setKeywords
                   NOTIFY keywordsChanged)
    // The server cannot list past highlights (GET /notifications is not
    // implemented there), so after a restart the list holds only what has
    // arrived since. The panel says so instead of "Nothing here yet".
    Q_PROPERTY(bool historyUnavailable READ historyUnavailable
                   NOTIFY historyUnavailableChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        KindRole,          // mention | room_mention | reply | thread | reaction | invite | keyword
        RoomIdRole,
        RoomNameRole,
        SenderIdRole,
        SenderNameRole,
        PreviewRole,       // bounded plain text; "" when unavailable
        TimestampMsRole,
        EventIdRole,       // the event to navigate to ("" for an invite)
        ThreadRootIdRole,
        SeenRole,
        EncryptedRole,     // preview withheld because the event is undecryptable
        ReactionKeyRole,   // reaction entries: the key
    };
    Q_ENUM(Roles)

    struct Store {
        // {seenUpToMs, keywords, invites}. `invites` maps a pending invite's
        // room id to {t: first noted (ms), seen: bool}, bounded.
        std::function<QVariantMap()> load;
        std::function<void(const QVariantMap &)> save;
    };

    static constexpr int kMaxEntries = 400;
    static constexpr int kMaxKeywords = 32;
    static constexpr int kMaxKeywordLength = 64;
    static constexpr int kPreviewChars = 140;
    static constexpr int kMaxRememberedInvites = 64;
    // How long a room that held rows must stay out of the joined/invited set
    // before its rows are dropped. The room list can drop and re-add a room
    // within one burst (a reset, a resync), and that is not a leave.
    static constexpr qint64 kLeftRoomGraceMs = 15000;
    // The same for a room the client has never known (a seeded row from a
    // room left long ago), counted only once the initial sync is done, since
    // a fresh login's room list fills in over time.
    static constexpr qint64 kUnknownRoomGraceMs = 60000;

    explicit ActivityModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setStore(Store store);
    /// Who the user has ignored. Consulted for every new row (live, decrypted,
    /// reaction, invite, seed); dropSender() removes what is already listed.
    void setIgnoredCheck(std::function<bool(const QString &userId)> isIgnored);
    /// The user ignored `userId`: drop every row they sent.
    void dropSender(const QString &userId);
    /// Tests only: the local clock used for arrival stamps and grace periods.
    /// Defaults to the system clock.
    void setClock(std::function<qint64()> now);
    /// A muted room is silent everywhere, the bell included (as Element): its
    /// rows are HIDDEN, not dropped, so unmuting brings them back, and they
    /// do not count toward the badge. `modeOf(roomId)` is the room's
    /// notification mode (2 = muted); invites are never hidden.
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);
    /// A room's notification mode changed: re-evaluate what is hidden.
    void notificationModesChanged();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_visible.size(); }
    int unseenCount() const;
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QStringList keywords() const { return m_keywords; }
    void setKeywords(const QStringList &keywords);
    bool historyUnavailable() const { return m_historyUnavailable; }
    /// A seed failure (MatrixClient::activitySeedFailed's category) that asking
    /// again will not fix: the server does not implement GET /notifications
    /// (M_UNRECOGNIZED, or a bare 404/405/501 from a proxy in front of it).
    /// Pure; AppController's retry and this model's hint share it.
    static bool seedFailureIsPermanent(const QString &category);

    // ---- Feeds (AppController). A thread-timeline copy is accepted: its
    // composite id is reduced to the real room and the thread root here, and
    // duplicates collapse on event id. Returns true when an entry was added.
    //
    // An edit (isEdit) never adds a row of its own: it refreshes the preview
    // of the row for the message it replaces, and only a user it newly
    // mentions turns into activity (the edit's m.mentions lists exactly the
    // newly mentioned users).
    bool ingest(const TimelineEvent &event, const QString &roomName);
    // An event first seen undecryptable has decrypted: classify it again under
    // the same id, replacing the row if there is one.
    void eventDecrypted(const QString &roomId, const TimelineEvent &event);
    // A message or reaction was redacted: its rows leave the list (a reaction
    // row by its own id, and any row pointing at a redacted message).
    void eventRedacted(const QString &roomId, const QString &eventId);
    // A reaction from any room; only reactions to the user's own messages
    // become entries. `targetIsOwn` is the backend's confirmation that the
    // target is the user's (for messages this session never saw sent).
    bool noteReaction(const QString &roomId, const QString &roomName,
                      const QString &reactionEventId, const QString &targetEventId,
                      const QString &senderId, const QString &senderName,
                      const QString &key, qint64 timestampMs,
                      bool targetIsOwn = false);
    // A room the user has been invited to (idempotent per room). Stripped
    // invite state has no origin_server_ts, so the row's time is when this
    // account first noted the invite, remembered across restarts with its
    // seen state; a pending invite does not re-badge on every launch.
    bool noteInvite(const RoomInfo &room);
    // The invite was answered (accepted or declined): drop its entry.
    void inviteResolved(const QString &roomId);
    // This client answered the invite and the server confirmed it
    // (MatrixClient::inviteAnswered): the entry goes now, and a room list
    // still listing the room as invited until its next sync cannot bring it
    // back. A later invite to the same room counts once the list has stopped
    // listing this one.
    void inviteAnswered(const QString &roomId);
    // Entries the server remembers (GET /notifications) for a fresh session,
    // oldest first; ids already present are skipped.
    void seed(const QVariantList &entries);

    Q_INVOKABLE void markAllSeen();
    Q_INVOKABLE void markSeen(const QString &id);
    /// Everything in `roomId` up to `timestampMs` has been read, so it is seen
    /// here too. Driven by the read receipt this client sends, which is exactly
    /// the claim being mirrored.
    ///
    /// `threadRootId` names the timeline that was read. Empty is the room's
    /// main receipt, an UNTHREADED one: the server treats everything before it
    /// as read, thread replies included, so it clears every row up to its point.
    /// A root id is a threaded receipt and clears only that thread's rows.
    void markRoomReadUpTo(const QString &roomId, qint64 timestampMs,
                          const QString &threadRootId = QString());
    /// The room was marked read without a timeline (room-list menu, a
    /// notification's Mark as read, Mark space read): its receipt is unthreaded
    /// and covers every thread, so everything it holds is read. Bounded by the
    /// clock so a future-dated row stays unseen.
    void markRoomReadNow(const QString &roomId);
    /// Every room holding unseen rows whose unread state is now fully clear has
    /// been read, on any device, so its rows are seen. Uses the SDK's
    /// receipt-derived unread state: num_unread_messages, the notification and
    /// highlight counts, and the mark-as-unread flag must all be clear.
    ///
    /// Only counters written AFTER a row arrived speak for it
    /// (RoomInfo::countsWrittenAtMs): roomsChanged is not per room, so a
    /// payload from before the row would read as "read" the instant it lands.
    ///
    /// Thread rows in an encrypted room are exempt: with threading support the
    /// SDK's client-side counts skip thread events and an encrypted room has
    /// no server count, so "clear" there says nothing about threads. A
    /// threaded receipt, an unthreaded one or an explicit mark-read clears
    /// them.
    ///
    /// There is deliberately no per-room public form: the obvious hook,
    /// MatrixClient::roomUpdated, never carries unread state and would mark a
    /// new mention seen as it arrives. Only roomsChanged follows a write of
    /// this state.
    ///
    /// Invite rows are exempt (nothing to read); inviteResolved() or the user
    /// clears them.
    void reconcileRoomsAgainstTheirReadState();
    /// Rooms that held rows and have left the joined/invited set (a leave,
    /// kick or ban) for longer than the grace lose their non-invite rows.
    void pruneRoomsNoLongerJoined();
    // Navigate: emits openRequested with the exact target and marks it seen.
    Q_INVOKABLE void open(const QString &id);
    // Account switch, sign-out, add-account: everything account-scoped goes,
    // the keyword list and the mute cache included.
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    // Pure classifier for tests: the kind an event would get, or "" when it is
    // not activity for `selfUserId`.
    //
    // Order: a reply to the user, a mention of the user, a reply in a thread
    // they started or joined, then highlights. With a push verdict the rules
    // decide highlights (a keyword or legacy mention is "highlight", @room is
    // "room_mention" only when the rules highlighted it, which carries the
    // sender's notification power). `keywords`, the bell's legacy local list,
    // only applies without a verdict (a backend that evaluates no rules).
    static QString classify(const TimelineEvent &event, const QString &selfUserId,
                            const QSet<QString> &ownEventIds,
                            const QSet<QString> &ownThreadRoots,
                            const QStringList &keywords);
    static bool matchesKeyword(const QString &body, const QString &keyword);

Q_SIGNALS:
    void countChanged();
    void unseenCountChanged();
    void filterChanged();
    void keywordsChanged();
    void historyUnavailableChanged();
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);

private:
    struct Entry {
        QString id;
        // Local clock when the row was added: what a counter write is compared
        // with (see markRoomReadIfClear).
        qint64 arrivedMs = 0;
        // The row is one of the room's unread HIGHLIGHTS as the server counts
        // them (a mention, keyword, push highlight, or anything seeded from
        // /notifications?only=highlight), whatever kind it is listed as.
        bool highlight = false;
        QString kind;
        QString roomId;
        QString roomName;
        QString senderId;
        QString senderName;
        QString preview;
        qint64 timestampMs = 0;
        QString eventId;
        QString threadRootId;
        QString reactionKey;
        bool encrypted = false;
        bool seenMark = false;
        // The seed runs on the first Syncing state, before room payloads or
        // member snapshots arrive, so a row may display a raw room or user id.
        // These flags mark that it is waiting for a real name. displayNameFor()
        // falls back to the user id, not "", so pending cannot be detected with
        // isEmpty() alone.
        bool roomNamePending = false;
        bool senderNamePending = false;
        // Listed from an EDIT of a message this list never saw. The edit's
        // sender and room are all that names the original, and an m.replace is
        // only valid from the original's own sender, which nothing here can
        // check yet: the original, if it arrives, replaces the row outright.
        bool provisional = false;
    };

    bool isSeen(const Entry &e) const;
    bool isMutedRoom(const Entry &e) const;
    /// Marking half of reconcileRoomsAgainstTheirReadState without signalling,
    /// so a batch emits once. True when a row changed.
    bool markRoomReadIfClear(const QString &roomId);
    bool passesFilter(const Entry &e) const;
    void rebuildVisible();
    bool addEntry(Entry entry); // false = already listed
    // The seed's per-notification `read` flag and the room list's
    // highlight_count can disagree; this keeps the room list's answer.
    /// Returns the ids it could not account for because their room was not yet
    /// known; they are retried (see m_seedAwaitingRooms). `flippedAny` reports
    /// whether any row changed, so a retry can avoid republishing.
    QStringList reconcileSeedAgainstRoomCounts(const QStringList &seededIds,
                                               bool *flippedAny = nullptr);
    /// Re-resolve the ids the seed fell back to. Empty roomId means every
    /// pending row; a room id (from membersChanged) narrows it.
    void resolvePendingSeedNames(const QString &roomId = QString());
    void rememberOwn(const TimelineEvent &event);
    void loadStore();
    void scheduleStoreLoad();
    void saveStore();
    void setHistoryUnavailable(bool unavailable);
    void rememberInvite(const QString &roomId, qint64 firstNotedMs, bool seen);
    void forgetInvite(const QString &roomId);
    bool persistSeenInvites();
    bool ingestEdit(const TimelineEvent &event, const QString &roomName);
    /// The original of a provisional row has arrived. A row listed from a
    /// forged edit (another sender or room) is dropped so the original can be
    /// listed; one the original confirms stays, with the edit's newer text.
    /// True when a confirmed row now stands for the original.
    bool settleProvisional(const QString &eventId, const QString &sender,
                           const QString &roomId);
    // Removes every entry matching `drop`; true when one went.
    bool removeEntries(const std::function<bool(const Entry &)> &drop);
    bool isIgnored(const QString &userId) const;
    qint64 now() const;
    static TimelineEvent normalised(const TimelineEvent &event);
    static QString realRoomId(const QString &roomId);
    static QString previewOf(const TimelineEvent &event);
    static QString glyphForMsgtype(const QString &msgtype);

    MatrixClient *m_client = nullptr;
    Store m_store;
    QList<Entry> m_entries;      // newest first
    QList<int> m_visible;        // indexes into m_entries after the filter
    QSet<QString> m_ids;
    QSet<QString> m_ownEventIds;
    QList<QString> m_ownEventOrder; // bounded LRU order for m_ownEventIds
    QSet<QString> m_ownThreadRoots;
    std::function<int(const QString &)> m_modeOf;
    std::function<bool(const QString &)> m_isIgnored;
    std::function<qint64()> m_now;
    // Cleared by notificationModesChanged() and by clear().
    mutable QHash<QString, bool> m_muteCache;
    QString m_filter = QStringLiteral("all");
    QStringList m_keywords;
    // The marker as persisted: the newest origin_server_ts "Mark all seen" has
    // covered, in any session. Saved; never compared in-session.
    qint64 m_seenUpToMs = 0;
    // The marker as LOADED, i.e. what earlier sessions showed. This is what
    // isSeen() compares, so a row arriving in this session after a click is
    // unseen even when its origin_server_ts is older than the click (a
    // federation delay, a skewed clock); rows present at the click carry
    // seenMark instead.
    qint64 m_seenLoadedUpToMs = 0;
    bool m_storeLoaded = false;
    // Bumped by clear(); a deferred store load from an earlier account dies.
    quint64 m_generation = 0;
    // room id -> {t, seen}, see Store.
    QVariantMap m_invites;
    // Rooms holding rows that are missing from the joined/invited set, and
    // since when (local clock). Rooms ever seen joined or invited.
    QHash<QString, qint64> m_roomMissingSince;
    QSet<QString> m_roomsEverKnown;
    // Redacted ids, so a late copy (decryption, a seed) cannot bring a row
    // back. Bounded.
    QSet<QString> m_redactedIds;
    QList<QString> m_redactedOrder;
    // Invites this client answered whose room the list may still report as
    // invited; see inviteAnswered(). Bounded.
    QSet<QString> m_answeredInvites;
    bool m_historyUnavailable = false;
    // Seeded rows whose room was unknown when the seed landed, left on the
    // server's `read` flag. Retried on roomsChanged; a seed before any room
    // payload leaves every row here.
    QStringList m_seedAwaitingRooms;
};
