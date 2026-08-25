// The call's SCREEN SHARES, one row each.
//
// WHY A SHARE IS A ROW AND NOT A BOOLEAN ON A PERSON.
//
// The stage used to ask "who is sharing?" and answer with the FIRST
// participant whose `screenSharing` flag was set. Everything downstream —
// which surface goes on the spotlight, what the strip excludes, whether a
// "watch this" affordance exists — hung off that single person. Two people
// sharing at once was therefore not merely unrendered, it was
// unrepresentable: there was no second thing to name, click, or route.
//
// A share is its own object. It has an owner, a routing key, and a lifetime
// that is NOT the owner's lifetime — a person can stop and restart a share
// several times inside one call. Modelling it as a row makes N simultaneous
// sharers N rows and needs no change on the wire at all: the SFU already
// gives every screen-share track its own sid, and SfuVideoRouter already
// routes per track key.
//
// THE ID IS THE TRACK SID, AND THAT IS LOAD-BEARING FOR DISMISSAL.
//
// `SfuCallController::trackKeyForSource(identity, "screen_share")` returns
// the TRACK's sid (not the participant sid and not the `mid` — a mid belongs
// to the publisher's connection and means nothing on ours). A share that
// stops and starts again is a NEW published track and therefore a NEW sid.
//
// That is exactly the property the stage's "dismiss this share from the
// spotlight" state needs. Dismissal is keyed by share id, so:
//
//   * dismissing a share cannot outlive it, and
//   * a sharer who stops and restarts is offered again rather than being
//     silently suppressed because the user waved away the previous one.
//
// Inheriting dismissal across a restart would be the worse failure: the user
// would have no way to know why nothing appeared.
//
// THE LOCAL SHARE gets an id of its own (`local:<n>`), because our own share
// exists the moment the portal grants it and the SFU may not have announced
// a sid for it yet. The counter makes each local share a distinct id for the
// same reason a remote restart gets a new sid.
#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

struct CallShareRow {
    /// Stable for one share. Remote: the screen-share track sid. Local:
    /// "local:<n>". Never reused across a stop/start.
    QString shareId;
    QString ownerIdentity;
    QString ownerDisplayName;
    /// What a VideoOutput attaches to. EMPTY is legitimate and transient for
    /// a local share the SFU has not yet stated a track for — the row still
    /// exists, and QML routes a local share through the local capture sink
    /// rather than through this key.
    QString trackKey;
    bool local = false;
};

class CallShareModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallShareModel is exposed via app.groupCall.shareModel")

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        ShareIdRole = Qt::UserRole + 1,
        OwnerIdentityRole,
        OwnerDisplayNameRole,
        TrackKeyRole,
        LocalRole,
    };
    Q_ENUM(Roles)

    explicit CallShareModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Reconcile against the live set of shares. Diffed exactly like the
    /// participant model: removals, then inserts/moves, then per-role
    /// dataChanged. Never a reset — a share tile owns a VideoOutput too.
    ///
    /// ORDER IS ARRIVAL ORDER, and the newest share is therefore the LAST
    /// row. `CallStageState` promotes the newest non-dismissed share, so
    /// this ordering is part of that contract.
    void applyShares(const QVector<CallShareRow> &desired);

    void clear();

    Q_INVOKABLE int indexOfShare(const QString &shareId) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    /// Every live share id, oldest first. Used by CallStageState to prune
    /// dismissals of shares that have ended and to answer "is anything
    /// still reachable?".
    QStringList shareIds() const;

Q_SIGNALS:
    void countChanged();
    /// A share that was not here a moment ago. CallStageState listens so a
    /// NEW share can re-arm the automatic spotlight even after the user
    /// pressed "back to grid" — the old code latched a layout mode instead,
    /// and nothing ever wrote it back.
    void shareAppeared(const QString &shareId);
    void shareEnded(const QString &shareId);

private:
    int indexOf(const QString &shareId) const;

    QVector<CallShareRow> m_rows;
};
