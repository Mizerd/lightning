#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>

class MatrixClient;

// v0.5.9: reusable, debounced Matrix user-directory search model.
//
// QML binds `query`; the model debounces (300 ms), dispatches through
// MatrixClient::searchUsers, and rejects stale completions by operation id.
// Complete Matrix IDs (@local:server) are accepted directly and appear as a
// result row even when the directory does not return them. Duplicate user
// IDs are removed; the current user is excluded (you cannot DM/invite
// yourself). No query text or result payload is ever logged, and nothing is
// persisted.
class UserSearchModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)

public:
    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        IsExactMxidRole,
    };

    struct Result {
        QString userId;
        QString displayName;
        QString avatarUrl;
        bool isExactMxid = false;
    };

    explicit UserSearchModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    // "idle" | "loading" | "results" | "no_results" | "error"
    QString state() const { return m_state; }
    bool supported() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void clear();
    // Row accessors for QML keyboard selection.
    Q_INVOKABLE QString userIdAt(int row) const;
    Q_INVOKABLE QString displayNameAt(int row) const;

    // True for a syntactically complete @local:server Matrix ID. Public and
    // static so it is unit-testable.
    static bool looksLikeMxid(const QString &text);

    // Debounce interval; exposed for deterministic tests.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }

Q_SIGNALS:
    void queryChanged();
    void stateChanged();

private Q_SLOTS:
    void dispatchSearch();
    void onSearchFinished(quint64 opId, bool ok, const QVariantList &results,
                          bool limited, const QString &category);

private:
    void setState(const QString &state);

    MatrixClient *m_client = nullptr;
    QString m_query;
    QString m_state = QStringLiteral("idle");
    QTimer m_debounce;
    quint64 m_pendingOp = 0;
    QList<Result> m_results;
};
