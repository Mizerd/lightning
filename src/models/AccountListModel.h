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

// Labs (concurrent live accounts): the account switcher's rows, for up to a
// couple of hundred saved accounts. Unlike AccountManager::accounts(), which
// reads every account's access token to answer needsSignIn, building these
// rows NEVER reads a secret: everything comes from the settings file
// (records, pins, lastLiveAt), the in-memory revoked flags, the injected
// revocation-proof check and the activity model's live states. A secret is
// read only when the user acts on a row, through the keyring worker.
//
// Sections: "running" (the active account first, then Starting, Running and
// Stopping accounts in saved order) and "other" (most recently live first,
// then saved order). A case-insensitive filter matches the display name and
// the user id. Badge updates never touch this model: rowsSnapshot() changes
// only with rowsChanged().
//
// Only AppController constructs it, and only while Labs is active. GUI thread.
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

    // Today's accounts() keys (userId, homeserver, displayName, avatarUrl,
    // isActive, needsSignIn, sessionRevoked) plus liveState, keepRunning,
    // section and lastLiveAt, in row order. Changes only on rowsChanged(),
    // NEVER on a badge update.
    Q_INVOKABLE QVariantList rowsSnapshot() const;

    // Re-read every row from its sources. Called by the signals below; public
    // for AppController after a write that has no signal (lastLiveAt).
    void refresh();

    // Whether an account's keyring ANSWERED and holds no sign-in (lost=true).
    // "Cannot tell" (a locked keyring, a timeout, legacy items this install
    // may not read directly) answers false. Asynchronous and off the GUI
    // thread (the pool's KeyringWorker); never called inside refresh()'s
    // row building. Set by AppController; without it, needsSignIn is what
    // the record, the revoked flags and the live states say (la1-M L2).
    using SignInProbe =
        std::function<void(const QString &uid, std::function<void(bool lost)> done)>;
    void setSignInProbe(SignInProbe probe);

Q_SIGNALS:
    // Structure, filter, liveState, needsSignIn, keepRunning, isActive or a
    // record's profile changed.
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
        bool running = false; // section
        qint64 lastLiveAt = 0;
        bool operator==(const Row &o) const;
        bool operator!=(const Row &o) const { return !(*this == o); }
    };

    QVector<Row> buildRows() const;
    static QVariantMap mapOf(const Row &row);
    void onActivityLiveStateChanged(const QString &userId);
    // Asks the probe about rows it has no answer for, a few at a time.
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
    // The probe's answers this process (true: the keyring holds no sign-in),
    // forgotten whenever a record changes (a fresh sign-in rewrites it).
    QHash<QString, bool> m_lostSignIn;
    QSet<QString> m_probing;
    quint64 m_probeGeneration = 0;
};
