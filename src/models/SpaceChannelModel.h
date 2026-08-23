// The Channels navigation layout: one Space's DIRECT hierarchy as a flat,
// sectioned list of categories and channels.
//
// This is deliberately NOT RoomListModel with a filter. RoomListModel answers
// "which conversations does this account have, most recent first", and its
// Space filter is TRANSITIVE — SpaceManager::rebuild() flattens a subspace's
// rooms into every ancestor's membership, which is right for "show me
// everything in this Space" and wrong for a channel list. In a Space with
// three subspaces, the transitive view shows every room three times over as
// one undifferentiated run, and the structure the Space's admin built is
// invisible.
//
// So this model reads the DIRECT hierarchy instead:
//
//   * direct child ROOMS of the active Space become channels;
//   * direct child SPACES become collapsible categories, with THEIR direct
//     child rooms nested one level under them.
//
// Order is the hierarchy's own (`m.space.child`), never activity order. A
// channel list whose rows move when someone speaks is not a channel list —
// the whole point is that a member learns where things are and they stay
// there. Unread state changes the row's WEIGHT, never its position.
//
// Depth stops at one. A Space tree can nest arbitrarily, and a sidebar that
// nests arbitrarily becomes unreadable at about three levels; a deeper
// subspace is offered as a category the user can OPEN (which re-roots this
// model), the same way Space Home already works.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class SpaceManager;

class SpaceChannelModel : public QAbstractListModel
{
    Q_OBJECT

    /// The Space whose hierarchy is being shown. Empty means "no Space", and
    /// this model is then EMPTY rather than falling back to every room — the
    /// host decides what to show for Home, and a silent fallback would make
    /// the Channels layout look like it works at Home when it does not.
    Q_PROPERTY(QString spaceId READ spaceId WRITE setSpaceId
                   NOTIFY spaceIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    /// True when the Space genuinely has no direct children. Distinct from
    /// "no Space selected", because the empty states differ: one says "this
    /// Space has no channels yet", the other is not shown at all.
    Q_PROPERTY(bool emptyHierarchy READ emptyHierarchy NOTIFY countChanged)

public:
    enum Kind {
        /// A direct child room. Opens a timeline.
        ChannelKind = 0,
        /// A direct child space, drawn as a collapsible header.
        CategoryKind = 1,
    };
    Q_ENUM(Kind)

    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        /// "category" | "channel". A STRING rather than the enum, because
        /// exposing the enum to QML would mean registering this type with
        /// the QML engine purely so a delegate can name a constant — and a
        /// bare integer comparison in QML is the kind of thing that silently
        /// stops matching when a value is inserted.
        KindRole,
        /// 0 for a category and for an uncategorised channel, 1 for a
        /// channel inside a category. Drives indentation only.
        DepthRole,
        AvatarUrlRole,
        /// Colour key for a fallback avatar, matching the policy the room
        /// list already uses.
        IdentityColorKeyRole,
        IsDirectRole,
        EncryptedRole,
        UnreadCountRole,
        HighlightCountRole,
        HasUnreadRole,
        /// Categories only: whether this one is collapsed.
        CollapsedRole,
        /// Categories only: unread/highlight totals of the channels HIDDEN
        /// inside it, so collapsing never hides the fact that something
        /// happened. Zero when expanded — the rows speak for themselves then.
        HiddenUnreadRole,
        HiddenHighlightRole,
    };

    explicit SpaceChannelModel(QObject *parent = nullptr);

    void setSpaceManager(SpaceManager *spaces);

    QString spaceId() const { return m_spaceId; }
    void setSpaceId(const QString &spaceId);
    bool emptyHierarchy() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Collapse or expand one category. Persisted per Space for the session
    /// only: a collapse is a glance-level choice, and restoring one from a
    /// month ago would hide channels the user has forgotten they collapsed.
    Q_INVOKABLE void toggleCategory(const QString &categoryId);
    Q_INVOKABLE bool categoryCollapsed(const QString &categoryId) const;

    /// The model row showing `roomId`, or -1. Lets the host highlight the
    /// active room without QML walking the model.
    Q_INVOKABLE int rowForRoom(const QString &roomId) const;

Q_SIGNALS:
    void spaceIdChanged();
    void countChanged();

private:
    struct Row {
        QString roomId;
        QString name;
        Kind kind = ChannelKind;
        int depth = 0;
        QString avatarUrl;
        QString identityColorKey;
        bool isDirect = false;
        bool encrypted = false;
        int unread = 0;
        int highlight = 0;
        bool hasUnread = false;
        /// Categories only.
        int hiddenUnread = 0;
        int hiddenHighlight = 0;
    };

    void rebuild();
    /// Append one Space's direct child rooms at `depth`, accumulating their
    /// unread totals into `unread`/`highlight` for a collapsed category.
    /// `emit` false counts without appending, which is exactly what a
    /// collapsed category needs.
    void appendChannels(const QString &parentId, int depth, bool append,
                        int *unread, int *highlight);

    SpaceManager *m_spaces = nullptr;
    QString m_spaceId;
    QVector<Row> m_rows;
    /// Collapsed category ids, per Space. Keyed by Space so collapsing a
    /// category in one Space does not collapse a same-named one elsewhere.
    QHash<QString, QSet<QString>> m_collapsed;
};
