#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

// Current-room member suggestions behind the composer's mention popup. Never
// queries the server user directory: it lists the room's members (via
// MatrixClient::requestRoomMembers), filters and ranks them against the query,
// excludes the signed-in user and caps the results. Refreshed on membership
// change; cleared on room switch and sign-out. Never logs message text.
class MentionSuggestionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool roomMentionAllowed READ roomMentionAllowed
                   WRITE setRoomMentionAllowed
                   NOTIFY roomMentionAllowedChanged)

public:
    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarMxcRole,
        AmbiguousRole,
        // Power-level role from the member snapshot ("creator"/"administrator"/
        // "moderator"/"user" on Rust, "default" on the mock). QML maps it to a
        // chip.
        RoleRole,
        IsRoomRole,
    };

    explicit MentionSuggestionModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    // Whether to offer @room, from the room's required level via the SDK.
    void setRoomMentionAllowed(bool allowed);
    bool roomMentionAllowed() const { return m_roomMentionAllowed; }
    int count() const { return int(m_results.size()); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Row snapshot for QML/tests: { userId, displayName, avatarMxc, ambiguous
    // }.
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
    void roomMentionAllowedChanged();

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
        // The synthetic whole-room row. Its id is the sentinel "@room", which
        // the bridge turns into m.mentions.room; a real Matrix id always has a
        // domain, so it cannot collide.
        bool isRoom = false;
    };

    void requestMembers();
    void rebuild();
    void clearResults();

    static constexpr int kMaxResults = 8;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    QString m_query;
    // Defaults to offered: the server enforces the real rule, and the caller
    // narrows this only when it knows the account cannot. Defaulting to false
    // would hide @room whenever no room-info snapshot was loaded.
    bool m_roomMentionAllowed = true;
    quint64 m_membersOp = 0;
    QList<Member> m_all;     // cached members for m_roomId (self excluded)
    QList<Member> m_results; // filtered + ranked + capped
};
