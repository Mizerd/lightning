#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

class MentionSuggestionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool membersLoaded READ membersLoaded NOTIFY membersLoadedChanged)
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

    bool membersLoaded() const { return m_membersLoaded; }

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    void setRoomMentionAllowed(bool allowed);
    bool roomMentionAllowed() const { return m_roomMentionAllowed; }
    int count() const { return int(m_results.size()); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int row) const;

    static int matchScore(const QString &query, const QString &displayName,
                          const QString &userId);

Q_SIGNALS:
    void roomIdChanged();
    void queryChanged();
    void countChanged();
    void roomMentionAllowedChanged();
    void membersLoadedChanged();

private Q_SLOTS:
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);
    void onMembersChanged(const QString &roomId);
    void onLoggedOut();

private:
    struct Member {
        QString userId;
        QString displayName;
        QString rawDisplayName;
        QString avatarMxc;
        QString role;
        bool ambiguous = false;
        bool isRoom = false;
    };

    void requestMembers();
    void setMembersLoaded(bool loaded);
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
    bool m_membersRequestOwed = false;
    bool m_membersLoaded = false;
    QList<Member> m_all;
    QList<Member> m_results;
};
