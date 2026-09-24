#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class MatrixClient;

// Message-history search, room-scoped or global.
//
// QML binds `query` (and `roomId` for in-room mode); the model debounces,
// dispatches to the local index or MatrixClient::searchMessages, pages, and
// rejects stale completions by op id. Cleared on logout so one account's
// results never surface under another.
//
// E2EE: server search covers unencrypted rooms only, and every surface bound
// to this model must say so. Result bodies are message text (possibly
// decrypted): memory only, never persisted or logged.
class MessageSearchController : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // "" = global search across rooms; otherwise scoped to that room.
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    // "idle" | "loading" | "loading_more" | "results" | "no_results" | "error"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    // Server-reported total (an estimate; 0 when unknown).
    Q_PROPERTY(quint64 totalCount READ totalCount NOTIFY stateChanged)
    // Applied filter snapshot. The panel edits a private draft and assigns
    // this only on Apply, so Cancel cannot mutate live results.
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
    /// Whether this backend has a local index, so the choice is offered only
    /// where it exists.
    Q_PROPERTY(bool localAvailable READ localAvailable NOTIFY sourceChanged)
    /// What the local index holds, so a surface can say what local search
    /// covers.
    Q_PROPERTY(qint64 indexedMessages READ indexedMessages
                   NOTIFY indexStatsChanged)
    Q_PROPERTY(qint64 indexedRooms READ indexedRooms NOTIFY indexStatsChanged)
    /// True while a deep index is paging a room's history in.
    Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)
    /// Shortest query the local tokenizer can match (0 until known), shown
    /// instead of "no results" for a query that can never match.
    Q_PROPERTY(int minLocalChars READ minLocalChars NOTIFY stateChanged)

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
    };

    explicit MessageSearchController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString state() const { return m_state; }
    bool supported() const;
    bool canLoadMore() const { return !m_nextBatch.isEmpty(); }
    quint64 totalCount() const { return m_totalCount; }
    QVariantMap filters() const { return m_filters; }
    void setFilters(const QVariantMap &filters);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// The source actually in use: the preference narrowed by backend support.
    QString source() const { return effectiveSource(); }
    /// The preference as set, before that narrowing.
    QString preferredSource() const { return m_source; }
    void setSource(const QString &source);
    bool localAvailable() const;
    qint64 indexedMessages() const { return m_indexedMessages; }
    qint64 indexedRooms() const { return m_indexedRooms; }
    bool indexing() const { return m_deepOp != 0; }
    int minLocalChars() const { return m_minLocalChars; }

    /// Ask the backend what the index holds. Cheap; safe to call on open.
    Q_INVOKABLE void refreshIndexStats();
    /// Walk cached events into the index across every joined room.
    Q_INVOKABLE void sweepIndex();
    /// Page one room's history in and index it ("search this room").
    Q_INVOKABLE void indexRoomHistory(const QString &roomId);
    Q_INVOKABLE void clearIndex();

    Q_INVOKABLE void search();   // dispatch immediately (Enter key)
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap rowAt(int row) const;

    // Debounce interval; exposed for deterministic tests.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }

Q_SIGNALS:
    void queryChanged();
    void roomIdChanged();
    void stateChanged();
    void filtersChanged();
    void sourceChanged();
    void indexStatsChanged();
    void indexingChanged();
    /// A deep index finished. `reachedStart` says whether the whole history is
    /// indexed or the page budget ran out.
    void roomHistoryIndexed(const QString &roomId, bool ok, bool reachedStart,
                            int written);

private Q_SLOTS:
    void dispatch(bool nextPage);
    void onSearchFinished(quint64 opId, bool ok, const QVariantList &results,
                          const QString &nextBatch, quint64 count,
                          const QString &category);
    void onLocalSearchFinished(quint64 opId, bool ok, const QString &category,
                               int minChars, const QVariantList &results);

private:
    QString effectiveSource() const;
    void setState(const QString &state);
    void invalidatePending();
    void requestPage(bool append);
    bool matchesFilters(const QVariantMap &row) const;
    void rebuildFilterSets();

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
    /// Limit of the in-flight local request; "was the page full?" is answered
    /// against this. 0 when nothing is in flight.
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
    qint64 m_indexedMessages = 0;
    qint64 m_indexedRooms = 0;
    quint64 m_deepOp = 0;
    int m_minLocalChars = 0;
    /// Rows per local page; "load more" raises the limit (there is no cursor).
    static constexpr int kLocalPage = 50;
};
