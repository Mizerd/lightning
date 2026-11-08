#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;
class UserProfileResolver;

class MessageSearchController : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(quint64 totalCount READ totalCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap filters READ filters WRITE setFilters
                   NOTIFY filtersChanged)
    // ── Where the results come from ──────────────────────────────────────
    //
    // "local"  — Lightning's FTS5 index over plaintext it holds. Works in
    // encrypted rooms and needs no round trip; covers what is indexed. "server"
    // — POST /_matrix/client/v3/search. The server's whole history, for rooms
    // it can read.
    //
    // A visible mode rather than a silent fallback, so "no results" always
    // means one thing.
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool localAvailable READ localAvailable NOTIFY sourceChanged)
    Q_PROPERTY(QStringList unsupportedFilters READ unsupportedFilters
                   NOTIFY sourceChanged)
    Q_PROPERTY(QStringList ignoredFilters READ ignoredFilters
                   NOTIFY filtersChanged)
    /// True when a server search with client-side filters stopped at its page
    /// budget with more history left: "nothing found" would be false, the
    /// honest answer is "nothing in the part searched so far".
    Q_PROPERTY(bool scanLimited READ scanLimited NOTIFY stateChanged)
    Q_PROPERTY(qint64 indexedMessages READ indexedMessages
                   NOTIFY indexStatsChanged)
    Q_PROPERTY(qint64 indexedRooms READ indexedRooms NOTIFY indexStatsChanged)
    Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)
    /// A sweep for newly arrived messages is in flight.
    Q_PROPERTY(bool sweeping READ sweeping NOTIFY indexingChanged)
    Q_PROPERTY(int minLocalChars READ minLocalChars NOTIFY stateChanged)
    Q_PROPERTY(QString indexAllState READ indexAllState NOTIFY indexAllChanged)
    Q_PROPERTY(bool indexAllActive READ indexAllActive NOTIFY indexAllChanged)
    Q_PROPERTY(bool indexAllResumable READ indexAllResumable
                   NOTIFY indexAllChanged)
    Q_PROPERTY(int indexAllTotal READ indexAllTotal NOTIFY indexAllChanged)
    Q_PROPERTY(int indexAllPosition READ indexAllPosition
                   NOTIFY indexAllChanged)
    Q_PROPERTY(QString indexAllRoomName READ indexAllRoomName
                   NOTIFY indexAllChanged)
    Q_PROPERTY(qint64 indexAllWritten READ indexAllWritten
                   NOTIFY indexAllChanged)
    Q_PROPERTY(qint64 indexAllUndecryptable READ indexAllUndecryptable
                   NOTIFY indexAllChanged)
    Q_PROPERTY(int indexAllSkipped READ indexAllSkipped NOTIFY indexAllChanged)
    Q_PROPERTY(int indexAllFailedRooms READ indexAllFailedRooms
                   NOTIFY indexAllChanged)
    Q_PROPERTY(qint64 indexAllEtaMs READ indexAllEtaMs NOTIFY indexAllChanged)
    Q_PROPERTY(qint64 indexAllRetryInMs READ indexAllRetryInMs
                   NOTIFY indexAllChanged)
    Q_PROPERTY(bool indexAllOffered READ indexAllOffered
                   NOTIFY indexAllOfferChanged)
    /// When the last index-all pass left nothing to walk, ms since epoch;
    /// 0 when none has.
    Q_PROPERTY(qint64 indexAllFinishedMs READ indexAllFinishedMs
                   NOTIFY indexAllChanged)

public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        RoomNameRole,
        EventIdRole,
        SenderRole,
        SenderDisplayNameRole,
        SenderAvatarUrlRole,
        TimestampMsRole,
        MsgtypeRole,
        BodyRole,
        FilenameRole,
    };

    explicit MessageSearchController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSettings(SettingsManager *settings);
    void setProfileResolver(UserProfileResolver *resolver);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString state() const { return m_state; }
    bool supported() const;
    bool canLoadMore() const { return !m_nextBatch.isEmpty(); }
    quint64 totalCount() const { return m_totalCount; }
    QVariantMap filters() const { return m_filters; }
    bool scanLimited() const { return m_scanLimited; }
    QStringList unsupportedFilters() const;
    QStringList ignoredFilters() const;
    void setFilters(const QVariantMap &filters);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString source() const { return effectiveSource(); }
    QString preferredSource() const { return m_source; }
    void setSource(const QString &source);
    bool localAvailable() const;
    qint64 indexedMessages() const { return m_indexedMessages; }
    qint64 indexedRooms() const { return m_indexedRooms; }
    bool indexing() const { return m_deepOp != 0; }
    bool sweeping() const { return m_sweepOp != 0; }
    int minLocalChars() const { return m_minLocalChars; }
    QString indexAllState() const { return m_indexAllState; }
    bool indexAllActive() const;
    bool indexAllResumable() const;
    int indexAllTotal() const { return m_indexAllTotal; }
    int indexAllPosition() const { return m_indexAllPosition; }
    QString indexAllRoomName() const { return m_indexAllRoomName; }
    qint64 indexAllWritten() const { return m_indexAllWritten; }
    qint64 indexAllUndecryptable() const { return m_indexAllUndecryptable; }
    int indexAllSkipped() const { return m_indexAllSkipped; }
    int indexAllFailedRooms() const { return m_indexAllFailedRooms; }
    qint64 indexAllEtaMs() const { return m_indexAllEtaMs; }
    qint64 indexAllRetryInMs() const { return m_indexAllRetryInMs; }
    bool indexAllOffered() const { return m_indexAllOffered; }
    qint64 indexAllFinishedMs() const { return m_indexAllFinishedMs; }

    Q_INVOKABLE void refreshIndexStats();
    Q_INVOKABLE void sweepIndex();
    Q_INVOKABLE void indexRoomHistory(const QString &roomId);
    Q_INVOKABLE bool clearIndex();
    Q_INVOKABLE void indexAllRooms();
    Q_INVOKABLE void pauseIndexAll();
    Q_INVOKABLE void cancelIndexAll();
    Q_INVOKABLE void refreshIndexAllStatus();
    void resumeIndexAllIfPending();
    void setIndexAllHold(unsigned reason, bool held);
    unsigned indexAllHold() const { return m_indexAllHold; }
    /// Arm the one-time offer: only after a sign-in the user just made here
    /// (never a restored session), only when this account has never answered
    /// it, and only where there is a local index.
    void offerIndexAllAfterSignIn(const QString &userId, bool interactiveSignIn);
    Q_INVOKABLE void answerIndexAllOffer(const QString &answer);

    Q_INVOKABLE void search();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap rowAt(int row) const;

    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }
    void setBackgroundIndexDelayMs(int ms) { m_backgroundIndex.setInterval(ms); }

Q_SIGNALS:
    void queryChanged();
    void roomIdChanged();
    void stateChanged();
    void filtersChanged();
    void sourceChanged();
    void indexStatsChanged();
    void indexingChanged();
    void indexAllChanged();
    void indexAllOfferChanged();
    void roomHistoryIndexed(const QString &roomId, bool ok, bool reachedStart,
                            int written);

private Q_SLOTS:
    void dispatch(bool nextPage);
    void onSearchFinished(quint64 opId, bool ok, const QVariantList &results,
                          const QString &nextBatch, quint64 count,
                          const QString &category);
    void onLocalSearchFinished(quint64 opId, bool ok, const QString &category,
                               int minChars, const QVariantList &results);
    void onIndexAllProgress(quint64 opId, const QVariantMap &status);

private:
    QString effectiveSource() const;
    void setState(const QString &state);
    void invalidatePending();
    void requestPage(bool append);
    bool matchesFilters(const QVariantMap &row, bool local) const;
    void rebuildFilterSets();
    void resetIndexAll();
    void setIndexAllOffered(bool offered);
    void onSyncing();
    bool backgroundIndexWanted() const;
    void startBackgroundIndex();

    MatrixClient *m_client = nullptr;
    QString m_query;
    QString m_roomId;
    QString m_state = QStringLiteral("idle");
    QTimer m_debounce;
    QList<QVariantMap> m_rows;
    QString m_nextBatch;
    quint64 m_totalCount = 0;
    quint64 m_pendingOp = 0;
    bool m_pendingIsNextPage = false;
    int m_pendingLocalLimit = 0;
    /// Raw count of the last local page, before matchesFilters(). The next
    /// limit grows from this, never from filtered m_rows.
    int m_lastLocalRawCount = 0;
    QVariantMap m_filters;
    QSet<QString> m_fromUsers;
    QSet<QString> m_mentionUsers;
    QSet<QString> m_contentTypes;
    QSet<QString> m_pinnedEventIds;
    qint64 m_afterMs = 0;
    qint64 m_beforeMs = 0;
    QString m_pinnedMode;
    int m_scanPages = 0;
    int m_scanTarget = 0;
    QString m_source = QStringLiteral("local");
    bool m_scanLimited = false;
    qint64 m_indexedMessages = 0;
    qint64 m_indexedRooms = 0;
    quint64 m_deepOp = 0;
    quint64 m_sweepOp = 0;
    int m_minLocalChars = 0;
    SettingsManager *m_settings = nullptr;
    UserProfileResolver *m_profiles = nullptr;
    QString m_indexAllState = QStringLiteral("idle");
    int m_indexAllTotal = 0;
    int m_indexAllPosition = 0;
    QString m_indexAllRoomName;
    qint64 m_indexAllWritten = 0;
    qint64 m_indexAllUndecryptable = 0;
    int m_indexAllSkipped = 0;
    int m_indexAllFailedRooms = 0;
    qint64 m_indexAllEtaMs = -1;
    qint64 m_indexAllRetryInMs = 0;
    unsigned m_indexAllHold = 0;
    bool m_indexAllOffered = false;
    QString m_indexAllOfferUserId;
    qint64 m_indexAllFinishedMs = 0;
    QTimer m_backgroundIndex;
    /// Once per sign-in lifecycle: set when the background pass was asked
    /// for, or when this lifecycle must not ask (the index was cleared).
    bool m_backgroundIndexSpent = false;
    static constexpr int kLocalPage = 50;
};
