#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

struct CallShareRow {
    QString shareId;
    QString ownerIdentity;
    QString ownerDisplayName;
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
    Q_INVOKABLE QString ownerIdentityFor(const QString &shareId) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    QStringList shareIds() const;

Q_SIGNALS:
    void countChanged();
    void shareAppeared(const QString &shareId);
    void shareEnded(const QString &shareId);

private:
    int indexOf(const QString &shareId) const;

    QVector<CallShareRow> m_rows;
};
