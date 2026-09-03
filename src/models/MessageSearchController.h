#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class MatrixClient;

// v0.7.x: server-side message-history search (room-scoped or global).
//
// QML binds `query` (and `roomId` for the in-room mode); the model
// debounces (400 ms), dispatches through MatrixClient::searchMessages,
// pages with the server's `next_batch` token, and rejects stale
// completions by operation id. Cleared on logout so one account's results
// can never surface under another.
//
// E2EE: the homeserver can only search what it can read, so results cover
// UNENCRYPTED rooms only — every UI surface bound to this model discloses
// that, and inside an encrypted room the loaded-timeline find remains the
// only search. The typed search term is the only content sent to the
// server. Result bodies may still be sensitive (they are message text):
// they live in this model's memory only and are never persisted or logged.
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
    // "local"  — Lightning's own FTS5 index over the plaintext it holds.
    //            Works in ENCRYPTED rooms, which server search cannot, and
    //            answers without a round trip. Covers what has been indexed.
    // "server" — POST /_matrix/client/v3/search. Covers the server's whole
    //            history, and only for rooms the server can READ.
    //
    // Two genuinely different answers, so this is a visible mode rather than
    // a fallback: a search that silently changed which of them it ran would
    // make "no results" mean two different things on consecutive keystrokes.
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    /// Whether a local index exists at all on this backend. Lets a surface
    /// offer the choice only where there is one.
    Q_PROPERTY(bool localAvailable READ localAvailable NOTIFY sourceChanged)
    /// What the local index holds, so a surface can say what local search
    /// covers instead of implying it covers everything.
    Q_PROPERTY(qint64 indexedMessages READ indexedMessages
                   NOTIFY indexStatsChanged)
    Q_PROPERTY(qint64 indexedRooms READ indexedRooms NOTIFY indexStatsChanged)
    /// True while a deep index is paging a room's history in.
    Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)
    /// Shortest query the local tokenizer can match, 0 until the backend has
    /// said. Surfaces show it rather than reporting "no results" for a query
    /// that could never have matched.
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

    /// The source actually in use — the preference NARROWED by what the
    /// backend can do. A surface binds to this, so what it shows is what runs.
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
    /// Page ONE room's history in and index it — the action that turns
    /// "search what you have read" into "search this room".
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
    /// A deep index finished. `reachedStart` says whether the room's whole
    /// history is now indexed or the page budget ran out first — a surface
    /// that reported "done" for both would be claiming completeness it does
    /// not have.
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
    /// Rows a local page returns. Local search has no server cursor, so
    /// "load more" is a larger LIMIT rather than a next-batch token.
    static constexpr int kLocalPage = 50;
};
