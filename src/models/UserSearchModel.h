#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>

class MatrixClient;

// Debounced Matrix user-directory search model.
//
// QML binds `query`; the model debounces, dispatches through
// MatrixClient::searchUsers and rejects stale completions by op id.
//
// Many directories omit local users who share no room with the searcher, so
// a plausible exact candidate is also resolved via fetchUserProfile ("admin"
// -> "@admin:<own-server>", server taken from the account, never hardcoded):
//   * a complete typed id ("@x:server" / "x:server") is offered immediately
//     (exactMxid) and enriched with the display name once confirmed;
//   * a bare-localpart candidate ("admin" / "@admin") appears only after the
//     homeserver confirms the profile (exactLocal); syntax alone never
//     invents a result.
// Duplicates are removed and the current user is excluded. Query text and
// results are never logged or persisted.
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
        SourceRole, // "directory" | "exact_local" | "exact_mxid"
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

    // Merge exact candidates and directory rows: deduplicated by user id (the
    // first keeps its provenance, later ones only fill missing name/avatar),
    // `ownUser` excluded.
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

    // Exact-candidate lifecycle.
    quint64 m_pendingProfileOp = 0;
    QString m_candidateUserId;
    bool m_candidateNamesServer = false; // typed with an explicit server
    bool m_candidateConfirmed = false;
    QString m_candidateDisplayName;
    QString m_candidateAvatarUrl;
};
