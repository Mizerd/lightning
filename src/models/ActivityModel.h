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

class ActivityModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int unseenCount READ unseenCount NOTIFY unseenCountChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool reactionsVisible READ reactionsVisible
                   NOTIFY reactionsVisibleChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QStringList keywords READ keywords WRITE setKeywords
                   NOTIFY keywordsChanged)
    Q_PROPERTY(bool historyUnavailable READ historyUnavailable
                   NOTIFY historyUnavailableChanged)
    Q_PROPERTY(int threadActivityRevision READ threadActivityRevision
                   NOTIFY threadActivityChanged)
    Q_PROPERTY(bool threadPageLoading READ threadPageLoading NOTIFY threadPageChanged)
    Q_PROPERTY(bool threadPageFailed READ threadPageFailed NOTIFY threadPageChanged)
    Q_PROPERTY(bool activityHasMore READ activityHasMore NOTIFY threadPageChanged)
    Q_PROPERTY(int readHorizonDays READ readHorizonDays WRITE setReadHorizonDays
                   NOTIFY readHorizonDaysChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        KindRole,
        RoomIdRole,
        RoomNameRole,
        SenderIdRole,
        SenderNameRole,
        PreviewRole,
        TimestampMsRole,
        EventIdRole,
        ThreadRootIdRole,
        SeenRole,
        EncryptedRole,
        ReactionKeyRole,
    };
    Q_ENUM(Roles)

    struct Store {
        std::function<QVariantMap()> load;
        std::function<void(const QVariantMap &)> save;
    };

    static constexpr int kMaxEntries = 400;
    static constexpr int kDefaultReadHorizonDays = 60;
    static constexpr int kMaxKeywords = 32;
    static constexpr int kMaxKeywordLength = 64;
    static constexpr int kPreviewChars = 140;
    static constexpr int kMaxRememberedInvites = 64;
    static constexpr qint64 kLeftRoomGraceMs = 15000;
    static constexpr qint64 kUnknownRoomGraceMs = 60000;

    explicit ActivityModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setStore(Store store);
    void setIgnoredCheck(std::function<bool(const QString &userId)> isIgnored);
    void dropSender(const QString &userId);
    void setClock(std::function<qint64()> now);
    /// A muted room is silent everywhere, the bell included (as Element): its
    /// rows are HIDDEN, not dropped, so unmuting brings them back, and they
    /// do not count toward the badge. `modeOf(roomId)` is the room's
    /// notification mode (2 = muted); invites are never hidden.
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);
    void notificationModesChanged();
    void setReactionsVisible(bool visible);
    bool reactionsVisible() const { return m_reactionsVisible; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_visible.size(); }
    int unseenCount() const;
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QStringList keywords() const { return m_keywords; }
    void setKeywords(const QStringList &keywords);
    bool historyUnavailable() const { return m_historyUnavailable; }
    int readHorizonDays() const { return m_readHorizonDays; }
    void setReadHorizonDays(int days);
    int threadActivityRevision() const { return m_threadActivityRevision; }
    bool threadPageLoading() const { return m_threadPageLoading; }
    bool threadPageFailed() const { return m_threadPageFailed; }
    bool activityHasMore() const
    { return !m_threadPageToken.isEmpty() && m_entries.size() < kMaxEntries; }
    Q_INVOKABLE QVariantList threadActivity(const QString &scope) const;
    Q_INVOKABLE void refreshThreadFollowing();
    Q_INVOKABLE void loadOlderActivity();
    Q_INVOKABLE void openThreadActivity(const QString &roomId,
                                       const QString &rootEventId,
                                       const QString &eventId);
    static bool seedFailureIsPermanent(const QString &category);

    bool ingest(const TimelineEvent &event, const QString &roomName);
    void eventDecrypted(const QString &roomId, const TimelineEvent &event);
    void eventRedacted(const QString &roomId, const QString &eventId);
    bool noteReaction(const QString &roomId, const QString &roomName,
                      const QString &reactionEventId, const QString &targetEventId,
                      const QString &senderId, const QString &senderName,
                      const QString &key, qint64 timestampMs,
                      bool targetIsOwn = false);
    bool noteInvite(const RoomInfo &room);
    void inviteResolved(const QString &roomId);
    void inviteAnswered(const QString &roomId);
    void seed(const QVariantList &entries, bool firstPage = false);

    Q_INVOKABLE void markAllSeen();
    Q_INVOKABLE void markSeen(const QString &id);
    void markRoomReadUpTo(const QString &roomId, qint64 timestampMs,
                          const QString &threadRootId = QString());
    void markRoomReadNow(const QString &roomId);
    void reconcileRoomsAgainstTheirReadState();
    void syncDirectUnreads();
    void pruneRoomsNoLongerJoined();
    Q_INVOKABLE void open(const QString &id);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap entryAt(int row) const;

    static QString classify(const TimelineEvent &event, const QString &selfUserId,
                            const QSet<QString> &ownEventIds,
                            const QSet<QString> &ownThreadRoots,
                            const QStringList &keywords);
    static bool matchesKeyword(const QString &body, const QString &keyword);

Q_SIGNALS:
    void countChanged();
    void unseenCountChanged();
    void filterChanged();
    void reactionsVisibleChanged();
    void searchQueryChanged();
    void keywordsChanged();
    void historyUnavailableChanged();
    void readHorizonDaysChanged();
    void threadActivityChanged();
    void threadPageChanged();
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);

private:
    struct Entry {
        QString id;
        qint64 arrivedMs = 0;
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
        bool readMark = false;
        bool roomNamePending = false;
        bool senderNamePending = false;
        bool provisional = false;
    };

    bool isSeen(const Entry &e) const;
    bool isMutedRoom(const Entry &e) const;
    bool markRoomReadIfClear(const QString &roomId);
    bool passesFilter(const Entry &e) const;
    void rebuildVisible();
    bool addEntry(Entry entry);
    QStringList reconcileSeedAgainstRoomCounts(const QStringList &seededIds,
                                               bool *flippedAny = nullptr);
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
    bool settleProvisional(const QString &eventId, const QString &sender,
                           const QString &roomId);
    bool removeEntries(const std::function<bool(const Entry &)> &drop);
    bool isIgnored(const QString &userId) const;
    qint64 now() const;
    static TimelineEvent normalised(const TimelineEvent &event);
    static QString realRoomId(const QString &roomId);
    static QString previewOf(const TimelineEvent &event);
    static QString glyphForMsgtype(const QString &msgtype);

    MatrixClient *m_client = nullptr;
    Store m_store;
    QList<Entry> m_entries;
    QList<int> m_visible;
    QSet<QString> m_ids;
    QSet<QString> m_ownEventIds;
    QList<QString> m_ownEventOrder;
    QSet<QString> m_ownThreadRoots;
    std::function<int(const QString &)> m_modeOf;
    std::function<bool(const QString &)> m_isIgnored;
    std::function<qint64()> m_now;
    mutable QHash<QString, bool> m_muteCache;
    QString m_searchQuery;
    QString m_filter = QStringLiteral("all");
    bool m_reactionsVisible = true;
    QStringList m_keywords;
    qint64 m_seenUpToMs = 0;
    // The marker as LOADED, i.e. what earlier sessions showed. This is what
    // isSeen() compares, so a row arriving in this session after a click is
    // unseen even when its origin_server_ts is older than the click (a
    // federation delay, a skewed clock); rows present at the click carry
    // seenMark instead.
    qint64 m_seenLoadedUpToMs = 0;
    bool m_storeLoaded = false;
    quint64 m_generation = 0;
    QVariantMap m_invites;
    QHash<QString, qint64> m_roomMissingSince;
    QSet<QString> m_roomsEverKnown;
    // Redacted ids, so a late copy (decryption, a seed) cannot bring a row
    // back. Bounded.
    QSet<QString> m_redactedIds;
    QList<QString> m_redactedOrder;
    QSet<QString> m_answeredInvites;
    bool m_historyUnavailable = false;
    int m_threadActivityRevision = 0;
    QHash<QString, int> m_threadFollowing;
    QString m_threadPageToken;
    bool m_threadPageLoading = false;
    bool m_threadPageFailed = false;
    QStringList m_seedAwaitingRooms;
    int m_readHorizonDays = kDefaultReadHorizonDays;
};
