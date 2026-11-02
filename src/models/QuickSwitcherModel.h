#pragma once

#include "matrix/RoomInfo.h"

#include <functional>

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class SpaceManager;

class QuickSwitcherModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY resultsChanged)

public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        SubtitleRole,
        CategoryRole,
        AvatarUrlRole,
        UnreadRole,
        HighlightRole,
        HasUnreadRole,
        IsSpaceRole,
        IsInviteRole,
        IdentityColorKeyRole,
    };

    struct Result {
        QString roomId;
        QString identityColorKey;
        QString name;
        QString subtitle;
        QString avatarUrl;
        QString category;
        int unread = 0;
        int highlight = 0;
        bool hasUnread = false;
        bool isSpace = false;
        bool isInvite = false;
        int score = 0;
    };

    explicit QuickSwitcherModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSpaceManager(SpaceManager *spaces);
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return m_query; }
    void setQuery(const QString &query);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reset();
    Q_INVOKABLE QVariantMap resultAt(int row) const;

    static int matchScore(const QString &query, const QString &name,
                          const QStringList &alternates);

    const QList<Result> &resultsForTest() const { return m_results; }

Q_SIGNALS:
    void queryChanged();
    void resultsChanged();

private:
    void rebuild();

    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    std::function<int(const QString &)> m_modeOf;
    QString m_query;
    QList<Result> m_results;

    static constexpr int kMaxResults = 50;
};
