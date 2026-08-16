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

private Q_SLOTS:
    void dispatch(bool nextPage);
    void onSearchFinished(quint64 opId, bool ok, const QVariantList &results,
                          const QString &nextBatch, quint64 count,
                          const QString &category);

private:
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
};
