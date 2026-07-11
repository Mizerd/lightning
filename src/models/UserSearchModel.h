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
//
// v0.5.11: in addition to the directory search, a plausible exact candidate
// is resolved through MatrixClient::fetchUserProfile — the directory on many
// homeservers does not list local users that never shared a room with the
// searcher, so "admin" is also looked up as "@admin:<own-server>" (server
// derived from the authenticated account, never hardcoded):
//   * a complete typed Matrix id ("@x:server" / "x:server") is offered as a
//     result row immediately (exactMxid provenance) and enriched with the
//     confirmed display name when the profile lookup succeeds;
//   * a bare localpart candidate ("admin" / "@admin") appears ONLY after the
//     homeserver confirms the profile (exactLocal provenance) — a result is
//     never invented merely because the id is syntactically valid.
// Duplicate user IDs are removed; the current user is excluded (you cannot
// DM/invite yourself). No query text or result payload is ever logged, and
// nothing is persisted.
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
        SourceRole, // v0.5.11: "directory" | "exact_local" | "exact_mxid"
    };

    struct Result {
        QString userId;
        QString displayName;
        QString avatarUrl;
        bool isExactMxid = false;
        QString source = QStringLiteral("directory");
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

    // v0.5.11: pure merge of exact candidates and directory rows —
    // deduplicated by user id (first occurrence keeps its provenance, a
    // later duplicate only contributes missing display name / avatar),
    // `ownUser` excluded. Public and static so it is unit-testable.
    static QList<Result> mergeResults(const QList<Result> &exact,
                                      const QList<Result> &directory,
                                      const QString &ownUser);

    // Debounce interval; exposed for deterministic tests.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }

Q_SIGNALS:
    void queryChanged();
    void stateChanged();

private Q_SLOTS:
    void dispatchSearch();
    void onSearchFinished(quint64 opId, bool ok, const QVariantList &results,
                          bool limited, const QString &category);
    void onProfileFinished(quint64 opId, bool ok, const QString &userId,
                           const QString &displayName,
                           const QString &avatarUrl, const QString &category);

private:
    void setState(const QString &state);
    void invalidatePending();
    void rebuildRows();
    void updateStateFromResults();

    MatrixClient *m_client = nullptr;
    QString m_query;
    QString m_state = QStringLiteral("idle");
    QTimer m_debounce;
    QList<Result> m_results;

    // Directory search lifecycle.
    quint64 m_pendingOp = 0;
    bool m_directoryDone = false;
    bool m_directoryOk = true;
    QList<Result> m_directoryResults;

    // Exact-candidate lifecycle (v0.5.11).
    quint64 m_pendingProfileOp = 0;
    QString m_candidateUserId;
    bool m_candidateNamesServer = false; // typed with an explicit server
    bool m_candidateConfirmed = false;
    QString m_candidateDisplayName;
    QString m_candidateAvatarUrl;
};
