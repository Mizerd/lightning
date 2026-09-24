// The call's screen shares, one row each.
//
// A share is its own object, not a flag on a person: it has an owner, a
// routing key and a lifetime shorter than the owner's, and several people can
// share at once. N sharers are N rows; the SFU already gives each screen-share
// track its own sid, and SfuVideoRouter routes per track key.
//
// The id is the track sid (from SfuCallController::trackKeyForSource), which
// matters for dismissal: a restarted share is a new track with a new sid, so
// a dismissal never outlives its share and never suppresses a restart.
//
// The local share gets its own id (`local:<n>`), since it exists before the
// SFU announces a sid; the counter gives each local share a distinct id.
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
    /// What a VideoOutput attaches to. May be empty for a local share the SFU
    /// has not named yet; QML routes local shares through the local capture
    /// sink.
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

    /// Reconcile against the live shares by diffing, like the participant
    /// model; never a reset (share tiles own VideoOutputs). Rows keep arrival
    /// order, so the newest share is last, which CallStageState relies on.
    void applyShares(const QVector<CallShareRow> &desired);

    void clear();

    Q_INVOKABLE int indexOfShare(const QString &shareId) const;
    /// Who publishes this share; needed to find its audio track.
    Q_INVOKABLE QString ownerIdentityFor(const QString &shareId) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    /// Every live share id, oldest first.
    QStringList shareIds() const;

Q_SIGNALS:
    void countChanged();
    /// A new share. CallStageState uses it to re-arm the automatic spotlight
    /// even after "back to grid".
    void shareAppeared(const QString &shareId);
    void shareEnded(const QString &shareId);

private:
    int indexOf(const QString &shareId) const;

    QVector<CallShareRow> m_rows;
};
