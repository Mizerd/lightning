#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class MatrixClient;

// v0.7.x: debounced public-room-directory search model (Discover / Join).
//
// QML binds `query` (and optionally `server` for another homeserver's
// directory); the model debounces (300 ms), dispatches through
// MatrixClient::searchPublicRooms, pages with the server's `next_batch`
// token via loadMore(), and rejects stale completions by operation id — a
// superseded page can never append to a newer query's results. Nothing is
// persisted and no query text is logged.
class RoomDirectorySearchModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString server READ server WRITE setServer NOTIFY serverChanged)
    // "idle" | "loading" | "loading_more" | "results" | "no_results" | "error"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorCategory READ errorCategory NOTIFY stateChanged)

public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        AliasRole,
        TopicRole,
        AvatarUrlRole,
        MembersRole,
        JoinRuleRole,
        MembershipRole,
        IsSpaceRole,
    };

    explicit RoomDirectorySearchModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QString server() const { return m_server; }
    void setServer(const QString &server);
    QString state() const { return m_state; }
    QString errorCategory() const { return m_errorCategory; }
    bool supported() const;
    bool canLoadMore() const { return !m_nextBatch.isEmpty(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Fetch the first page immediately (the dialog's initial browse view —
    // an empty query shows the server's unfiltered directory).
    Q_INVOKABLE void refresh();
    // Fetch the next page (no-op while loading or when no token is held).
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariantMap rowAt(int row) const;

    // Debounce interval; exposed for deterministic tests.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }

Q_SIGNALS:
    void queryChanged();
    void serverChanged();
    void stateChanged();

private Q_SLOTS:
    void dispatch(bool nextPage);
    void onPublicRooms(quint64 opId, bool ok, const QVariantList &rooms,
                       const QString &nextBatch, quint64 totalEstimate,
                       const QString &category);

private:
    void setState(const QString &state);
    void invalidatePending();

    MatrixClient *m_client = nullptr;
    QString m_query;
    QString m_server;
    QString m_state = QStringLiteral("idle");
    QString m_errorCategory;
    QTimer m_debounce;
    QList<QVariantMap> m_rows;
    QString m_nextBatch;
    quint64 m_pendingOp = 0;
    bool m_pendingIsNextPage = false;
};
