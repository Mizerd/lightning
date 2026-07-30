#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

// v0.7 outgoing @-mentions: the current-room member suggestion model that
// backs the composer's mention popup. It NEVER queries the server user
// directory — it lists only the current room's members (via
// MatrixClient::requestRoomMembers, the same authoritative snapshot Room
// Information uses), filters and ranks them against the typed query, excludes
// the signed-in user, and caps the visible results. The cached snapshot is
// re-requested when the room reports a membership change, and cleared on room
// switch and sign-out. No message text is ever logged.
class MentionSuggestionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarMxcRole,
        AmbiguousRole,
        // v0.6.5: the power-level-derived role string already present in the
        // requestRoomMembers snapshot ("creator"/"administrator"/"moderator"/
        // "user" on the Rust backend; always "default" on the mock). Exposed
        // as-is; QML maps it to a display chip (administrator/creator ->
        // ADMIN, moderator -> MOD, everything else -> no chip).
        RoleRole,
    };

    explicit MentionSuggestionModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    int count() const { return int(m_results.size()); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Row snapshot for QML/tests: { userId, displayName, avatarMxc, ambiguous }.
    Q_INVOKABLE QVariantMap get(int row) const;

    // Pure ranking predicate. Returns a non-negative score (higher = better;
    // prefix beats word-start beats substring; display name beats localpart
    // beats full MXID) or -1 when the query matches nothing. An empty query
    // matches everything at a low base score. Exposed for focused tests.
    static int matchScore(const QString &query, const QString &displayName,
                          const QString &userId);

Q_SIGNALS:
    void roomIdChanged();
    void queryChanged();
    void countChanged();

private Q_SLOTS:
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);
    void onMembersChanged(const QString &roomId);
    void onLoggedOut();

private:
    struct Member {
        QString userId;
        QString displayName; // resolved (localpart fallback), never a bare MXID
        QString rawDisplayName;
        QString avatarMxc;
        QString role; // power-level-derived role string, or empty/"default"
        bool ambiguous = false;
    };

    void requestMembers();
    void rebuild();
    void clearResults();

    static constexpr int kMaxResults = 8;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    QString m_query;
    quint64 m_membersOp = 0;
    QList<Member> m_all;     // cached members for m_roomId (self excluded)
    QList<Member> m_results; // filtered + ranked + capped
};
