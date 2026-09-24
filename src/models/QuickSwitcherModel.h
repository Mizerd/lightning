#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class SpaceManager;

// Result model for the quick switcher (Ctrl+K).
//
// A read-only filter over local room data (MatrixClient::rooms()). No network
// search and no message text: it matches room names, DM participant ids and
// canonical aliases. Rooms, DMs, Spaces and invites are searchable; selecting
// a result uses normal navigation (an invite opens its context, never
// auto-accepts). Results are recomputed on demand and bounded.
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
        CategoryRole,     // "invite" | "space" | "dm" | "room"
        AvatarUrlRole,
        UnreadRole,
        HighlightRole,
        HasUnreadRole,
        IsSpaceRole,
        IsInviteRole,
        // identityColorKey(RoomInfo) — the shared fallback-colour policy.
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

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return m_query; }
    void setQuery(const QString &query);

    // Rebuild results from the current client rooms with the current query.
    Q_INVOKABLE void refresh();
    // Clear the query and results (logout / account switch / close).
    Q_INVOKABLE void reset();
    // Presentation-safe routing payload for the selected row.
    Q_INVOKABLE QVariantMap resultAt(int row) const;

    // Pure scoring/filter helper, exposed for tests. Returns a score (higher =
    // better) or a negative value when the haystack does not match every query
    // token. An empty query matches everything with a neutral score.
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
    QString m_query;
    QList<Result> m_results;

    static constexpr int kMaxResults = 50;
};
