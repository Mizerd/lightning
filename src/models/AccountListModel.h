#pragma once

#include "models/AccountActivityModel.h"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

class AccountManager;
class SettingsManager;

class AccountListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AccountListModel is exposed via app.accountList")
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY rowsChanged)

public:
    // No secret is ever read. hasRevocationProof is injected so the model has
    // no src/accounts dependency; an empty function means "no proof".
    AccountListModel(SettingsManager *settings, AccountManager *accounts,
                     const AccountActivityModel *activity,
                     std::function<bool(const QString &uid)> hasRevocationProof,
                     QObject *parent = nullptr);

    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        HomeserverRole,
        IsActiveRole,
        NeedsSignInRole,
        SessionRevokedRole,
        LiveStateRole,
        KeepRunningRole,
        SectionRole,
        LastLiveAtRole
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);

    Q_INVOKABLE QVariantList rowsSnapshot() const;

    void refresh();

    using SignInProbe =
        std::function<void(const QString &uid, std::function<void(bool lost)> done)>;
    void setSignInProbe(SignInProbe probe);

Q_SIGNALS:
    void rowsChanged();
    void filterChanged();

private:
    struct Row {
        QString userId;
        QString displayName;
        QString avatarUrl;
        QString homeserver;
        QString addedAt;
        bool isActive = false;
        bool needsSignIn = false;
        bool sessionRevoked = false;
        int liveState = AccountActivityModel::NotLive;
        bool keepRunning = false;
        bool running = false;
        qint64 lastLiveAt = 0;
        bool operator==(const Row &o) const;
        bool operator!=(const Row &o) const { return !(*this == o); }
    };

    QVector<Row> buildRows() const;
    static QVariantMap mapOf(const Row &row);
    void onActivityLiveStateChanged(const QString &userId);
    void requestProbes();
    void forgetProbes();
    static constexpr int kMaxProbesInFlight = 4;

    QPointer<SettingsManager> m_settings;
    QPointer<AccountManager> m_accounts;
    QPointer<const AccountActivityModel> m_activity;
    std::function<bool(const QString &)> m_hasRevocationProof;
    QString m_filter;
    QVector<Row> m_rows;
    SignInProbe m_signInProbe;
    QHash<QString, bool> m_lostSignIn;
    QSet<QString> m_probing;
    quint64 m_probeGeneration = 0;
};
