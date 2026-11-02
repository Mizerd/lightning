#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QTimer>

class MatrixClient;

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
        SourceRole,
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
    QString state() const { return m_state; }
    bool supported() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void clear();
    Q_INVOKABLE QString userIdAt(int row) const;
    Q_INVOKABLE QString displayNameAt(int row) const;

    static bool looksLikeMxid(const QString &text);

    static QList<Result> mergeResults(const QList<Result> &exact,
                                      const QList<Result> &directory,
                                      const QString &ownUser);

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

    quint64 m_pendingOp = 0;
    bool m_directoryDone = false;
    bool m_directoryOk = true;
    QList<Result> m_directoryResults;

    quint64 m_pendingProfileOp = 0;
    QString m_candidateUserId;
    bool m_candidateNamesServer = false;
    bool m_candidateConfirmed = false;
    QString m_candidateDisplayName;
    QString m_candidateAvatarUrl;
};
